#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"

#include "wifi.h"
#include "audio.h"
#include "sdcard.h"
#include "wav.h"
#include "webserver.h"

static const char *TAG = "main";

// --- Recording source ---
typedef enum {
    REC_SOURCE_NONE = 0,
    REC_SOURCE_MANUAL,
    REC_SOURCE_AUTO,
} rec_source_t;

// --- Auto-record states ---
enum { AUTO_IDLE = 0, AUTO_RECORDING };

// Recording state -- protected by mutex
static SemaphoreHandle_t s_rec_mutex;
static volatile bool s_recording = false;
static volatile bool s_rec_request_start = false;
static volatile bool s_rec_request_stop = false;
static FILE *s_wav_file = NULL;
static char s_rec_filename[96];
static char s_rec_start_time[80];
static rec_source_t s_rec_source = REC_SOURCE_NONE;

// Auto-record state -- protected by mutex
static volatile bool     s_auto_mode = false;
static uint16_t          s_auto_threshold = 2000;
static int               s_auto_state = AUTO_IDLE;
static uint32_t          s_silence_chunks = 0;
static volatile uint16_t s_current_rms = 0;

// µ-law compression toggle
static volatile bool s_use_ulaw = false;

// File splitting state
static uint32_t s_samples_written = 0;
static int s_file_part = 1;
static char s_rec_basename[48];  // base name without .wav extension
#define MAX_FILE_SAMPLES (5 * 60 * AUDIO_SAMPLE_RATE)  // 6,000,000

// Smoothed RMS and adaptive noise floor
static float s_rms_smooth = 0;          // fast EMA of per-chunk RMS
static float s_noise_floor = 0;         // slow EMA tracking ambient level
static uint32_t s_loud_streak = 0;      // consecutive loud chunks

// Zero-crossing rate for auto-record
static float s_zcr_smooth = 0;          // smoothed ZCR
static volatile float s_current_zcr = 0;  // exposed to status API
#define ZCR_SMOOTH_ALPHA 0.3f

// EMA coefficients (alpha): higher = more responsive
#define RMS_SMOOTH_ALPHA    0.3f   // ~3 chunks to settle
#define NOISE_FLOOR_ALPHA   0.005f // ~200 chunks (~4s) to settle
// Trigger requires smoothed RMS to exceed BOTH:
//   1) noise_floor * NOISE_MULT (relative to ambient)
//   2) s_auto_threshold (absolute minimum, user-configurable)
#define NOISE_MULT          3.0f
// Need this many consecutive loud chunks to trigger (~100ms)
#define TRIGGER_STREAK      5
// Silence uses lower bar: just below threshold (hysteresis)
#define SILENCE_FRAC        0.7f

// Pre-buffer ring (1 second = 20000 samples @ 20kHz)
#define PRE_BUF_SAMPLES 20000
static int16_t *s_pre_buf = NULL;
static size_t    s_pre_buf_head = 0;
static size_t    s_pre_buf_count = 0;

// Write buffer -- accumulate PCM in PSRAM, flush to SD in larger chunks
#define WRITE_BUF_SAMPLES  8000  // 8000 samples = 16KB = ~400ms @ 20kHz
static int16_t *s_write_buf = NULL;
static size_t s_write_buf_pos = 0;

// Silence timeout: ~2 minutes at 20ms/chunk = 6000 chunks
#define SILENCE_TIMEOUT_CHUNKS 6000

// --- SNTP time sync ---
static void init_sntp(void)
{
    setenv("TZ", "MSK-3", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    // Wait up to 10 seconds for sync
    for (int i = 0; i < 20; i++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(500)) == ESP_OK) {
            time_t now;
            struct tm ti;
            time(&now);
            localtime_r(&now, &ti);
            ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d MSK",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
            return;
        }
    }
    ESP_LOGW(TAG, "SNTP sync timeout, using fallback filenames");
}

// --- Filename generation ---
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

