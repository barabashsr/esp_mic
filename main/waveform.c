#include "waveform.h"
#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waveform";

static int16_t ulaw_decode(uint8_t u)
{
    u = ~u;
    int sign = (u & 0x80) ? -1 : 1;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int magnitude = ((mantissa << 3) + 0x84) << exponent;
    magnitude -= 0x84;
    return (int16_t)(sign * magnitude);
}

static void cache_path_for(const char *wav_filename, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s.bin", WAVEFORM_CACHE_DIR, wav_filename);
}

bool waveform_has_cache(const char *wav_filename)
{
    char path[280];
    cache_path_for(wav_filename, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t waveform_read_cache(const char *wav_filename, uint16_t peaks[WAVEFORM_BINS])
{
    char path[280];
    cache_path_for(wav_filename, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    size_t got = fread(peaks, sizeof(uint16_t), WAVEFORM_BINS, f);
    fclose(f);
    return (got == WAVEFORM_BINS) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void waveform_delete_cache(const char *wav_filename)
{
    char path[280];
    cache_path_for(wav_filename, path, sizeof(path));
    unlink(path);
}

esp_err_t waveform_generate(const char *wav_filename)
{
    char wav_path[280];
    snprintf(wav_path, sizeof(wav_path), "%s/%s", SD_MOUNT_POINT, wav_filename);

    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", wav_path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t hdr[44];
    if (file_size < 44 || fread(hdr, 1, 44, f) != 44) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t audio_format   = hdr[20] | (hdr[21] << 8);
    uint16_t bits_per_sample = hdr[34] | (hdr[35] << 8);
    uint16_t block_align    = hdr[32] | (hdr[33] << 8);
    uint32_t data_size      = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24);

    bool is_ulaw  = (audio_format == 7 && bits_per_sample == 8);
    bool is_pcm16 = (audio_format == 1 && bits_per_sample == 16);
    if (!is_ulaw && !is_pcm16) {
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (data_size == 0 && file_size > 44) {
        data_size = (uint32_t)(file_size - 44);
    }

    uint32_t total_samples = data_size / block_align;
    int bins = WAVEFORM_BINS;
    if (bins > (int)total_samples) bins = (int)total_samples;
    if (bins < 1) bins = 1;

    uint32_t samples_per_bin = total_samples / bins;

    uint16_t peaks[WAVEFORM_BINS];
    memset(peaks, 0, sizeof(peaks));

    uint8_t chunk[512];
    for (int b = 0; b < bins; b++) {
        uint32_t offset = 44 + (uint32_t)b * samples_per_bin * block_align;
        fseek(f, offset, SEEK_SET);
        uint32_t remaining = samples_per_bin;
        uint16_t peak = 0;

        while (remaining > 0) {
            uint32_t to_read = remaining * block_align;
            if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
            size_t got = fread(chunk, 1, to_read, f);
            if (got == 0) break;

            uint32_t n = got / block_align;
            for (uint32_t i = 0; i < n; i++) {
                int16_t sample;
                if (is_ulaw) {
                    sample = ulaw_decode(chunk[i * block_align]);
                } else {
                    sample = (int16_t)(chunk[i * block_align] | (chunk[i * block_align + 1] << 8));
                }
                uint16_t abs_val = (sample < 0) ? -sample : sample;
                if (abs_val > peak) peak = abs_val;
            }
            remaining -= n;
        }
        peaks[b] = peak;
    }
    fclose(f);

    // Ensure cache directory exists
    mkdir(WAVEFORM_CACHE_DIR, 0755);

    // Write cache file
    char cache_path[280];
    cache_path_for(wav_filename, cache_path, sizeof(cache_path));

    FILE *cf = fopen(cache_path, "wb");
    if (!cf) {
        ESP_LOGW(TAG, "cannot write cache %s", cache_path);
        return ESP_FAIL;
    }
    fwrite(peaks, sizeof(uint16_t), WAVEFORM_BINS, cf);
    fclose(cf);

    ESP_LOGI(TAG, "generated cache for %s", wav_filename);
    return ESP_OK;
}

static void waveform_bg_task(void *arg)
{
    ESP_LOGI(TAG, "background cache task started");

    // Ensure cache directory exists
    mkdir(WAVEFORM_CACHE_DIR, 0755);

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "cannot open SD for scan");
        vTaskDelete(NULL);
        return;
    }

    int generated = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *ext = strrchr(ent->d_name, '.');
        if (!ext || strcasecmp(ext, ".wav") != 0) continue;

        if (!waveform_has_cache(ent->d_name)) {
            ESP_LOGI(TAG, "generating cache for %s", ent->d_name);
            waveform_generate(ent->d_name);
            generated++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "background cache task done, generated %d", generated);
    vTaskDelete(NULL);
}

void waveform_start_bg_task(void)
{
    xTaskCreatePinnedToCore(waveform_bg_task, "wf_cache", 4096, NULL, 2, NULL, 0);
}
