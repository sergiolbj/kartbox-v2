/*
 * gps.c - ver gps.h pro contexto das decisoes de arquitetura.
 */
#include "gps.h"
#include "config.h"
#include "app_events.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "driver/uart.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gps";

#define GPS_LINE_BUF_SIZE   (96)
#define GPS_UART_CHUNK_SIZE (64)

static SemaphoreHandle_t s_state_mutex;
static gps_sample_t      s_state;
static QueueHandle_t     s_sd_log_queue;

static gps_race_mode_t s_mode = GPS_MODE_QUALY;
static bool     s_recording = false;
static bool     s_waiting_for_movement = false;

static bool     s_finish_line_set = false;
static double   s_finish_lat, s_finish_lon;
static float    s_finish_heading;
static int64_t  s_last_cross_us = 0;

static uint32_t s_lap_number = 0;
static uint32_t s_best_lap_ms = 0;
static int32_t  s_last_delta_ms = 0;

/* Setores opcionais - todos false por default; ativados via gps_set_sector_point(). */
static bool     s_sector_set[GPS_MAX_SECTORS];
static double   s_sector_lat[GPS_MAX_SECTORS];
static double   s_sector_lon[GPS_MAX_SECTORS];
static float    s_sector_heading[GPS_MAX_SECTORS];
static bool     s_sector_crossed[GPS_MAX_SECTORS];   /* cruzou nessa volta? */
static uint32_t s_sector_split_ms[GPS_MAX_SECTORS];  /* split da volta atual (0 = ainda nao) */
static uint32_t s_best_sector_ms[GPS_MAX_SECTORS];   /* melhor split historico da sessao */

static int16_t  s_utc_offset_min = DEFAULT_UTC_OFFSET_MIN;
static float    s_gate_radius_m = GATE_RADIUS_M;
static uint32_t s_min_lap_time_ms = MIN_LAP_TIME_MS;
static struct tm s_last_utc_tm;
static bool      s_has_datetime = false;

/* ----------------------------------------------------------------------
 * Helpers de baixo nivel
 * ---------------------------------------------------------------------- */

static inline uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

/* Valida checksum NMEA (XOR de tudo entre '$' e '*'). Linha corrompida
 * (comum em UART perto de ignicao/motor) e descartada aqui em vez de
 * virar lat/lon furada la na frente. */
static bool nmea_checksum_ok(const char *line, size_t len)
{
    if (len < 4 || line[0] != '$') return false;
    const char *star = memchr(line, '*', len);
    if (!star || (size_t)(star - line) + 3 > len) return false;

    uint8_t calc = 0;
    for (const char *p = line + 1; p < star; p++) calc ^= (uint8_t)*p;

    uint8_t hi = hex_nibble(star[1]);
    uint8_t lo = hex_nibble(star[2]);
    if (hi == 0xFF || lo == 0xFF) return false;
    return calc == ((hi << 4) | lo);
}

/* ddmm.mmmm (lat) ou dddmm.mmmm (lon) -> graus decimais. Funciona pros
 * dois formatos porque so olha "ultimos 2 digitos antes da fracao = minutos". */
static double nmea_coord_to_decimal(double raw, char hemi)
{
    double minutes = fmod(raw, 100.0);
    double degrees = (raw - minutes) / 100.0;
    double dec = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

static float distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double mean_lat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    double x = dlon * cos(mean_lat);
    return (float)(sqrt(x * x + dlat * dlat) * R);
}

static float heading_diff(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360.0f);
    return (d > 180.0f) ? (360.0f - d) : d;
}

/* Algoritmo de Howard Hinnant (days-from-civil) - converte data
 * gregoriana pra dias desde epoch sem depender de timegm()/TZ, que
 * variam de disponibilidade entre toolchains embarcadas. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t my_timegm(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* ----------------------------------------------------------------------
 * Logica de gate / timing de volta. Chamada com s_state_mutex JA preso.
 * ---------------------------------------------------------------------- */
