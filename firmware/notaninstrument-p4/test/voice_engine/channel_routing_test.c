// Regression test for voice_engine.c's MIDI-channel-based instrument
// routing (piano vs. the built-in drum kit on channel 10). Mirrors the
// two decisions voice_engine_note_on()/voice_engine_note_off() make
// (not an include of voice_engine.c, which isn't portable outside
// ESP-IDF -- see this directory's other tests for the same pattern; the
// mirrored constants/logic below must be kept in sync with the real
// file if that routing logic changes):
//
//   1. voice_engine_note_on(channel, note, velocity) picks
//      bank = (channel == DRUM_MIDI_CHANNEL) ? drums : piano.
//   2. voice_engine_note_off(channel, note) only releases a HELD voice
//      whose *own* channel matches -- not just its note number.
//
// (2) is the one with a real bug class behind it: before voice_t grew a
// channel field, note-off only matched by note number, so a piano note
// and a drum "note" sharing the same number on different channels could
// stop each other. This test simulates exactly that MIDI event sequence
// (note-on for piano on channel 0, note-on for the *same* note number
// on drum channel 9, then a note-off on just one channel) and asserts
// only the intended voice releases.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DRUM_MIDI_CHANNEL 9
#define MAX_POLYPHONY 8

typedef enum { VOICE_FREE, VOICE_HELD, VOICE_RELEASING } voice_state_t;
typedef enum { BANK_NONE, BANK_PIANO, BANK_DRUMS } bank_t;

typedef struct {
    voice_state_t state;
    bank_t bank;
    uint8_t channel;
    uint8_t note;
} voice_t;

static voice_t s_voices[MAX_POLYPHONY];

static voice_t *find_free_voice(void) {
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state == VOICE_FREE) return &s_voices[i];
    }
    return NULL; // not modeling voice-stealing here -- routing/matching is what's under test
}

// Mirrors voice_engine_note_on()'s bank-selection decision.
static voice_t *simulate_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0) return NULL;
    voice_t *voice = find_free_voice();
    if (!voice) return NULL;
    voice->state = VOICE_HELD;
    voice->bank = (channel == DRUM_MIDI_CHANNEL) ? BANK_DRUMS : BANK_PIANO;
    voice->channel = channel;
    voice->note = note;
    return voice;
}

// Mirrors voice_engine_note_off()'s (note, channel) matching -- the
// actual fixed bug: must check channel, not just note.
static int simulate_note_off(uint8_t channel, uint8_t note) {
    int matched = 0;
    for (int i = 0; i < MAX_POLYPHONY; i++) {
        if (s_voices[i].state == VOICE_HELD && s_voices[i].note == note && s_voices[i].channel == channel) {
            s_voices[i].state = VOICE_RELEASING;
            matched++;
        }
    }
    return matched;
}

int main(void) {
    int failures = 0;
    memset(s_voices, 0, sizeof(s_voices));

    // Simulated MIDI event sequence:
    //   1. Note On, channel 0 (piano), note 60, velocity 100
    //   2. Note On, channel 9 (drums, human channel 10), note 60 (same
    //      note number -- e.g. GM key 60 has no drum meaning, but the
    //      point is the *number* collides), velocity 100
    //   3. Note Off, channel 0, note 60 -- must release ONLY the piano voice
    voice_t *piano_voice = simulate_note_on(0, 60, 100);
    voice_t *drum_voice = simulate_note_on(DRUM_MIDI_CHANNEL, 60, 100);

    if (!piano_voice || piano_voice->bank != BANK_PIANO) {
        fprintf(stderr, "FAIL: channel 0 note-on didn't route to piano\n");
        failures++;
    }
    if (!drum_voice || drum_voice->bank != BANK_DRUMS) {
        fprintf(stderr, "FAIL: channel %d note-on didn't route to drums\n", DRUM_MIDI_CHANNEL);
        failures++;
    }
    if (piano_voice == drum_voice) {
        fprintf(stderr, "FAIL: piano and drum note-on were allocated the same voice slot\n");
        failures++;
    }

    int matched = simulate_note_off(0, 60);
    if (matched != 1) {
        fprintf(stderr, "FAIL: note-off on channel 0 matched %d voices, expected exactly 1\n", matched);
        failures++;
    }
    if (piano_voice->state != VOICE_RELEASING) {
        fprintf(stderr, "FAIL: piano voice wasn't released by its own channel's note-off\n");
        failures++;
    }
    if (drum_voice->state != VOICE_HELD) {
        fprintf(stderr, "FAIL: drum voice was incorrectly released by a note-off on a different channel "
                        "(the actual bug this test exists to catch: matching by note number alone)\n");
        failures++;
    }

    // A second, independent check: a note-off on the drum channel for
    // the same note number now correctly releases only the drum voice.
    matched = simulate_note_off(DRUM_MIDI_CHANNEL, 60);
    if (matched != 1) {
        fprintf(stderr, "FAIL: note-off on drum channel matched %d voices, expected exactly 1\n", matched);
        failures++;
    }
    if (drum_voice->state != VOICE_RELEASING) {
        fprintf(stderr, "FAIL: drum voice wasn't released by its own channel's note-off\n");
        failures++;
    }

    // Velocity-0 Note On must not allocate a voice (MIDI's "running
    // status" Note-On-as-Note-Off convention) -- a basic sanity check
    // on the same simulated dispatch path.
    memset(s_voices, 0, sizeof(s_voices));
    voice_t *zero_vel_voice = simulate_note_on(0, 60, 0);
    if (zero_vel_voice != NULL) {
        fprintf(stderr, "FAIL: velocity-0 Note On allocated a voice\n");
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: channel-based bank routing and per-channel note-off matching both correct\n");
    return 0;
}
