/*
 * settings.h - kartbox v2
 *
 * Antes esses valores eram #define fixo em config.h - mudar exigia
 * recompilar e reflashar. Agora moram em NVS, ajustaveis na aba
 * Config, sobrevivem a reset/queda de energia. config.h continua
 * tendo os defaults de fabrica (usados na primeira vez que liga, ou se
 * a NVS estiver corrompida/vazia).
 *
 * Getters retornam valor em cache (RAM), nunca tocam a flash no hot
 * path. Setters gravam na NVS imediatamente (gravacao e rara - usuario
 * mexendo na tela de config - custo de latencia ai nao importa).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Abre a NVS, carrega valores salvos (ou aplica default de
 * config.h se for a primeira vez). Chame uma vez em app_main(), logo
 * apos nvs_flash_init(). */
void settings_init(void);

int16_t settings_get_utc_offset_min(void);
void    settings_set_utc_offset_min(int16_t value);

float settings_get_gate_radius_m(void);
void  settings_set_gate_radius_m(float value);

uint32_t settings_get_min_lap_time_ms(void);
void     settings_set_min_lap_time_ms(uint32_t value);

const char *settings_get_ble_name(void);
void        settings_set_ble_name(const char *name);

const char *settings_get_wifi_password(void);
void        settings_set_wifi_password(const char *password);

/* ------------------------------------------------------------------
 * Modo wifi (AP proprio vs cliente conectado na rede do usuario) e
 * credenciais da rede alvo quando em modo cliente. Ver wifi_export.h
 * pra API que de fato usa isso (settings so guarda os bytes).
 * 0 = AP (default, sempre funciona sem depender de rede externa).
 * 1 = STA (cliente).
 * ------------------------------------------------------------------ */
#define SETTINGS_WIFI_STA_SSID_MAX (33)  /* 32 + \0, limite 802.11 */
#define SETTINGS_WIFI_STA_PASS_MAX (64)

uint8_t settings_get_wifi_mode(void);
void    settings_set_wifi_mode(uint8_t mode);

const char *settings_get_wifi_sta_ssid(void);
void        settings_set_wifi_sta_ssid(const char *ssid);

const char *settings_get_wifi_sta_password(void);
void        settings_set_wifi_sta_password(const char *password);

/* ------------------------------------------------------------------
 * Pontos de setor (opcional) - persistidos pra sobreviver a reboot.
 *
 * Se is_set == false, o setor nao esta configurado (default de fabrica).
 * Gravado em NVS como blob; chaves "sec0".."sec<N-1>".
 * Deve acompanhar GPS_MAX_SECTORS (setores MANUAIS persistidos).
 * ------------------------------------------------------------------ */
#define SETTINGS_MAX_SECTORS 2

typedef struct {
    double lat;
    double lon;
    float  heading_deg;
    bool   is_set;
} settings_sector_t;

settings_sector_t settings_get_sector(int idx);
void settings_set_sector(int idx, double lat, double lon, float heading_deg);
void settings_clear_sector(int idx);

/* ------------------------------------------------------------------
 * Ultima pista usada - persiste em NVS pra auto-carregar no boot.
 * Retorna string vazia ("") se nenhuma pista foi carregada ainda.
 * Chave NVS: "last_track".
 * ------------------------------------------------------------------ */
#define SETTINGS_TRACK_NAME_MAX (32)

const char *settings_get_last_track(void);
void        settings_set_last_track(const char *name);

/* ------------------------------------------------------------------
 * PIN de autenticacao BLE (Parte A da seguranca dupla).
 *
 * String de ate 8 caracteres numericos/alfanumericos.
 * Se vazio (""), autenticacao por PIN esta desativada e qualquer
 * client que completar o pairing SMP pode usar os comandos.
 * Chave NVS: "ble_pin".
 * ------------------------------------------------------------------ */
#define SETTINGS_BLE_PIN_MAX (8)

const char *settings_get_ble_pin(void);
void        settings_set_ble_pin(const char *pin);

/* ------------------------------------------------------------------
 * Tema de cor da UI - troca a cor "de marca" (botoes, acentos de
 * celula, aba selecionada, spinner de boot, etc). Indicadores
 * SEMANTICOS nao mudam de proposito: delta ganho/perda continua
 * verde/vermelho, alertas continuam vermelho/dourado - pra nenhum
 * tema mascarar aviso (ver paleta em ui.c).
 * 0=Verde (default) 1=Azul 2=Ambar 3=Laranja 4=Roxo.
 * Chave NVS: "theme". Aplicado na construcao da UI (reinicio).
 * ------------------------------------------------------------------ */
