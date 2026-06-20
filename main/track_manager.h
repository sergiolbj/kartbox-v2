/*
 * track_manager.h - kartbox v2
 *
 * Gerencia configs de pista persistidas no SD como arquivos binarios .trk.
 * Cada pista tem: nome, linha de chegada, ate GPS_MAX_SECTORS pontos de setor.
 *
 * Fluxo tipico:
 *   Boot        : track_manager_init() escaneia /sdcard/tracks/
 *   Aba PISTA   : track_manager_list() preenche o dropdown de pistas salvas
 *   Carregar    : track_manager_load() le o .trk → main.c aplica ao GPS
 *   Salvar      : main.c monta track_config_t do estado atual → track_manager_save()
 *   Excluir     : track_manager_delete() remove o arquivo, UI refaz a lista
 *
 * Formato do arquivo: struct track_config_t direta (binario, sem JSON parser).
 * Pequeno (~96 bytes), leitura instantanea, zero dependencia externa.
 * O campo `magic` detecta arquivos corrompidos ou de versao incompativel.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "gps.h"   /* GPS_MAX_SECTORS */

#ifdef __cplusplus
extern "C" {
#endif

#define TRACK_MAGIC          (0x4B545231UL)  /* "KTR1" - bumpar se mudar a struct */
#define TRACK_NAME_MAX       (32)
#define TRACK_LIST_MAX       (32)            /* max de pistas indexadas em memoria */

typedef struct {
    double lat;
    double lon;
    float  heading_deg;
    bool   is_set;
} track_point_t;

typedef struct {
    uint32_t      magic;                         /* TRACK_MAGIC - validacao na leitura */
    char          name[TRACK_NAME_MAX];          /* nome definido pelo piloto, null-terminated */
    track_point_t finish;
    track_point_t sectors[GPS_MAX_SECTORS];
} track_config_t;

/**
 * @brief Escaneia SD_TRACKS_DIR e indexa os nomes das pistas encontradas.
 * Chame uma vez em app_main() apos sd_logger_init().
 * Se o diretorio nao existir, cria automaticamente.
 */
void track_manager_init(void);

/**
 * @brief Salva (cria ou substitui) uma pista no SD.
 * O nome da pista (cfg->name) e usado como nome de arquivo: "<name>.trk".
 * @return true em sucesso.
 */
bool track_manager_save(const track_config_t *cfg);

/**
 * @brief Carrega uma pista pelo nome (sem extensao).
 * @return true se o arquivo existiu e o magic bateu.
 */
bool track_manager_load(const char *name, track_config_t *out);

/**
 * @brief Remove o arquivo da pista e atualiza o indice em memoria.
 * @return true se apagou com sucesso.
 */
bool track_manager_delete(const char *name);

/**
 * @brief Preenche `out_names` com os nomes das pistas indexadas.
 * @param out_names  Array de strings, cada uma com TRACK_NAME_MAX bytes.
 * @param max_count  Tamanho maximo do array.
 * @return Numero real de pistas copiadas (0 se nenhuma).
 */
int track_manager_list(char out_names[][TRACK_NAME_MAX], int max_count);

/** @brief Numero de pistas indexadas em memoria. */
int track_manager_count(void);

#ifdef __cplusplus
}
#endif
