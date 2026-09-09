#pragma once

// The actual "main challenge": real-time polyphonic playback of .nib
// sample data in response to MIDI Note On/Off, mixed and streamed to
// I2S. Architecture per CLAUDE.md decision #3 (fixed voice pool, mixer
// sums active voices per I2S callback, voice-stealing when the pool is
// full) and the "known issues in the reference code" it documents:
// HELD/RELEASING are explicit states (not decay-coupled-to-note-ON),
// pitch-shift uses a phase accumulator against a precomputed ratio (not
// per-sample trig -- there's no trig here at all, just sample lookup +
// linear interpolation), and voices are scaled by 1/sqrt(active_count)
// rather than hard-clamped.

#include <stdbool.h>
#include <stdint.h>

// At least 8-note stereo polyphony, per the project's stated main
// challenge -- see MAX_POLYPHONY in voice_engine.c.
bool voice_engine_init(void);

// channel is the raw 0-15 MIDI channel (status byte's low nibble) --
// channel 9 (human-numbered channel 10, the GM percussion convention)
// routes to the built-in drum kit; every other channel plays piano. See
// voice_engine.c's DRUM_MIDI_CHANNEL.
void voice_engine_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void voice_engine_note_off(uint8_t channel, uint8_t note);

// Renders frame_count stereo frames (interleaved int16, L/R) into out,
// mixing every active voice -- this IS the "mixer sums active voices per
// I2S callback" from CLAUDE.md decision #3. Task-context version; kept
// for any non-ISR-driven use (audio_output.c currently uses the ISR
// version below instead, for lower/more deterministic latency).
void voice_engine_render(int16_t *out, int frame_count);

// ISR-context counterpart, called directly from audio_output.c's I2S
// on_sent callback (a real hardware interrupt firing the instant a DMA
// buffer drains) -- safe specifically because the render path underneath
// is pure fixed-point/integer arithmetic (see voice_engine.c): ESP-IDF's
// RISC-V FreeRTOS port hard-forbids any FPU instruction in ISR context,
// which is what made an earlier float-based version of this function
// crash immediately (docs/polyphony-latency-investigation.md has the
// full story).
void voice_engine_render_isr(int16_t *out, int frame_count);
