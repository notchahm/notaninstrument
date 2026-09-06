# Build/flash wrapper around arduino-cli for the USB-MIDI host spike test
# (docs/bring-up-plan.md step 2), on the Arduino-ESP32 + Adafruit TinyUSB
# path -- see ../spike-usb-midi-idf/ for the raw-ESP-IDF side of the same
# comparison (CLAUDE.md architecture decision #4).
#
# One-time setup (also installs the ESP32 core if you haven't already via
# ../notaninstrument-p4/Makefile):
#   make setup
#
# Day to day:
#   make list-ports
#   make build
#   make flash PORT=/dev/ttyUSB0      (or PORT=COM3 on Windows)
#   make monitor PORT=/dev/ttyUSB0
#   make all PORT=/dev/ttyUSB0        (build + flash + monitor)
#
# Requires arduino-cli on PATH: https://arduino.github.io/arduino-cli/latest/installation/

SKETCH_DIR   := $(CURDIR)
BUILD_DIR    := $(CURDIR)/build

# Confirmed against arduino-esp32 core 3.3.11 via
# `arduino-cli board details -b esp32:esp32:esp32p4`: the board's defaults
# already do the right thing for this spike --
# USB Mode = "USB-OTG (TinyUSB)" (default) leaves the native OTG port free
# for Adafruit TinyUSB to claim as HOST, while
# Upload Mode = "UART0 / Hardware CDC" (default) keeps Serial and flashing
# on UART0 (the FTDI port) instead of contending for the same OTG port. No
# extra menu options needed.
FQBN         := esp32:esp32:esp32p4

ESP32_CORE_URL := https://espressif.github.io/arduino-esp32/package_esp32_index.json

# Serial port of the FTDI/UART adapter (or the board's onboard UART bridge)
# used for flashing -- not the native USB-OTG host port this spike tests.
PORT         ?=

UPLOAD_BAUD  ?= 460800
MONITOR_BAUD ?= 115200

ARDUINO_CLI  := arduino-cli

.PHONY: all setup list-ports build flash monitor clean check-port

all: build flash monitor

setup:
	$(ARDUINO_CLI) config init --overwrite=false || true
	$(ARDUINO_CLI) config add board_manager.additional_urls $(ESP32_CORE_URL)
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install esp32:esp32
	$(ARDUINO_CLI) lib install "Adafruit TinyUSB Library"

list-ports:
	$(ARDUINO_CLI) board list

check-port:
ifeq ($(strip $(PORT)),)
	$(error PORT is not set. Run 'make list-ports' to find it, then pass PORT=/dev/ttyUSB0 (or PORT=COM3 on Windows))
endif

build:
	$(ARDUINO_CLI) compile \
		--fqbn $(FQBN) \
		--build-path $(BUILD_DIR) \
		$(SKETCH_DIR)

flash: check-port
	$(ARDUINO_CLI) upload \
		--fqbn $(FQBN) \
		--port $(PORT) \
		--input-dir $(BUILD_DIR) \
		--upload-property upload.speed=$(UPLOAD_BAUD) \
		$(SKETCH_DIR)

monitor: check-port
	$(ARDUINO_CLI) monitor --port $(PORT) --config baudrate=$(MONITOR_BAUD)

clean:
	rm -rf $(BUILD_DIR)
