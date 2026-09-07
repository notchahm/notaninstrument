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

## Status: PASSED, on real hardware (2026-09-06) -- recommended primary path

Confirmed enumerating a real **AKAI MPK Mini Play mk3** and streaming
real-time MIDI performance data through `tuh_midi_rx_cb` as pads were
pressed:

```
MIDI packet: 09 90 37 19   <- Note On,  ch 0, note 0x37, velocity 0x19
MIDI packet: 08 80 37 00   <- Note Off, ch 0, note 0x37
```

**Confirmed generalizing to a second, unrelated controller** (2026-09-06,
same session, zero code changes): a **Korg padKONTROL** (identified via its
own USB string descriptors -- "KORG INC." / "padKONTROL"), a different
vendor and a structurally different device (a multi-pad drum controller
reporting 3 rx / 2 tx virtual MIDI cables, vs. the AKAI's single cable).
Enumerated cleanly and streamed correctly-decoded real-time MIDI data as
pads were pressed:

```
usb_midi_spike: MIDI interface mounted: idx=0 addr=1 itf=0 rx_cables=3 tx_cables=2
MIDI packet: 19 99 3b 6b   <- Note On,  ch 9, note 0x3b, velocity 0x6b
MIDI packet: 18 89 3b 40   <- Note Off, ch 9, note 0x3b
```

(Interspersed with a lot of `00 00 00 00` padding packets -- normal: this
device packs multiple 4-byte USB-MIDI event slots per USB transfer and
zero-pads unused slots rather than omitting them. TinyUSB parsed all of it
correctly regardless.) This rules out "it only works because of something
specific to the AKAI controller's descriptor layout" as an explanation --
two different vendors, two different device topologies, same firmware, no
changes.

Three real bugs were found and fixed on the way here, and one wrong
conclusion was reached and later corrected -- worth reading in full since
it changes how you should interpret "it doesn't work" results on this
board generally, not just for this spike:

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
3. **"Connect interrupt never fires" -- wrong conclusion, corrected.** With
   both of the above fixed, the board no longer crashed, and
   `CFG_TUSB_DEBUG=3` showed completely healthy register-level init --
   real, non-zero `gsnpsid`/`ghwcfg2-4` reads, "Highspeed UTMI+ PHY init",
   `hcd_init()`'s own `HPRT_POWER` (VBUS-on) write present in the source.
   But no USB-MIDI controller, and no unrelated test device (a USB mouse)
   ever produced a single interrupt-level log line, under any condition
   tried. This looked like a real driver bug deep enough to invoke
   CLAUDE.md decision #4's fallback, and the project pivoted to
   `../spike-usb-host-native/` (ESP-IDF's native USB Host Library)
   instead, which passed immediately. **But that pivot was based on a
   false premise**: while testing the native path, it turned out this
   board's 4 USB-A ports aren't equal -- only the ones *not* adjacent to
   the documented host/device jumper actually deliver VBUS power to a
   bus-powered device. Every TinyUSB test above had been run on the
   jumper-adjacent (unpowered) port. The register-level diagnostics only
   proved the *software* executed correctly; they never proved the port
   had power. Re-flashing this exact build (no code changes) onto the
   correct port worked immediately and completely. TinyUSB was never
   broken -- the test setup was.

**Net result**: both this spike and `../spike-usb-host-native/` work on
real hardware, on the correct port. This one is the recommended path
going forward -- TinyUSB's `midi_host.c` gives ready-made USB-MIDI
event-packet parsing for free, where the native USB Host Library path
would need a hand-written class driver to reach the same point.

**Known hardware caveat**: use a non-jumper-adjacent USB-A port. See
`docs/hardware-bom.md` for the working theory on why.

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
  logs `MIDI packet: ...` lines. Confirmed -- see above.
- **Fail:** nothing logs on connect, or the device enumerates
  (`tuh_mount_cb` fires) but `tuh_midi_mount_cb` never does, or a
  crash/hang on connect. **If you hit this, check the USB-A port first**
  (see "Known hardware caveat" above) before suspecting the driver --
  that's exactly what produced a false "fail" here.

## Known risk / things to check if it doesn't flash or doesn't enumerate

- **Wrong USB-A port**: by far the most likely cause if this stops working
  -- see "Known hardware caveat" above. Confirmed to produce a completely
  silent, no-crash, no-log "fail" that looks exactly like a driver bug.
- **Known upstream bug**: a divide-by-zero in the DWC2 host driver on
  device connect has been reported (hathach/tinyusb#3525, filed against the
  esp32-arduino port but the underlying driver is shared). Not hit in
  testing here, but worth knowing about if a crash appears right at
  device-connect on a different TinyUSB version.
