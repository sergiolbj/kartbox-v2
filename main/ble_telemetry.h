/*
 * ble_telemetry.h - kartbox v2
 *
 * BLE bidirecional: telemetria (notify) + controle remoto (write/notify).
 *
 * ARQUITETURA
 * -----------
 * CHR 0001 — NOTIFY  : telemetria ao vivo (ble_telemetry_packet_t, 28 bytes, 10Hz)
 * CHR 0003 — WRITE   : comandos do celular → device (ble_cmd_packet_t)
 * CHR 0004 — NOTIFY  : respostas device → celular (ble_rsp_packet_t)
 *
 * Comandos correm numa FreeRTOS task separada pra nao bloquear o host
 * NimBLE. A write callback apenas posta na fila; a task processa SD/NVS
 * e envia as respostas via notify.
 *
 * PROTOCOLO
 * ---------
 * CMD: [cmd:u8][seq:u8][payload...]
 * RSP: [cmd:u8][seq:u8][status:u8][payload...]
 *
 * status:
 *   BLE_RSP_OK   (0x00) — unica resposta ou ultimo chunk
 *   BLE_RSP_ERR  (0x01) — erro, payload e mensagem de texto opcional
 *   BLE_RSP_MORE (0x02) — mais chunks virao (acumular no celular)
 *
 * SEGURANCA (dupla camada)
 * -----------------------
 * Parte B — BLE SMP pairing com passkey de 6 digitos exibido no display.
 *   IO capability: DISP_ONLY. Bonding habilitado (vincula uma vez, nao pede
 *   passkey novamente para devices ja vinculados).
 *   MITM + Secure Connections — conexao e criptografada.
 *
 * Parte A — PIN de aplicacao por sessao (verificado apos o pairing).
 *   O comando 0x00 AUTH deve ser enviado antes de qualquer outro.
 *   Sem AUTH valido, todos os comandos retornam BLE_RSP_ERR "nao autenticado".
 *   Telemetria notify (CHR 0001) permanece publica — e so leitura.
 *   PIN vazio em NVS desativa a verificacao de PIN (SMP ainda exige pairing).
 *
 * COMANDOS
 * --------
 * 0x00 AUTH              payload: pin(str, ate 8 chars)    ret: OK / ERR
 * 0x01 GET_SETTINGS      payload: —                ret: ble_settings_payload_t
 * 0x02 SET_SETTINGS      payload: ble_settings_payload_t  ret: OK/ERR
 * 0x03 LIST_TRACKS       payload: —                ret: nomes \n-separados (chunked se >BLE_CHUNK_SIZE)
 * 0x04 GET_TRACK         payload: nome(str)         ret: ble_track_payload_t
 * 0x05 PUT_TRACK         payload: ble_track_payload_t     ret: OK/ERR
 * 0x06 DEL_TRACK         payload: nome(str)         ret: OK/ERR
 * 0x10 LIST_SESSIONS     payload: —                ret: nomes \n-separados (chunked)
 * 0x11 GET_SESSION       payload: nome(str)         ret: conteudo CSV raw (chunked)
 * 0x12 DEL_ALL_SESSIONS  payload: —                ret: OK/ERR
 *
 * UUIDs (little-endian no BLE_UUID128_INIT, string = bytes invertidos)
 *   Service  : a1b2c3d4-0001-4a5b-8c9d-0123456789ab
 *   CHR telem: a1b2c3d4-0001-4a5b-8c9d-0123456789ab  (notify)
 *   CHR cmd  : a1b2c3d4-0003-4a5b-8c9d-0123456789ab  (write)
 *   CHR rsp  : a1b2c3d4-0004-4a5b-8c9d-0123456789ab  (notify)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Telemetria (CHR 0001, notify) ────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t lap_time_ms;      /* tempo decorrido da volta atual */
    int32_t  delta_ms;         /* delta da ultima volta vs best */
    uint32_t best_lap_ms;      /* 0 = sem melhor volta na sessao */
    uint16_t speed_kmh_x10;    /* km/h * 10 (873 = 87.3 km/h) */
    uint16_t heading_x10;      /* graus * 10 */
    uint16_t lap_number;
    uint8_t  satellites;
    uint8_t  fix_valid;        /* 0/1 */
    int32_t  lat_x1e6;         /* graus * 1e6 */
    int32_t  lon_x1e6;         /* graus * 1e6 */
} ble_telemetry_packet_t;      /* 28 bytes */

