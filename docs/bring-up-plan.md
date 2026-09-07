# Bring-up plan

Ordered to surface the biggest unknown (USB MIDI host on ESP32-P4) as early
as possible, before other work is built on top of it. Steps 3 and 4 don't
depend on step 2's outcome and can proceed in parallel if step 2 is blocked
or being re-worked.

## 1. Toolchain and basic bring-up -- DONE (2026-09-06, confirmed on real hardware)
Install Arduino-ESP32 (or ESP-IDF) targeting P4, flash a blink/serial test,
confirm the board enumerates. Check serial boot log for PSRAM size reported
as ~32MB — catch a PSRAM misconfiguration now, not after the soundbank
loader is built against it.

Confirmed via `firmware/notaninstrument-p4` flashed over the board's UART
port (COM5 on the Windows host, passed through to WSL2 via usbipd-win as
`/dev/ttyACM0` -- the CH343 bridge chip enumerates as USB CDC-ACM, not the
older vendor-driver path): `Chip model: ESP32-P4`, `Chip revision: 301`
(v3.1), `CPU cores: 2`, `CPU freq: 400 MHz`, `Flash size: 16777216 bytes`
(16MB exactly), `PSRAM size: 33554432 bytes` (32MB exactly) -- every number
matches `docs/hardware-bom.md` and CLAUDE.md's hardware section precisely.

**Real bug found and fixed along the way**: the `esp32:esp32:esp32p4` FQBN's
`ChipVariant` option defaults to "Before v3.00" (`prev3`), but this board's
actual silicon is revision v3.1. That mismatch caused an immediate
`CHIP_LP_WDT_RESET` boot loop -- the ROM bootloader repeating every ~1
second, app code never reached, confirmed via raw serial capture (`stty` +
`cat` on the device, since `arduino-cli monitor` produced no output when
captured non-interactively). Adding `ChipVariant=postv3` to the FQBN in
`firmware/notaninstrument-p4/Makefile` fixed it immediately. Worth checking
for the same `ChipVariant` mismatch before assuming any future P4 boot
issue is something else.

## 2. USB MIDI host spike test -- PASSED (2026-09-06, on real hardware)
Standalone sketch, not the full project: confirm the board's native USB-A
host port can enumerate a class-compliant USB-MIDI controller at all,
before building anything else on top of it.

