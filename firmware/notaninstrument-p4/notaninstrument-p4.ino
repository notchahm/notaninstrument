// Bring-up steps 1 + 3 + 4 (docs/bring-up-plan.md): toolchain smoke test,
// not the real firmware. Blinks an LED, logs chip/PSRAM info over serial,
// drives an SSD1306 OLED (midi_display.cpp) with a periodic demo cycle,
// and plays a continuous test tone through a PCM5102A over I2S
// (audio_output.cpp) -- proves the I2S wiring/clocking/DMA path in
// isolation before MIDI or SD touch it.

#include "midi_display.h"
#include "audio_output.h"

// There is no GPIO-controlled LED on this board at all (confirmed against
// the vendor schematic, docs/datasheets/ESP32-P4-WIFI6-DEV-KIT-schematic.pdf)
// -- the only LED present is a fixed power-on indicator wired straight to
// VCC_5V, not software-controllable. This is a pure software heartbeat
// with no physical indicator, kept for the serial "tick" log only.
// GPIO22 was picked as a plain, otherwise-unused header pin -- GPIO2 was
// the original guess and turned out to double as PCM5102_FMT_GPIO
// (audio_output.cpp), which caused an audible "pulsing" artifact twice a
// second by scrambling the DAC's format-select pin every toggle.
#define LED_PIN 22

#define SERIAL_BAUD 115200
#define BLINK_INTERVAL_MS 500
// Repeats the boot banner periodically, not just once at boot -- a serial
// monitor attaching even a second late (the common case: reset happens
// before a host-side reader has opened the port) would otherwise never see
// chip/PSRAM info at all.
#define STATUS_INTERVAL_MS 10000
// How often to cycle the OLED to a new demo note -- separate from
// STATUS_INTERVAL_MS so the display and serial banner don't have to
// refresh in lockstep.
#define DISPLAY_DEMO_INTERVAL_MS 2000

static bool led_state = false;
static unsigned long last_toggle_ms = 0;
static unsigned long last_status_ms = 0;
static unsigned long last_display_demo_ms = 0;
static int demo_note_index = 0;

static void print_status_banner() {
  Serial.println();
  Serial.println("=== notaninstrument P4 bring-up ===");
  Serial.printf("Chip model:      %s\n", ESP.getChipModel());
  Serial.printf("Chip revision:   %d\n", ESP.getChipRevision());
  Serial.printf("CPU cores:       %d\n", ESP.getChipCores());
  Serial.printf("CPU freq:        %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Flash size:      %lu bytes\n", (unsigned long)ESP.getFlashChipSize());

  if (psramFound()) {
    Serial.printf("PSRAM size:      %lu bytes (expect ~32MB per hardware-bom.md)\n",
                  (unsigned long)ESP.getPsramSize());
  } else {
    Serial.println("PSRAM size:      NOT DETECTED -- check board config "
                    "(PSRAM mode/menu option) before proceeding past bring-up step 1");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(SERIAL_BAUD);
  unsigned long wait_start = millis();
  while (!Serial && millis() - wait_start < 3000) {
    // give a USB-CDC/UART bridge time to enumerate; don't hang forever
  }

  print_status_banner();
  Serial.println("Blinking LED_PIN, logging a tick every toggle...");

  initialize_midi_display();
  display_text("notaninstrument\nP4 bring-up");

  // Test tone disabled now that step 4 is confirmed (docs/bring-up-plan.md)
  // -- was making continuous noise on every subsequent flash/test of other
  // bring-up steps. initialize_audio_output() (I2S/DAC setup) still runs
  // so the path stays exercised; only the continuous playback is off.
  // Uncomment start_audio_output_task() to re-enable the tone.
  initialize_audio_output();
  // start_audio_output_task();
  Serial.println("Audio: I2S/PCM5102A initialized, test tone disabled "
                  "(see notaninstrument-p4.ino setup())");
}

void loop() {
  unsigned long now = millis();
  if (now - last_toggle_ms >= BLINK_INTERVAL_MS) {
    last_toggle_ms = now;
    led_state = !led_state;
    digitalWrite(LED_PIN, led_state ? HIGH : LOW);
    Serial.printf("[%lu ms] tick, led=%s\n", now, led_state ? "ON" : "OFF");
  }
  if (now - last_status_ms >= STATUS_INTERVAL_MS) {
    last_status_ms = now;
    print_status_banner();
    scan_i2c_bus();
    print_audio_status();
  }
  if (now - last_display_demo_ms >= DISPLAY_DEMO_INTERVAL_MS) {
    last_display_demo_ms = now;
    // No real MIDI input wired up yet (that's spike-usb-midi-idf, a
    // separate ESP-IDF project) -- cycle fake note data so display_note()
    // (text layout, velocity bar) is visually exercised without one.
    int channel = demo_note_index % 16;
    int note = 60 + (demo_note_index % 12);
    int velocity = (demo_note_index * 17) % 128;
    display_note(channel, note, velocity);
    demo_note_index++;
  }
}
