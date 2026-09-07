// Bring-up steps 2 + 3 (docs/bring-up-plan.md): USB MIDI host, built
// directly on ESP-IDF + TinyUSB's host stack (tuh_* API) -- no Arduino
// involved (CLAUDE.md architecture decision #4) -- now also driving an
// SSD1306 OLED (via the k0i05/esp_ssd1306 component) to show incoming
// Note On/Off events in real time, integrating the two bring-up steps
// that were previously only proven separately (spike-usb-midi-idf's own
// tuh_midi_rx_cb logging, and firmware/notaninstrument-p4's Arduino-based
// display test). Still a standalone diagnostic, not the real project.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "ssd1306.h"
#include "tusb.h"

static const char *TAG = "usb_midi_spike";

// SSD1306 OLED, confirmed working wiring from docs/hardware-bom.md
// (firmware/notaninstrument-p4's bring-up step 3): SDA=GPIO7, SCL=GPIO8.
// Shares the bus with the onboard ES8311 codec (0x18) -- no address
// collision with the display (0x3C), confirmed via I2C scan on real
// hardware.
#define DISPLAY_SDA_GPIO 7
#define DISPLAY_SCL_GPIO 8

static ssd1306_handle_t s_display;

static void init_display(void) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = DISPLAY_SDA_GPIO,
        .scl_io_num = DISPLAY_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    ssd1306_config_t dev_config = I2C_SSD1306_128x64_CONFIG_DEFAULT;
    ESP_ERROR_CHECK(ssd1306_init(bus_handle, &dev_config, &s_display));

    ssd1306_clear_display(s_display, false);
    ssd1306_display_text(s_display, 0, "Waiting for", false);
    ssd1306_display_text(s_display, 1, "MIDI input...", false);
}

// Scientific pitch notation (MIDI note 60 = C4 = middle C, 69 = A4 = 440Hz
// -- the same convention General MIDI and most DAWs use, though Ableton
// notably offsets by one octave; not universal, but the least surprising
// default). octave can go negative for the lowest MIDI notes (0-11 -> C-1
// .. B-1); snprintf's %d handles that fine.
static const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void midi_note_name(uint8_t note, char *buf, size_t buf_size) {
    int octave = (note / 12) - 1;
    const char *name = NOTE_NAMES[note % 12];
    snprintf(buf, buf_size, "%s%d", name, octave);
}

// Rate-limits actual display writes. Without this, a fast-streaming
// controller (a joystick or pot sending CC updates every few ms) drives
// this function far faster than the I2C bus + this library's page-by-page
// text rendering can keep up with, which was visible as lag (updates
// queuing up behind real-time input) and flicker (see below). 20Hz is
// smooth to a human eye and comfortably below what the I2C bus can
// sustain for this page-oriented text API.
#define DISPLAY_MIN_UPDATE_INTERVAL_US (50 * 1000) // 50ms = 20Hz
static int64_t s_last_display_update_us = 0;

// Shared layout for both note and CC events (128x64 = 8 pages of 8px each;
// _x2 text spans 2 pages, _x3 spans 3 -- confirmed by reading ssd1306.c,
// not guessed. _x2 lines fit at most 8 chars, _x3 lines fit at most 5, per
// SSD1306_TEXT_X{2,3}_DISPLAY_MAX_LEN):
//   pages 0-1 (_x2): "Ch N"            -- always channel, for consistency
//   pages 2-4 (_x3): the event's "identity" -- note name, or "CCnn"
//   pages 5-6 (_x2): the event's "intensity" -- "Vel N" or "Val N"
//   page  7   (pixel row 56-63): a filled bar, width scaled to that value
//
// Every field is written at a fixed width (padded with trailing spaces)
// instead of clearing the whole display first -- ssd1306_clear_display()
// followed by a page-by-page redraw is what caused the visible flicker
// ("blinks on and off"): the screen briefly went fully blank on every
// single MIDI message before the new content landed. Overwriting
// fixed-width fields in place never blanks anything -- old characters get
// replaced by new ones (or trailing spaces) in the same write.
static void update_display(uint8_t channel, const char *identity, const char *intensity_label,
                            int intensity_value, bool show_bar) {
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_display_update_us < DISPLAY_MIN_UPDATE_INTERVAL_US) {
        return; // drop this update -- a later one will land once the window reopens
    }
    s_last_display_update_us = now_us;

    char channel_line[9];
    char intensity_line[9];

    // channel is the raw 0-15 value from the status byte's low nibble --
    // correct for the wire protocol, but MIDI channels are conventionally
    // shown to humans as 1-16 (every DAW/hardware display does this).
    // Only the display formatting adds 1; channel stays 0-based everywhere
    // else (e.g. if this is later wired into start_note/stop_note calls).
    snprintf(channel_line, sizeof(channel_line), "Ch %-5u", channel + 1);
    ssd1306_display_text_x2(s_display, 0, channel_line, false);

    char identity_padded[6];
    snprintf(identity_padded, sizeof(identity_padded), "%-5s", identity);
    ssd1306_display_text_x3(s_display, 2, identity_padded, false);

    if (show_bar) {
        snprintf(intensity_line, sizeof(intensity_line), "%s %-4u", intensity_label, intensity_value);
    } else {
        snprintf(intensity_line, sizeof(intensity_line), "%-8s", ""); // blank, no "(off)" text
    }
    ssd1306_display_text_x2(s_display, 5, intensity_line, false);

    // The bar can only shrink or grow from wherever it was left -- clear
    // its whole page first so a shorter bar doesn't leave old pixels lit
    // beyond its new (shorter) width.
    ssd1306_clear_display_page(s_display, 7, false);
    if (show_bar) {
        int bar_width = intensity_value * 128 / 127;
        ssd1306_display_filled_rectangle(s_display, 0, 56, bar_width, 8, false);
    }
}

