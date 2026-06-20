/*
 * wifi_export.c - ver wifi_export.h pro contexto.
 */
#include "wifi_export.h"
#include "config.h"
#include "app_events.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wifi_export";

static bool        s_active = false;
static bool        s_wifi_inited = false;
static httpd_handle_t s_httpd = NULL;
static char         s_ssid[32];
static char         s_password[64] = WIFI_AP_PASSWORD_DEFAULT;
static int64_t      s_last_activity_us = 0;
static esp_timer_handle_t s_idle_timer = NULL;

static void touch_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

/* ---------------------------------------------------------------------
 * HTTP - lista sessoes + download. Path traversal corrigido aqui: so
 * aceita basename, rejeita "..". O codigo da v1 (nunca usado em
 * producao) montava o path direto do parametro da URL sem checar nada.
 * --------------------------------------------------------------------- */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<html><body><h1>KartBox - sessoes</h1><ul>");

    DIR *dir = opendir(SD_SESSIONS_DIR);
    if (dir) {
        struct dirent *de;
        char line[160];
        while ((de = readdir(dir)) != NULL) {
            size_t len = strlen(de->d_name);
            if (len < 5 || strcmp(de->d_name + len - 4, ".csv") != 0) continue;
            snprintf(line, sizeof(line), "<li><a href=\"/download?file=%s\">%s</a></li>",
                     de->d_name, de->d_name);
            httpd_resp_sendstr_chunk(req, line);
        }
        closedir(dir);
    }

    httpd_resp_sendstr_chunk(req, "</ul></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    touch_activity();

    char query[160];
    char filename[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "file", filename, sizeof(filename));
    }

    /* SO o nome do arquivo. Rejeita qualquer coisa que tente sair da
     * pasta de sessoes. */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (base[0] == '\0' || strstr(base, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome de arquivo invalido");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_SESSIONS_DIR, base);

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char disp_hdr[96];
    snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri    = { .uri = "/",         .method = HTTP_GET, .handler = index_get_handler };
    const httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler };
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &download_uri);
    return ESP_OK;
}

/* ---------------------------------------------------------------------
 * Desliga sozinho depois de tempo parado - assim o WiFi (caro em RAM/
 * energia) so fica no ar quando alguem de fato esta baixando algo.
 * --------------------------------------------------------------------- */
static void idle_check_cb(void *arg)
{
    (void)arg;
    if (!s_active) return;
    int64_t elapsed_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (elapsed_ms >= WIFI_AP_IDLE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "AP ocioso ha %lld ms, desligando", (long long)elapsed_ms);
        wifi_export_stop();
        app_event_post(APP_EVT_WIFI_TIMEOUT, EVT_SRC_INTERNAL);
    }
}

static void start_idle_timer(void)
{
    const esp_timer_create_args_t targs = {
        .callback = idle_check_cb,
        .name = "wifi_idle",
    };
    esp_timer_create(&targs, &s_idle_timer);
    esp_timer_start_periodic(s_idle_timer, 30LL * 1000 * 1000); /* checa a cada 30s */
}

static void stop_idle_timer(void)
{
    if (s_idle_timer) {
        esp_timer_stop(s_idle_timer);
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
}

/* ---------------------------------------------------------------------
 * API publica
 * --------------------------------------------------------------------- */
void wifi_export_init(void)
{
    static bool netif_ready = false;
    if (!netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        netif_ready = true;
    }

    /* MAC lido direto da eFuse - nao precisa do driver WiFi rodando
     * ainda, da pra montar o SSID bem antes de subir o radio. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "%s%02X%02X", WIFI_AP_SSID_PREFIX, mac[4], mac[5]);

    ESP_LOGI(TAG, "WiFi export pronto (sob demanda) - SSID sera \"%s\"", s_ssid);
}

bool wifi_export_start(void)
{
    if (s_active) return false;

    if (!s_wifi_inited) {
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&init_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init falhou (%s) - confira esp_wifi_remote/esp_hosted no idf_component.yml",
                      esp_err_to_name(err));
            return false;
        }
        s_wifi_inited = true;
    }

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.ap.ssid, s_ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.ssid_len = strlen(s_ssid);
    strncpy((char *)wifi_cfg.ap.password, s_password, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.channel = WIFI_AP_CHANNEL;
    wifi_cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao subir AP (%s)", esp_err_to_name(err));
        return false;
    }

    if (start_http_server() != ESP_OK) {
        esp_wifi_stop();
        return false;
    }

    s_last_activity_us = esp_timer_get_time();
    start_idle_timer();
    s_active = true;
    ESP_LOGI(TAG, "AP \"%s\" ativo - export em http://192.168.4.1/", s_ssid);
    return true;
}

void wifi_export_stop(void)
{
    if (!s_active) return;
    stop_idle_timer();
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();
    s_active = false;
    ESP_LOGI(TAG, "AP de export desligado");
}

bool wifi_export_is_active(void)
{
    return s_active;
}

const char *wifi_export_get_ssid(void)
{
    return s_ssid;
}

const char *wifi_export_get_password(void)
{
    return s_password;
}

void wifi_export_set_password(const char *password)
{
    if (!password || strlen(password) < 8) return; /* WPA2 exige minimo 8 chars */
    strncpy(s_password, password, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';
}
