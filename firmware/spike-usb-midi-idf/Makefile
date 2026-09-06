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

IDF_PY   := idf.py
PORT     ?=
PORT_ARG := $(if $(strip $(PORT)),-p $(PORT),)

.PHONY: all set-target build flash monitor menuconfig clean fullclean

all: build flash monitor

set-target:
	$(IDF_PY) set-target esp32p4

build:
	$(IDF_PY) build

flash:
	$(IDF_PY) $(PORT_ARG) flash

monitor:
	$(IDF_PY) $(PORT_ARG) monitor

menuconfig:
	$(IDF_PY) menuconfig

clean:
	$(IDF_PY) clean

fullclean:
	$(IDF_PY) fullclean
