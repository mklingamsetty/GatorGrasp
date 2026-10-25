#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_err.h"
#include "dl_model_base.hpp"

static const char *TAG = "ESP_DL";

extern const uint8_t model_weights_espdl[] asm("_binary_model_weights_espdl_start");

extern "C" void app_main(void)
{
    auto *model = new dl::Model(
        reinterpret_cast<const char *>(model_weights_espdl),
        fbs::MODEL_LOCATION_IN_FLASH_RODATA
    );

    ESP_LOGI(TAG, "Model loaded");

    esp_err_t ret = model->test();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Model test passed");
    } else {
        ESP_LOGE(TAG, "Model test failed: %s", esp_err_to_name(ret));
    }

    model->profile_memory();
    model->profile_module();
}