// Called from tuh_midi_rx_cb below for Note On (CIN 0x9) / Note Off
// (CIN 0x8) USB-MIDI Event Packets.
static void update_display_for_note(bool note_on, uint8_t channel, uint8_t note, uint8_t velocity) {
    char note_name[6]; // e.g. "C#-1", "G9" -- fits the x3 5-char limit
    midi_note_name(note, note_name, sizeof(note_name));
    update_display(channel, note_name, "Vel", velocity, note_on);
}

// Called from tuh_midi_rx_cb below for Control Change (CIN 0xB) USB-MIDI
// Event Packets. CC value 0 is a real, meaningful value (not an "off"
// like Note On velocity 0 is) so the bar always shows here.
static void update_display_for_cc(uint8_t channel, uint8_t controller, uint8_t value) {
    char identity[6]; // "CC127" is exactly 5 chars, the x3 limit
    snprintf(identity, sizeof(identity), "CC%u", controller);
    update_display(channel, identity, "Val", value, true);
}

// TinyUSB's generic DWC2 port (components/tinyusb_host/src/portable/
// synopsys/dwc2/dwc2_esp32.h) has dwc2_phy_init()/dwc2_phy_update() as
// literal no-op stubs on ESP32 targets ("// maybe usb_utmi_hal_init()").
// Confirmed on real hardware 2026-09-06: without this, tusb_init()
// "succeeds" but any access to the HS controller's registers
// (DWC2_HS_REG_BASE, 0x50000000 on P4) takes a Load access fault --
// the peripheral is never actually clocked/powered on. usb_new_phy() is
// ESP-IDF's own (esp_hw_support/usb_phy) API for that -- "This function
// will enable the OTG Controller" per its own doc comment. Config verified
// against esp_hw_support's own test suite (test_apps/usb_phy, "Init
// internal UTMI PHY" case), not guessed.
static usb_phy_handle_t s_usb_phy_handle;

static void init_usb_phy(void) {
    const usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_config, &s_usb_phy_handle));
}

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

        // USB-MIDI Event Packet: packet[0] low nibble = Code Index Number,
        // packet[1] = MIDI status byte, packet[2]/[3] = data bytes.
        // CIN 0x8 = Note Off, 0x9 = Note On (velocity 0 is conventionally
        // treated as Note Off too, per the MIDI spec), 0xB = Control
        // Change -- per the USB-MIDI 1.0 CIN table, not guessed.
        uint8_t cin = packet[0] & 0x0F;
        uint8_t status = packet[1];
        uint8_t channel = status & 0x0F;
        uint8_t data1 = packet[2];
        uint8_t data2 = packet[3];
        if (cin == 0x9 && data2 > 0) {
            update_display_for_note(true, channel, data1, data2);
        } else if (cin == 0x8 || (cin == 0x9 && data2 == 0)) {
            update_display_for_note(false, channel, data1, data2);
        } else if (cin == 0xB) {
            update_display_for_cc(channel, data1, data2);
        }
    }
}

static void usb_host_task(void *arg) {
    (void) arg;

    init_usb_phy();
    ESP_LOGI(TAG, "init_usb_phy() done");

    tusb_init();
    ESP_LOGI(TAG, "tusb_init() done, entering tuh_task() loop -- "
                  "plug in a USB-MIDI controller now");

    while (1) {
        tuh_task();
    }
}

void app_main(void) {
    init_display();

    // Runs TinyUSB's host task on its own FreeRTOS task rather than
    // app_main's, matching the pattern used by TinyUSB's own ESP-IDF
    // examples (app_main can return; tuh_task() must run forever).
    xTaskCreate(usb_host_task, "usb_host_task", 4096, NULL, 5, NULL);
}
