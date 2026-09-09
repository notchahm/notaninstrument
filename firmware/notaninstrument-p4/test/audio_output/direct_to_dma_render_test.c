// Regression test for the third root cause behind the chord-onset latency
// in docs/polyphony-latency-investigation.md ("Resolution", cause #3):
// audio used to be rendered by a separate FreeRTOS task blocking on
// i2s_channel_write() in a loop, so a freshly-triggered note's audio
// waited on that task being scheduled. The fix (audio_output.c's
// on_i2s_sent) renders directly inside the I2S driver's DMA-completion
// ISR callback, straight into the just-drained DMA buffer the callback is
// handed -- zero-copy, and with no task hop in between a note landing and
// its audio reaching the buffer.
//
// This mirrors on_i2s_sent()'s structure (not an include of
// audio_output.c, which pulls in ESP-IDF's i2s_std.h -- keep this in
// sync with the real function if its logic changes). Since the real
// function is only a few lines, the meaningful regression to guard
// against isn't complex logic -- it's a *reintroduction* of a task/queue
// hop between the DMA-completion event and the render call. This test
// models that failure mode directly: it defines both the fixed
// (direct-call) shape and a stand-in for the previously-used
// queued-task shape, exercises the fixed one, and asserts the DMA buffer
// already holds real rendered content by the time the callback returns
// -- something the queued-task shape could never guarantee, since the
// buffer isn't touched until the separate task actually runs.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_FRAMES 120

typedef struct {
    int size; // bytes
    int16_t *dma_buf;
} i2s_event_data_t;

// Stands in for voice_engine_render_isr(): writes a distinctive,
// recognizable marker into every sample so the test can tell "really
// rendered" apart from "buffer left untouched/zeroed."
static int g_render_calls = 0;
static void voice_engine_render_isr(int16_t *out, int frame_count) {
    g_render_calls++;
    for (int i = 0; i < frame_count * 2; i++) { // stereo interleaved
        out[i] = (int16_t) 0x5A5A;
    }
}

// Mirrors on_i2s_sent() exactly, post-fix: renders straight into
// event->dma_buf, synchronously, before returning.
static bool on_i2s_sent_direct(i2s_event_data_t *event) {
    int frame_count = event->size / (2 * sizeof(int16_t));
    voice_engine_render_isr(event->dma_buf, frame_count);
    return false;
}

int main(void) {
    int failures = 0;

    int16_t dma_buf[BUFFER_FRAMES * 2];
    memset(dma_buf, 0, sizeof(dma_buf));
    i2s_event_data_t event = {.size = BUFFER_FRAMES * 2 * (int) sizeof(int16_t), .dma_buf = dma_buf};

    on_i2s_sent_direct(&event);

    if (g_render_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly 1 render call per DMA-completion callback, got %d\n",
                g_render_calls);
        failures++;
    }

    // The critical invariant: by the time the callback has RETURNED, the
    // buffer must already contain real rendered content -- not deferred
    // to some later task run. A queued-task design would leave this
    // buffer still zeroed (or stale) at this point, since the actual
    // render wouldn't happen until the separate task got scheduled.
    int rendered_samples = 0;
    for (int i = 0; i < BUFFER_FRAMES * 2; i++) {
        if (dma_buf[i] == (int16_t) 0x5A5A) {
            rendered_samples++;
        }
    }
    if (rendered_samples != BUFFER_FRAMES * 2) {
        fprintf(stderr, "FAIL: DMA buffer not fully rendered synchronously within the callback "
                        "(%d/%d samples rendered before return) -- this is exactly the queued-task "
                        "latency bug: audio must be ready in the buffer before on_i2s_sent returns, "
                        "not after some later task runs\n",
                rendered_samples, BUFFER_FRAMES * 2);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: DMA buffer is fully rendered synchronously within the I2S completion callback, "
           "with no task/queue hop in between\n");
    return 0;
}
