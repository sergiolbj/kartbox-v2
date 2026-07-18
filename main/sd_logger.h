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
    float    avg_speed_kmh; /* media das amostras com velocidade > 1km/h na volta (v1 zerava amostras paradas do mesmo jeito) */
    /* Splits de setor CUMULATIVOS desde a largada da volta (mesmo
     * significado de sector_split_ms em gps_sample_t). 0 = setor nao
     * cruzado nessa volta OU CSV antigo (gravado antes das colunas
     * s1_ms/s2_ms existirem) - UI trata os dois casos como "sem split". */
    uint32_t sector_ms[GPS_MAX_SECTORS];
} sd_lap_summary_t;

#define SD_MAX_LAPS_LISTED (100)

/* Pontos do tracado apos decimacao - suficiente pra desenhar o formato de
 * qualquer pista de kart com boa fidelidade (curvas nao perdem definicao)
 * sem precisar de um buffer do tamanho da sessao inteira (pode ter
 * dezenas de milhares de amostras numa sessao longa a 10Hz). */
#define SD_MAX_TRACK_POINTS (800)

typedef struct {
    float    x_m;       /* posicao local em metros (leste), relativa ao 1o ponto valido da sessao */
    float    y_m;       /* posicao local em metros (norte), relativa ao 1o ponto valido da sessao */
    float    speed_kmh;
    uint32_t lap;       /* coluna lap do CSV - identifica a qual volta o ponto pertence (ghost do mapa) */
} sd_track_point_t;

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
 *
 * @param track_name Nome da pista carregada (ou NULL/"" se nenhuma) -
 * entra sanitizado como sufixo no nome do arquivo, pra ficar visivel na
 * lista/pagina de export. So [A-Za-z0-9_-], max 16 chars.
 */
bool sd_logger_start_session(const char *track_name);

/** @brief Fecha o arquivo da sessao atual com seguranca (fflush+fclose). */
void sd_logger_stop_session(void);

bool sd_logger_is_recording(void);

/** @brief true se o cartao esta montado agora (boot ou apos remount). */
bool sd_logger_is_mounted(void);

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

/**
 * @brief Le uma sessao e devolve o tracado (posicao local em metros,
 * projecao plana simples em torno do 1o ponto valido - suficiente pra
 * pista de kart, poucas centenas de metros) + velocidade por ponto,
 * decimado pra caber em max_points. Usado pelo "mapa da pista" na aba
 * VOLTAS.
 * @return numero de pontos (ate max_points), ou 0 se a sessao nao tiver
 * pelo menos 2 pontos com fix valido.
 */
int sd_read_session_track(const char *filename, sd_track_point_t *out, int max_points);

#ifdef __cplusplus
}
#endif
