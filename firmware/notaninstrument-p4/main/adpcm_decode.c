#include "adpcm_decode.h"

#include <stddef.h>

// Standard IMA ADPCM tables -- copied verbatim from the vendored
// tools/sfz_preprocessor/adpcm-xq/adpcm-lib.c so this decoder is
// bit-exact with what encoded the data.
static const uint16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

static const int8_t INDEX_TABLE[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static inline int16_t block_header_sample(const uint8_t *block) {
    return (int16_t) (block[0] | (block[1] << 8));
}

// Decodes exactly one more frame beyond wherever the stream is
// positioned, advancing block_index/nibble_pos/predictor/step_index as
// needed, and returns that frame's sample. Does not touch decoded[]/
// have_frame -- callers own shifting those (reset and seek_forward do it
// differently: reset seeds decoded[0] directly from a block header
// without going through this at all).
static int16_t decode_next_raw_sample(adpcm_stream_t *s) {
    uint32_t nibbles_in_block = (uint32_t) s->samples_per_block - 1;

    if (s->nibble_pos >= nibbles_in_block) {
        // Crossed into the next block -- its header directly stores the
        // next sample and resets the running predictor/step state, per
        // the standard IMA ADPCM block format (no nibble decode needed
        // for a block's first sample).
        s->block_index++;
        const uint8_t *block = s->data + (size_t) s->block_index * s->block_size;
        int16_t predictor = block_header_sample(block);
        s->predictor = predictor;
        s->step_index = block[2];
        s->nibble_pos = 0;
        return predictor;
    }

    const uint8_t *payload = s->data + (size_t) s->block_index * s->block_size + 4;
    uint8_t byte = payload[s->nibble_pos >> 1];
    // Low nibble decodes first, matching adpcm-lib.c's adpcm_decode_block
    // (its first inner-loop sample uses *inbuf's low bits, the second
    // uses *inbuf's high bits, before advancing to the next byte).
    uint8_t nibble = (s->nibble_pos & 1) ? (byte >> 4) : (byte & 0x0F);
    s->nibble_pos++;

    int32_t step = STEP_TABLE[s->step_index];
    int32_t delta = step >> 3;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 4) delta += step;

    if (nibble & 8) {
        s->predictor -= delta;
    } else {
        s->predictor += delta;
    }
    if (s->predictor > 32767) s->predictor = 32767;
    else if (s->predictor < -32768) s->predictor = -32768;

    s->step_index += INDEX_TABLE[nibble & 0x07];
    if (s->step_index < 0) s->step_index = 0;
    else if (s->step_index > 88) s->step_index = 88;

    return (int16_t) s->predictor;
}

void adpcm_stream_reset(adpcm_stream_t *s, const uint8_t *data,
                         uint16_t block_size, uint16_t samples_per_block, uint32_t start_frame) {
    s->data = data;
    s->block_size = block_size;
    s->samples_per_block = samples_per_block;
    s->block_index = start_frame / samples_per_block;
    s->nibble_pos = 0;

    const uint8_t *block = s->data + (size_t) s->block_index * s->block_size;
    int16_t predictor = block_header_sample(block);
    s->predictor = predictor;
    s->step_index = block[2];
    s->decoded[0] = predictor;
    s->have_frame = start_frame;

    s->decoded[1] = decode_next_raw_sample(s);
    s->have_frame = start_frame + 1;
}

void adpcm_stream_seek_forward(adpcm_stream_t *s, uint32_t target_frame) {
    while (s->have_frame < target_frame + 1) {
        s->decoded[0] = s->decoded[1];
        s->decoded[1] = decode_next_raw_sample(s);
        s->have_frame++;
    }
}

int16_t adpcm_block_header_sample(const uint8_t *data, uint16_t block_size,
                                   uint16_t samples_per_block, uint32_t frame_index) {
    uint32_t block_index = frame_index / samples_per_block;
    return block_header_sample(data + (size_t) block_index * block_size);
}
