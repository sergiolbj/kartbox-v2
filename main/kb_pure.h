/*
 * kb_pure.h - kartbox v2
 *
 * Funcoes "puras": so recebem valores e devolvem valores. Nao usam FreeRTOS,
 * ESP-IDF, LVGL, SD nem estado global. Por isso o MESMO codigo roda:
 *   - no firmware (gps.c, track_manager.c, ui.c incluem este header);
 *   - no PC, nos testes automaticos (pasta tests/).
 *
 * Regra: nao inclua nada de IDF aqui. Se precisar de estado, passe por
 * parametro/struct (ex: kb_clock_sync_t).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ----------------------------------------------------------------------
 * NMEA
 * ---------------------------------------------------------------------- */
static inline uint8_t kb_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

/* Checksum NMEA: XOR de tudo entre '$' e '*' tem que bater com os 2 hex
 * depois do '*'. */
static inline bool kb_nmea_checksum_ok(const char *line, size_t len)
{
    if (len < 4 || line[0] != '$') return false;
    const char *star = (const char *)memchr(line, '*', len);
    if (!star || (size_t)(star - line) + 3 > len) return false;

    uint8_t calc = 0;
    for (const char *p = line + 1; p < star; p++) calc ^= (uint8_t)*p;

    uint8_t hi = kb_hex_nibble(star[1]);
    uint8_t lo = kb_hex_nibble(star[2]);
    if (hi == 0xFF || lo == 0xFF) return false;
    return calc == (uint8_t)((hi << 4) | lo);
}

/* ddmm.mmmm (lat) ou dddmm.mmmm (lon) -> graus decimais. */
static inline double kb_nmea_coord_to_decimal(double raw, char hemi)
{
    double minutes = fmod(raw, 100.0);
    double degrees = (raw - minutes) / 100.0;
    double dec = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

/* Divide a sentenca em campos por ',' PRESERVANDO campos vazios (troca ','
 * por '\0' no proprio buffer). Campo vazio e' informacao em NMEA (ex: COG
 * vazio = parado), por isso nao serve strtok. */
static inline int kb_nmea_split(char *line, char *f[], int maxf)
{
    int n = 0;
    char *p = line;
    while (n < maxf) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

/* ----------------------------------------------------------------------
 * Geometria
 * ---------------------------------------------------------------------- */
static inline float kb_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double mean_lat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    double x = dlon * cos(mean_lat);
    return (float)(sqrt(x * x + dlat * dlat) * R);
}

/* Diferenca angular minima entre dois rumos (0..180). */
static inline float kb_heading_diff(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360.0f);
    return (d > 180.0f) ? (360.0f - d) : d;
}

/* lat/lon -> metros locais (leste/norte) relativos a um ponto. */
static inline void kb_local_en_m(double lat, double lon, double ref_lat, double ref_lon,
                                 float *east, float *north)
{
    const double R = 6371000.0;
    double mean_lat = (lat + ref_lat) * 0.5 * M_PI / 180.0;
    *north = (float)((lat - ref_lat) * M_PI / 180.0 * R);
    *east  = (float)((lon - ref_lon) * M_PI / 180.0 * R * cos(mean_lat));
}

/* Distancia assinalada ao longo da direcao de viagem do gate (rumo em
 * graus, 0=Norte 90=Leste). Negativo = antes da linha. */
static inline float kb_signed_gate_dist(double lat, double lon, double gate_lat, double gate_lon,
                                        float heading_deg)
{
    float east, north;
    kb_local_en_m(lat, lon, gate_lat, gate_lon, &east, &north);
    float rad = heading_deg * ((float)M_PI / 180.0f);
    return east * sinf(rad) + north * cosf(rad);
}

/* A transicao amostra-anterior -> amostra-atual cruzou a linha do gate pra
 * frente, e o ponto de cruzamento (interpolado) esta a <= radius_m do ponto
 * do gate? Se sim devolve em *frac (0..1) em que fracao do intervalo entre
 * as duas amostras o cruzamento aconteceu. NAO checa rumo nem lacuna de
 * tempo (quem chama faz). */
