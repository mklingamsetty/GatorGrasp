#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERVO_GPIO      18
#define SERVO_FREQ_HZ   50
#define SERVO_RES_BITS  LEDC_TIMER_14_BIT
#define SERVO_US_MIN    1000
#define SERVO_US_MAX    2000

void servo_angle_set(int angle) {
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;

    uint32_t pulse_us = SERVO_US_MIN + (SERVO_US_MAX - SERVO_US_MIN) * angle / 180;
    uint32_t duty = (pulse_us * (1 << 14)) / (1000000 / SERVO_FREQ_HZ);

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void app_main() {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RES_BITS,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num   = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&channel);

    while (1) {
        servo_angle_set(0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        servo_angle_set(90);
        vTaskDelay(pdMS_TO_TICKS(1000));
        servo_angle_set(180);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