static void generate_rec_filename(void)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year + 1900 >= 2024) {
        snprintf(s_rec_basename, sizeof(s_rec_basename),
                 "%04d-%02d-%02d_%02d-%02d-%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        snprintf(s_rec_filename, sizeof(s_rec_filename), "%s.wav", s_rec_basename);
        snprintf(s_rec_start_time, sizeof(s_rec_start_time),
                 "%04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        int num = next_rec_number();
        snprintf(s_rec_basename, sizeof(s_rec_basename), "rec_%03d", num);
        snprintf(s_rec_filename, sizeof(s_rec_filename), "%s.wav", s_rec_basename);
        s_rec_start_time[0] = '\0';
    }
}

// --- Pre-buffer ring ---
static void pre_buf_write(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        s_pre_buf[s_pre_buf_head] = samples[i];
        s_pre_buf_head = (s_pre_buf_head + 1) % PRE_BUF_SAMPLES;
        if (s_pre_buf_count < PRE_BUF_SAMPLES) s_pre_buf_count++;
    }
}

static void pre_buf_flush_to_wav(void)
{
    if (!s_wav_file || s_pre_buf_count == 0) return;

    // Start reading from oldest sample
    size_t start;
    if (s_pre_buf_count < PRE_BUF_SAMPLES) {
        start = 0;
    } else {
        start = s_pre_buf_head; // head points to oldest when full
    }

    // Flush in one or two segments (ring buffer wrap)
    if (start + s_pre_buf_count <= PRE_BUF_SAMPLES) {
        if (s_use_ulaw)
            wav_write_ulaw(s_wav_file, &s_pre_buf[start], s_pre_buf_count);
        else
            wav_write(s_wav_file, &s_pre_buf[start], s_pre_buf_count);
        s_samples_written += s_pre_buf_count;
    } else {
        size_t first = PRE_BUF_SAMPLES - start;
        if (s_use_ulaw) {
            wav_write_ulaw(s_wav_file, &s_pre_buf[start], first);
            wav_write_ulaw(s_wav_file, &s_pre_buf[0], s_pre_buf_count - first);
        } else {
            wav_write(s_wav_file, &s_pre_buf[start], first);
            wav_write(s_wav_file, &s_pre_buf[0], s_pre_buf_count - first);
        }
        s_samples_written += s_pre_buf_count;
    }

    s_pre_buf_head = 0;
    s_pre_buf_count = 0;
}

// --- Getters for webserver ---
bool main_is_recording(void) { return s_recording; }
const char *main_rec_filename(void) { return s_rec_filename; }
const char *main_rec_start_time(void) { return s_rec_start_time; }
uint16_t main_current_rms(void) { return s_current_rms; }
bool main_auto_mode(void) { return s_auto_mode; }
uint16_t main_auto_threshold(void) { return s_auto_threshold; }
bool main_use_ulaw(void) { return s_use_ulaw; }
void main_set_use_ulaw(bool v) { s_use_ulaw = v; }
float main_current_zcr(void) { return s_current_zcr; }

const char *main_rec_source_str(void)
{
    switch (s_rec_source) {
    case REC_SOURCE_MANUAL: return "manual";
    case REC_SOURCE_AUTO:   return "auto";
    default:                return "none";
    }
}

void main_set_auto_mode(bool enabled)
{
    if (enabled && !s_auto_mode) {
        // Reset adaptive state on fresh enable
        s_noise_floor = 0;
        s_rms_smooth = 0;
        s_zcr_smooth = 0;
        s_loud_streak = 0;
        s_auto_state = AUTO_IDLE;
        s_silence_chunks = 0;
    }
    s_auto_mode = enabled;
}
void main_set_auto_threshold(uint16_t thr)
{
    if (thr < 100) thr = 100;
    if (thr > 10000) thr = 10000;
    s_auto_threshold = thr;
}

// --- Flush write buffer to SD (with file splitting) ---
static void flush_write_buf(void)
{
    if (s_write_buf_pos > 0 && s_wav_file) {
        if (s_use_ulaw)
            wav_write_ulaw(s_wav_file, s_write_buf, s_write_buf_pos);
        else
            wav_write(s_wav_file, s_write_buf, s_write_buf_pos);
        s_samples_written += s_write_buf_pos;
        s_write_buf_pos = 0;

        // File splitting at MAX_FILE_SAMPLES
        if (s_samples_written >= MAX_FILE_SAMPLES) {
            wav_close(s_wav_file);
            s_file_part++;
            snprintf(s_rec_filename, sizeof(s_rec_filename),
                     "%s_p%d.wav", s_rec_basename, s_file_part);
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, s_rec_filename);
            if (s_use_ulaw)
                s_wav_file = wav_open_ulaw(path, AUDIO_SAMPLE_RATE, 1);
            else
                s_wav_file = wav_open(path, AUDIO_SAMPLE_RATE, 16, 1);
            s_samples_written = 0;
            ESP_LOGI(TAG, "File split: now recording %s", s_rec_filename);
        }
    }
}

