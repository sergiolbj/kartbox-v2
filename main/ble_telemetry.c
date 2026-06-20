/*
 * ble_telemetry.c - ver ble_telemetry.h pro contexto e formato do pacote.
 */
#include "ble_telemetry.h"
#include "config.h"
#include "gps.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_telemetry";

/* UUIDs proprios do kartbox (nao sao de nenhum registro oficial, so
 * precisam ser unicos o suficiente pra nao colidir com outro
 * dispositivo BLE por perto). Servico e characteristic so diferem no
 * segundo grupo (0001 / 0002). */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_telemetry_chr_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x02, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_telemetry_val_handle;
static volatile bool s_subscribed = false;
static ble_telemetry_packet_t s_last_packet;
static char s_device_name[32] = BLE_DEVICE_NAME_DEFAULT;

static void start_advertising(void);

/* ---------------------------------------------------------------------
 * GATT - characteristic de telemetria (notify + read)
 * --------------------------------------------------------------------- */
static int gatt_access_telemetry(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    int rc = os_mbuf_append(ctxt->om, &s_last_packet, sizeof(s_last_packet));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def s_telemetry_chrs[] = {
    {
        .uuid       = &s_telemetry_chr_uuid.u,
        .access_cb  = gatt_access_telemetry,
        .val_handle = &s_telemetry_val_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_telemetry_chrs,
    },
    { 0 },
};

/* ---------------------------------------------------------------------
 * GAP - advertise, conexao, subscribe
 * --------------------------------------------------------------------- */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Central conectou (handle %d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "Falha ao conectar (status %d), voltando a anunciar",
                      event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Central desconectou (motivo %d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Telemetria %s", s_subscribed ? "inscrita" : "cancelada");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negociado: %d bytes", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields falhou (rc=%d)", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                            gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start falhou (rc=%d)", rc);
    }
}

static void ble_on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_advertising();
    ESP_LOGI(TAG, "BLE sincronizado, anunciando como \"%s\"", s_device_name);
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host resetou (motivo %d)", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------------
 * Task de telemetria - empacota e notifica em BLE_TELEMETRY_HZ
 * --------------------------------------------------------------------- */
static void pack_telemetry(const gps_sample_t *s, ble_telemetry_packet_t *out)
{
    out->lap_time_ms  = s->lap_time_ms;
    out->delta_ms      = s->last_delta_ms;
    out->best_lap_ms   = s->best_lap_ms;
    out->speed_kmh_x10 = (uint16_t)(s->speed_kmh * 10.0f + 0.5f);
    out->heading_x10   = (uint16_t)(s->heading_deg * 10.0f + 0.5f);
    out->lap_number    = (uint16_t)s->lap_number;
    out->satellites    = s->satellites;
    out->fix_valid     = s->fix_valid ? 1 : 0;
    out->lat_x1e6       = (int32_t)(s->lat * 1e6);
    out->lon_x1e6       = (int32_t)(s->lon * 1e6);
}

static void ble_telemetry_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / BLE_TELEMETRY_HZ);
    gps_sample_t sample;

    for (;;) {
        gps_get_latest(&sample);
        pack_telemetry(&sample, &s_last_packet);

        if (s_subscribed && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_last_packet, sizeof(s_last_packet));
            if (om) {
                int rc = ble_gatts_notify_custom(s_conn_handle, s_telemetry_val_handle, om);
                if (rc != 0) {
                    ESP_LOGW(TAG, "Notify falhou (rc=%d)", rc);
                }
            }
        }
        vTaskDelay(period);
    }
}

/* ---------------------------------------------------------------------
 * API publica
 * --------------------------------------------------------------------- */
void ble_telemetry_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init falhou (%s) - confira CONFIG_BT_ENABLED/NIMBLE no sdkconfig", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_device_name);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg falhou (rc=%d)", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs falhou (rc=%d)", rc);
        return;
    }

    nimble_port_freertos_init(ble_host_task);
    xTaskCreate(ble_telemetry_task, "ble_telem", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE telemetry pronto");
}

void ble_telemetry_set_device_name(const char *name)
{
    if (!name || !name[0]) return;
    strncpy(s_device_name, name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';
    ble_svc_gap_device_name_set(s_device_name);
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_adv_stop();
        start_advertising();
    }
}

bool ble_telemetry_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
