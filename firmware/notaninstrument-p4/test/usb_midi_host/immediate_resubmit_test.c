// Regression test for the other root cause behind the ~242ms chord-onset
// gap in docs/polyphony-latency-investigation.md: the project's own
// bulk-IN transfer must be resubmitted the INSTANT it completes, so
// exactly one MIDI IN transfer stays perpetually in flight with no
// host-side polling interval of the driver's own choosing -- that's what
// the comparison test measured as 0-11ms chord onset (native USB Host
// Library) vs. TinyUSB's ~242ms on the same real hardware.
//
// Also covers a second, related fix found during the USB-hub hot-swap
// investigation: if usb_host_transfer_submit() fails (confirmed on real
// hardware with ESP_ERR_INVALID_STATE once a hub-instability event left
// the underlying device handle stale), the transfer must be freed, not
// leaked -- resubmission can never succeed again on that transfer once
// this happens.
//
// Mirrors usb_midi_host.c's midi_transfer_cb() exactly (not an include of
// usb_midi_host.c, which pulls in FreeRTOS/ESP-IDF's usb_host.h --
// keep this in sync with the real function if its logic changes), with
// controllable fake usb_host_transfer_submit/free calls, and checks:
// every completed transfer gets resubmitted unconditionally (the
// "perpetually in flight" invariant), and a resubmit failure frees the
// transfer instead of leaking it.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103

typedef enum {
    USB_TRANSFER_STATUS_COMPLETED = 0,
    USB_TRANSFER_STATUS_NO_DEVICE = 1,
    USB_TRANSFER_STATUS_ERROR = 2,
} usb_transfer_status_t;

typedef struct {
    usb_transfer_status_t status;
    int actual_num_bytes;
    uint8_t data_buffer[64];
} usb_transfer_t;

static const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "ESP_ERR_SOMETHING"; }

static int g_note_events_seen = 0;
static void usb_midi_on_event(uint8_t status, uint8_t data1, uint8_t data2) {
    (void) status; (void) data1; (void) data2;
    g_note_events_seen++;
}

static esp_err_t g_submit_result = ESP_OK;
static int g_submit_calls = 0;
static int g_free_calls = 0;

static esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer) {
    (void) transfer;
    g_submit_calls++;
    return g_submit_result;
}
static void usb_host_transfer_free(usb_transfer_t *transfer) {
    (void) transfer;
    g_free_calls++;
}

// Mirrors midi_transfer_cb() exactly, post-fix.
static void midi_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4) {
            uint8_t *pkt = &transfer->data_buffer[i];
            uint8_t status = pkt[1];
            uint8_t data1 = pkt[2];
            uint8_t data2 = pkt[3];
            if (status != 0) {
                usb_midi_on_event(status, data1, data2);
            }
        }
    }

    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        usb_host_transfer_free(transfer);
        return;
    }

    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        (void) esp_err_to_name(err); // real code logs here
        usb_host_transfer_free(transfer);
    }
}

static void reset_globals(void) {
    g_note_events_seen = 0;
    g_submit_result = ESP_OK;
    g_submit_calls = 0;
    g_free_calls = 0;
}

int main(void) {
    int failures = 0;

    // Case 1: a chord's worth of Note On packets arrive in one transfer.
    // Every packet must be dispatched, AND the transfer must be
    // resubmitted right there in the same callback invocation -- the
    // "perpetually in flight" mechanism itself. This is what lets a
    // second real transfer be in flight again before the *next* USB
    // frame, rather than waiting on a task or a polling interval.
    reset_globals();
    usb_transfer_t transfer = {.status = USB_TRANSFER_STATUS_COMPLETED, .actual_num_bytes = 16};
    for (int i = 0; i < 4; i++) {
        transfer.data_buffer[i * 4 + 0] = 0x00; // cable/CIN, not checked by the mirror
        transfer.data_buffer[i * 4 + 1] = 0x90; // Note On, channel 0
        transfer.data_buffer[i * 4 + 2] = (uint8_t) (60 + i);
        transfer.data_buffer[i * 4 + 3] = 100;
    }
    midi_transfer_cb(&transfer);
    if (g_note_events_seen != 4) {
        fprintf(stderr, "FAIL: expected 4 MIDI events dispatched from one transfer, got %d\n", g_note_events_seen);
        failures++;
    }
    if (g_submit_calls != 1) {
        fprintf(stderr, "FAIL: completed transfer wasn't resubmitted exactly once (got %d) -- breaks the "
                        "\"exactly one transfer perpetually in flight\" invariant behind the measured "
                        "0-11ms chord-onset fix\n",
                g_submit_calls);
        failures++;
    }
    if (g_free_calls != 0) {
        fprintf(stderr, "FAIL: a successfully-resubmitted transfer was also freed (got %d frees)\n", g_free_calls);
        failures++;
    }

    // Case 2: resubmit fails with a stale-handle error (the real
    // hub-hotswap scenario) -- must free the transfer, not leak it.
    reset_globals();
    g_submit_result = ESP_ERR_INVALID_STATE;
    usb_transfer_t transfer2 = {.status = USB_TRANSFER_STATUS_COMPLETED, .actual_num_bytes = 0};
    midi_transfer_cb(&transfer2);
    if (g_submit_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly one resubmit attempt, got %d\n", g_submit_calls);
        failures++;
    }
    if (g_free_calls != 1) {
        fprintf(stderr, "FAIL: resubmit failure didn't free the transfer (got %d frees) -- this leaks a "
                        "transfer every time a device goes stale, the bug found during the hub hot-swap "
                        "investigation\n",
                g_free_calls);
        failures++;
    }

    // Case 3: device is gone -- must free without ever attempting a
    // resubmit on a transfer for a device that no longer exists.
    reset_globals();
    usb_transfer_t transfer3 = {.status = USB_TRANSFER_STATUS_NO_DEVICE, .actual_num_bytes = 0};
    midi_transfer_cb(&transfer3);
    if (g_submit_calls != 0) {
        fprintf(stderr, "FAIL: attempted to resubmit a transfer for a device that's gone (got %d attempts)\n",
                g_submit_calls);
        failures++;
    }
    if (g_free_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly one free for a gone device's transfer, got %d\n", g_free_calls);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: every completed transfer is resubmitted immediately and unconditionally, "
           "and a failed resubmit frees rather than leaks the transfer\n");
    return 0;
}
