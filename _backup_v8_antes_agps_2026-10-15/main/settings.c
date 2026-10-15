/*
 * settings.c - ver settings.h pro contexto.
 */
#include "settings.h"
#include "config.h"

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
static uint8_t          s_wifi_mode;
static char             s_wifi_sta_ssid[SETTINGS_WIFI_STA_SSID_MAX];
static char             s_wifi_sta_pass[SETTINGS_WIFI_STA_PASS_MAX];
static settings_sector_t s_sectors[SETTINGS_MAX_SECTORS];
static char             s_last_track[SETTINGS_TRACK_NAME_MAX];
static char             s_ble_pin[SETTINGS_BLE_PIN_MAX + 1];
static uint8_t          s_theme;
static uint8_t          s_auto_session;
static char             s_agps_token[SETTINGS_AGPS_TOKEN_MAX];
static uint8_t          s_ble_radio;
static uint8_t          s_brightness;
static uint8_t          s_scr_enabled;
static uint16_t         s_scr_dim_s;
static uint16_t         s_scr_off_s;
static uint8_t          s_layout_qualy;
static uint8_t          s_layout_race;
static uint16_t         s_led_scale_ms;

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

    /* modo wifi + credenciais STA - default = AP (0), STA vazio ate o
     * usuario escanear/digitar e conectar pela primeira vez. */
    uint8_t wifi_mode_def = 0;
    load_blob_or_default("wifi_mode", &s_wifi_mode, sizeof(s_wifi_mode), &wifi_mode_def);
    load_str_or_default("wifi_sta_ssid", s_wifi_sta_ssid, sizeof(s_wifi_sta_ssid), "");
    load_str_or_default("wifi_sta_pass", s_wifi_sta_pass, sizeof(s_wifi_sta_pass), "");

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

    /* Tema de cor da UI - default = 0 (Verde, paleta historica). Clamp
     * defensivo: NVS gravada por firmware futuro com mais temas nao pode
     * estourar a tabela de paletas do firmware atual. */
    uint8_t theme_def = 0;
    load_blob_or_default("theme", &s_theme, sizeof(s_theme), &theme_def);
    if (s_theme >= SETTINGS_THEME_COUNT) s_theme = 0;

    /* Auto-sessao - default ligado (feature pensada pra tirar o RESET
     * da frente do piloto; quem nao quiser desliga na CONFIG > CORRIDA). */
    uint8_t auto_sess_def = 1;
    load_blob_or_default("auto_sess", &s_auto_session, sizeof(s_auto_session), &auto_sess_def);
    if (s_auto_session > 1) s_auto_session = 1;

    /* Token AssistNow (A-GPS) - default vazio (nunca configurado) */
    load_str_or_default("agps_token", s_agps_token, sizeof(s_agps_token), "");

    /* Radio BLE - default ligado (comportamento historico) */
    uint8_t ble_radio_def = 1;
    load_blob_or_default("ble_radio", &s_ble_radio, sizeof(s_ble_radio), &ble_radio_def);
    if (s_ble_radio > 1) s_ble_radio = 1;

    /* Brilho do display - default 100%, clamp 20..100 */
    uint8_t bright_def = 100;
    load_blob_or_default("bright", &s_brightness, sizeof(s_brightness), &bright_def);
    if (s_brightness < 20)  s_brightness = 20;
    if (s_brightness > 100) s_brightness = 100;

    /* Protetor de tela - default ligado, tempos de fabrica. Clamps
     * defensivos contra NVS gravada por firmware futuro / valores fora
     * de faixa. off pode ser 0 (nunca apaga de vez). */
    uint8_t  scr_en_def  = SCREENSAVER_ENABLED_DEFAULT;
    uint16_t scr_dim_def = SCREENSAVER_DIM_AFTER_S;
    uint16_t scr_off_def = SCREENSAVER_OFF_AFTER_S;
    load_blob_or_default("scr_en",  &s_scr_enabled, sizeof(s_scr_enabled), &scr_en_def);
    load_blob_or_default("scr_dim", &s_scr_dim_s,   sizeof(s_scr_dim_s),   &scr_dim_def);
    load_blob_or_default("scr_off", &s_scr_off_s,   sizeof(s_scr_off_s),   &scr_off_def);
    if (s_scr_enabled > 1) s_scr_enabled = 1;
    if (s_scr_dim_s < SCREENSAVER_DIM_MIN_S) s_scr_dim_s = SCREENSAVER_DIM_MIN_S;
    if (s_scr_dim_s > SCREENSAVER_DIM_MAX_S) s_scr_dim_s = SCREENSAVER_DIM_MAX_S;
    if (s_scr_off_s != 0 && s_scr_off_s < SCREENSAVER_OFF_MIN_S) s_scr_off_s = SCREENSAVER_OFF_MIN_S;
    if (s_scr_off_s > SCREENSAVER_OFF_MAX_S) s_scr_off_s = SCREENSAVER_OFF_MAX_S;

    /* Layout da tela CORRIDA por modo - default 0 (completo) nos dois */
    uint8_t lay_def = 0;
    load_blob_or_default("lay_q", &s_layout_qualy, sizeof(s_layout_qualy), &lay_def);
    load_blob_or_default("lay_r", &s_layout_race,  sizeof(s_layout_race),  &lay_def);
    if (s_layout_qualy > 2) s_layout_qualy = 0;
    if (s_layout_race  > 2) s_layout_race  = 0;

    /* Escala da barra de LED do delta - default 1500ms, clamp 500..3000 */
    uint16_t led_def = 1500;
    load_blob_or_default("led_scale", &s_led_scale_ms, sizeof(s_led_scale_ms), &led_def);
    if (s_led_scale_ms < 500)  s_led_scale_ms = 500;
    if (s_led_scale_ms > 3000) s_led_scale_ms = 3000;

    ESP_LOGI(TAG, "Config carregada: utc=%dmin gate=%.1fm minlap=%lums ble=\"%s\" sec0=%s sec1=%s last_track=\"%s\" pin=%s wifi_modo=%s wifi_sta_ssid=\"%s\"",
             s_utc_offset_min, (double)s_gate_radius_m,
             (unsigned long)s_min_lap_time_ms, s_ble_name,
             s_sectors[0].is_set ? "ok" : "--",
             s_sectors[1].is_set ? "ok" : "--",
             s_last_track[0] ? s_last_track : "(nenhuma)",
             s_ble_pin[0] ? "****" : "(desativado)",
             s_wifi_mode == 0 ? "AP" : "STA",
             s_wifi_sta_ssid);
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

