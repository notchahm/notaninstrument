// Bring-up steps 2 + 3 + 4 + 5 (docs/bring-up-plan.md): USB MIDI host,
// driving an SSD1306 OLED (via the k0i05/esp_ssd1306 component) to show
// incoming Note On/Off events in real time, AND real polyphonic sample
// playback through a PCM5102A (voice_engine.c + nib_loader.c, reading the
// Salamander piano .nib built by tools/sfz_preprocessor/sfz_to_nib.py out
// of a dedicated flash partition -- partitions.csv) -- all of MIDI input,
// display, and actual sampled-instrument audio running as one firmware
// image.
//
// USB MIDI host is ESP-IDF's native USB Host Library (usb_midi_host.c),
// not TinyUSB (components/tinyusb_host/, kept vendored in-tree as
// reference/fallback but no longer built by this app) -- see
// docs/polyphony-latency-investigation.md: the ~242ms chord-onset lag
// this project fought for a long time turned out to be TinyUSB's own
// DWC2 driver stack, not this project's code or the MIDI device, and
// switching to this native path measured 0-11ms chord onset on the same
// real hardware/controller instead.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "audio_output.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ssd1306.h"
#include "usb_midi_host.h"
#include "voice_engine.h"

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

// Reverted to the original single-note display design (confirmed working
// on real hardware in earlier bring-up) after the multi-note redesign
// above showed a real bug on real hardware -- notes only appeared after
// release instead of on press -- and several rounds of layout iteration
// didn't land well. Not worth debugging further right now: this design
// is simple and proven; a multi-note display is worth revisiting later
// as its own separately-tested change, not stacked on top of the
// polyphony/timing work already in flight.
//
// Shared layout for both note and CC events (128x64 = 8 pages of 8px each;
// _x2 text spans 2 pages, _x3 spans 3. _x2 lines fit at most 8 chars,
// _x3 lines fit at most 5, per SSD1306_TEXT_X{2,3}_DISPLAY_MAX_LEN):
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

// Display updates are dispatched through this queue instead of being
// called directly from usb_midi_on_event -- see the note there for why:
// even with the audio-critical-call-first ordering, update_display_for_
// note/cc's I2C write still runs *inside* usb_midi_host.c's bulk-transfer
// completion callback, which delays that callback's own resubmission of
// the next IN transfer for as long as the write takes. Confirmed on real
// hardware (2026-09-07) that this drops notes: a genuinely simultaneous
// chord (3 keys struck together) produced only one Note On reaching this
// file at all, with the others' corresponding Note Offs arriving with no
// matching Note On ever seen -- consistent with the controller's own
// shallow internal MIDI buffer overwriting unsent events while the host
// was blocked in an I2C write instead of re-arming the endpoint. A chord
// played with any perceptible stagger between notes worked fine, which
// fits: enough time between notes for the previous one's (rate-limited,
// so not even every note triggers a real write) I2C write to finish
// before the next one needed the endpoint free again. The queue makes
// usb_midi_on_event's own worst case a fast, non-blocking xQueueSend --
// display work happens entirely on its own low-priority task instead.
typedef struct {
    bool is_cc;
    bool note_on; // meaningful only when !is_cc
    uint8_t channel;
    uint8_t data1; // note or CC controller number
    uint8_t data2; // velocity or CC value
} display_event_t;

static QueueHandle_t s_display_queue;

static void display_task(void *arg) {
    (void) arg;
    display_event_t evt;
    while (true) {
        if (xQueueReceive(s_display_queue, &evt, portMAX_DELAY)) {
            if (evt.is_cc) {
                update_display_for_cc(evt.channel, evt.data1, evt.data2);
            } else {
                update_display_for_note(evt.note_on, evt.channel, evt.data1, evt.data2);
            }
        }
    }
}

// Called from usb_midi_host.c for every decoded USB-MIDI Event Packet
// (raw status/data1/data2 bytes) from any claimed MIDIStreaming
// interface, on whichever device/core happened to service that transfer.
// Must never block -- see display_task above for why.
void usb_midi_on_event(uint8_t status, uint8_t data1, uint8_t data2) {
    // Status byte high nibble = MIDI message type, low nibble = channel.
    // Note On with velocity 0 is conventionally treated as Note Off too,
    // per the MIDI spec, not just when the message type is literally
    // Note Off.
    uint8_t message = status & 0xF0;
    uint8_t channel = status & 0x0F;
    display_event_t evt = {.channel = channel, .data1 = data1, .data2 = data2};
    if (message == 0x90 && data2 > 0) {
        voice_engine_note_on(channel, data1, data2);
        evt.is_cc = false;
        evt.note_on = true;
        xQueueSend(s_display_queue, &evt, 0); // non-blocking; drop on a full queue, display is cosmetic
    } else if (message == 0x80 || (message == 0x90 && data2 == 0)) {
        voice_engine_note_off(channel, data1);
        evt.is_cc = false;
        evt.note_on = false;
        xQueueSend(s_display_queue, &evt, 0);
    } else if (message == 0xB0) {
        evt.is_cc = true;
        xQueueSend(s_display_queue, &evt, 0);
    }
}

void app_main(void) {
    init_display();
    voice_engine_init();
    init_audio_output();
    voice_engine_start_diag_task();
    s_display_queue = xQueueCreate(16, sizeof(display_event_t));
    xTaskCreatePinnedToCore(display_task, "display_task", 4096, NULL, 1, NULL, 1);
    usb_midi_host_start();
}
