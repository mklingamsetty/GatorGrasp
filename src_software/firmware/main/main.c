#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "gatorgrasp";

/* ---------------- Pin configuration ---------------- */
#define SERVO_GPIO              8
#define NUM_SERVOS              1

/* 4 EMG inputs, all on ADC1 (ADC2 is unreliable while the BLE radio is on).
 * GPIO 4..7 sit next to each other on the header and are not strapping pins.
 *   EMG index:  0       1       2       3
 *   GPIO:       4       5       6       7
 *   ADC1 ch:    3       4       5       6
 * Unconnected inputs float and read noise; tie unused ones to GND. */
#define NUM_EMG                 4
#define EMG_ADC_UNIT            ADC_UNIT_1
static const adc_channel_t emg_channels[NUM_EMG] = {
    ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6,
};
static const int emg_gpios[NUM_EMG] = { 4, 5, 6, 7 };

/* Channel that drives the flex/relax servo logic: GPIO 7, the original
 * MyoWare wiring. */
#define FLEX_EMG_INDEX          3

/* ---------------- Servo PWM (MG995) ---------------- */
#define SERVO_LEDC_TIMER        LEDC_TIMER_0
#define SERVO_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_RES          LEDC_TIMER_13_BIT
#define SERVO_PWM_FREQ_HZ       50
#define SERVO_PERIOD_US         20000
#define SERVO_MIN_PULSE_US      500     /* 0 deg   */
#define SERVO_MAX_PULSE_US      2400    /* 180 deg */

/* ---------------- EMG thresholds & timing ---------------- */
#define FLEX_THRESHOLD_HIGH     350     /* flex trigger  (12-bit ADC, 0..4095) */
#define RELAX_THRESHOLD_LOW     250     /* relax trigger (hysteresis)          */
/* Per channel: read ADC_SAMPLES_PER_READ samples, drop the min and max,
 * average the rest (trimmed mean rejects single-sample spikes). */
#define ADC_SAMPLES_PER_READ    10
#define POLL_INTERVAL_MS        50      /* 20 Hz sample + BLE notify rate      */
#define LOG_EVERY_N_SAMPLES     20      /* periodic EMG log once per second    */

/* ---------------- State ---------------- */
static const int servo_gpios[NUM_SERVOS] = { SERVO_GPIO };
static const ledc_channel_t servo_channels[NUM_SERVOS] = { LEDC_CHANNEL_0 };

static adc_oneshot_unit_handle_t adc_handle;

/* BLE EMG packet: NUM_EMG x uint16 little-endian (8 bytes), one per channel
 * in emg_channels[] order. The ESP32-S3 is little-endian, so the array is
 * sent as-is. */
static uint16_t emg[NUM_EMG];

/* Calibration: per channel { min uint16 LE, max uint16 LE } = 4 bytes each.
 * Stored only; not used by the firmware logic yet. Default min 0, max 4095. */
#define CALIB_BYTES             (NUM_EMG * 4)
static uint8_t  calib[CALIB_BYTES] = {
    0,0,0xFF,0x0F,  0,0,0xFF,0x0F,  0,0,0xFF,0x0F,  0,0,0xFF,0x0F,
};

static uint8_t  own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t emg_handle;
static bool     emg_notify_on = false;

/* ---------------- UUIDs ---------------- */
#define GG_UUID(n) BLE_UUID128_INIT(0x4e,0x3d,0x2c,0x1b,0x8a,0x6f,0x3e,0x9c,0x0a,0x4d,0x2e,0x7b,(n),0x00,0x1c,0x5a)
static const ble_uuid128_t svc_uuid   = GG_UUID(0x01);
static const ble_uuid128_t emg_uuid   = GG_UUID(0x02);
static const ble_uuid128_t servo_uuid = GG_UUID(0x03);
static const ble_uuid128_t calib_uuid = GG_UUID(0x04);

/* ======================================================================
 * Servos
 * ====================================================================== */
static uint32_t servo_angle_to_duty(int angle)
{
    if (angle < 0)   angle = 0;
    if (angle > 180) angle = 180;

    uint32_t pulse_us = SERVO_MIN_PULSE_US +
        ((SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * (uint32_t)angle) / 180;

    uint32_t max_duty = (1u << SERVO_LEDC_RES);
    return (pulse_us * max_duty) / SERVO_PERIOD_US;
}

static void servos_init(void)
{
    /* XTAL clock keeps the PWM frequency stable if the CPU frequency scales */
    ledc_timer_config_t timer_conf = {
        .speed_mode      = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_LEDC_RES,
        .timer_num       = SERVO_LEDC_TIMER,
        .freq_hz         = SERVO_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_USE_XTAL_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    uint32_t initial_duty = servo_angle_to_duty(0);
    for (int i = 0; i < NUM_SERVOS; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = servo_gpios[i],
            .speed_mode = SERVO_LEDC_MODE,
            .channel    = servo_channels[i],
            .timer_sel  = SERVO_LEDC_TIMER,
            .duty       = initial_duty,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
}

/* Set all servos to the same angle (0..180) */
static void servos_set_angle(int angle)
{
    uint32_t duty = servo_angle_to_duty(angle);
    for (int i = 0; i < NUM_SERVOS; i++) {
        ESP_ERROR_CHECK(ledc_set_duty(SERVO_LEDC_MODE, servo_channels[i], duty));
        ESP_ERROR_CHECK(ledc_update_duty(SERVO_LEDC_MODE, servo_channels[i]));
    }
}

/* ======================================================================
 * MyoWare / ADC
 * ====================================================================== */
static void emg_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = EMG_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,   /* ~0..3.1 V full scale */
    };
    for (int i = 0; i < NUM_EMG; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, emg_channels[i], &chan_cfg));
    }
}

