# bringup-arduino-p4 firmware

This is bring-up step 1 from `docs/bring-up-plan.md`: a toolchain smoke
test, not the real firmware. It blinks an LED and logs chip/PSRAM info over
serial so we can confirm the ESP32-P4 board enumerates and PSRAM reports
~32MB before building anything else on top of it.

Flashing goes over a plain serial (UART) connection -- either the board's
own onboard UART-to-USB bridge, or an external FTDI adapter wired to the
UART0 RX/TX pins -- not the native USB-OTG host ports (those are jumpered to
host mode per `docs/hardware-bom.md` and are reserved for the USB-MIDI
controller).

## Prerequisites

- [`arduino-cli`](https://arduino.github.io/arduino-cli/latest/installation/)
  on your `PATH`.
- One-time setup, registers the ESP32 board index and installs the core:

  ```
  make setup
  ```

## Build and flash

```
make list-ports                    # find your adapter's port
make build
make flash PORT=/dev/ttyUSB0       # or PORT=COM3 on Windows
make monitor PORT=/dev/ttyUSB0     # Ctrl+C / Ctrl+] to exit, depending on OS
```

Or all in one go:

```
make all PORT=/dev/ttyUSB0
```

## What to check in the serial log

- Chip model reports as ESP32-P4.
- PSRAM size reports ~32MB. If it reports 0 / not detected, the board's
  PSRAM menu option (e.g. OPI PSRAM) likely needs to be added to the `FQBN`
  variable in the `Makefile` -- run `arduino-cli board listall esp32p4`
  after `make setup` to see the available menu options for this core
  version.
- The LED blinks and a `tick` line prints roughly twice a second.

## Known TBD

- `LED_PIN` in `bringup-arduino-p4.ino` is a guess (GPIO2, the common
  default on many ESP32 dev boards) and has not been verified against this
  specific Waveshare board's schematic/silkscreen. Update it and record the
  confirmed pin in `docs/hardware-bom.md` once checked.

`FQBN` in the `Makefile` (`PSRAM=enabled,FlashSize=16M`) has been confirmed
against the installed arduino-esp32 core 3.3.11 (`arduino-cli board details
-b esp32:esp32:esp32p4`) -- board defaults are PSRAM disabled and 4MB flash,
both wrong for this board, hence the explicit overrides.
