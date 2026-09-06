# spike-usb-midi-arduino

Bring-up step 2 (`docs/bring-up-plan.md`): USB-MIDI host spike test on the
**Arduino-ESP32 + Adafruit TinyUSB** path. See `../spike-usb-midi-idf/` for
the same test built directly on ESP-IDF + TinyUSB, no Arduino involved
(CLAUDE.md architecture decision #4) -- both spikes implement identical
`tuh_midi_*` callbacks so their results are directly comparable.

This is a standalone diagnostic, not the full project. It answers one
question: does `tuh_midi_mount_cb` / `tuh_midi_rx_cb` fire when a
class-compliant USB-MIDI controller is plugged into the board's USB-A host
port?

## CONFIRMED: this does not compile against the stock library right now

Verified 2026-09-06 against the installed toolchain (arduino-esp32 core
3.3.11, Adafruit TinyUSB Library 3.7.7) -- this is not speculation, it's
read directly from the installed library source:

- `Adafruit_TinyUSB_Library/src/arduino/ports/esp32/tusb_config_esp32.h`
  is force-included for every ESP32 target regardless of what a sketch
  defines (`src/tusb_config.h` explicitly no-ops on `ARDUINO_ARCH_ESP32`
  "since it is force include[d]... in tusb_option.h") -- so this spike's
  sketch-local `tusb_config.h` is **silently ignored** on ESP32.
- That forced config sets `CFG_TUH_MAX3421 1` unconditionally (line 146,
  no `#if CONFIG_IDF_TARGET_ESP32P4` guard) and never sets
  `CFG_TUSB_RHPORT0_MODE`. `CFG_TUH_MAX3421` is TinyUSB's driver for an
  **external MAX3421E SPI host-controller chip** -- not this board's
  native DWC2 USB-OTG hardware. There is no MAX3421E in this project's BOM
  (`docs/hardware-bom.md`); the board's native USB-A host ports go through
  the CH334F hub, not an SPI-attached MAX3421E.
- The same forced config never defines `CFG_TUH_MIDI`, so it falls back to
  `tusb_option.h`'s default of `0` -- the MIDI host class driver isn't even
  compiled in. Building errors with `'tuh_midi_packet_read' was not
  declared in this scope`.
- I tried forcing `-DCFG_TUH_MIDI=4` via
  `arduino-cli --build-property compiler.cpp.extra_flags=...` as a
  workaround. That broke the core's own `USBCDC.cpp` (the property
  override clobbers other flags the build needs) -- not a safe path, and
  abandoned.

**Net effect: as currently packaged, Arduino-ESP32 + Adafruit TinyUSB
cannot drive this board's native USB-A host ports for USB-MIDI at all.**
This is itself a real result for CLAUDE.md architecture decision #4's
spike test -- effectively a "fail" for the Arduino path, discovered via
static analysis of the installed toolchain rather than by plugging in
actual hardware. See `../spike-usb-midi-idf/`, which uses
`espressif/tinyusb` directly and does have confirmed native ESP32-P4 DWC2
host-controller support (landed ~tinyusb v0.15) -- that path doesn't
inherit this Arduino-specific restriction.

The `.ino`/`tusb_config.h` in this directory are left as-is (using the
current, correct `tuh_midi_*` API) for reference/documentation and in case
a future arduino-esp32 or Adafruit TinyUSB release adds native ESP32
host-mode MIDI support -- but do not expect them to build against the
versions above without patching the library itself.

## Why "just fork Adafruit_TinyUSB_Library and patch the config" doesn't work

Tested directly (2026-09-06): every ESP32 Arduino build actually involves
**two independent copies of TinyUSB**, not one:

1. **Espressif's own prebuilt `libarduino_tinyusb.a`**
   (`~/.arduino15/packages/esp32/tools/esp32p4-libs/<ver>/lib/`, built by
   their `esp32-arduino-lib-builder` pipeline and linked into every sketch
   via `-larduino_tinyusb`). Its baked-in config
   (`.../include/arduino_tinyusb/include/tusb_config.h`) already has
   `CFG_TUH_MIDI 1` and a full host stack (hub, HID, MIDI) enabled --
   but also hardcodes `CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE`. The board's
   one native USB-OTG controller is permanently wired for **device** role
   in this prebuilt binary; host traffic only reaches the stack through an
   external MAX3421E SPI chip, treated as a separate virtual controller.