static inline bool kb_gate_cross_frac(double prev_lat, double prev_lon,
                                      double cur_lat, double cur_lon,
                                      double gate_lat, double gate_lon,
                                      float gate_heading, float radius_m,
                                      float *frac)
{
    float d_prev = kb_signed_gate_dist(prev_lat, prev_lon, gate_lat, gate_lon, gate_heading);
    float d_cur  = kb_signed_gate_dist(cur_lat, cur_lon, gate_lat, gate_lon, gate_heading);

    if (!(d_prev < 0.0f && d_cur >= 0.0f)) return false;

    float span = d_cur - d_prev;
    float f = (span > 0.0001f) ? (-d_prev / span) : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    /* filtro de proximidade no PONTO DE CRUZAMENTO, nao nas amostras cruas */
    double cross_lat = prev_lat + (cur_lat - prev_lat) * (double)f;
    double cross_lon = prev_lon + (cur_lon - prev_lon) * (double)f;
    if (kb_distance_m(cross_lat, cross_lon, gate_lat, gate_lon) > radius_m) return false;

    if (frac) *frac = f;
    return true;
}

/* ----------------------------------------------------------------------
 * Tempo
 * ---------------------------------------------------------------------- */

/* Algoritmo de Howard Hinnant (days-from-civil): dias desde 1970-01-01. */
static inline int64_t kb_days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline size_t kb_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static inline bool kb_is_digit(char c) { return c >= '0' && c <= '9'; }

/* "hhmmss[.sss]" + "ddmmyy" do RMC -> microssegundos desde 1970 (UTC).
 * *has_frac = true se a hora veio com casas decimais (modulo a 10Hz manda
 * "hhmmss.00"/".10"...; so ai da pra usar o relogio do GPS com precisao). */
static inline bool kb_utc_to_us(const char *t, const char *d, int64_t *out_us, bool *has_frac)
{
    if (!t || !d) return false;
    for (int i = 0; i < 6; i++) {
        if (!kb_is_digit(t[i]) || !kb_is_digit(d[i])) return false;
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[2] - '0') * 10 + (t[3] - '0');
    int ss = (t[4] - '0') * 10 + (t[5] - '0');
    int dd = (d[0] - '0') * 10 + (d[1] - '0');
    int mo = (d[2] - '0') * 10 + (d[3] - '0');
    int yy = (d[4] - '0') * 10 + (d[5] - '0');
    if (hh > 23 || mm > 59 || ss > 60 || dd < 1 || dd > 31 || mo < 1 || mo > 12) return false;

    int64_t frac_us = 0;
    bool frac = false;
    if (t[6] == '.') {
        int64_t scale = 100000;
        for (int i = 7; i < 13 && kb_is_digit(t[i]); i++, scale /= 10) {
            frac_us += (int64_t)(t[i] - '0') * scale;
            frac = true;
        }
    }
    int64_t days = kb_days_from_civil(2000 + yy, mo, dd);
    *out_us = (days * 86400LL + hh * 3600LL + mm * 60LL + ss) * 1000000LL + frac_us;
    if (has_frac) *has_frac = frac;
    return true;
}

/* ----------------------------------------------------------------------
 * Sincronizacao relogio GPS <-> relogio local
 *
 * Problema: o firmware carimbava cada amostra com o instante em que a linha
 * NMEA foi PROCESSADA. Esse instante tem jitter (modulo manda a sentenca em
 * momento variavel apos a medicao, UART, agendamento de task): dezenas de
 * ms que viram erro direto no tempo de volta. O RMC traz a hora real da
 * medicao (hhmmss.ss) - sem jitter.
 *
 * Solucao: usa o relogio do GPS, convertido pra base do relogio local
 * (esp_timer) por um offset = (local - gps) estimado como o MINIMO numa
 * janela de ~10-20 s (menor latencia vista = mais proximo da verdade; a
 * janela acompanha a deriva entre os dois cristais). Variacao limitada por
 * amostra pra o tempo corrigido nunca andar pra tras.
 * ---------------------------------------------------------------------- */
typedef struct {
    bool    valid;
    int64_t off_us;      /* estimativa atual de (local - gps), ja limitada em velocidade */
    int64_t blk_min_us;  /* menor (local - gps) do bloco em andamento */
    int64_t prev_min_us; /* menor do bloco anterior */
    int32_t blk_n;       /* amostras no bloco em andamento */
} kb_clock_sync_t;

#define KB_SYNC_BLOCK         (100)       /* amostras por bloco (10 s a 10 Hz) */
#define KB_SYNC_MAX_STEP_US   (10000)     /* variacao max da estimativa por amostra */
#define KB_SYNC_RESET_US      (2000000)   /* salto > 2 s: recomeca */

