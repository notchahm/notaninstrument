#pragma once

// Bring-up step 5 (docs/bring-up-plan.md): loads a .nib soundbank
// (docs/multi-instrument-soundbanks.md), produced offline by
// tools/sfz_preprocessor/sfz_to_nib.py.
//
// The "soundbank" flash partition (partitions.csv) is memory-mapped
// directly (flash is XIP-mappable on the P4) and left that way -- each
// region's audio is compressed (IMA ADPCM, the primary/shipping codec, or
// experimentally QOA -- see NIB_COMPRESSION_* and sfz_to_nib.py's --codec
// flag) and decoded live, per-sample, by adpcm_decode.c or qoa_decode.c
// inside voice_engine.c's real-time render path, straight out of the
// mapped flash bytes. No boot-time bulk decode step of any kind: this
// loader only parses the small region table. See sfz_to_nib.py's module
// docstring for why not raw PCM or Ogg Vorbis (two earlier designs, each
// abandoned for a real, hardware-confirmed reason).

#include <stdbool.h>
#include <stdint.h>

// ADPCM stores 2 independent mono streams per region (left_data/right_data
// both meaningful). QOA stores one genuinely interleaved-stereo stream
// (only left_data/left_length meaningful; right_data is NULL, right_length
// is 0) -- see sfz_to_nib.py's build_nib for why (QOA supports true
// multi-channel natively; the raw IMA ADPCM format this project uses does
// not, without reimplementing Microsoft's stereo nibble-interleaving).
typedef enum {
    NIB_COMPRESSION_ADPCM = 2,
    NIB_COMPRESSION_QOA = 3,
} nib_compression_t;

typedef struct {
    uint8_t key_lo, key_hi;   // MIDI key range this region covers
    uint8_t vel_lo, vel_hi;   // velocity range
    uint8_t root_key;         // key the sample was recorded/tuned at
    const uint8_t *left_data;  // compressed bytes, mmap'd flash -- left channel (ADPCM) or the only stream (QOA)
    uint32_t left_length;      // bytes
    const uint8_t *right_data; // compressed bytes, mmap'd flash, right channel -- ADPCM only; NULL for QOA
    uint32_t right_length;     // bytes; 0 for QOA
    uint32_t sample_length;   // decoded frames
    uint32_t loop_start;      // decoded-frame index; always an exact multiple of the bank's
                               // codec_samples_per_unit (see sfz_to_nib.py's find_best_loop_points)
    uint32_t loop_end;        // loop_start==loop_end means no loop
    // QOA only (0 for ADPCM regions): length of this region's raw,
    // uncompressed-PCM prefix -- see voice_engine.c's qoa_compressed_data()
    // comment for the hybrid layout this measures into. Per-region, not a
    // shared bank-wide constant: sfz_to_nib.py measures each note's own
    // transient length and sizes its raw window just wide enough to keep
    // QOA's attack-transient quantization noise out of the compressed
    // portion (a hard hit's transient needs a much wider raw window than
    // a soft one -- confirmed by direct measurement, see that tool's
    // find_qoa_raw_prefix_samples()) rather than over- or under-sizing a
    // single fixed width for every note.
    uint32_t qoa_raw_prefix_samples;
} nib_region_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;   // always 2 for now
    nib_compression_t compression;
    uint16_t region_count;
    // Generalized across codecs -- bytes/block for ADPCM, bytes/frame for
    // QOA; codec_samples_per_unit is the matching decoded-frame count
    // (derived per-codec: adpcm_samples_per_block()-style math for ADPCM,
    // just the QOA frame's own known sample count for QOA).
    uint16_t codec_block_size;
    uint16_t codec_samples_per_unit;
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