uint8_t settings_get_wifi_mode(void) { return s_wifi_mode; }
void settings_set_wifi_mode(uint8_t mode)
{
    s_wifi_mode = mode;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "wifi_mode", &mode, sizeof(mode));
        nvs_commit(s_nvs);
    }
}

const char *settings_get_wifi_sta_ssid(void) { return s_wifi_sta_ssid; }
void settings_set_wifi_sta_ssid(const char *ssid)
{
    if (!ssid) ssid = "";
    strncpy(s_wifi_sta_ssid, ssid, sizeof(s_wifi_sta_ssid) - 1);
    s_wifi_sta_ssid[sizeof(s_wifi_sta_ssid) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "wifi_sta_ssid", s_wifi_sta_ssid);
        nvs_commit(s_nvs);
    }
}

const char *settings_get_wifi_sta_password(void) { return s_wifi_sta_pass; }
void settings_set_wifi_sta_password(const char *password)
{
    /* sem minimo de 8 aqui (diferente da senha do AP proprio) - rede
     * alheia pode ser aberta (sem senha) e a gente so repassa o que o
     * usuario digitou pro esp_wifi_connect() decidir. */
    if (!password) password = "";
    strncpy(s_wifi_sta_pass, password, sizeof(s_wifi_sta_pass) - 1);
    s_wifi_sta_pass[sizeof(s_wifi_sta_pass) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "wifi_sta_pass", s_wifi_sta_pass);
        nvs_commit(s_nvs);
    }
}

uint16_t settings_get_led_scale_ms(void) { return s_led_scale_ms; }
void settings_set_led_scale_ms(uint16_t ms)
{
    if (ms < 500)  ms = 500;
    if (ms > 3000) ms = 3000;
    s_led_scale_ms = ms;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "led_scale", &s_led_scale_ms, sizeof(s_led_scale_ms));
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_mode_layout(uint8_t race_mode)
{
    return race_mode ? s_layout_race : s_layout_qualy;
}

