/*
 * ble_telemetry.c - kartbox v2
 * Ver ble_telemetry.h para arquitetura e protocolo completo.
 */
#include "ble_telemetry.h"
#include "config.h"
#include "gps.h"
#include "settings.h"
#include "track_manager.h"
#include "sd_logger.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_telemetry";

/* ── UUIDs ──────────────────────────────────────────────────────────────
 * String canonica (big-endian): a1b2c3d4-XXXX-4a5b-8c9d-0123456789ab
 * BLE_UUID128_INIT recebe bytes little-endian (LSB primeiro).
 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_telem_chr_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_cmd_chr_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x03, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static const ble_uuid128_t s_rsp_chr_uuid = BLE_UUID128_INIT(
    0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x9d, 0x8c,
    0x5b, 0x4a, 0x04, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

/* ── Estado de conexao ──────────────────────────────────────────────── */
static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle       = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_telem_val_handle;
static uint16_t s_cmd_val_handle;
static uint16_t s_rsp_val_handle;
static volatile bool s_telem_subscribed = false;
static volatile bool s_rsp_subscribed   = false;
static ble_telemetry_packet_t s_last_packet;
static char s_device_name[32] = BLE_DEVICE_NAME_DEFAULT;

/* ── Fila de comandos ───────────────────────────────────────────────── */
#define CMD_QUEUE_LEN       4
#define CMD_MAX_PAYLOAD     512

typedef struct {
    uint8_t  cmd;
    uint8_t  seq;
    uint8_t  payload[CMD_MAX_PAYLOAD];
    uint16_t payload_len;
} ble_cmd_item_t;

static QueueHandle_t s_cmd_queue;

/* ── Forward declarations ───────────────────────────────────────────── */
static void start_advertising(void);
static void send_rsp(uint8_t cmd, uint8_t seq, uint8_t status,
                     const void *data, uint16_t data_len);
static void send_rsp_chunked(uint8_t cmd, uint8_t seq,
                              const uint8_t *data, uint32_t data_len);

/* ══════════════════════════════════════════════════════════════════════
 * HANDLERS DE COMANDO
 * ══════════════════════════════════════════════════════════════════════ */

static void handle_get_settings(uint8_t seq)
{
    ble_settings_payload_t p = {
        .utc_offset_min = settings_get_utc_offset_min(),
        .gate_radius_m  = settings_get_gate_radius_m(),
        .min_lap_ms     = settings_get_min_lap_time_ms(),
    };
    strncpy(p.ble_name,  settings_get_ble_name(),      sizeof(p.ble_name)  - 1);
    strncpy(p.wifi_pass, settings_get_wifi_password(), sizeof(p.wifi_pass) - 1);
    send_rsp(BLE_CMD_GET_SETTINGS, seq, BLE_RSP_OK, &p, sizeof(p));
}

static void handle_set_settings(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(ble_settings_payload_t)) {
        send_rsp(BLE_CMD_SET_SETTINGS, seq, BLE_RSP_ERR, "payload curto", 13);
        return;
    }
    const ble_settings_payload_t *p = (const ble_settings_payload_t *)payload;
    settings_set_utc_offset_min(p->utc_offset_min);
    settings_set_gate_radius_m(p->gate_radius_m);
    settings_set_min_lap_time_ms(p->min_lap_ms);
    if (p->ble_name[0])  settings_set_ble_name(p->ble_name);
    if (p->wifi_pass[0]) settings_set_wifi_password(p->wifi_pass);
    send_rsp(BLE_CMD_SET_SETTINGS, seq, BLE_RSP_OK, NULL, 0);
    ESP_LOGI(TAG, "Settings atualizados via BLE");
}

static void handle_list_tracks(uint8_t seq)
{
    static char names[TRACK_LIST_MAX][TRACK_NAME_MAX];
    int count = track_manager_list(names, TRACK_LIST_MAX);

    if (count == 0) {
        send_rsp(BLE_CMD_LIST_TRACKS, seq, BLE_RSP_OK, "", 0);
        return;
    }

    /* Monta buffer "nome1\nnome2\n..." */
    static char buf[TRACK_LIST_MAX * (TRACK_NAME_MAX + 1)];
    uint32_t pos = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s\n", names[i]);
        if (n > 0) pos += n;
    }
    send_rsp_chunked(BLE_CMD_LIST_TRACKS, seq, (uint8_t *)buf, pos);
}

