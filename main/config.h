/*
 * config.h - kartbox v2
 *
 * Pinagem confirmada contra o schematic oficial da GUITION JC4880P443C_I_W
 * e validada contra o demo funcional do fabricante. Ver display_init.h
 * pros detalhes de display/touch.
 *
 * v2: removido MPU6050/G-force - kart alugado, sem como fixar sensor no
 * chassi sessao a sessao (ver discussao no historico do projeto). Se um
 * dia for usar kart proprio, da pra reativar: o barramento I2C continua
 * exposto via display_get_i2c_bus() em display_init.h.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* ---------------------------------------------------------------------
 * Botoes fisicos
 * Wiring assumido: momentaneo pra GND, pull-up interno, ativo em LOW.
 * Confirme na bancada - se vier invertido e so trocar a logica em
 * app_events.c (um lugar so, nao em main.c E ui_kartbox.c como na v1).
 * --------------------------------------------------------------------- */
#define BTN_MODE_PIN        (GPIO_NUM_33)  /* alterna QUALY / CORRIDA */
#define BTN_SETLINE_PIN     (GPIO_NUM_31)  /* marca linha de chegada */
#define BTN_RESET_PIN       (GPIO_NUM_30)  /* inicia/encerra sessao */
#define BTN_DEBOUNCE_MS     (40)
#define BTN_RESET_HOLD_MS   (1500)         /* segurar pra confirmar reset */

/* ---------------------------------------------------------------------
 * GPS (UART NMEA)
 * --------------------------------------------------------------------- */
#define GPS_UART_NUM        (UART_NUM_1)
#define GPS_TX_PIN           (52)
#define GPS_RX_PIN           (51)
#define GPS_BAUD_RATE        (9600)
#define GPS_RX_BUF_SIZE       (1024)

/* ---------------------------------------------------------------------
 * Cartao SD (SDMMC nativo, 4-bit)
 * --------------------------------------------------------------------- */
#define SD_CLK_PIN           (43)
#define SD_CMD_PIN           (44)
#define SD_D0_PIN            (39)
#define SD_D1_PIN            (40)
#define SD_D2_PIN            (41)
#define SD_D3_PIN            (42)
#define SD_PWR_EN_PIN         (GPIO_NUM_45) /* liga VCC do slot via transistor */
#define SD_LDO_CHAN_ID        (4)
#define SD_LDO_VOLTAGE_MV     (3300)
#define SD_MOUNT_POINT        "/sdcard"
#define SD_SESSIONS_DIR       SD_MOUNT_POINT "/sessions"
#define SD_TRACKS_DIR         SD_MOUNT_POINT "/tracks"

/* ---------------------------------------------------------------------
 * Timing / gate de largada-chegada
 * --------------------------------------------------------------------- */
#define GATE_RADIUS_M           (15.0f)
#define GATE_MAX_HEADING_DIFF   (45.0f)
#define MIN_LAP_TIME_MS          (10000)
#define RACE_START_SPEED_KMH     (5.0f)

/* ---------------------------------------------------------------------
 * Fuso horario - configuravel via NVS na v2 (aba CONFIG), isso aqui e
 * so o default de fabrica.
 * --------------------------------------------------------------------- */
#define DEFAULT_UTC_OFFSET_MIN    (-180)   /* UTC-3 (Brasilia) */

/* ---------------------------------------------------------------------
 * BLE - telemetria ao vivo, sempre ligado (NimBLE, leve)
 * --------------------------------------------------------------------- */
#define BLE_DEVICE_NAME_DEFAULT   "KartBox"
#define BLE_TELEMETRY_HZ          (10)

/* ---------------------------------------------------------------------
 * WiFi - sob demanda, so quando usuario pede export (ver wifi_export.c)
 * --------------------------------------------------------------------- */
#define WIFI_AP_SSID_PREFIX       "KartBox-"
#define WIFI_AP_PASSWORD_DEFAULT  "kart2026race" /* trocavel via NVS quando a aba de config existir */
#define WIFI_AP_CHANNEL           (6)
#define WIFI_AP_MAX_CONN          (2)
#define WIFI_AP_IDLE_TIMEOUT_MS    (300000) /* desliga sozinho apos 5min parado */
