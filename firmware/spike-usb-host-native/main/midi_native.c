#include "midi_native.h"

#include "esp_log.h"
#include "timing_diag.h"
#include "usb/usb_helpers.h"

static const char *TAG = "midi_native";

// USB Audio class-code constants (not in usb_types_ch9.h -- only the base
// USB_CLASS_AUDIO code lives there).
#define AUDIO_SUBCLASS_MIDISTREAMING 0x03

static void midi_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        timing_diag_raw_callback();
        // USB-MIDI 1.0 Event Packet framing: 4 bytes per packet
        // (cable/CIN, MIDI status, data1, data2), possibly several packed
        // back-to-back in one transfer.
        for (int i = 0; i + 4 <= transfer->actual_num_bytes; i += 4) {
            uint8_t *pkt = &transfer->data_buffer[i];
            uint8_t status = pkt[1] & 0xF0;
            uint8_t note = pkt[2];
            uint8_t velocity = pkt[3];
            if (status == 0x90 && velocity > 0) {
                timing_diag_record(note, true);
            } else if (status == 0x80 || (status == 0x90 && velocity == 0)) {
                timing_diag_record(note, false);
            }
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "IN transfer status=%d", transfer->status);
    }

    if (transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        // Keep exactly one transfer perpetually in flight -- resubmit the
        // instant this one completes, so there's no host-side polling
        // interval of our own choosing to confound the latency
        // measurement against the TinyUSB path.
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "resubmit failed: %s", esp_err_to_name(err));
        }
    }
}

void midi_native_try_claim(usb_host_client_handle_t client_hdl,
                            usb_device_handle_t dev_hdl,
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
            const char *xfer_name[] = {"CTRL", "ISOC", "BULK", "INTR"};
            ESP_LOGI(TAG, "  EP %02x: %s bInterval=%u wMaxPacketSize=%u",
                     ep_desc->bEndpointAddress,
                     xfer_name[USB_EP_DESC_GET_XFERTYPE(ep_desc)],
                     ep_desc->bInterval, USB_EP_DESC_GET_MPS(ep_desc));
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
            return;
        }

        ESP_LOGI(TAG, "MIDI IN polling started on EP %02x (MPS=%u)",
                 in_ep->bEndpointAddress, mps);
        timing_diag_start_task();
        return; // only handle the first MIDIStreaming interface found
    }
    ESP_LOGI(TAG, "No MIDIStreaming interface on this device");
}
