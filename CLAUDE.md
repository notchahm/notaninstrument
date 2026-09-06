# Notaninstrument — USB-MIDI Polyphonic Sampler

## What this is

A standalone hardware module that turns any class-compliant USB-MIDI controller
into a self-contained instrument: plug a MIDI keyboard into a USB-A host port,
get polyphonic, sample-based sound out of a speaker/line-out — no computer,
no DAW, instant-on. Inspired by the AKAI MPK Mini Play, but built from scratch
to support real multi-sampled instruments rather than a small onboard synth
engine or a fixed General MIDI soundset (see `docs/vision.md` for how this
compares to the Miditech PianoBox line, the closest existing product).

The **Salamander Grand Piano SFZ library is the first instrument, not the
whole scope.** The goal is a real instrument library — piano, drums, guitar,
and other multi-sampled instruments, each swappable per MIDI channel (see
`docs/multi-instrument-soundbanks.md`) — plus the ability to build new
playable instruments from live-sampled audio on the device itself, not only
from pre-processed libraries (see `docs/dynamic-sampling.md`).

**See `docs/vision.md` first** for the full "why," the ladder of ambition
from core instrument through multi-instrument soundbanks, DSP effects,
recording/looping/dynamic sampling, and dongle/dock modularity, and an
honest current-status snapshot. This file (`CLAUDE.md`) is the technical
reference; `vision.md` is the map connecting it to
`docs/project-motivation.md` and the stretch-goal design docs.

## Current target hardware

- **MCU board:** Waveshare ESP32-P4-WIFI6-DEV-KIT
  - ESP32-P4: dual-core RISC-V @ 400MHz, DSP extensions + FPU, 32MB PSRAM,
    16MB NOR flash
  - Onboard ESP32-C6 co-processor (WiFi 6 / BLE) over SDIO — not currently
    used by this project; candidate for power-down if unused, since it adds
    quiescent draw
  - Native High-Speed USB 2.0 OTG (two USB-A ports via a CH334F hub, both
    switched together between host/device mode via jumper — set to HOST)
  - Native SDIO 3.0 microSD slot
  - Onboard ES8311 audio codec — **mono only**, not used; we drive our own
    stereo DAC instead (see below)
- **Display:** SSD1306 OLED (I2C) — exact model/resolution TBD once wired up
- **Audio DAC:** GY-PCM5102 module (PCM5102A), driven over I2S — chosen over
  the board's onboard ES8311 because ES8311 is mono and this project needs
  real stereo mixing
- **Storage:** microSD card (SDIO), loaded with a **pre-processed** Salamander
  Grand Piano soundbank (see "Soundbank pipeline" below) — the raw ~1.1GB SFZ
  library is never used on-device, only as input to an offline conversion tool
- **MIDI input:** class-compliant USB-MIDI controller via native USB host

## Prior art in this repo

`firmware/reference-rp2350/` contains the original working sketch from an
earlier RP2350 (Raspberry Pi Pico 2) prototype. It has:
- Working MIDI-in handling (`start_note`/`stop_note`) using rppicomidi's
  `usb_midi_host` driver on top of Adafruit TinyUSB Host (PIO-USB on RP2350)
- Commented-out synth/mixer code (`play_active_notes`, `synth_note`,
  `play_note_from_wav`) that was never fully wired up — **do not port this
  code as-is**, see "Known issues in the reference code" below
- It's kept as reference for the MIDI-handling callback shape and lessons
  learned, not as a base to build on directly — the P4 has a completely
  different USB host stack underneath.

### Known issues in the reference code (do not repeat these)
- Decay was coupled to note-ON, not note-OFF — gives a plucked-string
  envelope regardless of how long the key is held, not real sustain/release.
  New voice model must track HELD vs RELEASING state explicitly.
- Used `sin()` per sample per voice in the innermost mix loop — fine for one
  voice, expensive once polyphonic. Use a phase-accumulator + lookup table.
- Hard-clamped clipping on mix overflow — sounds harsh. Scale voices by
  ~1/sqrt(active_voice_count) before summing instead.
- Drove the mixer off wall-clock polling (`time_us_64()` + manual gating)
  rather than I2S buffer-empty/DMA callbacks — fragile under load, prefer
  buffer-driven timing on the new platform.
- Per-voice separate SD file handles/reads — fine for single-stream playback,
  bad for polyphony (N simultaneous small seeks per mix cycle), and unsafe at
  any polyphony count for a deeper reason than seek count: **SD card read
  latency jitter**. Consumer SD cards periodically stall for tens of
  milliseconds during internal housekeeping (flash translation layer garbage
  collection, wear leveling) — normal, expected behavior for the media, not
  a fault condition. If the I2S DMA buffer runs dry during one of these
  stalls, the result is an audible click or dropout, and the stall's timing
  is unpredictable — not something a read-ahead buffer sized for the common
  case reliably covers. This was the actual roadblock that killed real-time
  SD-per-voice streaming as a polyphony strategy on the RP2350 prototype, not
  just a performance nice-to-have. The new design must not stream sample
  data from SD in real time — see below.

