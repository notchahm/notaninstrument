// Regression test for one of the two root causes behind the chord-onset
// latency investigated in docs/polyphony-latency-investigation.md
// ("Resolution", cause #2): debug logging and display updates used to run
// synchronously inside the USB transfer-completion hot path
// (main.c's usb_midi_on_event, called directly from
// usb_midi_host.c's midi_transfer_cb). The fix was to make
// usb_midi_on_event call the audio-critical voice_engine_note_on/off
// FIRST, unconditionally, then hand the display update to a low-priority
// task via a non-blocking xQueueSend(..., 0) -- never waiting for queue
// space, and never skipping the audio call because the queue happened to
// be full or its consumer stalled.
//
// This mirrors usb_midi_on_event()'s dispatch structure (not an include
// of main.c, which pulls in FreeRTOS/ESP-IDF headers not portable outside
// ESP-IDF -- keep this in sync with the real function if its dispatch
// logic changes) with a controllable, always-full fake queue, and checks
// the fixed invariant directly: even when the display queue can never
// accept another item, every Note On/Off must still reach the voice
// engine, and the queue send must always be attempted with a zero
// (non-blocking) wait -- never a wait that could stall the next USB
// transfer's processing behind a slow or stuck display consumer.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Stand-in for FreeRTOS's queue handle/API shape, just enough to model
// the one call site under test.
typedef struct { int dummy; } *QueueHandle_t;
static QueueHandle_t s_display_queue = (QueueHandle_t) 1;

static int g_note_on_calls = 0;
static int g_note_off_calls = 0;
static int g_queue_send_calls = 0;
static int g_queue_send_nonzero_wait_calls = 0;
static int g_queue_send_return_value = 0; // 0 = pdFALSE ("queue full"), matching a stalled/full consumer

static void voice_engine_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    (void) channel; (void) note; (void) velocity;
    g_note_on_calls++;
}
static void voice_engine_note_off(uint8_t channel, uint8_t note) {
    (void) channel; (void) note;
    g_note_off_calls++;
}

typedef struct {
    int is_cc;
    int note_on;
    uint8_t channel, data1, data2;
} display_event_t;

// Mirrors xQueueSend(queue, item, ticks_to_wait) closely enough to check
// the one thing this test cares about: what wait value the real call site
// passes, and that a full queue's return value doesn't get treated as a
// reason to retry/block.
static int xQueueSend(QueueHandle_t queue, const display_event_t *item, int ticks_to_wait) {
    (void) queue; (void) item;
    g_queue_send_calls++;
    if (ticks_to_wait != 0) {
        g_queue_send_nonzero_wait_calls++;
    }
    return g_queue_send_return_value; // always "full" in this test -- see main()
}

// Mirrors main.c's usb_midi_on_event() exactly, post-fix: audio dispatch
// unconditional and first, display dispatch via a non-blocking send whose
// result is never checked/retried (display is cosmetic, per the real
// comment in main.c).
static void usb_midi_on_event(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t message = status & 0xF0;
    uint8_t channel = status & 0x0F;
    display_event_t evt = {.channel = channel, .data1 = data1, .data2 = data2};
    if (message == 0x90 && data2 > 0) {
        voice_engine_note_on(channel, data1, data2);
        evt.is_cc = 0;
        evt.note_on = 1;
        xQueueSend(s_display_queue, &evt, 0);
    } else if (message == 0x80 || (message == 0x90 && data2 == 0)) {
        voice_engine_note_off(channel, data1);
        evt.is_cc = 0;
        evt.note_on = 0;
        xQueueSend(s_display_queue, &evt, 0);
    } else if (message == 0xB0) {
        evt.is_cc = 1;
        xQueueSend(s_display_queue, &evt, 0);
    }
}

int main(void) {
    int failures = 0;
    g_queue_send_return_value = 0; // the display queue is permanently full/stalled for this whole test

    // Simulate a genuinely simultaneous chord: many Note On events for
    // different notes arriving back-to-back, exactly the scenario that
    // used to show only the first note reaching voice_engine_note_on
    // when a slow/blocking display path stole time from this hot path.
    const int chord_size = 8;
    for (int i = 0; i < chord_size; i++) {
        usb_midi_on_event(0x90, 60 + i, 100); // Note On, channel 0
    }
    for (int i = 0; i < chord_size; i++) {
        usb_midi_on_event(0x80, 60 + i, 0); // Note Off, channel 0
    }

    if (g_note_on_calls != chord_size) {
        fprintf(stderr, "FAIL: expected %d voice_engine_note_on calls with a full display queue, got %d "
                        "(a full/stalled display consumer must never suppress audio dispatch)\n",
                chord_size, g_note_on_calls);
        failures++;
    }
    if (g_note_off_calls != chord_size) {
        fprintf(stderr, "FAIL: expected %d voice_engine_note_off calls with a full display queue, got %d\n",
                chord_size, g_note_off_calls);
        failures++;
    }
    if (g_queue_send_calls != 2 * chord_size) {
        fprintf(stderr, "FAIL: expected %d display queue-send attempts, got %d\n",
                2 * chord_size, g_queue_send_calls);
        failures++;
    }
    if (g_queue_send_nonzero_wait_calls != 0) {
        fprintf(stderr, "FAIL: %d display queue-send call(s) used a non-zero wait -- this can block the "
                        "USB transfer-completion hot path behind a full/stalled display consumer, "
                        "exactly the bug this test exists to catch\n",
                g_queue_send_nonzero_wait_calls);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: audio dispatch stays unconditional and display queue-send stays non-blocking "
           "even with a permanently full display queue\n");
    return 0;
}
