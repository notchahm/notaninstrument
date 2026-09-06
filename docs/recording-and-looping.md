# Recording, looping, and multi-controller input

Status: stretch goal, not yet implemented. Larger in scope than the
soundbank or DSP effects plans — touches new input hardware (audio ADC),
new MIDI input hardware (5-pin DIN), and a new transport/clock engine, not
just firmware on the existing signal path.

## Goal

Add EP-133 K.O. II-style capability, at higher audio quality: recording and
looping, layered across multiple tracks, controllable standalone (no
computer) via a second USB MIDI controller acting as a transport surface.
See `docs/vision.md` for the fuller comparison, including where the EP-133
is honestly still ahead (its effects and sequencing workflow) and where it
falls short of this project's goals (no USB-MIDI host input, deliberately
lo-fi sample quality, closed/non-modular instrument engine).

## Two distinct recording capabilities

These are genuinely different features with different hardware needs — keep
them separate in implementation order, not bundled as one "recording"
feature.

### 1. MIDI performance recording/looping
Capture the note-on/off events and timing already flowing through the
existing MIDI input path, loop them, layer multiple tracks. Needs **no new
hardware** — built entirely on top of the existing voice mixer and MIDI
parser, plus a new clock/transport layer and event buffer. This is the
near-term, build-first version.

### 2. Real audio sampling
Record actual incoming audio (mic/line-in), not just MIDI notes — the way
the EP-133's own sampling works. Needs a new hardware input path: an I2S ADC
module, e.g. **PCM1808** (TI, 24-bit stereo, I2S output) — the input-side
counterpart to the PCM5102 already chosen for output. Wiring is structurally
similar (BCK/LRCK/DOUT plus format-select pins); needs its own I2S peripheral
or careful sharing, and should run on the same I2S clock domain as the
output path to avoid record/playback sample-rate drift. Most breakout
modules include the necessary anti-aliasing/DC-blocking on the analog input
already.

## Multi-track / layering architecture

- Clock/transport engine: BPM, bar/beat/tick counting, play/record/overdub
  state machine per track
- N track buffers:
  - MIDI-recorded tracks: event lists (key, velocity, timestamp) — cheap,
    tiny
  - Audio-recorded tracks: raw PCM buffers — expensive. A single 30-second
    stereo loop at a modest rate is several MB. This is a real PSRAM/SD
    consumer, in the same budget-contention category as soundbank slots
    (`docs/multi-instrument-soundbanks.md`) and effect delay-line buffers
    (`docs/dsp-effects-chain.md`) — all three compete for the same pool and
    should be budgeted together, not designed in isolation
- Overdub logic: merging newly recorded material into an already-playing
  loop without losing existing content, and handling loop-point wraparound
  cleanly

## Physical control: a second USB MIDI controller as transport surface

Rather than adding dedicated buttons/encoders to the device's own
enclosure, use a second class-compliant USB MIDI controller (e.g. a Korg
nanoKONTROL) plugged into the board's other USB-A host port, with its
knobs/faders/transport buttons mapped to record/play/track-select instead
of musical notes.

**This is well-supported, not a workaround.** rppicomidi's `usb_midi_host`
driver — the one this project's MIDI host layer is built on — explicitly
supports up to 4 simultaneous USB MIDI devices through a hub by default.
This lines up directly with the board's CH334F hub already fanning its one
USB host controller out to two physical USB-A ports. The "only one device"
restriction seen in the original reference sketch (`tuh_midi_mount_cb`
explicitly disabling a second device) was that sketch's own artificial
choice, not a driver limitation — this restriction needs to be removed, not
worked around.

### Device role identification
With two devices mounted, the firmware needs to know which is the musical
controller and which is the transport surface. Match each device's USB
VID/PID (sent during enumeration) against a small table. Extend
`notaninstrument.cfg` with an `[input_devices]` section mapping VID/PID to a
role (`note_input` vs `transport_control`), keeping this configurable rather
than hardcoded:

```ini
[input_devices]
0499:1608=transport_control   ; example VID:PID, verify against actual nanoKONTROL
```

