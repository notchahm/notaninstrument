#include "voice_engine.h"

#include <math.h>
#include <string.h>
#include "adpcm_decode.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nib_loader.h"

static const char *TAG = "voice_engine";

// "At least 8 note stereo polyphony" is the project's stated main
// challenge -- 8 simultaneous voices, each independently pitched,
// looped, and enveloped.
#define MAX_POLYPHONY 8

// Must match audio_output.c's I2S sample rate.
#define OUTPUT_SAMPLE_RATE 48000

#define RELEASE_SECONDS 0.3f
// Velocity-dependent HELD decay -- see voice_engine_note_on. A real
// struck piano string's higher-velocity hits carry more high-frequency/
// inharmonic energy that dissipates faster, so hard-hit notes decay
// noticeably quicker than soft ones, not just louder.
//
// Lengthened significantly (was 6.0/1.5s): that pair was tuned against a
// misdiagnosis -- what looked like decay needing to be *more aggressive*
// was actually the loop-length/leveling bug (docs/
// polyphony-latency-investigation.md's sibling decay-fix work) making
// held notes sound wrong for reasons that had nothing to do with the
// envelope. With the loop itself fixed (long, level-corrected, click-
// free), the envelope's only job is the note's overall loudness decay
// across many loop repeats, and 6s/1.5s was too fast for that on its own
// -- a real piano's sustain (pedal up, string just decaying naturally)
// commonly runs into the 10-20s range depending on register.
#define HELD_DECAY_SECONDS_SOFT 18.0f // velocity 1 (softest)
#define HELD_DECAY_SECONDS_HARD 6.0f  // velocity 127 (hardest)

// Fixed-point throughout the hot per-sample path (phase accumulation,
// interpolation, envelope, mix scaling, and now ADPCM decode itself --
// adpcm_decode.c) instead of float. Two reasons, not one: this is
// well-precedented in embedded audio regardless of ISR concerns (GBA/
// SNES-era sound hardware and most tracker/chiptune engines used integer
// phase accumulators + lookup tables because those platforms had no FPU
// at all) -- and specifically here, it's what makes ISR-context
// rendering (voice_engine_render_isr, used by audio_output.c's DMA
// on_sent callback) possible in the first place: ESP-IDF's RISC-V
// FreeRTOS port hard-forbids any FPU instruction inside interrupt
// context (confirmed on real hardware -- see docs/
// polyphony-latency-investigation.md for the crash and why floats made
// the earlier ISR attempt non-viable). powf() is still used, but only in
// voice_engine_note_on/off, computing a handful of per-note constants
// once in task context (usb_midi_host.c), never in the ISR-reachable
// render path.
//
// 12 fractional bits (was 16): confirmed on real hardware that 16 left
// only 20 integer bits, i.e. a hard ceiling of 65536 frames (2.0s @
// 32kHz) before `region->sample_length << PHASE_FRAC_BITS` silently
// overflows uint32_t -- exactly what broke when sfz_to_nib.py's
// attack+loop total grew to 2.1s (67200 frames): loop_end_fixed and
// sample_length_fixed wrapped to garbage, and voices cut off almost
// immediately instead of sustaining. 12 bits raises that ceiling to
// 2^20 = 1,048,576 frames (~32.8s @ 32kHz) -- comfortable headroom for
// any reasonable future tuning -- while costing only interpolation
// precision (1/4096th of a sample step instead of 1/65536th), nowhere
// near audible for a linear interpolator that's already a coarse
// approximation to begin with.
#define PHASE_FRAC_BITS 12
#define PHASE_ONE (1u << PHASE_FRAC_BITS)

// Envelope keeps a full 32-bit fixed-point range for its *decrement
// rate*, not just the 16 bits actually used to scale a sample -- 16 bits
// alone doesn't have enough resolution to represent the slow end of
// HELD_DECAY_SECONDS_SOFT (a per-sample decrement under 2^-16 at these
// decay times would round to zero in a 16-bit rate and never decay at
// all). Only the top 16 bits are read when actually scaling a sample.
#define ENVELOPE_MAX 0xFFFFFFFFu

typedef enum {
    VOICE_FREE,
    VOICE_HELD,
    VOICE_RELEASING,
} voice_state_t;

