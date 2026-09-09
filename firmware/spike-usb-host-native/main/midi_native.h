#pragma once

// Minimal, timing-focused USB-MIDI class driver on top of ESP-IDF's native
// USB Host Library (usb/usb_host.h), written specifically to answer one
// question for docs/polyphony-latency-investigation.md: is the ~242ms
// chord-onset delay seen on the TinyUSB path (notaninstrument-p4) a
// TinyUSB-stack artifact, or does the same delay show up here too, on a
// completely different host stack talking to the same hardware/devices?
//
// Deliberately not a general MIDI class driver: finds the first
// Audio-class/MIDIStreaming-subclass interface's bulk (or interrupt) IN
// endpoint, claims it, and keeps exactly one IN transfer perpetually
// in flight (resubmitted the instant the previous one completes) --
// there's no polling interval of our own choosing to confound the
// measurement.

#include "usb/usb_host.h"

// Called once per newly opened device, after its config descriptor has
// been fetched. Scans for a MIDIStreaming interface; if found, claims it
// and starts the perpetual IN-transfer loop. No-op (logs and returns) if
// the device has no such interface.
void midi_native_try_claim(usb_host_client_handle_t client_hdl,
                            usb_device_handle_t dev_hdl,
                            const usb_config_desc_t *config_desc);
