/*
 * settings.c - ver settings.h pro contexto.
 */
#include "settings.h"
#include "config.h"

#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "settings";
#define NVS_NAMESPACE "kartbox"

static nvs_handle_t s_nvs = 0;
static bool         s_nvs_ok = false;

static int16_t          s_utc_offset_min;
static float            s_gate_radius_m;
static uint32_t         s_min_lap_time_ms;
static char             s_ble_name[32];
static char             s_wifi_password[64];
static settings_sector_t s_sectors[SETTINGS_MAX_SECTORS];
static char             s_last_track[SETTINGS_TRACK_NAME_MAX];
static char             s_ble_pin[SETTINGS_BLE_PIN_MAX + 1];

static void load_blob_or_default(const char *key, void *out, size_t size, const void *def)
{
    size_t actual = size;
    if (!s_nvs_ok || nvs_get_blob(s_nvs, key, out, &actual) != ESP_OK || actual != size) {
        memcpy(out, def, size);
    }
}

static void load_str_or_default(const char *key, char *out, size_t out_size, const char *def)
{
    size_t len = out_size;
    if (!s_nvs_ok || nvs_get_str(s_nvs, key, out, &len) != ESP_OK) {
        strncpy(out, def, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

void settings_init(void)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    s_nvs_ok = (err == ESP_OK);
    if (!s_nvs_ok) {
        ESP_LOGW(TAG, "nvs_open falhou (%s) - rodando so com defaults de fabrica, sem persistir",
                  esp_err_to_name(err));
    }

    int16_t  utc_def  = DEFAULT_UTC_OFFSET_MIN;
    float    gate_def = GATE_RADIUS_M;
    uint32_t lap_def  = MIN_LAP_TIME_MS;

    load_blob_or_default("utc_off",  &s_utc_offset_min,  sizeof(s_utc_offset_min),  &utc_def);
    load_blob_or_default("gate_r",   &s_gate_radius_m,   sizeof(s_gate_radius_m),   &gate_def);
    load_blob_or_default("min_lap",  &s_min_lap_time_ms, sizeof(s_min_lap_time_ms), &lap_def);
    load_str_or_default("ble_name",  s_ble_name,  sizeof(s_ble_name),  BLE_DEVICE_NAME_DEFAULT);
    load_str_or_default("wifi_pass", s_wifi_password, sizeof(s_wifi_password), WIFI_AP_PASSWORD_DEFAULT);

    /* setores: default = nao configurado (is_set = false) */
    const settings_sector_t sec_def = {0};
    for (int i = 0; i < SETTINGS_MAX_SECTORS; i++) {
        char key[6];
        snprintf(key, sizeof(key), "sec%d", i);
        load_blob_or_default(key, &s_sectors[i], sizeof(s_sectors[i]), &sec_def);
    }

    /* ultima pista usada - default = vazio (nenhuma pista carregada ainda) */
    load_str_or_default("last_track", s_last_track, sizeof(s_last_track), "");

    /* PIN BLE - default = vazio (PIN desativado ate o usuario configurar) */
    load_str_or_default("ble_pin", s_ble_pin, sizeof(s_ble_pin), "");

    ESP_LOGI(TAG, "Config carregada: utc=%dmin gate=%.1fm minlap=%lums ble=\"%s\" sec0=%s sec1=%s last_track=\"%s\" pin=%s",
             s_utc_offset_min, (double)s_gate_radius_m,
             (unsigned long)s_min_lap_time_ms, s_ble_name,
             s_sectors[0].is_set ? "ok" : "--",
             s_sectors[1].is_set ? "ok" : "--",
             s_last_track[0] ? s_last_track : "(nenhuma)",
             s_ble_pin[0] ? "****" : "(desativado)");
}

int16_t settings_get_utc_offset_min(void) { return s_utc_offset_min; }
void settings_set_utc_offset_min(int16_t value)
{
    s_utc_offset_min = value;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "utc_off", &value, sizeof(value));
        nvs_commit(s_nvs);
    }
}

float settings_get_gate_radius_m(void) { return s_gate_radius_m; }
void settings_set_gate_radius_m(float value)
{
    if (value <= 0.0f) return;
    s_gate_radius_m = value;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "gate_r", &value, sizeof(value));
        nvs_commit(s_nvs);
    }
}

uint32_t settings_get_min_lap_time_ms(void) { return s_min_lap_time_ms; }
void settings_set_min_lap_time_ms(uint32_t value)
{
    s_min_lap_time_ms = value;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "min_lap", &value, sizeof(value));
        nvs_commit(s_nvs);
    }
}

const char *settings_get_ble_name(void) { return s_ble_name; }
void settings_set_ble_name(const char *name)
{
    if (!name || !name[0]) return;
    strncpy(s_ble_name, name, sizeof(s_ble_name) - 1);
    s_ble_name[sizeof(s_ble_name) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "ble_name", s_ble_name);
        nvs_commit(s_nvs);
    }
}

const char *settings_get_wifi_password(void) { return s_wifi_password; }
void settings_set_wifi_password(const char *password)
{
    if (!password || strlen(password) < 8) return; /* WPA2 minimo 8 chars */
    strncpy(s_wifi_password, password, sizeof(s_wifi_password) - 1);
    s_wifi_password[sizeof(s_wifi_password) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "wifi_pass", s_wifi_password);
        nvs_commit(s_nvs);
    }
}

settings_sector_t settings_get_sector(int idx)
{
    if (idx < 0 || idx >= SETTINGS_MAX_SECTORS) {
        settings_sector_t empty = {0};
        return empty;
    }
    return s_sectors[idx];
}

void settings_set_sector(int idx, double lat, double lon, float heading_deg)
{
    if (idx < 0 || idx >= SETTINGS_MAX_SECTORS) return;
    s_sectors[idx].lat         = lat;
    s_sectors[idx].lon         = lon;
    s_sectors[idx].heading_deg = heading_deg;
    s_sectors[idx].is_set      = true;
    if (s_nvs_ok) {
        char key[6];
        snprintf(key, sizeof(key), "sec%d", idx);
        nvs_set_blob(s_nvs, key, &s_sectors[idx], sizeof(s_sectors[idx]));
        nvs_commit(s_nvs);
    }
}

void settings_clear_sector(int idx)
{
    if (idx < 0 || idx >= SETTINGS_MAX_SECTORS) return;
    memset(&s_sectors[idx], 0, sizeof(s_sectors[idx]));
    if (s_nvs_ok) {
        char key[6];
        snprintf(key, sizeof(key), "sec%d", idx);
        nvs_set_blob(s_nvs, key, &s_sectors[idx], sizeof(s_sectors[idx]));
        nvs_commit(s_nvs);
    }
}

const char *settings_get_last_track(void) { return s_last_track; }
void settings_set_last_track(const char *name)
{
    if (!name) name = "";
    strncpy(s_last_track, name, sizeof(s_last_track) - 1);
    s_last_track[sizeof(s_last_track) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "last_track", s_last_track);
        nvs_commit(s_nvs);
    }
}

const char *settings_get_ble_pin(void) { return s_ble_pin; }
void settings_set_ble_pin(const char *pin)
{
    if (!pin) pin = "";
    strncpy(s_ble_pin, pin, SETTINGS_BLE_PIN_MAX);
    s_ble_pin[SETTINGS_BLE_PIN_MAX] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "ble_pin", s_ble_pin);
        nvs_commit(s_nvs);
    }
}
