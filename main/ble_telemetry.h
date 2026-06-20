/*
 * ble_telemetry.h - kartbox v2
 *
 * BLE sempre ligado (NimBLE, pegada de RAM bem menor que Bluedroid).
 * GATT custom com 1 characteristic de notify, ~10Hz (BLE_TELEMETRY_HZ
 * em config.h), pacote binario compacto - sem JSON, sem overhead de
 * parsing do lado do celular/app.
 *
 * IMPORTANTE pra essa placa especifica: P4 nao tem radio BT proprio,
 * sai pelo ESP32-C6 via esp-hosted (mesmo link usado pro WiFi sob
 * demanda). sdkconfig precisa de CONFIG_BT_ENABLED=y e
 * CONFIG_BT_NIMBLE_ENABLED=y com o componente esp_hosted/esp_wifi_remote
 * ja presente (o mesmo que resolveu o crash do WiFi na v1 - ver
 * historico do projeto). Sem isso o host NimBLE sobe mas nunca sincroniza
 * com o controller (ble_on_sync nunca dispara).
 *
 * Formato do pacote de telemetria (28 bytes, little-endian, ver
 * ble_telemetry_packet_t abaixo) - documentado aqui porque e
 * efetivamente um contrato de fio pra quem for escrever o app/central
 * que conecta nisso.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint32_t lap_time_ms;     /* tempo decorrido da volta atual */
    int32_t  delta_ms;        /* delta da ultima volta fechada vs best anterior */
    uint32_t best_lap_ms;     /* 0 = ainda sem melhor volta na sessao */
    uint16_t speed_kmh_x10;   /* km/h * 10 (873 = 87.3 km/h) - evita float no ar */
    uint16_t heading_x10;     /* graus * 10 */
    uint16_t lap_number;
    uint8_t  satellites;
    uint8_t  fix_valid;       /* 0/1 */
    int32_t  lat_x1e6;        /* graus * 1e6 */
    int32_t  lon_x1e6;        /* graus * 1e6 */
} ble_telemetry_packet_t;

/**
 * @brief Inicia o stack NimBLE, sobe o GATT de telemetria e comeca a
 * advertise. Chame uma vez em app_main(), depois de gps_init() (a task
 * de telemetria ja comeca lendo gps_get_latest()).
 */
void ble_telemetry_init(void);

/**
 * @brief Troca o nome anunciado via BLE (persistido em NVS pela aba de
 * configuracoes, isso so aplica o valor em uso e reinicia o advertise).
 */
void ble_telemetry_set_device_name(const char *name);

/** @brief true se ha uma central conectada agora (pro indicador na tela). */
bool ble_telemetry_is_connected(void);

#ifdef __cplusplus
}
#endif
