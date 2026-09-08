#include "qoa_decode.h"

#include <stddef.h>

// QOA constants/tables -- copied verbatim from the vendored
// tools/sfz_preprocessor/qoa/qoa.h so this decoder is bit-exact with
// what encoded the data. QOA_LMS_LEN(4)/slice length(20) are fixed by
// the format spec, not tunable.
#define QOA_LMS_LEN 4
#define QOA_SLICE_LEN 20

static const int QOA_DEQUANT_TAB[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

static inline int16_t read_i16_be(const uint8_t *p) {
    return (int16_t) ((p[0] << 8) | p[1]);
}

static inline uint64_t read_u64_be(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static inline int qoa_clamp_s16(int v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

static int qoa_lms_predict(const qoa_rt_lms_t *lms) {
    int prediction = 0;
    for (int i = 0; i < QOA_LMS_LEN; i++) {
        prediction += (int) lms->weights[i] * (int) lms->history[i];
    }
    return prediction >> 13;
}

static void qoa_lms_update(qoa_rt_lms_t *lms, int sample, int residual) {
    int delta = residual >> 4;
    for (int i = 0; i < QOA_LMS_LEN; i++) {
        lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
    }
    for (int i = 0; i < QOA_LMS_LEN - 1; i++) {
        lms->history[i] = lms->history[i + 1];
    }
    lms->history[QOA_LMS_LEN - 1] = (int16_t) sample;
}

// Byte layout within one frame (see qoa_decode.h and qoa.h's own format
// comment): 8-byte frame header, then per-channel LMS state (4 history +
// 4 weights, 2 bytes each = 16 bytes/channel -- 32 bytes total for
// stereo), then slices.
#define QOA_FRAME_HEADER_SIZE 8
#define QOA_LMS_STATE_SIZE_STEREO (QOA_LMS_LEN * 4 * 2) // 2 fields x LEN x 2 bytes, x 2 channels
#define QOA_SLICES_BASE_OFFSET (QOA_FRAME_HEADER_SIZE + QOA_LMS_STATE_SIZE_STEREO)

static void load_lms_state(qoa_stream_t *s, uint32_t frame_index) {
    const uint8_t *p = s->data + (size_t) frame_index * s->frame_size_bytes + QOA_FRAME_HEADER_SIZE;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < QOA_LMS_LEN; i++) {
            s->lms[ch].history[i] = read_i16_be(p);
            p += 2;
        }
        for (int i = 0; i < QOA_LMS_LEN; i++) {
            s->lms[ch].weights[i] = read_i16_be(p);
            p += 2;
        }
    }
}

static void load_slices(qoa_stream_t *s) {
    uint32_t group = s->sample_in_frame / QOA_SLICE_LEN;
    const uint8_t *frame_base = s->data + (size_t) s->frame_index * s->frame_size_bytes;
    const uint8_t *slices_base = frame_base + QOA_SLICES_BASE_OFFSET;
    for (int ch = 0; ch < 2; ch++) {
        const uint8_t *p = slices_base + (size_t) (group * 2 + ch) * 8;
        uint64_t slice = read_u64_be(p);
        s->scalefactor[ch] = (uint8_t) ((slice >> 60) & 0xf);
        s->slice_bits[ch] = slice << 4;
    }
}

// Decodes exactly one more sample beyond wherever the stream is
// positioned (both channels), advancing frame/slice bookkeeping and LMS
// state as needed. Does not touch decoded[]/have_sample -- callers own
// shifting those, same split as adpcm_decode.c's decode_next_raw_sample.
static void advance_and_get_sample(qoa_stream_t *s, int16_t out[2]) {
    if (s->sample_in_frame >= s->samples_per_frame) {
        s->frame_index++;
        s->sample_in_frame = 0;
        load_lms_state(s, s->frame_index);
    }
    if (s->sample_in_frame % QOA_SLICE_LEN == 0) {
        load_slices(s);
    }

    for (int ch = 0; ch < 2; ch++) {
        int quantized = (int) ((s->slice_bits[ch] >> 61) & 0x7);
        int predicted = qoa_lms_predict(&s->lms[ch]);
        int dequantized = QOA_DEQUANT_TAB[s->scalefactor[ch]][quantized];
        int reconstructed = qoa_clamp_s16(predicted + dequantized);
        out[ch] = (int16_t) reconstructed;
        s->slice_bits[ch] <<= 3;
        qoa_lms_update(&s->lms[ch], reconstructed, dequantized);
    }
    s->sample_in_frame++;
}

void qoa_stream_reset(qoa_stream_t *s, const uint8_t *data,
                       uint16_t frame_size_bytes, uint16_t samples_per_frame, uint32_t start_sample) {
    s->data = data;
    s->frame_size_bytes = frame_size_bytes;
    s->samples_per_frame = samples_per_frame;
    s->frame_index = start_sample / samples_per_frame;
    s->sample_in_frame = 0;
    load_lms_state(s, s->frame_index);

    int16_t sample0[2], sample1[2];
    advance_and_get_sample(s, sample0); // sample_in_frame starts at 0 -- load_slices fires naturally
    advance_and_get_sample(s, sample1);
    for (int ch = 0; ch < 2; ch++) {
        s->decoded[ch][0] = sample0[ch];
        s->decoded[ch][1] = sample1[ch];
    }
    s->have_sample = start_sample + 1;
}

void qoa_stream_seek_forward(qoa_stream_t *s, uint32_t target_sample) {
    while (s->have_sample < target_sample + 1) {
        int16_t next[2];
        advance_and_get_sample(s, next);
        for (int ch = 0; ch < 2; ch++) {
            s->decoded[ch][0] = s->decoded[ch][1];
            s->decoded[ch][1] = next[ch];
        }
        s->have_sample++;
    }
}

int16_t qoa_frame_first_sample(const uint8_t *data, uint16_t frame_size_bytes,
                                uint16_t samples_per_frame, uint32_t sample_index, int channel) {
    qoa_stream_t tmp;
    tmp.data = data;
    tmp.frame_size_bytes = frame_size_bytes;
    tmp.samples_per_frame = samples_per_frame;
    tmp.frame_index = sample_index / samples_per_frame;
    tmp.sample_in_frame = 0;
    load_lms_state(&tmp, tmp.frame_index);
    int16_t sample0[2];
    advance_and_get_sample(&tmp, sample0);
    return sample0[channel];
}
