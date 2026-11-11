/**
 * @file main.cpp
 * @brief GatorGrasp Full-System Dual-Core + Low-Power Integration Benchmark
 *
 * Combines all GatorGrasp subsystems on a single Hosyond ESP32-S3-WROOM-1 N16R8:
 *   1. 4-channel MyoWare 2.0 EMG acquisition via ADC1 DMA (GPIO 4, 5, 6, 7)
 *   2. Edge Impulse ML classification (GatorGrasp_EMG_V2, 300x4 window) on Core 1
 *   3. Dorhea PCA9685 16-ch 12-bit PWM Servo Driver via I2C (GPIO 8 SDA, GPIO 9 SCL)
 *      driving 6 MG995 servos (5 fingers + 1 wrist rotation)
 *   4. NimBLE GATT server ("GatorGrasp") streaming 4-ch ADC readings & receiving
 *      manual servo overrides / calibration from the companion mobile app on Core 0
 *   5. ESP-PM Dynamic Frequency Scaling (40-240 MHz) + Automatic Light Sleep
 *      (Tickless Idle + BLE Controller Modem Sleep + duty-cycled ADC DMA bursts)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// Edge Impulse Classifier Headers
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

// Real EMG Test Vectors from baseline (used for optional boot self-test)
#include "test_vectors.h"

// NimBLE Stack Headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "GG_FULL_LP";

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================ */

// Core pinning: Core 0 = BLE + ADC DAQ, Core 1 = Edge Impulse ML + I2C Actuation
#define CORE_DAQ_BLE                    0
#define CORE_ML_CONTROL                 1

// Set 1 to enable Automatic Light Sleep (duty-cycled 1 kHz ADC DMA bursts so ADC PM lock
// releases during sleep intervals). Set 0 for continuous 1 kHz DMA + DFS-only comparison.
#define LOW_POWER_LIGHT_SLEEP_ENABLE    1

// Set 1 to run baseline Zenodo ground-truth vector verification once on boot
#define RUN_BOOT_MODEL_SELF_TEST        1

// Power Management frequency bounds (MHz)
#define PM_CPU_MAX_MHZ                  240
#define PM_CPU_MIN_MHZ                  40

// Duty-cycle timing when LOW_POWER_LIGHT_SLEEP_ENABLE == 1:
// Capture 75 samples @ 1 kHz/ch (matches EI_CLASSIFIER_SLICE_SIZE = 75 ms), stop ADC DMA,
// run ML + I2C + BLE notify, then idle in Automatic Light Sleep for 125 ms (5 Hz cycle).
#define NUM_EMG_CHANNELS                4
#define ADC_SAMPLE_RATE_PER_CH_HZ       EI_CLASSIFIER_FREQUENCY           // 1000 Hz
#define ADC_TOTAL_SAMPLE_FREQ_HZ        (NUM_EMG_CHANNELS * ADC_SAMPLE_RATE_PER_CH_HZ) // 4000 Hz
#define ADC_BURST_SAMPLES               EI_CLASSIFIER_SLICE_SIZE          // 75 samples/ch (75 ms)
#define CYCLE_SLEEP_MS                  125                               // Sleep window between bursts
#define CYCLE_TOTAL_MS                  (ADC_BURST_SAMPLES + CYCLE_SLEEP_MS) // 200 ms (5 Hz)

// 4x MyoWare 2.0 ADC1 Channels (AVOID ADC2 & strapping pins 0/3/45/46 & PSRAM 33-37)
static const adc_channel_t kEmgAdcChannels[NUM_EMG_CHANNELS] = {
    ADC_CHANNEL_3,  // GPIO 4 -> MyoWare Sensor 1 (emg1)
    ADC_CHANNEL_4,  // GPIO 5 -> MyoWare Sensor 2 (emg2)
    ADC_CHANNEL_5,  // GPIO 6 -> MyoWare Sensor 3 (emg3)
    ADC_CHANNEL_6,  // GPIO 7 -> MyoWare Sensor 4 (emg4 - matches prior firmware GPIO 7)
};
static const int kEmgGpios[NUM_EMG_CHANNELS] = { 4, 5, 6, 7 };

// Default ADC normalization bounds (raw 12-bit counts 0..4095 -> normalized [0.0, 1.0] for EI).
// Adjust or write via BLE CALIB characteristic once MyoWare 2.0 electrodes are calibrated.
#define EMG_DEFAULT_CALIB_MIN_RAW       0
#define EMG_DEFAULT_CALIB_MAX_RAW       4095

// I2C & Dorhea PCA9685 16-Channel 12-Bit PWM Servo Driver Configuration
#define I2C_MASTER_PORT                 I2C_NUM_0
#define I2C_MASTER_SDA_IO               GPIO_NUM_8
#define I2C_MASTER_SCL_IO               GPIO_NUM_9
#define I2C_MASTER_FREQ_HZ              400000
#define I2C_XFER_TIMEOUT_MS             50
#define PCA9685_I2C_ADDR                0x40
#define PCA9685_OSC_HZ                  25000000UL

// PCA9685 Register Map
#define PCA9685_REG_MODE1               0x00
#define PCA9685_REG_MODE2               0x01
#define PCA9685_REG_LED0_ON_L           0x06
#define PCA9685_REG_ALL_LED_OFF_H       0xFD
#define PCA9685_REG_PRE_SCALE           0xFE

#define PCA9685_MODE1_ALLCALL           0x01
#define PCA9685_MODE1_SLEEP             0x10
#define PCA9685_MODE1_AI                0x20
#define PCA9685_MODE1_RESTART           0x80
#define PCA9685_MODE2_OUTDRV            0x04

// 6x MG995 Servo Configuration (Channels 0..4 = Fingers, Channel 5 = Wrist Rotation)
#define NUM_SERVOS                      6
#define SERVO_PWM_FREQ_HZ               50
#define SERVO_PERIOD_US                 20000U
#define SERVO_MIN_PULSE_US              500U
#define SERVO_MAX_PULSE_US              2500U
#define SERVO_MAX_ANGLE_DEG             180U

// Gesture classification & debouncing
#define GESTURE_CONF_THRESHOLD          ((float)EI_CLASSIFIER_THRESHOLD)  // 0.60f
#define GESTURE_DEBOUNCE_STREAK         2
#define BLE_SERVO_OVERRIDE_HOLD_US      5000000LL   // 5 s manual override hold before ML resumes

