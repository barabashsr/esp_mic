#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define WAVEFORM_BINS      64
#define WAVEFORM_CACHE_DIR "/sdcard/.waveforms"

// Generate 64-bin peaks for a WAV file and save to cache.
esp_err_t waveform_generate(const char *wav_filename);

// Read cached peaks. Returns ESP_OK if cache exists, fills peaks[64].
esp_err_t waveform_read_cache(const char *wav_filename, uint16_t peaks[WAVEFORM_BINS]);

// Delete cache file for a WAV file.
void waveform_delete_cache(const char *wav_filename);

// Check if cache exists for a WAV file.
bool waveform_has_cache(const char *wav_filename);

// Background task: scans all WAV files, generates missing caches.
void waveform_start_bg_task(void);
