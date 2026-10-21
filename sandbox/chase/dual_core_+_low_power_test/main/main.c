#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Wi-Fi includes
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

// NimBLE includes
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "DUAL_CORE_LP";

// Core assignments
#define CORE_WIRELESS_PRO       0   // Core 0 handles Wi-Fi, BLE, and wireless task
#define CORE_SERVO_APP          1   // Core 1 dedicated to Servo PWM control

// MG995 Servo timing constants (50 Hz -> 20,000 us period)
#define SERVO_PWM_FREQ_HZ       50
#define SERVO_PERIOD_US         (1000000 / SERVO_PWM_FREQ_HZ)
#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500
#define SERVO_MAX_ANGLE_DEG     180.0f

#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER        LEDC_TIMER_0
#define SERVO_DUTY_RES          LEDC_TIMER_14_BIT
#define SERVO_MAX_DUTY          (1 << SERVO_DUTY_RES)

#define NUM_SERVOS              3
static const int servo_gpios[NUM_SERVOS] = {4, 5, 6};
static const ledc_channel_t servo_channels[NUM_SERVOS] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2
};

// Shared state between Core 1 (Servo writer) and Core 0 (BLE/Wi-Fi reader)
static volatile float g_current_servo_angle = 0.0f;
static volatile uint32_t g_wireless_tx_count = 0;

/* =========================================================================
 * 1. SERVO PWM DRIVER (Runs on Core 1, stays alive in Automatic Light Sleep)
 * ========================================================================= */
static esp_err_t servo_write_angle(ledc_channel_t channel, float angle_deg)
{
    if (angle_deg < 0.0f) angle_deg = 0.0f;
    if (angle_deg > SERVO_MAX_ANGLE_DEG) angle_deg = SERVO_MAX_ANGLE_DEG;

    float pulse_us = SERVO_MIN_PULSEWIDTH_US +
                     (angle_deg / SERVO_MAX_ANGLE_DEG) *
                     (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US);

    uint32_t duty = (uint32_t)((pulse_us * SERVO_MAX_DUTY) / SERVO_PERIOD_US);

    ESP_ERROR_CHECK(ledc_set_duty(SERVO_LEDC_MODE, channel, duty));
    return ledc_update_duty(SERVO_LEDC_MODE, channel);
}

static void servo_pwm_init(void)
{
    // Keep internal RC_FAST clock powered during automatic light sleep cycles
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON));

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num       = SERVO_LEDC_TIMER,
        .freq_hz         = SERVO_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_USE_RC_FAST_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    for (int i = 0; i < NUM_SERVOS; i++) {
        ledc_channel_config_t channel_cfg = {
            .gpio_num   = servo_gpios[i],
            .speed_mode = SERVO_LEDC_MODE,
            .channel    = servo_channels[i],
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = SERVO_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
            .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
        gpio_sleep_sel_dis(servo_gpios[i]);
    }
}

static void servo_control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Servo Task started on Core %d", xPortGetCoreID());

    const float angles[] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 90.0f};
    const int num_steps = sizeof(angles) / sizeof(angles[0]);
    int step = 0;

    while (1) {
        float angle = angles[step];
        g_current_servo_angle = angle;

        ESP_LOGI(TAG, "[Core %d | SERVO] Driving GPIOs 4, 5, 6 -> %.1f deg",
                 xPortGetCoreID(), angle);

        for (int i = 0; i < NUM_SERVOS; i++) {
            servo_write_angle(servo_channels[i], angle);
        }

        step = (step + 1) % num_steps;

        // While this task blocks for 3000 ms, Core 1 enters the Idle task,
        // allowing Automatic Light Sleep whenever Core 0 is also idle.
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* =========================================================================
 * 2. BLE ADVERTISING & WI-FI MODEM SLEEP (Runs on Core 0)
 * ========================================================================= */
static void ble_update_adv_payload(uint8_t angle_byte, uint16_t pkt_count)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    // Custom manufacturer payload: [0xFF, 0xFF, servo_angle_deg, pkt_cnt_low, pkt_cnt_high]
    uint8_t mfg_data[5] = {
        0xFF, 0xFF,
        angle_byte,
        (uint8_t)(pkt_count & 0xFF),
        (uint8_t)((pkt_count >> 8) & 0xFF)
    };
    fields.mfg_data = mfg_data;
    fields.mfg_data_len = sizeof(mfg_data);

    ble_gap_adv_set_fields(&fields);
}

