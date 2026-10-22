/*
 * GatorGrasp firmware (barebones) - ESP32-S3, ESP-IDF >= 5.2, NimBLE
 *
 * GATT service (custom 128-bit UUIDs, keep in sync with app/src/ble/uuids.js):
 *   EMG    (read, notify) : 3 x uint16 LE  - raw ADC counts from the 3 MyoWare sensors
 *   SERVO  (write)        : 6 x uint8      - manual servo angles 0..180 (overrides EMG control for 2 s)
 *   CALIB  (read, write)  : 3 x {uint16 min, uint16 max} LE = 12 bytes, persisted in NVS
 *
 * The ESP32 does NOT need Wi-Fi: the phone bridges BLE <-> Firebase.
 * (Wi-Fi + BLE can coexist on the S3 if you ever want OTA updates.)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "gatorgrasp";

/* ---------- Pin map: EDIT TO MATCH THE SCHEMATIC ---------- */
#define NUM_EMG    3
#define NUM_SERVO  6
static const adc_channel_t EMG_CH[NUM_EMG]   = { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2 }; /* GPIO1,2,3 */
static const int SERVO_GPIO[NUM_SERVO]       = { 4, 5, 6, 7, 15, 16 };

#define SERVO_MIN_US 500
#define SERVO_MAX_US 2500
#define SAMPLE_PERIOD_MS 20          /* 50 Hz */
#define MANUAL_TIMEOUT_US 2000000

/* ---------- Shared state ---------- */
static uint16_t emg[NUM_EMG];
static uint8_t  servo_angle[NUM_SERVO];
static struct { uint16_t min, max; } calib[NUM_EMG];
static int64_t  last_manual_us = 0;

static adc_oneshot_unit_handle_t adc1;
static uint8_t  own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t emg_handle;
static bool     emg_notify_on = false;

/* ---------- UUIDs (little-endian bytes of 5a1c00XX-7b2e-4d0a-9c3e-6f8a1b2c3d4e) ---------- */
#define GG_UUID(n) BLE_UUID128_INIT(0x4e,0x3d,0x2c,0x1b,0x8a,0x6f,0x3e,0x9c,0x0a,0x4d,0x2e,0x7b,(n),0x00,0x1c,0x5a)
static const ble_uuid128_t svc_uuid   = GG_UUID(0x01);
static const ble_uuid128_t emg_uuid   = GG_UUID(0x02);
static const ble_uuid128_t servo_uuid = GG_UUID(0x03);
static const ble_uuid128_t calib_uuid = GG_UUID(0x04);

/* ---------- Servos (LEDC, 50 Hz, 14-bit) ---------- */
static void servos_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    for (int i = 0; i < NUM_SERVO; i++) {
        ledc_channel_config_t c = {
            .gpio_num = SERVO_GPIO[i], .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i, .timer_sel = LEDC_TIMER_0,
            .duty = 0, .hpoint = 0 };
        ESP_ERROR_CHECK(ledc_channel_config(&c));
        servo_angle[i] = 90;
    }
}

static void servo_set(int i, uint8_t angle)
{
    if (angle > 180) angle = 180;
    servo_angle[i] = angle;
    uint32_t us   = SERVO_MIN_US + (uint32_t)angle * (SERVO_MAX_US - SERVO_MIN_US) / 180;
    uint32_t duty = us * ((1u << 14) - 1) / 20000u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
}

/* ---------- ADC ---------- */
static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&u, &adc1));
    adc_oneshot_chan_cfg_t c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    for (int i = 0; i < NUM_EMG; i++)
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1, EMG_CH[i], &c));
}

/* ---------- NVS calibration ---------- */
static void calib_load(void)
{
    for (int i = 0; i < NUM_EMG; i++) { calib[i].min = 0; calib[i].max = 4095; }
    nvs_handle_t h;
    if (nvs_open("gg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof calib;
        nvs_get_blob(h, "calib", calib, &len);
        nvs_close(h);
    }
}
static void calib_save(void)
{
    nvs_handle_t h;
    if (nvs_open("gg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "calib", calib, sizeof calib);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- GATT access callbacks ---------- */
static int emg_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, emg, sizeof emg) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    return BLE_ATT_ERR_UNLIKELY;
}

static int servo_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != NUM_SERVO) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t buf[NUM_SERVO]; uint16_t len;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    for (int i = 0; i < NUM_SERVO; i++) servo_set(i, buf[i]);
    last_manual_us = esp_timer_get_time();
    return 0;
}

static int calib_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return os_mbuf_append(ctxt->om, calib, sizeof calib) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof calib) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint16_t len;
        if (ble_hs_mbuf_to_flat(ctxt->om, calib, sizeof calib, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        calib_save();
        ESP_LOGI(TAG, "calibration updated");
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = &emg_uuid.u, .access_cb = emg_access, .val_handle = &emg_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
        { .uuid = &servo_uuid.u, .access_cb = servo_access,
          .flags = BLE_GATT_CHR_F_WRITE },
        { .uuid = &calib_uuid.u, .access_cb = calib_access,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
        { 0 } } },
    { 0 }
};

/* ---------- GAP ---------- */
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
    f.name = (uint8_t *)name; f.name_len = strlen(name); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields r = {0};   /* 128-bit UUID goes in scan response (too big for adv pkt) */
    r.uuids128 = (ble_uuid128_t *)&svc_uuid; r.num_uuids128 = 1; r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGE(TAG, "adv start failed %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
}

static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

/* ---------- Sensor / control loop ---------- */
static uint8_t normalize_to_angle(int ch)
{
    int32_t range = (int32_t)calib[ch].max - calib[ch].min;
    if (range <= 0) return 0;
    int32_t v = ((int32_t)emg[ch] - calib[ch].min) * 180 / range;
    return (uint8_t)(v < 0 ? 0 : (v > 180 ? 180 : v));
}

static void sensor_task(void *p)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        for (int i = 0; i < NUM_EMG; i++) {
            int raw = 0;
            adc_oneshot_read(adc1, EMG_CH[i], &raw);
            emg[i] = (uint16_t)raw;
        }

        if (emg_notify_on && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(emg, sizeof emg);
            if (om) ble_gatts_notify_custom(conn_handle, emg_handle, om);
        }

        /* PLACEHOLDER mapping until Integration milestone:
         * ch0 -> fingers (servos 0-4), ch1 -> wrist (servo 5). Skipped while app is driving servos. */
        if (esp_timer_get_time() - last_manual_us > MANUAL_TIMEOUT_US) {
            uint8_t grip = normalize_to_angle(0);
            for (int i = 0; i < 5; i++) servo_set(i, grip);
            servo_set(5, normalize_to_angle(1));
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    calib_load();
    servos_init();
    adc_init();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set("GatorGrasp");
    nimble_port_freertos_init(host_task);

    xTaskCreate(sensor_task, "sensor", 4096, NULL, 5, NULL);
}