static void process_gate_timing(int64_t now_us)
{
    if (!s_finish_line_set) return;

    if (s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement) {
        if (s_state.speed_kmh > RACE_START_SPEED_KMH) {
            s_waiting_for_movement = false;
            s_last_cross_us = now_us; /* largada = cruzamento zero da contagem */
        }
        return;
    }

    /* ---- verificacao de setores opcionais ----------------------------------------
     * Cada setor usa o mesmo algoritmo de proximidade+heading que o gate principal.
     * Debounce de 3s pra nao registrar cruzamento imediatamente apos a largada/gate.
     * Se o piloto nao marcou nenhum setor, esse loop e no-op inteiro.
     * ---------------------------------------------------------------------------- */
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (!s_sector_set[i] || s_sector_crossed[i]) continue;
        float sdist  = distance_m(s_state.lat, s_state.lon, s_sector_lat[i], s_sector_lon[i]);
        float shdiff = heading_diff(s_state.heading_deg, s_sector_heading[i]);
        if (sdist > s_gate_radius_m || shdiff > GATE_MAX_HEADING_DIFF) continue;
        int64_t since_ms = (now_us - s_last_cross_us) / 1000;
        if (since_ms < 3000) continue; /* debounce: < 3s desde ultima passagem pelo gate */
        s_sector_split_ms[i] = (uint32_t)since_ms;
        s_sector_crossed[i]  = true;
        if (s_best_sector_ms[i] == 0 || s_sector_split_ms[i] < s_best_sector_ms[i])
            s_best_sector_ms[i] = s_sector_split_ms[i];
        ESP_LOGI(TAG, "Setor %d: %u ms", i + 1, (unsigned)s_sector_split_ms[i]);
    }

    /* ---- gate principal (linha de chegada) ---- */
    float dist  = distance_m(s_state.lat, s_state.lon, s_finish_lat, s_finish_lon);
    float hdiff = heading_diff(s_state.heading_deg, s_finish_heading);
    if (dist > s_gate_radius_m || hdiff > GATE_MAX_HEADING_DIFF) return;

    int64_t since_last_ms = (now_us - s_last_cross_us) / 1000;
    if (since_last_ms < s_min_lap_time_ms) return; /* debounce do mesmo cruzamento */

    uint32_t lap_time_ms = (uint32_t)since_last_ms;
    int32_t  delta_ms = (s_best_lap_ms > 0) ? (int32_t)lap_time_ms - (int32_t)s_best_lap_ms : 0;
    bool     is_new_best = (s_best_lap_ms == 0 || lap_time_ms < s_best_lap_ms);

    if (is_new_best) s_best_lap_ms = lap_time_ms;
    s_last_delta_ms = delta_ms;
    s_lap_number++;
    s_last_cross_us = now_us;

    /* reseta estado de setor pra proxima volta */
    memset(s_sector_crossed,  0, sizeof(s_sector_crossed));
    memset(s_sector_split_ms, 0, sizeof(s_sector_split_ms));

    app_event_t evt = {
        .type = APP_EVT_LAP_COMPLETE,
        .source = EVT_SRC_INTERNAL,
    };
    evt.data.lap.lap_number  = s_lap_number;
    evt.data.lap.lap_time_ms = lap_time_ms;
    evt.data.lap.delta_ms    = delta_ms;
    evt.data.lap.is_new_best = is_new_best;
    app_event_post_data(&evt);

    ESP_LOGI(TAG, "Volta %u: %u ms (delta %d ms)%s",
             (unsigned)s_lap_number, (unsigned)lap_time_ms, (int)delta_ms,
             is_new_best ? " - novo best" : "");
}

/* ----------------------------------------------------------------------
 * Parsing de sentencas
 * ---------------------------------------------------------------------- */
