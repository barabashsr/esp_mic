#include "audio.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "audio";
static adc_continuous_handle_t s_adc_handle = NULL;
static TaskHandle_t s_notify_task = NULL;

// --- Configurable biquad filters ---

typedef struct {
    float b0, b1, b2, a1, a2;  // coefficients
    float z1, z2;                // state (direct form II transposed)
    bool enabled;
} biquad_t;

static biquad_t s_hp = {0};  // high-pass
static biquad_t s_lp = {0};  // low-pass
static uint16_t s_hp_freq = 0;  // 0 = disabled
static uint16_t s_lp_freq = 0;  // 0 = disabled

static void biquad_compute_lp(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * 0.7071068f);  // Q = 1/sqrt(2)
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - cosw0) / 2.0f / a0;
    f->b1 = (1.0f - cosw0) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw0 / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = 0; f->z2 = 0;
    f->enabled = true;
}

static void biquad_compute_hp(biquad_t *f, float fc, float fs)
{
    float w0 = 2.0f * M_PI * fc / fs;
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * 0.7071068f);  // Q = 1/sqrt(2)
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f + cosw0) / 2.0f / a0;
    f->b1 = -(1.0f + cosw0) / a0;
    f->b2 = f->b0;
    f->a1 = -2.0f * cosw0 / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = 0; f->z2 = 0;
    f->enabled = true;
}

static inline float biquad_process(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

void audio_set_filter(uint16_t hp_freq, uint16_t lp_freq)
{
    s_hp_freq = hp_freq;
    s_lp_freq = lp_freq;
    if (hp_freq > 0) {
        biquad_compute_hp(&s_hp, (float)hp_freq, (float)AUDIO_SAMPLE_RATE);
    } else {
        s_hp.enabled = false;
        s_hp.z1 = 0; s_hp.z2 = 0;
    }
    if (lp_freq > 0) {
        biquad_compute_lp(&s_lp, (float)lp_freq, (float)AUDIO_SAMPLE_RATE);
    } else {
        s_lp.enabled = false;
        s_lp.z1 = 0; s_lp.z2 = 0;
    }
    ESP_LOGI(TAG, "Filter set: HP=%u Hz, LP=%u Hz", hp_freq, lp_freq);
}

uint16_t audio_get_hp_freq(void) { return s_hp_freq; }
uint16_t audio_get_lp_freq(void) { return s_lp_freq; }

static bool IRAM_ATTR conv_done_cb(adc_continuous_handle_t handle,
                                    const adc_continuous_evt_data_t *edata,
                                    void *user_data)
{
    BaseType_t must_yield = pdFALSE;
    if (s_notify_task) {
        vTaskNotifyGiveFromISR(s_notify_task, &must_yield);
    }
    return (must_yield == pdTRUE);
}

static volatile uint32_t s_pool_ovf_count = 0;

static bool IRAM_ATTR pool_ovf_cb(adc_continuous_handle_t handle,
                                    const adc_continuous_evt_data_t *edata,
                                    void *user_data)
{
    s_pool_ovf_count++;
    return false;
}

uint32_t audio_get_overflow_count(void) { return s_pool_ovf_count; }

esp_err_t audio_init(void)
{
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 4096,
        .conv_frame_size = AUDIO_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &s_adc_handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = AUDIO_SAMPLE_RATE,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };

    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN_DB_12,
        .channel = ADC_CHANNEL_0,
        .unit = ADC_UNIT_1,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = &adc_pattern;

    ESP_ERROR_CHECK(adc_continuous_config(s_adc_handle, &dig_cfg));

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = conv_done_cb,
        .on_pool_ovf = pool_ovf_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(s_adc_handle, &cbs, NULL));

    ESP_LOGI(TAG, "ADC initialized: CH0 @ %d Hz", AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_start(void)
{
    s_notify_task = xTaskGetCurrentTaskHandle();
    return adc_continuous_start(s_adc_handle);
}

esp_err_t audio_stop(void)
{
    s_notify_task = NULL;
    return adc_continuous_stop(s_adc_handle);
}

esp_err_t audio_read(int16_t *out_buf, size_t *out_samples)
{
    uint8_t raw[AUDIO_READ_LEN];
    uint32_t ret_num = 0;

    esp_err_t ret = adc_continuous_read(s_adc_handle, raw, AUDIO_READ_LEN, &ret_num, 0);
    if (ret != ESP_OK) {
        *out_samples = 0;
        return ret;
    }

    // On ESP32, each ADC result is 2 bytes (SOC_ADC_DIGI_RESULT_BYTES = 2)
    size_t count = 0;
    for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)&raw[i];
        uint32_t data = p->type1.data;  // 12-bit unsigned [0..4095]

        // Convert 12-bit unsigned (centered at ~2048) to 16-bit signed PCM
        float sample = (float)(((int32_t)data - 2048) << 4);

        // Apply optional filters
        if (s_hp.enabled) sample = biquad_process(&s_hp, sample);
        if (s_lp.enabled) sample = biquad_process(&s_lp, sample);

        int32_t out = (int32_t)sample;
        if (out > 32767) out = 32767;
        if (out < -32768) out = -32768;
        out_buf[count++] = (int16_t)out;
    }

    *out_samples = count;
    return ESP_OK;
}
