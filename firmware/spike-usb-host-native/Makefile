# Build/flash wrapper around idf.py for the native-usb_host spike
# (docs/bring-up-plan.md step 2, pivot). Built on ESP-IDF's own first-party
# USB Host Library (usb/usb_host.h -- the `espressif/usb` component, fetched
# by the component manager) rather than TinyUSB -- see
# ../notaninstrument-p4/README.md for why: that path's vendored TinyUSB
# fork's connect/disconnect interrupt never fired on real hardware despite
# correct-looking register-level init, a deep enough gap that CLAUDE.md's
# own pre-planned fallback (architecture decision #4) was invoked.
#
# This spike answers a narrower question first, before writing a full MIDI
# class driver: does ESP-IDF's own host stack detect ANY device at all on
# this board's hardware? It's Espressif's own usb_host_lib example
# (examples/peripherals/usb/host/usb_host_lib) copied in directly, unmodified
# -- no custom class driver yet, just descriptor logging on connect.
#
# Requires an activated ESP-IDF environment so `idf.py` is on PATH, e.g.:
#   . $IDF_PATH/export.sh
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
