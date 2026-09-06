# Vendored, host-mode-enabled TinyUSB component for the USB-MIDI host spike
# (docs/bring-up-plan.md step 2).
#
# Why this exists instead of depending on the `espressif/tinyusb` IDF
# Component Registry package directly: that published component's own
# CMakeLists.txt only lists DEVICE-mode sources (cdc_device.c, midi_device.c,
# dcd_dwc2.c, usbd.c, ...) -- no usbh.c, no hcd_dwc2.c, no midi_host.c. It is
# device-mode only, regardless of tusb_config.h settings. The actual
# host-side source (host controller driver for DWC2, host stack, MIDI host
# class driver) IS present in the package's `src/` tree -- it's the same
# upstream https://github.com/espressif/tinyusb.git (the Espressif fork with
# ESP32 host-controller ports added, confirmed pinned here at commit
# ab4a1817907ed865b10a712b5e03e5e0a9902df5, same as
# managed_components/espressif__tinyusb/idf_component.yml `repository_info`
# when this was vendored) -- it's just not wired into that component's
# build. This CMakeLists.txt is a from-scratch component definition over
# that same source, building the HOST side instead.
#
# `src/` here is trimmed to only what an ESP32(-P4) MIDI-host-only build
# needs (2026-09-06: 3.8MB/181 files -> 720KB/30 files, verified by a clean
# rebuild producing an identical binary, 0x34e10 bytes, before and after).
# Removed: every portable/* backend except synopsys/dwc2 (other MCU
# families -- STM32, NXP, Renesas, WCH, etc. -- entirely unused here); the
# other-vendor DWC2 per-chip headers and dcd_dwc2.c (device controller,
# unused -- this is host-only) within synopsys/dwc2 itself, keeping only
# dwc2_esp32.h, dwc2_type.h, dwc2_common.*, hcd_dwc2.c; every class/*
# except midi (class/audio/audio.h is kept as a midi_host.h dependency,
# audio_device.c/h dropped); device/ and typec/ entirely (device-mode core
# and USB-C PD, both unrelated to a MIDI host build); every osal/*.h except
# the generic header and the FreeRTOS backend actually used; and
# tinyusb.mk (this project builds via CMake, not tinyusb's own Makefile).
#
# To refresh this vendored copy against a newer TinyUSB version: add
# `espressif/tinyusb` back to main/idf_component.yml temporarily, run
# `idf.py reconfigure` to fetch it into managed_components/, copy its `src/`
# over this component's `src/` (this reintroduces everything trimmed above
# -- redo the trim afterward, and verify with a clean `idf.py fullclean &&
# idf.py build` producing a working binary before trusting it), then remove
# the idf_component.yml dependency again (having both this component and
# the registry one active at once will produce duplicate-symbol link
# errors -- they share files like tusb.c, tusb_fifo.c, dwc2_common.c).

idf_build_get_property(target IDF_TARGET)

set(srcs
    "src/tusb.c"
    "src/common/tusb_fifo.c"
    "src/host/usbh.c"
    "src/host/hub.c"
    "src/class/midi/midi_host.c"
    "src/portable/synopsys/dwc2/hcd_dwc2.c"
    "src/portable/synopsys/dwc2/dwc2_common.c"
    )

set(requirements_private "")

if(${target} STREQUAL "esp32p4")
    # hcd_dwc2.c / dwc2_common.c use esp_cache.h for cache-coherent DMA
    # buffers on P4, same as the device-mode dcd_dwc2.c build does.
    list(APPEND requirements_private esp_mm)
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "src"
                       PRIV_INCLUDE_DIRS "src" "src/host"
                       PRIV_REQUIRES ${requirements_private}
                       # "main" provides tusb_config.h, which tusb_option.h
                       # (included from tusb.c and friends) expects to find.
                       REQUIRES freertos main
                       )

# Matches the upstream device-mode component's own workaround: when no
# builtin class driver is enabled, a uint8_t compared against
# BUILTIN_DRIVER_COUNT is always false/true and warns under -Werror.
set_source_files_properties("src/host/usbh.c" PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
