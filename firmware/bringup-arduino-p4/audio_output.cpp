#include "audio_output.h"

#include <Arduino.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// PCM5102A wiring (docs/hardware-bom.md): BCK=GPIO4, LRCK/WS=GPIO5,
// DIN=GPIO6. No MCLK connected -- the PCM5102A derives its own clock
// internally via its onboard PLL, using BCK as the reference.
//
// Real-hardware gotcha (confirmed the actual cause of total silence on
// this exact module, despite correct I2S data and a correctly-unmuted
// XSMT): this module also breaks out a separate SCK pin (distinct from
// BCK) that MUST be tied to GND -- not just left floating -- to select
// that internal-PLL mode at all. SCK is a physical wire straight to GND,
// not a GPIO this firmware drives.
#define I2S_BCK_GPIO  GPIO_NUM_4
#define I2S_WS_GPIO   GPIO_NUM_5
#define I2S_DOUT_GPIO GPIO_NUM_6

// This particular PCM5102A module breaks XSMT/FMT/FLT out to pins instead
// of hard-wiring them on-board, so they're floating (undefined logic
// level) until firmware drives them -- confirmed as the actual cause of
// "no sound despite correct I2S data" on this board. All three are plain
// static logic levels, not I2S signals.
#define PCM5102_FLT_GPIO  1 // filter select: LOW = normal (sharp) roll-off
#define PCM5102_FMT_GPIO  2 // format select: LOW = I2S standard, matches I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG below
#define PCM5102_XSMT_GPIO 3 // soft mute: HIGH = unmuted/normal operation, LOW = muted

#define SAMPLE_RATE_HZ 48000
// Chosen so SAMPLE_RATE_HZ / TONE_HZ divides evenly -- one full cycle fits
// exactly in the buffer with no phase discontinuity at the loop boundary
// (which would otherwise click on every repeat).
#define TONE_HZ 480
#define SAMPLES_PER_CYCLE (SAMPLE_RATE_HZ / TONE_HZ)
#define TONE_AMPLITUDE 8000 // ~-12dBFS of int16 full scale -- audible, not clipping

// One cycle precomputed once at init, not per-sample sin() in a hot loop
// (CLAUDE.md's documented lesson from the RP2350 reference code) -- DMA
// just replays this buffer continuously.
static int16_t tone_buffer[SAMPLES_PER_CYCLE * 2]; // interleaved stereo
static i2s_chan_handle_t tx_channel;

static esp_err_t init_new_channel_err = ESP_FAIL;
static esp_err_t init_std_mode_err = ESP_FAIL;
static esp_err_t init_enable_err = ESP_FAIL;
static volatile uint32_t total_writes = 0;
static volatile uint32_t total_write_errors = 0;
static volatile uint32_t total_bytes_written = 0;

static void generate_tone_buffer() {
  for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
    int16_t sample = (int16_t)(TONE_AMPLITUDE * sinf(2.0f * (float)M_PI * i / SAMPLES_PER_CYCLE));
    tone_buffer[i * 2] = sample;     // left
    tone_buffer[i * 2 + 1] = sample; // right
  }
}

void initialize_audio_output() {
  // Set filter/format before the DAC clocks up, unmute (XSMT) only after
  // I2S is actually running below -- avoids unmuting into an unclocked,
  // undefined input.
  pinMode(PCM5102_FLT_GPIO, OUTPUT);
  digitalWrite(PCM5102_FLT_GPIO, LOW);
  pinMode(PCM5102_FMT_GPIO, OUTPUT);
  digitalWrite(PCM5102_FMT_GPIO, LOW);
  pinMode(PCM5102_XSMT_GPIO, OUTPUT);
  digitalWrite(PCM5102_XSMT_GPIO, LOW);

  i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  init_new_channel_err = i2s_new_channel(&chan_config, &tx_channel, NULL);

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
  init_std_mode_err = i2s_channel_init_std_mode(tx_channel, &std_config);
  init_enable_err = i2s_channel_enable(tx_channel);

  generate_tone_buffer();

  digitalWrite(PCM5102_XSMT_GPIO, HIGH); // unmute now that I2S is clocking
}

static void audio_task(void *arg) {
  (void)arg;
  while (true) {
    size_t bytes_written = 0;
    // Blocks until DMA has room -- this naturally paces continuous
    // playback, which is why it runs on its own task rather than inline
    // in the Arduino loop() alongside the LED/display timing.
    esp_err_t err = i2s_channel_write(tx_channel, tone_buffer, sizeof(tone_buffer), &bytes_written, portMAX_DELAY);
    total_writes++;
    total_bytes_written += bytes_written;
    if (err != ESP_OK) {
      total_write_errors++;
    }
  }
}

void start_audio_output_task() {
  xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);
}

void print_audio_status() {
  Serial.println("--- audio status ---");
  Serial.printf("  i2s_new_channel:        %s (%d)\n", esp_err_to_name(init_new_channel_err), init_new_channel_err);
  Serial.printf("  i2s_channel_init_std:   %s (%d)\n", esp_err_to_name(init_std_mode_err), init_std_mode_err);
  Serial.printf("  i2s_channel_enable:     %s (%d)\n", esp_err_to_name(init_enable_err), init_enable_err);
  Serial.printf("  XSMT (GPIO%d) reads:    %d\n", PCM5102_XSMT_GPIO, digitalRead(PCM5102_XSMT_GPIO));
  Serial.printf("  writes so far:          %lu (errors: %lu, bytes: %lu, expected %u bytes/write)\n",
                (unsigned long)total_writes, (unsigned long)total_write_errors,
                (unsigned long)total_bytes_written, (unsigned)sizeof(tone_buffer));
}
