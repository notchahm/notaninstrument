#include "audio_output.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "voice_engine.h"

static const char *TAG = "audio_output";

// PCM5102A wiring, confirmed on real hardware via
// firmware/bringup-arduino-p4 (docs/hardware-bom.md): BCK=GPIO4,
// LRCK/WS=GPIO5, DIN=GPIO6. No MCLK connected -- the DAC derives its own
// clock internally via its onboard PLL, using BCK as reference.
#define I2S_BCK_GPIO  GPIO_NUM_4
#define I2S_WS_GPIO   GPIO_NUM_5
#define I2S_DOUT_GPIO GPIO_NUM_6

// This module breaks XSMT/FMT/FLT out to pins instead of hard-wiring them
// on-board, so they float (undefined logic level) until driven. Real
// hardware gotcha, also confirmed via firmware/bringup-arduino-p4: this
// module additionally has a separate SCK pin (distinct from BCK) that
// must be tied directly to GND -- not a GPIO, a physical wire -- to
// select the DAC's internal-PLL clock mode. Without that, it stays
// completely silent regardless of how correct everything else is.
#define PCM5102_FLT_GPIO  GPIO_NUM_1 // filter select: LOW = normal (sharp) roll-off
#define PCM5102_FMT_GPIO  GPIO_NUM_2 // format select: LOW = I2S standard
#define PCM5102_XSMT_GPIO GPIO_NUM_3 // soft mute: HIGH = unmuted, LOW = muted

#define SAMPLE_RATE_HZ 48000

// Renders directly inside the I2S driver's on_sent interrupt (real
// hardware DMA-completion interrupt, not a task woken by one) instead of
// a separate FreeRTOS task blocking on i2s_channel_write() in a loop --
// docs/polyphony-latency-investigation.md has the full history: a first
// attempt at this crashed immediately because voice_engine_render was
// float-based and ESP-IDF's RISC-V port forbids FPU use in ISRs; that's
// resolved now by voice_engine.c's fixed-point rewrite, and this is the
// standard low-latency pattern for embedded audio (Teensy Audio Library,
// GBA Direct Sound) -- DMA-interrupt-driven double buffering rather than
// a task+blocking-write loop.
//
// dma_desc_num x dma_frame_num was originally 6 x 240 = 1440 frames
// (30ms at 48kHz) of already-decided audio queued ahead of any freshly
// rendered buffer. DMA_DESC_NUM x BUFFER_FRAMES below is 3 x 120 (the
// driver rounds up to 128 for DMA alignment) = 384 frames (~8ms) --
// close to Teensy Audio Library's own ~128-sample (2.9ms @ 44.1kHz)
// block size, same reasoning: small enough for low latency, still large
// enough that per-buffer interrupt overhead isn't wasteful.
#define BUFFER_FRAMES 120
#define DMA_DESC_NUM  3
static i2s_chan_handle_t tx_channel;

static void set_static_gpio(gpio_num_t gpio, int level) {
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, level);
}

// Runs in ISR context, fired by the I2S driver the instant a DMA buffer
// finishes sending -- event->dma_buf points directly at that now-free
// buffer ("The first level pointer of DMA buffer that just finished
// sending", per i2s_event_data_t's own doc comment), so rendering
// straight into it is a genuine zero-copy handoff: no separate
// audio_buffer, no extra copy into the DMA descriptor.
static bool on_i2s_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void) handle;
    (void) user_ctx;
    int frame_count = event->size / (2 * sizeof(int16_t)); // stereo int16
    voice_engine_render_isr((int16_t *) event->dma_buf, frame_count);
    return false; // no higher-priority task woken that needs a context-switch request
}

void init_audio_output(void) {
    set_static_gpio(PCM5102_FLT_GPIO, 0);
    set_static_gpio(PCM5102_FMT_GPIO, 0);
    set_static_gpio(PCM5102_XSMT_GPIO, 0);

    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_config.dma_desc_num = DMA_DESC_NUM;
    chan_config.dma_frame_num = BUFFER_FRAMES;
    // Deliberately NOT setting auto_clear_after_cb: confirmed by reading
    // the driver source (esp_driver_i2s/i2s_common.c) that this memsets
    // the buffer to zero immediately AFTER on_sent returns -- i.e. it
    // would wipe out the render we just wrote into event->dma_buf right
    // before it gets queued for transmission. Total silence on real
    // hardware (audible for zero notes, including single ones) is
    // exactly what that produces. It's meant for a different pattern
    // (silence-by-default unless something else fills the buffer via a
    // separate write), not zero-copy rendering inside the callback.
    ESP_ERROR_CHECK(i2s_new_channel(&chan_config, &tx_channel, NULL));

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_channel, &std_config));

    i2s_event_callbacks_t callbacks = {
        .on_sent = on_i2s_sent,
    };
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx_channel, &callbacks, NULL));

    ESP_ERROR_CHECK(i2s_channel_enable(tx_channel));

    gpio_set_level(PCM5102_XSMT_GPIO, 1); // unmute now that I2S is clocking

    ESP_LOGI(TAG, "PCM5102A ready (BCK=GPIO4, LRCK=GPIO5, DIN=GPIO6), "
                  "DMA-interrupt-driven, %d x %d-frame buffers", DMA_DESC_NUM, BUFFER_FRAMES);
}