void settings_set_mode_layout(uint8_t race_mode, uint8_t layout)
{
    if (layout > 2) layout = 0;
    if (race_mode) s_layout_race = layout;
    else           s_layout_qualy = layout;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, race_mode ? "lay_r" : "lay_q", &layout, sizeof(layout));
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_brightness(void) { return s_brightness; }
void settings_set_brightness(uint8_t percent)
{
    if (percent < 20)  percent = 20;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "bright", &s_brightness, sizeof(s_brightness));
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_screensaver_enabled(void) { return s_scr_enabled; }
void settings_set_screensaver_enabled(uint8_t enabled)
{
    s_scr_enabled = enabled ? 1 : 0;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "scr_en", &s_scr_enabled, sizeof(s_scr_enabled));
        nvs_commit(s_nvs);
    }
}

uint16_t settings_get_screensaver_dim_s(void) { return s_scr_dim_s; }
void settings_set_screensaver_dim_s(uint16_t seconds)
{
    if (seconds < SCREENSAVER_DIM_MIN_S) seconds = SCREENSAVER_DIM_MIN_S;
    if (seconds > SCREENSAVER_DIM_MAX_S) seconds = SCREENSAVER_DIM_MAX_S;
    s_scr_dim_s = seconds;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "scr_dim", &s_scr_dim_s, sizeof(s_scr_dim_s));
        nvs_commit(s_nvs);
    }
}

uint16_t settings_get_screensaver_off_s(void) { return s_scr_off_s; }
void settings_set_screensaver_off_s(uint16_t seconds)
{
    /* 0 = nunca apaga de vez (so escurece). Fora isso, respeita a faixa. */
    if (seconds != 0 && seconds < SCREENSAVER_OFF_MIN_S) seconds = SCREENSAVER_OFF_MIN_S;
    if (seconds > SCREENSAVER_OFF_MAX_S) seconds = SCREENSAVER_OFF_MAX_S;
    s_scr_off_s = seconds;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "scr_off", &s_scr_off_s, sizeof(s_scr_off_s));
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_ble_radio(void) { return s_ble_radio; }
void settings_set_ble_radio(uint8_t enabled)
{
    s_ble_radio = enabled ? 1 : 0;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "ble_radio", &s_ble_radio, sizeof(s_ble_radio));
        nvs_commit(s_nvs);
    }
}

const char *settings_get_agps_token(void) { return s_agps_token; }
void settings_set_agps_token(const char *token)
{
    if (!token) token = "";
    strncpy(s_agps_token, token, sizeof(s_agps_token) - 1);
    s_agps_token[sizeof(s_agps_token) - 1] = '\0';
    if (s_nvs_ok) {
        nvs_set_str(s_nvs, "agps_token", s_agps_token);
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_auto_session(void) { return s_auto_session; }
void settings_set_auto_session(uint8_t enabled)
{
    s_auto_session = enabled ? 1 : 0;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "auto_sess", &s_auto_session, sizeof(s_auto_session));
        nvs_commit(s_nvs);
    }
}

uint8_t settings_get_theme(void) { return s_theme; }
void settings_set_theme(uint8_t theme)
{
    if (theme >= SETTINGS_THEME_COUNT) return;
    s_theme = theme;
    if (s_nvs_ok) {
        nvs_set_blob(s_nvs, "theme", &theme, sizeof(theme));
        nvs_commit(s_nvs);
    }
}

/* ------------------------------------------------------------------
 * Backup/restore - texto key=value, uma config por linha. Formato
 * proposital de gente: da pra abrir no bloco de notas e conferir/editar
 * antes de restaurar. Setores em "secN=lat;lon;heading" (so os
 * marcados). Keys desconhecidas no restore sao IGNORADAS - um arquivo
 * de firmware mais novo restaura parcialmente num mais velho sem erro.
 * ------------------------------------------------------------------ */
