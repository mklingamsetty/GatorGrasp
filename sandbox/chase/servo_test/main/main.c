#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"

#define SERVO_PIN       7     // Signal pin on ESP32-S3
#define SERVO_PERIOD_US 20000 // 50Hz (20,000us)
#define SERVO_LOW_ENDPOINT_US  500  // MG995 low-end travel limit
#define SERVO_HIGH_ENDPOINT_US 2500 // MG995 high-end travel limit
#define SERVO_ENDPOINT_DELAY_MS 3000

static uint32_t us_to_duty(uint32_t us)
{
    return (us * (1 << LEDC_TIMER_14_BIT)) / SERVO_PERIOD_US;
}

static void set_servo_pulse_us(ledc_channel_t channel, uint32_t us)
{
    uint32_t duty = us_to_duty(us);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    printf("Sent pulse: %lu us (Duty: %lu)\n", (unsigned long)us, (unsigned long)duty);
}

void app_main(void)
{
    // 1. Configure the PWM Timer (50Hz, 14-bit resolution)
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    // 2. Configure the PWM Channel
    ledc_channel_config_t channel_conf = {
        .gpio_num   = SERVO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&channel_conf);

    // Wait 500ms for servo power to stabilize
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        printf("Moving to the low-end travel limit (%dus)...\n", SERVO_LOW_ENDPOINT_US);
        set_servo_pulse_us(LEDC_CHANNEL_0, SERVO_LOW_ENDPOINT_US);
        vTaskDelay(pdMS_TO_TICKS(SERVO_ENDPOINT_DELAY_MS));

        printf("Moving to the high-end travel limit (%dus)...\n", SERVO_HIGH_ENDPOINT_US);
        set_servo_pulse_us(LEDC_CHANNEL_0, SERVO_HIGH_ENDPOINT_US);
        vTaskDelay(pdMS_TO_TICKS(SERVO_ENDPOINT_DELAY_MS));
    }
}