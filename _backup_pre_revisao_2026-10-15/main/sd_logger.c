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
#include <errno.h>
#include <math.h>
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
        errno = 0;
        if (mkdir(SD_SESSIONS_DIR, 0755) != 0) {
            ESP_LOGW(TAG, "Nao consegui criar %s (errno %d: %s)", SD_SESSIONS_DIR, errno, strerror(errno));
        }
    }
}

/* Monta e le funcionam mesmo com a escrita quebrada por algum motivo -
 * antes isso so aparecia minutos depois como "Falha ao criar
 * sessao_XX.csv" no meio de uma sessao, sem pista nenhuma do motivo
 * real. Testa escrita de verdade logo apos montar, pra avisar alto e na
 * hora. Nome de arquivo 8.3-safe de proposito (sem ponto no inicio -
 * convencao Unix de "arquivo oculto" que nao significa nada pro FAT;
 * usado aqui so pra nao arriscar confundir com o bug real que a gente
 * ACHOU por causa desse teste: FATFS_LFN_NONE recusando nomes fora do
 * padrao 8.3 estrito - ver CONFIG_FATFS_LFN_HEAP no sdkconfig). */
static bool sd_check_writable(void)
{
    char path[80];
    snprintf(path, sizeof(path), "%s/wtest.tmp", SD_MOUNT_POINT);

    errno = 0;
    FILE *f = fopen(path, "w");
    if (!f) {
        /* errno aqui e' a pista real: EROFS = FS/cartao recusando escrita
         * de verdade (lock por comando SD, nao so switch mecanico - PC
         * costuma nem checar/emitir esse lock); EIO = falha de
         * comunicacao (sinal/alimentacao marginal na escrita, que puxa
         * mais corrente que leitura); ENOSPC = cartao se achando cheio
         * (cluster/allocation_unit mal interpretado). */
        ESP_LOGE(TAG, "sd_check_writable: fopen(%s) falhou - errno %d: %s", path, errno, strerror(errno));
        return false;
    }
    bool ok = (fputc('x', f) != EOF) && (fflush(f) == 0);
    if (!ok) {
        ESP_LOGE(TAG, "sd_check_writable: escrita falhou apos abrir - errno %d: %s", errno, strerror(errno));
    }
    fclose(f);
    remove(path);
    return ok;
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
    /* SLOT_1 aqui colidia com o esp_hosted (C6 tambem configurado pro
     * "slot 1" - ver CONFIG_ESP_HOSTED_SDIO_SLOT no sdkconfig, e a doc
     * oficial "Slot 0 = SD card, Slot 1 = C6 onboard"). Os pinos fisicos
     * do SD sao todos setados abaixo via GPIO matrix (slot_cfg.clk/cmd/
     * d0-d3), entao o numero do slot aqui e' so identidade logica pro
     * controlador SDMMC compartilhado - SLOT_0 nao muda nenhum pino,
     * so para de brigar com o esp_hosted pelo mesmo slot 1. Era a causa
     * real do crash "xQueueSemaphoreTake queue.c:1709" dentro do
     * hosted_sdio_card_init() ao ligar WiFi/BT (ver esp-hosted-mcu#124). */
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    /* ATUALIZACAO: a suspeita original aqui (D1-D3 marginais causando
     * falha so na escrita) NAO era a causa real - confirmado depois que
     * a causa de verdade era FATFS_LFN_NONE recusando nomes fora do 8.3
     * estrito (ver CONFIG_FATFS_LFN_HEAP no sdkconfig e o comentario em
     * sd_check_writable()). Mantendo width=1 mesmo assim porque bate com
     * a v1 (telemetry_sd.c) e e' banda de sobra pro volume de dado real
     * (CSV pequeno a 10Hz) - sem motivo pra arriscar voltar pro 4-bit
     * sem necessidade. */
    slot_cfg.width = 1;
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

    if (!sd_check_writable()) {
        /* Se aparecer com errno EINVAL, quase certeza que e' NOME DE
         * ARQUIVO incompativel com 8.3 estrito (CONFIG_FATFS_LFN_NONE) -
         * ja corrigido pro LFN_HEAP no sdkconfig, mas fica o log alto
         * caso volte a acontecer por outro motivo (cartao real
         * travado/danificado, EROFS ou EIO). */
        ESP_LOGE(TAG, "!!! Cartao SD montado mas ESCRITA FALHOU - sessoes/pistas NAO vao gravar. "
                       "Ver errno acima: EINVAL costuma ser nome de arquivo incompativel com FAT "
                       "8.3 (checar CONFIG_FATFS_LFN_HEAP); EROFS/EIO sugerem cartao travado/com defeito.");
    }
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
        /* s1_ms/s2_ms: splits cumulativos da volta em andamento (0 = setor
         * ainda nao cruzado). O leitor pega os da ULTIMA linha de cada
         * volta = splits finais dela. Formato assume GPS_MAX_SECTORS==2 -
         * se mudar, atualizar aqui, no cabecalho e no leitor. */
        _Static_assert(GPS_MAX_SECTORS == 2, "colunas s1_ms/s2_ms assumem 2 setores");
        fprintf(s_session_file, "%lu,%.6f,%.6f,%.1f,%.1f,%u,%lu,%lu,%lu,%ld,%lu,%lu\n",
                (unsigned long)t_ms,
                entry.sample.lat, entry.sample.lon,
                (double)entry.sample.speed_kmh, (double)entry.sample.heading_deg,
                (unsigned)entry.sample.satellites,
                (unsigned long)entry.sample.lap_number,
                (unsigned long)entry.sample.lap_time_ms,
                (unsigned long)entry.sample.best_lap_ms,
                (long)entry.sample.last_delta_ms,
                (unsigned long)entry.sample.sector_split_ms[0],
                (unsigned long)entry.sample.sector_split_ms[1]);

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

    /* Task sempre sobe, monte ou nao o cartao agora - ela mesma checa
     * s_recording/s_session_file e vira no-op se nao ha sessao/cartao.
     * Bug real encontrado: antes essa task SO nascia se o mount inicial
     * desse certo aqui, e nunca era criada de novo depois (nem em
     * sd_logger_remount(), que so remonta o FS, sem recriar a task). Se
     * o cartao nao estivesse presente/legivel bem no boot (ex: formatado
     * NTFS), a task de gravacao nunca existia pro resto do power-cycle -
     * mesmo remontando o cartao bom depois (troca de cartao + modo USB,
     * ou card hot-plugado), nada jamais era escrito, porque nao tinha
     * quem consumisse a fila. Subindo sempre aqui elimina essa classe de
     * bug de uma vez. */
    xTaskCreate(sd_logger_task, "sd_logger", 4096, NULL, 7, NULL);

    s_mounted = sd_power_and_mount();
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD indisponivel no boot - kartbox segue sem gravar ate remontar (task de log ja de pe)");
        return false;
    }

    ESP_LOGI(TAG, "SD montado, logger pronto");
    return true;
}

