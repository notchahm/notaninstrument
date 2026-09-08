#pragma once

// Real-time QOA ("Quite OK Audio") decoder for voice_engine.c's render
// path -- an experimental alternative to adpcm_decode.c, not a
// replacement (see tools/sfz_preprocessor/sfz_to_nib.py's --codec flag
// and module docstring). Measured offline on real Salamander piano audio
// at a clear win over this project's IMA ADPCM on both compression ratio
// and SNR, at an equal-or-finer loop-restart granularity -- worth a real
// on-device comparison, kept alongside ADPCM rather than replacing it
// since it has far less real-hardware mileage so far.
//
// QOA's own frame structure (an 8-byte frame header + per-channel LMS
// predictor state + 20-sample slices) gives the same property this
// project's ADPCM blocks rely on: each frame is independently decodable
// from its own stored state, so a loop wrap is an O(1) reset to
// loop_start's frame instead of a replay from track start. Unlike
// upstream QOA (256 slices/frame, ~160ms @ 32kHz -- far coarser than
// this project's loop lengths), sfz_to_nib.py encodes with a much
// smaller frame (16 slices, ~10ms @ 32kHz) specifically for fine-grained
// loop points, and pads every region's audio to an exact multiple of the
// frame length so every frame -- including the last -- is full-size,
// letting this decoder address frames as `frame_index * frame_size_bytes`
// instead of needing to walk variable-size frame headers sequentially.
//
// Bit-exact match to qoa.h's reference decoder (qoa_decode_frame,
// 2-channel) -- this file doesn't vendor qoa.h itself (that's a
// build-machine-only encoder dependency, tools/sfz_preprocessor/qoa/);
// it's a from-scratch, ISR-safe (pure integer, no allocation) real-time
// reimplementation of the same documented algorithm.

#include <stdint.h>

// Named qoa_rt_lms_t (not qoa_lms_t) specifically so this header can
// never collide with the vendored build-machine encoder's own qoa.h
// (tools/sfz_preprocessor/qoa/qoa.h defines a type of that name) --
// they're never compiled together in this project (qoa.h never ships to
// the device), but keeping the names distinct removes any ambiguity for
// anyone cross-referencing the two, and is what caught this decoder's
// standalone-build verification working at all (see the offline
// bit-exact test this was checked against before ever touching real
// hardware).
typedef struct {
    int16_t history[4]; // most recent last, per qoa.h's own convention
    int16_t weights[4];
} qoa_rt_lms_t;

typedef struct {
    const uint8_t *data;         // this region's QOA frame stream (mmap'd flash, region-owned)
    uint16_t frame_size_bytes;   // bytes per frame (uniform across the whole region -- see file comment above)
    uint16_t samples_per_frame;
    uint32_t frame_index;        // which QOA frame the stream is currently positioned in
    uint32_t sample_in_frame;    // next sample to decode within that frame (0..samples_per_frame-1)
    qoa_rt_lms_t lms[2];            // per-channel running predictor state
    uint8_t scalefactor[2];      // current slice's scalefactor per channel (4 bits, re-read every 20 samples)
    uint64_t slice_bits[2];      // remaining (already-left-shifted) bits of the current slice per channel
    int16_t decoded[2][2];       // decoded[ch][0] = sample at frame (have_sample-1), decoded[ch][1] = sample at have_sample
    uint32_t have_sample;        // absolute sample index decoded[*][1] currently holds
} qoa_stream_t;

// Positions the stream so sample `start_sample` and `start_sample + 1`
// are immediately available in decoded[ch][0]/decoded[ch][1] for both
// channels. start_sample MUST be an exact multiple of samples_per_frame
// (true for both of this project's reset points: region start = sample
// 0, and loop_start, which sfz_to_nib.py's find_best_loop_points snaps
// to a frame boundary) -- that's what makes this O(1): it just reads
// that frame's own stored LMS state, never replays from an earlier
// position.
void qoa_stream_reset(qoa_stream_t *s, const uint8_t *data,
                       uint16_t frame_size_bytes, uint16_t samples_per_frame, uint32_t start_sample);

// Ensures decoded[ch][0] == sample at `target_sample` and decoded[ch][1]
// == sample at `target_sample + 1` for both channels, decoding forward
// one sample at a time (crossing frame boundaries by reading each new
// frame's own header/LMS state, and re-reading a fresh slice every 20
// samples) as needed. target_sample must be >= the sample this stream is
// already positioned at -- this only ever moves forward;
// qoa_stream_reset is how callers move backward (used for looping).
//
// target_sample + 1 is allowed to run one sample past this region's real
// audio (the read_voice_frame caller substitutes the real value itself
// in that case, per the loop-seam handling described there) -- decoding
// one sample past a region's own data reads into whatever bytes happen
// to follow in the shared sample blob, which is harmless: still-mapped
// memory, and the resulting garbage sample is never actually used.
void qoa_stream_seek_forward(qoa_stream_t *s, uint32_t target_sample);

// Reads the LMS history/weights stored directly in a frame's header --
// valid only when `sample_index` is itself an exact frame boundary
// (guaranteed for loop_start by sfz_to_nib.py). Used once per note-on,
// mirroring adpcm_decode.h's adpcm_block_header_sample: QOA has no
// single "the sample at this point" shortcut the way IMA ADPCM's header
// does (every QOA sample, including a frame's first, is LMS-predicted,
// not stored directly), so this decodes the frame's very first sample
// from a fresh LMS state to get that cached loop-seam substitution value
// -- still O(1), just a couple of integer ops instead of a raw read.
int16_t qoa_frame_first_sample(const uint8_t *data, uint16_t frame_size_bytes,
                                uint16_t samples_per_frame, uint32_t sample_index, int channel);