static void parse_rmc(char *line)
{
    char *save = NULL;
    if (!strtok_r(line, ",", &save)) return; /* $..RMC */

    char *f_time   = strtok_r(NULL, ",", &save);
    char *f_status = strtok_r(NULL, ",", &save);
    char *f_lat    = strtok_r(NULL, ",", &save);
    char *f_ns     = strtok_r(NULL, ",", &save);
    char *f_lon    = strtok_r(NULL, ",", &save);
    char *f_ew     = strtok_r(NULL, ",", &save);
    char *f_speed  = strtok_r(NULL, ",", &save);
    char *f_course = strtok_r(NULL, ",", &save);
    char *f_date   = strtok_r(NULL, ",", &save);

    if (!f_time || !f_status || !f_date || strlen(f_time) < 6 || strlen(f_date) < 6) return;

    if (f_status[0] != 'A') {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.fix_valid = false;
        xSemaphoreGive(s_state_mutex);
        return;
    }
    if (!f_lat || !f_ns || !f_lon || !f_ew) return;

    double raw_lat = atof(f_lat);
    double raw_lon = atof(f_lon);
    float  speed_kmh = f_speed  ? (float)(atof(f_speed) * 1.852) : 0.0f;
    float  course    = f_course ? (float)atof(f_course) : 0.0f;

    struct tm utc_tm = {0};
    utc_tm.tm_hour = (f_time[0] - '0') * 10 + (f_time[1] - '0');
    utc_tm.tm_min  = (f_time[2] - '0') * 10 + (f_time[3] - '0');
    utc_tm.tm_sec  = (f_time[4] - '0') * 10 + (f_time[5] - '0');
    utc_tm.tm_mday = (f_date[0] - '0') * 10 + (f_date[1] - '0');
    utc_tm.tm_mon  = (f_date[2] - '0') * 10 + (f_date[3] - '0') - 1;
    utc_tm.tm_year = 100 + (f_date[4] - '0') * 10 + (f_date[5] - '0');

    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.fix_valid    = true;
    s_state.lat          = nmea_coord_to_decimal(raw_lat, f_ns[0]);
    s_state.lon          = nmea_coord_to_decimal(raw_lon, f_ew[0]);
    s_state.speed_kmh    = speed_kmh;
    s_state.heading_deg  = course;
    s_last_utc_tm         = utc_tm;
    s_has_datetime        = true;

    process_gate_timing(now_us);

    /* Mesmo calculo que gps_get_latest() faz pra UI - precisa estar
     * tambem em s_state porque a entrada do log copia s_state direto
     * (sem isso aqui, a coluna lap_time_ms do CSV saia sempre zero). */
    bool counting = s_finish_line_set && !(s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement);
    s_state.lap_time_ms = counting ? (uint32_t)((now_us - s_last_cross_us) / 1000) : 0;

    gps_log_entry_t entry;
    bool should_log = s_recording && s_sd_log_queue;
    if (should_log) {
        entry.sample = s_state;
        entry.sample.lap_number    = s_lap_number;
        entry.sample.best_lap_ms   = s_best_lap_ms;
        entry.sample.last_delta_ms = s_last_delta_ms;
        entry.timestamp_us = now_us;
    }
    xSemaphoreGive(s_state_mutex);

    if (should_log) {
        /* Nao bloqueia o GPS task se o logger de SD atrasar - perder uma
         * amostra ocasional e melhor que travar a leitura do GPS. */
        xQueueSend(s_sd_log_queue, &entry, 0);
    }
}

static void parse_gga(char *line)
{
    char *save = NULL;
    if (!strtok_r(line, ",", &save)) return; /* $..GGA */
    strtok_r(NULL, ",", &save); /* time */
    strtok_r(NULL, ",", &save); /* lat */
    strtok_r(NULL, ",", &save); /* N/S */
    strtok_r(NULL, ",", &save); /* lon */
    strtok_r(NULL, ",", &save); /* E/W */
    char *f_quality = strtok_r(NULL, ",", &save);
    char *f_sats    = strtok_r(NULL, ",", &save);
    if (!f_quality || !f_sats) return;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.satellites = (uint8_t)atoi(f_sats);
    if (f_quality[0] == '0') s_state.fix_valid = false;
    xSemaphoreGive(s_state_mutex);
}

