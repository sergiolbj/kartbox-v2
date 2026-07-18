/*
 * ui.h - contrato minimo entre main.c e a tela.
 *
 * Implementacao completa (as 3 abas no estilo GT3 que desenhamos) vem
 * na proxima etapa. Esse header existe agora pra main.c compilar e o
 * dispatcher de eventos ja ficar com a forma final - quando ui.c for
 * escrito, so precisa bater com essas assinaturas.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "gps.h"
#include "track_manager.h"
#include "wifi_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Constroi as 3 abas (Corrida/Voltas/Config) na tela ja iniciada. */
void ui_init(lv_display_t *disp);

/**
 * @brief Atualiza o texto da tela de loading exibida no boot (overlay
 * cheio, ja visivel por padrao desde ui_init()). Chame entre as etapas de
 * inicializacao em app_main() pra dar feedback do que esta acontecendo
 * (ex: "Iniciando GPS...", "Montando cartao SD...").
 * Adquire lvgl_port_lock() internamente - PRECISA disso (diferente de
 * ui_refresh_track_list/ui_update_pista_status/ui_set_mode_label) porque
 * o spinner do overlay anima o boot inteiro, entao taskLVGL fica ativa
 * (redesenhando) durante toda a janela em que essa funcao e chamada pela
 * task "main" - sem lock da race real (visto em campo: hang em lv_inv_area).
 */
void ui_boot_set_status(const char *msg);

/** @brief Remove a tela de loading do boot (lv_obj_delete, nao so
 *  esconder - o spinner tem animacao propria que nao para sozinha ao
 *  ficar so invisivel), revelando a UI real por baixo. Chame por ultimo
 *  em app_main(), quando toda a inicializacao (GPS/SD/pistas/BLE/WiFi)
 *  tiver terminado. Adquire lvgl_port_lock() internamente (mesmo motivo
 *  de ui_boot_set_status). */
void ui_boot_finish(void);

/** @brief Atualiza o badge de modo (QUALY/CORRIDA) no topo da tela. */
void ui_set_mode_label(gps_race_mode_t mode);

/** @brief Reflete se ha sessao gravando (ex: indicador visual, trava de botoes de SD). */
void ui_set_recording_state(bool recording);

/** @brief Reacao visual a uma volta fechada (numero ja atualiza sozinho
 * via gps_get_latest() no timer de refresh; isso e so pro "flash" extra
 * tipo destacar a cor por um instante quando bate recorde). */
void ui_flash_lap_complete(uint32_t lap_number, uint32_t lap_time_ms,
                            int32_t delta_ms, bool is_new_best);

/** @brief Recarrega a lista de sessoes do SD na aba Voltas (ex: apos apagar tudo). */
void ui_refresh_session_list(void);

/**
 * @brief Reflete o estado do AP de export sob demanda na aba Config.
 * @param active false desliga o indicador; ssid/password podem ser NULL nesse caso.
 */
void ui_set_wifi_export_state(bool active, const char *ssid, const char *password);

/** @brief Reflete o modo wifi atual (AP proprio vs cliente) no switch
 *  e mostra/esconde o bloco de controles exclusivos do modo cliente
 *  (escanear/SSID/senha/conectar). Chame no boot e apos APP_EVT_WIFI_
 *  MODE_SET. */
void ui_set_wifi_mode_ui(wifi_export_mode_t mode);

/** @brief Popula o dropdown de redes encontradas na aba Config. Chame
 *  apos wifi_export_scan() retornar (count pode ser 0). */
void ui_set_wifi_scan_results(char ssids[][WIFI_SCAN_SSID_MAX], int count);

/** @brief Reflete o estado da conexao STA (desconectado/conectando/
 *  conectado com IP/falhou) no label de status da aba Config.
 *  @param ip Texto do IP quando WIFI_STA_STATE_CONNECTED, ignorado
 *  (pode ser NULL) nos demais estados. */
void ui_set_wifi_sta_state(wifi_sta_state_t state, const char *ip);

/** @brief Reflete o estado do modo pen drive (USB) na aba Config. */
void ui_set_usb_mode_state(bool active);

/** @brief Reflete o estado do toggle de BLE (switch da aba Config). */
void ui_set_ble_radio_state(bool enabled);

/**
 * @brief Sinaliza atividade do usuario pra acordar o protetor de tela
 * (restaura o brilho e zera o timer de inatividade). O toque ja e' contado
 * automaticamente (inatividade do LVGL); chame isto pros BOTOES FISICOS,
 * que nao passam pelo indev do LVGL. Seguro de qualquer contexto - adquire
 * o lock LVGL internamente. */
void ui_screensaver_notify_activity(void);

/** @brief Carrega e mostra as voltas da sessao selecionada na aba Voltas. */
void ui_show_session_laps(uint32_t session_index);

/** @brief Abre o overlay de mapa da pista com o tracado (colorido por
 *  velocidade) da sessao selecionada. Mostra um toast em vez de abrir se
 *  a sessao nao tiver dados de posicao suficientes. */
void ui_show_session_map(uint32_t session_index);

/**
 * @brief Mostra que a pista foi carregada com sucesso (toast com
 * countdown 3..2..1) e, ao zerar, troca automaticamente pra aba CORRIDA.
 * Chame apos aplicar a pista ao GPS com sucesso (APP_EVT_TRACK_LOAD).
 * Adquire lvgl_port_lock() internamente.
 */
void ui_show_track_loaded_countdown(const char *track_name);

