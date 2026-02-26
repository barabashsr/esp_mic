#include "wav.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"

static const char *TAG = "wav";

// Standard 44-byte WAV header for PCM
typedef struct __attribute__((packed)) {
    char     riff_tag[4];       // "RIFF"
    uint32_t riff_size;         // file size - 8
    char     wave_tag[4];       // "WAVE"
    char     fmt_tag[4];        // "fmt "
    uint32_t fmt_size;          // 16 for PCM
    uint16_t audio_format;      // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;         // sample_rate * num_channels * bits/8
    uint16_t block_align;       // num_channels * bits/8
    uint16_t bits_per_sample;
    char     data_tag[4];       // "data"
    uint32_t data_size;         // num samples * num_channels * bits/8
} wav_header_t;

FILE *wav_open(const char *path, int sample_rate, int bits_per_sample, int channels)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return NULL;
    }

    wav_header_t hdr;
    memcpy(hdr.riff_tag, "RIFF", 4);
    hdr.riff_size = 0;  // placeholder, fixed on close
    memcpy(hdr.wave_tag, "WAVE", 4);
    memcpy(hdr.fmt_tag, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.num_channels = channels;
    hdr.sample_rate = sample_rate;
    hdr.bits_per_sample = bits_per_sample;
    hdr.block_align = channels * bits_per_sample / 8;
    hdr.byte_rate = sample_rate * hdr.block_align;
    memcpy(hdr.data_tag, "data", 4);
    hdr.data_size = 0;  // placeholder, fixed on close

    fwrite(&hdr, sizeof(hdr), 1, f);
    ESP_LOGI(TAG, "Opened WAV: %s (%d Hz, %d-bit, %d ch)", path, sample_rate, bits_per_sample, channels);
    return f;
}

// ITU-T G.711 µ-law encoding: 16-bit linear PCM → 8-bit µ-law
static uint8_t linear_to_ulaw(int16_t sample)
{
    const int16_t BIAS = 0x84;   // 132
    const int16_t CLIP = 32635;
    uint8_t sign = 0;
    if (sample < 0) {
        sign = 0x80;
        sample = -sample;
    }
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;

    // Find segment (exponent)
    int seg = 0;
    int shifted = sample >> 7;
    while (shifted > 0) {
        seg++;
        shifted >>= 1;
    }
    // Build µ-law byte: sign(1) | exponent(3) | mantissa(4)
    uint8_t uval = (uint8_t)(sign | ((uint8_t)seg << 4) |
                              ((sample >> (seg + 3)) & 0x0F));
    return ~uval;  // complement per standard
}

FILE *wav_open_ulaw(const char *path, int sample_rate, int channels)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return NULL;
    }

    wav_header_t hdr;
    memcpy(hdr.riff_tag, "RIFF", 4);
    hdr.riff_size = 0;
    memcpy(hdr.wave_tag, "WAVE", 4);
    memcpy(hdr.fmt_tag, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 7;  // µ-law
    hdr.num_channels = channels;
    hdr.sample_rate = sample_rate;
    hdr.bits_per_sample = 8;
    hdr.block_align = channels * 1;  // 8-bit = 1 byte per sample
    hdr.byte_rate = sample_rate * hdr.block_align;
    memcpy(hdr.data_tag, "data", 4);
    hdr.data_size = 0;

    fwrite(&hdr, sizeof(hdr), 1, f);
    ESP_LOGI(TAG, "Opened WAV (µ-law): %s (%d Hz, 8-bit, %d ch)", path, sample_rate, channels);
    return f;
}

size_t wav_write_ulaw(FILE *f, const int16_t *samples, size_t num_samples)
{
    if (!f || !samples || num_samples == 0) return 0;
    // Encode in chunks to avoid large stack allocation
    uint8_t ubuf[256];
    size_t total = 0;
    while (total < num_samples) {
        size_t chunk = num_samples - total;
        if (chunk > sizeof(ubuf)) chunk = sizeof(ubuf);
        for (size_t i = 0; i < chunk; i++) {
            ubuf[i] = linear_to_ulaw(samples[total + i]);
        }
        total += fwrite(ubuf, 1, chunk, f);
    }
    return total;
}

size_t wav_write(FILE *f, const int16_t *samples, size_t num_samples)
{
    if (!f || !samples || num_samples == 0) return 0;
    return fwrite(samples, sizeof(int16_t), num_samples, f);
}

void wav_close(FILE *f)
{
    if (!f) return;

    long file_size = ftell(f);
    uint32_t data_size = file_size - sizeof(wav_header_t);
    uint32_t riff_size = file_size - 8;

    // Fix RIFF size at offset 4
    fseek(f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, f);

    // Fix data size at offset 40
    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);

    fclose(f);
    ESP_LOGI(TAG, "WAV closed: %ld bytes total, %"PRIu32" bytes PCM data", file_size, data_size);
}