typedef struct {
    voice_state_t state;
    const nib_region_t *region;
    uint8_t note;
    uint32_t phase;             // Q20.12 frame position into the region's sample data
    uint32_t phase_inc;         // Q20.12 frames advanced per output frame
    // Precomputed once at note-on so the hot path never re-shifts region
    // fields per sample. Assumes sample_length/loop bounds stay under
    // 2^20 = 1,048,576 frames (~32.8s @ 32kHz, comfortably above this
    // project's trimmed samples) -- otherwise these overflow uint32_t
    // (confirmed on real hardware once trimmed samples grew past the
    // previous Q16.16 format's much lower 65536-frame ceiling -- see
    // PHASE_FRAC_BITS above).
    uint32_t loop_start_fixed;
    uint32_t loop_end_fixed;
    uint32_t sample_length_fixed;
    // Real-time ADPCM decode state, one per channel -- see
    // adpcm_decode.h. Each stream tracks its own running predictor/step/
    // block position; read_voice_frame advances them to whatever frame
    // voice->phase currently points at, and advance_voice resets them to
    // loop_start (an O(1) jump to that block's own header) on a loop wrap.
    adpcm_stream_t left, right;
    // The sample value exactly at loop_start, cached once at note-on
    // (loop_start is always a block boundary, so this is a direct header
    // read, not a decode) -- used only for the one frame per loop cycle
    // where interpolation needs the sample just past loop_end, which is
    // actually loop_start's sample once the loop has wrapped. See
    // read_voice_frame.
    int16_t loop_start_sample_left, loop_start_sample_right;
    uint32_t envelope;          // Q0.32, ENVELOPE_MAX = full volume
    uint32_t decay_rate;        // envelope decrement per frame while HELD
    uint32_t release_rate;      // envelope decrement per frame while RELEASING
    uint32_t age;               // monotonically increasing at note-on, for oldest-first voice stealing
} voice_t;

static nib_bank_t s_bank;
static bool s_bank_loaded = false;
static voice_t s_voices[MAX_POLYPHONY];
static uint32_t s_voice_age_counter = 0;

// s_voices[]/s_voice_age_counter are written from voice_engine_note_on/off
// (task context) and read+written from voice_engine_render/_isr. portMUX
// spinlocks correctly interlock across that boundary as long as each side
// uses the critical-section macro matching its own context (plain for
// task code, _ISR for ISR code) -- see voice_engine_render_isr below.
static portMUX_TYPE s_voice_lock = portMUX_INITIALIZER_UNLOCKED;

// Q16.16 1/sqrt(n) for n=1..MAX_POLYPHONY -- replaces a runtime sqrtf()
// call with a lookup table, since active voice count only ever ranges
// 1-8. [0] is unused (0 active voices means left_sum/right_sum are
// already 0, so the scale value never matters).
static const uint32_t INV_SQRT_TABLE[MAX_POLYPHONY + 1] = {
    0, 65536, 46341, 37837, 32768, 29309, 26755, 24777, 23170,
};

// TEMPORARY diagnostic (2026-09-07 chording-bug investigation): now that
// docs/polyphony-latency-investigation.md's ~242ms USB-layer delay is
// fixed (usb_midi_host.c, native USB Host Library), a genuinely
// simultaneous chord reportedly still doesn't sound together while a
// slightly staggered one does -- this records each note_on/off's voice-
// allocation decision to find out whether that's a USB-layer symptom
// resurfacing here or a real bug in voice allocation itself. Same
// decoupled-logging discipline as the earlier timing_diag: a cheap
// struct write in the hot path (voice_engine_note_on/off, which already
// run in task context, never the ISR), all ESP_LOGI dumping deferred to
// its own low-priority task.
#define VOICE_DIAG_RING_SIZE 64
typedef struct {
    int64_t us;
    uint8_t note;
    uint8_t velocity;
    int8_t slot;          // assigned voice index; -1 = no region found for this note
    int8_t active_before; // voices already active (non-free) before this decision;
                           // for note_off, this holds the number of matching voices released instead
} voice_diag_entry_t;
static voice_diag_entry_t s_diag_ring[VOICE_DIAG_RING_SIZE];
static volatile uint32_t s_diag_write_idx = 0;

static void voice_diag_record(uint8_t note, uint8_t velocity, int8_t slot, int8_t active_before) {
    voice_diag_entry_t *e = &s_diag_ring[s_diag_write_idx % VOICE_DIAG_RING_SIZE];
    e->us = esp_timer_get_time();
    e->note = note;
    e->velocity = velocity;
    e->slot = slot;
    e->active_before = active_before;
    s_diag_write_idx++;
}