#define SETTINGS_THEME_COUNT (5)

uint8_t settings_get_theme(void);
void    settings_set_theme(uint8_t theme);

/* ------------------------------------------------------------------
 * Auto-sessao - inicia/encerra a gravacao sozinho pela velocidade
 * (ver gps_set_auto_session e AUTO_SESSION_* em config.h).
 * 0 = desligado, 1 = ligado (default de fabrica: ligado).
 * Chave NVS: "auto_sess".
 * ------------------------------------------------------------------ */
uint8_t settings_get_auto_session(void);
void    settings_set_auto_session(uint8_t enabled);

/* ------------------------------------------------------------------
 * Token do servico u-blox AssistNow (A-GPS). Gratis via Thingstream
 * (thingstream.io > Location Services > AssistNow). Vazio = nunca
 * configurado. Informado uma vez na pagina do WiFi export e salvo aqui
 * pros proximos downloads. Chave NVS: "agps_token".
 * ------------------------------------------------------------------ */
#define SETTINGS_AGPS_TOKEN_MAX (64)

const char *settings_get_agps_token(void);
void        settings_set_agps_token(const char *token);

/* ------------------------------------------------------------------
 * Radio BLE ligado/desligado - persiste o switch da CONFIG > BLE.
 * 0 = anuncio BLE nao sobe no boot (o link SDIO com o C6 continua, e'
 * infra do WiFi remoto; so o TX/anuncio BLE fica quieto).
 * Default 1 (comportamento historico). Chave NVS: "ble_radio".
 * ------------------------------------------------------------------ */
uint8_t settings_get_ble_radio(void);
void    settings_set_ble_radio(uint8_t enabled);

/* ------------------------------------------------------------------
 * Brilho do display em % (20..100 - minimo de 20 de proposito: brilho 0
 * = tela apagada sem jeito obvio de recuperar no touch). Aplicado no
 * boot e ao vivo pelo stepper da CONFIG > SISTEMA. Chave NVS: "bright".
 * ------------------------------------------------------------------ */
uint8_t settings_get_brightness(void);
void    settings_set_brightness(uint8_t percent);

/* ------------------------------------------------------------------
 * Protetor de tela (economia de bateria) - ver SCREENSAVER_* em config.h.
 * Tempos em SEGUNDOS. off=0 significa "nunca apaga de vez, so escurece".
 * Chaves NVS: "scr_en", "scr_dim", "scr_off".
 * ------------------------------------------------------------------ */
uint8_t  settings_get_screensaver_enabled(void);
void     settings_set_screensaver_enabled(uint8_t enabled);

uint16_t settings_get_screensaver_dim_s(void);
void     settings_set_screensaver_dim_s(uint16_t seconds);

uint16_t settings_get_screensaver_off_s(void);
void     settings_set_screensaver_off_s(uint16_t seconds);

/* ------------------------------------------------------------------
 * Layout da tela CORRIDA, POR MODO (0=QUALY, 1=RACE):
 *   0 = completo (celulas + velocimetro + strip)
 *   1 = foco no DELTA (numero gigante)
 *   2 = foco na VELOCIDADE (numero gigante)
 * Trocar de modo recarrega o layout preferido daquele modo - qualy e
 * corrida podem ter telas diferentes. Chaves NVS: "lay_q"/"lay_r".
 * ------------------------------------------------------------------ */
uint8_t settings_get_mode_layout(uint8_t race_mode);
void    settings_set_mode_layout(uint8_t race_mode, uint8_t layout);

/* ------------------------------------------------------------------
 * Escala da barra de LED do delta: magnitude (ms) que acende a barra
 * INTEIRA de um lado. Cada segmento representa escala/8. Default 1500ms
 * (~187ms/LED); pista curta/piloto fino pode apertar pra 500ms (~62ms/
 * LED), pista longa relaxar ate 3000ms. Chave NVS: "led_scale".
 * ------------------------------------------------------------------ */
uint16_t settings_get_led_scale_ms(void);
void     settings_set_led_scale_ms(uint16_t ms);

/* ------------------------------------------------------------------
 * Backup/restore de TODAS as configuracoes num arquivo texto key=value
 * (tipicamente no SD - o caller passa o path). Restore aplica via os
 * proprios setters (RAM + NVS na hora); consumidores de runtime (gps,
 * widgets ja construidos) so pegam tudo no proximo boot - a UI avisa
 * "reinicie". Linhas/keys desconhecidas sao ignoradas (forward compat).
 * ------------------------------------------------------------------ */
bool settings_backup_to_file(const char *path);
bool settings_restore_from_file(const char *path);

#ifdef __cplusplus
}
#endif
