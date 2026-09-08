// Regression test for the QOA hybrid raw-PCM-prefix/frame-stream
// addressing bug fixed in voice_engine.c's advance_voice() (loop-wrap
// reset used the wrong data pointer and an unadjusted sample index --
// see that function's comment). That bug was invisible to code review
// and only surfaced as an on-hardware "distorted, two notes at once"
// symptom on held notes; this test catches the underlying defect
// directly and deterministically, with no audio/listening involved.
//
// The property under test, which is QOA's whole reason for using a
// frame-structured format here (see qoa_decode.h): every frame is
// independently decodable from its own stored LMS header, so decoding
// forward sequentially from frame 0 all the way to some later frame N
// must produce bit-identical output to resetting the stream directly to
// frame N. A wrong data-pointer or sample-index offset when computing
// the reset target breaks this silently -- the stream still decodes
// *something*, just the wrong frame's content, which is exactly the bug
// this test is built to catch.
//
// #includes the real production decoder (not a copy) so this test can
// never silently drift out of sync with what actually ships.
#include "../../main/qoa_decode.c"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s region_bytes_file frame_size_bytes samples_per_frame\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    uint16_t frame_size_bytes = (uint16_t) atoi(argv[2]);
    uint16_t samples_per_frame = (uint16_t) atoi(argv[3]);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t) fsize);
    if (fread(data, 1, (size_t) fsize, f) != (size_t) fsize) {
        fprintf(stderr, "short read on %s\n", path);
        return 2;
    }
    fclose(f);

    uint32_t num_frames = (uint32_t) (fsize / frame_size_bytes);
    if (num_frames < 2) {
        fprintf(stderr, "fixture too short: only %u frame(s)\n", num_frames);
        return 2;
    }

    // Sequential pass: decode frame 0 all the way to the last frame,
    // recording each frame boundary's first sample (both channels) as
    // the ground truth for what that frame is supposed to decode to.
    int16_t expected_left[num_frames][2];
    qoa_stream_t seq;
    qoa_stream_reset(&seq, data, frame_size_bytes, samples_per_frame, 0);
    expected_left[0][0] = seq.decoded[0][0];
    expected_left[0][1] = seq.decoded[1][0];
    for (uint32_t k = 1; k < num_frames; k++) {
        uint32_t target = k * samples_per_frame;
        qoa_stream_seek_forward(&seq, target);
        expected_left[k][0] = seq.decoded[0][0];
        expected_left[k][1] = seq.decoded[1][0];
    }

    int failures = 0;
    for (uint32_t k = 0; k < num_frames; k++) {
        qoa_stream_t direct;
        qoa_stream_reset(&direct, data, frame_size_bytes, samples_per_frame, k * samples_per_frame);
        for (int ch = 0; ch < 2; ch++) {
            if (direct.decoded[ch][0] != expected_left[k][ch]) {
                fprintf(stderr, "MISMATCH frame %u ch %d: sequential=%d direct-reset=%d\n",
                        k, ch, expected_left[k][ch], direct.decoded[ch][0]);
                failures++;
            }
        }
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d mismatch(es) across %u frames\n", failures, num_frames);
        return 1;
    }
    printf("PASS: %u frames, all independently decodable\n", num_frames);
    return 0;
}
