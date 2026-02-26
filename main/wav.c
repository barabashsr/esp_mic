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