static void handle_get_track(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len == 0) {
        send_rsp(BLE_CMD_GET_TRACK, seq, BLE_RSP_ERR, "nome vazio", 10);
        return;
    }
    char name[TRACK_NAME_MAX] = {0};
    strncpy(name, (const char *)payload, sizeof(name) - 1);

    track_config_t cfg;
    if (!track_manager_load(name, &cfg)) {
        send_rsp(BLE_CMD_GET_TRACK, seq, BLE_RSP_ERR, "nao encontrada", 14);
        return;
    }

    ble_track_payload_t tp = {
        .magic          = cfg.magic,
        .finish_lat     = cfg.finish.lat,
        .finish_lon     = cfg.finish.lon,
        .finish_heading = cfg.finish.heading_deg,
        .finish_is_set  = cfg.finish.is_set ? 1 : 0,
        .s0_lat         = cfg.sectors[0].lat,
        .s0_lon         = cfg.sectors[0].lon,
        .s0_heading     = cfg.sectors[0].heading_deg,
        .s0_is_set      = cfg.sectors[0].is_set ? 1 : 0,
        .s1_lat         = cfg.sectors[1].lat,
        .s1_lon         = cfg.sectors[1].lon,
        .s1_heading     = cfg.sectors[1].heading_deg,
        .s1_is_set      = cfg.sectors[1].is_set ? 1 : 0,
    };
    strncpy(tp.name, cfg.name, sizeof(tp.name) - 1);
    send_rsp(BLE_CMD_GET_TRACK, seq, BLE_RSP_OK, &tp, sizeof(tp));
}

static void handle_put_track(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len < sizeof(ble_track_payload_t)) {
        send_rsp(BLE_CMD_PUT_TRACK, seq, BLE_RSP_ERR, "payload curto", 13);
        return;
    }
    const ble_track_payload_t *tp = (const ble_track_payload_t *)payload;

    track_config_t cfg = { .magic = TRACK_MAGIC };
    strncpy(cfg.name, tp->name, sizeof(cfg.name) - 1);
    cfg.finish.lat         = tp->finish_lat;
    cfg.finish.lon         = tp->finish_lon;
    cfg.finish.heading_deg = tp->finish_heading;
    cfg.finish.is_set      = tp->finish_is_set != 0;
    cfg.sectors[0].lat         = tp->s0_lat;
    cfg.sectors[0].lon         = tp->s0_lon;
    cfg.sectors[0].heading_deg = tp->s0_heading;
    cfg.sectors[0].is_set      = tp->s0_is_set != 0;
    cfg.sectors[1].lat         = tp->s1_lat;
    cfg.sectors[1].lon         = tp->s1_lon;
    cfg.sectors[1].heading_deg = tp->s1_heading;
    cfg.sectors[1].is_set      = tp->s1_is_set != 0;

    if (!track_manager_save(&cfg)) {
        send_rsp(BLE_CMD_PUT_TRACK, seq, BLE_RSP_ERR, "falha ao salvar", 15);
        return;
    }
    send_rsp(BLE_CMD_PUT_TRACK, seq, BLE_RSP_OK, NULL, 0);
    ESP_LOGI(TAG, "Pista '%s' salva via BLE", cfg.name);
}

static void handle_del_track(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len == 0) {
        send_rsp(BLE_CMD_DEL_TRACK, seq, BLE_RSP_ERR, "nome vazio", 10);
        return;
    }
    char name[TRACK_NAME_MAX] = {0};
    strncpy(name, (const char *)payload, sizeof(name) - 1);

    if (!track_manager_delete(name)) {
        send_rsp(BLE_CMD_DEL_TRACK, seq, BLE_RSP_ERR, "nao encontrada", 14);
        return;
    }
    send_rsp(BLE_CMD_DEL_TRACK, seq, BLE_RSP_OK, NULL, 0);
}

static void handle_list_sessions(uint8_t seq)
{
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);

    if (count == 0) {
        send_rsp(BLE_CMD_LIST_SESSIONS, seq, BLE_RSP_OK, "", 0);
        return;
    }

    static char buf[SD_MAX_SESSIONS_LISTED * (SD_SESSION_NAME_LEN + 1)];
    uint32_t pos = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s\n", sessions[i].filename);
        if (n > 0) pos += n;
    }
    send_rsp_chunked(BLE_CMD_LIST_SESSIONS, seq, (uint8_t *)buf, pos);
}