static void voice_diag_task(void *arg) {
    (void) arg;
    int64_t last_us = 0;
    uint32_t read_idx = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(300));
        uint32_t write_snapshot = s_diag_write_idx;
        if (write_snapshot - read_idx > VOICE_DIAG_RING_SIZE) {
            read_idx = write_snapshot - VOICE_DIAG_RING_SIZE;
        }
        while (read_idx != write_snapshot) {
            const voice_diag_entry_t *e = &s_diag_ring[read_idx % VOICE_DIAG_RING_SIZE];
            int64_t delta_ms = (last_us == 0) ? 0 : (e->us - last_us) / 1000;
            ESP_LOGI(TAG, "note=%3u vel=%3u slot=%d active_before=%d +%lldms",
                     e->note, e->velocity, e->slot, e->active_before, (long long) delta_ms);
            last_us = e->us;
            read_idx++;
        }
    }
}

void voice_engine_start_diag_task(void) {
    xTaskCreatePinnedToCore(voice_diag_task, "voice_diag", 4096, NULL, 1, NULL, 1);
}

static voice_t *find_voice_to_use(void) {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state == VOICE_FREE) {
            return &s_voices[i];
        }
    }
    // Pool full: prefer stealing a voice already fading out over cutting
    // off one still fully held.
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state == VOICE_RELEASING) {
            return &s_voices[i];
        }
    }
    voice_t *oldest = &s_voices[0];
    for (int i = 1; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].age < oldest->age) {
            oldest = &s_voices[i];
        }
    }
    return oldest;
}

bool voice_engine_init(void) {
    memset(s_voices, 0, sizeof(s_voices));
    s_bank_loaded = nib_loader_init(&s_bank);
    if (!s_bank_loaded) {
        ESP_LOGE(TAG, "no soundbank loaded -- notes will be silent until one is flashed");
    }
    return s_bank_loaded;
}

void voice_engine_note_on(uint8_t note, uint8_t velocity) {
    if (!s_bank_loaded || velocity == 0) {
        return;
    }
    const nib_region_t *region = nib_find_region(&s_bank, note, velocity);
    if (region == NULL) {
        voice_diag_record(note, velocity, -1, -1);
        return; // key outside every region's range -- nothing to play
    }

    // One-time per-note setup -- float is fine here, this always runs in
    // task context (usb_midi_host.c), never in the ISR.
    float semitones = (float) note - (float) region->root_key;
    float ratio = powf(2.0f, semitones / 12.0f) * ((float) s_bank.sample_rate / (float) OUTPUT_SAMPLE_RATE);
    uint32_t phase_inc = (uint32_t) (ratio * (float) PHASE_ONE + 0.5f);

    float velocity_frac = (float) (velocity - 1) / 126.0f; // 0.0 (softest) .. 1.0 (hardest)
    float decay_seconds = HELD_DECAY_SECONDS_SOFT - velocity_frac * (HELD_DECAY_SECONDS_SOFT - HELD_DECAY_SECONDS_HARD);
    uint32_t decay_rate = (uint32_t) ((double) ENVELOPE_MAX / (decay_seconds * OUTPUT_SAMPLE_RATE) + 0.5);
    if (decay_rate < 1) {
        decay_rate = 1;
    }

    bool has_loop = region->loop_end > region->loop_start;
    int16_t loop_sample_left = 0, loop_sample_right = 0;
    if (has_loop) {
        loop_sample_left = adpcm_block_header_sample(region->left_data, s_bank.adpcm_block_size,
                                                       s_bank.adpcm_samples_per_block, region->loop_start);
        loop_sample_right = adpcm_block_header_sample(region->right_data, s_bank.adpcm_block_size,
                                                        s_bank.adpcm_samples_per_block, region->loop_start);
    }

    portENTER_CRITICAL(&s_voice_lock);
    int8_t active_before = 0;
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state != VOICE_FREE) {
            active_before++;
        }
    }
    voice_t *voice = find_voice_to_use();
    int8_t slot = (int8_t) (voice - s_voices);
    voice->state = VOICE_HELD;
    voice->region = region;
    voice->note = note;
    voice->phase = 0;
    voice->phase_inc = phase_inc;
    voice->loop_start_fixed = region->loop_start << PHASE_FRAC_BITS;
    voice->loop_end_fixed = region->loop_end << PHASE_FRAC_BITS;
    voice->sample_length_fixed = region->sample_length << PHASE_FRAC_BITS;
    adpcm_stream_reset(&voice->left, region->left_data, s_bank.adpcm_block_size, s_bank.adpcm_samples_per_block, 0);
    adpcm_stream_reset(&voice->right, region->right_data, s_bank.adpcm_block_size, s_bank.adpcm_samples_per_block, 0);
    voice->loop_start_sample_left = loop_sample_left;
    voice->loop_start_sample_right = loop_sample_right;
    voice->envelope = ENVELOPE_MAX;
    voice->decay_rate = decay_rate;
    voice->release_rate = 0;
    voice->age = ++s_voice_age_counter;
    portEXIT_CRITICAL(&s_voice_lock);
    voice_diag_record(note, velocity, slot, active_before);
}

