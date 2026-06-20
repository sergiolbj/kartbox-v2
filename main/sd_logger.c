/*
 * sd_logger.c - ver sd_logger.h pro contexto.
 */
#include "sd_logger.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sd_logger";

static QueueHandle_t    s_log_queue;
static sdmmc_card_t    *s_card = NULL;
static FILE             *s_session_file = NULL;
static volatile bool     s_recording = false;
static int64_t           s_session_start_us = 0;
static esp_ldo_channel_handle_t s_sd_ldo = NULL;
static bool               s_mounted = false;

/* ---------------------------------------------------------------------- */

static void ensure_sessions_dir(void)
{
    struct stat st;
    if (stat(SD_SESSIONS_DIR, &st) != 0) {
        if (mkdir(SD_SESSIONS_DIR, 0755) != 0) {
            ESP_LOGW(TAG, "Nao consegui criar %s (pode ja existir)", SD_SESSIONS_DIR);
        }
    }
}

static bool sd_power_and_mount(void)
{
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = SD_LDO_CHAN_ID,
        .voltage_mv = SD_LDO_VOLTAGE_MV,
    };
    if (esp_ldo_acquire_channel(&ldo_cfg, &s_sd_ldo) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao ligar LDO do slot SD");
        return false;
    }

    /* Assumido ativo em HIGH (transistor de chaveamento da VCC do slot).
     * Se o cartao nunca montar na bancada, essa e a primeira coisa pra
     * inverter. */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << SD_PWR_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(SD_PWR_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20)); /* tempo pro rail estabilizar antes do clock SDMMC subir */

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;
    slot_cfg.clk   = SD_CLK_PIN;
    slot_cfg.cmd   = SD_CMD_PIN;
    slot_cfg.d0    = SD_D0_PIN;
    slot_cfg.d1    = SD_D1_PIN;
    slot_cfg.d2    = SD_D2_PIN;
    slot_cfg.d3    = SD_D3_PIN;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao montar cartao SD (%s) - verifique se ha cartao no slot", esp_err_to_name(err));
        return false;
    }

    ensure_sessions_dir();
    return true;
}

/* ---------------------------------------------------------------------- */

static void sd_logger_task(void *arg)
{
    (void)arg;
    gps_log_entry_t entry;
    int since_flush = 0;

    for (;;) {
        if (xQueueReceive(s_log_queue, &entry, portMAX_DELAY) != pdTRUE) continue;
        if (!s_recording || !s_session_file) continue;

        uint32_t t_ms = (uint32_t)((entry.timestamp_us - s_session_start_us) / 1000);
        fprintf(s_session_file, "%lu,%.6f,%.6f,%.1f,%.1f,%u,%lu,%lu,%lu,%ld\n",
                (unsigned long)t_ms,
                entry.sample.lat, entry.sample.lon,
                (double)entry.sample.speed_kmh, (double)entry.sample.heading_deg,
                (unsigned)entry.sample.satellites,
                (unsigned long)entry.sample.lap_number,
                (unsigned long)entry.sample.lap_time_ms,
                (unsigned long)entry.sample.best_lap_ms,
                (long)entry.sample.last_delta_ms);

        /* Flush periodico, nao a cada linha - cada flush forca escrita
         * fisica no cartao, custa caro. 10 amostras ~= 1s de dado preso
         * no buffer se a energia cair no meio, troca aceitavel por
         * desgaste/latencia bem menores no resto do tempo. */
        if (++since_flush >= 10) {
            fflush(s_session_file);
            since_flush = 0;
        }
    }
}

/* ---------------------------------------------------------------------- */

bool sd_logger_init(void)
{
    s_log_queue = xQueueCreate(SD_LOG_QUEUE_LEN, sizeof(gps_log_entry_t));
    if (!s_log_queue) {
        ESP_LOGE(TAG, "Falha ao criar fila de log - sem memoria");
        return false;
    }

    s_mounted = sd_power_and_mount();
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD indisponivel - kartbox segue sem gravar sessoes");
        return false;
    }

    xTaskCreate(sd_logger_task, "sd_logger", 4096, NULL, 7, NULL);
    ESP_LOGI(TAG, "SD montado, logger pronto");
    return true;
}

QueueHandle_t sd_logger_get_queue(void)
{
    return s_log_queue;
}

