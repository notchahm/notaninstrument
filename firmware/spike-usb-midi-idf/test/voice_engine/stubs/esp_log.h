#pragma once
// Stub for native/host unit testing (test/voice_engine/) -- lets the real
// main/voice_engine.c compile unmodified outside ESP-IDF.
#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