bool settings_backup_to_file(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "backup: falha ao criar %s", path);
        return false;
    }
    fprintf(f, "# kartbox config backup v1 - key=value, edite por sua conta e risco\n");
    fprintf(f, "utc_off=%d\n",      (int)s_utc_offset_min);
    fprintf(f, "gate_r=%.2f\n",     (double)s_gate_radius_m);
    fprintf(f, "min_lap=%lu\n",     (unsigned long)s_min_lap_time_ms);
    fprintf(f, "ble_name=%s\n",     s_ble_name);
    fprintf(f, "wifi_pass=%s\n",    s_wifi_password);
    fprintf(f, "wifi_mode=%u\n",    (unsigned)s_wifi_mode);
    fprintf(f, "sta_ssid=%s\n",     s_wifi_sta_ssid);
    fprintf(f, "sta_pass=%s\n",     s_wifi_sta_pass);
    fprintf(f, "last_track=%s\n",   s_last_track);
    fprintf(f, "ble_pin=%s\n",      s_ble_pin);
    fprintf(f, "theme=%u\n",        (unsigned)s_theme);
    fprintf(f, "auto_sess=%u\n",    (unsigned)s_auto_session);
    fprintf(f, "agps_token=%s\n",   s_agps_token);
    fprintf(f, "ble_radio=%u\n",    (unsigned)s_ble_radio);
    fprintf(f, "bright=%u\n",       (unsigned)s_brightness);
    fprintf(f, "scr_en=%u\n",       (unsigned)s_scr_enabled);
    fprintf(f, "scr_dim=%u\n",      (unsigned)s_scr_dim_s);
    fprintf(f, "scr_off=%u\n",      (unsigned)s_scr_off_s);
    fprintf(f, "lay_q=%u\n",        (unsigned)s_layout_qualy);
    fprintf(f, "lay_r=%u\n",        (unsigned)s_layout_race);
    fprintf(f, "led_scale=%u\n",    (unsigned)s_led_scale_ms);
    for (int i = 0; i < SETTINGS_MAX_SECTORS; i++) {
        if (s_sectors[i].is_set) {
            fprintf(f, "sec%d=%.8f;%.8f;%.2f\n", i,
                    s_sectors[i].lat, s_sectors[i].lon, (double)s_sectors[i].heading_deg);
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Backup de config salvo em %s", path);
    return true;
}

bool settings_restore_from_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "restore: %s nao encontrado", path);
        return false;
    }
    char line[160];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        if      (!strcmp(key, "utc_off"))    settings_set_utc_offset_min((int16_t)atoi(val));
        else if (!strcmp(key, "gate_r"))     settings_set_gate_radius_m((float)atof(val));
        else if (!strcmp(key, "min_lap"))    settings_set_min_lap_time_ms((uint32_t)strtoul(val, NULL, 10));
        else if (!strcmp(key, "ble_name"))   settings_set_ble_name(val);
        else if (!strcmp(key, "wifi_pass"))  settings_set_wifi_password(val);
        else if (!strcmp(key, "wifi_mode"))  settings_set_wifi_mode((uint8_t)atoi(val));
        else if (!strcmp(key, "sta_ssid"))   settings_set_wifi_sta_ssid(val);
        else if (!strcmp(key, "sta_pass"))   settings_set_wifi_sta_password(val);
        else if (!strcmp(key, "last_track")) settings_set_last_track(val);
        else if (!strcmp(key, "ble_pin"))    settings_set_ble_pin(val);
        else if (!strcmp(key, "theme"))      settings_set_theme((uint8_t)atoi(val));
        else if (!strcmp(key, "auto_sess"))  settings_set_auto_session((uint8_t)atoi(val));
        else if (!strcmp(key, "agps_token")) settings_set_agps_token(val);
        else if (!strcmp(key, "ble_radio"))  settings_set_ble_radio((uint8_t)atoi(val));
        else if (!strcmp(key, "bright"))     settings_set_brightness((uint8_t)atoi(val));
        else if (!strcmp(key, "scr_en"))     settings_set_screensaver_enabled((uint8_t)atoi(val));
        else if (!strcmp(key, "scr_dim"))    settings_set_screensaver_dim_s((uint16_t)atoi(val));
        else if (!strcmp(key, "scr_off"))    settings_set_screensaver_off_s((uint16_t)atoi(val));
        else if (!strcmp(key, "lay_q"))      settings_set_mode_layout(0, (uint8_t)atoi(val));
        else if (!strcmp(key, "lay_r"))      settings_set_mode_layout(1, (uint8_t)atoi(val));
        else if (!strcmp(key, "led_scale"))  settings_set_led_scale_ms((uint16_t)atoi(val));
        else if (!strncmp(key, "sec", 3) && key[3] >= '0' && key[3] <= '9') {
            int idx = key[3] - '0';
            double lat = 0, lon = 0, hdg = 0;
            if (sscanf(val, "%lf;%lf;%lf", &lat, &lon, &hdg) == 3) {
                settings_set_sector(idx, lat, lon, (float)hdg);
            } else {
                continue;
            }
        }
        else continue; /* key desconhecida - ignora */
        applied++;
    }
    fclose(f);
    ESP_LOGI(TAG, "Restore de config: %d itens aplicados de %s", applied, path);
    return applied > 0;
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
