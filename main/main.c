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
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "esp_partition.h"

#include "config.h"
#include "settings.h"
#include "app_events.h"
#include "battery.h"
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

/* Confirmacao de "sobrescrever linha ja marcada" - ver APP_EVT_BTN_SETLINE.
 * 0 = desarmado. Timestamp (us) do primeiro toque = armado, aguardando
 * confirmacao dentro da janela abaixo. */
static int64_t s_setline_armed_us = 0;
#define SETLINE_CONFIRM_WINDOW_MS (4000)

/* Crash na sessao anterior? O coredump ELF fica na particao "coredump"
 * (ver partitions.csv); aqui ele e' copiado pro SD com nome unico e a
 * particao e' apagada (senao o proximo boot salvaria duplicado). Vira
 * arquivo analisavel em casa, sem cabo no kartodromo:
 *   espcoredump.py info_corefile -c coredump_N.elf build/kartbox.elf
 * Chamada depois do SD montar e da UI subir (toast avisa o piloto). */
static void save_coredump_to_sd(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) return;

    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        /* Sem dump valido. Se a particao tem LIXO (regiao da flash nunca
         * apagada - acontece na primeira vez apos adicionar a particao a
         * tabela), o verificador de boot reclama "Incorrect size" em todo
         * boot. Detecta (primeiro word != 0xFFFFFFFF de flash apagada) e
         * apaga UMA vez pra silenciar de vez. */
        uint32_t first_word = 0;
        if (esp_partition_read(part, 0, &first_word, sizeof(first_word)) == ESP_OK &&
            first_word != 0xFFFFFFFF) {
            ESP_LOGW(TAG, "Particao coredump com lixo (regiao nunca apagada) - limpando");
            esp_core_dump_image_erase();
        }
        return; /* sem crash pendente - caminho normal de todo boot */
    }

    /* nome unico: primeiro coredump_N.elf livre */
    char path[64];
    for (int n = 0; n < 100; n++) {
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/coredump_%d.elf", n);
        FILE *probe = fopen(path, "r");
        if (!probe) break;
        fclose(probe);
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Coredump pendente mas SD indisponivel - fica na flash pro proximo boot");
        return;
    }

    static uint8_t buf[512];
    size_t off = 0;
    while (off < size) {
        size_t chunk = (size - off) > sizeof(buf) ? sizeof(buf) : (size - off);
        if (esp_partition_read(part, off, buf, chunk) != ESP_OK) break;
        fwrite(buf, 1, chunk, f);
        off += chunk;
    }
    fclose(f);

    if (off == size) {
        esp_core_dump_image_erase();
        ESP_LOGW(TAG, "Coredump do crash anterior salvo: %s (%u bytes)", path, (unsigned)size);
        ui_show_toast("Crash anterior salvo no SD (coredump)", 3500);
    } else {
        ESP_LOGE(TAG, "Falha ao copiar coredump (%u de %u bytes) - mantido na flash",
                 (unsigned)off, (unsigned)size);
        remove(path);
    }
}

static void start_session(void)
{
    gps_session_reset();
    gps_session_set_recording(true);
    /* passa a pista carregada (se houver) pro nome do arquivo da sessao */
    if (!sd_logger_start_session(settings_get_last_track())) {
        /* Antes so ia pro log serial - piloto na pista nunca via isso e
         * achava que a sessao tinha gravado normal. Agora avisa na tela
         * tambem, no momento exato que a sessao "comeca" sem gravar nada. */
        ESP_LOGW(TAG, "SD indisponivel - sessao roda so com telemetria ao vivo, sem gravar");
        ui_show_toast("SD indisponivel - voltas NAO serao salvas", 3000);
    }
    s_session_state = SESSION_RECORDING;
    ui_set_recording_state(true);
    ESP_LOGI(TAG, "Sessao iniciada (modo %s)", s_mode == GPS_MODE_CORRIDA ? "CORRIDA" : "QUALY");
}

