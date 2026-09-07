#include "nib_loader.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "nib_loader";

// Header/region layout matches tools/sfz_preprocessor/sfz_to_nib.py's
// struct.pack format strings exactly ('<4sIBBBH32sIIH' / '<BBBBBIIIIIII').
// Deliberately NOT read via a C struct cast: several fields land on
// non-4-byte-aligned offsets, and an unaligned uint32_t/uint16_t pointer
// access is undefined behavior / a possible fault depending on the core's
// strict-alignment configuration. Manual little-endian byte reads are
// slower but correct regardless of alignment or compiler struct padding
// choices on either side (Python vs. C).
#define NIB_HEADER_SIZE 55
#define NIB_REGION_SIZE 33
#define COMPRESSION_ADPCM 2

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

bool nib_loader_init(nib_bank_t *bank) {
    memset(bank, 0, sizeof(*bank));

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "soundbank");
    if (partition == NULL) {
        ESP_LOGE(TAG, "no 'soundbank' partition found -- check partitions.csv was actually flashed");
        return false;
    }

    const void *mapped;
    esp_partition_mmap_handle_t mmap_handle;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                        ESP_PARTITION_MMAP_DATA, &mapped, &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        return false;
    }

    const uint8_t *base = (const uint8_t *) mapped;
    if (memcmp(base, "NIB1", 4) != 0) {
        ESP_LOGE(TAG, "bad magic -- partition doesn't contain a valid .nib "
                      "(was it actually flashed? see firmware README)");
        return false;
    }

    bank->sample_rate = read_u32_le(base + 4);
    uint8_t bit_depth = base[8];
    bank->channels = base[9];
    uint8_t compression = base[10];
    bank->region_count = read_u16_le(base + 11);
    memcpy(bank->display_name, base + 13, 32);
    bank->display_name[32] = '\0';
    uint32_t region_table_offset = read_u32_le(base + 45);
    uint32_t sample_data_offset = read_u32_le(base + 49);
    bank->adpcm_block_size = read_u16_le(base + 53);

    if (bit_depth != 16 || compression != COMPRESSION_ADPCM) {
        ESP_LOGE(TAG, "unsupported format: bit_depth=%u compression=%u "
                      "(this loader only handles 16-bit/IMA-ADPCM .nib files -- "
                      "was this built with an old sfz_to_nib.py?)", bit_depth, compression);
        return false;
    }
    if (bank->adpcm_block_size < 5) {
        ESP_LOGE(TAG, "bad adpcm_block_size %u in .nib header", bank->adpcm_block_size);
        return false;
    }
    // Standard IMA ADPCM, mono: 4-byte header (int16 predictor, uint8
    // step index, uint8 reserved) stores the first sample directly, then
    // every remaining byte packs 2 nibble-encoded samples.
    bank->adpcm_samples_per_block = 1 + (bank->adpcm_block_size - 4) * 2;

    // Parsed into a proper heap array rather than left as raw mmap'd
    // bytes -- see the alignment note above. This is small (33 bytes x a
    // few hundred regions at most) and happens once at boot, so a heap
    // allocation + a parse pass per region is not a hot-path concern --
    // unlike the audio itself, which is never bulk-copied or bulk-parsed
    // anywhere; only these small per-region headers are.
    bank->regions = malloc(sizeof(nib_region_t) * bank->region_count);
    if (bank->regions == NULL) {
        ESP_LOGE(TAG, "out of memory allocating %u region entries", bank->region_count);
        return false;
    }

    for (uint16_t i = 0; i < bank->region_count; i++) {
        const uint8_t *r = base + region_table_offset + (size_t) i * NIB_REGION_SIZE;
        nib_region_t *region = &bank->regions[i];
        region->key_lo = r[0];
        region->key_hi = r[1];
        region->vel_lo = r[2];
        region->vel_hi = r[3];
        region->root_key = r[4];
        uint32_t left_offset = read_u32_le(r + 5);
        region->left_length = read_u32_le(r + 9);
        uint32_t right_offset = read_u32_le(r + 13);
        region->right_length = read_u32_le(r + 17);
        region->sample_length = read_u32_le(r + 21);
        region->loop_start = read_u32_le(r + 25);
        region->loop_end = read_u32_le(r + 29);
        region->left_data = base + sample_data_offset + left_offset;
        region->right_data = base + sample_data_offset + right_offset;
    }

    ESP_LOGI(TAG, "loaded '%s': %u regions, %luHz, %u ch, IMA ADPCM (%u B/block), "
                  "mmap'd directly from flash -- no boot-time decode",
             bank->display_name, bank->region_count, (unsigned long) bank->sample_rate,
             bank->channels, bank->adpcm_block_size);
    return true;
}

const nib_region_t *nib_find_region(const nib_bank_t *bank, uint8_t key, uint8_t velocity) {
    for (uint16_t i = 0; i < bank->region_count; i++) {
        const nib_region_t *r = &bank->regions[i];
        if (key >= r->key_lo && key <= r->key_hi && velocity >= r->vel_lo && velocity <= r->vel_hi) {
            return r;
        }
    }
    return NULL;
}
