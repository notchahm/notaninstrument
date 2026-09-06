# Bring-up plan

Ordered to surface the biggest unknown (USB MIDI host on ESP32-P4) as early
as possible, before other work is built on top of it. Steps 3 and 4 don't
depend on step 2's outcome and can proceed in parallel if step 2 is blocked
or being re-worked.

## 1. Toolchain and basic bring-up
Install Arduino-ESP32 (or ESP-IDF) targeting P4, flash a blink/serial test,
confirm the board enumerates. Check serial boot log for PSRAM size reported
as ~32MB — catch a PSRAM misconfiguration now, not after the soundbank
loader is built against it.

## 2. USB MIDI host spike test (do this before anything else substantial)
Standalone sketch, not the full project: call `USBHost.begin()`, log whether
`tuh_midi_mount_cb` / `tuh_midi_rx_cb` fire when the MIDI controller is
plugged in. This determines whether Arduino-ESP32 + Adafruit TinyUSB has
working P4 host support yet. Pass -> continue on Arduino. Fail -> pivot to
raw ESP-IDF (TinyUSB as IDF component, or a hand-written class driver on the
ESP-IDF USB Host Library, modeled on `usb_host_cdc_acm`).

## 3. Display bring-up (SSD1306)
Wire OLED to a free I2C bus (distinct from the onboard ES8311 codec's bus, if
that's still active). Get `Adafruit_SSD1306` printing text. Use this as the
on-device debug surface for every later phase instead of relying solely on
serial output.

## 4. Audio output path (PCM5102 over I2S)
Get one hard-coded test tone or short WAV playing cleanly through the DAC
before touching MIDI or SD. Proves I2S wiring, clocking, and DMA buffering in
isolation, so later audio glitches can be localized to mixing/SD rather than
the output chain itself.

## 5. Soundbank pipeline: offline tool + on-device loading
Build and test the SFZ->binary preprocessing tool on a computer first (no
hardware needed): pick velocity layers, find/set loop points, resample,
optionally ADPCM-encode, pack into the custom binary format with an index.
Then on-device: mount SD over `SD_MMC`, read the bank file, load fully into
PSRAM, log total bytes used against the offline size estimate.

## 6. Single-voice integration: MIDI note triggers a real sample
Wire the proven pieces together for the simplest case: one note-on plays one
sample from PSRAM through I2S, one note-off releases it. No polyphony, no
envelope state machine yet — isolates "does the full chain work end to end"
from "does the mixer work."

## 7. Voice manager: polyphony and release envelopes
Build the `Voice` struct, fixed voice pool, mixer summing, and HELD/RELEASING
envelope stages, now that every dependency (MIDI in, sample playback, I2S
out) is independently proven. Test 2-3 note chords before pushing toward
target polyphony.

## 8. Power and battery sizing (last, using real measurements)
Measure actual current draw with a USB power meter under realistic load
(MIDI active, audio playing, display on). Size LiPo capacity and
charge/boost circuit off that number — this board draws more than the
RP2350 prototype did (P4 + onboard C6 co-processor + USB hub chip), so don't
reuse earlier capacity assumptions.

