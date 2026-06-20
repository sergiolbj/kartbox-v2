/*
 * gps.h - kartbox v2
 *
 * Decisoes de arquitetura em relacao a v1:
 *
 *  - Parsing sem malloc/strdup por linha (v1 fazia isso a 10Hz, risco
 *    de fragmentar heap numa sessao longa). Buffer estatico + strtok_r
 *    in-place.
 *  - Checksum NMEA validado antes de aceitar qualquer sentenca. Linha
 *    corrompida (comum com UART perto de motor/ignicao) e descartada
 *    em vez de gerar lat/lon furada.
 *  - Fuso horario configuravel em runtime (era hardcoded "h<3?h+21:h-3"
 *    na v1, e nao ajustava a DATA no rollover de meia-noite - bug
 *    latente). Agora e aritmetica de epoch de verdade via gps_set_utc_offset_min().
 *  - GPS nao manda amostra pra UI via fila. UI le o snapshot mais
 *    recente (gps_get_latest()) no proprio timer de refresh do LVGL -
 *    mais simples que fila pra um consumidor "ultimo valor vale".
 *    Quem PRECISA de toda amostra sem perder nenhuma (o logger de SD)
 *    recebe via fila dedicada passada em gps_init().
 *  - So o evento "volta completou" vai pra fila geral de eventos
 *    (app_events.h) - e o unico evento de GPS que e discreto/notavel o
 *    suficiente pra UI reagir (flash, beep) e nao so atualizar numero.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPS_MODE_QUALY,   /* timing comeca na hora que a linha e marcada */
    GPS_MODE_CORRIDA, /* timing so comeca quando o kart sai do parado (largada) */
} gps_race_mode_t;

/* Numero de setores intermediarios suportados. 0 = sem setor (so linha de
 * chegada). Piloto pode marcar 0, 1 ou ate GPS_MAX_SECTORS pontos de setor.
 * Adicionar mais e so aumentar essa constante. */
#define GPS_MAX_SECTORS 2

typedef struct {
    bool     fix_valid;
    uint8_t  satellites;
    double   lat;
    double   lon;
    float    speed_kmh;
    float    heading_deg;

    uint32_t lap_number;
    uint32_t lap_time_ms;   /* tempo decorrido da volta atual, sempre fresco */
    uint32_t best_lap_ms;   /* 0 = ainda sem melhor volta na sessao */
    int32_t  last_delta_ms; /* delta da ULTIMA volta fechada vs o best anterior a ela */

    /* Setores opcionais. sector_is_set[i] == false => setor i nao configurado.
     * sector_split_ms[i] == 0 => ainda nao cruzou o setor na volta atual.
     * best_sector_ms[i]  == 0 => sem referencia ainda nessa sessao. */
    bool     sector_is_set[GPS_MAX_SECTORS];
    uint32_t sector_split_ms[GPS_MAX_SECTORS];
    uint32_t best_sector_ms[GPS_MAX_SECTORS];
} gps_sample_t;

/**
 * @brief Amostra completa que vai pra fila do logger de SD a cada fix
 * processado (so quando ha sessao gravando - ver gps_session_start()).
 */
typedef struct {
    gps_sample_t sample;
    int64_t      timestamp_us; /* esp_timer_get_time() no momento do fix */
} gps_log_entry_t;

/**
 * @brief Inicia UART, task de leitura/parsing e logica de timing.
 *
 * @param sd_log_queue Fila (criada pelo modulo de SD) onde cada amostra
 * processada e publicada enquanto uma sessao estiver gravando. Pode ser
 * NULL se o logger de SD ainda nao estiver pronto (amostras so deixam
 * de ser enfileiradas, GPS continua funcionando normal).
 */
void gps_init(QueueHandle_t sd_log_queue);

/** @brief Marca a posicao+heading atuais como linha de chegada/saida. */
void gps_set_finish_line(void);

/** @brief Carrega linha de chegada a partir de coordenadas explicitas
 *  (ex: restore de pista salva no SD). Equivalente ao gps_load_sector()
 *  mas para o gate principal. No-op se coordenadas invalidas. */
void gps_load_finish_line(double lat, double lon, float heading_deg);

/** @brief Preenche lat/lon/heading da linha de chegada e retorna true
 *  se ela estiver configurada. Usado para montar track_config_t antes
 *  de salvar a pista. */
bool gps_get_finish_line(double *lat, double *lon, float *heading_deg);

/** @brief QUALY ou CORRIDA - muda a regra de quando o timing comeca. */
void gps_set_mode(gps_race_mode_t mode);

/** @brief Zera contadores de volta/melhor tempo pra uma sessao nova. */
void gps_session_reset(void);

/** @brief Liga/desliga o encaminhamento de amostras pra fila do SD. */
void gps_session_set_recording(bool recording);

/**
 * @brief Offset de fuso em minutos (ex: -180 = UTC-3). Persistido em
 * NVS pela aba de configuracoes; isso so atualiza o valor em uso.
 */
void gps_set_utc_offset_min(int16_t offset_min);

/** @brief Ajustaveis em runtime pela aba de configuracoes (settings.h). */
void gps_set_gate_radius_m(float meters);
void gps_set_min_lap_time_ms(uint32_t ms);

/* ------------------------------------------------------------------
 * API de setores intermediarios (opcionais).
 *
 * Fluxo tipico:
 *   1. No pista: piloto navega ate o ponto desejado e chama
 *      gps_set_sector_point(0) pra marcar o S1.
 *   2. Coordenadas sao lidas de volta com gps_get_sector_point() e
 *      persistidas em NVS pelo chamador (main.c usa settings.c).
 *   3. No proximo boot, main.c recarrega via gps_load_sector().
 *   4. Durante a sessao, process_gate_timing() detecta a passagem e
 *      atualiza sector_split_ms[] em gps_sample_t.
 *   5. Se o piloto nao quiser setor, simplesmente nao marca nada --
 *      sector_is_set[] fica false e todo o codigo de setor e no-op.
 * ------------------------------------------------------------------ */

/** @brief Marca a posicao+heading atuais como ponto do setor idx (0..GPS_MAX_SECTORS-1).
 *  No-op se sem fix GPS valido ou idx fora do range. */
void gps_set_sector_point(int idx);

/** @brief Carrega setor a partir de coordenadas salvas (ex: restore de NVS no boot). */
void gps_load_sector(int idx, double lat, double lon, float heading_deg);

/** @brief Remove o setor idx; zera split e best do setor nessa sessao. */
void gps_clear_sector_point(int idx);

/** @brief Preenche lat/lon/heading do setor idx e retorna true se estiver configurado. */
bool gps_get_sector_point(int idx, double *lat, double *lon, float *heading_deg);

/** @brief Retorna true se o setor idx esta configurado (sem lock prolongado). */
bool gps_sector_is_set(int idx);

/** @brief Snapshot thread-safe do estado mais recente. */
void gps_get_latest(gps_sample_t *out);

/**
 * @brief Hora local (UTC + offset configurado), ja com data corrigida
 * em caso de virada de meia-noite no rollover UTC->local - era um bug
 * latente na v1 (so ajustava a hora, nao o dia).
 *
 * @return false se ainda nao houve fix valido com data/hora pra derivar.
 */
bool gps_get_local_datetime(struct tm *out);

#ifdef __cplusplus
}
#endif
