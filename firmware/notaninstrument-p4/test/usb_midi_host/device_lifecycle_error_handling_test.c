// Regression test for TWO real bugs in usb_midi_host.c's device
// lifecycle handling, found in the same investigation (2026-09-08):
//
// 1. Every one of handle_device()'s four USB device lifecycle calls
//    (open, get_device_descriptor, get_active_config_descriptor, close)
//    used to be wrapped in ESP_ERROR_CHECK -- appropriate for a call that
//    "should never fail" during stable operation, but wrong here, since a
//    USB device can legitimately disappear (unplugged, a hub losing
//    power/resetting, a bus error) between any two of these steps.
//    Confirmed on real hardware: hot-swapping a MIDI controller onto a
//    USB hub mid-session made usb_host_device_close() return
//    ESP_ERR_INVALID_STATE, and ESP_ERROR_CHECK on that aborted the
//    entire firmware -- killing piano and drum playback too, not just
//    the USB connection.
//
// 2. Fixing (1) surfaced a second, deeper bug behind that same
//    device_close() failure: usb_host_device_close() unconditionally
//    returns ESP_ERR_INVALID_STATE if a client hasn't released every
//    interface it claimed on that device first (usb_host.h's own doc
//    comments for both usb_host_device_close() and
//    usb_host_interface_release() say so directly) -- and midi_try_claim()
//    claims a MIDIStreaming interface but this file never released it
//    anywhere. Confirmed reproducing identically with no hub at all
//    involved, on a plain direct connect/disconnect. Worse than a
//    cosmetic log message: per the managed espressif__usb component's own
//    hub.c, the root USB port's internal device-free/recycle sequence --
//    and therefore whether that port can ever detect a *new* connection
//    -- is gated on the close actually succeeding, so every disconnect
//    permanently wedged the port until the whole board was reset.
//
// This mirrors handle_device()'s exact structure (not an include of
// usb_midi_host.c, which pulls in FreeRTOS/ESP-IDF's usb_host.h --
// neither portable outside ESP-IDF nor practical to fully stub here;
// keep this in sync with the real function if its logic changes) with
// fake usb_host_* calls whose return values the test controls, and
// checks both fixes directly: any of the five calls failing must make
// handle_device() log and return (or, for close specifically, still
// clear its own bookkeeping) rather than abort the process -- there's no
// ESP_ERROR_CHECK-equivalent call anywhere in this file, so a real
// abort() in the mirrored version would crash this test binary itself,
// exactly like it crashed the real firmware -- AND that ACTION_CLOSE_DEV
// always releases a claimed interface before attempting to close, even
// if that release itself fails.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105

typedef void *usb_host_client_handle_t;
typedef void *usb_device_handle_t;
typedef struct { int idVendor, idProduct; } usb_device_desc_t;
typedef struct { int dummy; } usb_config_desc_t;

// Controllable fake return values + call counters, set by each test case.
static esp_err_t g_open_result = ESP_OK;
static esp_err_t g_get_dev_desc_result = ESP_OK;
static esp_err_t g_get_config_desc_result = ESP_OK;
static esp_err_t g_close_result = ESP_OK;
static esp_err_t g_release_result = ESP_OK;
static int g_midi_try_claim_calls = 0;
static int g_interface_release_calls = 0;
static int g_device_close_calls = 0;
// Whether midi_try_claim's mirror should simulate finding (and claiming)
// a MIDIStreaming interface -- some devices legitimately have none.
static bool g_midi_try_claim_finds_interface = true;

static const char *esp_err_to_name(esp_err_t err) {
    return err == ESP_OK ? "ESP_OK" : "ESP_ERR_SOMETHING";
}

static esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t addr, usb_device_handle_t *out) {
    (void) client; (void) addr;
    *out = (usb_device_handle_t) 1; // a non-NULL "handle"
    return g_open_result;
}
static esp_err_t usb_host_get_device_descriptor(usb_device_handle_t dev, const usb_device_desc_t **out) {
    (void) dev;
    static usb_device_desc_t desc = {0x1234, 0x5678};
    *out = &desc;
    return g_get_dev_desc_result;
}
static esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t dev, const usb_config_desc_t **out) {
    (void) dev;
    static usb_config_desc_t desc;
    *out = &desc;
    return g_get_config_desc_result;
}
static esp_err_t usb_host_interface_release(usb_host_client_handle_t client, usb_device_handle_t dev, uint8_t intf) {
    (void) client; (void) dev; (void) intf;
    g_interface_release_calls++;
    return g_release_result;
}
static esp_err_t usb_host_device_close(usb_host_client_handle_t client, usb_device_handle_t dev) {
    (void) client; (void) dev;
    g_device_close_calls++;
    return g_close_result;
}

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    unsigned actions;
    bool interface_claimed;
    uint8_t interface_number;
} usb_device_t;

