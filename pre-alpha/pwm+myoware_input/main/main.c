#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "EMG_SERVO";

/* ---------------- Pin Configuration ---------------- */
#define SERVO_1_GPIO            17
#define SERVO_2_GPIO            18
#define SERVO_3_GPIO            8
#define NUM_SERVOS              3

// GPIO 7 maps to ADC1 Channel 6 on ESP32-S3
#define MYOWARE_ADC_UNIT        ADC_UNIT_1
#define MYOWARE_ADC_CHANNEL     ADC_CHANNEL_6

/* ---------------- Servo PWM Configuration (MG995) ---------------- */
#define SERVO_LEDC_TIMER        LEDC_TIMER_0
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_RES          LEDC_TIMER_13_BIT   // 8192 ticks resolution
#define SERVO_PWM_FREQ_HZ       50                  // 50 Hz (20 ms period)
#define SERVO_PERIOD_US         20000               // 20,000 us

// MG995 pulse width bounds (adjust slightly if your servos hit mechanical end-stops)
#define SERVO_MIN_PULSE_US      500                 // 0 degrees
#define SERVO_MAX_PULSE_US      2400                // 180 degrees

/* ---------------- EMG Thresholds & Timing ---------------- */
// 12-bit ADC range: 0 to 4095. Tune these after watching serial logs!
#define FLEX_THRESHOLD_HIGH     350                // Flex trigger threshold
#define RELAX_THRESHOLD_LOW     250                // Relax trigger threshold (hysteresis)
#define ADC_SAMPLES_PER_READ    8                   // Multi-sample smoothing

// Power management timings
#define SERVO_SETTLE_TIME_MS    600                 // Keep PWM active for 600ms so MG995 finishes sweep
#define POLL_INTERVAL_MS        50                  // Sample EMG at 20 Hz (every 50ms)
#define ENABLE_LIGHT_SLEEP      0                   // Set to 0 while calibrating thresholds over USB Serial

static const int servo_gpios[NUM_SERVOS] = {
    SERVO_1_GPIO,
    SERVO_2_GPIO,
    SERVO_3_GPIO
};

static const ledc_channel_t servo_channels[NUM_SERVOS] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2
};

/**
 * @brief Convert an angle (0-180 degrees) into a 13-bit LEDC duty value
 */
static uint32_t servo_angle_to_duty(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    uint32_t pulse_width_us = SERVO_MIN_PULSE_US +
        ((SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * (uint32_t)angle) / 180;

    uint32_t max_duty = (1 << SERVO_LEDC_RES);
    return (pulse_width_us * max_duty) / SERVO_PERIOD_US;
}

/**
 * @brief Initialize LEDC PWM timer and 3 channels for the MG995 servos
 */
static void servos_init(void)
{
    // Use XTAL_CLK (40 MHz) so PWM frequency stays locked during CPU frequency scaling
    ledc_timer_config_t timer_conf = {
        .speed_mode       = SERVO_LEDC_MODE,
        .duty_resolution  = SERVO_LEDC_RES,
        .timer_num        = SERVO_LEDC_TIMER,
        .freq_hz          = SERVO_PWM_FREQ_HZ,
        .clk_cfg          = LEDC_USE_XTAL_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    uint32_t initial_duty = servo_angle_to_duty(0);

    for (int i = 0; i < NUM_SERVOS; i++) {
        ledc_channel_config_t chan_conf = {
            .gpio_num       = servo_gpios[i],
            .speed_mode     = SERVO_LEDC_MODE,
            .channel        = servo_channels[i],
            .timer_sel      = SERVO_LEDC_TIMER,
            .duty           = initial_duty,
            .hpoint         = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&chan_conf));
    }
}

/**
 * @brief Set all 3 servos to the target angle (0 to 180 degrees)
 */
static void servos_set_angle(int angle)
{
    uint32_t duty = servo_angle_to_duty(angle);
    for (int i = 0; i < NUM_SERVOS; i++) {
        ESP_ERROR_CHECK(ledc_set_duty(SERVO_LEDC_MODE, servo_channels[i], duty));
        ESP_ERROR_CHECK(ledc_update_duty(SERVO_LEDC_MODE, servo_channels[i]));
    }
}

/**
 * @brief Initialize ADC1 Channel 6 (GPIO 7) for the MyoWare sensor
 */
static void myoware_adc_init(adc_oneshot_unit_handle_t *adc_handle)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = MYOWARE_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, adc_handle));

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12, // Full-scale ~0V to 3.1V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(*adc_handle, MYOWARE_ADC_CHANNEL, &chan_config));
}

/**
 * @brief Read averaged ADC value from MyoWare sensor
 */
static int myoware_read_averaged(adc_oneshot_unit_handle_t adc_handle)
{
    int sum = 0;
    int raw = 0;
    for (int i = 0; i < ADC_SAMPLES_PER_READ; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, MYOWARE_ADC_CHANNEL, &raw));
        sum += raw;
    }
    return sum / ADC_SAMPLES_PER_READ;
}

/**
 * @brief Configure ESP32-S3 low-power settings (DFS + Timer Wakeup)
 */
static void low_power_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = false
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Dynamic Frequency Scaling (DFS) enabled.");
#endif

#if ENABLE_LIGHT_SLEEP
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(POLL_INTERVAL_MS * 1000ULL));
    ESP_LOGI(TAG, "Light Sleep polling enabled (%d ms interval).", POLL_INTERVAL_MS);
#endif
}

void app_main(void)
{
    adc_oneshot_unit_handle_t adc_handle;

    servos_init();
    myoware_adc_init(&adc_handle);
    low_power_init();

    bool is_flexed = false;
    int64_t last_move_time_ms = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "System initialized. Servos at 0 deg. Waiting for EMG input on GPIO 7...");

    while (1) {
        int emg_val = myoware_read_averaged(adc_handle);
        int64_t now_ms = esp_timer_get_time() / 1000;

        // Hysteresis state machine: Flex -> 180 deg, Relax -> 0 deg
        if (!is_flexed && emg_val >= FLEX_THRESHOLD_HIGH) {
            is_flexed = true;
            last_move_time_ms = now_ms;
            servos_set_angle(180);
            ESP_LOGI(TAG, "FLEX detected (ADC: %d) -> Servos set to 180 deg", emg_val);
        } else if (is_flexed && emg_val <= RELAX_THRESHOLD_LOW) {
            is_flexed = false;
            last_move_time_ms = now_ms;
            servos_set_angle(0);
            ESP_LOGI(TAG, "RELAX detected (ADC: %d) -> Servos set to 0 deg", emg_val);
        }

#if ENABLE_LIGHT_SLEEP
        if ((now_ms - last_move_time_ms) < SERVO_SETTLE_TIME_MS) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        } else {
            for (int i = 0; i < NUM_SERVOS; i++) {
                ledc_stop(SERVO_LEDC_MODE, servo_channels[i], 0);
            }
            esp_light_sleep_start();
        }
#else
        ESP_LOGI(TAG, "EMG ADC: %4d | State: %s", emg_val, is_flexed ? "FLEX (180)" : "RELAX (0)");
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
#endif
    }
}