Several paths were tried, in order, based on real evidence at each step --
full detail in CLAUDE.md architecture decision #4:
1. Arduino-ESP32 + Adafruit TinyUSB -- confirmed dead end via static
   analysis alone, before touching hardware (forces an external MAX3421E
   host chip not in this project's BOM).
2. Raw ESP-IDF + TinyUSB (vendored host-mode component) -- built clean,
   but real-hardware testing surfaced two real bugs (wrong root hub port
   for this board's dual-DWC2-controller P4; a stubbed-out PHY/clock init
   causing a register-access crash, fixed via ESP-IDF's own
   `usb_new_phy()`). After fixing both, the driver's own connect/disconnect
   interrupt appeared to never fire at all despite otherwise-healthy
   register-level init -- this *looked* like a third real bug, deep enough
   to trigger a pivot to option 3 below.
3. ESP-IDF's native USB Host Library (not TinyUSB) --
   `firmware/spike-usb-host-native/`, Espressif's own `usb_host_lib`
   example with one fix (the stock example's `peripheral_map = BIT0`
   likely selects the wrong -- FS, not HS -- peripheral on this board;
   `peripheral_map = 0` selects the documented default, HS on HS-capable
   targets). This enumerated a real AKAI MPK Mini Play mk3 immediately,
   which looked like confirmation that TinyUSB's interrupt issue was real.
4. **It wasn't.** While testing option 3, a real hardware finding emerged:
   of this board's 4 USB-A ports, only the ones *not* adjacent to the
   documented host/device jumper deliver VBUS power to a bus-powered
   device at all. Every test of option 2 had been run on the
   jumper-adjacent (unpowered) port -- the register-level diagnostics only
   proved the software executed correctly, never that the port had power.
   **Re-flashing option 2's exact build (no code changes) onto the
   correct port worked immediately and completely**: full enumeration,
   both `tuh_mount_cb`/`tuh_midi_mount_cb` firing, and real-time MIDI
   performance data streaming correctly through `tuh_midi_rx_cb` as pads
   were pressed on the real controller. Confirmed generalizing to a
   second, unrelated controller too (a Korg padKONTROL, different vendor
   and device topology) with zero code changes -- see
   `firmware/spike-usb-midi-idf/README.md`.

**Net result: both option 2 (TinyUSB) and option 3 (native USB Host
Library) work on real hardware, on the correct port.** Option 2 is the
recommended path going forward -- its `midi_host.c` gives ready-made
USB-MIDI event-packet parsing for free, where option 3 only dumps raw
descriptors and would need a hand-written class driver to reach the same
point.

One still-relevant hardware finding, not a firmware bug: use a
non-jumper-adjacent USB-A port. Working theory (unconfirmed against the
schematic): the jumper-adjacent port is the board's one true dual-role OTG
connector, needing its own board-specific VBUS-enable GPIO that a generic
example has no way to know about, while the other ports are simpler
always-on fixed host ports. See `docs/hardware-bom.md`.

**Next**: `firmware/spike-usb-midi-idf` already parses real MIDI events
via `tuh_midi_rx_cb` -- the next real step is wiring that into actual
`start_note`/`stop_note` calls (CLAUDE.md architecture decisions #1-#3),
before moving on to step 3 below.

## 3. Display bring-up (SSD1306) -- DONE (2026-09-06, confirmed on real hardware)
Wire OLED to a free I2C bus (distinct from the onboard ES8311 codec's bus, if
that's still active). Get `Adafruit_SSD1306` printing text. Use this as the
on-device debug surface for every later phase instead of relying solely on
serial output.

Confirmed via `firmware/notaninstrument-p4` (extended, not a separate
spike -- this is the "active P4 firmware" bring-up sketch accumulating
across steps). Wired a GeeekPi 128x64 SSD1306 module to GPIO7 (SDA) /
GPIO8 (SCL) / 5V / GND. An I2C bus scan (`scan_i2c_bus()`, now a permanent
periodic diagnostic, not a one-off) found two devices sharing that bus:
`0x18` (almost certainly the onboard ES8311 codec CLAUDE.md already
flagged as a possible bus-sharing risk on these pins) and `0x3C` (the
SSD1306) -- no address collision, both coexist fine. `display_text()` and
`display_note()` (adapted from `firmware/legacy_code/midi_display.cpp`,
which had several real bugs fixed along the way -- see that firmware's
README) both confirmed rendering correctly on the physical screen.

One thing worth recording for next time: the display appeared completely
blank on the very first flash, which looked at first like a wiring/power
problem -- but the I2C scan proved the display was electrically alive and
ACKing at 0x3C the whole time, ruling that out. It started working on a
subsequent flash without a code change identified as the fix, so the
actual cause of that first blank screen is unresolved (possibly just a
display needing a moment to settle after power-up, or an incidental wire
reseat) -- not treated as a real bug since it hasn't recurred, but worth
a first troubleshooting step (I2C scan, not wiring) if it ever does.

**Since integrated into step 2's real hardware**: `firmware/spike-usb-midi-idf`
now drives this same OLED directly from live `tuh_midi_rx_cb` events (via
the ESP-IDF-native `k0i05/esp_ssd1306` component, not the Arduino
`Adafruit_SSD1306` used for this step's own standalone proof) -- channel,
note name (scientific pitch notation) or CC number, and a velocity/value
bar, confirmed updating in real time as a real controller was played. Two
real problems surfaced and got fixed once a fast-streaming control (a pot/
joystick) was scrubbed continuously: clearing the whole screen before
every redraw caused visible flicker, and redrawing on every single MIDI
message (which can arrive far faster than the I2C bus + this library's
page-oriented text API can render) caused visible lag. Fixed by rate-limiting
actual screen writes to 20Hz and overwriting fixed-width fields in place
instead of clearing first -- see `firmware/spike-usb-midi-idf/README.md`.

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