bool sd_logger_is_mounted(void)
{
    return s_mounted;
}

QueueHandle_t sd_logger_get_queue(void)
{
    return s_log_queue;
}

bool sd_logger_start_session(const char *track_name)
{
    if (!s_mounted) return false;
    if (s_recording) sd_logger_stop_session();

    /* Sufixo com o nome da pista (quando ha pista carregada) pra ficar
     * visivel no nome do arquivo/pagina de export. Sanitizado pra so
     * [A-Za-z0-9_-] (max 16 chars) - nome vai pra path do SD e pra URL,
     * entao nada de espaco/acento/pontuacao. Vazio => so datetime. */
    char tsuf[20] = "";
    if (track_name && track_name[0]) {
        int k = 0;
        tsuf[k++] = '_';
        for (const char *p = track_name; *p && k < (int)sizeof(tsuf) - 1; p++) {
            char c = *p;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-') tsuf[k++] = c;
            else if (c == ' ' || c == '_')          tsuf[k++] = '_';
            /* demais caracteres (acento, pontuacao) sao descartados */
        }
        tsuf[k] = '\0';
        if (k == 1) tsuf[0] = '\0'; /* so sobrou "_" -> descarta */
    }

    char path[96];
    struct tm local_tm;
    if (gps_get_local_datetime(&local_tm)) {
        snprintf(path, sizeof(path), "%s/%04d%02d%02d_%02d%02d%02d%s.csv",
                 SD_SESSIONS_DIR,
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, tsuf);
    } else {
        /* Sem fix de GPS ainda (ex: ligou debaixo de teto) - usa o
         * relogio interno desde o boot como nome generico. */
        snprintf(path, sizeof(path), "%s/sessao_%lld%s.csv",
                 SD_SESSIONS_DIR, (long long)(esp_timer_get_time() / 1000000), tsuf);
    }

    s_session_file = fopen(path, "w");
    if (!s_session_file) {
        ESP_LOGE(TAG, "Falha ao criar %s", path);
        return false;
    }

    fprintf(s_session_file, "t_ms,lat,lon,speed_kmh,heading_deg,sats,lap,lap_time_ms,best_lap_ms,delta_ms,s1_ms,s2_ms\n");
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

    char path[128];
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
    uint32_t last_seen_split[2] = {0, 0};
    double   speed_sum = 0.0;
    uint32_t speed_samples = 0;
    int count = 0;

    while (count < max_laps && fgets(line, sizeof(line), f)) {
        unsigned long t_ms, lap, lap_time_ms, best_lap_ms;
        double lat, lon, speed, heading;
        unsigned sats;
        long delta_ms;
        unsigned long s1_ms = 0, s2_ms = 0;

        /* s1_ms/s2_ms sao opcionais: CSV antigo tem 10 colunas, novo tem
         * 12. n>=10 aceita os dois; splits ficam 0 no formato antigo. */
        int n = sscanf(line, "%lu,%lf,%lf,%lf,%lf,%u,%lu,%lu,%lu,%ld,%lu,%lu",
                        &t_ms, &lat, &lon, &speed, &heading, &sats,
                        &lap, &lap_time_ms, &best_lap_ms, &delta_ms, &s1_ms, &s2_ms);
        if (n < 10) continue;

        if (last_seen_lap != 0 && lap != last_seen_lap) {
            out[count].lap_number    = last_seen_lap;
            out[count].lap_time_ms   = last_seen_lap_time;
            out[count].delta_ms      = (int32_t)delta_ms;
            out[count].max_speed_kmh = last_seen_max_speed;
            out[count].avg_speed_kmh = (speed_samples > 0) ? (float)(speed_sum / speed_samples) : 0.0f;
            /* splits finais da volta que fechou = os da ULTIMA linha dela
             * (cumulativos; zerados pelo gps.c logo apos o cruzamento,
             * entao a 1a linha da volta nova ja vem limpa) */
            out[count].sector_ms[0]  = last_seen_split[0];
            out[count].sector_ms[1]  = last_seen_split[1];
            count++;
            last_seen_max_speed = 0.0f; /* zera pro proximo lap */
            speed_sum = 0.0;
            speed_samples = 0;
        }
        if ((float)speed > last_seen_max_speed) last_seen_max_speed = (float)speed;
        /* amostras paradas (kart parado no grid/box) distorceriam a media pra baixo -
         * mesmo corte de 1km/h que a v1 usava (speed_sum so soma se speed_kmh > 1.0). */
        if (speed > 1.0) { speed_sum += speed; speed_samples++; }
        last_seen_lap = (uint32_t)lap;
        last_seen_lap_time = (uint32_t)lap_time_ms;
        last_seen_split[0] = (uint32_t)s1_ms;
        last_seen_split[1] = (uint32_t)s2_ms;
    }

    fclose(f);
    return count;
}

