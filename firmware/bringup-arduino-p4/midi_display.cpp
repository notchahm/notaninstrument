// Bring-up step 3 (docs/bring-up-plan.md): SSD1306 OLED display, adapted
// from firmware/legacy_code/midi_display.cpp (RP2350 prior art -- kept as
// reference only, per CLAUDE.md's convention for that directory, not a
// base to build on directly). That version doesn't compile as-is:
// display_note()'s implementation doesn't match its own header's
// declared signature and references undefined variables (note_index,
// octave, midi_note_names), F() is misapplied to runtime variables (it's
// for wrapping compile-time string literals, not pointers), fillRect() is
// called with corner coordinates where width/height are expected, and
// leftover pinMode() calls configure GPIO26/27 for a TM1637 7-segment
// display that was never actually used with this OLED. Fixed all of the
// above.
//
// Wiring is this board's confirmed I2C pins (docs/hardware-bom.md):
// SDA=GPIO7, SCL=GPIO8, 5V, GND. GeeekPi 128x64 SSD1306 module.

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "midi_display.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1       // no dedicated reset pin
#define SCREEN_ADDRESS 0x3C // 0x3D for some 128x64 modules -- try that if begin() fails

#define I2C_SDA_PIN 7
#define I2C_SCL_PIN 8

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Scans all 7-bit I2C addresses and reports which ACK -- run before
// trusting display.begin()'s own pass/fail, since that alone doesn't say
// whether the bus is even electrically alive (vs. e.g. a dead 5V-to-3.3V
// regulator on the display module, swapped SDA/SCL, or no continuity).
void scan_i2c_bus() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("  Found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  No I2C devices found -- check wiring/power before "
                    "suspecting the display library or SCREEN_ADDRESS.");
  }
}

void initialize_midi_display() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  scan_i2c_bus();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
    return;
  }
  display.clearDisplay();
  display.display();
}

void display_text(const char* text) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(text);
  display.display();
}

void display_note(int channel, int note, int velocity) {
  char channel_buffer[24];
  char note_buffer[24];
  snprintf(channel_buffer, sizeof(channel_buffer), "Channel: %d", channel);
  snprintf(note_buffer, sizeof(note_buffer), "Note %d", note);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(channel_buffer);
  display.setCursor(0, 20);
  display.println(note_buffer);

  // Velocity bar along the bottom, width scaled 0-127 -> 0-SCREEN_WIDTH.
  int bar_width = map(velocity, 0, 127, 0, SCREEN_WIDTH);
  display.fillRect(0, SCREEN_HEIGHT - 8, bar_width, 8, SSD1306_WHITE);

  display.display();
}
