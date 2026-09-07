// Bring-up step 1 (docs/bring-up-plan.md): toolchain smoke test, not the
// real firmware. Blinks an LED and logs chip/PSRAM info over serial so we
// can confirm the board enumerates and PSRAM reports ~32MB before building
// anything else on top of it.

// TBD: exact onboard LED GPIO is unconfirmed for the Waveshare
// ESP32-P4-WIFI6-DEV-KIT (see docs/hardware-bom.md, "Pin assignments").
// GPIO2 is the common default on many ESP32 dev boards but has NOT been
// verified against this board's schematic/silkscreen yet. Update this once
// confirmed, and record it in docs/hardware-bom.md.
#define LED_PIN 2

#define SERIAL_BAUD 115200
#define BLINK_INTERVAL_MS 500
// Repeats the boot banner periodically, not just once at boot -- a serial
// monitor attaching even a second late (the common case: reset happens
// before a host-side reader has opened the port) would otherwise never see
// chip/PSRAM info at all.
#define STATUS_INTERVAL_MS 10000

static bool led_state = false;
static unsigned long last_toggle_ms = 0;
static unsigned long last_status_ms = 0;

static void print_status_banner() {
  Serial.println();
  Serial.println("=== notaninstrument P4 bring-up ===");
  Serial.printf("Chip model:      %s\n", ESP.getChipModel());
  Serial.printf("Chip revision:   %d\n", ESP.getChipRevision());
  Serial.printf("CPU cores:       %d\n", ESP.getChipCores());
  Serial.printf("CPU freq:        %lu MHz\n", (unsigned long)ESP.getCpuFreqMHz());
  Serial.printf("Flash size:      %lu bytes\n", (unsigned long)ESP.getFlashChipSize());

  if (psramFound()) {
    Serial.printf("PSRAM size:      %lu bytes (expect ~32MB per hardware-bom.md)\n",
                  (unsigned long)ESP.getPsramSize());
  } else {
    Serial.println("PSRAM size:      NOT DETECTED -- check board config "
                    "(PSRAM mode/menu option) before proceeding past bring-up step 1");
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(SERIAL_BAUD);
  unsigned long wait_start = millis();
  while (!Serial && millis() - wait_start < 3000) {
    // give a USB-CDC/UART bridge time to enumerate; don't hang forever
  }

  print_status_banner();
  Serial.println("Blinking LED_PIN, logging a tick every toggle...");
}

void loop() {
  unsigned long now = millis();
  if (now - last_toggle_ms >= BLINK_INTERVAL_MS) {
    last_toggle_ms = now;
    led_state = !led_state;
    digitalWrite(LED_PIN, led_state ? HIGH : LOW);
    Serial.printf("[%lu ms] tick, led=%s\n", now, led_state ? "ON" : "OFF");
  }
  if (now - last_status_ms >= STATUS_INTERVAL_MS) {
    last_status_ms = now;
    print_status_banner();
  }
}