## Architecture decisions made so far

1. **Soundbank is loaded once into PSRAM at boot, not streamed from SD during
   play.** SD access is a load-time-only step. This turns a hard real-time
   problem (streaming N voices off one SD card without dropouts) into an easy
   one (drain PSRAM into I2S DMA). Concretely, this decision exists because of
   SD card read latency jitter (see "Known issues in the reference code"
   above) — an I2S DMA buffer running dry during a normal tens-of-milliseconds
   SD housekeeping stall is audible, and that's what actually broke down
   under polyphony on the RP2350 prototype, not a theoretical risk.
2. **The soundbank must be pre-processed offline before it goes anywhere near
   the device.** Salamander's raw SFZ+WAV library is ~1.1GB (16 velocity
   layers, 24-bit/48kHz) — 30-50x too large for 32MB PSRAM. Offline pipeline
   (`tools/sfz_preprocessor/`, not yet built) will: pick a reduced velocity
   layer count (target: 4-6), trim each sample to attack + short sustain with
   loop points instead of full decay tails, downsample to 16-bit/32kHz
   stereo, and optionally apply 4:1 ADPCM. Target output size: single-digit
   MB, leaving headroom in the 32MB budget.
3. **Voice model:** fixed pool of `Voice` structs (key, velocity, sample
   pointer/offset, phase, envelope stage [HELD/RELEASING], envelope level),
   mixer sums active voices per I2S callback, voice-stealing when pool is
   full. Not yet implemented for this platform.
