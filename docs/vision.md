# Vision

## Why this exists

MIDI controllers are instruments without an instrument. Picking one up and
playing it shouldn't require hooking up a computer first — but the built-in
sounds on cheap all-in-one keyboards are usually an afterthought, and the
good all-in-one ones weld the sound engine to a specific controller. There's
no modularity: you can't swap controllers without swapping sounds, or swap
sounds without swapping controllers.

The AKAI MPK Mini Play is the closest thing on the market to solving this
from the controller side — plug in, play, no computer. It's also the thing
that proved the gap instead of closing it: a fun, portable device with a
mediocre built-in synth permanently welded to that one controller. No
modularity at all — no way to give it better sounds, no way to reuse that
sound engine with a different controller.

The **Miditech PianoBox line** (Mini, USB, Pro) is closer still, and closer
than it's comfortable to admit: a standalone box a MIDI keyboard plugs
straight into, no computer required — the exact shape this project takes.
It's real, shipping hardware. Most models support USB-MIDI host input
alongside traditional 5-pin DIN, the same connection method this project
uses, and the Pro model runs up to 64-voice polyphony over 16-bit/48kHz
samples with onboard reverb, battery power, and USB charging. So "does a
computer-free MIDI-to-sound module already exist" is, honestly, already
answered: yes.

What it doesn't do is the actual point of this project. PianoBox plays a
fixed bank of 128 General MIDI sounds — a GM soundfont-style set, not a real
acoustic instrument recording. "Piano" is the product name, not a
description of the sound: a real Salamander Grand Piano library (16
velocity layers, 24-bit/48kHz per note) is a different category of sound
entirely from one GM patch among 128. And that soundset is baked into the
hardware — no SD card, no swappable soundbanks, no path to loading a
different instrument, let alone the multi-instrument, effects, or looping
ambitions in this project's later rungs. It's a finished consumer appliance
with a fixed feature set, not a platform.

A third reference point, closer to this project's later rungs than its
core: Teenage Engineering's **EP-133 K.O. II**. It's the actual named
inspiration behind the recording/looping and dynamic-sampling rungs below
(`docs/recording-and-looping.md`, `docs/dynamic-sampling.md`) — a pocket
sampler that captures real audio and turns it into playable, sequenced
material, with a genuinely more mature effects and sequencing workflow (6
master FX, 12 punch-in FX, a real step sequencer, 999 sample slots) than
anything built here yet. Worth being honest about where it's ahead, not
just where it falls short.

And it does fall short of this project's specific goals, in different ways
than PianoBox does. It has no USB-MIDI host input at all — MIDI I/O is
3.5mm TRS-A jacks, and its native playing surface is its own 16
pressure-sensitive pads, not a way to plug in an external keyboard and play
it chromatically across a real range. Sample quality (46kHz/16-bit) is a
deliberate lo-fi character choice core to its identity, not aimed at the
depth of a properly captured multi-velocity acoustic instrument. And like
PianoBox, it's a closed, self-contained instrument — its "modularity" is in
sequencing and pattern workflow, not in swapping the voice engine itself or
physically docking into other hardware.

So the actual gap isn't "no computer-free MIDI module exists" or "no clever
sampler exists" — PianoBox and the EP-133 each close one of those, and each
does part of its job better than this project does today. It's the
combination neither offers: played from any real MIDI keyboard, with real
sampled-instrument depth, modular enough to swap instruments and build
effects, looping, and dynamic sampling on top — rather than living inside
one closed box. That's the reason to build this rather than buying one of
each.

The hardware path here reflects that goal, not just convenience:
- **Teensy 4.0/4.1** has the best existing audio ecosystem (the Teensy Audio
  Library is a real reference point for the DSP effects work), but no
  out-of-the-box USB-MIDI host support and a higher price for the full
  board+shield+host-adapter combo.
- **RP2350** proved the concept works: USB-MIDI host via Arduino +
  Adafruit TinyUSB + PIO-USB, a note triggering a real WAV sample. It also
  proved the concept's ceiling — ~520KB of SRAM can't hold a real
  multi-sampled instrument, only a small, compressed one, or real-time SD
  streaming per voice (a polyphony problem, not a solution).
- **ESP32-P4** was chosen specifically to remove that ceiling: 32MB of
  PSRAM to hold a real soundbank (not a toy one), dual RISC-V cores with
  DSP/FPU headroom for real-time mixing and effects, and native high-speed
  USB-OTG host hardware — the same category of MIDI-host capability RP2350
  already proved out, just with the memory budget to actually do something
  with it.

That tradeoff was made deliberately and re-confirmed on 2026-09-06, after
hitting real USB-host toolchain friction on P4 that RP2350 doesn't have:
the harder platform was chosen on purpose, because the payoff — a real
soundbank, real polyphony, and headroom for everything below — isn't
reachable on RP2350's memory budget. See "Where things actually stand"
below for the current, honest state of that bet.

## What this becomes

Not just "plays piano sounds." Six rungs, each building on mechanisms the
rung below it already established — this holds together as one
architecture, not a pile of bolted-on feature ideas:

