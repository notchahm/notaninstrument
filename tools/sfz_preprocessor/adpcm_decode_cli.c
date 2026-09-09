// One-off verification CLI, same discipline as qoa/qoa_decode_cli.c:
// #includes the real production decoder unmodified so a manual sanity
// check of a freshly-built .nib (e.g. muldjordkit_to_nib.py's fadeout
// handling on truncated cymbal/tom hits) exercises the exact code that
// will run on real hardware, not a reimplementation that could diverge.
//
// Takes one channel's raw ADPCM bytes and dumps decoded mono int16 PCM to
// stdout (no WAV header) for the caller to inspect directly.
#include "../../firmware/notaninstrument-p4/main/adpcm_decode.c"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s channel_bytes_file block_size_bytes samples_per_block num_samples\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t) fsize);
    if (fread(data, 1, (size_t) fsize, f) != (size_t) fsize) { fprintf(stderr, "short read\n"); return 2; }
    fclose(f);

    uint16_t block_size = (uint16_t) atoi(argv[2]);
    uint16_t samples_per_block = (uint16_t) atoi(argv[3]);
    uint32_t num_samples = (uint32_t) atol(argv[4]);

    adpcm_stream_t s;
    adpcm_stream_reset(&s, data, block_size, samples_per_block, 0);
    for (uint32_t i = 0; i < num_samples; i++) {
        adpcm_stream_seek_forward(&s, i);
        int16_t sample = s.decoded[0];
        fwrite(&sample, sizeof(sample), 1, stdout);
    }
    return 0;
}
