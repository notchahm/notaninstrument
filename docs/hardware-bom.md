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

## USB-A port behavior (confirmed 2026-09-06, real hardware + schematic)

The board actually has **4** USB-A ports, not the 2 the main table's
description implies — confirmed by direct inspection, not just the "2x
via CH334F hub" documentation. Empirically, only the ports **not adjacent
to the documented host/device jumper** delivered VBUS power to a
bus-powered test device (a USB mouse showed no sign of life on the
jumper-adjacent port, but powered up immediately on a different one; the
same non-jumper-adjacent port then successfully enumerated both the mouse
and an AKAI MPK Mini Play mk3, confirmed independently via both
`firmware/spike-usb-host-native/` and `firmware/spike-usb-midi-idf/`
(TinyUSB) — the latter had been misdiagnosed as having a driver bug before
this port finding explained the real cause; see that spike's README for
the full story).

**Root cause, now confirmed against Waveshare's own schematic** (vendor
PDF, `docs/datasheets/ESP32-P4-WIFI6-DEV-KIT-schematic.pdf` — fetched from
`files.waveshare.com`, rendered and read directly, not guessed): the
earlier "VBUS-enable GPIO" theory below was wrong. It's a **data-line
topology switch, not a power switch**:

- **J2** and **J8** are each a stacked dual USB-A socket ("双层USB母座 90度
  弯脚") — 2 physical shells apiece, 4 total, matching the empirical count.
- **U14 (CH334F)** is a genuine 4-downstream-port USB2.0 hub. Three of its
  four downstream ports go to real connectors: `DP1/DM1`→J2 shell 1,
  `DP2/DM2`→J2 shell 2, `DP3/DM3`→J8 shell 1. Its **4th downstream port
  (`DP4/DM4`) only breaks out to unpopulated test points (TP1/TP2)** — not
  a real connector at all.
- **U15 (FSUSB42UMX)** is a 2:1 USB high-speed data mux. Its common port
  (D+/D-) is wired straight to the ESP32-P4's own native USB D+/D-
  (`USBD_P`/`USBD_N`). Per the schematic's own annotation, `SEL:H-->2;
  L-->1`: side 1 goes to CH334F's *upstream* port (feeding the whole hub),
  side 2 goes directly to **J8's 2nd shell**.
- **H3** is the physical 3-pin jumper header driving that `SEL` line (via a
  0Ω resistor, R36) — this is the "USB OTG Function Selection" jumper
  called out in Waveshare's docs, physically near J8, which is exactly why
  it reads as "jumper-adjacent."
- **VBUS is a red herring.** `U6 (DIO7003HEST5)` is a simple load switch
  whose `EN` pin is hard pulled high via a resistor (R25) straight to
  `VCC_5V` — always on, not gated by the hub, a GPIO, or the jumper at all.
  Its output (`VBUS_OUT`) feeds **all 4 shells' VBUS pins identically, all
  the time**. Every shell has 5V present regardless of jumper position.

So with the jumper on **HOST** (as this project has it): the P4's native
USB feeds CH334F's upstream port, lighting up J2's 2 shells + J8's 1st
shell as 3 real, simultaneous host ports — that's the documented "expand
USB ports" behavior. **J8's 2nd shell's data lines simply go nowhere in
this jumper position** — not unpowered, not broken, just disconnected by
design, because that shell is wired to the mux's *other* side (the P4's
single native port, active only in the opposite/device jumper position,
where the other 3 hub ports would in turn go dark instead). It's mutually
exclusive by hardware design, not a bug and not something firmware can
route around.

**Practical takeaway: nothing to fix.** Use the 3 non-jumper-adjacent
shells (J2 both + J8's outer one) as host ports, which is already what
this project's spikes have been doing. The 4th shell is the OTG/device
connector, not a 4th simultaneous host port — nothing in the schematic
offers a way to have 4 host ports live at once.

## Pin assignments

| Signal | Pin | Notes |
|---|---|---|
| I2C SDA (SSD1306) | GPIO7 | Confirmed working 2026-09-06, real hardware. |
| I2C SCL (SSD1306) | GPIO8 | Confirmed working 2026-09-06, real hardware. |

An I2C bus scan on these pins (`firmware/notaninstrument-p4`'s
`scan_i2c_bus()`) found the SSD1306 at `0x3C` and a second device at
`0x18` -- almost certainly the onboard ES8311 codec, whose default I2C
address is commonly `0x18`. **This confirms GPIO7/GPIO8 are the same I2C
bus the ES8311 is on, not a separate free bus** -- but that's not
currently a problem, since the two devices' addresses don't collide and
both ACK correctly. Worth re-checking if the ES8311 is ever actually used
for anything (it isn't currently -- see "What this is" in CLAUDE.md).

Still TBD — fill in once checked:
- Which I2S pins to use for the PCM5102 (a full I2S peripheral separate from
  whatever the ES8311 uses, if that codec is left connected at all)
- Whether the board exposes a labeled 5V rail on the GPIO header for future
  battery-boost input, separate from the USB-C power/programming port