static void gps_handle_line(char *line, size_t len)
{
    if (!nmea_checksum_ok(line, len)) {
        ESP_LOGD(TAG, "Checksum invalido, sentenca descartada");
        return;
    }
    char *star = strchr(line, '*');
    if (star) *star = '\0';

    if (strstr(line, "RMC")) {
        parse_rmc(line);
    } else if (strstr(line, "GGA")) {
        parse_gga(line);
    }
}

/* ----------------------------------------------------------------------
 * UART + task
 * ---------------------------------------------------------------------- */
static void uart_init_gps(void)
{
    const uart_config_t cfg = {
        .baud_rate  = GPS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void gps_task(void *arg)
{
    (void)arg;
    uint8_t chunk[GPS_UART_CHUNK_SIZE];
    static char line[GPS_LINE_BUF_SIZE]; /* static: nao usa stack da task pra isso */
    size_t line_len = 0;

    for (;;) {
        int n = uart_read_bytes(GPS_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (b == '\n' || b == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    gps_handle_line(line, line_len);
                    line_len = 0;
                }
                continue;
            }
            if (line_len < GPS_LINE_BUF_SIZE - 1) {
                line[line_len++] = (char)b;
            } else {
                line_len = 0; /* linha absurdamente longa - descarta, provavel lixo */
            }
        }
    }
}

/* ----------------------------------------------------------------------
 * API publica
 * ---------------------------------------------------------------------- */
void gps_init(QueueHandle_t sd_log_queue)
{
    s_sd_log_queue = sd_log_queue;
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex de estado");
        abort();
    }
    memset(&s_state, 0, sizeof(s_state));

    uart_init_gps();
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 8, NULL);
    ESP_LOGI(TAG, "GPS pronto (UART%d, %d baud)", GPS_UART_NUM, GPS_BAUD_RATE);
}

void gps_set_finish_line(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_state.fix_valid) {
        s_finish_lat       = s_state.lat;
        s_finish_lon       = s_state.lon;
        s_finish_heading   = s_state.heading_deg;
        s_finish_line_set  = true;
        s_last_cross_us    = esp_timer_get_time();
        s_waiting_for_movement = (s_mode == GPS_MODE_CORRIDA);
        ESP_LOGI(TAG, "Linha de chegada marcada (%.6f, %.6f) heading=%.1f",
                 s_finish_lat, s_finish_lon, (double)s_finish_heading);
    } else {
        ESP_LOGW(TAG, "Sem fix valido - linha de chegada nao marcada");
    }
    xSemaphoreGive(s_state_mutex);
}

void gps_load_finish_line(double lat, double lon, float heading_deg)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_finish_lat      = lat;
    s_finish_lon      = lon;
    s_finish_heading  = heading_deg;
    s_finish_line_set = true;
    /* Nao reinicia s_last_cross_us - o timer de volta ja esta rodando;
     * isso e um restore de config, nao um cruzamento de gate. */
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Linha de chegada restaurada (%.6f, %.6f) heading=%.1f",
             lat, lon, (double)heading_deg);
}

bool gps_get_finish_line(double *lat, double *lon, float *heading_deg)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool set = s_finish_line_set;
    if (set) {
        if (lat)         *lat         = s_finish_lat;
        if (lon)         *lon         = s_finish_lon;
        if (heading_deg) *heading_deg = s_finish_heading;
    }
    xSemaphoreGive(s_state_mutex);
    return set;
}

void gps_set_mode(gps_race_mode_t mode)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_mode = mode;
    xSemaphoreGive(s_state_mutex);
}