void voice_engine_note_off(uint8_t note) {
    uint32_t release_rate = (uint32_t) ((double) ENVELOPE_MAX / (RELEASE_SECONDS * OUTPUT_SAMPLE_RATE) + 0.5);
    if (release_rate < 1) {
        release_rate = 1;
    }

    portENTER_CRITICAL(&s_voice_lock);
    int8_t matched = 0;
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state == VOICE_HELD && s_voices[i].note == note) {
            s_voices[i].state = VOICE_RELEASING;
            s_voices[i].release_rate = release_rate;
            matched++;
        }
    }
    portEXIT_CRITICAL(&s_voice_lock);
    voice_diag_record(note, 0, -2, matched);
}

static inline void read_voice_frame(voice_t *voice, int32_t *left, int32_t *right) {
    uint32_t idx = voice->phase >> PHASE_FRAC_BITS;
    uint32_t frac = voice->phase & (PHASE_ONE - 1);
    bool has_loop = voice->loop_end_fixed > voice->loop_start_fixed;
    uint32_t bound_idx = (has_loop ? voice->loop_end_fixed : voice->sample_length_fixed) >> PHASE_FRAC_BITS;

    // Decodes forward as needed (cheap: almost always 0-1 frames, since
    // phase_inc is close to 1.0 for the modest per-region pitch shifts
    // this soundbank uses) so decoded[0]/decoded[1] cover idx/idx+1.
    // idx+1 is allowed to run one frame past this region's real audio
    // here -- see adpcm_decode.h's seek_forward doc comment for why
    // that's safe; its result is simply never used below when that
    // happens (the loop-seam substitution takes over instead).
    adpcm_stream_seek_forward(&voice->left, idx);
    adpcm_stream_seek_forward(&voice->right, idx);

    int32_t l0 = voice->left.decoded[0];
    int32_t r0 = voice->right.decoded[0];
    int32_t l1, r1;
    if (idx + 1 >= bound_idx) {
        // Wrap to the loop start's sample for interpolation continuity
        // across the seam (the offline tool already crossfaded the
        // loop's tail to resemble its head, so this is smooth); with no
        // loop, just repeat the last frame rather than reading past the end.
        if (has_loop) {
            l1 = voice->loop_start_sample_left;
            r1 = voice->loop_start_sample_right;
        } else {
            l1 = l0;
            r1 = r0;
        }
    } else {
        l1 = voice->left.decoded[1];
        r1 = voice->right.decoded[1];
    }

    // Integer linear interpolation: frame0*(1-frac) + frame1*frac, frac
    // in Q16.16 -- the fixed-point equivalent of the float version this
    // replaced. int32_t is sufficient headroom: worst case is both frames
    // at a int16 extreme (+-32767/-32768), giving a product magnitude of
    // at most 32768*65536 = 2^31, which just fits.
    *left = (l0 * (int32_t) (PHASE_ONE - frac) + l1 * (int32_t) frac) >> PHASE_FRAC_BITS;
    *right = (r0 * (int32_t) (PHASE_ONE - frac) + r1 * (int32_t) frac) >> PHASE_FRAC_BITS;
}

static inline void advance_voice(voice_t *voice) {
    uint32_t new_phase = voice->phase + voice->phase_inc;
    bool has_loop = voice->loop_end_fixed > voice->loop_start_fixed;

    if (has_loop && new_phase >= voice->loop_end_fixed) {
        uint32_t loop_len = voice->loop_end_fixed - voice->loop_start_fixed;
        new_phase = voice->loop_start_fixed + (new_phase - voice->loop_start_fixed) % loop_len;
        // Loop wrap: reset both ADPCM streams to loop_start's own block
        // header -- O(1) regardless of how long the loop or the note's
        // been held, which is the entire point of snapping loop_start to
        // a block boundary at encode time (sfz_to_nib.py). The next
        // read_voice_frame call decodes forward from there to wherever
        // new_phase's remainder actually lands, same as it would for any
        // other frame.
        adpcm_stream_reset(&voice->left, voice->region->left_data,
                            voice->left.block_size, voice->left.samples_per_block, voice->region->loop_start);
        adpcm_stream_reset(&voice->right, voice->region->right_data,
                            voice->right.block_size, voice->right.samples_per_block, voice->region->loop_start);
    } else if (!has_loop && new_phase >= voice->sample_length_fixed) {
        voice->state = VOICE_FREE; // reached the natural end with no loop to sustain into
    }
    voice->phase = new_phase;

    // Envelope decays continuously regardless of state -- HELD uses the
    // slow natural rate, RELEASING switches to the much faster key-lift
    // rate. A voice can reach silence and free itself either way: a note
    // held indefinitely eventually decays out on its own, same as a real
    // piano string.
    if (voice->state == VOICE_HELD || voice->state == VOICE_RELEASING) {
        uint32_t rate = (voice->state == VOICE_RELEASING) ? voice->release_rate : voice->decay_rate;
        if (rate >= voice->envelope) {
            voice->envelope = 0;
            voice->state = VOICE_FREE;
        } else {
            voice->envelope -= rate;
        }
    }
}