static inline int64_t kb_clock_sync(kb_clock_sync_t *s, int64_t local_us, int64_t gps_us)
{
    int64_t off = local_us - gps_us;
    if (!s->valid ||
        off - s->off_us > KB_SYNC_RESET_US || s->off_us - off > KB_SYNC_RESET_US) {
        s->valid = true;
        s->off_us = off;
        s->blk_min_us = off;
        s->prev_min_us = off;
        s->blk_n = 0;
        return gps_us + s->off_us;
    }

    /* minimo numa janela deslizante de 1-2 blocos (10-20 s): a menor latencia
     * vista e' a mais proxima da verdade; amostras atrasadas nao entram. */
    if (off < s->blk_min_us) s->blk_min_us = off;
    if (++s->blk_n >= KB_SYNC_BLOCK) {
        s->prev_min_us = s->blk_min_us;
        s->blk_min_us = off;
        s->blk_n = 0;
    }
    int64_t target = (s->prev_min_us < s->blk_min_us) ? s->prev_min_us : s->blk_min_us;

    /* anda ate o alvo em passos limitados: o tempo corrigido nunca recua e
     * nunca da salto brusco. */
    int64_t diff = target - s->off_us;
    if (diff >  KB_SYNC_MAX_STEP_US) diff =  KB_SYNC_MAX_STEP_US;
    if (diff < -KB_SYNC_MAX_STEP_US) diff = -KB_SYNC_MAX_STEP_US;
    s->off_us += diff;
    return gps_us + s->off_us;
}

/* ----------------------------------------------------------------------
 * Rotulo legivel de sessao (lista da aba VOLTAS)
 *
 * Arquivos de sessao: "YYYYMMDD_HHMMSS[_pista].csv". Mostra
 * "DD/MM/AA HH:MM pista". Nome fora do padrao (ex: "sessao_123_x.csv", sem
 * GPS na hora de gravar) aparece como o proprio nome, sem ".csv" e com
 * '_' virando espaco. Sempre termina em NUL e nunca escreve alem de n.
 * ---------------------------------------------------------------------- */
static inline void kb_session_label(const char *fn, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!fn) return;

    size_t len = strlen(fn);
    if (len > 4 && strcmp(fn + len - 4, ".csv") == 0) len -= 4;

    bool dated = (len >= 15 && fn[8] == '_');
    for (int i = 0; i < 15 && dated; i++) {
        if (i != 8 && !kb_is_digit(fn[i])) dated = false;
    }
    if (dated) {
        int mo = (fn[4] - '0') * 10 + (fn[5] - '0');
        int dd = (fn[6] - '0') * 10 + (fn[7] - '0');
        int hh = (fn[9] - '0') * 10 + (fn[10] - '0');
        int mi = (fn[11] - '0') * 10 + (fn[12] - '0');
        int ss = (fn[13] - '0') * 10 + (fn[14] - '0');
        if (mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh > 23 || mi > 59 || ss > 59) dated = false;
    }

    size_t o = 0;
#define KB_PUT(c) do { if (o + 1 < n) out[o++] = (c); } while (0)
    if (dated) {
        KB_PUT(fn[6]); KB_PUT(fn[7]); KB_PUT('/');
        KB_PUT(fn[4]); KB_PUT(fn[5]); KB_PUT('/');
        KB_PUT(fn[2]); KB_PUT(fn[3]); KB_PUT(' ');
        KB_PUT(fn[9]); KB_PUT(fn[10]); KB_PUT(':');
        KB_PUT(fn[11]); KB_PUT(fn[12]);
        if (len > 16 && fn[15] == '_') {
            KB_PUT(' ');
            for (size_t i = 16; i < len; i++) KB_PUT(fn[i] == '_' ? ' ' : fn[i]);
        }
    } else {
        for (size_t i = 0; i < len; i++) KB_PUT(fn[i] == '_' ? ' ' : fn[i]);
    }
#undef KB_PUT
    out[o] = '\0';
}

/* ----------------------------------------------------------------------
 * Nome de pista (vira nome de arquivo no SD)
 * ---------------------------------------------------------------------- */
enum {
    KB_NAME_OK = 0,
    KB_NAME_EMPTY,      /* nulo ou vazio */
    KB_NAME_TOO_LONG,   /* nao cabe em max_with_nul (incluindo o NUL) */
    KB_NAME_BAD         /* caractere/forma invalida pra FAT */
};

static inline int kb_name_check(const char *name, size_t max_with_nul)
{
    if (!name) return KB_NAME_EMPTY;
    size_t n = kb_strnlen(name, max_with_nul);
    if (n == 0) return KB_NAME_EMPTY;
    if (n >= max_with_nul) return KB_NAME_TOO_LONG;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return KB_NAME_BAD;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return KB_NAME_BAD;
    }
    if (name[n - 1] == ' ' || name[n - 1] == '.') return KB_NAME_BAD; /* FAT remove */
    return KB_NAME_OK;
}

