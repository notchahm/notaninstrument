# Hardware bill of materials

| Part | Notes |
|---|---|
| Waveshare ESP32-P4-WIFI6-DEV-KIT | Main board. 32MB PSRAM, 16MB flash, onboard ESP32-C6 (WiFi6/BLE, unused by this project), native HS USB OTG (2x USB-A via CH334F hub, jumper-set to host mode), native SDIO microSD slot. |
| SSD1306 OLED display (128x64, I2C) | **Already owned, proven working on the RP2350 prototype.** Exact wiring/pin assignment TBD once re-wired to the P4 board. |
| GY-PCM5102 (PCM5102A) I2S DAC module | **Already owned, proven working on the RP2350 prototype.** Stereo line-level output. Chosen over the board's onboard ES8311 codec because ES8311 is mono-only. |
| 2x 2" 4Ω 3W speakers | **Already owned, proven working on the RP2350 prototype**, driven off the PAM8403 below rather than line-out/headphones. |
| PAM8403 class-D amp module (3W x2, 5V) | **Already owned, proven working on the RP2350 prototype** — this is not a "later, if needed" item, it's tested hardware that drove the speakers above directly. Still only needed if driving bare speakers rather than line-out/headphones on the P4 build; keep both options open until that's decided. |
| microSD card | Loaded with pre-processed Salamander Grand Piano soundbank (not the raw SFZ library — see tools/sfz_preprocessor). Use a name-brand card for consistent read latency. P4 reads it over its native SDIO slot — no separate reader breakout needed (see "Hardware that doesn't carry over from RP2350" below). |
| USB-MIDI controller | Class-compliant, connects to one of the board's USB-A host ports. |
| (Stretch goal — `docs/recording-and-looping.md`) Treedix DIN-5 breakout jacks + Adafruit MIDI FeatherWing | **Already owned, proven working on the RP2350 prototype** (opto-isolated 5-pin DIN MIDI in/out). Not part of the core MVP bring-up plan — revisit when the DIN MIDI merge stretch goal is picked up. |
| (Stretch goal — `docs/recording-and-looping.md`, `docs/dynamic-sampling.md`) PCM1808 I2S ADC module | Not yet owned. Audio input for real audio sampling/looping and for the dynamic-sampling rung — the input-side counterpart to the PCM5102 output. Needed by two stretch goals now, not one; worth prioritizing if either is picked up. |
| (Later) LiPo cell + charge IC (e.g. MCP73831) + boost to 5V | For portable/battery operation. Size after measuring real current draw (bring-up step 8) — this board draws more than a bare RP2350 would. |

## Hardware that doesn't carry over from RP2350

Two parts of the RP2350 prototype's kit were specific to that board's
constraints and aren't part of the P4 build, worth recording so the reason
isn't lost:
- **SPI microSD reader breakouts** (a level-shifted one, and one of
  Adafruit's 3.3V-only boards) — the RP2350 needed an external SPI SD
  breakout; the P4 dev kit has a native SDIO microSD slot built in, so no
  separate reader module is needed here.
- **Micro-USB OTG adapter cable** — needed on the RP2350/Pico 2 to let its
  single micro-USB port act as USB host while still drawing power from
  somewhere else. The P4 dev kit's two USB-A host ports (via the onboard
  CH334F hub, jumper-set to host mode) don't need this workaround.

## Pin assignments

TBD — fill in once the board is in hand and its pinout documentation/silkscreen
is checked. Things to confirm:
- Which I2C pins are free for the SSD1306, and whether they conflict with the
  onboard ES8311 codec's I2C bus
- Which I2S pins to use for the PCM5102 (a full I2S peripheral separate from
  whatever the ES8311 uses, if that codec is left connected at all)
- Whether the board exposes a labeled 5V rail on the GPIO header for future
  battery-boost input, separate from the USB-C power/programming port

