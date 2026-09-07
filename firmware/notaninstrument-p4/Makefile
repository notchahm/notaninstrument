# Build/flash wrapper around arduino-cli for the ESP32-P4 bring-up sketch.
#
# One-time setup:
#   make setup          # points arduino-cli at the ESP32 board index + installs the core
#
# Day to day:
#   make list-ports     # find your FTDI/UART adapter's COM/tty port
#   make build
#   make flash PORT=/dev/ttyUSB0      (or PORT=COM3 on Windows)
#   make monitor PORT=/dev/ttyUSB0
#   make all PORT=/dev/ttyUSB0        (build + flash + monitor)
#
# Requires arduino-cli on PATH: https://arduino.github.io/arduino-cli/latest/installation/

SKETCH_DIR   := $(CURDIR)
BUILD_DIR    := $(CURDIR)/build

# Board core + FQBN, confirmed against arduino-esp32 core 3.3.11 via
# `arduino-cli board details -b esp32:esp32:esp32p4`. PSRAM defaults to
# disabled on this board definition -- must be enabled explicitly or the
# bring-up sketch's PSRAM check will always report "not detected" even
# though the board has it. FlashSize matches the board's actual 16MB flash
# (default is 4MB). ChipVariant defaults to "Before v3.00" (prev3) but the
# real board flashed 2026-09-06 reported "Chip type: ESP32-P4 (revision
# v3.1)" -- the prev3 default caused an immediate CHIP_LP_WDT_RESET boot
# loop (confirmed via raw serial capture: ROM bootloader repeating every
# ~1s, app code never reached) that ChipVariant=postv3 fixed.
FQBN         := esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=postv3

ESP32_CORE_URL := https://espressif.github.io/arduino-esp32/package_esp32_index.json

# Serial port of the FTDI/UART adapter (or the board's onboard UART bridge).
# Must be passed explicitly -- there's no safe default across machines.
PORT         ?=

UPLOAD_BAUD  ?= 460800
MONITOR_BAUD ?= 115200

ARDUINO_CLI  := arduino-cli

.PHONY: all setup list-ports build flash monitor clean check-port

all: build flash monitor

# Registers the ESP32 board index and installs the esp32 core. Modifies the
# arduino-cli global config (~/.arduino15/arduino-cli.yaml) -- run this
# manually once per machine, not as part of every build.
setup:
	$(ARDUINO_CLI) config init --overwrite=false || true
	$(ARDUINO_CLI) config add board_manager.additional_urls $(ESP32_CORE_URL)
	$(ARDUINO_CLI) core update-index
	$(ARDUINO_CLI) core install esp32:esp32
	$(ARDUINO_CLI) lib install "Adafruit SSD1306" "Adafruit GFX Library"

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