// Mirrors midi_try_claim()'s one relevant side effect for this test: it
// sets interface_claimed/interface_number on a successful claim, and
// leaves interface_claimed false if no MIDIStreaming interface was found
// on this device (a real, normal case -- not every USB device is a MIDI
// controller).
static void midi_try_claim(usb_device_t *device, const usb_config_desc_t *cfg) {
    (void) cfg;
    g_midi_try_claim_calls++;
    if (g_midi_try_claim_finds_interface) {
        device->interface_claimed = true;
        device->interface_number = 2;
    }
}

enum {
    ACTION_OPEN_DEV = (1 << 0),
    ACTION_GET_DEV_DESC = (1 << 1),
    ACTION_GET_CONFIG_DESC = (1 << 2),
    ACTION_CLOSE_DEV = (1 << 3),
};

// Mirrors the real handle_device() in usb_midi_host.c exactly, post-fix:
// every call checked, none wrapped in an abort-on-failure macro, and a
// claimed interface always released before the device itself is closed.
static void handle_device(usb_device_t *device) {
    unsigned actions = device->actions;
    device->actions = 0;

    if (actions & ACTION_OPEN_DEV) {
        esp_err_t err = usb_host_device_open(device->client_hdl, device->dev_addr, &device->dev_hdl);
        if (err != ESP_OK) {
            return;
        }
        actions |= ACTION_GET_DEV_DESC;
    }
    if (actions & ACTION_GET_DEV_DESC) {
        const usb_device_desc_t *dev_desc;
        esp_err_t err = usb_host_get_device_descriptor(device->dev_hdl, &dev_desc);
        if (err != ESP_OK) {
            return;
        }
        actions |= ACTION_GET_CONFIG_DESC;
    }
    if (actions & ACTION_GET_CONFIG_DESC) {
        const usb_config_desc_t *config_desc;
        esp_err_t err = usb_host_get_active_config_descriptor(device->dev_hdl, &config_desc);
        if (err != ESP_OK) {
            return;
        }
        midi_try_claim(device, config_desc);
    }
    if (actions & ACTION_CLOSE_DEV) {
        if (device->interface_claimed) {
            esp_err_t rel_err = usb_host_interface_release(device->client_hdl, device->dev_hdl,
                                                              device->interface_number);
            if (rel_err != ESP_OK) {
                (void) esp_err_to_name(rel_err); // real code logs here
            }
            device->interface_claimed = false;
        }
        esp_err_t err = usb_host_device_close(device->client_hdl, device->dev_hdl);
        if (err != ESP_OK) {
            (void) esp_err_to_name(err); // real code logs here; nothing to assert on that
        }
        device->dev_hdl = NULL;
        device->dev_addr = 0;
    }
}

static void reset_globals(void) {
    g_open_result = ESP_OK;
    g_get_dev_desc_result = ESP_OK;
    g_get_config_desc_result = ESP_OK;
    g_close_result = ESP_OK;
    g_release_result = ESP_OK;
    g_midi_try_claim_calls = 0;
    g_interface_release_calls = 0;
    g_device_close_calls = 0;
    g_midi_try_claim_finds_interface = true;
}

