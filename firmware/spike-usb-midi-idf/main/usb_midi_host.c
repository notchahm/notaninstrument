#include "usb_midi_host.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// TEMPORARY diagnostic (2026-09-07 chording-bug investigation): dumps
// every raw 4-byte USB-MIDI Event Packet this code sees, cable/CIN byte
// included, before any note/CC interpretation -- voice_engine.c's own
// diagnostic showed Note Off events for notes whose Note On never
// reached voice_engine_note_on at all, which could mean either the
// device isn't sending them, or something in this file's own packet
// decode is dropping/mis-parsing them. This settles which, by showing
// the literal bytes. Same decoupled-logging discipline as the other
// diagnostics in this investigation: a cheap struct write in the hot
// path (the transfer completion callback), all ESP_LOGI dumping deferred
// to its own low-priority task.
#define RAW_DIAG_RING_SIZE 128
typedef struct {
    int64_t us;
    uint8_t cin_byte; // pkt[0]: cable number (high nibble) | Code Index Number (low nibble)
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} raw_diag_entry_t;
static raw_diag_entry_t s_raw_diag_ring[RAW_DIAG_RING_SIZE];
static volatile uint32_t s_raw_diag_write_idx = 0;

static void raw_diag_record(uint8_t cin_byte, uint8_t status, uint8_t data1, uint8_t data2) {
    raw_diag_entry_t *e = &s_raw_diag_ring[s_raw_diag_write_idx % RAW_DIAG_RING_SIZE];
    e->us = esp_timer_get_time();
    e->cin_byte = cin_byte;
    e->status = status;
    e->data1 = data1;
    e->data2 = data2;
    s_raw_diag_write_idx++;
}

static void raw_diag_task(void *arg) {
    (void) arg;
    uint32_t read_idx = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(300));
        uint32_t write_snapshot = s_raw_diag_write_idx;
        if (write_snapshot - read_idx > RAW_DIAG_RING_SIZE) {
            read_idx = write_snapshot - RAW_DIAG_RING_SIZE;
        }
        while (read_idx != write_snapshot) {
            const raw_diag_entry_t *e = &s_raw_diag_ring[read_idx % RAW_DIAG_RING_SIZE];
            ESP_LOGI(TAG, "RAW cin=%02x status=%02x d1=%3u d2=%3u",
                     e->cin_byte, e->status, e->data1, e->data2);
            read_idx++;
        }
    }
}

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
            raw_diag_record(pkt[0], status, data1, data2);
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
        ESP_LOGW(TAG, "resubmit failed: %s", esp_err_to_name(err));
    }
}

static void midi_try_claim(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl,
                            const usb_config_desc_t *config_desc) {
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

        esp_err_t err = usb_host_interface_claim(client_hdl, dev_hdl,
                                                   intf_desc->bInterfaceNumber, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
            return;
        }

        uint16_t mps = USB_EP_DESC_GET_MPS(in_ep);
        usb_transfer_t *transfer;
        err = usb_host_transfer_alloc(mps, 0, &transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transfer_alloc failed: %s", esp_err_to_name(err));
            return;
        }
        transfer->device_handle = dev_hdl;
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
} usb_device_t;

static usb_device_t s_devices[DEV_MAX_COUNT];
static volatile bool s_unhandled_devices = false;
static SemaphoreHandle_t s_mux_lock;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    (void) arg;
    xSemaphoreTake(s_mux_lock, portMAX_DELAY);
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_devices[event_msg->new_dev.address].dev_addr = event_msg->new_dev.address;
        s_devices[event_msg->new_dev.address].dev_hdl = NULL;
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

static void handle_device(usb_device_t *device) {
    uint8_t actions = device->actions;
    device->actions = 0;

    if (actions & ACTION_OPEN_DEV) {
        ESP_LOGI(TAG, "Opening device at address %d", device->dev_addr);
        ESP_ERROR_CHECK(usb_host_device_open(device->client_hdl, device->dev_addr, &device->dev_hdl));
        actions |= ACTION_GET_DEV_DESC;
    }
    if (actions & ACTION_GET_DEV_DESC) {
        const usb_device_desc_t *dev_desc;
        ESP_ERROR_CHECK(usb_host_get_device_descriptor(device->dev_hdl, &dev_desc));
        ESP_LOGI(TAG, "Device VID:PID %04x:%04x", dev_desc->idVendor, dev_desc->idProduct);
        actions |= ACTION_GET_CONFIG_DESC;
    }
    if (actions & ACTION_GET_CONFIG_DESC) {
        const usb_config_desc_t *config_desc;
        ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(device->dev_hdl, &config_desc));
        midi_try_claim(device->client_hdl, device->dev_hdl, config_desc);
    }
    if (actions & ACTION_CLOSE_DEV) {
        ESP_ERROR_CHECK(usb_host_device_close(device->client_hdl, device->dev_hdl));
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
    xTaskCreatePinnedToCore(raw_diag_task, "usb_midi_raw_diag", 4096, NULL, 1, NULL, 1);

    TaskHandle_t host_lib_task_hdl;
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(),
                             HOST_LIB_TASK_PRIORITY, &host_lib_task_hdl, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // wait for usb_host_install() to finish
    xTaskCreatePinnedToCore(class_driver_task, "usb_midi_class", 5 * 1024, NULL,
                             CLASS_TASK_PRIORITY, NULL, 0);
}
