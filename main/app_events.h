/*
 * app_events.h
 *
 * Fila de eventos central. ESSA e a correcao do bug mais feio achado na
 * v1: la, botao fisico (main.c) e botao na tela (ui_kartbox.c) tinham
 * cada um seu proprio caminho de codigo pra fazer "a mesma coisa", e em
 * algum momento isso divergiu - reset fisico chamava end_race_session()
 * direto no loop principal (sincrono, travando UI durante o save),
 * enquanto o botao da tela disparava uma task dedicada (assincrono,
 * sem travar). Mesma acao, dois comportamentos.
 *
 * Na v2: TUDO que pode disparar uma acao - GPIO fisico, toque na tela,
 * comando futuro via BLE - posta um app_event_t nessa fila. Existe UM
 * dispatcher (em main.c) que trata cada tipo de evento UMA vez. Fonte
 * do evento (campo `source`) so existe pra log/debug, nao pra ramificar
 * logica diferente.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVT_BTN_MODE,        /* alterna QUALY / CORRIDA */
    APP_EVT_BTN_SETLINE,     /* marca/confirma linha de chegada */
    APP_EVT_BTN_RESET,       /* inicia sessao nova, ou pede confirmacao de encerrar */
    APP_EVT_BTN_RESET_HELD,  /* reset confirmado (segurou o tempo minimo) */

    APP_EVT_USB_MODE_TOGGLE, /* botao de tela: entrar/sair do modo pen drive */
    APP_EVT_WIFI_EXPORT_TOGGLE, /* botao de tela: ligar/desligar AP de export sob demanda */
    APP_EVT_SD_DELETE_ALL,   /* botao de tela: apagar tudo do SD (com confirmacao) */
    APP_EVT_SESSION_SELECT,  /* usuario escolheu uma sessao no dropdown da aba Voltas */

    APP_EVT_GPS_SAMPLE,      /* nova amostra de GPS processada (timing+posicao) */
    APP_EVT_LAP_COMPLETE,    /* uma volta fechou - carrega tempo+delta prontos */

    APP_EVT_SD_WRITE_ERROR,  /* logger task sinalizando falha de escrita */
    APP_EVT_WIFI_TIMEOUT,    /* AP sob demanda atingiu timeout de inatividade */

    APP_EVT_SECTOR_MARK,     /* marcar setor na posicao atual (data.param = indice 0..1) */
    APP_EVT_SECTOR_CLEAR,    /* remover setor (data.param = indice 0..1) */

    APP_EVT_BTN_RESET_HOLD_ABORT, /* RESET solto antes de completar o hold - cancela barra de progresso */

    /* Aba PISTA - gestao de configs de pista no SD */
    APP_EVT_TRACK_LOAD,   /* carregar pista pelo nome (data.track_name) e aplicar ao GPS */
    APP_EVT_TRACK_SAVE,   /* salvar estado GPS atual como pista (nome vem de ui_get_track_draft) */
    APP_EVT_TRACK_DELETE, /* excluir pista pelo nome (data.track_name) */
    APP_EVT_TRACK_NEW,    /* limpar estado de pista atual (finish + setores) para nova config */
} app_event_type_t;

typedef enum {
    EVT_SRC_GPIO,
    EVT_SRC_TOUCH,
    EVT_SRC_BLE,
    EVT_SRC_INTERNAL,
} app_event_source_t;

typedef struct {
    float    speed_kmh;
    double   lat;
    double   lon;
    float    heading;
    uint8_t  satellites;
    uint32_t lap_number;
    uint32_t lap_time_ms;     /* tempo da volta atual em andamento */
    int32_t  delta_ms;        /* diferenca vs melhor volta, pode ser negativo */
} gps_sample_payload_t;

typedef struct {
    uint32_t lap_number;
    uint32_t lap_time_ms;
    int32_t  delta_ms;
    bool     is_new_best;
} lap_complete_payload_t;

typedef struct {
    app_event_type_t   type;
    app_event_source_t source;
    union {
        gps_sample_payload_t   gps;
        lap_complete_payload_t lap;
        uint32_t               session_index; /* p/ APP_EVT_SESSION_SELECT */
        uint32_t               param;         /* generico - p/ SECTOR_MARK/CLEAR: indice */
        char                   track_name[32]; /* p/ TRACK_LOAD / TRACK_DELETE */
    } data;
} app_event_t;

/* Fila global. Tamanho pensado pra nao perder evento de botao mesmo se
 * o dispatcher atrasar um pouco (ex: durante um flush de SD). */
#define APP_EVENT_QUEUE_LEN  (16)
extern QueueHandle_t g_app_event_queue;

/**
 * @brief Cria a fila de eventos e a task que poll os 3 botoes fisicos
 * com debounce, postando os eventos correspondentes.
 *
 * Chame uma vez, cedo, em app_main().
 */
void app_events_init(void);

/**
 * @brief Posta um evento na fila a partir de qualquer lugar (ex: callback
 * de toque do LVGL, ou futuro handler BLE). Nao bloqueia (timeout 0) -
 * eventos de UI nunca devem travar esperando espaco na fila.
 *
 * @return true se enfileirou, false se a fila estava cheia (raro;
 * logado como warning).
 */
bool app_event_post(app_event_type_t type, app_event_source_t source);

/**
 * @brief Variante que carrega payload (ex: GPS_SAMPLE, LAP_COMPLETE).
 */
bool app_event_post_data(const app_event_t *evt);

#ifdef __cplusplus
}
#endif
