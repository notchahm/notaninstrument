// Bring-up step 2 (docs/bring-up-plan.md): USB MIDI host spike test on the
// Arduino-ESP32 + Adafruit TinyUSB path. Standalone sketch, not the full
// project -- the only question this answers is whether tuh_midi_mount_cb /
// tuh_midi_rx_cb fire when a class-compliant USB-MIDI controller is
// plugged into the board's USB-A host port. See
// ../spike-usb-midi-idf/main/main.c for the same test built directly on
// ESP-IDF + TinyUSB, no Arduino involved (CLAUDE.md architecture
// decision #4) -- both spikes implement identical tuh_midi_* callbacks so
// the results are directly comparable.
//
// Requires the "Adafruit TinyUSB Library" (Tools > Manage Libraries).
//
// CONFIRMED (see README.md): this does NOT currently compile against
// arduino-esp32 3.3.11 + Adafruit TinyUSB Library 3.7.7. That combo forces
// CFG_TUH_MAX3421 (an external SPI host-controller chip not in this
// project's BOM) as the only host controller on ESP32 targets, and never
// enables CFG_TUH_MIDI at all -- there's no supported way to override
// either from a sketch on ESP32. Kept as reference/documentation using the
// current tuh_midi_* API, not as a working build right now.

#include "Adafruit_TinyUSB.h"

Adafruit_USBH_Host USBHost;

void setup() {
  Serial.begin(115200);
  unsigned long wait_start = millis();
  while (!Serial && millis() - wait_start < 3000) {
    // give a USB-CDC/UART bridge time to enumerate; don't hang forever
  }

  Serial.println();
  Serial.println("=== USB-MIDI host spike (Arduino + Adafruit TinyUSB) ===");
  Serial.println("Plug a class-compliant USB-MIDI controller into the host port now...");

  // rhport 0 -- TBD: verify this is the port actually wired to the board's
  // USB-A host jack(s); see tusb_config.h.
  USBHost.begin(0);
}

void loop() {
  USBHost.task();
}

// --- TinyUSB host callbacks, mirrors ../spike-usb-midi-idf/main/main.c ---

void tuh_mount_cb(uint8_t daddr) {
  Serial.printf("USB device mounted, address=%u\n", daddr);
}

void tuh_umount_cb(uint8_t daddr) {
  Serial.printf("USB device unmounted, address=%u\n", daddr);
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data) {
  Serial.printf("MIDI interface mounted: idx=%u addr=%u itf=%u rx_cables=%u tx_cables=%u\n",
                idx, mount_cb_data->daddr, mount_cb_data->bInterfaceNumber,
                mount_cb_data->rx_cable_count, mount_cb_data->tx_cable_count);
}

void tuh_midi_umount_cb(uint8_t idx) {
  Serial.printf("MIDI interface unmounted: idx=%u\n", idx);
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
  (void) xferred_bytes;
  uint8_t packet[4];
  while (tuh_midi_packet_read(idx, packet)) {
    Serial.printf("MIDI packet: %02x %02x %02x %02x\n",
                  packet[0], packet[1], packet[2], packet[3]);
  }
}