// Periodic Console Diagnostics
#define LOG_EVERY_N_CYCLES              5           // Print telemetry every ~1 s (5 x 200 ms)
#define DUMP_PM_LOCKS_EVERY_N_LOGS      10          // Dump ESP-PM lock stats every ~10 s

/* ============================================================================
 * GESTURE -> 6-SERVO POSE TABLE (Thumb, Index, Middle, Ring, Pinky, Wrist)
 * ============================================================================ */
struct GesturePose {
    const char *label;
    const char *description;
    uint8_t angles[NUM_SERVOS];
};

// Adjust angles to match tendon calibration & mechanical limits on the printed hand
static const GesturePose kGesturePoses[EI_CLASSIFIER_LABEL_COUNT] = {
    { "gesture_0", "Rest / Open Hand",     {   0,   0,   0,   0,   0,  90 } },
    { "gesture_1", "Power Grasp / Fist",   { 160, 175, 175, 175, 175,  90 } },
    { "gesture_2", "Pinch / Precision",    { 130, 145,  20,  10,  10,  90 } },
    { "gesture_3", "Tripod / Hook Grip",   { 135, 155, 155,  25,  20,  60 } },
    { "gesture_4", "Wrist Rotate / Point", { 120,  10, 165, 165, 165, 145 } },
};

/* ============================================================================
 * SHARED STATE & SYNCHRONIZATION
 * ============================================================================ */

// Edge Impulse raw ADC ring buffer and double-buffered ML input frame
static float s_emg_ring[EI_CLASSIFIER_RAW_SAMPLE_COUNT][NUM_EMG_CHANNELS];
static size_t s_ring_write_idx = 0;
static bool s_ring_primed = false;

static float s_ml_frames[2][EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
static volatile int s_ml_ready_buf_idx = 0;
static std::atomic<bool> s_ml_busy{false};
static std::atomic<uint32_t> s_ml_dropped_frames{0};

// Task handles
static TaskHandle_t s_ml_task_handle = nullptr;
static TaskHandle_t s_daq_task_handle = nullptr;

// Power Management locks & light-sleep telemetry
static esp_pm_lock_handle_t s_ml_cpu_lock = nullptr;
static std::atomic<uint32_t> s_light_sleep_entries{0};
static std::atomic<int64_t> s_light_sleep_total_us{0};

// ADC handles
static adc_continuous_handle_t s_adc_handle = nullptr;
static adc_cali_handle_t s_adc_cali[NUM_EMG_CHANNELS] = { nullptr };
static uint8_t s_adc_raw_dma_buf[ADC_BURST_SAMPLES * NUM_EMG_CHANNELS * SOC_ADC_DIGI_RESULT_BYTES * 2];
static adc_continuous_data_t s_adc_parsed_buf[ADC_BURST_SAMPLES * NUM_EMG_CHANNELS * 2];

// Calibration ranges (shared with BLE CALIB characteristic)
static portMUX_TYPE s_state_spinlock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_calib_min_raw[NUM_EMG_CHANNELS] = {
    EMG_DEFAULT_CALIB_MIN_RAW, EMG_DEFAULT_CALIB_MIN_RAW,
    EMG_DEFAULT_CALIB_MIN_RAW, EMG_DEFAULT_CALIB_MIN_RAW
};
static uint16_t s_calib_max_raw[NUM_EMG_CHANNELS] = {
    EMG_DEFAULT_CALIB_MAX_RAW, EMG_DEFAULT_CALIB_MAX_RAW,
    EMG_DEFAULT_CALIB_MAX_RAW, EMG_DEFAULT_CALIB_MAX_RAW
};

// Latest telemetry snapshots
static uint16_t s_latest_adc_raw[NUM_EMG_CHANNELS] = {0};
static uint16_t s_latest_adc_mv[NUM_EMG_CHANNELS] = {0};
static uint8_t  s_current_servo_angles[NUM_SERVOS] = { 0, 0, 0, 0, 0, 90 };

struct MlTelemetry {
    int active_gesture;
    int last_pred_gesture;
    float confidence;
    int64_t total_us;
    int dsp_ms;
    int nn_ms;
    uint32_t inference_count;
};
static MlTelemetry s_ml_telemetry = { 0, 0, 0.0f, 0, 0, 0, 0 };

// I2C / PCA9685 state
static SemaphoreHandle_t s_i2c_mutex = nullptr;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static i2c_master_dev_handle_t s_pca9685_dev = nullptr;
static bool s_pca9685_online = false;
static int64_t s_last_i2c_tx_us = 0;
static uint32_t s_i2c_tx_count = 0;
static uint32_t s_i2c_err_count = 0;

// BLE NimBLE state (compatible with src_software/app UUIDs)
static uint8_t s_own_addr_type = 0;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_emg_val_handle = 0;
static uint16_t s_gesture_val_handle = 0;
static bool s_emg_notify_en = false;
static bool s_gesture_notify_en = false;
static std::atomic<uint32_t> s_ble_notify_count{0};
static std::atomic<int64_t> s_manual_override_until_us{0};

/* ============================================================================
 * POWER MANAGEMENT & LIGHT SLEEP CALLBACKS
 * ============================================================================ */

static IRAM_ATTR esp_err_t on_light_sleep_enter(int64_t /*sleep_time_us*/, void * /*arg*/)
{
    s_light_sleep_entries.fetch_add(1, std::memory_order_relaxed);
    return ESP_OK;
}

static IRAM_ATTR esp_err_t on_light_sleep_exit(int64_t sleep_time_us, void * /*arg*/)
{
    s_light_sleep_total_us.fetch_add(sleep_time_us, std::memory_order_relaxed);
    return ESP_OK;
}

static void power_management_init(void)
{
    esp_pm_config_t pm_cfg = {};
    pm_cfg.max_freq_mhz = PM_CPU_MAX_MHZ;
    pm_cfg.min_freq_mhz = PM_CPU_MIN_MHZ;
    pm_cfg.light_sleep_enable = (LOW_POWER_LIGHT_SLEEP_ENABLE != 0);
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));

    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ml_cpu_max", &s_ml_cpu_lock));

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = {};
    cbs.enter_cb = on_light_sleep_enter;
    cbs.exit_cb  = on_light_sleep_exit;
    ESP_ERROR_CHECK(esp_pm_light_sleep_register_cbs(&cbs));
#endif

    ESP_LOGI(TAG, "Power management initialized: DFS %d-%d MHz, Automatic Light Sleep = %s",
             PM_CPU_MIN_MHZ, PM_CPU_MAX_MHZ,
             pm_cfg.light_sleep_enable ? "ENABLED" : "DISABLED");
}

