# Dongle/dock modular architecture

Status: stretch goal, not yet implemented. Orthogonal to the other stretch
goal (`docs/recording-and-looping.md`) — both can proceed independently.

## Goal

Two physical modes sharing one brain:

1. **Dongle mode**: small, streamlined, self-powered. MIDI in via USB host,
   line/headphone out. Attaches to any MIDI instrument on its own.
2. **Dock mode**: the same dongle seats into a larger dock that adds
   charging, Class-D amp + real speakers, a larger display, and more
   controls/ports.

## Core principle: the dock is not a second brain

The ESP32-P4 in the dongle remains the only microcontroller that matters.
The dock is passive amplification, power, and I/O extension around it — not
a second compute platform with its own firmware to keep in sync. This one
decision resolves most of the hard questions below.

## How dock features map onto already-planned mechanisms

- **Dock controls (buttons/knobs/faders)** = a USB-MIDI control surface,
  driven by its own small cheap MCU (RP2040 or similar), plugged into the
  dongle's second USB-A host port. This is the exact same mechanism as the
  nanoKONTROL-as-transport-controller design in
  `docs/recording-and-looping.md` — identified by VID/PID in the same
  `[input_devices]` role table. No new protocol, no custom dock<->dongle
  data link needed for this.
- **Dock Class-D amp + speakers** = a powered speaker from the dongle's
  point of view. Takes the PCM5102's existing analog stereo line-out as
  input, same as headphones or a powered monitor would. Zero dongle-side
  change.
- **Charging while docked** = the one genuinely new piece of engineering,
  but a well-trodden problem: proper power-path management (a PMIC in the
  BQ2407x class is the standard reference point), not naively wiring
  charger + battery + load together. Power-path management lets the device
  run directly off dock power while simultaneously charging the battery,
  avoiding the classic failure modes of charging-under-load.

## What crosses the docking connector

Deliberately limited to signal types forgiving of a separable mechanical
connector:

1. **Power/charge pins** — a couple of pogo contacts or a small connector,
   feeding the PMIC power-path circuit above
2. **Analog stereo line-out pins** — the PCM5102's existing output, routed
   to dock contacts instead of only a headphone jack
3. **A real USB connection** to the dock's own control-surface MCU — an
   actual USB connector (even USB-C), not raw signal pins on the dock
   connector itself, since USB's electrical spec already handles
   unplug/replug reliably

### Explicitly avoid routing through the dock connector
Anything high-speed and digital: I2S audio, and especially a display bus.
SPI is borderline-acceptable through a good connector; MIPI-DSI (which the
P4 supports natively for higher-res panels) is not separable-connector-
friendly at the signal-integrity level realistic hobbyist connectors
achieve.

## Open question: the larger dock display

If a bigger/higher-res display is wanted in dock mode, this is the one
feature without a clean answer yet. Safest path: an SPI color TFT with its
own bus physically extended through the connector — not a bigger/higher-res
panel that would need MIPI. Revisit once the connector itself is chosen and
tested for signal integrity at whatever SPI clock rate the display needs.

## Staging: dev-kit prototyping vs. custom carrier PCB

Pursuing the "small, streamlined dongle" form factor for real — not just
the Waveshare dev board in an enclosure — eventually means a custom carrier
PCB with a purpose-built docking connector. That is a separate, later-stage
hardware effort (real PCB design, not breadboarding), and shouldn't block
earlier firmware work.

Early prototyping can keep using the dev board with a breadboarded/
3D-printed dock mockup that proves the electrical concept (audio hand-off,
USB control surface, charging path) before committing to a compact form
factor or laying out a custom board.

## Suggested phasing

1. Prove the electrical concept on the dev board: line-out into a
   breadboarded Class-D amp + speaker, a second USB MIDI device as a mock
   control surface, and a basic power-path charging circuit — no custom
   connector yet, just jumper wires
2. Design and test the actual docking connector (pin count, mechanical
   mating, power/audio/USB routing) once the electrical concept is proven
3. Design the custom carrier PCB for the compact dongle form factor
4. Design the dock enclosure/PCB (amp, speakers, display, controls, power
   supply)

## Open questions

- Exact docking connector choice (pogo pin block, USB-C acting as the
  physical connector for power+data, a custom multi-pin connector) — needs
  research once the pin list above is finalized
- Whether the dock's control-surface MCU also needs its own small display
  or indicator LEDs, and if so whether that's driven by the dock's own MCU
  independently (simplest) or needs any coordination with the dongle
- PMIC selection and exact charge-current budget once real current draw
  (dongle bring-up phase 8) and dock amp power draw are both known
