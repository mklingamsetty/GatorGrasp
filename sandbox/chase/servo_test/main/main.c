#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"

#define SERVO_PIN       4   // Safe GPIO pin on the ESP32-S3

// MG995 uses a 50Hz (20,000us) PWM period.
// With 14-bit resolution (max duty = 16383):
// 0.5ms (0 deg)   = (500 / 20000) * 16384  = ~410
// 1.5ms (90 deg)  = (1500 / 20000) * 16384 = ~1229
// 2.5ms (180 deg) = (2500 / 20000) * 16384 = ~2048
#define SERVO_MIN_DUTY  410
#define SERVO_MID_DUTY  1229
#define SERVO_MAX_DUTY  2048

static void set_servo_duty(uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void app_main(void)
{
    // 1. Configure the PWM Timer (50Hz, 14-bit resolution)
    ledc_timer_config_t timer_conf = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_14_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 50,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    // 2. Configure the PWM Channel (Attach Timer to GPIO 4)
    ledc_channel_config_t channel_conf = {
        .gpio_num       = SERVO_PIN,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&channel_conf);

    // 3. Sweep between 0, 90, and 180 degrees
    while (1) {
        printf("Moving to 0 degrees\n");
        set_servo_duty(SERVO_MIN_DUTY);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("Moving to 90 degrees\n");
        set_servo_duty(SERVO_MID_DUTY);
        vTaskDelay(pdMS_TO_TICKS(1500));

        printf("Moving to 180 degrees\n");
        set_servo_duty(SERVO_MAX_DUTY);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}