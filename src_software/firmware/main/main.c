// GATT service (UUIDs match app/src/ble/uuids.js):
//     EMG   (read, notify): 3 x uint16 LE, raw ADC counts, 50 Hz
//     SERVO (write)       : first byte = angle 0..180 (extra bytes ignored)
//     CALIB (read, write) : 12 bytes, kept in RAM only (resets on reboot)

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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

/* ---------- Config ---------- */
#define SERVO_GPIO   4
#define SERVO_MIN_US 500     /* check your servo's datasheet */
#define SERVO_MAX_US 2500
#define NUM_EMG      3
static const adc_channel_t EMG_CH[NUM_EMG] = { ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2 }; /* GPIO1,2,3 */

/* ---------- State ---------- */
static uint16_t emg[NUM_EMG];
static uint8_t  calib[12] = { 0,0,0xFF,0x0F, 0,0,0xFF,0x0F, 0,0,0xFF,0x0F };  /* min 0, max 4095 x3 */
static adc_oneshot_unit_handle_t adc1;
static uint8_t  own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t emg_handle;
static bool     emg_notify_on = false;

/* ---------- UUIDs ---------- */
#define GG_UUID(n) BLE_UUID128_INIT(0x4e,0x3d,0x2c,0x1b,0x8a,0x6f,0x3e,0x9c,0x0a,0x4d,0x2e,0x7b,(n),0x00,0x1c,0x5a)
static const ble_uuid128_t svc_uuid   = GG_UUID(0x01);
static const ble_uuid128_t emg_uuid   = GG_UUID(0x02);
static const ble_uuid128_t servo_uuid = GG_UUID(0x03);
static const ble_uuid128_t calib_uuid = GG_UUID(0x04);

/* ---------- Servo (LEDC, 50 Hz, 14-bit) ---------- */
static void servo_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = SERVO_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0 };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void servo_set(uint8_t angle)
{
    if (angle > 180) angle = 180;
    uint32_t us   = SERVO_MIN_US + (uint32_t)angle * (SERVO_MAX_US - SERVO_MIN_US) / 180;
    uint32_t duty = us * ((1u << 14) - 1) / 20000u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "servo -> %d deg (%lu us)", angle, (unsigned long)us);
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

/* ---------- GATT callbacks ---------- */
static int emg_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, emg, sizeof emg) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int servo_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t angle; uint16_t len;
    if (OS_MBUF_PKTLEN(ctxt->om) < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, &angle, 1, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
    servo_set(angle);
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

    struct ble_hs_adv_fields r = {0};
    r.uuids128 = (ble_uuid128_t *)&svc_uuid; r.num_uuids128 = 1; r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND; p.disc_mode = BLE_GAP_DISC_MODE_GEN;
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

static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

/* ---------- EMG sampling + notify (50 Hz) ---------- */
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
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();   /* required by the BLE stack */
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    servo_init();
    servo_set(90);
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