### Transport controller mapping
A small lookup table translating the transport controller's specific CC/note
numbers (its faders, knobs, play/stop/record buttons) into this project's
transport commands (start/stop loop, arm record, select track) — entirely
separate from the note-on/off handling used for musical input.

### USB power budget
Hosting two devices simultaneously through the hub means both draw from the
same power budget that was previously sized for one controller. Check the
CH334F hub's per-port current limit and overall 5V supply headroom once
both devices are actually connected — a measurement to take during bring-up,
not something to assume doubles cleanly from the single-device estimate.

## Optional: 5-pin DIN MIDI input

Adds compatibility with pre-USB MIDI gear alongside the USB host ports.
Hardware: an opto-isolated MIDI-in circuit (6N138-based, standard MIDI input
circuit) into a UART pin — same approach noted early in this project's
hardware planning, now being formally added to the BOM.

### Merging DIN input with USB input(s)

MIDI's only native multi-device mechanism is **THRU-chaining**, which fans
one source out to multiple downstream receivers (Controller -> Module A IN,
Module A THRU -> Module B IN, ...). It solves the opposite problem from what
this project needs: THRU is one-source-many-receivers; this project needs
many-sources-one-receiver, which is **merging** — a distinct, harder
capability that dedicated MIDI merge boxes exist specifically to solve.

The hazard in naive merging is at the raw-byte level: MIDI's "running
status" omits repeated status bytes to save bandwidth, so interleaving two
raw byte streams before parsing can corrupt messages from either stream.

**This project avoids the hazard by construction, for both USB and DIN
input:** each input stream — every USB device (already separated by the
host stack's per-device addressing) and the DIN UART stream — is parsed
independently into complete, decoded MIDI messages, each maintaining its
own running-status state. Merging happens **after** parsing, at the message
level (already-decoded note-on/off/CC events), not by interleaving raw
bytes. At that point "merging" is just: route each decoded message to its
handler based on which stream/device it came from (reusing the same
device-role table as the two-USB-controller case above). This is true
regardless of whether the second stream is another USB device or the DIN
UART — the design principle is the same either way: **parse per-stream
first, merge decoded messages second.**

## Display upgrade (related, optional)

A stretch-goal-tier "good visual UI" (waveform display, track state, real
menus) likely means moving off the SSD1306 to a color display driven by
LVGL. Worth noting the ESP32-P4 has genuine, verified hardware support for
this: a dedicated Pixel Processing Accelerator (PPA) + DMA2D block with
official Espressif-backed LVGL integration (fill/blend/scale/rotate
offloaded from the CPU, up to 9x speedup on fills per Espressif's own
benchmarks). Not required for MIDI-loop recording (capability 1 above), but
relevant once real audio waveforms and multi-track visual state are in
scope.

## Suggested phasing

Larger and more speculative than the soundbank/effects plans — treat as
later-phase, and sequence the two recording capabilities separately rather
than as one feature:

1. MIDI performance recording/looping (capability 1) — no new hardware,
   builds directly on the existing voice mixer and MIDI parser
2. Multi-device USB MIDI host support (remove the single-device
   restriction) + VID/PID role table + nanoKONTROL transport mapping —
   enables standalone control of the above without a computer
3. PCM1808 audio input hardware + real audio sampling (capability 2)
4. 5-pin DIN MIDI input, with the parse-then-merge architecture above
5. Display upgrade to LVGL + color panel, if the visual-UI stretch goal is
   pursued

## Open questions

- Exact nanoKONTROL model and its CC/note assignments — needs to be looked
  up (or configured via Korg's own editor) once the specific unit is chosen
- Whether audio-recorded tracks get written straight to PSRAM only, or also
  flushed to SD for persistence across power cycles ("saving a session")
- How loop length/quantization grid is set for audio-recorded tracks vs.
  MIDI-recorded ones — audio loops need a hard pre-defined length (or a
  defined recording-stop trigger); MIDI loops could in principle support
  free-length recording more easily
- Total PSRAM budget reconciliation across soundbank slots, DSP effect
  buffers, and now track-recording buffers — revisit once all three
  features have real numbers, not just each planned independently