void gps_session_reset(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_lap_number = 0;
    s_best_lap_ms = 0;
    s_last_delta_ms = 0;
    s_last_cross_us = esp_timer_get_time();
    s_waiting_for_movement = (s_mode == GPS_MODE_CORRIDA) && s_finish_line_set;
    /* zera historico de setores da sessao anterior (pontos de setor sao mantidos) */
    memset(s_sector_crossed,  0, sizeof(s_sector_crossed));
    memset(s_sector_split_ms, 0, sizeof(s_sector_split_ms));
    memset(s_best_sector_ms,  0, sizeof(s_best_sector_ms));
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Sessao resetada (linha de chegada e setores mantidos se ja marcados)");
}

void gps_session_set_recording(bool recording)
{
    s_recording = recording;
}

void gps_set_utc_offset_min(int16_t offset_min)
{
    s_utc_offset_min = offset_min;
}

void gps_set_gate_radius_m(float meters)
{
    if (meters > 0.0f) s_gate_radius_m = meters;
}

void gps_set_min_lap_time_ms(uint32_t ms)
{
    s_min_lap_time_ms = ms;
}

void gps_get_latest(gps_sample_t *out)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_state;
    out->lap_number    = s_lap_number;
    out->best_lap_ms   = s_best_lap_ms;
    out->last_delta_ms = s_last_delta_ms;

    bool counting = s_finish_line_set && !(s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement);
    out->lap_time_ms = counting ? (uint32_t)((esp_timer_get_time() - s_last_cross_us) / 1000) : 0;

    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        out->sector_is_set[i]    = s_sector_set[i];
        out->sector_split_ms[i]  = s_sector_split_ms[i];
        out->best_sector_ms[i]   = s_best_sector_ms[i];
    }
    xSemaphoreGive(s_state_mutex);
}

void gps_set_sector_point(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_state.fix_valid) {
        s_sector_lat[idx]     = s_state.lat;
        s_sector_lon[idx]     = s_state.lon;
        s_sector_heading[idx] = s_state.heading_deg;
        s_sector_set[idx]     = true;
        ESP_LOGI(TAG, "Setor %d marcado (%.6f, %.6f) heading=%.1f",
                 idx + 1, s_sector_lat[idx], s_sector_lon[idx], (double)s_sector_heading[idx]);
    } else {
        ESP_LOGW(TAG, "Sem fix GPS valido - setor %d nao marcado", idx + 1);
    }
    xSemaphoreGive(s_state_mutex);
}

void gps_load_sector(int idx, double lat, double lon, float heading_deg)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_sector_lat[idx]     = lat;
    s_sector_lon[idx]     = lon;
    s_sector_heading[idx] = heading_deg;
    s_sector_set[idx]     = true;
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Setor %d restaurado (%.6f, %.6f)", idx + 1, lat, lon);
}

void gps_clear_sector_point(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_sector_set[idx]        = false;
    s_sector_crossed[idx]    = false;
    s_sector_split_ms[idx]   = 0;
    s_best_sector_ms[idx]    = 0;
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Setor %d removido", idx + 1);
}

bool gps_get_sector_point(int idx, double *lat, double *lon, float *heading_deg)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool set = s_sector_set[idx];
    if (set) {
        if (lat)         *lat         = s_sector_lat[idx];
        if (lon)         *lon         = s_sector_lon[idx];
        if (heading_deg) *heading_deg = s_sector_heading[idx];
    }
    xSemaphoreGive(s_state_mutex);
    return set;
}

bool gps_sector_is_set(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool set = s_sector_set[idx];
    xSemaphoreGive(s_state_mutex);
    return set;
}

bool gps_get_local_datetime(struct tm *out)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool has = s_has_datetime;
    struct tm utc_copy = s_last_utc_tm;
    xSemaphoreGive(s_state_mutex);

    if (!has) return false;

    time_t utc_epoch   = my_timegm(&utc_copy);
    time_t local_epoch = utc_epoch + (int64_t)s_utc_offset_min * 60;
    gmtime_r(&local_epoch, out);
    return true;
}