/* ── Protocolo de controle (CHR 0003 write / CHR 0004 notify) ─────────── */

#define BLE_CMD_AUTH              0x00
#define BLE_CMD_GET_SETTINGS      0x01
#define BLE_CMD_SET_SETTINGS      0x02
#define BLE_CMD_LIST_TRACKS       0x03
#define BLE_CMD_GET_TRACK         0x04
#define BLE_CMD_PUT_TRACK         0x05
#define BLE_CMD_DEL_TRACK         0x06
#define BLE_CMD_LIST_SESSIONS     0x10
#define BLE_CMD_GET_SESSION       0x11
#define BLE_CMD_DEL_ALL_SESSIONS  0x12

#define BLE_RSP_OK    0x00
#define BLE_RSP_ERR   0x01
#define BLE_RSP_MORE  0x02

/* Tamanho maximo de chunk por notify (MTU 512 - 3 ATT header - 3 RSP header) */
#define BLE_CHUNK_SIZE  480

/* Payload de settings — versao packed pra transmissao BLE */
typedef struct __attribute__((packed)) {
    int16_t  utc_offset_min;
    float    gate_radius_m;
    uint32_t min_lap_ms;
    char     ble_name[32];
    char     wifi_pass[32];
    char     ble_pin[8];       /* PIN de autenticacao (Parte A); vazio = PIN desativado */
} ble_settings_payload_t;      /* 82 bytes */

/* Payload de pista — versao packed, evita ambiguidade de padding entre
 * compiladores (track_config_t usa bool que pode ser 1 ou 4 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    char     name[32];
    double   finish_lat;
    double   finish_lon;
    float    finish_heading;
    uint8_t  finish_is_set;
    double   s0_lat;
    double   s0_lon;
    float    s0_heading;
    uint8_t  s0_is_set;
    double   s1_lat;
    double   s1_lon;
    float    s1_heading;
    uint8_t  s1_is_set;
} ble_track_payload_t;         /* 99 bytes */

/* ── API publica ──────────────────────────────────────────────────────── */

/**
 * @brief Inicia NimBLE, sobe GATT (telemetria + controle) e começa a
 * anunciar. Chame uma vez em app_main() apos gps_init().
 */
void ble_telemetry_init(void);

/**
 * @brief Troca o nome anunciado via BLE (persistido em NVS).
 */
void ble_telemetry_set_device_name(const char *name);

/** @brief true se ha uma central conectada. */
bool ble_telemetry_is_connected(void);

/**
 * @brief Liga/desliga o anuncio (advertising) e aceite de nova conexao
 * BLE em runtime - reduz o trafego de RF ativo do NimBLE (util p.ex.
 * pra testar se o radio BLE atrapalha a recepcao do GPS). Desconecta a
 * central atual se estiver indo de true->false.
 *
 * IMPORTANTE: isso NAO desliga o link SDIO com o co-processador C6, que
 * continua ligado por ser infraestrutura compartilhada com o WiFi
 * remoto - so para a transmissao/anuncio BLE em si, nao e' "radio
 * desligado" no nivel de hardware.
 */
void ble_telemetry_set_enabled(bool enabled);

/** @brief Estado atual do toggle acima (default: true, ligado). */
bool ble_telemetry_is_enabled(void);

/**
 * @brief true depois que o host NimBLE sincronizou com o controller no
 * C6 (via esp_hosted). Continuar false apos o boot = link SDIO/C6 com
 * problema - consumido pelo autoteste da CONFIG > SISTEMA.
 */
bool ble_telemetry_host_synced(void);

#ifdef __cplusplus
}
#endif
