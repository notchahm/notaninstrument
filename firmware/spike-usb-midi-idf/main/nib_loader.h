#pragma once

// Bring-up step 5 (docs/bring-up-plan.md): loads a .nib soundbank
// (docs/multi-instrument-soundbanks.md), produced offline by
// tools/sfz_preprocessor/sfz_to_nib.py.
//
// The "soundbank" flash partition (partitions.csv) is memory-mapped
// directly (flash is XIP-mappable on the P4) and left that way -- each
// region's audio is stored as IMA ADPCM (two independent mono streams,
// left/right) and decoded live, per-sample, by adpcm_decode.c inside
// voice_engine.c's real-time render path, straight out of the mapped
// flash bytes. No boot-time bulk decode step of any kind: this loader
// only parses the small region table. See sfz_to_nib.py's module
// docstring for why (two earlier designs -- raw PCM on flash, then Ogg
// Vorbis decoded to PSRAM at boot -- were each abandoned for a real,
// hardware-confirmed reason).

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t key_lo, key_hi;   // MIDI key range this region covers
    uint8_t vel_lo, vel_hi;   // velocity range
    uint8_t root_key;         // key the sample was recorded/tuned at
    const uint8_t *left_data;  // raw IMA ADPCM bytes, mmap'd flash, left channel
    uint32_t left_length;      // bytes
    const uint8_t *right_data; // raw IMA ADPCM bytes, mmap'd flash, right channel
    uint32_t right_length;     // bytes
    uint32_t sample_length;   // decoded frames
    uint32_t loop_start;      // decoded-frame index; always an exact multiple of
                               // the bank's adpcm_samples_per_block (see sfz_to_nib.py)
    uint32_t loop_end;        // loop_start==loop_end means no loop
} nib_region_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels; // always 2 for now -- sfz_to_nib.py only emits stereo (as 2 mono ADPCM streams)
    uint16_t region_count;
    uint16_t adpcm_block_size;    // bytes/block, same for every region in this bank
    uint16_t adpcm_samples_per_block; // derived: 1 + (adpcm_block_size - 4) * 2
    char display_name[33];
    nib_region_t *regions; // heap-allocated, parsed copy (see .c for why)
} nib_bank_t;

// Maps the "soundbank" flash partition and parses its header + region
// table. Returns false (bank left zeroed) if the partition is missing,
// too small, or its magic doesn't match -- callers should treat that as
// "no default instrument available" rather than crash.
bool nib_loader_init(nib_bank_t *bank);

// Finds the region covering (key, velocity), or NULL if no region
// matches -- e.g. a key outside every region's range, which can happen
// at the extreme ends of the keyboard depending on how the source
// library covered them.
const nib_region_t *nib_find_region(const nib_bank_t *bank, uint8_t key, uint8_t velocity);