// --- Start/stop recording helpers (must be called under mutex) ---
static bool start_recording(rec_source_t source)
{
    uint64_t free_space = sdcard_free_bytes();
    if (free_space < 1024 * 1024) {
        ESP_LOGW(TAG, "SD card full, cannot start recording");
        return false;
    }

    generate_rec_filename();
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, s_rec_filename);

    if (s_use_ulaw)
        s_wav_file = wav_open_ulaw(path, AUDIO_SAMPLE_RATE, 1);
    else
        s_wav_file = wav_open(path, AUDIO_SAMPLE_RATE, 16, 1);
    if (!s_wav_file) return false;

    s_recording = true;
    s_rec_source = source;
    s_write_buf_pos = 0;
    s_samples_written = 0;
    s_file_part = 1;
    ESP_LOGI(TAG, "Recording started (%s): %s",
             source == REC_SOURCE_AUTO ? "auto" : "manual", s_rec_filename);
    return true;
}

static void stop_recording(void)
{
    flush_write_buf();
    s_recording = false;
    wav_close(s_wav_file);
    s_wav_file = NULL;
    ESP_LOGI(TAG, "Recording stopped (%s): %s",
             s_rec_source == REC_SOURCE_AUTO ? "auto" : "manual", s_rec_filename);
    s_rec_source = REC_SOURCE_NONE;
}

// --- WebSocket command callback (runs in httpd task context) ---
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

