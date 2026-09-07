#pragma once

// TinyUSB configuration for the USB-MIDI host spike (docs/bring-up-plan.md
// step 2), built directly on ESP-IDF + the `espressif/tinyusb` component --
// no Arduino involved. Mirrors
// firmware/spike-usb-midi-arduino/tusb_config.h so both spikes exercise the
// same host/MIDI configuration for a fair pass/fail comparison.

#define CFG_TUSB_MCU    OPT_MCU_ESP32P4
#define CFG_TUSB_OS     OPT_OS_FREERTOS

// Internal TinyUSB host-stack logging (port reset, attach/detach interrupts,
// enumeration steps) is completely compiled out by default -- CFG_TUSB_DEBUG
// defaults to 0, but CFG_TUH_LOG_LEVEL defaults to 2, and the former must be
// >= the latter for any of it to print. Cranked up while diagnosing why no
// device enumerates despite a clean, non-crashing boot -- our own
// application-level ESP_LOGI calls can't see what's happening below tuh_task().
#define CFG_TUSB_DEBUG  3

// Root hub port 1 in HOST mode. ESP32-P4 exposes two DWC2 controllers --
// per components/tinyusb_host/src/portable/synopsys/dwc2/dwc2_esp32.h's
// own comment, "Port0 to OTG_FS, and Port1 to OTG_HS". This board is
// documented (docs/hardware-bom.md) as having *native HS* USB-OTG feeding
// the USB-A host ports via the CH334F hub, so that's Port1, not Port0.
//
// CONFIRMED 2026-09-06 on real hardware: RHPORT0 produced a healthy boot
// log (tusb_init() "succeeded") but never detected a USB-MIDI controller
// under any condition tried -- already connected at boot, unplug/replug,
// two different physical ports, host/dev jumper confirmed set to HOST.
// RHPORT0 = OTG_FS is very likely simply not wired to any of this board's
// physical connectors at all. Switching to RHPORT1 is the fix.
#define CFG_TUSB_RHPORT1_MODE   (OPT_MODE_HOST)

#define CFG_TUH_ENABLED             1
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#define CFG_TUH_HUB             1   // both USB-A ports go through a CH334F hub
#define CFG_TUH_MIDI            4  // concurrent MIDI devices to support
#define CFG_TUH_DEVICE_MAX      (CFG_TUH_HUB ? 4 : 1)

// Keep the class surface minimal for this spike -- only MIDI matters here.
#define CFG_TUH_HID             0
#define CFG_TUH_CDC             0
#define CFG_TUH_MSC             0
#define CFG_TUH_VENDOR          0
