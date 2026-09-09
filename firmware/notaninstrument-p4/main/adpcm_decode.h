#pragma once

// Real-time IMA ADPCM decoder for voice_engine.c's render path -- see
// tools/sfz_preprocessor/sfz_to_nib.py's module docstring for why ADPCM
// (not Ogg Vorbis, not raw PCM) is what makes this possible: standard
// IMA ADPCM is block-structured (each block stores its own predictor/
// step-index header, decodable independently of every other block) and
// each sample's decode is a few integer ops against a running per-stream
// state -- no frequency-domain transform, no float, and (the property
// that actually matters here) fully compatible with this project's
// access pattern: strictly-forward, variable-rate (pitch-shifted)
// playback that only ever needs to jump backward at exactly one point --
// the loop wrap -- which sfz_to_nib.py guarantees lands on a block
// boundary specifically so that jump is an O(1) reset to that block's
// own header, not a replay from track start.
//
// Bit-exact match to the vendored tools/sfz_preprocessor/adpcm-xq's
// reference decoder (adpcm-lib.c's adpcm_decode_block, bps=4, mono) --
// adpcm-xq's "lookahead"/noise-shaping cleverness is entirely on the
// encoder side; the decode algorithm itself is completely standard IMA
// ADPCM, verified byte-for-byte against that reference implementation.

#include <stdint.h>

typedef struct {
    const uint8_t *data;   // this channel's compressed bytes (mmap'd flash, region-owned)
    uint16_t block_size;   // bytes/block (from the bank header, same for every region)
    uint16_t samples_per_block;
    uint32_t block_index;  // which block the stream is currently positioned in
    uint32_t nibble_pos;   // nibble-samples consumed in the current block beyond its header sample
    int32_t predictor;     // current running predictor (also decoded[1] mirrored as int16)
    int32_t step_index;    // current running step-table index (0..88)
    int16_t decoded[2];    // decoded[0] = sample at frame (have_frame-1), decoded[1] = sample at frame have_frame
    uint32_t have_frame;   // frame index decoded[1] currently holds
} adpcm_stream_t;

// Positions the stream so frame `start_frame` and `start_frame + 1` are
// immediately available in decoded[0]/decoded[1]. start_frame MUST be an
// exact multiple of samples_per_block (true for both of this project's
// reset points: region start = frame 0, and loop_start, which
// sfz_to_nib.py snaps to a block boundary at encode time specifically so
// this holds) -- that's what makes this O(1): it just reads that block's
// own stored header, never replays from an earlier position.
void adpcm_stream_reset(adpcm_stream_t *s, const uint8_t *data,
                         uint16_t block_size, uint16_t samples_per_block, uint32_t start_frame);

// Ensures decoded[0] == sample at `target_frame` and decoded[1] ==
// sample at `target_frame + 1`, decoding forward one frame at a time
// (crossing block boundaries by reading each new block's own header) as
// needed. target_frame must be >= the frame this stream is already
// positioned at -- this only ever moves forward; adpcm_stream_reset is
// how callers move backward (used for looping).
//
// target_frame + 1 is allowed to run one frame past this region's real
// audio (the read_voice_frame caller substitutes the real value itself
// in that case, per the loop-seam handling described there) -- decoding
// one frame past a region's own data reads into whatever bytes happen to
// follow in the shared sample blob (the next channel's or region's data,
// or padding), which is harmless: still-mapped memory, and the resulting
// garbage sample is never actually used.
void adpcm_stream_seek_forward(adpcm_stream_t *s, uint32_t target_frame);

// Reads the sample stored directly in a block's header -- valid only
// when `frame_index` is itself an exact block boundary (guaranteed for
// loop_start by sfz_to_nib.py). Used once per note-on to cache the
// loop-seam substitution value (see voice_engine.c's read_voice_frame) --
// cheaper than keeping a second decode stream permanently parked at
// loop_start just to read one fixed, never-changing value.
int16_t adpcm_block_header_sample(const uint8_t *data, uint16_t block_size,
                                   uint16_t samples_per_block, uint32_t frame_index);
