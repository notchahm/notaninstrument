#include "usb_midi_host.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_midi_host";

#define AUDIO_SUBCLASS_MIDISTREAMING 0x03
#define HOST_LIB_TASK_PRIORITY 5
#define CLASS_TASK_PRIORITY 5
#define CLIENT_NUM_EVENT_MSG 5
#define DEV_MAX_COUNT 16

// ---- MIDI interface claim + perpetual bulk IN polling -------------------
//
// Adapted from firmware/spike-usb-host-native/main/midi_native.c (the
// timing-comparison spike that led to this file existing at all) with the
// diagnostic timing_diag_* calls swapped for real note/CC dispatch into
// this project's voice engine and display, via usb_midi_on_event().

static void midi_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        // USB-MIDI 1.0 Event Packet framing: 4 bytes per packet
        // (cable/CIN, MIDI status, data1, data2), possibly several packed
        // back-to-back in one transfer.
        for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4) {
            uint8_t *pkt = &transfer->data_buffer[i];
            uint8_t status = pkt[1];
            uint8_t data1 = pkt[2];
            uint8_t data2 = pkt[3];
            if (status != 0) {
                usb_midi_on_event(status, data1, data2);
            }
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "IN transfer status=%d", transfer->status);
    }

    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        usb_host_transfer_free(transfer);
        return;
    }

    // Keep exactly one transfer perpetually in flight -- resubmitted the
    // instant this one completes, so there's no host-side polling
    // interval of our own choosing (see docs/polyphony-latency-
    // investigation.md's TinyUSB-vs-native comparison for why that
    // matters).
    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        // Confirmed on real hardware (a hub-instability hot-swap): this
        // can fail with ESP_ERR_INVALID_STATE once the underlying device
        // handle has gone stale, at which point it will never succeed
        // again on this transfer -- free it rather than leak it. The
        // eventual USB_HOST_CLIENT_EVENT_DEV_GONE callback (handled in
        // handle_device's ACTION_CLOSE_DEV) cleans up the device itself;
        // a fresh USB_HOST_CLIENT_EVENT_NEW_DEV on reconnect starts a
        // brand new transfer via midi_try_claim(), so there's nothing
        // else to resume here.
        ESP_LOGW(TAG, "resubmit failed: %s -- dropping this transfer", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
    }
}

typedef enum {
    ACTION_OPEN_DEV = (1 << 0),
    ACTION_GET_DEV_DESC = (1 << 1),
    ACTION_GET_CONFIG_DESC = (1 << 2),
    ACTION_CLOSE_DEV = (1 << 3),
} action_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    action_t actions;
    // Set by midi_try_claim() on a successful usb_host_interface_claim().
    // Root-caused 2026-09-08: usb_host_device_close() unconditionally
    // returns ESP_ERR_INVALID_STATE ("client has not released all
    // interfaces") if a claimed interface is still open on the device --
    // confirmed directly in usb_host.h's doc comments for both
    // usb_host_device_close() and usb_host_interface_release(). This
    // project's ACTION_CLOSE_DEV handling used to call device_close()
    // directly with no matching interface_release() first, so it *always*
    // failed on any device that ever had a MIDIStreaming interface claimed
    // -- which, per the managed espressif__usb component's own hub.c, is
    // exactly what gates the root port's internal device-free/recycle
    // sequence, and therefore whether the port ever re-arms to detect a
    // fresh connection. That silent, unconditional close failure -- not
    // hub involvement specifically -- is what left the USB port
    // permanently unable to detect any new device after a disconnect,
    // confirmed reproducing identically with no hub at all involved.
    bool interface_claimed;
    uint8_t interface_number;
} usb_device_t;

