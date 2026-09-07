#pragma once

// USB-MIDI host on top of ESP-IDF's native USB Host Library
// (usb/usb_host.h), replacing this project's earlier TinyUSB-based
// approach (components/tinyusb_host/, kept vendored as reference/fallback
// but no longer used here) -- see docs/polyphony-latency-investigation.md
// for why: the ~242ms chord-onset lag on the TinyUSB path turned out to
// be TinyUSB's own DWC2 driver stack, confirmed by the same real
// hardware/controller showing 0-11ms chord onset on this library instead.
//
// Handles device enumeration, claims the first MIDIStreaming interface on
// each connected device, and keeps one bulk IN transfer perpetually in
// flight per claimed interface (resubmitted the instant it completes, so
// there's no polling interval of this code's own choosing). Multiple
// simultaneous devices (e.g. two controllers through an external hub,
// confirmed working elsewhere in this project) are handled independently,
// one per USB device address.
//
// Does not distinguish USB-MIDI cable numbers -- every decoded 4-byte
// Event Packet on a claimed endpoint is dispatched to usb_midi_on_event()
// regardless of which embedded MIDI jack it came from. That's fine for
// this project (every note goes to the same voice engine/synth
// regardless of source cable); a device that needs cable-aware routing
// would need that added.

#include <stdint.h>

// Starts USB host library installation and the enumeration/claim pipeline
// on their own FreeRTOS tasks (pinned to core 0, mirroring where the
// TinyUSB host task used to run). Returns once both tasks are created;
// does not block waiting for a device to connect.
void usb_midi_host_start(void);

// Implemented by main.c. Called for every decoded USB-MIDI Event Packet
// (raw MIDI status/data1/data2 bytes) from any claimed MIDIStreaming
// interface. May run on either core, from whichever task's
// usb_host_client_handle_events() call happened to service the
// completed transfer -- keep this fast (same audio-critical-first
// discipline as the old tuh_midi_rx_cb: voice_engine calls before
// display updates).
void usb_midi_on_event(uint8_t status, uint8_t data1, uint8_t data2);
