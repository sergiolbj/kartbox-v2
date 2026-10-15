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
#define BTN_MODE_HOLD_MS    (1000)         /* segurar MODE = trocar QUALY/RACE (tap = ciclar layout da tela) */

/* ---------------------------------------------------------------------
 * GPS (UART NMEA)
 * --------------------------------------------------------------------- */
#define GPS_UART_NUM        (UART_NUM_1)
/* Fiacao fisica ficou reta (R do GPS -> GPIO51, T do GPS -> GPIO52) em vez
 * de cruzada - ja soldado, sem re-soldar. Corrigido aqui: dizemos pro P4
 * que o TX dele e' o pino 51 (onde o T do GPS de fato chega) e o RX e' o
 * 52 (onde o R do GPS de fato esta). GPIO matrix do P4 e' livre, entao
 * isso equivale a fiacao cruzada sem mexer no hardware. */
#define GPS_TX_PIN           (51)
#define GPS_RX_PIN           (52)
/* FlyFishRC M10 Mini (u-blox MAX-M10S) - datasheet recomenda 115200bps,
 * padrao de fabrica nesses modulos voltados pra FPV (update rate alto).
 * Lixo binario no log RAW com 9600 confirmou baud errado, nao wiring. */
#define GPS_BAUD_RATE        (115200)
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
/* editor de pista via celular (ver wifi_export.c) - servido em GET /editor.
 * Precisa ser copiado manualmente pra raiz do cartao (nao vai embutido no
 * firmware pra nao inchar a flash - e so um arquivo HTML autocontido). */
#define WIFI_EDITOR_HTML_PATH SD_MOUNT_POINT "/editor.html"

/* ---------------------------------------------------------------------
 * Timing / gate de largada-chegada
 * --------------------------------------------------------------------- */
#define GATE_RADIUS_M           (15.0f)
#define GATE_MAX_HEADING_DIFF   (45.0f)
#define MIN_LAP_TIME_MS          (10000)
#define RACE_START_SPEED_KMH     (5.0f)

/* Auto-sessao (ver gps_set_auto_session): inicia a gravacao sozinho
 * quando o kart anda sustentado com pista carregada; encerra sozinho
 * depois de muito tempo parado. Limiar de start com folga sobre o ruido
 * de GPS parado (~1-2 km/h de jitter e' normal em modulo de consumo);
 * limiar de stop mais baixo pra nao encerrar em fila de box lenta. */
#define AUTO_SESSION_START_KMH      (10.0f)
#define AUTO_SESSION_START_HOLD_MS  (3000)   /* 3s andando = comecou de verdade */
#define AUTO_SESSION_STOP_KMH       (3.0f)
#define AUTO_SESSION_STOP_HOLD_MS   (120000) /* 2min parado = sessao acabou */

/* Sem nenhum byte da UART do GPS por mais que isso = modulo desconectado
 * ou com defeito (nao e so "sem fix", e "nao esta nem respondendo"). */
#define GPS_LINK_TIMEOUT_MS      (2000)

/* ---------------------------------------------------------------------
 * Fuso horario - configuravel via NVS na v2 (aba CONFIG), isso aqui e
 * so o default de fabrica.
 * --------------------------------------------------------------------- */
#define DEFAULT_UTC_OFFSET_MIN    (-180)   /* UTC-3 (Brasilia) */

/* ---------------------------------------------------------------------
 * Protetor de tela (economia de bateria) - escurece e depois apaga o
 * backlight apos um tempo sem interacao (toque/botao) e com o kart parado.
 * Nunca dorme durante sessao de gravacao. Configuravel na CONFIG > SISTEMA;
 * aqui so os defaults de fabrica. Tempos em SEGUNDOS.
 *   - DIM_AFTER: escurece pro nivel reduzido
 *   - OFF_AFTER: apaga o backlight (0 = nunca apaga de vez, so escurece)
 *   - DIM_LEVEL: brilho (%) no estagio escurecido (fixo, nao exposto na UI)
 *   - MOVE_KMH:  acima disso o kart esta andando -> mantem a tela acesa
 * CUIDADO: com a tela apagada o consumo cai; se ficar abaixo de ~45mA o
 * IP5306 pode cortar o rail e desligar o aparelho (ver PRODUTO/historico).
 * --------------------------------------------------------------------- */
#define SCREENSAVER_ENABLED_DEFAULT   (1)
#define SCREENSAVER_DIM_AFTER_S       (60)     /* 1 min parado -> escurece */
#define SCREENSAVER_OFF_AFTER_S       (180)    /* 3 min parado -> apaga */
#define SCREENSAVER_DIM_LEVEL         (15)     /* % de brilho no estagio dim */
#define SCREENSAVER_MOVE_KMH          (5.0f)   /* andando acima disso = acordado */
/* Limites dos steppers da CONFIG (segundos). */
#define SCREENSAVER_DIM_MIN_S         (15)
#define SCREENSAVER_DIM_MAX_S         (600)
#define SCREENSAVER_DIM_STEP_S        (15)
#define SCREENSAVER_OFF_MIN_S         (0)       /* 0 = nunca apaga */
#define SCREENSAVER_OFF_MAX_S         (1800)
#define SCREENSAVER_OFF_STEP_S        (30)

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
/* Nome mDNS fixo, sem ".local" - funciona tanto no AP proprio quanto
 * (modo STA) conectado na rede do usuario, sem precisar saber o IP. */
#define WIFI_MDNS_HOSTNAME        "kartbox"