/* Trimmed-mean read of one channel; returns -1 on ADC error.
 * The first conversion after switching channels is discarded: the ADC's
 * sample capacitor still holds charge from the previous pin, which otherwise
 * bleeds one channel's value into the next (crosstalk). */
static int emg_read_channel(adc_channel_t ch)
{
    int raw = 0;
    if (adc_oneshot_read(adc_handle, ch, &raw) != ESP_OK) return -1;  /* settle */

    int sum = 0, lo = 4095, hi = 0;
    for (int i = 0; i < ADC_SAMPLES_PER_READ; i++) {
        if (adc_oneshot_read(adc_handle, ch, &raw) != ESP_OK) return -1;
        sum += raw;
        if (raw < lo) lo = raw;
        if (raw > hi) hi = raw;
    }
    return (sum - lo - hi) / (ADC_SAMPLES_PER_READ - 2);
}

/* Reads all channels into out[]; returns false if any read failed. */
static bool emg_read_all(uint16_t out[NUM_EMG])
{
    for (int i = 0; i < NUM_EMG; i++) {
        int v = emg_read_channel(emg_channels[i]);
        if (v < 0) return false;
        out[i] = (uint16_t)v;
    }
    return true;
}

/* ======================================================================
 * GATT
 * ====================================================================== */
static int emg_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, emg, sizeof emg) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Manual servo override: write 1 byte (0..180) -> servo.
 * The EMG state machine takes over again on the next flex/relax transition. */
static int servo_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[16];
    uint16_t len;
    uint16_t pktlen = OS_MBUF_PKTLEN(ctxt->om);
    if (pktlen < 1 || pktlen > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    servos_set_angle(buf[0]);
    ESP_LOGI(TAG, "BLE servo override -> %d deg", buf[0]);
    return 0;
}

static int calib_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, calib, sizeof calib) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof calib) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t len;
    return ble_hs_mbuf_to_flat(ctxt->om, calib, sizeof calib, &len) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = &emg_uuid.u, .access_cb = emg_access, .val_handle = &emg_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
        { .uuid = &servo_uuid.u, .access_cb = servo_access, .flags = BLE_GATT_CHR_F_WRITE },
        { .uuid = &calib_uuid.u, .access_cb = calib_access,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
        { 0 } } },
    { 0 }
};

/* ======================================================================
 * GAP
 * ====================================================================== */
static void advertise(void);

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", e->connect.status);
        if (e->connect.status == 0) conn_handle = e->connect.conn_handle;
        else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect");
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        emg_notify_on = false;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == emg_handle) emg_notify_on = e->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields r = {0};
    r.uuids128 = (ble_uuid128_t *)&svc_uuid;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv start failed %d", rc);
    else    ESP_LOGI(TAG, "advertising as GatorGrasp");
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
}

static void host_task(void *p)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ======================================================================
 * EMG task: sample -> clasp state machine -> BLE notify
 * ====================================================================== */
static void emg_task(void *p)
{
    bool is_flexed = false;
    int  log_count = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        uint16_t sample[NUM_EMG];
        if (emg_read_all(sample)) {
            memcpy(emg, sample, sizeof emg);
            int val = sample[FLEX_EMG_INDEX];

            /* Hysteresis state machine: flex -> 180 deg, relax -> 0 deg */
            if (!is_flexed && val >= FLEX_THRESHOLD_HIGH) {
                is_flexed = true;
                servos_set_angle(180);
                ESP_LOGI(TAG, "FLEX (ADC: %d) -> servo 180 deg", val);
            } else if (is_flexed && val <= RELAX_THRESHOLD_LOW) {
                is_flexed = false;
                servos_set_angle(0);
                ESP_LOGI(TAG, "RELAX (ADC: %d) -> servo 0 deg", val);
            }

            if (++log_count >= LOG_EVERY_N_SAMPLES) {
                log_count = 0;
                ESP_LOGI(TAG, "EMG GPIO4:%4u GPIO5:%4u GPIO6:%4u GPIO7:%4u | %s",
                         sample[0], sample[1], sample[2], sample[3],
                         is_flexed ? "FLEX (180)" : "RELAX (0)");
            }

            /* Send over BLE if a client is subscribed */
            if (emg_notify_on && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(sample, sizeof sample);
                if (om) ble_gatts_notify_custom(conn_handle, emg_handle, om);
            }
        } else {
            ESP_LOGW(TAG, "ADC read failed");
        }

        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ======================================================================
 * Power management (DFS only; light sleep is disabled because it would
 * interfere with the continuous EMG polling and BLE link)
 * ====================================================================== */
static void low_power_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) ESP_LOGI(TAG, "Dynamic frequency scaling enabled");
    else               ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
#endif
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();   /* required by the BLE stack */
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    servos_init();            /* servos start at 0 deg (relaxed) */
    emg_adc_init();
    low_power_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set("GatorGrasp");
    nimble_port_freertos_init(host_task);

    xTaskCreate(emg_task, "emg", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Initialized. Servos at 0 deg. EMG on GPIO %d/%d/%d/%d, flex logic on GPIO %d",
             emg_gpios[0], emg_gpios[1], emg_gpios[2], emg_gpios[3], emg_gpios[FLEX_EMG_INDEX]);
}