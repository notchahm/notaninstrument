# spike-usb-host-native

Bring-up step 2 (`docs/bring-up-plan.md`) — **PASSED, on real hardware,
2026-09-06.** This is the winning path: ESP-IDF's own first-party **USB
Host Library** (`usb/usb_host.h`, the `espressif/usb` component), not
TinyUSB. See `../spike-usb-midi-idf/README.md` for why that path was
abandoned — three real bugs found on real hardware, the last of which (the
driver's own connect/disconnect interrupt never firing) was deep enough to
invoke CLAUDE.md architecture decision #4's own pre-planned fallback.

This is Espressif's own `examples/peripherals/usb/host/usb_host_lib`
example, copied in with one fix (see below) — not written from scratch.
It explicitly lists ESP32-P4 as a supported target.

## Confirmed result

Enumerated a real **AKAI MPK Mini Play mk3** (the actual device this whole
project is inspired by — see `docs/project-motivation.md`) on real
hardware, full descriptor set:

```
Manufacturer: Akai Professional
Product:      MPK mini Play mk3
VID:PID:      0x09e8:0x0050

Interface 0: HID (control surface)
Interface 1: Audio Class, Audio Control subclass
Interface 2: Audio Class, MIDI Streaming subclass  <- the actual MIDI interface
  EP2 OUT (bulk) / EP3 IN (bulk), exactly per the USB-MIDI 1.0 spec
```

Also confirmed enumerating a Logitech Gaming Mouse G303 as an
unrelated-device sanity check before the MIDI controller test.

## The one fix needed

The stock example hardcodes:
```c
.peripheral_map = BIT0,
```
ESP32-P4 has two DWC2 USB controllers (Port0 = FS, Port1 = HS — same
numbering that broke the TinyUSB spike). `peripheral_map`'s own doc
comment in `usb_host.h`: *"Set to 0 to use the default peripheral... on
High-Speed capable targets, the default is the High-Speed peripheral."*
`BIT0` is not `0` — it's a bitmask explicitly selecting peripheral 0,
likely FS, not this board's native HS USB-A ports. Changed to:
```c
.peripheral_map = 0,
```

## A real, unresolved hardware finding (not a firmware bug)

This board has 4 USB-A ports. In testing, only ports **not adjacent to the
documented host/device jumper** delivered VBUS power to a bus-powered test
device (a mouse showed no sign of life on the jumper-adjacent port, but
powered up immediately on a different one). The jumper-adjacent port is
likely the board's one genuine dual-role OTG connector — probably needing
its own board-specific VBUS-enable GPIO that a generic example has no way
to know about — while the other ports are plausibly simpler, always-on
fixed host ports. **Use a non-jumper-adjacent port** until/unless that GPIO
is identified from the board schematic. This is not something firmware can
route around without knowing the actual pin.

## Build and flash

```
make set-target       # one-time per clone
make build
make flash PORT=/dev/ttyACM0     # or COM3 on Windows, /dev/ttyUSB0, etc.
make monitor PORT=/dev/ttyACM0
```

Or `make all PORT=/dev/ttyACM0` for build + flash + monitor together.

Flashing/monitoring goes over the board's UART programming port, not the
USB-A host ports this spike tests against. If running under WSL2, see
`docs/bring-up-plan.md` step 1's notes on `usbipd-win` for getting the
board's serial port passed through.

## What's next

This spike only dumps USB descriptors on connect (`class_driver.c`'s
`action_open_dev()` path) — it doesn't parse MIDI messages. The next real
step is writing an actual MIDI class driver on top of this proven
foundation: open the MIDI Streaming interface's bulk endpoints (EP2 OUT /
EP3 IN in the descriptor dump above) and parse USB-MIDI event packets,
modeled on ESP-IDF's `usb_host_cdc_acm` component as CLAUDE.md originally
planned (MIDI's bulk-endpoint shape is structurally similar to CDC's data
endpoints).