/* ----------------------------------------------------------------------
 * A-GPS (AssistNow): helpers puros
 * ---------------------------------------------------------------------- */

/* Bytes -> hex MAIUSCULO continuo (sem espacos). Devolve o numero de chars
 * escritos (sem o NUL), ou 0 se outsz nao comporta tudo (out fica vazio). */
static inline size_t kb_hex_encode(const uint8_t *in, size_t n, char *out, size_t outsz)
{
    static const char H[] = "0123456789ABCDEF";
    if (!out || outsz == 0) return 0;
    out[0] = '\0';
    if (!in || n * 2 + 1 > outsz) return 0;
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[in[i] >> 4];
        out[2 * i + 1] = H[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
    return 2 * n;
}

/* Le o valor STRING de uma chave de um JSON simples ("key":"valor"),
 * tolerando espacos em volta do ':'. Nao e' um parser completo: serve pra
 * respostas planas como {"chipcode":"...","serviceUrl":"..."}. Valor com
 * aspas escapadas (\") nao e' suportado. false se nao achou ou nao coube. */
static inline bool kb_json_str(const char *json, const char *key, char *out, size_t outsz)
{
    if (!json || !key || !out || outsz == 0) return false;
    out[0] = '\0';
    char pat[48];
    size_t kl = strlen(key);
    if (kl + 3 > sizeof(pat)) return false;
    pat[0] = '"';
    memcpy(pat + 1, key, kl);
    pat[kl + 1] = '"';
    pat[kl + 2] = '\0';
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += kl + 2;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        if (o + 1 >= outsz) { out[0] = '\0'; return false; }
        out[o++] = *p++;
    }
    if (*p != '"') { out[0] = '\0'; return false; }
    out[o] = '\0';
    return true;
}

/* Percent-encode do minimo pra um valor de query string (base64 do chipcode
 * pode ter + / =). Devolve false se nao coube. */
