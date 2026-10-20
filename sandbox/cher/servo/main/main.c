#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_err.h"

// Servo configuration
#define SERVO_GPIO          18
#define SERVO_FREQUENCY     50
#define SERVO_TIMER         LEDC_TIMER_0
#define SERVO_CHANNEL       LEDC_CHANNEL_0
#define SERVO_MODE          LEDC_LOW_SPEED_MODE
#define SERVO_RESOLUTION    LEDC_TIMER_13_BIT

#define SERVO_PERIOD_US     20000U
#define DUTY_STEPS          (1U << 13)

#define SERVO_MIN_PULSE_US  1000U
#define SERVO_MAX_PULSE_US  2000U

static void set_servo_pulse_us(uint32_t pulse_us) {
    if (pulse_us < SERVO_MIN_PULSE_US) pulse_us = SERVO_MIN_PULSE_US;
    if (pulse_us > SERVO_MAX_PULSE_US) pulse_us = SERVO_MAX_PULSE_US;

    uint32_t duty = (pulse_us * DUTY_STEPS + SERVO_PERIOD_US / 2) / SERVO_PERIOD_US;

    ESP_ERROR_CHECK(ledc_set_duty(SERVO_MODE, SERVO_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(SERVO_MODE, SERVO_CHANNEL));
}

static void set_servo_angle(uint32_t angle_deg) {
    if (angle_deg > 180) angle_deg = 180;

    uint32_t pulse_us = SERVO_MIN_PULSE_US +
        (angle_deg * (SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US)) / 180;

    set_servo_pulse_us(pulse_us);
}

static void init_servo(void) {
    ledc_timer_config_t timer_config = {
        .speed_mode      = SERVO_MODE,
        .timer_num       = SERVO_TIMER,
        .duty_resolution = SERVO_RESOLUTION,
        .freq_hz         = SERVO_FREQUENCY,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    ledc_channel_config_t channel_config = {
        .gpio_num   = SERVO_GPIO,
        .speed_mode = SERVO_MODE,
        .channel    = SERVO_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SERVO_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));

    set_servo_angle(90);
    vTaskDelay(pdMS_TO_TICKS(500));
}

void app_main(void) {
    printf("Starting servo test!\n");

    init_servo();

    while (1) {
        for (uint32_t angle = 0; angle <= 180; angle += 2) {
            set_servo_angle(angle);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(500));

        for (int angle = 180; angle >= 0; angle -= 2) {
            set_servo_angle((uint32_t)angle);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}