static void flush_uart_console(void)
{
    fflush(stdout);
    uart_wait_tx_idle_polling((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM);
}

/* ============================================================================
 * DORHEA PCA9685 I2C SERVO DRIVER (ESP-IDF v6.1 i2c_master)
 * ============================================================================ */

static esp_err_t pca9685_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_pca9685_dev, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
}

static uint16_t angle_deg_to_pca9685_ticks(uint8_t angle_deg)
{
    if (angle_deg > SERVO_MAX_ANGLE_DEG) {
        angle_deg = SERVO_MAX_ANGLE_DEG;
    }
    uint32_t pulse_us = SERVO_MIN_PULSE_US +
        ((SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * (uint32_t)angle_deg) / SERVO_MAX_ANGLE_DEG;
    // 12-bit PCA9685 counter (4096 ticks per 20,000 us @ 50 Hz)
    uint32_t ticks = (pulse_us * 4096U + (SERVO_PERIOD_US / 2U)) / SERVO_PERIOD_US;
    if (ticks > 4095U) {
        ticks = 4095U;
    }
    return (uint16_t)ticks;
}

static esp_err_t pca9685_configure_chip_locked(void)
{
    // Probe first so we never stall or crash if the PCA9685 board isn't wired on the bench yet
    esp_err_t err = i2c_master_probe(s_i2c_bus, PCA9685_I2C_ADDR, I2C_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        s_pca9685_online = false;
        return err;
    }

    // Datasheet prescale = round(osc / (4096 * freq)) - 1 => 121 for 25 MHz @ 50 Hz
    uint8_t prescale = (uint8_t)((PCA9685_OSC_HZ / (4096UL * SERVO_PWM_FREQ_HZ)) - 1UL);

    // Put PCA9685 into sleep mode with Auto-Increment enabled so PRE_SCALE can be written
    err = pca9685_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_SLEEP | PCA9685_MODE1_AI);
    if (err != ESP_OK) { s_pca9685_online = false; return err; }

    err = pca9685_write_reg(PCA9685_REG_PRE_SCALE, prescale);
    if (err != ESP_OK) { s_pca9685_online = false; return err; }

    // Totem-pole output stage (standard for servo PWM inputs)
    err = pca9685_write_reg(PCA9685_REG_MODE2, PCA9685_MODE2_OUTDRV);
    if (err != ESP_OK) { s_pca9685_online = false; return err; }

    // Wake oscillator, keep Auto-Increment & All-Call enabled, wait >= 500 us per datasheet
    err = pca9685_write_reg(PCA9685_REG_MODE1, PCA9685_MODE1_AI | PCA9685_MODE1_ALLCALL);
    if (err != ESP_OK) { s_pca9685_online = false; return err; }

    vTaskDelay(pdMS_TO_TICKS(1));

    err = pca9685_write_reg(PCA9685_REG_MODE1,
                            PCA9685_MODE1_RESTART | PCA9685_MODE1_AI | PCA9685_MODE1_ALLCALL);
    if (err != ESP_OK) { s_pca9685_online = false; return err; }

    s_pca9685_online = true;
    ESP_LOGI(TAG, "PCA9685 initialized at 0x%02X (prescale=%u -> %d Hz)",
             PCA9685_I2C_ADDR, prescale, SERVO_PWM_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t pca9685_set_servos(const uint8_t angles[NUM_SERVOS])
{
    if (!s_i2c_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_state_spinlock);
    memcpy(s_current_servo_angles, angles, NUM_SERVOS);
    portEXIT_CRITICAL(&s_state_spinlock);

    esp_err_t err = ESP_OK;
    if (!s_pca9685_online) {
        // Attempt hot-plug recovery if board was connected after boot
        err = pca9685_configure_chip_locked();
        if (err != ESP_OK) {
            s_i2c_err_count++;
            xSemaphoreGive(s_i2c_mutex);
            return err;
        }
    }

    // Burst-write all 6 channels (LED0..LED5) in one I2C transaction using MODE1 Auto-Increment:
    // [start_reg, 6 * (ON_L, ON_H, OFF_L, OFF_H)] = 25 bytes (~0.55 ms @ 400 kHz)
    uint8_t payload[1 + NUM_SERVOS * 4];
    payload[0] = PCA9685_REG_LED0_ON_L;
    for (int ch = 0; ch < NUM_SERVOS; ch++) {
        uint16_t off = angle_deg_to_pca9685_ticks(angles[ch]);
        size_t base = 1 + (size_t)ch * 4;
        payload[base + 0] = 0x00;                     // ON_L
        payload[base + 1] = 0x00;                     // ON_H
        payload[base + 2] = (uint8_t)(off & 0xFF);    // OFF_L
        payload[base + 3] = (uint8_t)((off >> 8) & 0x0F); // OFF_H
    }

    int64_t t0 = esp_timer_get_time();
    err = i2c_master_transmit(s_pca9685_dev, payload, sizeof(payload), I2C_XFER_TIMEOUT_MS);
    int64_t dt = esp_timer_get_time() - t0;

    if (err == ESP_OK) {
        s_last_i2c_tx_us = dt;
        s_i2c_tx_count++;
    } else {
        s_i2c_err_count++;
        s_pca9685_online = false; // Trigger re-probe on next write
    }

    xSemaphoreGive(s_i2c_mutex);
    return err;
}

static void pca9685_i2c_init(void)
{
    s_i2c_mutex = xSemaphoreCreateMutex();
    assert(s_i2c_mutex != nullptr);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_MASTER_PORT;
    bus_cfg.sda_io_num = I2C_MASTER_SDA_IO;
    bus_cfg.scl_io_num = I2C_MASTER_SCL_IO;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.intr_priority = 0;
    bus_cfg.trans_queue_depth = 0;
    bus_cfg.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = PCA9685_I2C_ADDR;
    dev_cfg.scl_speed_hz = I2C_MASTER_FREQ_HZ;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pca9685_dev));

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
    esp_err_t err = pca9685_configure_chip_locked();
    xSemaphoreGive(s_i2c_mutex);

    if (err == ESP_OK) {
        pca9685_set_servos(kGesturePoses[0].angles);
    } else {
        ESP_LOGW(TAG, "PCA9685 not detected at I2C 0x%02X (SDA=GPIO%d, SCL=GPIO%d): %s. "
                      "Will auto-retry on pose updates.",
                 PCA9685_I2C_ADDR, (int)I2C_MASTER_SDA_IO, (int)I2C_MASTER_SCL_IO,
                 esp_err_to_name(err));
    }
}

