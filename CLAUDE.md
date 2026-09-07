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
   `firmware/spike-usb-midi-idf/` vendored those files itself in
   `components/tinyusb_host/` (same upstream source, `git://github.com/
   espressif/tinyusb.git`, just with a from-scratch `CMakeLists.txt`
   building the host-mode file set). That built end-to-end with no
   undefined references, but real-hardware testing (2026-09-06) found two
   more bugs specific to this fork's ESP32-P4 host-mode support: (1) it
   defaulted host mode onto `CFG_TUSB_RHPORT0_MODE`, but P4 has two DWC2
   controllers and Port0 is FS -- this board's native HS USB-A ports are
   Port1; (2) `dwc2_phy_init()`/`dwc2_phy_update()` for ESP32 are literal
   no-op stubs (`// maybe usb_utmi_hal_init()`), so even after fixing (1),
   any register access on the HS controller took a Load access fault --
   the peripheral was never actually clocked on. Fixed that specific crash
   by calling ESP-IDF's own `usb_new_phy()` (`esp_hw_support/usb_phy`)
   before `tusb_init()`, confirmed against that component's own test suite.
   That got past the crash. At that point a further real-hardware finding
   -- the driver's own connect/disconnect interrupt never fired at all,
   confirmed via `CFG_TUSB_DEBUG=3` showing healthy register reads
   (`gsnpsid`, `ghwcfg2-4` all real-looking) but zero interrupt-level
   activity on a live connect -- looked deep enough to invoke this
   decision's own pre-planned fallback, so the TinyUSB path was abandoned
   in favor of ESP-IDF's native USB Host Library (`firmware/
   spike-usb-host-native/`, Espressif's own `usb_host_lib` example with
   one fix: `peripheral_map = 0` instead of the stock example's `BIT0`,
   which likely selects the wrong -- FS, not HS -- peripheral on this
   board). That **passed** immediately: confirmed enumerating a real AKAI
   MPK Mini Play mk3 (VID:PID `0x09e8:0x0050`), full descriptor set
   including its MIDI Streaming interface.
   **CORRECTION, same day: the "interrupt never fires" diagnosis was
   wrong.** While testing the native path, a real hardware finding
   emerged: of this board's 4 USB-A ports, only the ones *not* adjacent to
   the documented host/device jumper actually deliver VBUS power to a
   bus-powered device -- every single TinyUSB test above had been run on
   the jumper-adjacent port, which never had power at all. The
   register-level diagnostics only proved the *software* executed its
   init sequence correctly; they never proved the physical port had
   power. Re-flashing the exact same TinyUSB build (RHPORT1 + `usb_new_phy()`
   fixes intact, no code changes) onto the correct port worked
   immediately and completely: full enumeration, `tuh_mount_cb` /
   `tuh_midi_mount_cb` both fired, and real-time MIDI performance data
   streamed correctly through `tuh_midi_rx_cb` (`09 90 37 19` = Note On
   ch0 note 0x37 vel 0x19, etc.) as pads were pressed on the real
   controller. TinyUSB was never broken -- the test setup was.
   **Net result: both `firmware/spike-usb-midi-idf/` (TinyUSB) and
   `firmware/spike-usb-host-native/` (native USB Host Library) work on
   real hardware, on the correct port.** TinyUSB is the more complete
   result of the two and the recommended path going forward: its
   `midi_host.c` gives ready-made USB-MIDI event-packet parsing (interface/
   jack descriptor parsing, endpoint binding, 4-byte Event Packet framing)
   for free, whereas `spike-usb-host-native`'s class driver only dumps raw
   descriptors -- getting equivalent MIDI parsing there means hand-writing
   all of that on ESP-IDF's native USB Host Library (modeled on
   `usb_host_cdc_acm`, the originally-planned third option), a materially
   bigger lift for no longer any clear benefit now that TinyUSB is
   confirmed working. Keep `spike-usb-host-native` as a proven fallback
   and cross-reference, not as the primary path.
   **Known hardware caveat, not a firmware bug**: use a non-jumper-adjacent
   USB-A port. The jumper-adjacent port is likely this board's one true
   dual-role OTG connector, needing its own board-specific VBUS-enable
   GPIO no generic example or driver would know about; the other ports are
   plausibly simpler always-on fixed host ports. See
   `docs/hardware-bom.md`.
   **Next**: build the real voice-triggering pipeline on
   `firmware/spike-usb-midi-idf`'s proven `tuh_midi_rx_cb` foundation --
   parse incoming Note On/Off bytes into `start_note`/`stop_note` calls
   (architecture decisions #1-#3), continuing the bring-up plan from
   there.

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
  spike-usb-midi-idf/     -- bring-up step 2, WINNING PATH: PASSED on real
                              hardware, raw ESP-IDF + TinyUSB (tuh_*), no
                              Arduino. Confirmed enumerating a real AKAI MPK
                              Mini Play mk3 with real-time MIDI performance
                              data flowing through tuh_midi_rx_cb. Recommended
                              primary path -- see architecture decision #4
                              for the full story (three real bugs found and
                              fixed, one false "abandon" conclusion later
                              corrected). Vendors its own host-mode TinyUSB
                              in components/tinyusb_host/ (the published
                              registry component is device-mode only).
                              Makefile wraps idf.py.
  spike-usb-host-native/  -- bring-up step 2, proven fallback: ESP-IDF's
                              native USB Host Library (espressif/usb
                              component), not TinyUSB. Also confirmed
                              enumerating the same real MPK Mini Play, but
                              descriptor-dump only -- no MIDI event parsing
                              written (would need a hand-written class
                              driver, unlike TinyUSB's ready-made
                              midi_host.c). Keep as a working reference,
                              not the primary path. Makefile wraps idf.py.
  spike-usb-midi-arduino/ -- bring-up step 2, abandoned path: Arduino-ESP32
                              + Adafruit TinyUSB, confirmed dead end before
                              even reaching hardware (forces an external
                              MAX3421E host chip not in this project's BOM).
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