/**
 * @brief Exibe um popup centralizado (toast) que some automaticamente.
 *
 * @param msg         Mensagem curta a exibir (ex: "Linha de chegada marcada").
 * @param duration_ms Tempo em ms antes de fechar sozinho (tipico: 2000-3000).
 *
 * Pode ser chamado fora do contexto LVGL (main.c dispatcher); adquire o
 * lock internamente.
 */
void ui_show_toast(const char *msg, uint32_t duration_ms);

/**
 * @brief Popup central com o resumo da sessao recem-encerrada (voltas,
 * best, media, consistencia, vel max). Fecha com toque ou sozinho apos
 * alguns segundos. Chamado pelo main.c dentro de stop_session();
 * adquire o lock internamente.
 */
void ui_show_session_stats(const gps_session_stats_t *stats);

/**
 * @brief Overlay opaco de tela inteira "ATUALIZANDO FIRMWARE - NAO
 * DESLIGUE" durante o OTA. Alem de avisar o piloto, cobre a UI inteira
 * com conteudo estatico (menos redesenho = menos disputa de banda com a
 * gravacao da flash = menos underrun/piscada no painel). show=false so
 * e' usado nos caminhos de erro - no sucesso o aparelho reinicia com o
 * overlay na tela. Adquire o lock internamente.
 */
void ui_show_ota_progress(bool show);

/**
 * @brief Cicla o layout da tela CORRIDA (completo -> delta gigante ->
 * velocidade gigante), salvando a preferencia do modo atual. Mesmo
 * efeito do toque na area central da tela; chamado pelo main.c no TAP
 * do botao fisico MODE. Adquire o lock internamente.
 */
void ui_cycle_race_layout(void);

/**
 * @brief Mostra/esconde a barra de progresso de "segure pra encerrar sessao".
 *
 * @param start true  → exibe overlay e anima barra 0→100 em BTN_RESET_HOLD_MS.
 *              false → cancela animacao e esconde imediatamente.
 *
 * Pode ser chamado fora do contexto LVGL; adquire o lock internamente.
 */
void ui_show_hold_progress(bool start);

/**
 * @brief Barra de progresso do hold do botao MODE (trocar QUALY/RACE).
 * Mesmo overlay do RESET, com carencia de 250ms pra nao piscar a cada
 * tap (que tambem passa por um aperto rapido). start=true no PRESSED,
 * false no tap/held. Adquire o lock internamente.
 */
void ui_show_mode_hold_progress(bool start);

/**
 * @brief Sincroniza UI com o estado atual dos setores (configurados ou nao).
 *
 * Chame apos marcar/limpar um setor (APP_EVT_SECTOR_MARK/CLEAR) e no
 * boot (depois de restaurar setores do NVS). Atualiza:
 *  - strip de splits S1/S2 na aba CORRIDA (visivel so quando ha setor)
 */
void ui_update_sector_status(void);

/* ------------------------------------------------------------------
 * Aba PISTA
 * ------------------------------------------------------------------ */

/**
 * @brief Atualiza os labels de status da aba PISTA (linha de chegada e
 * setores) com base no estado atual do GPS. Chame apos SET LINE,
 * sector mark/clear, ou load de pista.
 */
void ui_update_pista_status(void);

/**
 * @brief Popula o dropdown de pistas salvas na aba PISTA com a lista
 * atual do track_manager. Chame apos save, delete ou boot.
 */
void ui_refresh_track_list(void);

/**
 * @brief Chamado por main.c apos carregar uma pista com sucesso.
 * @param cfg Pista carregada (para preencher o campo de nome e status),
 *            ou NULL para "nova pista em branco".
 */
void ui_on_track_loaded(const track_config_t *cfg);

/**
 * @brief Mostra/esconde o painel de campos de edicao (NOME + CHEGADA/S1/
 * S2 + Salvar Pista) na aba PISTA. Comeca escondido - so aparece ao tocar
 * "Nova Pista" ou "Editar" (pista existente), e fecha sozinho apos salvar
 * com sucesso (APP_EVT_TRACK_SAVE). "Carregar" NAO abre esse painel (vai
 * direto pra CORRIDA, ver ui_show_track_loaded_countdown()).
 */
void ui_show_pista_edit_panel(bool show);

/**
 * @brief Coleta o rascunho de pista atual da aba PISTA (nome do campo
 * de texto + estado GPS de finish/setores) e preenche `out`.
 * @return false se o nome estiver vazio (nao deve salvar nesse caso).
 */
bool ui_get_track_draft(track_config_t *out);

/* ------------------------------------------------------------------
 * BLE Security (Parte B)
 * ------------------------------------------------------------------ */

/**
 * @brief Exibe o passkey SMP de 6 digitos no display.
 *
 * Chamado por ble_telemetry.c quando o NimBLE SMP solicita que o device
 * exiba o passkey para o usuario digitar no celular.
 * A tela deve mostrar o codigo ate a proxima conexao ou ate ser chamado
 * novamente (ex: popup centralizado, nao some automaticamente).
 *
 * @param passkey Valor de 0 a 999999 — exibir com zero-padding: "%06" PRIu32
 *
 * Pode ser chamado de qualquer contexto; adquire o lock LVGL internamente.
 */
void ui_show_ble_passkey(uint32_t passkey);

/** @brief Esconde o overlay de passkey (chame apos conexao estabelecida). */
void ui_hide_ble_passkey(void);

#ifdef __cplusplus
}
#endif
