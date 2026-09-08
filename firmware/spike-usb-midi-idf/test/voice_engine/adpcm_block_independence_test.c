// ADPCM counterpart to qoa_frame_independence_test.c: IMA ADPCM blocks
// make the same "independently decodable from a stored header" claim
// (see adpcm_decode.h) that voice_engine.c relies on for O(1) loop-wrap.
// No hybrid raw-prefix layer exists on this path (that's QOA-only), so
// there's no known bug here today -- this exists for parity/coverage so
// a future change to the ADPCM addressing gets the same protection.
//
// #includes the real production decoder (not a copy) so this test can
// never silently drift out of sync with what actually ships.
#include "../../main/adpcm_decode.c"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s block_stream_file block_size_bytes samples_per_block\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    uint16_t block_size = (uint16_t) atoi(argv[2]);
    uint16_t samples_per_block = (uint16_t) atoi(argv[3]);

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

    uint32_t num_blocks = (uint32_t) (fsize / block_size);
    if (num_blocks < 2) {
        fprintf(stderr, "fixture too short: only %u block(s)\n", num_blocks);
        return 2;
    }

    int16_t expected[num_blocks];
    adpcm_stream_t seq;
    adpcm_stream_reset(&seq, data, block_size, samples_per_block, 0);
    expected[0] = seq.decoded[0];
    for (uint32_t k = 1; k < num_blocks; k++) {
        uint32_t target = k * samples_per_block;
        adpcm_stream_seek_forward(&seq, target);
        expected[k] = seq.decoded[0];
    }

    int failures = 0;
    for (uint32_t k = 0; k < num_blocks; k++) {
        adpcm_stream_t direct;
        adpcm_stream_reset(&direct, data, block_size, samples_per_block, k * samples_per_block);
        if (direct.decoded[0] != expected[k]) {
            fprintf(stderr, "MISMATCH block %u: sequential=%d direct-reset=%d\n",
                    k, expected[k], direct.decoded[0]);
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d mismatch(es) across %u blocks\n", failures, num_blocks);
        return 1;
    }
    printf("PASS: %u blocks, all independently decodable\n", num_blocks);
    return 0;
}