static inline bool kb_url_encode(const char *in, char *out, size_t outsz)
{
    if (!in || !out || outsz == 0) return false;
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            if (o + 2 > outsz) { out[0] = '\0'; return false; }
            out[o++] = (char)c;
        } else {
            static const char H[] = "0123456789ABCDEF";
            if (o + 4 > outsz) { out[0] = '\0'; return false; }
            out[o++] = '%';
            out[o++] = H[c >> 4];
            out[o++] = H[c & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

/* Extrator de frames UBX de um stream de bytes (B5 62 cls id lenL lenH
 * payload ckA ckB). Alimenta byte a byte; kb_ubx_feed devolve true quando
 * um frame COMPLETO com checksum Fletcher valido acabou de fechar: o frame
 * inteiro (inclui header e checksum) fica em p->buf[0..p->n-1] ate o
 * proximo feed. Frame maior que KB_UBX_MAX e' descartado (resync no
 * proximo B5 62). NMEA nunca tem bytes >= 0x80, entao nao ha falso positivo. */
#define KB_UBX_MAX 520

typedef struct {
    uint8_t  buf[KB_UBX_MAX];
    uint16_t n;     /* bytes acumulados do frame atual */
    bool     done;  /* ultimo feed fechou um frame (limpo no feed seguinte) */
} kb_ubx_parser_t;

static inline bool kb_ubx_feed(kb_ubx_parser_t *p, uint8_t b)
{
    if (p->done) { p->n = 0; p->done = false; }

    if (p->n == 0) {
        if (b == 0xB5) p->buf[p->n++] = b;
        return false;
    }
    if (p->n == 1) {
        if (b == 0x62)      p->buf[p->n++] = b;   /* header completo */
        else if (b == 0xB5) p->n = 1;             /* outro B5: recomeca daqui (buf[0] ja e' B5) */
        else                p->n = 0;
        return false;
    }
    p->buf[p->n++] = b;
    if (p->n < 6) return false;

    uint16_t len = (uint16_t)(p->buf[4] | (p->buf[5] << 8));
    if ((uint32_t)len + 8 > KB_UBX_MAX) { p->n = 0; return false; }
    if (p->n < (uint16_t)(len + 8)) return false;

    uint8_t a = 0, c = 0;
    for (uint16_t i = 2; i < (uint16_t)(len + 6); i++) { a = (uint8_t)(a + p->buf[i]); c = (uint8_t)(c + a); }
    if (a == p->buf[len + 6] && c == p->buf[len + 7]) { p->done = true; return true; }
    p->n = 0;
    return false;
}

/* ----------------------------------------------------------------------
 * Explorer do SD: validacao de caminho vindo da URL.
 * Aceita "/" ou "/a/b/c.txt" (barra no fim tolerada). Rejeita: nao comecar
 * com '/', segmento vazio ("//"), segmento "." ou "..", '\\', '"', '%',
 * controles, e tamanho >= maxlen. Nada de sair do /sdcard por construcao
 * (o chamador SEMPRE prefixa o ponto de montagem).
 * ---------------------------------------------------------------------- */
static inline bool kb_fs_path_ok(const char *p, size_t maxlen)
{
    if (!p || p[0] != '/') return false;
    size_t n = strlen(p);
    if (n >= maxlen) return false;
    if (n == 1) return true;
    size_t i = 1;
    while (i <= n) {
        size_t s = i;
        while (i < n && p[i] != '/') i++;
        size_t seglen = i - s;
        if (seglen == 0) {
            /* segmento vazio so e' aceito como barra FINAL ("/a/") */
            if (!(i == n && s == n)) return false;
        } else if ((seglen == 1 && p[s] == '.') || (seglen == 2 && p[s] == '.' && p[s + 1] == '.')) {
            return false;
        }
        i++;
    }
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) {
        if (*c < 0x20 || *c == 0x7F || *c == '\\' || *c == '"' || *c == '%') return false;
    }
    return true;
}

/* Nome (so o ultimo segmento) pra Content-Disposition. Copia p/ out
 * trocando aspas por '_'. Devolve out. */
static inline const char *kb_fs_basename(const char *p, char *out, size_t outsz)
{
    const char *b = p;
    for (const char *c = p; *c; c++) if (*c == '/' && c[1]) b = c + 1;
    size_t o = 0;
    for (; *b && *b != '/' && o + 1 < outsz; b++) out[o++] = (*b == '"') ? '_' : *b;
    out[o] = '\0';
    return out;
}

/* ----------------------------------------------------------------------
 * UBX: montar frame e ler resposta de UBX-CFG-VALGET
 * ---------------------------------------------------------------------- */

/* Monta B5 62 cls id len payload ck_a ck_b em out. Devolve o tamanho total
 * (len+8) ou 0 se nao couber em cap. */
static inline size_t kb_ubx_build(uint8_t *out, size_t cap, uint8_t cls, uint8_t id,
                                  const uint8_t *payload, uint16_t len)
{
    if (!out || (size_t)len + 8 > cap) return 0;
    out[0] = 0xB5; out[1] = 0x62; out[2] = cls; out[3] = id;
    out[4] = (uint8_t)(len & 0xFF); out[5] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) out[6 + i] = payload ? payload[i] : 0;
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < (size_t)len + 6; i++) { a = (uint8_t)(a + out[i]); b = (uint8_t)(b + a); }
    out[6 + len] = a; out[7 + len] = b;
    return (size_t)len + 8;
}

/* Procura `key` numa resposta UBX-CFG-VALGET (frame completo, n bytes) e
 * devolve o 1o byte do valor (chaves L/U1 - todas as CFG-SIGNAL-*). false
 * se nao e' VALGET, esta truncada ou nao tem a chave. */
static inline bool kb_ubx_valget_u8(const uint8_t *f, size_t n, uint32_t key, uint8_t *val)
{
    if (!f || n < 14 || f[0] != 0xB5 || f[1] != 0x62 || f[2] != 0x06 || f[3] != 0x8B) return false;
    size_t len = (size_t)f[4] | ((size_t)f[5] << 8);
    if (len + 8 > n || len < 4) return false;
    size_t off = 6 + 4;                    /* header 6 + (version, layer, position[2]) */
    size_t end = 6 + len;
    while (off + 5 <= end) {
        uint32_t k = (uint32_t)f[off] | ((uint32_t)f[off + 1] << 8) |
                     ((uint32_t)f[off + 2] << 16) | ((uint32_t)f[off + 3] << 24);
        static const uint8_t SZ[8] = { 0, 1, 1, 2, 4, 8, 0, 0 };
        uint8_t sz = SZ[(k >> 28) & 7];
        if (sz == 0 || off + 4 + sz > end) return false;
        if (k == key) { *val = f[off + 4]; return true; }
        off += 4 + sz;
    }
    return false;
}