static void handle_get_session(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    if (len == 0) {
        send_rsp(BLE_CMD_GET_SESSION, seq, BLE_RSP_ERR, "nome vazio", 10);
        return;
    }

    char filename[SD_SESSION_NAME_LEN] = {0};
    strncpy(filename, (const char *)payload, sizeof(filename) - 1);

    /* Monta path completo */
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, filename);

    FILE *f = fopen(path, "r");
    if (!f) {
        send_rsp(BLE_CMD_GET_SESSION, seq, BLE_RSP_ERR, "arquivo nao encontrado", 22);
        return;
    }

    /* Le e envia em chunks de BLE_CHUNK_SIZE */
    static uint8_t chunk[BLE_CHUNK_SIZE];
    bool first = true;
    size_t n;

    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        bool more = !feof(f);
        uint8_t status = more ? BLE_RSP_MORE : BLE_RSP_OK;

        if (first) {
            ESP_LOGI(TAG, "Enviando sessao '%s' via BLE", filename);
            first = false;
        }

        send_rsp(BLE_CMD_GET_SESSION, seq, status, chunk, (uint16_t)n);

        if (more) {
            /* Pausa curta entre chunks pra nao saturar a fila de notify */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (first) {
        /* Arquivo existia mas estava vazio */
        send_rsp(BLE_CMD_GET_SESSION, seq, BLE_RSP_OK, NULL, 0);
    }

    fclose(f);
}

static void handle_del_all_sessions(uint8_t seq)
{
    sd_delete_all_sessions();
    send_rsp(BLE_CMD_DEL_ALL_SESSIONS, seq, BLE_RSP_OK, NULL, 0);
    ESP_LOGI(TAG, "Todas as sessoes apagadas via BLE");
}

/* ══════════════════════════════════════════════════════════════════════
 * DISPATCH E ENVIO
 * ══════════════════════════════════════════════════════════════════════ */

static void dispatch_cmd(const ble_cmd_item_t *item)
{
    switch (item->cmd) {
    case BLE_CMD_GET_SETTINGS:
        handle_get_settings(item->seq);
        break;
    case BLE_CMD_SET_SETTINGS:
        handle_set_settings(item->seq, item->payload, item->payload_len);
        break;
    case BLE_CMD_LIST_TRACKS:
        handle_list_tracks(item->seq);
        break;
    case BLE_CMD_GET_TRACK:
        handle_get_track(item->seq, item->payload, item->payload_len);
        break;
    case BLE_CMD_PUT_TRACK:
        handle_put_track(item->seq, item->payload, item->payload_len);
        break;
    case BLE_CMD_DEL_TRACK:
        handle_del_track(item->seq, item->payload, item->payload_len);
        break;
    case BLE_CMD_LIST_SESSIONS:
        handle_list_sessions(item->seq);
        break;
    case BLE_CMD_GET_SESSION:
        handle_get_session(item->seq, item->payload, item->payload_len);
        break;
    case BLE_CMD_DEL_ALL_SESSIONS:
        handle_del_all_sessions(item->seq);
        break;
    default:
        ESP_LOGW(TAG, "Comando desconhecido: 0x%02X", item->cmd);
        {
            uint8_t hdr[3] = { item->cmd, item->seq, BLE_RSP_ERR };
            send_rsp(item->cmd, item->seq, BLE_RSP_ERR, "cmd desconhecido", 16);
        }
        break;
    }
}

/* Envia uma resposta simples (cabe num unico notify) */
static void send_rsp(uint8_t cmd, uint8_t seq, uint8_t status,
                     const void *data, uint16_t data_len)
{
    if (!s_rsp_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    uint16_t total = 3 + data_len;  /* cmd + seq + status + data */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(NULL, 0);
    if (!om) return;

    uint8_t hdr[3] = { cmd, seq, status };
    if (os_mbuf_append(om, hdr, 3) != 0) { os_mbuf_free_chain(om); return; }
    if (data && data_len > 0) {
        if (os_mbuf_append(om, data, data_len) != 0) {
            os_mbuf_free_chain(om); return;
        }
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_rsp_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "send_rsp notify falhou (rc=%d)", rc);
    }
    (void)total;
}