static void midi_try_claim(usb_device_t *device, const usb_config_desc_t *config_desc) {
    for (uint8_t intf_num = 0; intf_num < config_desc->bNumInterfaces; intf_num++) {
        int offset = 0;
        const usb_intf_desc_t *intf_desc =
            usb_parse_interface_descriptor(config_desc, intf_num, 0, &offset);
        if (intf_desc == NULL) {
            continue;
        }
        if (intf_desc->bInterfaceClass != USB_CLASS_AUDIO ||
            intf_desc->bInterfaceSubClass != AUDIO_SUBCLASS_MIDISTREAMING) {
            continue;
        }

        ESP_LOGI(TAG, "Found MIDIStreaming interface %u (%u endpoints)",
                 intf_desc->bInterfaceNumber, intf_desc->bNumEndpoints);

        const usb_ep_desc_t *in_ep = NULL;
        for (int ep_idx = 0; ep_idx < intf_desc->bNumEndpoints; ep_idx++) {
            int ep_offset = offset;
            const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(
                intf_desc, ep_idx, config_desc->wTotalLength, &ep_offset);
            if (ep_desc == NULL) {
                continue;
            }
            if (USB_EP_DESC_GET_EP_DIR(ep_desc) == 1 && in_ep == NULL) {
                in_ep = ep_desc;
            }
        }

        if (in_ep == NULL) {
            ESP_LOGW(TAG, "MIDIStreaming interface has no IN endpoint");
            continue;
        }

        esp_err_t err = usb_host_interface_claim(device->client_hdl, device->dev_hdl,
                                                   intf_desc->bInterfaceNumber, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
            return;
        }
        device->interface_claimed = true;
        device->interface_number = intf_desc->bInterfaceNumber;

        uint16_t mps = USB_EP_DESC_GET_MPS(in_ep);
        usb_transfer_t *transfer;
        err = usb_host_transfer_alloc(mps, 0, &transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transfer_alloc failed: %s", esp_err_to_name(err));
            return;
        }
        transfer->device_handle = device->dev_hdl;
        transfer->bEndpointAddress = in_ep->bEndpointAddress;
        transfer->num_bytes = mps;
        transfer->callback = midi_transfer_cb;
        transfer->context = NULL;

        err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "initial transfer_submit failed: %s", esp_err_to_name(err));
            usb_host_transfer_free(transfer);
            return;
        }

        ESP_LOGI(TAG, "MIDI IN polling started on EP %02x (MPS=%u)",
                 in_ep->bEndpointAddress, mps);
        return; // only the first MIDIStreaming interface on this device
    }
    ESP_LOGI(TAG, "No MIDIStreaming interface on this device");
}

// ---- Device enumeration lifecycle ----------------------------------------
//
// Adapted from Espressif's own usb_host_lib example
// (examples/peripherals/usb/host/usb_host_lib, also the base for
// firmware/spike-usb-host-native/), trimmed to what this project actually
// needs: enumerate, fetch the config descriptor, hand it to
// midi_try_claim() above. No app-quit/GPIO handling -- this runs forever.
// (action_t/usb_device_t are defined above, next to midi_try_claim, since
// that function now needs the full usb_device_t definition too.)

static usb_device_t s_devices[DEV_MAX_COUNT];
static volatile bool s_unhandled_devices = false;
static SemaphoreHandle_t s_mux_lock;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    (void) arg;
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        // Defensive bounds check -- USB device addresses go up to 127 by
        // spec, well past DEV_MAX_COUNT, and repeated hot-plug cycling
        // (confirmed happening on real hardware with an unstable hub --
        // see External Hubs support note in usb_midi_host_start()) is
        // exactly the kind of scenario that could climb past whatever
        // address range "normal" single-session use stays within.
        // Silently dropping a device we have no slot for beats an
        // out-of-bounds write.
        if (event_msg->new_dev.address >= DEV_MAX_COUNT) {
            ESP_LOGW(TAG, "new device at address %d exceeds DEV_MAX_COUNT=%d -- ignoring",
                     event_msg->new_dev.address, DEV_MAX_COUNT);
            break;
        }
        s_devices[event_msg->new_dev.address].dev_addr = event_msg->new_dev.address;
        s_devices[event_msg->new_dev.address].dev_hdl = NULL;
        s_devices[event_msg->new_dev.address].interface_claimed = false;
        s_devices[event_msg->new_dev.address].actions |= ACTION_OPEN_DEV;
        s_unhandled_devices = true;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        for (int i = 0; i < DEV_MAX_COUNT; i++) {
            if (s_devices[i].dev_hdl == event_msg->dev_gone.dev_hdl) {
                s_devices[i].actions = ACTION_CLOSE_DEV;
                s_unhandled_devices = true;
            }
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported client event: %d", event_msg->event);
        break;
    }
    xSemaphoreGive(s_mux_lock);
}

