#pragma once

// Same decoupled timing-diagnostic design as
// spike-usb-midi-idf/main/timing_diag.h, copied here (not shared -- these
// are two separate ESP-IDF projects) so the ~242ms chord-onset-lag
// investigation can be repeated against ESP-IDF's native USB Host Library
// (usb/usb_host.h) instead of TinyUSB, to check whether the lag is a
// TinyUSB-stack artifact or something inherent to the device/bus.
//
// Only ever does a cheap counter increment / struct write in the hot path
// (the bulk-transfer completion callback); the actual ESP_LOGI dumping
// happens on its own low-priority task, decoupled entirely.

#include <stdbool.h>
#include <stdint.h>

// Call on every completed IN transfer, regardless of whether it decoded to
// a note (empty/malformed packets included) -- just increments a counter,
// no I/O.
void timing_diag_raw_callback(void);

// Call when a Note On/Off is decoded. Records the timestamp plus how many
// raw transfer completions (see above) fired since the *previous* recorded
// note.
void timing_diag_record(uint8_t note, bool is_on);

void timing_diag_start_task(void);