// Shared mixing core, no locking -- callers take s_voice_lock with
// whichever critical-section flavor matches their context.
static void render_locked(int16_t *out, int frame_count) {
    for (int i = 0; i < frame_count; i++) {
        int32_t left_sum = 0, right_sum = 0;
        int active = 0;

        for (int v = 0; v < MAX_POLYPHONY; v++) {
            voice_t *voice = &s_voices[v];
            if (voice->state == VOICE_FREE) {
                continue;
            }
            active++;

            int32_t left, right;
            read_voice_frame(voice, &left, &right);
            uint32_t gain16 = voice->envelope >> 16; // top 16 bits of the Q0.32 envelope
            left_sum += (left * (int32_t) gain16) >> 16;
            right_sum += (right * (int32_t) gain16) >> 16;

            advance_voice(voice);
        }

        // Scale by 1/sqrt(active voices) rather than hard-clamping on
        // overflow (CLAUDE.md's documented lesson from the RP2350
        // reference code -- hard clamping there "sounds harsh"). Product
        // needs int64_t: left_sum can be a few hundred thousand in
        // magnitude (up to MAX_POLYPHONY int16-range voices summed) times
        // a Q16.16 factor up to 65536.
        int64_t left_scaled = ((int64_t) left_sum * INV_SQRT_TABLE[active]) >> 16;
        int64_t right_scaled = ((int64_t) right_sum * INV_SQRT_TABLE[active]) >> 16;
        if (left_scaled > 32767) left_scaled = 32767;
        else if (left_scaled < -32768) left_scaled = -32768;
        if (right_scaled > 32767) right_scaled = 32767;
        else if (right_scaled < -32768) right_scaled = -32768;

        out[i * 2] = (int16_t) left_scaled;
        out[i * 2 + 1] = (int16_t) right_scaled;
    }
}

void voice_engine_render(int16_t *out, int frame_count) {
    portENTER_CRITICAL(&s_voice_lock);
    render_locked(out, frame_count);
    portEXIT_CRITICAL(&s_voice_lock);
}

// ISR-context counterpart, called directly from audio_output.c's I2S
// on_sent callback (a real hardware interrupt firing the instant a DMA
// buffer drains). This is safe specifically because render_locked above
// (now including the ADPCM decode inside read_voice_frame/advance_voice)
// is pure integer/fixed-point -- ESP-IDF's RISC-V FreeRTOS port
// hard-forbids any FPU instruction in ISR context (confirmed on real
// hardware: an earlier float-based version of this function crashed
// immediately with "Coprocessors must not be used in ISRs!" -- see
// docs/polyphony-latency-investigation.md), and portENTER_CRITICAL_ISR/
// EXIT_CRITICAL_ISR are the documented ESP-IDF counterparts to the
// task-context macros above, correctly interlocking on the same
// portMUX_TYPE as long as each side uses the macro matching its context.
//
// Remaining known, accepted trade-off: this function and everything it
// calls live in regular flash, not IRAM, and nib_loader.c's sample data
// is read via memory-mapped flash too -- none of it is IRAM-safe. ESP-IDF
// disables the flash cache system-wide during a concurrent flash
// write/erase, and code fetched or data read from flash during that
// window would fault. This project doesn't perform runtime flash writes
// in normal operation, so the risk is real but low-probability; making
// this call chain fully IRAM-resident would be a separate change.
void voice_engine_render_isr(int16_t *out, int frame_count) {
    portENTER_CRITICAL_ISR(&s_voice_lock);
    render_locked(out, frame_count);
    portEXIT_CRITICAL_ISR(&s_voice_lock);
}
