/*
 * sd_logger.h - kartbox v2
 *
 * Corrige o bug mais caro da v1: la, sd_log_sample() era chamado direto
 * no loop principal a cada ~10ms, fazendo fwrite sincrono SEGURANDO o
 * lock do LVGL - cartao SD lento = tela inteira engasgando junto.
 *
 * Aqui: GPS posta cada amostra numa fila (sd_logger_get_queue(), usada
 * em gps_init()) e SO essa task mexe no arquivo. Main loop e GPS nunca
 * esperam o cartao.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOG_QUEUE_LEN     (32)
#define SD_MAX_SESSIONS_LISTED (64)
#define SD_SESSION_NAME_LEN   (40)

typedef struct {
    char filename[SD_SESSION_NAME_LEN];
} sd_session_entry_t;

typedef struct {
    uint32_t lap_number;
    uint32_t lap_time_ms;
    int32_t  delta_ms;
    float    max_speed_kmh; /* pico de velocidade registrado na volta */
} sd_lap_summary_t;

#define SD_MAX_LAPS_LISTED (100)

/**
 * @brief Liga alimentacao do slot, monta o cartao, cria a fila e a task
 * de escrita. Chame uma vez em app_main(), antes de gps_init() (que
 * precisa da fila retornada por sd_logger_get_queue()).
 *
 * @return false se o cartao nao montou (slot vazio, cartao corrompido,
 * etc) - kartbox continua funcionando sem gravar, so sem persistir.
 */
bool sd_logger_init(void);

/** @brief Fila pra passar em gps_init(sd_logger_get_queue()). */
QueueHandle_t sd_logger_get_queue(void);

/**
 * @brief Abre um arquivo novo de sessao (nome com data/hora local,
 * vinda de gps_get_local_datetime()) e comeca a aceitar amostras da
 * fila. Se nao houver fix de GPS ainda, usa um nome generico com
 * contador.
 */
bool sd_logger_start_session(void);

/** @brief Fecha o arquivo da sessao atual com seguranca (fflush+fclose). */
void sd_logger_stop_session(void);

bool sd_logger_is_recording(void);

/** @brief Espaco usado/livre do cartao montado, em bytes. */
bool sd_get_card_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

/**
 * @brief Desmonta o FS e libera o cartao por completo (fecha sessao
 * ativa com seguranca antes). Usado antes de expor o cartao via USB MSC
 * - so pode existir UM dono do barramento SDMMC por vez, firmware ou
 * host via USB, nunca os dois (era exatamente esse compartilhamento
 * indevido que a v1 nao tratava).
 */
void sd_logger_unmount(void);

/**
 * @brief Reinicializa o cartao e remonta o FS - chamado ao sair do
 * modo USB pra devolver o cartao ao firmware.
 * @return false se nao achou cartao no slot.
 */
bool sd_logger_remount(void);

/**
 * @brief Lista as sessoes gravadas, mais recente primeiro.
 * @return numero de sessoes encontradas (ate max_entries).
 */
int sd_list_sessions(sd_session_entry_t *out, int max_entries);

/** @brief Apaga todas as sessoes gravadas. Chamado so apos confirmacao na UI. */
void sd_delete_all_sessions(void);

/**
 * @brief Le um arquivo de sessao e extrai um resumo por volta (uma
 * linha por volta fechada, com o tempo e delta finais dela).
 * @param filename so o nome do arquivo (igual ao retornado por
 * sd_list_sessions), nao o path completo.
 * @return numero de voltas encontradas (ate max_laps).
 */
int sd_read_session_laps(const char *filename, sd_lap_summary_t *out, int max_laps);

#ifdef __cplusplus
}
#endif
