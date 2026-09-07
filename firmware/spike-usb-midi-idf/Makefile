# Build/flash wrapper around idf.py for the USB-MIDI host spike test
# (docs/bring-up-plan.md step 2), built directly on ESP-IDF + TinyUSB's
# host stack -- see ../spike-usb-midi-arduino/ for the Arduino side of the
# same comparison (CLAUDE.md architecture decision #4).
#
# Requires an activated ESP-IDF environment so `idf.py` is on PATH, e.g.:
#   . $IDF_PATH/export.sh        (export.ps1 on Windows)
#
# One-time per clone:
#   make set-target
#
# Day to day:
#   make build
#   make flash PORT=/dev/ttyUSB0      (PORT optional -- idf.py auto-detects if omitted)
#   make monitor PORT=/dev/ttyUSB0
#   make all PORT=/dev/ttyUSB0        (build + flash + monitor)
#
# The built-in default soundbank (docs/multi-instrument-soundbanks.md)
# lives in its own flash partition (partitions.csv), separate from the
# app -- flash it once with:
#   make flash-soundbank PORT=/dev/ttyUSB0 NIB=../../tools/sfz_preprocessor/testdata/salamander_piano.nib
# It won't be overwritten by `make flash` (app-only) afterwards.

IDF_PY   := idf.py
PORT     ?=
PORT_ARG := $(if $(strip $(PORT)),-p $(PORT),)
# Must match partitions.csv's "soundbank" row offset exactly.
SOUNDBANK_OFFSET := 0x190000
NIB              ?=

.PHONY: all set-target build flash flash-soundbank monitor menuconfig clean fullclean

all: build flash monitor

set-target:
	$(IDF_PY) set-target esp32p4

build:
	$(IDF_PY) build

flash:
	$(IDF_PY) $(PORT_ARG) flash

flash-soundbank:
	@if [ -z "$(NIB)" ]; then echo "Usage: make flash-soundbank NIB=path/to/file.nib [PORT=...]"; exit 1; fi
	esptool.py --chip esp32p4 $(PORT_ARG) write-flash $(SOUNDBANK_OFFSET) $(NIB)

monitor:
	$(IDF_PY) $(PORT_ARG) monitor

menuconfig:
	$(IDF_PY) menuconfig

clean:
	$(IDF_PY) clean

fullclean:
	$(IDF_PY) fullclean
