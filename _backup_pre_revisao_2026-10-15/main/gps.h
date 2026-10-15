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

/* Estado de "saude" do link com o modulo GPS - distingue "sem fix ainda"
 * (normal, so esperando o ceu abrir) de "nem esta respondendo" (cabo
 * solto, modulo com defeito, baud rate errado etc), que a v1 nao
 * diferenciava. Ver gps_get_link_status(). */
typedef enum {
    GPS_LINK_ERROR,     /* sem nenhuma sentenca NMEA valida ha GPS_LINK_TIMEOUT_MS - hardware/fiacao */
    GPS_LINK_SEARCHING, /* UART comunicando normal, so ainda sem fix valido */
    GPS_LINK_FIXED,     /* fix valido, posicao confiavel */
} gps_link_status_t;

/* Numero de setores intermediarios suportados. 0 = sem setor (so linha de
 * chegada). Piloto pode marcar 0, 1 ou ate GPS_MAX_SECTORS pontos de setor
 * MANUAIS. Se nao marcar nenhum manual, o firmware gera GPS_MAX_SECTORS
 * setores AUTOMATICOS por distancia (1/(N+1), 2/(N+1)... da volta de
 * referencia) - ver s_auto_sectors_active em gps.c.
 * 2 setores (3 trechos) e o padrao de kart. NOTA: mudar esse valor muda o
 * tamanho de track_config_t (bumpar TRACK_MAGIC) e de settings
 * (SETTINGS_MAX_SECTORS deve acompanhar). */
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
    int32_t  last_delta_ms; /* delta da ULTIMA volta fechada vs o best anterior a ela - usado na lista de VOLTAS */

    /* Delta AO VIVO (tipo delta de carro de corrida) - compara a distancia
     * ja percorrida na volta atual contra o tempo que a MELHOR volta da
     * sessao levou pra percorrer essa mesma distancia, atualizado a cada
     * amostra de GPS. Negativo = ganhando tempo, positivo = perdendo.
     * live_delta_valid so vira true depois que existe pelo menos 1 volta
     * de referencia (a melhor) gravada nessa sessao. */
    int32_t  live_delta_ms;
    bool     live_delta_valid;

    /* Setores opcionais. sector_is_set[i] == false => setor i nao configurado.
     * sector_split_ms[i] == 0 => ainda nao cruzou o setor na volta atual.
     * best_sector_ms[i]  == 0 => sem referencia ainda nessa sessao. */
    bool     sector_is_set[GPS_MAX_SECTORS];
    uint32_t sector_split_ms[GPS_MAX_SECTORS];
    uint32_t best_sector_ms[GPS_MAX_SECTORS];

    /* Delta AO VIVO do setor: no instante do cruzamento, diferenca do
     * split contra o MELHOR split da sessao ANTES desse cruzamento
     * (negativo = novo recorde de setor). delta_valid[i] == false =
     * primeiro cruzamento da sessao (sem referencia) ou setor ainda nao
     * cruzado nessa volta. Zerado junto com sector_split_ms. */
    int32_t  sector_delta_ms[GPS_MAX_SECTORS];
    bool     sector_delta_valid[GPS_MAX_SECTORS];

    /* true => os setores ativos sao AUTOMATICOS (gerados por distancia,
     * sem marcacao manual). A UI usa isso pra rotular/contar (manual: ate
     * GPS_MAX_SECTORS marcados; auto: sempre GPS_MAX_SECTORS). */
    bool     sectors_auto;

    /* Volta ideal: soma dos melhores SEGMENTOS da sessao (largada->S1,
     * S1->S2, S2->chegada - nao confundir com best_sector_ms acima, que
     * e' cumulativo desde a largada). Pode combinar pedacos de voltas
     * diferentes - e' a definicao classica de "ideal lap" em telemetria.
     * 0 = ainda sem pelo menos 1 volta "limpa" completa nessa sessao. */
    uint32_t ideal_lap_ms;
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