static void stop_session(void)
{
    /* Snapshot das estatisticas ANTES de mexer no estado - elas so sao
     * zeradas no reset da PROXIMA sessao, mas capturar aqui deixa a
     * ordem obvia. */
    gps_session_stats_t stats;
    gps_get_session_stats(&stats);

    gps_session_set_recording(false);
    sd_logger_stop_session();
    s_session_state = SESSION_IDLE;
    ui_set_recording_state(false);
    ESP_LOGI(TAG, "Sessao encerrada (%u voltas)", (unsigned)stats.lap_count);

    /* Resumo na tela - so se houve pelo menos 1 volta fechada (sessao
     * vazia nao tem o que resumir, e o popup so atrapalharia). */
    if (stats.lap_count > 0) {
        ui_show_session_stats(&stats);
    }
}

/* Aplica uma track_config_t carregada do SD ao estado ao vivo do GPS
 * (linha de chegada + setores, persistindo setores em NVS tambem) - usado
 * tanto por APP_EVT_TRACK_LOAD (carregar pra pilotar, vai pra CORRIDA)
 * quanto por APP_EVT_TRACK_EDIT (carregar pra editar, fica na aba PISTA).
 * Extraido pra nao duplicar essa logica entre os 2 (antes so existia
 * dentro do case TRACK_LOAD). */
static void apply_track_cfg_to_gps(const track_config_t *cfg)
{
    if (cfg->finish.is_set) {
        gps_load_finish_line(cfg->finish.lat, cfg->finish.lon, cfg->finish.heading_deg);
    }
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (cfg->sectors[i].is_set) {
            gps_load_sector(i, cfg->sectors[i].lat, cfg->sectors[i].lon, cfg->sectors[i].heading_deg);
            settings_set_sector(i, cfg->sectors[i].lat, cfg->sectors[i].lon, cfg->sectors[i].heading_deg);
        } else {
            gps_clear_sector_point(i);
            settings_clear_sector(i);
        }
    }
}

