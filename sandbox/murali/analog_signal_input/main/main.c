#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADC_UNIT        ADC_UNIT_1
#define ADC_CHANNEL     ADC_CHANNEL_6
#define ADC_ATTENUATION ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH   ADC_BITWIDTH_DEFAULT

static const char *TAG = "analog_input";

void app_main(void)
{
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &adc_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BIT_WIDTH,
        .atten = ADC_ATTENUATION,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        adc_handle, ADC_CHANNEL, &channel_config));

    adc_cali_handle_t calibration_handle = NULL;
    adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT,
        .chan = ADC_CHANNEL,
        .atten = ADC_ATTENUATION,
        .bitwidth = ADC_BIT_WIDTH,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(
        &calibration_config, &calibration_handle));

    while (1) {
        int raw_value;
        int voltage_mv;

        esp_err_t read_error = adc_oneshot_read(
            adc_handle, ADC_CHANNEL, &raw_value);
        if (read_error != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read ADC: %s", esp_err_to_name(read_error));
        } else {
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(
                calibration_handle, raw_value, &voltage_mv));
            ESP_LOGI(TAG, "GPIO12 voltage: %d mV (%.3f V)",
                     voltage_mv, voltage_mv / 1000.0f);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}