/* ============================================================================
 * 4-CHANNEL ADC1 DMA ACQUISITION & CALIBRATION
 * ============================================================================ */

static void adc_init_all(void)
{
    adc_continuous_handle_cfg_t hdl_cfg = {};
    hdl_cfg.max_store_buf_size = sizeof(s_adc_raw_dma_buf) * 2;
    hdl_cfg.conv_frame_size = ADC_BURST_SAMPLES * NUM_EMG_CHANNELS * SOC_ADC_DIGI_RESULT_BYTES;
    hdl_cfg.flags.flush_pool = 1;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hdl_cfg, &s_adc_handle));

    adc_digi_pattern_config_t patterns[NUM_EMG_CHANNELS] = {};
    for (int i = 0; i < NUM_EMG_CHANNELS; i++) {
        patterns[i].atten     = ADC_ATTEN_DB_12;
        patterns[i].channel   = kEmgAdcChannels[i];
        patterns[i].unit      = ADC_UNIT_1;
        patterns[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH; // 12-bit on ESP32-S3
    }

    adc_continuous_config_t dig_cfg = {};
    dig_cfg.pattern_num = NUM_EMG_CHANNELS;
    dig_cfg.adc_pattern = patterns;
    dig_cfg.sample_freq_hz = ADC_TOTAL_SAMPLE_FREQ_HZ; // 4000 Hz => 1000 Hz/ch
    dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    ESP_ERROR_CHECK(adc_continuous_config(s_adc_handle, &dig_cfg));

    // Create curve-fitting voltage calibration handles per ADC1 channel
    for (int i = 0; i < NUM_EMG_CHANNELS; i++) {
        adc_cali_curve_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id  = ADC_UNIT_1;
        cali_cfg.chan     = kEmgAdcChannels[i];
        cali_cfg.atten    = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali[i]) != ESP_OK) {
            s_adc_cali[i] = nullptr;
        }
    }

    ESP_LOGI(TAG, "ADC1 continuous DMA configured: 4 channels (GPIOs %d,%d,%d,%d) @ %d Hz/ch",
             kEmgGpios[0], kEmgGpios[1], kEmgGpios[2], kEmgGpios[3], ADC_SAMPLE_RATE_PER_CH_HZ);
}

static uint16_t raw_to_mv(int ch_idx, uint32_t raw)
{
    int mv = 0;
    if (s_adc_cali[ch_idx] && adc_cali_raw_to_voltage(s_adc_cali[ch_idx], (int)raw, &mv) == ESP_OK) {
        return (mv < 0) ? 0 : (uint16_t)mv;
    }
    return (uint16_t)((raw * 3100U) / 4095U);
}

static int find_channel_index(adc_channel_t chan)
{
    for (int i = 0; i < NUM_EMG_CHANNELS; i++) {
        if (kEmgAdcChannels[i] == chan) {
            return i;
        }
    }
    return -1;
}

static float normalize_adc_sample(int ch_idx, uint32_t raw)
{
    uint16_t min_r, max_r;
    portENTER_CRITICAL(&s_state_spinlock);
    min_r = s_calib_min_raw[ch_idx];
    max_r = s_calib_max_raw[ch_idx];
    portEXIT_CRITICAL(&s_state_spinlock);

    float denom = (max_r > min_r) ? (float)(max_r - min_r) : 4095.0f;
    float v = ((float)raw - (float)min_r) / denom;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static void push_frame_to_ring(const float frame[NUM_EMG_CHANNELS])
{
    for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
        s_emg_ring[s_ring_write_idx][c] = frame[c];
    }
    s_ring_write_idx = (s_ring_write_idx + 1) % EI_CLASSIFIER_RAW_SAMPLE_COUNT;
}

/**
 * @brief Collect ADC_BURST_SAMPLES frames across all 4 ADC1 channels.
 *
 * In LOW_POWER_LIGHT_SLEEP_ENABLE mode, starts ADC DMA for the burst and stops it
 * immediately afterwards so ESP_PM_APB_FREQ_MAX is released for Automatic Light Sleep.
 */
static bool acquire_adc_burst(uint16_t out_avg_raw[NUM_EMG_CHANNELS],
                              uint16_t out_avg_mv[NUM_EMG_CHANNELS])
{
    static uint32_t ch_samples[NUM_EMG_CHANNELS][ADC_BURST_SAMPLES];
    size_t ch_counts[NUM_EMG_CHANNELS] = { 0, 0, 0, 0 };

#if LOW_POWER_LIGHT_SLEEP_ENABLE
    if (adc_continuous_flush_pool(s_adc_handle) != ESP_OK) {
        return false;
    }
    if (adc_continuous_start(s_adc_handle) != ESP_OK) {
        return false;
    }
#endif

    int64_t deadline_us = esp_timer_get_time() + 180000LL; // 180 ms safety ceiling for 75 ms burst
    while (esp_timer_get_time() < deadline_us) {
        bool all_full = true;
        for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
            if (ch_counts[c] < ADC_BURST_SAMPLES) {
                all_full = false;
                break;
            }
        }
        if (all_full) {
            break;
        }

        uint32_t bytes_read = 0;
        esp_err_t err = adc_continuous_read(s_adc_handle, s_adc_raw_dma_buf,
                                            sizeof(s_adc_raw_dma_buf), &bytes_read, 100);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK || bytes_read == 0) {
            break;
        }

        uint32_t num_parsed = 0;
        if (adc_continuous_parse_data(s_adc_handle, s_adc_raw_dma_buf, bytes_read,
                                      s_adc_parsed_buf, &num_parsed) != ESP_OK) {
            continue;
        }

        for (uint32_t i = 0; i < num_parsed; i++) {
            const adc_continuous_data_t &item = s_adc_parsed_buf[i];
            if (!item.valid || item.unit != ADC_UNIT_1) {
                continue;
            }
            int idx = find_channel_index(item.channel);
            if (idx >= 0 && ch_counts[idx] < ADC_BURST_SAMPLES) {
                ch_samples[idx][ch_counts[idx]++] = item.raw_data;
            }
        }
    }

#if LOW_POWER_LIGHT_SLEEP_ENABLE
    adc_continuous_stop(s_adc_handle);