int sd_read_session_track(const char *filename, sd_track_point_t *out, int max_points)
{
    if (!s_mounted || !filename || !out || max_points <= 0) return 0;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, filename);

    /* 1a passada: so conta quantas linhas de dado validas existem, pra
     * calcular o passo de decimacao (stride). Sessao longa (10Hz, muitos
     * minutos) pode ter dezenas de milhares de linhas - nao cabe (nem
     * precisa) 1 ponto por amostra no mapa, que so tem ~700px de largura
     * de tela. */
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[160];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; } /* cabecalho */

    long total_lines = 0;
    while (fgets(line, sizeof(line), f)) total_lines++;

    if (total_lines < 2) { fclose(f); return 0; }

    int stride = (int)(total_lines / max_points);
    if (stride < 1) stride = 1;

    /* 2a passada: agora de verdade, decimando por stride. Projecao plana
     * simples (equiretangular) em torno do 1o ponto valido - pista de
     * kart tem no maximo algumas centenas de metros, erro de curvatura
     * da Terra nessa escala e irrelevante (so pra desenhar o formato, nao
     * pra navegacao). 111320m/grau de latitude e' constante o bastante
     * pra esse uso; longitude escala por cos(latitude). */
    rewind(f);
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; } /* cabecalho de novo */

    int count = 0;
    long line_idx = 0;
    bool have_origin = false;
    double origin_lat = 0.0, origin_lon = 0.0;
    double m_per_deg_lat = 111320.0;
    double m_per_deg_lon = 111320.0;

    while (count < max_points && fgets(line, sizeof(line), f)) {
        unsigned long t_ms, lap, lap_time_ms, best_lap_ms;
        double lat, lon, speed, heading;
        unsigned sats;
        long delta_ms;

        int n = sscanf(line, "%lu,%lf,%lf,%lf,%lf,%u,%lu,%lu,%lu,%ld",
                        &t_ms, &lat, &lon, &speed, &heading, &sats,
                        &lap, &lap_time_ms, &best_lap_ms, &delta_ms);
        line_idx++;
        if (n < 10) continue; /* CSV novo tem 12 colunas; as 10 primeiras bastam aqui */
        if (lat == 0.0 && lon == 0.0) continue; /* amostra sem fix (gravada como 0,0) */
        if ((line_idx - 1) % stride != 0) continue;

        if (!have_origin) {
            origin_lat = lat;
            origin_lon = lon;
            m_per_deg_lon = 111320.0 * cos(origin_lat * M_PI / 180.0);
            have_origin = true;
        }

        out[count].x_m = (float)((lon - origin_lon) * m_per_deg_lon);
        out[count].y_m = (float)((lat - origin_lat) * m_per_deg_lat);
        out[count].speed_kmh = (float)speed;
        out[count].lap = (uint32_t)lap; /* ghost do mapa: identifica a volta do ponto */
        count++;
    }

    fclose(f);
    return count;
}