/* DIAGNOSTICO: permite setar a fila do SD depois do gps_init(), pra poder
 * inicializar a UART do GPS ANTES de tentar montar o cartao SD (teste de
 * hipotese: falha no mount do SD pode estar deixando o dominio de clock
 * compartilhado num estado que trava o proximo uart_driver_install). */
void gps_set_log_queue(QueueHandle_t sd_log_queue);

/**
 * @brief Injeta dados de assistencia A-GPS (u-blox AssistNow, stream de
 * mensagens UBX-MGA-*) direto na UART do modulo. Reduz o cold start de
 * minutos pra segundos. Os bytes vem do servico online da u-blox -
 * baixados pelo proprio firmware (GET /agps_dl no wifi_export, modo
 * Cliente com internet) ou enviados pelo navegador (POST /agps).
 * Bloqueia ~15ms a cada 512 bytes (throttle pro buffer do modulo).
 *
 * @return true se tudo foi escrito na UART.
 */
bool gps_inject_agps(const uint8_t *data, size_t len);

/** @brief Marca a posicao+heading atuais como linha de chegada/saida.
 *  @return true se marcou (havia fix GPS valido), false se nao (sem fix -
 *  nada foi alterado). Chamador deve checar isso antes de dar feedback
 *  de sucesso pro piloto. */
bool gps_set_finish_line(void);

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

/* ------------------------------------------------------------------
 * Estatisticas da sessao - acumuladas a cada volta fechada, zeradas em
 * gps_session_reset(). Usadas pelo popup de resumo que aparece quando
 * o piloto encerra a sessao (ver ui_show_session_stats).
 * stddev_ms e' a "consistencia": quanto menor, mais regular o piloto.
 * ------------------------------------------------------------------ */
typedef struct {
    uint32_t lap_count;
    uint32_t best_ms;
    uint32_t avg_ms;
    uint32_t stddev_ms;
    float    max_speed_kmh; /* pico da sessao (so amostras com recording ativo) */
} gps_session_stats_t;

/** @brief Snapshot thread-safe das estatisticas acumuladas da sessao. */
void gps_get_session_stats(gps_session_stats_t *out);

/* ------------------------------------------------------------------
 * Auto-sessao - inicia/encerra a gravacao sozinho pela velocidade:
 * andando ha alguns segundos com pista configurada -> posta
 * APP_EVT_AUTO_SESSION_START; parado tempo demais com sessao ativa ->
 * APP_EVT_AUTO_SESSION_STOP. Quem de fato inicia/encerra e' o
 * dispatcher em main.c (mesmo caminho do botao RESET - o GPS so
 * detecta e avisa). Limiares em config.h (AUTO_SESSION_*).
 * ------------------------------------------------------------------ */
void gps_set_auto_session(bool enabled);

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
 *  @return true se marcou, false se sem fix GPS valido ou idx fora do range. */
bool gps_set_sector_point(int idx);

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

/* ------------------------------------------------------------------
 * Diagnostico de RF pra UI (card na CONFIG > SISTEMA) - mesmos numeros
 * do log periodico: satelites CONHECIDOS (in_view, sobe com almanaque/
 * A-GPS sem a antena melhorar) vs OUVINDO de verdade (tracked, com SNR
 * no GSV) por constelacao, + melhor SNR recente. Indices: 0=GPS 1=GLO
 * 2=GAL 3=BDS. Diagnostico de antena/EMI sem laptop plugado.
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t in_view[4];
    uint8_t tracked[4];
    uint8_t best_snr; /* dBHz - max desde o ultimo log periodico (janela de ate 15s) */
} gps_rf_diag_t;

void gps_get_rf_diag(gps_rf_diag_t *out);

/** @brief Estado de saude do link com o modulo GPS (ver gps_link_status_t).
 *  Pensado pra aba CONFIG sinalizar "modulo com problema" vs "so buscando
 *  sinal ainda", que sao causas bem diferentes de "sem fix" na tela. */
gps_link_status_t gps_get_link_status(void);

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