4. **USB MIDI host is the single biggest technical risk in this project.**
   **UPDATE 2026-09-06 -- Arduino path confirmed broken, not just unproven:**
   static analysis of the installed toolchain (arduino-esp32 core 3.3.11,
   Adafruit TinyUSB Library 3.7.7 -- see
   `firmware/spike-usb-midi-arduino/README.md` for exact file/line
   evidence) shows Adafruit TinyUSB's ESP32 host support unconditionally
   forces `CFG_TUH_MAX3421` (an external SPI MAX3421E host-controller chip,
   not in this project's BOM) as the only host controller on every ESP32
   target, with no per-sketch override, and never enables `CFG_TUH_MIDI` at
   all. It cannot drive this board's native USB-A/DWC2 host ports for
   USB-MIDI as currently packaged -- this is effectively a failing spike
   result obtained without even needing hardware.
   **Raw ESP-IDF + TinyUSB is now the primary path**, not just the fallback.
   **UPDATE 2026-09-06 -- builds cleanly, untested on hardware:** the
   published `espressif/tinyusb` *registry component* turned out to be
   device-mode only (its own `CMakeLists.txt` never builds `usbh.c`,
   `hcd_dwc2.c`, or `midi_host.c`, regardless of config) -- so
   `firmware/spike-usb-midi-idf/` now vendors those files itself in
   `components/tinyusb_host/` (same upstream source, `git://github.com/
   espressif/tinyusb.git`, just with a from-scratch `CMakeLists.txt`
   building the host-mode file set). With that fix, ESP-IDF v6.1 built the
   spike end-to-end with no undefined references (`spike_usb_midi_idf.elf`,
   ~216KB). Still needs to be **flashed to real hardware and tested against
   an actual USB-MIDI controller** -- a clean build proves the code and
   link graph are sound, not that the DWC2 host controller actually
   enumerates a device on this board. If that also fails, the remaining
   fallback is a hand-written class driver on ESP-IDF's native USB Host
   Library (modeled on `usb_host_cdc_acm`, since MIDI's bulk-endpoint shape
   is structurally similar to CDC's data endpoints).

## Bring-up plan (in order — see docs/bring-up-plan.md for full detail)

1. Toolchain + basic board bring-up (confirm PSRAM detected correctly)
2. **USB MIDI host spike test — do this before anything else substantial**
3. SSD1306 display bring-up
4. PCM5102/I2S audio output path (single test tone/WAV)
5. Soundbank pipeline: offline SFZ→binary tool + on-device SD→PSRAM loading
6. Single-voice integration: one MIDI note plays one real sample
7. Voice manager: polyphony + release envelopes
8. Power/battery sizing (measure real current draw first — this board draws
   more than the RP2350 prototype did, due to the onboard C6 co-processor
   and USB hub chip)

## Toolchain setup

**UPDATE 2026-09-06:** decision #4's spikes now point at ESP-IDF as
primary, not Arduino-ESP32 (Arduino's USB host support is confirmed broken
for this board, see decision #4). ESP-IDF v6.1 (`esp32p4` target only) has
been installed and validated against `firmware/spike-usb-midi-idf/` — clone
with `git clone -b v6.1 --recursive --depth 1 --shallow-submodules
https://github.com/espressif/esp-idf.git`, then `./install.sh esp32p4`,
then `. ./export.sh` in each new shell before running `idf.py`/`make` in
any `firmware/spike-usb-midi-idf`-style project. Arduino-ESP32 setup below
is kept for `firmware/notaninstrument-p4/` (blink/serial/PSRAM bring-up
only — that part doesn't touch USB host, so Arduino is fine for it) and for
historical/comparison purposes.

- Install Arduino IDE or `arduino-cli`
- Add board manager URL for `espressif/arduino-esp32` (use a recent release
  that explicitly lists ESP32-P4 support — check release notes for P4 status
  before pinning a version)
- Select board: ESP32-P4 target (exact board definition name TBD once
  installed — check board manager listing)
- Libraries needed:
  - `Adafruit TinyUSB Library` (for `Adafruit_USBH_Host` — verify P4 host
    mode support directly, per architecture decision #4)
  - rppicomidi's `usb_midi_host` (application driver on top of TinyUSB;
    chip-agnostic in principle)
  - `Adafruit_SSD1306` + `Adafruit_GFX`
  - I2S output: native ESP32 I2S driver (via Arduino `driver/i2s_std.h` or
    a thin wrapper) — no extra library needed
  - SD: `SD_MMC.h` (native SDIO, not `SD.h`/SPI)

If the USB host spike fails on Arduino-ESP32, fall back to raw ESP-IDF:
`idf.py create-project`, target `esp32p4`, and reference
`examples/peripherals/usb/host/` in the ESP-IDF tree plus the
`usb_host_cdc_acm` component as a structural template for a custom MIDI
class driver.

## Repo layout

```
Makefile                  -- repo-level `make test`/`make test-clean`,
                              wraps firmware/test-builds.sh
firmware/
  test-builds.sh          -- build verification suite: compiles every
                              firmware target and checks pass/fail against
                              what's expected (including treating
                              spike-usb-midi-arduino's known compile
                              failure as an XFAIL, not a permanent red
                              herring -- flagged if it ever changes)
  notaninstrument-p4/     -- active P4 firmware (bring-up step 1: blink +
                              serial/PSRAM check, Makefile wraps arduino-cli)
  spike-usb-midi-idf/     -- bring-up step 2, raw-ESP-IDF side: USB MIDI
                              host spike test on ESP-IDF + TinyUSB (tuh_*),
                              no Arduino. Builds clean (ESP-IDF v6.1),
                              untested on hardware. Vendors its own
                              host-mode TinyUSB in components/tinyusb_host/
                              (the published registry component is
                              device-mode only). Makefile wraps idf.py.
  spike-usb-midi-arduino/ -- bring-up step 2, Arduino side: same spike test
                              on Arduino-ESP32 + Adafruit TinyUSB, identical
                              tuh_midi_* callbacks for a fair comparison.
                              Makefile wraps arduino-cli.
  reference-rp2350/       -- prior working RP2350 sketch, reference only
tools/
  sfz_preprocessor/       -- offline SFZ->binary soundbank converter (TBD)
docs/
  vision.md                       -- start here: why, the full ambition
                                      ladder, honest current status
  vision.html                     -- designed page version of vision.md;
                                      source of the published artifact, not
                                      auto-synced -- re-copy and republish by
                                      hand after editing vision.md
  project-motivation.md           -- the original, personal "why" (raw)
  bring-up-plan.md                -- full staged bring-up plan
  hardware-bom.md                 -- bill of materials + pin assignments
  multi-instrument-soundbanks.md  -- stretch goal: .nib format, channel
                                      routing, runtime instrument swap
  dsp-effects-chain.md            -- stretch goal: EQ/delay/compression/
                                      chorus/reverb on the mixed output
  recording-and-looping.md        -- stretch goal: MIDI + audio looping,
                                      multi-controller input, DIN MIDI merge
  dynamic-sampling.md             -- stretch goal: turn live-recorded audio
                                      into a new playable instrument on-device
  dongle-dock-architecture.md     -- stretch goal: dongle/dock modularity
```

## Open questions / TBD

- Exact GPIO pin assignments for I2S (PCM5102), I2C (SSD1306), and whether
  they conflict with the onboard ES8311 codec's pins — fill in once board is
  in hand and pinout doc is checked
- Whether Adafruit TinyUSB's Arduino wrapper actually exposes working P4 host
  mode (architecture decision #4 — this is the first thing to test)
- Final velocity-layer count / loop-trim length for the soundbank, once
  actual PSRAM headroom after firmware overhead is known
- Battery/power circuit sizing — deferred until real current draw is measured

