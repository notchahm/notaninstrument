#pragma once

// TinyUSB configuration for the USB-MIDI host spike (docs/bring-up-plan.md
// step 2), built directly on ESP-IDF + the `espressif/tinyusb` component --
// no Arduino involved. Mirrors
// firmware/spike-usb-midi-arduino/tusb_config.h so both spikes exercise the
// same host/MIDI configuration for a fair pass/fail comparison.

#define CFG_TUSB_MCU    OPT_MCU_ESP32P4
#define CFG_TUSB_OS     OPT_OS_FREERTOS

// Root hub port 0 in HOST mode -- this project drives USB-MIDI controllers
// plugged into the board's USB-A port(s) (docs/hardware-bom.md). TBD:
// confirm rhport 0 is actually the port wired to those jacks once the
// board's USB routing is checked against the CH334F hub datasheet.
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_HOST)

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
