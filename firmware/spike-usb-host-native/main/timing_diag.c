#include "timing_diag.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "timing_diag";

#define RING_SIZE 64
typedef struct {
    int64_t us;
    uint8_t note;
    bool is_on;
    uint32_t raw_callbacks_since_prev;
} timing_entry_t;

static timing_entry_t s_ring[RING_SIZE];
static volatile uint32_t s_write_idx = 0; // written from the transfer completion callback (single writer)
static uint32_t s_read_idx = 0;           // only touched by timing_diag_task
static volatile uint32_t s_raw_callback_count = 0;

void timing_diag_raw_callback(void) {
    s_raw_callback_count++;
}

void timing_diag_record(uint8_t note, bool is_on) {
    timing_entry_t *entry = &s_ring[s_write_idx % RING_SIZE];
    entry->us = esp_timer_get_time();
    entry->note = note;
    entry->is_on = is_on;
    entry->raw_callbacks_since_prev = s_raw_callback_count;
    s_raw_callback_count = 0;
    s_write_idx++;
}

static void timing_diag_task(void *arg) {
    (void) arg;
    int64_t last_us = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t write_snapshot = s_write_idx;
        if (write_snapshot - s_read_idx > RING_SIZE) {
            s_read_idx = write_snapshot - RING_SIZE;
        }
        while (s_read_idx != write_snapshot) {
            const timing_entry_t *entry = &s_ring[s_read_idx % RING_SIZE];
            int64_t delta_ms = (last_us == 0) ? 0 : (entry->us - last_us) / 1000;
            ESP_LOGI(TAG, "note=%3u %s  +%lldms  (%lu raw xfers since prev note)",
                     entry->note, entry->is_on ? "ON " : "OFF", (long long) delta_ms,
                     (unsigned long) entry->raw_callbacks_since_prev);
            last_us = entry->us;
            s_read_idx++;
        }
    }
}

void timing_diag_start_task(void) {
    xTaskCreatePinnedToCore(timing_diag_task, "timing_diag", 4096, NULL, 1, NULL, 1);
}
