#pragma once

#include "esp_err.h"
#include "esp_adc/adc_continuous.h"

#define AUDIO_SAMPLE_RATE   20000
#define AUDIO_READ_LEN      800   // bytes per ADC read (400 samples * 2 bytes)

// Initialize the ADC continuous driver on ADC1_CH0 @ 20kHz.
esp_err_t audio_init(void);

// Start ADC conversions.
esp_err_t audio_start(void);

// Stop ADC conversions.
esp_err_t audio_stop(void);

// Read ADC data and convert to 16-bit signed PCM.
// out_buf must hold at least AUDIO_READ_LEN bytes (will contain int16_t samples).
// out_samples receives the number of int16_t samples written.
// Returns ESP_OK on success, ESP_ERR_TIMEOUT if no data available.
esp_err_t audio_read(int16_t *out_buf, size_t *out_samples);