static void ble_start_advertising(void)
{
    ble_update_adv_payload((uint8_t)g_current_servo_angle, 0);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // Use a 250 ms advertising interval (400 * 0.625 ms) to save power
    adv_params.itvl_min = 400;
    adv_params.itvl_max = 400;

    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, NULL, NULL);
    ESP_LOGI(TAG, "[Core %d | BLE] Advertising started as 'ESP32S3-Servo-LP'", xPortGetCoreID());
}

static void ble_on_sync(void)
{
    ble_start_advertising();
}

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE Host Task running on Core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void wireless_init_ble_and_wifi(void)
{
    // 1. Initialize Wi-Fi in Station Mode with Modem Sleep enabled
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Enable Wi-Fi Modem Sleep so the radio sleeps between beacons/activity
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_LOGI(TAG, "[Core %d | Wi-Fi] Station initialized in WIFI_PS_MIN_MODEM power-save mode",
             xPortGetCoreID());

    // 2. Initialize NimBLE stack
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("ESP32S3-Servo-LP");
    ble_hs_cfg.sync_cb = ble_on_sync;

    // Pin NimBLE host task to Core 0
    xTaskCreatePinnedToCore(nimble_host_task, "nimble_host", 4096, NULL, 5, NULL, CORE_WIRELESS_PRO);
}

static void wireless_comm_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Wireless Comm Task started on Core %d", xPortGetCoreID());

    while (1) {
        g_wireless_tx_count++;
        uint8_t current_angle = (uint8_t)g_current_servo_angle;

        // Update live BLE advertisement data with latest servo angle from Core 1
        ble_update_adv_payload(current_angle, (uint16_t)g_wireless_tx_count);

        // Query Wi-Fi power-save state as a lightweight status check
        wifi_ps_type_t ps_type;
        esp_wifi_get_ps(&ps_type);

        ESP_LOGI(TAG, "[Core %d | WIRELESS] BLE Broadcast #%lu (Servo Angle: %u deg) | Wi-Fi PS Mode: %d",
                 xPortGetCoreID(), (unsigned long)g_wireless_tx_count, current_angle, ps_type);

        // Sleep task for 2000 ms (chip enters Automatic Light Sleep between events)
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* =========================================================================
 * 3. AUTOMATIC LIGHT SLEEP & POWER MANAGEMENT CONFIGURATION
 * ========================================================================= */
static void power_management_init(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 240,        // Scale up to 240 MHz during active bursts
        .min_freq_mhz = 80,         // Scale down to 80 MHz when lightly loaded
        .light_sleep_enable = true  // Automatically enter Light Sleep when both cores are idle
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "Automatic Light Sleep & Dynamic Frequency Scaling (80-240 MHz) enabled");
}

void app_main(void)
{
    // Initialize NVS (Required for Wi-Fi and BLE PHY calibration data)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. Initialize Servo PWM on GPIOs 4, 5, 6 with sleep retention
    servo_pwm_init();

    // 2. Initialize Wi-Fi (Modem Sleep) & BLE (NimBLE)
    wireless_init_ble_and_wifi();

    // 3. Activate Automatic Light Sleep & Dynamic Frequency Scaling
    power_management_init();

    // 4. Launch Wireless Telemetry Task on Core 0 (PRO_CPU)
    xTaskCreatePinnedToCore(
        wireless_comm_task,
        "wireless_comm_task",
        4096,
        NULL,
        4,
        NULL,
        CORE_WIRELESS_PRO
    );

    // 5. Launch Dedicated Servo Control Task on Core 1 (APP_CPU)
    xTaskCreatePinnedToCore(
        servo_control_task,
        "servo_control_task",
        4096,
        NULL,
        5,
        NULL,
        CORE_SERVO_APP
    );
}