/* Envia dados maiores que BLE_CHUNK_SIZE em multiplos notifies */
static void send_rsp_chunked(uint8_t cmd, uint8_t seq,
                              const uint8_t *data, uint32_t data_len)
{
    if (data_len == 0) {
        send_rsp(cmd, seq, BLE_RSP_OK, NULL, 0);
        return;
    }

    uint32_t offset = 0;
    while (offset < data_len) {
        uint32_t remain = data_len - offset;
        uint16_t chunk  = remain > BLE_CHUNK_SIZE ? BLE_CHUNK_SIZE : (uint16_t)remain;
        bool     more   = (offset + chunk) < data_len;

        send_rsp(cmd, seq, more ? BLE_RSP_MORE : BLE_RSP_OK,
                 data + offset, chunk);
        offset += chunk;

        if (more) vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ── Task de comandos ─────────────────────────────────────────────────── */
static void ble_cmd_task(void *arg)
{
    (void)arg;
    ble_cmd_item_t item;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &item, portMAX_DELAY) == pdTRUE) {
            dispatch_cmd(&item);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * GATT — CHARACTERISTICS
 * ══════════════════════════════════════════════════════════════════════ */

/* CHR 0001 — telemetria (notify + read) */
static int gatt_access_telem(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    int rc = os_mbuf_append(ctxt->om, &s_last_packet, sizeof(s_last_packet));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* CHR 0003 — comando (write) */
static int gatt_access_cmd(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    ble_cmd_item_t item = {0};
    os_mbuf_copydata(ctxt->om, 0, 1, &item.cmd);
    os_mbuf_copydata(ctxt->om, 1, 1, &item.seq);

    uint16_t payload_len = len - 2;
    if (payload_len > CMD_MAX_PAYLOAD) payload_len = CMD_MAX_PAYLOAD;
    if (payload_len > 0) {
        os_mbuf_copydata(ctxt->om, 2, payload_len, item.payload);
    }
    item.payload_len = payload_len;

    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Fila de comandos cheia, descartando cmd=0x%02X", item.cmd);
    }
    return 0;
}

/* CHR 0004 — resposta (notify apenas, sem read util) */
static int gatt_access_rsp(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    /* Leitura retorna vazio — respostas chegam so via notify */
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = &s_telem_chr_uuid.u,
        .access_cb  = gatt_access_telem,
        .val_handle = &s_telem_val_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid       = &s_cmd_chr_uuid.u,
        .access_cb  = gatt_access_cmd,
        .val_handle = &s_cmd_val_handle,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = &s_rsp_chr_uuid.u,
        .access_cb  = gatt_access_rsp,
        .val_handle = &s_rsp_val_handle,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

/* ══════════════════════════════════════════════════════════════════════
 * GAP — advertise, conexao, subscribe
 * ══════════════════════════════════════════════════════════════════════ */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_telem_subscribed = false;
            s_rsp_subscribed   = false;
            ESP_LOGI(TAG, "Central conectou (handle %d)", s_conn_handle);
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Central desconectou (motivo %d)", event->disconnect.reason);
        s_conn_handle      = BLE_HS_CONN_HANDLE_NONE;
        s_telem_subscribed = false;
        s_rsp_subscribed   = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telem_val_handle) {
            s_telem_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Telemetria %s", s_telem_subscribed ? "subscrita" : "cancelada");
        } else if (event->subscribe.attr_handle == s_rsp_val_handle) {
            s_rsp_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "RSP channel %s", s_rsp_subscribed ? "subscrito" : "cancelado");
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
    fields.flags              = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name               = (const uint8_t *)s_device_name;
    fields.name_len           = strlen(s_device_name);
    fields.name_is_complete   = 1;
    fields.uuids128           = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128       = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields falhou (rc=%d)", rc); return; }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_handler, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start falhou (rc=%d)", rc);
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

/* ══════════════════════════════════════════════════════════════════════
 * TASK DE TELEMETRIA (empacota GPS → notify 10Hz)
 * ══════════════════════════════════════════════════════════════════════ */

static void pack_telemetry(const gps_sample_t *s, ble_telemetry_packet_t *out)
{
    out->lap_time_ms    = s->lap_time_ms;
    out->delta_ms       = s->last_delta_ms;
    out->best_lap_ms    = s->best_lap_ms;
    out->speed_kmh_x10  = (uint16_t)(s->speed_kmh * 10.0f + 0.5f);
    out->heading_x10    = (uint16_t)(s->heading_deg * 10.0f + 0.5f);
    out->lap_number     = (uint16_t)s->lap_number;
    out->satellites     = s->satellites;
    out->fix_valid      = s->fix_valid ? 1 : 0;
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

        if (s_telem_subscribed && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_last_packet,
                                                        sizeof(s_last_packet));
            if (om) {
                int rc = ble_gatts_notify_custom(s_conn_handle,
                                                  s_telem_val_handle, om);
                if (rc != 0) ESP_LOGW(TAG, "telem notify falhou (rc=%d)", rc);
            }
        }
        vTaskDelay(period);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * API PUBLICA
 * ══════════════════════════════════════════════════════════════════════ */

void ble_telemetry_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(ble_cmd_item_t));
    configASSERT(s_cmd_queue);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init falhou (%s)", esp_err_to_name(err));
        return;
    }

    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_device_name);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg falhou (rc=%d)", rc); return; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs falhou (rc=%d)", rc); return; }

    nimble_port_freertos_init(ble_host_task);
    xTaskCreate(ble_telemetry_task, "ble_telem", 4096, NULL, 5, NULL);
    xTaskCreate(ble_cmd_task,       "ble_cmd",   8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "BLE telemetry + controle prontos");
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