// Every one of these USB device lifecycle calls used to be wrapped in
// ESP_ERROR_CHECK -- appropriate for a call that "should never fail"
// during stable operation, but wrong here: a USB device can legitimately
// disappear (unplugged, a hub resetting/losing power, a bus error)
// between any two of these steps, and any of the four calls below can
// then fail with a real, expected error rather than a programming bug.
// First confirmed on real hardware via a USB hub hot-swap, which made
// usb_host_device_close() return ESP_ERR_INVALID_STATE; ESP_ERROR_CHECK
// on that aborted the entire firmware -- killing piano and drum playback
// too, not just the USB connection. Logging and bailing out of *this
// device's* handling for this cycle, rather than aborting the whole
// system, is what a class driver actually needs to tolerate normal
// hot-plug/hot-unplug.
//
// That close-failure symptom's real cause, found later (2026-09-08) once
// fixing the abort surfaced a second, deeper bug: it was never actually
// about hub instability specifically -- device_close() unconditionally
// returns ESP_ERR_INVALID_STATE if a claimed interface hasn't been
// released first (usb_host.h's own doc comments for both
// usb_host_device_close() and usb_host_interface_release() say so
// directly), and this file claimed a MIDIStreaming interface in
// midi_try_claim() but never released it anywhere. Confirmed reproducing
// identically on a plain direct connection with no hub at all involved.
// Worse than a cosmetic log message: per the managed espressif__usb
// component's own hub.c, the root port's internal device-free/recycle
// sequence -- and therefore whether that port can ever detect a *new*
// connection -- is gated on the close actually succeeding. So every
// disconnect permanently wedged the port (traced with runtime
// esp_log_level_set("HUB"/"USBH", ESP_LOG_DEBUG) tracing the vendored
// driver's own root_port_handle_events()/dev_tree_node_dev_gone(), which
// showed "Root port reset"/"New device N" never appearing again after
// any disconnect) until the whole board was reset. See
// usb_device_t.interface_claimed above and its use in ACTION_CLOSE_DEV
// below for the actual fix: release before close, every time.
static void handle_device(usb_device_t *device) {
    uint8_t actions = device->actions;
    device->actions = 0;

    if (actions & ACTION_OPEN_DEV) {
        ESP_LOGI(TAG, "Opening device at address %d", device->dev_addr);
        esp_err_t err = usb_host_device_open(device->client_hdl, device->dev_addr, &device->dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "device_open failed for address %d: %s -- device likely already gone",
                     device->dev_addr, esp_err_to_name(err));
            return;
        }
        actions |= ACTION_GET_DEV_DESC;
    }
    if (actions & ACTION_GET_DEV_DESC) {
        const usb_device_desc_t *dev_desc;
        esp_err_t err = usb_host_get_device_descriptor(device->dev_hdl, &dev_desc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "get_device_descriptor failed for address %d: %s",
                     device->dev_addr, esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "Device VID:PID %04x:%04x", dev_desc->idVendor, dev_desc->idProduct);
        actions |= ACTION_GET_CONFIG_DESC;
    }
    if (actions & ACTION_GET_CONFIG_DESC) {
        const usb_config_desc_t *config_desc;
        esp_err_t err = usb_host_get_active_config_descriptor(device->dev_hdl, &config_desc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "get_active_config_descriptor failed for address %d: %s",
                     device->dev_addr, esp_err_to_name(err));
            return;
        }
        midi_try_claim(device, config_desc);
    }
    if (actions & ACTION_CLOSE_DEV) {
        // Must release any claimed interface BEFORE closing -- see
        // usb_device_t's interface_claimed comment. Attempted
        // unconditionally: even if release itself fails (device already
        // fully gone at a lower level), still attempt the close below
        // rather than leaving the device open on our side too.
        if (device->interface_claimed) {
            esp_err_t rel_err = usb_host_interface_release(device->client_hdl, device->dev_hdl,
                                                              device->interface_number);
            if (rel_err != ESP_OK) {
                ESP_LOGW(TAG, "interface_release failed for address %d: %s -- attempting close anyway",
                         device->dev_addr, esp_err_to_name(rel_err));
            }
            device->interface_claimed = false;
        }
        esp_err_t err = usb_host_device_close(device->client_hdl, device->dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "device_close failed for address %d: %s -- device already gone, "
                          "clearing our own bookkeeping anyway",
                     device->dev_addr, esp_err_to_name(err));
        }
        device->dev_hdl = NULL;
        device->dev_addr = 0;
    }
}

static void class_driver_task(void *arg) {
    (void) arg;
    s_mux_lock = xSemaphoreCreateMutex();

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    usb_host_client_handle_t client_hdl;
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_hdl));
    for (int i = 0; i < DEV_MAX_COUNT; i++) {
        s_devices[i].client_hdl = client_hdl;
    }

    while (1) {
        if (s_unhandled_devices) {
            xSemaphoreTake(s_mux_lock, portMAX_DELAY);
            for (int i = 0; i < DEV_MAX_COUNT; i++) {
                if (s_devices[i].actions) {
                    handle_device(&s_devices[i]);
                }
            }
            s_unhandled_devices = false;
            xSemaphoreGive(s_mux_lock);
        } else {
            usb_host_client_handle_events(client_hdl, portMAX_DELAY);
        }
    }
}

static void usb_host_lib_task(void *arg) {
    ESP_LOGI(TAG, "Installing USB Host Library");
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        // ESP32-P4 has two DWC2 controllers (Port0=FS, Port1=HS); 0 means
        // "use the default peripheral", which resolves to the HS one on
        // High-Speed-capable targets -- this board's native USB-A ports
        // (confirmed in firmware/spike-usb-host-native/README.md).
        .peripheral_map = 0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t) arg);

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

void usb_midi_host_start(void) {
    TaskHandle_t host_lib_task_hdl;
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(),
                             HOST_LIB_TASK_PRIORITY, &host_lib_task_hdl, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // wait for usb_host_install() to finish
    xTaskCreatePinnedToCore(class_driver_task, "usb_midi_class", 5 * 1024, NULL,
                             CLASS_TASK_PRIORITY, NULL, 0);
}