#endif

    size_t min_frames = ADC_BURST_SAMPLES;
    for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
        if (ch_counts[c] < min_frames) {
            min_frames = ch_counts[c];
        }
    }
    if (min_frames == 0) {
        return false;
    }

    uint64_t sum_raw[NUM_EMG_CHANNELS] = { 0, 0, 0, 0 };
    for (size_t s = 0; s < min_frames; s++) {
        float norm_frame[NUM_EMG_CHANNELS];
        for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
            uint32_t r = ch_samples[c][s];
            sum_raw[c] += r;
            norm_frame[c] = normalize_adc_sample(c, r);
        }
        push_frame_to_ring(norm_frame);
    }

    // On first burst, replicate across remaining slots so the 300-sample window has no zeros
    if (!s_ring_primed) {
        s_ring_primed = true;
        size_t repeats = (EI_CLASSIFIER_RAW_SAMPLE_COUNT / min_frames);
        for (size_t rep = 1; rep < repeats; rep++) {
            for (size_t s = 0; s < min_frames; s++) {
                float norm_frame[NUM_EMG_CHANNELS];
                for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
                    norm_frame[c] = normalize_adc_sample(c, ch_samples[c][s]);
                }
                push_frame_to_ring(norm_frame);
            }
        }
    }

    for (int c = 0; c < NUM_EMG_CHANNELS; c++) {
        out_avg_raw[c] = (uint16_t)(sum_raw[c] / min_frames);
        out_avg_mv[c]  = raw_to_mv(c, out_avg_raw[c]);
    }
    return true;
}

/* ============================================================================
 * NIMBLE BLE GATT SERVER (Compatible with src_software/app)
 * ============================================================================ */

// UUID base: 5a1c00xx-7b2e-4d0a-9c3e-6f8a1b2c3d4e
#define GG_UUID(n) BLE_UUID128_INIT(0x4e,0x3d,0x2c,0x1b,0x8a,0x6f,0x3e,0x9c,0x0a,0x4d,0x2e,0x7b,(n),0x00,0x1c,0x5a)
static const ble_uuid128_t s_svc_uuid     = GG_UUID(0x01);
static const ble_uuid128_t s_emg_uuid     = GG_UUID(0x02);
static const ble_uuid128_t s_servo_uuid   = GG_UUID(0x03);
static const ble_uuid128_t s_calib_uuid   = GG_UUID(0x04);
static const ble_uuid128_t s_gesture_uuid = GG_UUID(0x05);

static int gatt_emg_access(uint16_t /*conn_h*/, uint16_t /*attr_h*/,
                           struct ble_gatt_access_ctxt *ctxt, void * /*arg*/)
{
    uint16_t snap[NUM_EMG_CHANNELS];
    portENTER_CRITICAL(&s_state_spinlock);
    memcpy(snap, s_latest_adc_raw, sizeof(snap));
    portEXIT_CRITICAL(&s_state_spinlock);
    return os_mbuf_append(ctxt->om, snap, sizeof(snap)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_servo_access(uint16_t /*conn_h*/, uint16_t /*attr_h*/,
                             struct ble_gatt_access_ctxt *ctxt, void * /*arg*/)
{
    uint8_t buf[16];
    uint16_t len = 0;
    uint16_t pktlen = OS_MBUF_PKTLEN(ctxt->om);
    if (pktlen < 1 || pktlen > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t target[NUM_SERVOS];
    if (len >= NUM_SERVOS) {
        for (int i = 0; i < NUM_SERVOS; i++) {
            target[i] = (buf[i] > SERVO_MAX_ANGLE_DEG) ? SERVO_MAX_ANGLE_DEG : buf[i];
        }
    } else {
        uint8_t angle = (buf[0] > SERVO_MAX_ANGLE_DEG) ? SERVO_MAX_ANGLE_DEG : buf[0];
        for (int i = 0; i < NUM_SERVOS; i++) {
            target[i] = angle;
        }
    }

    s_manual_override_until_us.store(esp_timer_get_time() + BLE_SERVO_OVERRIDE_HOLD_US,
                                     std::memory_order_relaxed);
    esp_err_t err = pca9685_set_servos(target);
    ESP_LOGI(TAG, "BLE servo override -> [%u,%u,%u,%u,%u,%u] (%s)",
             target[0], target[1], target[2], target[3], target[4], target[5],
             esp_err_to_name(err));
    return 0;
}

static int gatt_calib_access(uint16_t /*conn_h*/, uint16_t /*attr_h*/,
                             struct ble_gatt_access_ctxt *ctxt, void * /*arg*/)
{
    uint16_t buf[NUM_EMG_CHANNELS * 2]; // 4 x (min, max) = 16 bytes; app v1 sends 12 bytes (3 pairs)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        portENTER_CRITICAL(&s_state_spinlock);
        for (int i = 0; i < NUM_EMG_CHANNELS; i++) {
            buf[i * 2 + 0] = s_calib_min_raw[i];
            buf[i * 2 + 1] = s_calib_max_raw[i];
        }
        portEXIT_CRITICAL(&s_state_spinlock);
        // Send 12 bytes by default if app expects 3 pairs (still works cleanly with 16-byte readers)
        return os_mbuf_append(ctxt->om, buf, 12) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    uint16_t pktlen = OS_MBUF_PKTLEN(ctxt->om);
    if (pktlen != 12 && pktlen != 16) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int pairs = len / 4;
    portENTER_CRITICAL(&s_state_spinlock);
    for (int i = 0; i < pairs && i < NUM_EMG_CHANNELS; i++) {
        s_calib_min_raw[i] = buf[i * 2 + 0];
        s_calib_max_raw[i] = buf[i * 2 + 1];
    }
    portEXIT_CRITICAL(&s_state_spinlock);
    ESP_LOGI(TAG, "BLE calibration updated (%d channels)", pairs);
    return 0;
}

static int gatt_gesture_access(uint16_t /*conn_h*/, uint16_t /*attr_h*/,
                               struct ble_gatt_access_ctxt *ctxt, void * /*arg*/)
{
    uint8_t status[4];
    portENTER_CRITICAL(&s_state_spinlock);
    status[0] = (uint8_t)s_ml_telemetry.active_gesture;
    status[1] = (uint8_t)s_ml_telemetry.last_pred_gesture;
    status[2] = (uint8_t)(s_ml_telemetry.confidence * 100.0f);
    status[3] = s_pca9685_online ? 1 : 0;
    portEXIT_CRITICAL(&s_state_spinlock);
    return os_mbuf_append(ctxt->om, status, sizeof(status)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def s_gatt_chrs[] = {
    {
        .uuid = &s_emg_uuid.u,
        .access_cb = gatt_emg_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_emg_val_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &s_servo_uuid.u,
        .access_cb = gatt_servo_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &s_calib_uuid.u,
        .access_cb = gatt_calib_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .min_key_size = 0,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &s_gesture_uuid.u,
        .access_cb = gatt_gesture_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_gesture_val_handle,
        .cpfd = nullptr,
    },
    { nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr },
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .includes = nullptr,
        .characteristics = s_gatt_chrs,
    },
    { 0, nullptr, nullptr, nullptr },
};

static void ble_start_advertise(void);

static int ble_gap_event(struct ble_gap_event *event, void * /*arg*/)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "[BLE] Connect status=%d", event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            // Request low-power friendly connection interval (30-60 ms, slave latency 2)
            struct ble_gap_upd_params upd = {};
            upd.itvl_min = 24;                  // 30 ms
            upd.itvl_max = 48;                  // 60 ms
            upd.latency  = 2;
            upd.supervision_timeout = 400;      // 4.0 s
            ble_gap_update_params(s_conn_handle, &upd);
        } else {
            ble_start_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[BLE] Disconnected (reason=0x%x)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_emg_notify_en = false;
        s_gesture_notify_en = false;
        ble_start_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_emg_val_handle) {
            s_emg_notify_en = (event->subscribe.cur_notify != 0);
        } else if (event->subscribe.attr_handle == s_gesture_val_handle) {
            s_gesture_notify_en = (event->subscribe.cur_notify != 0);
        }
        ESP_LOGI(TAG, "[BLE] Subscribe: emg=%d gesture=%d", s_emg_notify_en, s_gesture_notify_en);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_start_advertise();
        break;

    default:
        break;
    }
    return 0;
}

static void ble_start_advertise(void)
{
    struct ble_hs_adv_fields fields = {};
    const char *name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp_fields);

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 200-300 ms advertising interval saves substantial power during modem sleep
    adv_params.itvl_min = 320;
    adv_params.itvl_max = 480;