// --- Audio pipeline task -- pinned to core 1 ---
static void audio_pipeline_task(void *arg)
{
    const size_t max_samples = AUDIO_READ_LEN / 2;  // 400 samples
    int16_t *pcm_buf = heap_caps_malloc(max_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    s_write_buf = heap_caps_malloc(WRITE_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_write_buf) {
        ESP_LOGE(TAG, "Failed to allocate write buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    s_pre_buf = heap_caps_malloc(PRE_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_pre_buf) {
        ESP_LOGE(TAG, "Failed to allocate pre-buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(audio_start());
    ESP_LOGI(TAG, "Audio pipeline running on core %d", xPortGetCoreID());

    int space_check_count = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        while (1) {
            size_t num_samples = 0;
            esp_err_t ret = audio_read(pcm_buf, &num_samples);
            if (ret != ESP_OK || num_samples == 0) break;

            // 1. Compute RMS and ZCR (before mutex)
            float sum_sq = 0;
            int zc = 0;
            for (size_t i = 0; i < num_samples; i++) {
                float s = (float)pcm_buf[i];
                sum_sq += s * s;
                if (i > 0 && ((pcm_buf[i] > 0) != (pcm_buf[i-1] > 0))) zc++;
            }
            s_current_rms = (uint16_t)sqrtf(sum_sq / num_samples);
            float zcr = (num_samples > 1) ? (float)zc / (num_samples - 1) : 0;
            s_current_zcr = zcr;

            // 2. Broadcast to WebSocket (before mutex)
            webserver_broadcast_audio(pcm_buf, num_samples);

            // 3. Take mutex
            xSemaphoreTake(s_rec_mutex, portMAX_DELAY);

            // 4. Handle manual start/stop requests
            if (s_rec_request_start) {
                s_rec_request_start = false;
                // Manual stop of any auto-recording first
                if (s_recording && s_rec_source == REC_SOURCE_AUTO) {
                    stop_recording();
                    s_auto_state = AUTO_IDLE;
                }
                if (!s_recording) {
                    start_recording(REC_SOURCE_MANUAL);
                    space_check_count = 0;
                }
            }

            if (s_rec_request_stop && s_recording) {
                s_rec_request_stop = false;
                stop_recording();
                // If was auto-recording, reset auto state
                s_auto_state = AUTO_IDLE;
                s_silence_chunks = 0;
            }

            // 5. Auto-record state machine (if enabled and no manual rec)
            if (s_auto_mode && s_rec_source != REC_SOURCE_MANUAL) {
                float rms = (float)s_current_rms;

                // Update smoothed RMS (fast EMA)
                s_rms_smooth += RMS_SMOOTH_ALPHA * (rms - s_rms_smooth);
                // Update smoothed ZCR
                s_zcr_smooth += ZCR_SMOOTH_ALPHA * (s_current_zcr - s_zcr_smooth);

                // Trigger threshold = max(user threshold, noise_floor * multiplier)
                float trig_level = (float)s_auto_threshold;
                float noise_trig = s_noise_floor * NOISE_MULT;
                if (noise_trig > trig_level) trig_level = noise_trig;

                // Silence threshold with hysteresis (lower than trigger)
                float silence_level = trig_level * SILENCE_FRAC;

                // Combined trigger: high energy AND low ZCR (not white noise)
                bool loud = (s_rms_smooth >= trig_level) && (s_zcr_smooth < 0.40f);
                bool quiet = (s_rms_smooth < silence_level);

                switch (s_auto_state) {
                case AUTO_IDLE:
                    // Update adaptive noise floor (slow EMA, only in IDLE)
                    s_noise_floor += NOISE_FLOOR_ALPHA * (rms - s_noise_floor);

                    // Feed pre-buffer
                    pre_buf_write(pcm_buf, num_samples);

                    if (loud) {
                        s_loud_streak++;
                    } else {
                        s_loud_streak = 0;
                    }

                    // Require sustained loud signal to trigger
                    if (s_loud_streak >= TRIGGER_STREAK) {
                        ESP_LOGI(TAG, "Auto-trigger: rms=%.0f noise=%.0f trig=%.0f zcr=%.2f",
                                 s_rms_smooth, s_noise_floor, trig_level, s_zcr_smooth);
                        if (start_recording(REC_SOURCE_AUTO)) {
                            pre_buf_flush_to_wav();
                            s_auto_state = AUTO_RECORDING;
                            s_silence_chunks = 0;
                            s_loud_streak = 0;
                            space_check_count = 0;
                        }
                    }
                    break;

                case AUTO_RECORDING:
                    if (!quiet) {
                        s_silence_chunks = 0;
                    } else {
                        s_silence_chunks++;
                    }
                    if (s_silence_chunks >= SILENCE_TIMEOUT_CHUNKS) {
                        ESP_LOGI(TAG, "Auto-record: 2 min silence, stopping");
                        stop_recording();
                        s_auto_state = AUTO_IDLE;
                        s_silence_chunks = 0;
                        s_loud_streak = 0;
                    }
                    break;
                }
            } else if (!s_auto_mode && s_rec_source == REC_SOURCE_AUTO) {
                // Auto-mode disabled mid-recording: stop
                ESP_LOGI(TAG, "Auto-mode disabled, stopping auto-recording");
                stop_recording();
                s_auto_state = AUTO_IDLE;
                s_silence_chunks = 0;
                s_loud_streak = 0;
                s_noise_floor = 0;
                s_rms_smooth = 0;
                s_zcr_smooth = 0;
            }

            // 6. Buffer audio to SD if recording (any source)
            if (s_recording && s_wav_file) {
                size_t to_copy = num_samples;
                if (s_write_buf_pos + to_copy > WRITE_BUF_SAMPLES) {
                    to_copy = WRITE_BUF_SAMPLES - s_write_buf_pos;
                }
                memcpy(&s_write_buf[s_write_buf_pos], pcm_buf, to_copy * sizeof(int16_t));
                s_write_buf_pos += to_copy;

                if (s_write_buf_pos >= WRITE_BUF_SAMPLES) {
                    flush_write_buf();
                }

                if (++space_check_count >= 250) {
                    space_check_count = 0;
                    if (sdcard_free_bytes() < 512 * 1024) {
                        ESP_LOGW(TAG, "SD card nearly full, stopping recording");
                        stop_recording();
                        if (s_auto_state == AUTO_RECORDING) {
                            s_auto_state = AUTO_IDLE;
                            s_silence_chunks = 0;
                        }
                    }
                }
            }

            // 7. Release mutex
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

    // Sync time via SNTP
    ESP_LOGI(TAG, "Syncing time via SNTP...");
    init_sntp();

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
    xTaskCreatePinnedToCore(audio_pipeline_task, "audio_pipe", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System ready!");
}
