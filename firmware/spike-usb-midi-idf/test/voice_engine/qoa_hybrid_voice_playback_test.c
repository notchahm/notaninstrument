// Regression test for the actual bug: voice_engine.c's advance_voice()
// QOA loop-wrap reset used to pass the region's raw data pointer and an
// unadjusted (non-QOA-relative) sample index to qoa_stream_reset(),
// instead of qoa_compressed_data(region) and
// (loop_start - qoa_raw_prefix_samples()) like every other QOA call site
// in that file uses. qoa_frame_independence_test.c (in this same
// directory) proves qoa_decode.c's own reset/seek primitives are
// internally consistent, but that test never exercises voice_engine.c's
// hybrid-layout addressing arithmetic at all -- this one does, by
// mirroring voice_engine.c's read_voice_frame()/advance_voice() logic
// (phase_inc pinned to exactly 1.0 so there's no interpolation to reason
// about: every output sample is exactly one decoded PCM frame) against a
// synthetic fixture whose loop region is a pure periodic tone, and
// checking that decoded output actually repeats with the loop's period.
// Wrong addressing decodes a different (or garbage) frame every wrap,
// which this periodicity check catches directly -- no ear required.
//
// KEEP THIS MIRRORED LOGIC IN SYNC with voice_engine.c's
// qoa_raw_prefix_samples()/qoa_compressed_data()/read_voice_frame()/
// advance_voice() -- if that addressing changes, update this file's
// mirror of it too, or this test stops meaning anything.
//
// #includes the real production decoder (not a copy).
#include "../../main/qoa_decode.c"

#include <stdio.h>
#include <stdlib.h>

#define PHASE_FRAC_BITS 12
#define PHASE_ONE (1u << PHASE_FRAC_BITS)

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s region_file frame_size_bytes samples_per_frame "
                "raw_prefix_samples loop_start_abs loop_end_abs total_output_samples\n",
                argv[0]);
        return 2;
    }
    const char *path = argv[1];
    uint16_t frame_size_bytes = (uint16_t) atoi(argv[2]);
    uint16_t samples_per_frame = (uint16_t) atoi(argv[3]);
    uint32_t raw_prefix_samples = (uint32_t) atol(argv[4]);
    uint32_t loop_start_abs = (uint32_t) atol(argv[5]);
    uint32_t loop_end_abs = (uint32_t) atol(argv[6]);
    uint32_t total_output_samples = (uint32_t) atol(argv[7]);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *region_data = malloc((size_t) fsize);
    if (fread(region_data, 1, (size_t) fsize, f) != (size_t) fsize) {
        fprintf(stderr, "short read on %s\n", path);
        return 2;
    }
    fclose(f);

    // Mirrors voice_engine.c's qoa_compressed_data(): the QOA stream only
    // ever addresses the post-raw-prefix span.
    const uint8_t *qoa_data = region_data + (size_t) raw_prefix_samples * 2 * sizeof(int16_t);
    uint32_t loop_start_qoa_relative = loop_start_abs - raw_prefix_samples;

    qoa_stream_t qoa;
    qoa_stream_reset(&qoa, qoa_data, frame_size_bytes, samples_per_frame, 0);

    uint32_t loop_start_fixed = loop_start_abs << PHASE_FRAC_BITS;
    uint32_t loop_end_fixed = loop_end_abs << PHASE_FRAC_BITS;
    uint32_t phase = 0;
    // phase_inc pinned to exactly PHASE_ONE: idx advances by exactly 1
    // native sample per output sample, and frac is always 0, so
    // read_voice_frame's interpolation collapses to "output l0 exactly"
    // -- removes interpolation from what this test needs to reason about.
    const uint32_t phase_inc = PHASE_ONE;

    FILE *out = stdout;
    for (uint32_t n = 0; n < total_output_samples; n++) {
        uint32_t idx = phase >> PHASE_FRAC_BITS;

        int16_t l0, r0;
        if (idx < raw_prefix_samples) {
            const int16_t *raw = (const int16_t *) region_data;
            l0 = raw[idx * 2];
            r0 = raw[idx * 2 + 1];
        } else {
            qoa_stream_seek_forward(&qoa, idx - raw_prefix_samples);
            l0 = qoa.decoded[0][0];
            r0 = qoa.decoded[1][0];
        }
        fwrite(&l0, 2, 1, out);
        fwrite(&r0, 2, 1, out);

        // advance_voice(), loop-wrap branch, WITH the fix.
        uint32_t new_phase = phase + phase_inc;
        if (new_phase >= loop_end_fixed) {
            uint32_t loop_len = loop_end_fixed - loop_start_fixed;
            new_phase = loop_start_fixed + (new_phase - loop_start_fixed) % loop_len;
            qoa_stream_reset(&qoa, qoa_data, frame_size_bytes, samples_per_frame, loop_start_qoa_relative);
        }
        phase = new_phase;
    }
    return 0;
}
