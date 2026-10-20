#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "LOW_POWER_SERVO";

// Toggle this macro to compare Light Sleep behavior:
// 1 = Keep PWM alive during Light Sleep using RC_FAST_CLK
// 0 = Default LEDC behavior (PWM shuts off during Light Sleep)
#define KEEP_PWM_IN_LIGHT_SLEEP 1

// MG995 Servo timing constants (50 Hz -> 20,000 us period)
#define SERVO_PWM_FREQ_HZ       50
#define SERVO_PERIOD_US         (1000000 / SERVO_PWM_FREQ_HZ) // 20000 us
#define SERVO_MIN_PULSEWIDTH_US 500                           // 0 degrees
#define SERVO_MAX_PULSEWIDTH_US 2500                          // 180 degrees
#define SERVO_MAX_ANGLE_DEG     180.0f

// ESP32-S3 supports up to 14-bit LEDC timer resolution (16384 counts)
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER        LEDC_TIMER_0
#define SERVO_DUTY_RES          LEDC_TIMER_14_BIT
#define SERVO_MAX_DUTY          (1 << SERVO_DUTY_RES)         // 16384

// Servo GPIO pins and corresponding LEDC channels
#define NUM_SERVOS              3
static const int servo_gpios[NUM_SERVOS] = {4, 5, 6};
static const ledc_channel_t servo_channels[NUM_SERVOS] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2
};

/**
 * @brief Convert angle (0-180 deg) to 14-bit LEDC duty cycle and update channel
 */
static esp_err_t servo_write_angle(ledc_channel_t channel, float angle_deg)
{
    if (angle_deg < 0.0f) angle_deg = 0.0f;
    if (angle_deg > SERVO_MAX_ANGLE_DEG) angle_deg = SERVO_MAX_ANGLE_DEG;

    // Map angle to pulse width in microseconds (500 us to 2500 us)
    float pulse_us = SERVO_MIN_PULSEWIDTH_US +
                     (angle_deg / SERVO_MAX_ANGLE_DEG) *
                     (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US);

    // Convert pulse width to LEDC duty value
    uint32_t duty = (uint32_t)((pulse_us * SERVO_MAX_DUTY) / SERVO_PERIOD_US);

    ESP_ERROR_CHECK(ledc_set_duty(SERVO_LEDC_MODE, channel, duty));
    return ledc_update_duty(SERVO_LEDC_MODE, channel);
}

/**
 * @brief Initialize LEDC timer and 3 channels for low-power servo control
 */
static void servo_pwm_init(void)
{
#if KEEP_PWM_IN_LIGHT_SLEEP
    // Keep the internal ~17.5 MHz RC_FAST oscillator powered during Light Sleep
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON));
#endif

    // 1. Configure shared 50 Hz LEDC Timer
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num       = SERVO_LEDC_TIMER,
        .freq_hz         = SERVO_PWM_FREQ_HZ,
#if KEEP_PWM_IN_LIGHT_SLEEP
        .clk_cfg         = LEDC_USE_RC_FAST_CLK, // Stays active in Light Sleep
#else
        .clk_cfg         = LEDC_AUTO_CLK,        // Uses APB/XTAL (gated in Light Sleep)
#endif
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    // 2. Configure the 3 LEDC Channels on GPIOs 4, 5, and 6
    for (int i = 0; i < NUM_SERVOS; i++) {
        ledc_channel_config_t channel_cfg = {
            .gpio_num   = servo_gpios[i],
            .speed_mode = SERVO_LEDC_MODE,
            .channel    = servo_channels[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = SERVO_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
#if KEEP_PWM_IN_LIGHT_SLEEP
            .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
#else
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
#endif
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

#if KEEP_PWM_IN_LIGHT_SLEEP
        // Disable automatic sleep pad isolation so the GPIO continues routing LEDC output
        gpio_sleep_sel_dis(servo_gpios[i]);
#endif
    }

    ESP_LOGI(TAG, "Initialized 3 servos on GPIOs 4, 5, 6 (Keep PWM in sleep: %d)",
             KEEP_PWM_IN_LIGHT_SLEEP);
}

void app_main(void)
{
    servo_pwm_init();

    // Configure Light Sleep timer wake-up for 4 seconds (4,000,000 us)
    const uint64_t sleep_duration_us = 4ULL * 1000000ULL;
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_duration_us));

    const float test_angles[] = {0.0f, 90.0f, 180.0f, 90.0f};
    const int num_steps = sizeof(test_angles) / sizeof(test_angles[0]);
    int step = 0;

    while (1) {
        float target_angle = test_angles[step];
        ESP_LOGI(TAG, "[ACTIVE] Driving GPIOs 4, 5, 6 to %.1f degrees", target_angle);

        // Set all 3 servos (or offset them if desired)
        for (int i = 0; i < NUM_SERVOS; i++) {
            servo_write_angle(servo_channels[i], target_angle);
        }

        // Wait 800 ms in Active mode for the MG995 servos to physically reach the angle
        vTaskDelay(pdMS_TO_TICKS(800));

        ESP_LOGI(TAG, "[SLEEP] Entering Light Sleep for 4 seconds...");
        // Flush UART output before clocks are gated in Light Sleep
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));

        // Enter Light Sleep: CPU halts here for 4 seconds
        esp_light_sleep_start();

        // Execution resumes here upon timer wake-up
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "[WAKE] Woke up from Light Sleep (cause: %d)", cause);

        step = (step + 1) % num_steps;
    }
}