2. **Adafruit's own vendored TinyUSB source** bundled inside
   `Adafruit_TinyUSB_Library`, with its own separate config
   (`tusb_config_esp32.h`) that a sketch's `.ino` actually compiles
   against.

I tried patching just Adafruit's config header to add `CFG_TUH_MIDI 4`
(the minimal, "just fork the Arduino library" fix) and rebuilding. It did
NOT cleanly work: Arduino's build system adds every used library's include
path globally, across the *entire* build -- so the edit made Adafruit's
TinyUSB header tree leak into compilation of the core's own
`USBCDC.cpp` (an unrelated device-CDC file), which expects the *other*
copy's types/macros, and broke that instead
(`'cdc_line_coding_t' has not been declared`, etc.). The two copies aren't
cleanly separable by editing one header.

Even if that conflict were resolved, the deeper blocker remains:
**`CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE` is baked into the already-compiled
`libarduino_tinyusb.a` binary.** Actually enabling native OTG host mode on
P4 means rebuilding that static library from source with a different
tinyusb host config -- i.e. forking/rebuilding
[espressif/esp32-arduino-lib-builder](https://github.com/espressif/esp32-arduino-lib-builder)
(their Docker-based ESP-IDF build pipeline that produces `esp32p4-libs`),
not just Adafruit's thin Arduino wrapper. That's a real, doable option, but
a much bigger lift (a full ESP-IDF cross-build, likely hours, then
redistributing the result as a custom board package) than "fork one
GitHub library" -- and it would produce, at best, the same native P4 DWC2
host capability that `../spike-usb-midi-idf/` already gets directly and
far more cheaply by building against ESP-IDF's `espressif/tinyusb`
component fresh from source, with no prebuilt-static-library boundary in
the way. Recommendation: don't fork/rebuild the Arduino toolchain for
this -- put the effort into the ESP-IDF path instead.

## Prerequisites

- `arduino-cli` on `PATH`.
- `make setup` -- registers the ESP32 board index, installs the `esp32:esp32`
  core, and installs the "Adafruit TinyUSB Library". (If you already ran
  `make setup` in `../notaninstrument-p4/`, only the library install here is
  new.)

## Build and flash

```
make list-ports
make build
make flash PORT=/dev/ttyUSB0     # or PORT=COM3 on Windows
make monitor PORT=/dev/ttyUSB0
```

Or `make all PORT=/dev/ttyUSB0` for build + flash + monitor together.

## What counts as pass/fail

- **Pass:** plugging in a USB-MIDI controller logs `USB device mounted`
  followed by `MIDI device mounted`, and pressing keys on the controller
  logs `MIDI packet: ...` lines. Per CLAUDE.md decision #4, this means
  continue building the real project on Arduino.
- **Fail:** nothing logs on connect, `tuh_mount_cb` fires but
  `tuh_midi_mount_cb` never does, or a crash/hang on connect. Per decision
  #4, this means pivot to raw ESP-IDF (see `../spike-usb-midi-idf/`) for the
  real project.

## Known TBD

- **FQBN / USB mode**: confirmed against arduino-esp32 core 3.3.11
  (`arduino-cli board details -b esp32:esp32:esp32p4`) -- the board's
  defaults already do the right thing (USB Mode = USB-OTG/TinyUSB, Upload
  Mode = UART0/Hardware CDC), so no menu options need to be added to `FQBN`.
- `tusb_config.h` (sketch-local, overrides the library default) assumes
  root hub port 0 is wired to the board's USB-A host jacks via the onboard
  CH334F hub -- unconfirmed.
- If this fails while the ESP-IDF spike passes, that points at the Arduino
  wrapper specifically (board menu config, or Adafruit TinyUSB's vendored
  TinyUSB version lagging the fix) rather than P4 host mode being broken in
  general -- worth re-checking before fully committing to the ESP-IDF pivot.
