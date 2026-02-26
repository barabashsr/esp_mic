#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "wifi.h"
#include "audio.h"
#include "sdcard.h"
#include "wav.h"
#include "webserver.h"

static const char *TAG = "main";

// Recording state — protected by mutex
static SemaphoreHandle_t s_rec_mutex;
static volatile bool s_recording = false;
static volatile bool s_rec_request_start = false;
static volatile bool s_rec_request_stop = false;
static FILE *s_wav_file = NULL;
static char s_rec_filename[32];

// Write buffer — accumulate PCM in PSRAM, flush to SD in larger chunks
#define WRITE_BUF_SAMPLES  8000  // 8000 samples = 16KB = ~400ms @ 20kHz
static int16_t *s_write_buf = NULL;
static size_t s_write_buf_pos = 0;  // current position in samples

// Getters for webserver status endpoint
bool main_is_recording(void) { return s_recording; }
const char *main_rec_filename(void) { return s_rec_filename; }

// Scan existing rec_NNN.wav files and return the next number
static int next_rec_number(void)
{
    int max_num = 0;
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) return 1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int n;
        if (sscanf(ent->d_name, "rec_%d.wav", &n) == 1) {
            if (n > max_num) max_num = n;
        }
    }
    closedir(dir);
    return max_num + 1;
}

// WebSocket command callback (runs in httpd task context)
static void on_ws_command(const char *cmd)
{
    xSemaphoreTake(s_rec_mutex, portMAX_DELAY);
    if (strcmp(cmd, "start_rec") == 0 && !s_recording) {
        s_rec_request_start = true;
    } else if (strcmp(cmd, "stop_rec") == 0 && s_recording) {
        s_rec_request_stop = true;
    }
    xSemaphoreGive(s_rec_mutex);
}

// Flush write buffer to SD
static void flush_write_buf(void)
{
    if (s_write_buf_pos > 0 && s_wav_file) {
        wav_write(s_wav_file, s_write_buf, s_write_buf_pos);
        s_write_buf_pos = 0;
    }
}

// Audio pipeline task — pinned to core 1
static void audio_pipeline_task(void *arg)
{
    // Allocate PCM buffer in PSRAM
    const size_t max_samples = AUDIO_READ_LEN / 2;  // 400 samples
    int16_t *pcm_buf = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    // Allocate write buffer in PSRAM
    s_write_buf = heap_caps_malloc(WRITE_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_write_buf) {
        ESP_LOGE(TAG, "Failed to allocate write buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(audio_start());
    ESP_LOGI(TAG, "Audio pipeline running on core %d", xPortGetCoreID());

    static int space_check_count = 0;

    while (1) {
        // Wait for ADC data notification
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        // Read all available ADC data
        while (1) {
            size_t num_samples = 0;
            esp_err_t ret = audio_read(pcm_buf, &num_samples);
            if (ret != ESP_OK || num_samples == 0) break;

            // Broadcast to WebSocket clients
            webserver_broadcast_audio(pcm_buf, num_samples);

            // Handle recording state changes
            xSemaphoreTake(s_rec_mutex, portMAX_DELAY);

            if (s_rec_request_start) {
                s_rec_request_start = false;
                int num = next_rec_number();
                snprintf(s_rec_filename, sizeof(s_rec_filename), "rec_%03d.wav", num);
                char path[64];
                snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, s_rec_filename);

                // Check free space (need at least 1MB)
                uint64_t free_space = sdcard_free_bytes();
                if (free_space < 1024 * 1024) {
                    ESP_LOGW(TAG, "SD card full, cannot start recording");
                } else {
                    s_wav_file = wav_open(path, AUDIO_SAMPLE_RATE, 16, 1);
                    if (s_wav_file) {
                        s_recording = true;
                        s_write_buf_pos = 0;
                        space_check_count = 0;
                        ESP_LOGI(TAG, "Recording started: %s", s_rec_filename);
                    }
                }
            }

            if (s_rec_request_stop && s_recording) {
                s_rec_request_stop = false;
                flush_write_buf();
                s_recording = false;
                wav_close(s_wav_file);
                s_wav_file = NULL;
                ESP_LOGI(TAG, "Recording stopped: %s", s_rec_filename);
            }

            // Buffer audio for recording
            if (s_recording && s_wav_file) {
                // Copy samples into write buffer
                size_t to_copy = num_samples;
                if (s_write_buf_pos + to_copy > WRITE_BUF_SAMPLES) {
                    to_copy = WRITE_BUF_SAMPLES - s_write_buf_pos;
                }
                memcpy(&s_write_buf[s_write_buf_pos], pcm_buf, to_copy * sizeof(int16_t));
                s_write_buf_pos += to_copy;

                // Flush when buffer is full
                if (s_write_buf_pos >= WRITE_BUF_SAMPLES) {
                    flush_write_buf();
                }

                // Check SD space every ~5 seconds
                if (++space_check_count >= 250) {
                    space_check_count = 0;
                    if (sdcard_free_bytes() < 512 * 1024) {
                        ESP_LOGW(TAG, "SD card nearly full, stopping recording");
                        flush_write_buf();
                        s_recording = false;
                        wav_close(s_wav_file);
                        s_wav_file = NULL;
                    }
                }
            }

            xSemaphoreGive(s_rec_mutex);
        }
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Recording mutex
    s_rec_mutex = xSemaphoreCreateMutex();

    // Connect WiFi
    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(wifi_init_sta());

    // Mount SD card
    ESP_LOGI(TAG, "Mounting SD card...");
    ESP_ERROR_CHECK(sdcard_init());

    // Initialize ADC
    ESP_LOGI(TAG, "Initializing audio...");
    ESP_ERROR_CHECK(audio_init());

    // Start web server
    ESP_LOGI(TAG, "Starting web server...");
    ESP_ERROR_CHECK(webserver_start(on_ws_command));

    // Launch audio pipeline on core 1
    xTaskCreatePinnedToCore(audio_pipeline_task, "audio_pipe", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready!");
}
