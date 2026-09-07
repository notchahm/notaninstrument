# spike-usb-host-native

Bring-up step 2 (`docs/bring-up-plan.md`) — **PASSED, on real hardware,
2026-09-06**, but now a **proven fallback, not the primary path** — see
`../spike-usb-midi-idf/README.md` for the full, corrected story.

Short version: this path (ESP-IDF's own first-party **USB Host Library**,
`usb/usb_host.h`, not TinyUSB) was originally reached for after
`spike-usb-midi-idf`'s TinyUSB build appeared to hit a real driver bug
(its connect/disconnect interrupt never firing) on real hardware. This
path passed immediately, which looked like confirmation TinyUSB was
genuinely broken. **It wasn't** — the real cause, discovered afterward, was
that every TinyUSB test had been run on this board's one USB-A port that
doesn't deliver power at all (see `docs/hardware-bom.md`). Once TinyUSB
was re-tested on the correct port, it worked too, and more completely: its
`midi_host.c` gives ready-made USB-MIDI event-packet parsing for free,
where this path's `class_driver.c` only dumps raw descriptors — reaching
MIDI parsing here would mean hand-writing a class driver on top of it.

Kept as a working, real-hardware-confirmed reference and fallback, not
deleted — it's genuinely useful evidence that this board's USB-A host
hardware itself works correctly, independent of which USB stack drives it.

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

Not this path, for now — `../spike-usb-midi-idf/` already has working
MIDI event parsing via TinyUSB's `midi_host.c`, confirmed on real
hardware, and is the recommended path going forward. This spike stays as
a working reference in case TinyUSB ever needs to be revisited (a real
regression, an unrelated future project needing a lighter dependency,
etc.) — if that day comes, the remaining work is writing a MIDI class
driver on top of this foundation: open the MIDI Streaming interface's
bulk endpoints (EP2 OUT / EP3 IN in the descriptor dump above) and parse
USB-MIDI event packets, modeled on ESP-IDF's `usb_host_cdc_acm` component
as CLAUDE.md originally planned.
