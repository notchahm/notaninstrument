# notaninstrument-p4

The official firmware for the notaninstrument USB-MIDI polyphonic sampler
(see the repo root `CLAUDE.md` and `docs/vision.md` for the project as a
whole). This is a single ESP-IDF application, built for the
Waveshare ESP32-P4-WIFI6-DEV-KIT, that does everything the device does:

- **USB MIDI host** (`usb_midi_host.c`) -- enumerates a class-compliant
  USB-MIDI controller on the board's native USB-A host port via ESP-IDF's
  native USB Host Library, and streams decoded Note On/Off/CC events in
  real time. Handles USB hub hot-swap (a hub plugged in mid-session, or a
  controller hot-swapped through one) without crashing.
- **Polyphonic voice engine** (`voice_engine.c`) -- a fixed 8-voice pool,
  ISR-driven straight from the I2S DMA-completion callback, fixed-point
  throughout (no floats in the hot path). Two built-in instruments,
  channel-routed: a Salamander grand piano on every channel except MIDI
  channel 10, and a drum kit (DrumGizmo MuldjordKit, overhead mic pair) on
  channel 10, per the GM percussion convention.
- **Sample playback from flash** (`nib_loader.c`, `adpcm_decode.c`,
  `qoa_decode.c`) -- both instruments are pre-processed offline
  (`tools/sfz_preprocessor/`) into a compact `.nib` binary format, mmap'd
  directly from dedicated flash partitions (`partitions.csv`) rather than
  streamed from SD or loaded into PSRAM -- see `CLAUDE.md` architecture
  decision #1 for why (SD read-latency jitter under polyphony).
- **Stereo audio out** (`audio_output.c`) -- PCM5102A DAC over I2S,
  rendered direct-to-DMA from the ISR (no separate audio task).
- **SSD1306 OLED display** (`main.c`) -- shows the current MIDI
  channel/note/velocity (or CC) live, rate-limited and flicker-free.

Regression tests for this firmware's audio pipeline and MIDI/USB event
handling live in `test/` and run via `make test-audio` at the repo root
(`tools/sfz_preprocessor/test_soundbank_regression.py`) -- no hardware
required.

## Build and flash

```
make set-target       # one-time per clone
make build
make flash PORT=/dev/ttyUSB0     # or COM3 on Windows; omit PORT to let idf.py auto-detect
make monitor PORT=/dev/ttyUSB0
```

Or `make all PORT=/dev/ttyUSB0` for build + flash + monitor together.

Flashing/monitoring goes over the board's UART programming port (an FTDI
adapter or onboard UART bridge), not the native USB-OTG host port -- that
port is jumpered to host mode for the USB-MIDI controller. See
`docs/hardware-bom.md` for the board's USB-A port quirks (only one of the
four shells actually works, independent of jumper position).

The two built-in instrument soundbanks live in their own flash partitions
(`partitions.csv`), separate from the app, so firmware and content can be
reflashed independently:

```
make flash-soundbank PORT=/dev/ttyUSB0 NIB=../../tools/sfz_preprocessor/testdata/salamander_piano_qoa.nib
make flash-drumkit   PORT=/dev/ttyUSB0 NIB=../../tools/sfz_preprocessor/testdata/muldjordkit_drums.nib
```

Neither is overwritten by a plain `make flash` (app-only) afterwards.

## Why a vendored TinyUSB component (`components/tinyusb_host/`)

This app's own USB MIDI host path (`usb_midi_host.c`) uses ESP-IDF's
native USB Host Library, not TinyUSB -- see "History" below for why. The
vendored TinyUSB fork under `components/tinyusb_host/` is excluded from
the build (`CMakeLists.txt`'s `EXCLUDE_COMPONENTS`) and kept only as an
in-tree reference: it was vendored by hand because the published
`espressif/tinyusb` registry component's own `CMakeLists.txt` only builds
device-mode sources (no `usbh.c`, `hcd_dwc2.c`, or `midi_host.c`,
regardless of config) -- confirmed by reading it directly. The host-mode
sources exist in that same package's `src/` tree; `components/tinyusb_host/`
copies that tree (from the resolved `espressif/tinyusb@0.19.0~3` package,
commit `ab4a1817907ed865b10a712b5e03e5e0a9902df5`) with a from-scratch
`CMakeLists.txt` that builds the host-mode file set instead. See the
comment block at the top of `components/tinyusb_host/CMakeLists.txt` for
how to refresh this vendored copy against a newer TinyUSB version, should
the native-library path ever need revisiting.

## History: TinyUSB vs. the native USB Host Library

This firmware's USB MIDI host path went through both real options in
`CLAUDE.md` architecture decision #4 before landing on the native USB Host
Library:

1. **TinyUSB, built directly on ESP-IDF** (no Arduino) -- got real USB-MIDI
   working first: confirmed enumerating and streaming correctly-decoded
   real-time MIDI from both an AKAI MPK Mini Play mk3 and a Korg
   padKONTROL, after finding and fixing three real bugs (wrong DWC2 root
   port for this board's HS USB-A ports, stubbed PHY/clock init in the
   vendored fork's ESP32 support, and a red-herring "connect interrupt
   never fires" diagnosis that turned out to be an unpowered USB-A port,
   not a driver bug).
2. **ESP-IDF's native USB Host Library** -- adopted as the primary path
   after the chord-onset latency investigation (`docs/
   polyphony-latency-investigation.md`) traced a consistent ~242ms
   chord-onset gap to TinyUSB's own DWC2 host-stack behavior on this
   ESP32-P4 port, not this project's code, the MIDI device, or the
   endpoint type. Swapping in the native library on the same real
   hardware/controller measured 0-11ms instead, so `usb_midi_host.c`
   (this app's current MIDI host path) is built on it, and TinyUSB was
   dropped from the build.

Full blow-by-blow (exact bugs, register-level evidence, the two
controllers' captured MIDI logs) is in `CLAUDE.md` architecture decision
#4 and `docs/polyphony-latency-investigation.md` -- not duplicated here to
avoid the two drifting out of sync.

## Known hardware caveats

- **USB-A port**: only one of the board's four USB-A shells actually
  delivers VBUS power to a bus-powered device, independent of the
  documented host/device jumper position. See `docs/hardware-bom.md`.
- **Multiple controllers**: the working port only exposes one device
  directly, but a standard external USB hub plugged into it supports
  multiple simultaneous class-compliant MIDI controllers, confirmed on
  real hardware (`CONFIG_USB_HOST_HUBS_SUPPORTED=y` in
  `sdkconfig.defaults`, required for reliable hub hot-swap).