1. **Core instrument** (this file's baseline, `CLAUDE.md`). Plug in any
   class-compliant USB-MIDI controller, play a real multi-sampled instrument,
   polyphonically, standalone — no computer, no DAW, instant-on. Salamander
   Grand Piano is the first instrument built, not the ceiling on what this
   plays.
2. **Multi-instrument soundbanks**
   (`docs/multi-instrument-soundbanks.md`). A real instrument library, not
   just piano — drums, guitar, strings, and whatever else gets converted —
   each a soundbank on the SD card, one per MIDI channel, swappable at
   runtime via Program Change without a reboot.
3. **DSP effects chain** (`docs/dsp-effects-chain.md`). EQ, delay,
   compression, chorus, reverb on the mixed output. The difference between
   "plays samples" and "sounds like an instrument."
4. **Recording & looping** (`docs/recording-and-looping.md`). EP-133 K.O.
   II-style layered looping, controlled standalone via a second USB-MIDI
   device acting as a transport surface — the same multi-device host
   support this rung needs is a generalization of "one controller" that
   rung 1 already has to get right. Later: real audio sampling via a
   PCM1808 ADC, and 5-pin DIN MIDI merge for pre-USB gear.
5. **Dynamic sampling** (`docs/dynamic-sampling.md`). Record a real sound on
   the device itself — a guitar pluck, a voice, anything into the same
   PCM1808 input rung 4 needs anyway — and play it back chromatically across
   the keyboard as a new instrument, without a computer or an offline
   preprocessing step. The new instrument comes out in the same `.nib`
   format rung 2 already knows how to route and hot-swap, so this rung
   doesn't need new plumbing for that half of the problem, only the
   recording and on-device processing half.
6. **Dongle/dock modularity**
   (`docs/dongle-dock-architecture.md`). The literal physical answer to the
   original motivation: a small self-powered dongle that can also seat into
   a larger dock adding an amp, real speakers, a bigger display, and
   physical controls — one brain, never two firmwares to keep in sync. The
   dock's controls are, again, just another USB-MIDI device.

Each of these docs already has a phasing plan, a memory/CPU budget
discussion, and open questions — this file is the map connecting them, not
a replacement for any of them.

## Where things actually stand (2026-09-06)

**Bring-up step 2 of 8** (`docs/bring-up-plan.md`) — USB-MIDI host
viability on ESP32-P4, the single biggest technical risk this project
identified up front (`CLAUDE.md` architecture decision #4) — **passed, on
real hardware, 2026-09-06.**

- **Arduino-ESP32 + Adafruit TinyUSB was a confirmed dead end**, found
  before even touching hardware — Adafruit's ESP32 host config hardcodes
  an external MAX3421E SPI chip not in this project's BOM. See
  `firmware/spike-usb-midi-arduino/README.md`.
- **Raw ESP-IDF + vendored TinyUSB found two real bugs on real hardware**
  a clean build couldn't have caught (wrong root hub port for P4's dual
  DWC2 controllers; a stubbed PHY/clock-init function causing a
  register-access crash, fixed via ESP-IDF's own `usb_new_phy()`). After
  fixing both, the driver's own connect/disconnect interrupt appeared to
  never fire at all, even with every register read confirmed healthy —
  this looked like a third real bug, deep enough to pivot away from
  TinyUSB entirely.
- **ESP-IDF's native USB Host Library passed immediately** — not TinyUSB
  at all. `firmware/spike-usb-host-native/`, Espressif's own
  `usb_host_lib` example with one fix (the stock example's hardcoded
  peripheral selection likely picks the wrong — FS, not HS — controller on
  this board). Confirmed enumerating a real AKAI MPK Mini Play mk3 — the
  same device that started this whole project — which looked like
  confirmation that TinyUSB's interrupt issue was a genuine, unfixable
  driver bug.
- **It wasn't.** While testing the native path, a real hardware finding
  emerged: of this board's 4 USB-A ports, only the ones *not* adjacent to
  the documented host/device jumper deliver VBUS power to a bus-powered
  device at all. Every TinyUSB test had been run on the jumper-adjacent
  port — unpowered the whole time. The register-level diagnostics only
  ever proved the *software* executed correctly; they never proved the
  port had power. **Re-flashing the exact same TinyUSB build (no code
  changes) onto the correct port worked immediately and completely**:
  full enumeration, and real-time MIDI performance data streaming
  correctly through `tuh_midi_rx_cb` as pads were pressed on the real
  controller. TinyUSB was never broken — the test setup was. Confirmed
  generalizing to a second, unrelated controller (a Korg padKONTROL) with
  zero code changes, ruling out anything specific to the AKAI's own
  descriptor layout as the explanation.
- **Net result: both paths work on real hardware, on the correct port.**
  TinyUSB (`firmware/spike-usb-midi-idf/`) is the recommended path going
  forward — its `midi_host.c` gives ready-made USB-MIDI event-packet
  parsing for free, where the native USB Host Library path
  (`firmware/spike-usb-host-native/`, kept as a proven fallback) only
  dumps raw descriptors and would need a hand-written class driver to
  reach the same point.
- **One still-relevant hardware finding, not a firmware bug**: use a
  non-jumper-adjacent USB-A port. Working theory: the jumper-adjacent port
  is the one true dual-role OTG connector, needing a board-specific
  VBUS-enable GPIO no generic example would know about; the other ports
  are simpler always-on fixed host ports.
- **The platform bet was explicitly re-examined and re-confirmed** earlier
  in this process: given RP2350 already had working MIDI-in and P4 didn't
  yet, staying on P4 was a deliberate choice for the soundbank/polyphony
  ceiling described above, not inertia — and it's now paid off.
- **`make test`** (`firmware/test-builds.sh`) catches build regressions
  across firmware targets automatically, including flagging if the
  Arduino dead-end ever stops failing for its documented reason.

## What's next

1. `firmware/spike-usb-midi-idf` already parses real MIDI events via
   `tuh_midi_rx_cb`, confirmed on real hardware. Wire that into actual
   `start_note`/`stop_note` calls (CLAUDE.md architecture decisions #1-#3).
2. Continue the bring-up plan in order from there — display, audio output,
   soundbank pipeline, single-voice integration, then polyphony.
3. Optionally, track down the jumper-adjacent OTG port's power-enable GPIO
   from the board schematic, if dual-role (host/device-switchable)
   behavior on that specific connector ever matters — not currently
   blocking anything.
