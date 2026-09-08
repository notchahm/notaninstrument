// Command-line decoder companion to qoa_encode_cli.c, purpose-built for
// sfz_to_nib.py's find_qoa_raw_prefix_samples() (adaptive per-region
// raw-PCM-prefix search -- see that function's docstring for why this
// exists: measuring whether a candidate prefix width actually keeps
// QOA's attack-transient quantization noise out of the compressed
// portion requires decoding a real candidate encoding and comparing it
// against the clean source, not just guessing from the encoder side).
//
// #includes the real production decoder (../../firmware/spike-usb-midi-idf/
// main/qoa_decode.c) unmodified, the same "never drift from what ships"
// principle the firmware's own test/voice_engine/*.c regression tests
// use -- this tool's whole point is to answer "what would the real
// decoder on real hardware actually produce," so it must run the exact
// same code, not a reimplementation that could silently diverge.
//
// Takes a hybrid region's raw bytes (raw-PCM prefix followed immediately
// by QOA frames, exactly the layout build_nib assembles) and dumps
// decoded interleaved-stereo 16-bit PCM to stdout -- no WAV header,
// since the caller (sfz_to_nib.py) already knows the sample rate/format
// and just wants raw samples for spectral analysis.
#include "../../../firmware/spike-usb-midi-idf/main/qoa_decode.c"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s region_file frame_size_bytes samples_per_frame raw_prefix_samples num_samples\n",
                argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *region_data = malloc((size_t) fsize);
    if (fread(region_data, 1, (size_t) fsize, f) != (size_t) fsize) {
        fprintf(stderr, "short read on %s\n", argv[1]);
        return 2;
    }
    fclose(f);

    uint16_t frame_size_bytes = (uint16_t) atoi(argv[2]);
    uint16_t samples_per_frame = (uint16_t) atoi(argv[3]);
    uint32_t raw_prefix_samples = (uint32_t) atol(argv[4]);
    uint32_t num_samples = (uint32_t) atol(argv[5]);

    const uint8_t *qoa_data = region_data + (size_t) raw_prefix_samples * 2 * sizeof(int16_t);
    qoa_stream_t qoa;
    qoa_stream_reset(&qoa, qoa_data, frame_size_bytes, samples_per_frame, 0);

    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t l, r;
        if (i < raw_prefix_samples) {
            const int16_t *raw = (const int16_t *) region_data;
            l = raw[i * 2];
            r = raw[i * 2 + 1];
        } else {
            qoa_stream_seek_forward(&qoa, i - raw_prefix_samples);
            l = qoa.decoded[0][0];
            r = qoa.decoded[1][0];
        }
        fwrite(&l, 2, 1, stdout);
        fwrite(&r, 2, 1, stdout);
    }
    free(region_data);
    return 0;
}
