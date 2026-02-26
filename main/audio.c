#include "audio.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "audio";
static adc_continuous_handle_t s_adc_handle = NULL;
static TaskHandle_t s_notify_task = NULL;

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
        int32_t sample = ((int32_t)data - 2048) << 4;
        out_buf[count++] = (int16_t)sample;
    }

    *out_samples = count;
    return ESP_OK;
}