    int rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params, ble_gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] Advertising start failed: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "[BLE] Advertising as '%s'", name);
    }
}

static void ble_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    ble_start_advertise();
}

static void nimble_host_task(void * /*param*/)
{
    ESP_LOGI(TAG, "NimBLE Host Task running on Core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init_all(void)
{
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_gatt_svcs));
    ble_svc_gap_device_name_set("GatorGrasp");

    // Pin NimBLE host task to Core 0 alongside BT controller
    xTaskCreatePinnedToCore(nimble_host_task, "nimble_host", 5120, nullptr, 5, nullptr, CORE_DAQ_BLE);
}

static void ble_notify_adc_and_status(const uint16_t adc_raw[NUM_EMG_CHANNELS])
{
    uint16_t ch = s_conn_handle;
    if (ch == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    if (s_emg_notify_en) {
        // 8 bytes = 4 x uint16_t LE (first 4 bytes remain directly compatible with App.js)
        struct os_mbuf *om = ble_hs_mbuf_from_flat(adc_raw, sizeof(uint16_t) * NUM_EMG_CHANNELS);
        if (om && ble_gatts_notify_custom(ch, s_emg_val_handle, om) == 0) {
            s_ble_notify_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (s_gesture_notify_en) {
        uint8_t status[4];
        portENTER_CRITICAL(&s_state_spinlock);
        status[0] = (uint8_t)s_ml_telemetry.active_gesture;
        status[1] = (uint8_t)s_ml_telemetry.last_pred_gesture;
        status[2] = (uint8_t)(s_ml_telemetry.confidence * 100.0f);
        status[3] = s_pca9685_online ? 1 : 0;
        portEXIT_CRITICAL(&s_state_spinlock);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(status, sizeof(status));
        if (om) {
            ble_gatts_notify_custom(ch, s_gesture_val_handle, om);
        }
    }
}

/* ============================================================================
 * EDGE IMPULSE ML INFERENCE + I2C SERVO ACTUATION TASK (CORE 1)
 * ============================================================================ */

static void run_boot_model_self_test(void)
{
    struct TestCase {
        const char *expected;
        const float *vec;
    };
    static const TestCase cases[] = {
        { "gesture_0", test_gesture_0 },
        { "gesture_1", test_gesture_1 },
        { "gesture_2", test_gesture_2 },
        { "gesture_3", test_gesture_3 },
        { "gesture_4", test_gesture_4 },
    };

    printf("\n>>> BOOT SELF-TEST: Ground-Truth Edge Impulse Vectors (Core %d) <<<\n", xPortGetCoreID());
    size_t passed = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memcpy(s_ml_frames[0], cases[i].vec, sizeof(s_ml_frames[0]));
        signal_t signal;
        if (numpy::signal_from_buffer(s_ml_frames[0], EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal) != 0) {
            printf("ERR: signal_from_buffer failed\n");
            continue;
        }

        if (s_ml_cpu_lock) esp_pm_lock_acquire(s_ml_cpu_lock);
        int64_t t0 = esp_timer_get_time();
        ei_impulse_result_t result = {};
        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
        int64_t dt_us = esp_timer_get_time() - t0;
        if (s_ml_cpu_lock) esp_pm_lock_release(s_ml_cpu_lock);

        if (res != EI_IMPULSE_OK) {
            printf("ERR: run_classifier failed (%d)\n", res);
            continue;
        }

        size_t best_idx = 0;
        float best_val = 0.0f;
        for (size_t c = 0; c < EI_CLASSIFIER_LABEL_COUNT; c++) {
            if (result.classification[c].value > best_val) {
                best_val = result.classification[c].value;
                best_idx = c;
            }
        }
        const char *pred = result.classification[best_idx].label;
        bool ok = (strcmp(pred, cases[i].expected) == 0);
        if (ok) passed++;

        printf("  Expected %-9s -> Predicted %-9s (%5.1f%%) | %lld us (DSP %d ms, NN %d ms) [%s]\n",
               cases[i].expected, pred, best_val * 100.0f, dt_us,
               result.timing.dsp, result.timing.classification, ok ? "PASS" : "FAIL");
    }
    printf(">>> SELF-TEST RESULT: %u / %u PASSED <<<\n\n",
           (unsigned)passed, (unsigned)(sizeof(cases) / sizeof(cases[0])));
    flush_uart_console();
}

static void ml_control_task(void * /*pvParameters*/)
{
    ESP_LOGI(TAG, "ML + I2C Control Task started on Core %d", xPortGetCoreID());

#if RUN_BOOT_MODEL_SELF_TEST
    run_boot_model_self_test();
#endif

    // Notify DAQ task on Core 0 that Core 1 is ready to process windows
    if (s_daq_task_handle) {
        xTaskNotifyGive(s_daq_task_handle);
    }

    int active_gesture = 0;
    int candidate_gesture = 0;
    int candidate_streak = 0;

    while (true) {
        // Sleep until Core 0 supplies a new EMG window snapshot
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int buf_idx = s_ml_ready_buf_idx;
        signal_t signal;
        int err = numpy::signal_from_buffer(s_ml_frames[buf_idx],
                                            EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (err != 0) {
            s_ml_busy.store(false, std::memory_order_release);
            ESP_LOGE(TAG, "signal_from_buffer failed (%d)", err);
            continue;
        }

        // Boost CPU to 240 MHz strictly during DSP + NN inference burst
        if (s_ml_cpu_lock) esp_pm_lock_acquire(s_ml_cpu_lock);
        int64_t t_start = esp_timer_get_time();
        ei_impulse_result_t result = {};
        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
        int64_t t_us = esp_timer_get_time() - t_start;
        if (s_ml_cpu_lock) esp_pm_lock_release(s_ml_cpu_lock);

        s_ml_busy.store(false, std::memory_order_release);

        if (res != EI_IMPULSE_OK) {
            ESP_LOGE(TAG, "run_classifier failed (%d)", res);
            continue;
        }

        size_t best_idx = 0;
        float best_val = 0.0f;
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (result.classification[i].value > best_val) {
                best_val = result.classification[i].value;
                best_idx = i;
            }
        }

        bool pose_changed = false;
        if (best_val >= GESTURE_CONF_THRESHOLD) {
            if ((int)best_idx == candidate_gesture) {
                candidate_streak++;
            } else {
                candidate_gesture = (int)best_idx;
                candidate_streak = 1;
            }

            if (candidate_streak >= GESTURE_DEBOUNCE_STREAK && candidate_gesture != active_gesture) {
                active_gesture = candidate_gesture;
                pose_changed = true;
            }
        }

        portENTER_CRITICAL(&s_state_spinlock);
        s_ml_telemetry.active_gesture    = active_gesture;
        s_ml_telemetry.last_pred_gesture = (int)best_idx;
        s_ml_telemetry.confidence        = best_val;
        s_ml_telemetry.total_us          = t_us;
        s_ml_telemetry.dsp_ms            = result.timing.dsp;
        s_ml_telemetry.nn_ms             = result.timing.classification;
        s_ml_telemetry.inference_count++;
        portEXIT_CRITICAL(&s_state_spinlock);

        // Only send I2C traffic when the debounced gesture actually changes (and no BLE manual override).
        // PCA9685 autonomously maintains the 50 Hz PWM pulses while ESP32-S3 enters Light Sleep.
        int64_t now_us = esp_timer_get_time();
        if (pose_changed && now_us >= s_manual_override_until_us.load(std::memory_order_relaxed)) {
            esp_err_t i2c_err = pca9685_set_servos(kGesturePoses[active_gesture].angles);
            ESP_LOGI(TAG, "[Core %d | GESTURE] %s (%s, %.1f%%) -> PCA9685 I2C: %s",
                     xPortGetCoreID(),
                     kGesturePoses[active_gesture].label,
                     kGesturePoses[active_gesture].description,
                     best_val * 100.0f,
                     esp_err_to_name(i2c_err));
        }
    }
}

/* ============================================================================
 * 4-CH ADC DAQ + BLE TELEMETRY + BENCHMARK MONITOR TASK (CORE 0)
 * ============================================================================ */

static void copy_ring_to_linear_buffer(float *dst)
{
    size_t read_idx = s_ring_write_idx; // Oldest sample in circular buffer
    for (size_t s = 0; s < EI_CLASSIFIER_RAW_SAMPLE_COUNT; s++) {
        size_t pos = (read_idx + s) % EI_CLASSIFIER_RAW_SAMPLE_COUNT;
        memcpy(&dst[s * NUM_EMG_CHANNELS], s_emg_ring[pos], sizeof(float) * NUM_EMG_CHANNELS);
    }
}

static void print_benchmark_status(uint32_t cycle_num,
                                   int64_t window_elapsed_us,
                                   int64_t *prev_sleep_us,
                                   uint32_t *prev_sleep_cnt)
{
    int64_t total_sleep_us = s_light_sleep_total_us.load(std::memory_order_relaxed);
    uint32_t total_sleep_cnt = s_light_sleep_entries.load(std::memory_order_relaxed);

    int64_t delta_sleep_us = total_sleep_us - *prev_sleep_us;
    uint32_t delta_sleep_cnt = total_sleep_cnt - *prev_sleep_cnt;
    *prev_sleep_us = total_sleep_us;
    *prev_sleep_cnt = total_sleep_cnt;

    float sleep_pct = (window_elapsed_us > 0)
        ? (100.0f * (float)delta_sleep_us / (float)window_elapsed_us)
        : 0.0f;

    uint16_t adc_raw[NUM_EMG_CHANNELS];
    uint16_t adc_mv[NUM_EMG_CHANNELS];
    uint8_t servos[NUM_SERVOS];
    MlTelemetry ml_snap;
    portENTER_CRITICAL(&s_state_spinlock);
    memcpy(adc_raw, s_latest_adc_raw, sizeof(adc_raw));
    memcpy(adc_mv, s_latest_adc_mv, sizeof(adc_mv));
    memcpy(servos, s_current_servo_angles, sizeof(servos));
    ml_snap = s_ml_telemetry;
    portEXIT_CRITICAL(&s_state_spinlock);

    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_sram  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    printf("--------------------------------------------------------------------------------\n");
    printf("[Cycle %4lu] ADC1 Raw: [%4u, %4u, %4u, %4u] | mV: [%4u, %4u, %4u, %4u]\n",
           (unsigned long)cycle_num,
           adc_raw[0], adc_raw[1], adc_raw[2], adc_raw[3],
           adc_mv[0], adc_mv[1], adc_mv[2], adc_mv[3]);
    printf("  ML (Core 1) | Active: %-9s | Pred: %-9s (%5.1f%%) | %lld us | Inf: %lu | Drop: %lu\n",
           kGesturePoses[ml_snap.active_gesture].label,
           kGesturePoses[ml_snap.last_pred_gesture].label,
           ml_snap.confidence * 100.0f,
           ml_snap.total_us,
           (unsigned long)ml_snap.inference_count,
           (unsigned long)s_ml_dropped_frames.load(std::memory_order_relaxed));
    printf("  I2C PCA9685 | %s | Angles: [%3u,%3u,%3u,%3u,%3u,%3u] | Tx/Err: %lu/%lu (%lld us)\n",
           s_pca9685_online ? "ONLINE " : "OFFLINE",
           servos[0], servos[1], servos[2], servos[3], servos[4], servos[5],
           (unsigned long)s_i2c_tx_count, (unsigned long)s_i2c_err_count, s_last_i2c_tx_us);
    printf("  BLE (Core 0)| State: %-4s | Notify Pkts: %lu | LightSleep: %5.1f%% (%lu wakes)\n",
           (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? "CONN" : "ADV",
           (unsigned long)s_ble_notify_count.load(std::memory_order_relaxed),
           sleep_pct, (unsigned long)delta_sleep_cnt);
    printf("  Memory      | SRAM Free: %lu B (Min: %lu B) | PSRAM Free: %lu B\n",
           (unsigned long)free_sram, (unsigned long)min_sram, (unsigned long)free_psram);
}

static void daq_ble_task(void * /*pvParameters*/)
{
    ESP_LOGI(TAG, "DAQ + BLE Task started on Core %d", xPortGetCoreID());

    // Wait for boot ML self-test on Core 1 to complete so logs don't interleave
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

#if !LOW_POWER_LIGHT_SLEEP_ENABLE
    ESP_ERROR_CHECK(adc_continuous_start(s_adc_handle));
#endif

    uint32_t cycle = 0;
    uint32_t log_reports = 0;
    int write_buf_idx = 0;
    int64_t last_report_us = esp_timer_get_time();
    int64_t prev_sleep_us = 0;
    uint32_t prev_sleep_cnt = 0;

    while (true) {
        cycle++;

        uint16_t avg_raw[NUM_EMG_CHANNELS] = {0};
        uint16_t avg_mv[NUM_EMG_CHANNELS]  = {0};
        if (acquire_adc_burst(avg_raw, avg_mv)) {
            portENTER_CRITICAL(&s_state_spinlock);
            memcpy(s_latest_adc_raw, avg_raw, sizeof(avg_raw));
            memcpy(s_latest_adc_mv, avg_mv, sizeof(avg_mv));
            portEXIT_CRITICAL(&s_state_spinlock);

            // Send 4-channel ADC values + status over BLE to companion app
            ble_notify_adc_and_status(avg_raw);

            // Dispatch frozen 300x4 window snapshot to Core 1 if ML task is idle
            if (!s_ml_busy.exchange(true, std::memory_order_acquire)) {
                copy_ring_to_linear_buffer(s_ml_frames[write_buf_idx]);
                s_ml_ready_buf_idx = write_buf_idx;
                write_buf_idx ^= 1;
                if (s_ml_task_handle) {
                    xTaskNotifyGive(s_ml_task_handle);
                }
            } else {
                s_ml_dropped_frames.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            ESP_LOGW(TAG, "ADC burst acquisition timed out");
        }

        if ((cycle % LOG_EVERY_N_CYCLES) == 0) {
            int64_t now_us = esp_timer_get_time();
            print_benchmark_status(cycle, now_us - last_report_us, &prev_sleep_us, &prev_sleep_cnt);
            last_report_us = now_us;
            log_reports++;

            if ((log_reports % DUMP_PM_LOCKS_EVERY_N_LOGS) == 0) {
                printf("\n>>> ESP-PM Lock & Residency Report <<<\n");
                esp_pm_dump_locks(stdout);
                printf("\n");
            }
            flush_uart_console();
        }

#if LOW_POWER_LIGHT_SLEEP_ENABLE
        // Both cores block here -> Tickless Idle enters Automatic Light Sleep
        vTaskDelay(pdMS_TO_TICKS(CYCLE_SLEEP_MS));
#endif
    }
}

/* ============================================================================
 * APPLICATION ENTRY POINT
 * ============================================================================ */

extern "C" void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n================================================================================\n");
    printf("   GatorGrasp Full-System Dual-Core + Low-Power Integration Test (ESP32-S3)\n");
    printf("================================================================================\n");
    printf(" Model:   %s (ID %d) | Window: %d x %d @ %d Hz | Classes: %d\n",
           EI_CLASSIFIER_PROJECT_NAME, EI_CLASSIFIER_PROJECT_ID,
           EI_CLASSIFIER_RAW_SAMPLE_COUNT, EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME,
           EI_CLASSIFIER_FREQUENCY, EI_CLASSIFIER_LABEL_COUNT);
    printf(" ADC1:    GPIOs %d, %d, %d, %d | Mode: %s\n",
           kEmgGpios[0], kEmgGpios[1], kEmgGpios[2], kEmgGpios[3],
           LOW_POWER_LIGHT_SLEEP_ENABLE ? "Duty-Cycled Burst + Light Sleep" : "Continuous 1kHz + DFS");
    printf(" I2C:     PCA9685 @ 0x%02X (SDA=GPIO%d, SCL=GPIO%d, %d Hz) -> %d MG995 Servos\n",
           PCA9685_I2C_ADDR, (int)I2C_MASTER_SDA_IO, (int)I2C_MASTER_SCL_IO,
           I2C_MASTER_FREQ_HZ, NUM_SERVOS);
    printf(" BLE:     NimBLE 'GatorGrasp' (4-ch ADC Notify + 6-ch Servo Override + Calib)\n");
    printf("================================================================================\n\n");
    flush_uart_console();

    // 1. Initialize NVS (required for BLE PHY calibration data)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Power Management (DFS + Automatic Light Sleep)
    power_management_init();

    // 3. Initialize I2C Master & Dorhea PCA9685 Servo Driver
    pca9685_i2c_init();

    // 4. Initialize 4-Channel ADC1 DMA + Curve Fitting Calibration
    adc_init_all();

    // 5. Initialize NimBLE GATT Server (Core 0)
    ble_init_all();

    // 6. Launch Dual-Core Application Tasks
    // Core 0: 4-ch ADC DAQ + BLE Notifications + Benchmark Monitor
    xTaskCreatePinnedToCore(daq_ble_task, "daq_ble_task", 6144, nullptr, 4,
                            &s_daq_task_handle, CORE_DAQ_BLE);

    // Core 1: Edge Impulse ML Classifier + Gesture Debounce + PCA9685 I2C Actuation
    xTaskCreatePinnedToCore(ml_control_task, "ml_ctrl_task", 8192, nullptr, 5,
                            &s_ml_task_handle, CORE_ML_CONTROL);
}
