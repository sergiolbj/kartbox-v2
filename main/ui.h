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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Constroi as 3 abas (Corrida/Voltas/Config) na tela ja iniciada. */
void ui_init(lv_display_t *disp);

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

/** @brief Reflete o estado do modo pen drive (USB) na aba Config. */
void ui_set_usb_mode_state(bool active);

/** @brief Carrega e mostra as voltas da sessao selecionada na aba Voltas. */
void ui_show_session_laps(uint32_t session_index);

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
 * @brief Mostra/esconde a barra de progresso de "segure pra encerrar sessao".
 *
 * @param start true  → exibe overlay e anima barra 0→100 em BTN_RESET_HOLD_MS.
 *              false → cancela animacao e esconde imediatamente.
 *
 * Pode ser chamado fora do contexto LVGL; adquire o lock internamente.
 */
void ui_show_hold_progress(bool start);

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
