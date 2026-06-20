/*
 * track_manager.c - kartbox v2
 *
 * Ver track_manager.h pro contexto.
 * Usa a POSIX file API que o ESP-IDF expoe sobre FATFS (fopen/fclose/
 * fread/fwrite). O acesso ao SD deve ser exclusivo - track_manager NAO
 * chama sd_logger_unmount(); caller (main.c) garante que nenhuma sessao
 * esta gravando antes de chamar save/delete.
 */
#include "track_manager.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#include "esp_log.h"

static const char *TAG = "track_mgr";

/* Indice em RAM das pistas encontradas no boot (ou apos save/delete). */
static char s_names[TRACK_LIST_MAX][TRACK_NAME_MAX];
static int  s_count = 0;

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */
static void build_path(char *buf, size_t len, const char *name)
{
    snprintf(buf, len, "%s/%s.trk", SD_TRACKS_DIR, name);
}

/* Reescaneia o diretorio e reconstroi o indice em RAM. */
static void rescan(void)
{
    s_count = 0;
    DIR *d = opendir(SD_TRACKS_DIR);
    if (!d) {
        ESP_LOGW(TAG, "Nao foi possivel abrir %s para listagem", SD_TRACKS_DIR);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_count < TRACK_LIST_MAX) {
        /* Aceita apenas arquivos .trk */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) continue;
        if (strcmp(ent->d_name + nlen - 4, ".trk") != 0) continue;
        /* Copia nome sem a extensao */
        size_t copy = nlen - 4;
        if (copy >= TRACK_NAME_MAX) copy = TRACK_NAME_MAX - 1;
        memcpy(s_names[s_count], ent->d_name, copy);
        s_names[s_count][copy] = '\0';
        s_count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "%d pista(s) encontrada(s) em %s", s_count, SD_TRACKS_DIR);
}

/* ------------------------------------------------------------------
 * API publica
 * ------------------------------------------------------------------ */
void track_manager_init(void)
{
    /* Garante que o diretorio existe */
    int rc = mkdir(SD_TRACKS_DIR, 0755);
    if (rc != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Nao foi possivel criar %s (errno %d)", SD_TRACKS_DIR, errno);
    }
    rescan();
}

bool track_manager_save(const track_config_t *cfg)
{
    if (!cfg || cfg->name[0] == '\0') {
        ESP_LOGW(TAG, "save: nome de pista vazio - ignorado");
        return false;
    }
    char path[128];
    build_path(path, sizeof(path), cfg->name);

    track_config_t to_write = *cfg;
    to_write.magic = TRACK_MAGIC;

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "save: nao foi possivel abrir %s para escrita", path);
        return false;
    }
    bool ok = (fwrite(&to_write, sizeof(to_write), 1, f) == 1);
    fflush(f);
    fclose(f);

    if (ok) {
        ESP_LOGI(TAG, "Pista \"%s\" salva em %s", cfg->name, path);
        rescan();
    } else {
        ESP_LOGE(TAG, "Falha ao escrever %s", path);
    }
    return ok;
}

bool track_manager_load(const char *name, track_config_t *out)
{
    if (!name || !out) return false;
    char path[128];
    build_path(path, sizeof(path), name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "load: arquivo nao encontrado: %s", path);
        return false;
    }
    bool ok = (fread(out, sizeof(*out), 1, f) == 1);
    fclose(f);

    if (!ok) {
        ESP_LOGE(TAG, "load: falha ao ler %s", path);
        return false;
    }
    if (out->magic != TRACK_MAGIC) {
        ESP_LOGW(TAG, "load: magic invalido em %s (0x%08lX) - arquivo corrompido ou versao diferente",
                 path, (unsigned long)out->magic);
        return false;
    }
    /* Garante null-termination defensiva */
    out->name[TRACK_NAME_MAX - 1] = '\0';
    ESP_LOGI(TAG, "Pista \"%s\" carregada de %s", out->name, path);
    return true;
}

bool track_manager_delete(const char *name)
{
    if (!name) return false;
    char path[128];
    build_path(path, sizeof(path), name);

    if (remove(path) != 0) {
        ESP_LOGW(TAG, "delete: falha ao remover %s (errno %d)", path, errno);
        return false;
    }
    ESP_LOGI(TAG, "Pista \"%s\" removida", name);
    rescan();
    return true;
}

int track_manager_list(char out_names[][TRACK_NAME_MAX], int max_count)
{
    int n = (s_count < max_count) ? s_count : max_count;
    for (int i = 0; i < n; i++) {
        strncpy(out_names[i], s_names[i], TRACK_NAME_MAX - 1);
        out_names[i][TRACK_NAME_MAX - 1] = '\0';
    }
    return n;
}

int track_manager_count(void)
{
    return s_count;
}
