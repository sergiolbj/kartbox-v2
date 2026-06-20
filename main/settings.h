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
 * Pontos de setor (opcional) - persistidos pra sobreviver a reboot.
 *
 * Se is_set == false, o setor nao esta configurado (default de fabrica).
 * Gravado em NVS como blob; chave "sec0" e "sec1".
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

#ifdef __cplusplus
}
#endif
