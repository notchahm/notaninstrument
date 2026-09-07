# spike-usb-midi-idf

Bring-up step 2 (`docs/bring-up-plan.md`): USB-MIDI host spike test, built
directly on **ESP-IDF + TinyUSB's host stack**, no Arduino involved. This is
the "raw ESP-IDF" side of CLAUDE.md's architecture decision #4. See
`../spike-usb-midi-arduino/` for the Arduino + Adafruit TinyUSB side of the
same test -- both spikes implement the identical `tuh_midi_*` callbacks so
their results are directly comparable.

This is a standalone diagnostic, not the real project. It answers one
question: does `tuh_midi_mount_cb` / `tuh_midi_rx_cb` fire when a
class-compliant USB-MIDI controller is plugged into the board's USB-A host
port?

## Status: ABANDONED (2026-09-06) -- see ../spike-usb-host-native/ instead

This path built cleanly and is kept as a documented dead end, not deleted.
Real-hardware testing found three real bugs in sequence, the last of which
was deep enough to invoke CLAUDE.md architecture decision #4's own
pre-planned fallback:

1. **Wrong root hub port.** `main/tusb_config.h` set
   `CFG_TUSB_RHPORT0_MODE`, but ESP32-P4 has two DWC2 controllers --
   `components/tinyusb_host/src/portable/synopsys/dwc2/dwc2_esp32.h`'s own
   comment: "Port0 to OTG_FS, and Port1 to OTG_HS". This board's native HS
   USB-A ports are Port1. Fixed by switching to `CFG_TUSB_RHPORT1_MODE`.
2. **Stubbed PHY/clock init.** With RHPORT1 selected, the board immediately
   hit a `Load access fault` at exactly `DWC2_HS_REG_BASE + 0x48` -- a real
   crash, not a config mismatch. `dwc2_phy_init()`/`dwc2_phy_update()` for
   ESP32 in this vendored fork are literal no-op stubs (`// maybe
   usb_utmi_hal_init()`), so the HS controller's peripheral clock was never
   actually enabled. Fixed by calling ESP-IDF's own `usb_new_phy()`
   (`esp_hw_support/usb_phy`) before `tusb_init()`, with the exact config
   verified against that component's own test suite
   (`test_apps/usb_phy`, "Init internal UTMI PHY" case), not guessed.
3. **Connect interrupt never fires.** With both of the above fixed, the
   board no longer crashed, and `CFG_TUSB_DEBUG=3` showed completely
   healthy register-level init -- real, non-zero `gsnpsid`/`ghwcfg2-4`
   reads, "Highspeed UTMI+ PHY init", `hcd_init()`'s own `HPRT_POWER`
   (VBUS-on) and interrupt-unmask writes all present in the source. But no
   USB-MIDI controller, and no unrelated test device (a USB mouse) ever
   produced a single interrupt-level log line, under any condition tried:
   already connected at boot, live unplug/replug, multiple physical ports.
   Not fixed -- this is where the pivot to ESP-IDF's native USB Host
   Library happened instead, and that path immediately succeeded (see
   `../spike-usb-host-native/README.md`).

Kept in the repo for the documented bugs above, in case a future TinyUSB
fork version fixes the interrupt issue and this path is worth revisiting.

## Prerequisites

- ESP-IDF (v6.1 tested; anything v5.x+ with esp32p4 support should work)
  installed and activated in your shell:
  ```
  . $IDF_PATH/export.sh
  ```
- No component-manager fetch needed -- `components/tinyusb_host/` vendors
  the TinyUSB source directly in this repo (see "Why a vendored component"
  below). `make build` / `make set-target` just work against a clean ESP-IDF
  install.

## Why a vendored component, not `main/idf_component.yml`