static void handle_event(const app_event_t *evt)
{
    /* Qualquer botao fisico acorda o protetor de tela (o toque ja e' contado
     * pela inatividade nativa do LVGL; botao GPIO nao passa pelo indev). */
    if (evt->source == EVT_SRC_GPIO) {
        ui_screensaver_notify_activity();
    }

    switch (evt->type) {

    case APP_EVT_BTN_MODE_PRESSED:
        /* apertou (ainda nao se sabe se e' tap ou hold) - arma a barra
         * de progresso; ela so aparece se o dedo passar da carencia */
        ui_show_mode_hold_progress(true);
        break;

    case APP_EVT_BTN_MODE:
        /* TAP no MODE: cicla o layout da tela CORRIDA (completo/delta/
         * velocidade) - acao frequente e inofensiva, pode em movimento. */
        ui_show_mode_hold_progress(false);
        ui_cycle_race_layout();
        break;

    case APP_EVT_BTN_MODE_HELD:
        /* SEGURAR MODE: troca QUALY/RACE - acao rara e consequente,
         * mesmo padrao tap/hold do RESET. */
        ui_show_mode_hold_progress(false);
        if (s_session_state == SESSION_RECORDING) {
            ESP_LOGW(TAG, "Sessao gravando - troca de modo ignorada");
            ui_show_toast("Pare a sessao antes de trocar de modo", 2500);
            break;
        }
        s_mode = (s_mode == GPS_MODE_QUALY) ? GPS_MODE_CORRIDA : GPS_MODE_QUALY;
        gps_set_mode(s_mode);
        ui_set_mode_label(s_mode);
        break;

    case APP_EVT_BTN_SETLINE: {
        if (s_session_state == SESSION_RECORDING) {
            /* Clique acidental durante sessao: linha nao pode ser alterada
             * enquanto dados estao gravando. Piloto para a sessao primeiro
             * (hold RESET) e reconfigura em seguida. */
            ESP_LOGW(TAG, "Sessao gravando - SET LINE ignorado");
            break;
        }

        double cur_lat, cur_lon; float cur_hdg;
        bool already_set = gps_get_finish_line(&cur_lat, &cur_lon, &cur_hdg);
        if (already_set) {
            /* Ja tem linha marcada - exige confirmacao (segundo toque/
             * aperto dentro da janela) antes de sobrescrever, mesmo
             * parado (idle). Mesmo padrao de "apertar de novo pra
             * confirmar" ja usado em Apagar tudo (SD) na aba Config -
             * funciona igual pro botao fisico e pro toque na tela, ja
             * que os dois caem no mesmo evento aqui. */
            int64_t now_us = esp_timer_get_time();
            bool confirmed = (s_setline_armed_us != 0) &&
                              ((now_us - s_setline_armed_us) <= (int64_t)SETLINE_CONFIRM_WINDOW_MS * 1000);
            if (!confirmed) {
                s_setline_armed_us = now_us;
                ui_show_toast("Linha ja marcada - toque de novo pra sobrescrever", 3000);
                break;
            }
            s_setline_armed_us = 0;
        }

        if (gps_set_finish_line()) {
            ui_update_pista_status();
            ui_show_toast("Linha de chegada marcada", 2500);
        } else {
            ui_show_toast("Sem fix GPS - aguarde sinal", 2500);
        }
        break;
    }

    case APP_EVT_BTN_RESET:
        /* Tap simples: so inicia (acao de baixo risco). Encerrar exige
         * segurar - ver APP_EVT_BTN_RESET_HELD - pra nao perder sessao
         * por toque/aperto acidental. */
        if (s_session_state == SESSION_IDLE) {
            double lat, lon; float hdg;
            if (!gps_get_finish_line(&lat, &lon, &hdg)) {
                /* Sem linha de chegada nao ha como fechar volta - sessao
                 * so registraria "tempo decorrido" sem sentido nenhum.
                 * Ignora o start e avisa (o aviso da aba CORRIDA ja
                 * mostra isso o tempo todo, mas o toast reforca no
                 * momento exato que o piloto tentou começar). */
                ui_show_toast("Marque a linha de chegada antes de iniciar", 2500);
                break;
            }
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
            /* usb_mode_exit() remonta o FS (sd_logger_remount()) mas nao
             * reescaneia pistas nem avisa a UI - se o cartao voltou
             * montado, refaz esse passo aqui (idempotente, seguro chamar
             * de novo). Cobre o caso de trocar o cartao fisico durante o
             * modo pen drive. */
            if (sd_logger_is_mounted()) {
                track_manager_init();
                ui_refresh_track_list();
            }
        } else if (s_session_state == SESSION_RECORDING) {
            ESP_LOGW(TAG, "Sessao gravando - modo USB ignorado de proposito");
        } else if (usb_mode_enter()) {
            ui_set_usb_mode_state(true);
        }
        break;

    case APP_EVT_WIFI_EXPORT_TOGGLE:
        if (wifi_export_is_active()) {
            wifi_export_stop();
            if (wifi_export_get_mode() == WIFI_EXPORT_MODE_AP) {
                ui_set_wifi_export_state(false, NULL, NULL);
            } else {
                ui_set_wifi_sta_state(WIFI_STA_STATE_IDLE, NULL);
            }
            break;
        }

        /* wifi_export_start() bloqueia ate ~15s no modo STA (espera
         * conectar) - reflete "conectando..." na tela antes de chamar,
         * senao o botao fica "travado" sem feedback nenhum durante a
         * espera. */
        if (wifi_export_get_mode() == WIFI_EXPORT_MODE_STA) {
            ui_set_wifi_sta_state(WIFI_STA_STATE_CONNECTING, NULL);
        }

        if (wifi_export_start()) {
            if (wifi_export_get_mode() == WIFI_EXPORT_MODE_AP) {
                ui_set_wifi_export_state(true, wifi_export_get_ssid(), wifi_export_get_password());
            } else {
                char ip[16];
                wifi_export_get_sta_ip(ip, sizeof(ip));
                ui_set_wifi_sta_state(WIFI_STA_STATE_CONNECTED, ip);
            }
        } else {
            /* Bug real encontrado: falha aqui nunca dava feedback nenhum -
             * botao nao mudava, nada aparecia, usuario via "nao funciona"
             * sem pista do motivo (o erro real ja ia pro log serial via
             * ESP_LOGE dentro de wifi_export_start(), so nao chegava na
             * tela). */
            if (wifi_export_get_mode() == WIFI_EXPORT_MODE_STA) {
                ui_set_wifi_sta_state(wifi_export_get_sta_state(), NULL);
                ui_show_toast("Falha ao conectar - confira rede/senha", 2500);
            } else {
                ui_show_toast("Falha ao ativar WiFi - ver log serial", 2500);
            }
        }
        break;

    case APP_EVT_WIFI_MODE_SET:
        if (wifi_export_is_active()) {
            ui_show_toast("Desligue o WiFi antes de trocar de modo", 2500);
            break;
        }
        wifi_export_set_mode((wifi_export_mode_t)evt->data.param);
        settings_set_wifi_mode((uint8_t)evt->data.param);
        ui_set_wifi_mode_ui((wifi_export_mode_t)evt->data.param);
        break;

    case APP_EVT_WIFI_SCAN: {
        if (wifi_export_is_active()) {
            ui_show_toast("Desligue o WiFi antes de escanear", 2500);
            break;
        }
        char ssids[WIFI_SCAN_MAX_RESULTS][WIFI_SCAN_SSID_MAX];
        ui_show_toast("Escaneando redes...", 6000);
        int n = wifi_export_scan(ssids, WIFI_SCAN_MAX_RESULTS);
        ui_set_wifi_scan_results(ssids, n);
        ui_show_toast(n > 0 ? "Escaneamento concluido" : "Nenhuma rede encontrada", 2000);
        break;
    }

    case APP_EVT_WIFI_STA_CONNECT: {
        const char *ssid = evt->data.wifi_sta.ssid;
        const char *pass = evt->data.wifi_sta.password;
        if (ssid[0] == '\0') {
            ui_show_toast("Escolha uma rede antes de conectar", 2500);
            break;
        }
        wifi_export_set_sta_credentials(ssid, pass);
        settings_set_wifi_sta_ssid(ssid);
        settings_set_wifi_sta_password(pass);
        if (wifi_export_is_active()) {
            wifi_export_stop(); /* credenciais mudaram - reconecta do zero */
        }
        /* Forca modo STA aqui, defensivo - na pratica o bloco "Conectar"
         * so fica visivel/tocavel depois que o usuario ja trocou o switch
         * pra Cliente (que ja posta WIFI_MODE_SET com STA antes), mas nao
         * custa garantir mesmo se essa premissa mudar no futuro. */
        wifi_export_set_mode(WIFI_EXPORT_MODE_STA);
        settings_set_wifi_mode((uint8_t)WIFI_EXPORT_MODE_STA);
        ui_set_wifi_sta_state(WIFI_STA_STATE_CONNECTING, NULL);
        ui_show_toast("Conectando...", 6000);
        if (wifi_export_start()) {
            char ip[16];
            wifi_export_get_sta_ip(ip, sizeof(ip));
            ui_set_wifi_sta_state(WIFI_STA_STATE_CONNECTED, ip);
            ui_show_toast("Conectado", 2000);
        } else {
            ui_set_wifi_sta_state(wifi_export_get_sta_state(), NULL);
            ui_show_toast("Falha ao conectar - confira rede/senha", 3000);
        }
        break;
    }

    case APP_EVT_BLE_RADIO_TOGGLE: {
        bool new_state = !ble_telemetry_is_enabled();
        ble_telemetry_set_enabled(new_state);
        ui_set_ble_radio_state(new_state);
        /* Persiste - antes o switch voltava LIGADO em todo boot, o que
         * atrapalhava inclusive o teste de interferencia BLE x GPS
         * (usuario desligava, reiniciava pra testar, e o radio subia de
         * novo sozinho). */
        settings_set_ble_radio(new_state ? 1 : 0);
        break;
    }

    case APP_EVT_SESSION_SELECT:
        ui_show_session_laps(evt->data.session_index);
        break;

    case APP_EVT_SESSION_MAP:
        ui_show_session_map(evt->data.session_index);
        break;

    case APP_EVT_SD_WRITE_ERROR:
        ESP_LOGE(TAG, "Erro de escrita no SD reportado");
        break;

    case APP_EVT_WIFI_TIMEOUT:
        /* wifi_export_stop() ja foi chamado dentro do proprio
         * wifi_export.c antes de postar esse evento - aqui so reflete
         * na UI. */
        if (wifi_export_get_mode() == WIFI_EXPORT_MODE_AP) {
            ui_set_wifi_export_state(false, NULL, NULL);
        } else {
            ui_set_wifi_sta_state(WIFI_STA_STATE_IDLE, NULL);
        }
        break;

    case APP_EVT_SECTOR_MARK: {
        int idx = (int)evt->data.param;
        if (gps_set_sector_point(idx)) {
            /* persiste as coordenadas recem-marcadas em NVS */
            double lat, lon; float hdg;
            if (gps_get_sector_point(idx, &lat, &lon, &hdg)) {
                settings_set_sector(idx, lat, lon, hdg);
            }
            ui_update_sector_status();
            ui_show_toast("Setor marcado", 2000);
        } else {
            ui_show_toast("Sem fix GPS - aguarde sinal", 2500);
        }
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

    case APP_EVT_AUTO_SESSION_START: {
        /* Mesmas guardas do RESET manual - o gps.c so detecta movimento,
         * a decisao final mora aqui. Revalida tudo porque o evento pode
         * chegar atrasado (fila) em relacao ao estado atual. */
        if (s_session_state != SESSION_IDLE) break;
        if (usb_mode_is_active()) break; /* SD nas maos do PC - sem gravacao */
        double lat, lon; float hdg;
        if (!gps_get_finish_line(&lat, &lon, &hdg)) break;
        start_session();
        ui_show_toast("Sessao iniciada automaticamente", 2500);
        break;
    }

    case APP_EVT_AUTO_SESSION_STOP:
        if (s_session_state == SESSION_RECORDING) {
            stop_session(); /* popup de estatisticas ja avisa o encerramento */
        }
        break;

    case APP_EVT_CFG_BACKUP:
        if (!sd_logger_is_mounted()) {
            ui_show_toast("SD indisponivel - sem onde salvar o backup", 2500);
        } else if (settings_backup_to_file(SD_MOUNT_POINT "/kartbox_config.txt")) {
            ui_show_toast("Config salva em kartbox_config.txt (raiz do SD)", 3000);
        } else {
            ui_show_toast("Falha ao salvar backup no SD", 2500);
        }
        break;

    case APP_EVT_CFG_RESTORE:
        if (!sd_logger_is_mounted()) {
            ui_show_toast("SD indisponivel", 2500);
        } else if (settings_restore_from_file(SD_MOUNT_POINT "/kartbox_config.txt")) {
            /* Setters ja gravaram RAM+NVS; consumidores de runtime (gps,
             * switches ja construidos, tema) pegam tudo no proximo boot. */
            ui_show_toast("Config restaurada - reinicie para aplicar tudo", 3500);
        } else {
            ui_show_toast("kartbox_config.txt nao encontrado na raiz do SD", 3000);
        }
        break;

    /* ------------------------------------------------------------------
     * Aba PISTA - gestao de configs de pista no SD
     * ------------------------------------------------------------------ */
    case APP_EVT_TRACK_LOAD: {
        /* "Carregar": aplica a pista pra PILOTAR - toast com contagem e
         * vai sozinho pra aba CORRIDA. NAO abre o painel de edicao (o
         * piloto ja esta indo dirigir, nao editar pontos). */
        track_config_t cfg;
        if (!track_manager_load(evt->data.track_name, &cfg)) {
            ui_show_toast("Erro ao carregar pista", 2500);
            break;
        }
        apply_track_cfg_to_gps(&cfg);
        settings_set_last_track(cfg.name);
        ui_on_track_loaded(&cfg);
        ui_show_track_loaded_countdown(cfg.name);
        ESP_LOGI(TAG, "Pista \"%s\" aplicada ao GPS (carregar)", cfg.name);
        s_setline_armed_us = 0; /* pista trocou - descarta confirmacao pendente da anterior */
        break;
    }

    case APP_EVT_TRACK_EDIT: {
        /* "Editar": mesma aplicacao ao GPS que Carregar (precisa pro
         * fluxo de marcar/limpar setor bater com a pista certa), mas fica
         * na propria aba PISTA com o painel de campos aberto, em vez de
         * navegar pra CORRIDA - o piloto vai mexer nos pontos, nao dirigir. */
        track_config_t cfg;
        if (!track_manager_load(evt->data.track_name, &cfg)) {
            ui_show_toast("Erro ao carregar pista", 2500);
            break;
        }
        apply_track_cfg_to_gps(&cfg);
        settings_set_last_track(cfg.name);
        ui_on_track_loaded(&cfg);
        ui_show_pista_edit_panel(true);
        ui_show_toast("Editando pista - ajuste os pontos e toque Salvar Pista", 2500);
        ESP_LOGI(TAG, "Pista \"%s\" aplicada ao GPS (editar)", cfg.name);
        s_setline_armed_us = 0;
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
        /* Fecha o painel de edicao - tarefa de criar/editar essa pista
         * esta feita, volta pro estado inicial (Nova Pista + dropdown). */
        ui_show_pista_edit_panel(false);
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
        ui_show_pista_edit_panel(true);
        s_setline_armed_us = 0; /* pista limpou - descarta confirmacao pendente */
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
    /* liga no brilho salvo (nao 100% fixo) - preferencia da CONFIG > SISTEMA */
    display_set_brightness(settings_get_brightness());

    /* Tela de loading fica visivel desde ui_init() acima. Marca o instante
     * aqui pra garantir um tempo minimo de exibicao la embaixo, antes de
     * ui_boot_finish() - pedido do usuario: se o boot todo (GPS/SD/
     * pistas/BLE/WiFi) terminar rapido demais, a tela de loading pisca e
     * some quase instantaneo, o que parece mais bug/flicker do que
     * "carregando" de verdade. */
#define BOOT_SCREEN_MIN_MS (5000)
    int64_t boot_start_us = esp_timer_get_time();

    /* DIAGNOSTICO: GPS (UART1) sobe ANTES do SD (SDMMC). Suspeita: falha no
     * mount do SD (sem cartao) deixa o dominio de clock compartilhado num
     * estado que trava o uart_driver_install seguinte (crash "Interrupt
     * wdt timeout" em uart_ll_update, MEPC sempre dentro dele, logo apos
     * "SD indisponivel" no log). Testando se invertendo a ordem o crash
     * desaparece - se sim, confirma a hipotese. Fila do SD e' setada
     * depois, via gps_set_log_queue(), quando/se o mount funcionar. */
    /* ADC da bateria (GPIO53) - rapido, sem dependencia. Feito antes do
     * GPS so pra ja ter leitura valida quando a status bar aparecer. */
    battery_init();

    ui_boot_set_status("Iniciando GPS...");
    gps_init(NULL);
    gps_set_utc_offset_min(settings_get_utc_offset_min());
    gps_set_gate_radius_m(settings_get_gate_radius_m());
    gps_set_min_lap_time_ms(settings_get_min_lap_time_ms());

    ui_boot_set_status("Montando cartao SD...");
    bool sd_ok = sd_logger_init();
    /* Fila e' criada dentro de sd_logger_init() independente do mount ter
     * dado certo (ver comentario la) - conecta o GPS nela sempre, senao
     * um boot com SD ausente/ilegivel no arranque deixa gps.c apontando
     * pra fila NULL pro resto do power-cycle, mesmo que o cartao seja
     * remontado com sucesso depois (ex: troca de cartao + ciclo de modo
     * USB). Bug real que explicava "SD montado mas nada grava". */
    gps_set_log_queue(sd_logger_get_queue());
    if (sd_ok) {
        ui_boot_set_status("Carregando pistas...");
        track_manager_init(); /* escaneia /sdcard/tracks/ */
    }

    /* DE PROPOSITO: boot NAO carrega mais nenhuma pista/setor automatico
     * (nem a ultima usada do SD, nem o fallback de setores em NVS). Pedido
     * explicito do usuario: comecar sempre limpo (aba CORRIDA mostra o
     * aviso de "sem linha de chegada" ate o piloto escolher), e a escolha
     * de qual pista usar fica so na aba PISTA (dropdown "Carregar"). Isso
     * evita o risco de aplicar sem querer a configuracao de uma pista
     * errada/antiga so porque ela foi a ultima salva. */
    app_events_init();

    ui_boot_set_status("Ligando radio BLE...");
    ble_telemetry_init();
    ble_telemetry_set_device_name(settings_get_ble_name());
    /* Preferencia persistida do switch CONFIG > BLE - se o usuario
     * desligou o radio, ele NAO volta sozinho no proximo boot (tambem
     * essencial pro teste de interferencia BLE x GPS: reiniciar nao pode
     * religar o anuncio escondido). ui_set_ble_radio_state sincroniza o
     * switch na tela com o estado real. */
    if (!settings_get_ble_radio()) {
        ble_telemetry_set_enabled(false);
        ui_set_ble_radio_state(false);
        ESP_LOGI(TAG, "Radio BLE desligado por preferencia salva");
    }

    ui_boot_set_status("Preparando WiFi...");
    wifi_export_init();
    wifi_export_set_password(settings_get_wifi_password());
    wifi_export_set_mode((wifi_export_mode_t)settings_get_wifi_mode());
    wifi_export_set_sta_credentials(settings_get_wifi_sta_ssid(), settings_get_wifi_sta_password());

    /* NAO chama usb_mode_init() aqui de proposito. Bug real encontrado:
     * tinyusb_driver_install() no boot deixa a interface MSC presente no
     * descriptor USB (CONFIG_TINYUSB_MSC_ENABLED=y) desde o arranque -
     * a porta USB-OTG nativa do P4 (a "segunda" porta, separada da
     * ponte serial usada pra flash) ja enumera como pen drive assim que
     * um cabo/PC e' conectado, MESMO sem o usuario nunca ter ativado o
     * modo pen drive no menu. O PC entao manda comandos SCSI (INQUIRY/
     * READ_CAPACITY) de cara, que caem num storage MSC ainda nao
     * registrado (isso so acontece dentro de usb_mode_enter()) - crash.
     * usb_mode_enter() ja instala o driver sozinho na hora (guarda
     * !s_driver_installed em usb_mode.c), entao a porta OTG so fica
     * "viva" pro host quando o usuario realmente pede modo pen drive. */

    /* Essas 4 chamadas nao adquirem lvgl_port_lock() sozinhas (ui.c nao
     * tem esse lock nelas - motivo historico: nada mais mexia na arvore
     * LVGL durante o boot). Isso deixou de ser verdade com o overlay de
     * loading (spinner anima o boot inteiro, taskLVGL fica redesenhando
     * em paralelo) - protege aqui com 1 lock so, do lado de fora, em vez
     * de mudar a assinatura/comportamento dessas 4 funcoes que tambem sao
     * chamadas de outros lugares (ex: apos evento de troca de modo). */
    if (lvgl_port_lock(0)) {
        ui_refresh_track_list();     /* popula dropdown da aba PISTA */
        ui_update_pista_status();    /* reflete finish + setores no boot */
        ui_set_mode_label(s_mode);
        ui_set_recording_state(false);
        lvgl_port_unlock();
    }

    /* Auto-sessao - aplica a preferencia salva (NVS) ao detector do GPS.
     * O switch na CONFIG > CORRIDA atualiza os dois lados em runtime. */
    gps_set_auto_session(settings_get_auto_session() != 0);

    /* Tudo carregado - mas garante ao menos BOOT_SCREEN_MIN_MS de tela de
     * loading antes de esconder (ver comentario la em cima). vTaskDelay
     * aqui e' seguro: ainda estamos fora do loop de eventos (nenhum toque
     * de usuario e' processado nesse ponto mesmo), e taskLVGL/o spinner
     * continuam rodando normalmente durante a espera (vTaskDelay so cede
     * a CPU, nao trava outras tasks). */
    int64_t boot_elapsed_ms = (esp_timer_get_time() - boot_start_us) / 1000;
    if (boot_elapsed_ms < BOOT_SCREEN_MIN_MS) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(BOOT_SCREEN_MIN_MS - boot_elapsed_ms)));
    }
#undef BOOT_SCREEN_MIN_MS

    /* Esconde a tela de loading e revela a UI de verdade (ja com
     * dropdown/status/modo corretos, sem o usuario ver nada mudando de
     * "vazio" pra "preenchido" na frente dele). */
    ui_boot_finish();

    /* Crash no uso anterior? Salva o coredump no SD agora que FS e UI
     * estao de pe (toast avisa; log tem o caminho do arquivo). */
    save_coredump_to_sd();

    ESP_LOGI(TAG, "kartbox v2 pronto");

    /* OTA rollback: chegar ate aqui = display + GPS + SD + BLE subiram,
     * firmware novo e' funcional - confirma o boot pro bootloader nao
     * voltar pra versao anterior no proximo reset. No-op se o rollback
     * nao estiver habilitado no sdkconfig ou se o boot nao veio de um
     * update pendente. */
    esp_ota_mark_app_valid_cancel_rollback();

    app_event_t evt;
    for (;;) {
        if (xQueueReceive(g_app_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            handle_event(&evt);
        }
    }
}
