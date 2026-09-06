// Bring-up step 2 (docs/bring-up-plan.md): USB MIDI host spike test, built
// directly on ESP-IDF + TinyUSB's host stack (tuh_* API) -- no Arduino
// involved. This is the "raw ESP-IDF" side of CLAUDE.md architecture
// decision #4; see ../../spike-usb-midi-arduino/ for the Arduino +
// Adafruit TinyUSB side of the same comparison. Standalone diagnostic, not
// the real project: the only question this answers is whether
// tuh_midi_mount_cb / tuh_midi_rx_cb fire when a class-compliant USB-MIDI
// controller is plugged into the board's USB-A host port.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "usb_midi_spike";

void tuh_mount_cb(uint8_t daddr) {
    ESP_LOGI(TAG, "USB device mounted, address=%u", daddr);
}

void tuh_umount_cb(uint8_t daddr) {
    ESP_LOGI(TAG, "USB device unmounted, address=%u", daddr);
}

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount_cb_data) {
    ESP_LOGI(TAG,
             "MIDI interface mounted: idx=%u addr=%u itf=%u rx_cables=%u tx_cables=%u",
             idx, mount_cb_data->daddr, mount_cb_data->bInterfaceNumber,
             mount_cb_data->rx_cable_count, mount_cb_data->tx_cable_count);
}

void tuh_midi_umount_cb(uint8_t idx) {
    ESP_LOGI(TAG, "MIDI interface unmounted: idx=%u", idx);
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t xferred_bytes) {
    (void) xferred_bytes;
    uint8_t packet[4];
    while (tuh_midi_packet_read(idx, packet)) {
        ESP_LOGI(TAG, "MIDI packet: %02x %02x %02x %02x",
                 packet[0], packet[1], packet[2], packet[3]);
    }
}

static void usb_host_task(void *arg) {
    (void) arg;

    tusb_init();
    ESP_LOGI(TAG, "tusb_init() done, entering tuh_task() loop -- "
                  "plug in a USB-MIDI controller now");

    while (1) {
        tuh_task();
    }
}

void app_main(void) {
    // Runs TinyUSB's host task on its own FreeRTOS task rather than
    // app_main's, matching the pattern used by TinyUSB's own ESP-IDF
    // examples (app_main can return; tuh_task() must run forever).
    xTaskCreate(usb_host_task, "usb_host_task", 4096, NULL, 5, NULL);
}