bool sd_logger_start_session(void)
{
    if (!s_mounted) return false;
    if (s_recording) sd_logger_stop_session();

    char path[80];
    struct tm local_tm;
    if (gps_get_local_datetime(&local_tm)) {
        snprintf(path, sizeof(path), "%s/%04d%02d%02d_%02d%02d%02d.csv",
                 SD_SESSIONS_DIR,
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    } else {
        /* Sem fix de GPS ainda (ex: ligou debaixo de teto) - usa o
         * relogio interno desde o boot como nome generico. */
        snprintf(path, sizeof(path), "%s/sessao_%lld.csv",
                 SD_SESSIONS_DIR, (long long)(esp_timer_get_time() / 1000000));
    }

    s_session_file = fopen(path, "w");
    if (!s_session_file) {
        ESP_LOGE(TAG, "Falha ao criar %s", path);
        return false;
    }

    fprintf(s_session_file, "t_ms,lat,lon,speed_kmh,heading_deg,sats,lap,lap_time_ms,best_lap_ms,delta_ms\n");
    s_session_start_us = esp_timer_get_time();
    s_recording = true;

    ESP_LOGI(TAG, "Sessao iniciada: %s", path);
    return true;
}

void sd_logger_stop_session(void)
{
    s_recording = false;
    if (s_session_file) {
        fflush(s_session_file);
        fclose(s_session_file);
        s_session_file = NULL;
        ESP_LOGI(TAG, "Sessao encerrada e salva");
    }
}

bool sd_logger_is_recording(void)
{
    return s_recording;
}

bool sd_get_card_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_mounted) return false;
    return esp_vfs_fat_info(SD_MOUNT_POINT, out_total_bytes, out_free_bytes) == ESP_OK;
}

void sd_logger_unmount(void)
{
    if (s_recording) sd_logger_stop_session();
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "Cartao desmontado (cedido pro modo USB)");
    }
}

bool sd_logger_remount(void)
{
    s_mounted = sd_power_and_mount();
    if (s_mounted) {
        ESP_LOGI(TAG, "Cartao remontado, firmware voltou a gravar");
    } else {
        ESP_LOGW(TAG, "Falha ao remontar cartao apos modo USB");
    }
    return s_mounted;
}

static int session_name_cmp_desc(const void *a, const void *b)
{
    /* Nomes sao YYYYMMDD_HHMMSS.csv - ordem alfabetica == ordem
     * cronologica. Desc pra mais recente vir primeiro. */
    return strcmp(((const sd_session_entry_t *)b)->filename,
                  ((const sd_session_entry_t *)a)->filename);
}

int sd_list_sessions(sd_session_entry_t *out, int max_entries)
{
    if (!s_mounted) return 0;

    DIR *dir = opendir(SD_SESSIONS_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < max_entries) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".csv") != 0) continue;
        strncpy(out[count].filename, de->d_name, SD_SESSION_NAME_LEN - 1);
        out[count].filename[SD_SESSION_NAME_LEN - 1] = '\0';
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(sd_session_entry_t), session_name_cmp_desc);
    return count;
}

void sd_delete_all_sessions(void)
{
    if (!s_mounted) return;
    if (s_recording) sd_logger_stop_session();

    DIR *dir = opendir(SD_SESSIONS_DIR);
    if (!dir) return;

    char path[80];
    struct dirent *de;
    int deleted = 0;
    while ((de = readdir(dir)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".csv") != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, de->d_name);
        if (unlink(path) == 0) deleted++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Apagadas %d sessoes", deleted);
}

int sd_read_session_laps(const char *filename, sd_lap_summary_t *out, int max_laps)
{
    if (!s_mounted || !filename || !out || max_laps <= 0) return 0;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, filename);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[160];
    if (!fgets(line, sizeof(line), f)) { /* arquivo vazio, nem cabecalho */
        fclose(f);
        return 0;
    }

    /* A coluna lap_time_ms conta o tempo decorrido desde o ultimo
     * cruzamento e reseta logo apos cada volta fechar; delta_ms reflete
     * a volta que ACABOU de fechar no exato momento da transicao. Por
     * isso uma volta so e "fechada" quando o numero da volta muda na
     * linha seguinte - usamos o lap_time_ms da ULTIMA linha da volta
     * anterior e o delta_ms da PRIMEIRA linha da volta nova. */
    uint32_t last_seen_lap = 0;
    uint32_t last_seen_lap_time = 0;
    float    last_seen_max_speed = 0.0f;
    int count = 0;

    while (count < max_laps && fgets(line, sizeof(line), f)) {
        unsigned long t_ms, lap, lap_time_ms, best_lap_ms;
        double lat, lon, speed, heading;
        unsigned sats;
        long delta_ms;

        int n = sscanf(line, "%lu,%lf,%lf,%lf,%lf,%u,%lu,%lu,%lu,%ld",
                        &t_ms, &lat, &lon, &speed, &heading, &sats,
                        &lap, &lap_time_ms, &best_lap_ms, &delta_ms);
        if (n != 10) continue;

        if (last_seen_lap != 0 && lap != last_seen_lap) {
            out[count].lap_number    = last_seen_lap;
            out[count].lap_time_ms   = last_seen_lap_time;
            out[count].delta_ms      = (int32_t)delta_ms;
            out[count].max_speed_kmh = last_seen_max_speed;
            count++;
            last_seen_max_speed = 0.0f; /* zera pro proximo lap */
        }
        if ((float)speed > last_seen_max_speed) last_seen_max_speed = (float)speed;
        last_seen_lap = (uint32_t)lap;
        last_seen_lap_time = (uint32_t)lap_time_ms;
    }

    fclose(f);
    return count;
}