Originally this pulled `espressif/tinyusb` via the component manager. That
did NOT work: the **published registry component's own `CMakeLists.txt` only
builds DEVICE-mode sources** (`cdc_device.c`, `midi_device.c`, `dcd_dwc2.c`,
`usbd.c`, ...) -- no `usbh.c`, no `hcd_dwc2.c`, no `midi_host.c`, regardless
of `tusb_config.h` settings. It's device-mode only, full stop -- confirmed
by reading its `CMakeLists.txt` directly, not by trial and error alone.

The host-side source (host controller driver, host stack, MIDI host class)
**is present** in that same package's `src/` tree, though -- it's pulled
from `git://github.com/espressif/tinyusb.git` (the Espressif fork with
ESP32 host-controller ports added, e.g. DWC2 host DMA for P4), the same
repo the earlier "does P4 host mode exist at all" research confirmed. The
registry component's CMakeLists.txt just doesn't reference those files.

So `components/tinyusb_host/` vendors that same `src/` tree (copied from
the resolved `espressif/tinyusb@0.19.0~3` package, commit
`ab4a1817907ed865b10a712b5e03e5e0a9902df5`) with a from-scratch
`CMakeLists.txt` that builds the **host**-mode file set instead:
`tusb.c`, `common/tusb_fifo.c`, `host/usbh.c`, `host/hub.c`,
`class/midi/midi_host.c`, `portable/synopsys/dwc2/hcd_dwc2.c`,
`portable/synopsys/dwc2/dwc2_common.c`. See the comment block at the top of
`components/tinyusb_host/CMakeLists.txt` for how to refresh this vendored
copy against a newer TinyUSB version.

Getting this to build also required adding `main` to the component's
`REQUIRES` (tusb_option.h expects to find `main`'s `tusb_config.h`) and
`tinyusb_host` to `main`'s `PRIV_REQUIRES` (so `main.c` can see `tusb.h`) --
ESP-IDF's per-component include isolation doesn't do this automatically in
either direction.

## Build and flash

```
make set-target       # one-time per clone
make build
make flash PORT=/dev/ttyUSB0     # or COM3 on Windows; omit PORT to let idf.py auto-detect
make monitor PORT=/dev/ttyUSB0
```

Or `make all PORT=/dev/ttyUSB0` for build + flash + monitor together.

Flashing/monitoring goes over the board's UART programming port (an FTDI
adapter or onboard UART bridge), not the native USB-OTG host port(s) --
those are jumpered to host mode and are what this spike is testing against.

## What counts as pass/fail

- **Pass:** plugging in a USB-MIDI controller logs `USB device mounted`
  followed by `MIDI device mounted`, and pressing keys on the controller
  logs `MIDI packet: ...` lines.
- **Fail:** nothing logs on connect, or the device enumerates
  (`tuh_mount_cb` fires) but `tuh_midi_mount_cb` never does (MIDI class
  driver not recognizing the interface), or a crash/hang on connect.

Per CLAUDE.md decision #4, a pass here (or on the Arduino side) means P4 USB
MIDI host is viable at all right now -- worth knowing even if the project
ultimately builds on the other toolchain, since it rules out "TinyUSB on P4
just doesn't work yet" as the failure mode.

## Known risk / things to check if it doesn't flash or doesn't enumerate

- **Known upstream bug**: a divide-by-zero in the DWC2 host driver on
  device connect has been reported (hathach/tinyusb#3525, filed against the
  esp32-arduino port but the underlying driver is shared). If you hit a
  crash right at device-connect, check whether a newer component version
  fixes it before assuming the board/wiring is at fault.
- **RHPORT**: `main/tusb_config.h` assumes root hub port 0 is wired to the
  board's USB-A host jacks (via the onboard CH334F hub). Unconfirmed --
  verify against the board's actual USB routing if nothing enumerates at
  all.
- If TinyUSB host mode turns out broken on P4 entirely, CLAUDE.md's
  documented fallback is a hand-written MIDI class driver directly on
  ESP-IDF's native `usb_host` library (modeled on `usb_host_cdc_acm`,
  bypassing TinyUSB altogether) -- a bigger undertaking than this spike, not
  attempted here.
