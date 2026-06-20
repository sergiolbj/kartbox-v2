/*
 * main.c - kartbox v2
 *
 * Unico dispatcher de eventos. Toda acao (botao fisico, toque na tela
 * mais pra frente, BLE no futuro) passa por handle_event() UMA vez -
 * era exatamente a falta disso que causava o bug da v1 (reset fisico e
 * reset por toque fazendo coisas diferentes).
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"

#include "config.h"
#include "settings.h"
#include "app_events.h"
#include "display_init.h"
#include "gps.h"
#include "sd_logger.h"
#include "track_manager.h"
#include "ble_telemetry.h"
#include "wifi_export.h"
#include "usb_mode.h"
#include "ui.h"

static const char *TAG = "main";

typedef enum {
    SESSION_IDLE,
    SESSION_RECORDING,
} session_state_t;

static session_state_t s_session_state = SESSION_IDLE;
static gps_race_mode_t s_mode = GPS_MODE_QUALY;

static void start_session(void)
{
    gps_session_reset();
    gps_session_set_recording(true);
    if (!sd_logger_start_session()) {
        ESP_LOGW(TAG, "SD indisponivel - sessao roda so com telemetria ao vivo, sem gravar");
    }
    s_session_state = SESSION_RECORDING;
    ui_set_recording_state(true);
    ESP_LOGI(TAG, "Sessao iniciada (modo %s)", s_mode == GPS_MODE_CORRIDA ? "CORRIDA" : "QUALY");
}

static void stop_session(void)
{
    gps_session_set_recording(false);
    sd_logger_stop_session();
    s_session_state = SESSION_IDLE;
    ui_set_recording_state(false);
    ESP_LOGI(TAG, "Sessao encerrada");
}

static void handle_event(const app_event_t *evt)
{
    switch (evt->type) {

    case APP_EVT_BTN_MODE:
        if (s_session_state == SESSION_RECORDING) {
            ESP_LOGW(TAG, "Sessao gravando - troca de modo ignorada");
            break;
        }
        s_mode = (s_mode == GPS_MODE_QUALY) ? GPS_MODE_CORRIDA : GPS_MODE_QUALY;
        gps_set_mode(s_mode);
        ui_set_mode_label(s_mode);
        break;

    case APP_EVT_BTN_SETLINE:
        if (s_session_state == SESSION_RECORDING) {
            /* Clique acidental durante sessao: linha nao pode ser alterada
             * enquanto dados estao gravando. Piloto para a sessao primeiro
             * (hold RESET) e reconfigura em seguida. */
            ESP_LOGW(TAG, "Sessao gravando - SET LINE ignorado");
            break;
        }
        gps_set_finish_line();
        ui_update_pista_status();
        ui_show_toast("Linha de chegada marcada", 2500);
        break;

    case APP_EVT_BTN_RESET:
        /* Tap simples: so inicia (acao de baixo risco). Encerrar exige
         * segurar - ver APP_EVT_BTN_RESET_HELD - pra nao perder sessao
         * por toque/aperto acidental. */
        if (s_session_state == SESSION_IDLE) {
            start_session();
        } else {
            /* Sessao ativa: mostra barra de progresso enquanto piloto segura */
            ui_show_hold_progress(true);
        }
        break;

    case APP_EVT_BTN_RESET_HOLD_ABORT:
        /* Soltou antes do tempo - cancela a barra de progresso */
        ui_show_hold_progress(false);
        break;

    case APP_EVT_BTN_RESET_HELD:
        if (s_session_state == SESSION_RECORDING) {
            ui_show_hold_progress(false);
            stop_session();
        }
        break;

    case APP_EVT_LAP_COMPLETE:
        ui_flash_lap_complete(evt->data.lap.lap_number, evt->data.lap.lap_time_ms,
                               evt->data.lap.delta_ms, evt->data.lap.is_new_best);
        break;

    case APP_EVT_SD_DELETE_ALL:
        sd_delete_all_sessions();
        ui_refresh_session_list();
        break;

    case APP_EVT_USB_MODE_TOGGLE:
        if (usb_mode_is_active()) {
            usb_mode_exit();
            ui_set_usb_mode_state(false);
        } else if (s_session_state == SESSION_RECORDING) {
            ESP_LOGW(TAG, "Sessao gravando - modo USB ignorado de proposito");
        } else if (usb_mode_enter()) {
            ui_set_usb_mode_state(true);
        }
        break;

    case APP_EVT_WIFI_EXPORT_TOGGLE:
        if (wifi_export_is_active()) {
            wifi_export_stop();
            ui_set_wifi_export_state(false, NULL, NULL);
        } else if (s_session_state == SESSION_RECORDING) {
            ESP_LOGW(TAG, "Sessao gravando - WiFi export ignorado de proposito (evita disputa de CPU/radio)");
        } else if (wifi_export_start()) {
            ui_set_wifi_export_state(true, wifi_export_get_ssid(), wifi_export_get_password());
        }
        break;

    case APP_EVT_SESSION_SELECT:
        ui_show_session_laps(evt->data.session_index);
        break;

    case APP_EVT_SD_WRITE_ERROR:
        ESP_LOGE(TAG, "Erro de escrita no SD reportado");
        break;

    case APP_EVT_WIFI_TIMEOUT:
        /* wifi_export_stop() ja foi chamado dentro do proprio
         * wifi_export.c antes de postar esse evento - aqui so reflete
         * na UI. */
        ui_set_wifi_export_state(false, NULL, NULL);
        break;

    case APP_EVT_SECTOR_MARK: {
        int idx = (int)evt->data.param;
        gps_set_sector_point(idx);
        /* persiste as coordenadas recem-marcadas em NVS */
        double lat, lon; float hdg;
        if (gps_get_sector_point(idx, &lat, &lon, &hdg)) {
            settings_set_sector(idx, lat, lon, hdg);
        }
        ui_update_sector_status();
        break;
    }

    case APP_EVT_SECTOR_CLEAR: {
        int idx = (int)evt->data.param;
        gps_clear_sector_point(idx);
        settings_clear_sector(idx);
        ui_update_sector_status();
        break;
    }

    case APP_EVT_GPS_SAMPLE:
        /* nao usado de proposito - UI le gps_get_latest() direto no
         * proprio timer de refresh, ver gps.h pra explicacao */
        break;

    /* ------------------------------------------------------------------
     * Aba PISTA - gestao de configs de pista no SD
     * ------------------------------------------------------------------ */
    case APP_EVT_TRACK_LOAD: {
        track_config_t cfg;
        if (!track_manager_load(evt->data.track_name, &cfg)) {
            ui_show_toast("Erro ao carregar pista", 2500);
            break;
        }
        /* Aplica finish line ao GPS */
        if (cfg.finish.is_set) {
            gps_load_finish_line(cfg.finish.lat, cfg.finish.lon, cfg.finish.heading_deg);
        }
        /* Aplica setores ao GPS e persiste em NVS */
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            if (cfg.sectors[i].is_set) {
                gps_load_sector(i, cfg.sectors[i].lat, cfg.sectors[i].lon, cfg.sectors[i].heading_deg);
                settings_set_sector(i, cfg.sectors[i].lat, cfg.sectors[i].lon, cfg.sectors[i].heading_deg);
            } else {
                gps_clear_sector_point(i);
                settings_clear_sector(i);
            }
        }
        settings_set_last_track(cfg.name);
        ui_on_track_loaded(&cfg);
        ui_show_toast(cfg.name, 2000);
        ESP_LOGI(TAG, "Pista \"%s\" aplicada ao GPS", cfg.name);
        break;
    }

    case APP_EVT_TRACK_SAVE: {
        track_config_t cfg;
        if (!ui_get_track_draft(&cfg)) {
            ui_show_toast("Nome da pista vazio", 2000);
            break;
        }
        if (!track_manager_save(&cfg)) {
            ui_show_toast("Erro ao salvar no SD", 2500);
            break;
        }
        settings_set_last_track(cfg.name);
        ui_refresh_track_list();
        ui_show_toast("Pista salva", 2000);
        break;
    }

    case APP_EVT_TRACK_DELETE: {
        if (!track_manager_delete(evt->data.track_name)) {
            ui_show_toast("Erro ao excluir", 2000);
            break;
        }
        ui_refresh_track_list();
        ui_show_toast("Pista excluida", 2000);
        break;
    }

    case APP_EVT_TRACK_NEW:
        /* Limpa estado de pista atual no GPS */
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            gps_clear_sector_point(i);
            settings_clear_sector(i);
        }
        ui_on_track_loaded(NULL); /* NULL = nova pista em branco */
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    settings_init();

    lv_display_t *disp = display_init_start();
    if (!disp) {
        ESP_LOGE(TAG, "Display nao iniciou - sem tela nao da pra continuar");
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
        ui_init(disp);
        lvgl_port_unlock();
    }
    display_backlight_on();

    bool sd_ok = sd_logger_init();
    if (sd_ok) {
        track_manager_init(); /* escaneia /sdcard/tracks/ */
    }

    gps_init(sd_ok ? sd_logger_get_queue() : NULL);
    gps_set_utc_offset_min(settings_get_utc_offset_min());
    gps_set_gate_radius_m(settings_get_gate_radius_m());
    gps_set_min_lap_time_ms(settings_get_min_lap_time_ms());

    /* Tenta restaurar a ultima pista usada do SD.
     * Se nao encontrar (SD ausente, arquivo apagado), cai de volta nos
     * setores salvos no NVS (caminho antigo). */
    bool track_restored = false;
    const char *last = settings_get_last_track();
    if (sd_ok && last[0] != '\0') {
        track_config_t cfg;
        if (track_manager_load(last, &cfg)) {
            if (cfg.finish.is_set) {
                gps_load_finish_line(cfg.finish.lat, cfg.finish.lon, cfg.finish.heading_deg);
            }
            for (int i = 0; i < GPS_MAX_SECTORS; i++) {
                if (cfg.sectors[i].is_set) {
                    gps_load_sector(i, cfg.sectors[i].lat, cfg.sectors[i].lon, cfg.sectors[i].heading_deg);
                }
            }
            track_restored = true;
            ESP_LOGI(TAG, "Pista \"%s\" restaurada do SD", last);
        }
    }
    if (!track_restored) {
        /* Fallback: restaura setores salvos individualmente em NVS */
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            settings_sector_t sec = settings_get_sector(i);
            if (sec.is_set) {
                gps_load_sector(i, sec.lat, sec.lon, sec.heading_deg);
            }
        }
    }

    app_events_init();

    ble_telemetry_init();
    ble_telemetry_set_device_name(settings_get_ble_name());

    wifi_export_init();
    wifi_export_set_password(settings_get_wifi_password());

    usb_mode_init();

    ui_refresh_track_list();     /* popula dropdown da aba PISTA */
    ui_update_pista_status();    /* reflete finish + setores no boot */
    ui_set_mode_label(s_mode);
    ui_set_recording_state(false);

    ESP_LOGI(TAG, "kartbox v2 pronto");

    app_event_t evt;
    for (;;) {
        if (xQueueReceive(g_app_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            handle_event(&evt);
        }
    }
}