int main(void) {
    int failures = 0;

    // Case 1: everything succeeds -- the ordinary path, sanity check.
    reset_globals();
    usb_device_t dev = {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = NULL,
                         .actions = ACTION_OPEN_DEV};
    handle_device(&dev);
    if (g_midi_try_claim_calls != 1) {
        fprintf(stderr, "FAIL: normal open->desc->config->claim chain didn't reach midi_try_claim\n");
        failures++;
    }

    // Case 2: device_open fails (device vanished before we could even
    // open it) -- must return without touching later stages.
    reset_globals();
    g_open_result = ESP_ERR_NOT_FOUND;
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = NULL, .actions = ACTION_OPEN_DEV};
    handle_device(&dev); // must not abort
    if (g_midi_try_claim_calls != 0) {
        fprintf(stderr, "FAIL: device_open failure still reached midi_try_claim\n");
        failures++;
    }

    // Case 3: get_active_config_descriptor fails partway through --
    // must not abort and must not call midi_try_claim.
    reset_globals();
    g_get_config_desc_result = ESP_ERR_INVALID_STATE;
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = NULL, .actions = ACTION_OPEN_DEV};
    handle_device(&dev); // must not abort
    if (g_midi_try_claim_calls != 0) {
        fprintf(stderr, "FAIL: get_active_config_descriptor failure still reached midi_try_claim\n");
        failures++;
    }

    // Case 4: closing a device with a claimed MIDIStreaming interface --
    // the actual bug this test exists to catch. Must release the
    // interface BEFORE attempting to close the device, exactly once each.
    reset_globals();
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = (void *) 1,
                           .interface_claimed = true, .interface_number = 2,
                           .actions = ACTION_CLOSE_DEV};
    handle_device(&dev);
    if (g_interface_release_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly 1 interface_release call before close, got %d -- "
                        "this is the real bug: usb_host_device_close() unconditionally fails with "
                        "ESP_ERR_INVALID_STATE if a claimed interface isn't released first, which "
                        "silently wedges the USB root port until the whole board is reset\n",
                g_interface_release_calls);
        failures++;
    }
    if (g_device_close_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly 1 device_close call, got %d\n", g_device_close_calls);
        failures++;
    }
    if (dev.interface_claimed) {
        fprintf(stderr, "FAIL: interface_claimed still true after close handling\n");
        failures++;
    }

    // Case 5: interface_release itself fails -- must still attempt
    // device_close anyway (best-effort cleanup), not abort or skip it.
    reset_globals();
    g_release_result = ESP_ERR_INVALID_STATE;
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = (void *) 1,
                           .interface_claimed = true, .interface_number = 2,
                           .actions = ACTION_CLOSE_DEV};
    handle_device(&dev); // must not abort
    if (g_device_close_calls != 1) {
        fprintf(stderr, "FAIL: interface_release failure prevented the device_close attempt "
                        "(got %d close calls, expected 1)\n", g_device_close_calls);
        failures++;
    }

    // Case 6: a device with no MIDIStreaming interface at all (a real,
    // normal case) -- must NOT call interface_release (nothing was ever
    // claimed), just close directly.
    reset_globals();
    g_midi_try_claim_finds_interface = false;
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = NULL,
                           .actions = ACTION_OPEN_DEV};
    handle_device(&dev);
    dev.actions = ACTION_CLOSE_DEV;
    handle_device(&dev);
    if (g_interface_release_calls != 0) {
        fprintf(stderr, "FAIL: interface_release called for a device that never claimed an interface "
                        "(got %d calls, expected 0)\n", g_interface_release_calls);
        failures++;
    }
    if (g_device_close_calls != 1) {
        fprintf(stderr, "FAIL: expected exactly 1 device_close call, got %d\n", g_device_close_calls);
        failures++;
    }

    // Case 7: device_close fails (a stale handle, e.g. hub instability or
    // any other disconnect race) -- must not abort, and must still clear
    // the slot's own bookkeeping so it can be reused for a future device
    // at this address. This is the original ESP_ERROR_CHECK crash bug.
    reset_globals();
    g_close_result = ESP_ERR_INVALID_STATE;
    dev = (usb_device_t) {.client_hdl = (void *) 1, .dev_addr = 5, .dev_hdl = (void *) 1,
                           .actions = ACTION_CLOSE_DEV};
    handle_device(&dev); // must not abort -- this is the exact call that used to crash the firmware
    if (dev.dev_hdl != NULL || dev.dev_addr != 0) {
        fprintf(stderr, "FAIL: device_close failure left stale bookkeeping (dev_hdl=%p dev_addr=%d)\n",
                dev.dev_hdl, dev.dev_addr);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    // Reaching here at all (the process didn't abort) is itself part of
    // what this test verifies.
    printf("PASS: USB device lifecycle failure modes handled without aborting, and interface "
           "release always happens before device close\n");
    return 0;
}
