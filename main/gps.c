/*
 * gps.c - ver gps.h pro contexto das decisoes de arquitetura.
 */
#include "gps.h"
#include "config.h"
#include "app_events.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "driver/uart.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "gps";

#define GPS_LINE_BUF_SIZE   (96)
#define GPS_UART_CHUNK_SIZE (64)

static SemaphoreHandle_t s_state_mutex;
static gps_sample_t      s_state;
static QueueHandle_t     s_sd_log_queue;

static gps_race_mode_t s_mode = GPS_MODE_QUALY;
static bool     s_recording = false;
static bool     s_waiting_for_movement = false;

static bool     s_finish_line_set = false;
static double   s_finish_lat, s_finish_lon;
static float    s_finish_heading;
static int64_t  s_last_cross_us = 0;

static uint32_t s_lap_number = 0;
static uint32_t s_best_lap_ms = 0;
static int32_t  s_last_delta_ms = 0;

/* Estatisticas da sessao (popup de encerramento - ver gps_get_session_stats).
 * sumsq em u64 aguenta: volta de 120s = 1.44e10 de quadrado, cabem ~1e9
 * voltas antes de estourar. */
static uint32_t s_stat_lap_count    = 0;
static uint64_t s_stat_sum_ms       = 0;
static uint64_t s_stat_sumsq_ms     = 0;
static float    s_stat_max_speed    = 0.0f;

/* Auto-sessao - ver gps.h. Timers de "condicao sustentada" + trava de
 * um-disparo (nao repostar o evento a 10Hz enquanto o main nao processa). */
static bool     s_auto_session_enabled = false;
static int64_t  s_auto_start_since_us  = 0;
static int64_t  s_auto_stop_since_us   = 0;
static bool     s_auto_start_fired     = false;
static bool     s_auto_stop_fired      = false;

/* ----------------------------------------------------------------------
 * Delta AO VIVO - trajetoria distancia x tempo da melhor volta, usada
 * como referencia pra comparar contra a volta em andamento a cada
 * amostra de GPS (mesma ideia dos deltas de telemetria de carro de
 * corrida real: nao espera a volta fechar, mostra ganhando/perdendo
 * tempo o tempo todo comparado ao ritmo da melhor volta ja feita).
 *
 * Guarda (distancia acumulada desde o gate, tempo decorrido) a cada
 * amostra da volta em andamento (s_cur_trace). Quando uma volta fecha e
 * vira a nova melhor, esse buffer e copiado pra s_best_trace, que passa
 * a ser a referencia. Delta ao vivo = tempo decorrido agora menos o
 * tempo que a melhor volta levou pra chegar nessa MESMA distancia
 * (interpolado entre as duas amostras mais proximas do trace).
 *
 * Alocado em PSRAM (32MB sobrando, RAM interna e' escassa nesse projeto -
 * ver historico de crash por falta de RAM interna com BT/WiFi ligados).
 * ---------------------------------------------------------------------- */
#define LAP_TRACE_MAX_SAMPLES (3000) /* ~5min de volta a 10Hz - generoso pra kart */

typedef struct {
    float    dist_m;   /* distancia acumulada desde o ultimo cruzamento do gate */
    uint32_t time_ms;  /* tempo decorrido desde o ultimo cruzamento do gate */
} lap_trace_point_t;

static lap_trace_point_t *s_cur_trace;        /* volta em andamento */
static int                 s_cur_trace_count;
static lap_trace_point_t *s_best_trace;       /* referencia: melhor volta da sessao */
static int                 s_best_trace_count;
static float                s_cur_lap_dist_m; /* distancia acumulada desde o ultimo gate */

static int32_t  s_live_delta_ms = 0;
static bool     s_live_delta_valid = false;

/* Zera o trace/distancia da volta em andamento - chamado sempre que um
 * novo "zero" de volta comeca (cruzamento do gate, largada em RACE, ou
 * reset de sessao). NAO mexe em s_best_trace (referencia so muda quando
 * uma volta nova bate o recorde). */
static void lap_trace_reset_current(void)
{
    s_cur_trace_count = 0;
    s_cur_lap_dist_m  = 0.0f;
    s_live_delta_valid = false;
}

/* Interpola o tempo (ms) que o trace de referencia levou pra alcancar
 * dist_m. Busca linear - poucos milhares de pontos no maximo, roda a
 * 10Hz, sobra CPU de sobra nesse chip. */
static uint32_t lap_trace_interp(const lap_trace_point_t *trace, int count, float dist_m)
{
    if (count <= 0) return 0;
    if (dist_m <= trace[0].dist_m) return trace[0].time_ms;
    if (dist_m >= trace[count - 1].dist_m) return trace[count - 1].time_ms;

    for (int i = 1; i < count; i++) {
        if (dist_m <= trace[i].dist_m) {
            float span = trace[i].dist_m - trace[i - 1].dist_m;
            float frac = (span > 0.0001f) ? (dist_m - trace[i - 1].dist_m) / span : 0.0f;
            return trace[i - 1].time_ms +
                   (uint32_t)(frac * (float)(trace[i].time_ms - trace[i - 1].time_ms));
        }
    }
    return trace[count - 1].time_ms;
}

/* Setores opcionais - todos false por default; ativados via gps_set_sector_point(). */
static bool     s_sector_set[GPS_MAX_SECTORS];
static double   s_sector_lat[GPS_MAX_SECTORS];
static double   s_sector_lon[GPS_MAX_SECTORS];
static float    s_sector_heading[GPS_MAX_SECTORS];
static bool     s_sector_crossed[GPS_MAX_SECTORS];   /* cruzou nessa volta? */
static uint32_t s_sector_split_ms[GPS_MAX_SECTORS];  /* split da volta atual (0 = ainda nao) */
static uint32_t s_best_sector_ms[GPS_MAX_SECTORS];   /* melhor split historico da sessao (cumulativo desde a largada) */
static int32_t  s_sector_delta_ms[GPS_MAX_SECTORS];    /* delta do cruzamento vs best ANTERIOR (feedback ao vivo) */
static bool     s_sector_delta_valid[GPS_MAX_SECTORS]; /* false = 1o cruzamento (sem referencia) ou nao cruzado */

/* ----------------------------------------------------------------------
 * Setores AUTOMATICOS por distancia. So entram em cena quando o piloto
 * NAO marcou nenhum setor manual (all-or-nothing). Em vez de gates
 * geograficos, sao limiares de DISTANCIA acumulada na volta: assim que a
 * distancia da volta em andamento cruza N/(GPS_MAX_SECTORS+1) da volta de
 * referencia, registra o split. Reusa s_sector_set/crossed/split_ms/
 * best_sector_ms (mesmo maquinario de UI/ideal), so a DETECCAO muda.
 * Nao sao persistidos (gps_get_sector_point retorna false quando auto).
 * ---------------------------------------------------------------------- */
static bool     s_auto_sectors_active = false;
static float    s_auto_sector_dist_m[GPS_MAX_SECTORS]; /* limiar de distancia de cada setor auto */
static float    s_ref_lap_dist_m = 0.0f;               /* distancia total da volta de referencia */

/* Constroi/atualiza os setores automaticos a partir de s_ref_lap_dist_m.
 * No-op se ainda nao ha referencia, ou se ha QUALQUER setor manual
 * marcado (manual tem prioridade). Chamada com s_state_mutex preso. */
static void build_auto_sectors(void)
{
    if (s_ref_lap_dist_m < 1.0f) return;
    /* se ja ha setor manual, nao gera automatico */
    if (!s_auto_sectors_active) {
        for (int i = 0; i < GPS_MAX_SECTORS; i++) if (s_sector_set[i]) return;
    }
    s_auto_sectors_active = true;
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        s_sector_set[i]         = true;
        s_sector_crossed[i]     = false;
        s_sector_split_ms[i]    = 0;
        s_auto_sector_dist_m[i] = s_ref_lap_dist_m * (float)(i + 1) / (float)(GPS_MAX_SECTORS + 1);
    }
    ESP_LOGI(TAG, "Setores automaticos gerados (ref=%.0fm)", (double)s_ref_lap_dist_m);
}

/* Volta ideal (feature pedida pelo usuario) - soma dos melhores SEGMENTOS
 * (nao dos melhores splits cumulativos acima). Com GPS_MAX_SECTORS=2 setores
 * marcados, uma volta tem ate 3 segmentos: largada->S1, S1->S2, S2->chegada.
 * s_best_seg_ms[i] guarda o menor tempo ja visto pra cada segmento i,
 * possivelmente vindo de voltas DIFERENTES (e essa a definicao classica de
 * "ideal lap" em telemetria - o que seria possivel combinando os melhores
 * pedacos). 0 = segmento ainda sem registro nessa sessao. So atualiza a
 * partir de voltas "limpas" (todos os setores marcados foram cruzados) -
 * ver update_best_segments(). Persiste durante a sessao, zera em
 * gps_session_reset() e gps_clear_sector_point() (mudou o numero de
 * segmentos, os antigos nao fazem mais sentido). */
static uint32_t s_best_seg_ms[GPS_MAX_SECTORS + 1];

static int16_t  s_utc_offset_min = DEFAULT_UTC_OFFSET_MIN;
static float    s_gate_radius_m = GATE_RADIUS_M;
static uint32_t s_min_lap_time_ms = MIN_LAP_TIME_MS;
static struct tm s_last_utc_tm;
static bool      s_has_datetime = false;

/* Ultima vez que ALGUM byte chegou da UART do GPS (nao so sentenca
 * valida) - usado so pra saber se o modulo esta fisicamente respondendo.
 * Ver gps_get_link_status(). */
static volatile int64_t s_last_uart_rx_ms = 0;

/* ----------------------------------------------------------------------
 * Log de diagnostico do link serial - resumo a cada 2s no monitor serial
 * (bytes recebidos, linhas validas/invalidas, ultima sentenca crua) pra
 * confirmar visualmente se o dado esta mesmo chegando do modulo e em que
 * formato, sem inundar o log a 10Hz. So gps_task mexe nesses contadores.
 * ---------------------------------------------------------------------- */
static uint32_t s_dbg_bytes_rx  = 0;
static uint32_t s_dbg_lines_ok  = 0;
static uint32_t s_dbg_lines_bad = 0;
static char     s_dbg_last_line[GPS_LINE_BUF_SIZE] = {0};
static int64_t  s_dbg_last_log_ms = 0;

/* Diagnostico de RF (GSV) - satelites EM VISTA por constelacao e melhor
 * SNR do periodo. Independe de fix: ceu aberto com antena boa mostra
 * 8-20 em vista em SEGUNDOS mesmo sem fix nenhum; ficar em 0-1 por
 * minutos = sinal nao chega no receptor (antena/orientacao/EMI), e
 * nenhum A-GPS resolve isso. Melhor SNR da a dimensao: <20 dBHz =
 * inutilizavel, 25-35 = fraco, >40 = saudavel. */
static uint8_t s_dbg_sv_gps = 0, s_dbg_sv_glo = 0, s_dbg_sv_gal = 0, s_dbg_sv_bds = 0;
static uint8_t s_dbg_best_snr = 0;
/* "Em vista" (acima) = satelites que o modulo SABE que existem (almanaque/
 * A-GPS preenche elevacao/azimute) - sobe sem a antena melhorar nada.
 * "Ouvindo" (abaixo) = satelites com campo SNR preenchido no GSV, ou
 * seja, sinal REALMENTE chegando no receptor. E' o numero que importa
 * pra diagnosticar antena. Acumulado por ciclo de mensagens GSV (msg
 * 1..N do mesmo talker) e comitado no fim do ciclo. */
static uint8_t s_gsv_trk_tmp[4] = {0};                 /* contagem parcial do ciclo em andamento */
static uint8_t s_dbg_trk[4]     = {0};                 /* ultimo ciclo completo: 0=gps 1=glo 2=gal 3=bds */
/* true assim que QUALQUER $GLGSV chegar - confirma que o UBX-CFG-VALSET
 * de habilitar GLONASS foi aceito pelo modulo (com GLO ligado o modulo
 * emite GLGSV mesmo com 0 satelites, igual faz com GAGSV). Se isso
 * ficar false, o VALSET foi rejeitado/perdido. */
static bool s_dbg_glgsv_seen = false;
/* Period bem mais espacado (era 2s, inundava o log serial e atrapalhava
 * ver qualquer outra coisa - ex: diagnostico de wifi export). Nivel hoje
 * e' LOGI (1 linha/15s e' barato e foi decisivo pra diagnosticar antena
 * em campo); se um dia incomodar, baixa pra LOGD de novo. */
#define GPS_DEBUG_LOG_PERIOD_MS (15000)

/* ----------------------------------------------------------------------
 * Helpers de baixo nivel
 * ---------------------------------------------------------------------- */

static inline uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

/* Valida checksum NMEA (XOR de tudo entre '$' e '*'). Linha corrompida
 * (comum em UART perto de ignicao/motor) e descartada aqui em vez de
 * virar lat/lon furada la na frente. */
static bool nmea_checksum_ok(const char *line, size_t len)
{
    if (len < 4 || line[0] != '$') return false;
    const char *star = memchr(line, '*', len);
    if (!star || (size_t)(star - line) + 3 > len) return false;

    uint8_t calc = 0;
    for (const char *p = line + 1; p < star; p++) calc ^= (uint8_t)*p;

    uint8_t hi = hex_nibble(star[1]);
    uint8_t lo = hex_nibble(star[2]);
    if (hi == 0xFF || lo == 0xFF) return false;
    return calc == ((hi << 4) | lo);
}

/* ddmm.mmmm (lat) ou dddmm.mmmm (lon) -> graus decimais. Funciona pros
 * dois formatos porque so olha "ultimos 2 digitos antes da fracao = minutos". */
static double nmea_coord_to_decimal(double raw, char hemi)
{
    double minutes = fmod(raw, 100.0);
    double degrees = (raw - minutes) / 100.0;
    double dec = degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
}

static float distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double mean_lat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    double x = dlon * cos(mean_lat);
    return (float)(sqrt(x * x + dlat * dlat) * R);
}

static float heading_diff(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360.0f);
    return (d > 180.0f) ? (360.0f - d) : d;
}

/* Divide a sentenca em campos por ',' PRESERVANDO campos vazios (troca
 * ',' por '\0' no proprio buffer; f[i] aponta pro inicio de cada campo).
 * strtok_r NAO serve pra NMEA: ele colapsa delimitadores consecutivos
 * (",," vira um so), e campo VAZIO e' informacao aqui - COG vazio =
 * parado, SNR vazio = satelite nao rastreado, GGA sem fix e' quase toda
 * vazia. O colapso desalinhava os campos seguintes (ver bug real no
 * comentario de parse_rmc). */
static int nmea_split(char *line, char *f[], int maxf)
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
 * Deteccao de cruzamento de gate (chegada + setores).
 *
 * Antes: contava a volta assim que a posicao ATUAL entrava no raio do
 * ponto marcado (com heading parecido) - "amostra unica, distancia ao
 * ponto". Problema real (reportado pelo usuario): o exato instante em
 * que uma leitura de GPS cai dentro desse circulo depende do angulo de
 * aproximacao/trajetoria, que varia um pouco a cada volta - isso ja e
 * inconsistencia suficiente pra estragar telemetria (tempo de volta
 * varia so por causa de ONDE dentro do raio o GPS "pegou" a leitura,
 * nao por diferenca real de performance).
 *
 * Fix: tratar a chegada como uma LINHA imaginaria perpendicular a
 * direcao registrada ao marcar o gate, e detectar o CRUZAMENTO de
 * verdade comparando a amostra atual com a anterior (projecao assinada
 * na direcao de viagem muda de negativa pra positiva = cruzou pra
 * frente). O raio configuravel vira so um filtro grosseiro ("isso
 * aconteceu perto do gate de verdade, nao e a reta infinita cruzando em
 * outro lugar da pista"), nao o gatilho em si. Bonus: interpola o
 * instante exato do cruzamento entre as duas amostras de GPS, o que da
 * resolucao temporal MELHOR que o proprio intervalo entre fixes.
 * ---------------------------------------------------------------------- */
static bool    s_prev_valid = false;
static double  s_prev_lat, s_prev_lon;
static int64_t s_prev_us;

/* lat/lon -> metros locais (leste/norte) relativos a um ponto de
 * referencia. Projecao equiretangular - precisao de sobra na escala de
 * uma pista de kart (poucas centenas de metros no maximo). */
static void local_en_m(double lat, double lon, double ref_lat, double ref_lon,
                        float *east, float *north)
{
    const double R = 6371000.0;
    double mean_lat = (lat + ref_lat) * 0.5 * M_PI / 180.0;
    *north = (float)((lat - ref_lat) * M_PI / 180.0 * R);
    *east  = (float)((lon - ref_lon) * M_PI / 180.0 * R * cos(mean_lat));
}

/* Distancia assinalada ao longo da direcao de viagem do gate
 * (heading_deg), relativa ao ponto do gate. Negativo = antes da linha,
 * positivo (ou zero) = na linha ou depois dela. */
static float signed_gate_dist(double lat, double lon, double gate_lat, double gate_lon,
                               float heading_deg)
{
    float east, north;
    local_en_m(lat, lon, gate_lat, gate_lon, &east, &north);
    float rad = heading_deg * ((float)M_PI / 180.0f);
    /* heading 0=Norte(+north), 90=Leste(+east) - projecao no vetor
     * unitario da direcao de viagem */
    return east * sinf(rad) + north * cosf(rad);
}

/* Checa se a transicao amostra-anterior -> amostra-atual cruzou o gate
 * pra frente, perto o suficiente dele e na direcao certa. Retorna o
 * instante interpolado do cruzamento via *out_us (so valido se
 * retornar true). */
static bool check_gate_cross(double gate_lat, double gate_lon, float gate_heading,
                              int64_t cur_us, int64_t *out_us)
{
    if (!s_prev_valid) return false;

    float d_prev = signed_gate_dist(s_prev_lat, s_prev_lon, gate_lat, gate_lon, gate_heading);
    float d_cur  = signed_gate_dist(s_state.lat, s_state.lon, gate_lat, gate_lon, gate_heading);

    /* cruzamento pra frente: estava antes da linha, agora esta na linha
     * ou depois dela */
    if (!(d_prev < 0.0f && d_cur >= 0.0f)) return false;

    /* interpolacao linear do instante/posicao exatos do cruzamento entre
     * as duas amostras, baseado em quao perto de zero cada lado estava */
    float span = d_cur - d_prev;
    float frac = (span > 0.0001f) ? (-d_prev / span) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    /* Filtro de proximidade no PONTO DE CRUZAMENTO interpolado, nao nas
     * amostras cruas (correcao de campo: raio < 15m nao contava volta).
     * Nas amostras, a distancia ao gate carrega a componente LONGITUDINAL
     * (a 100km/h/10Hz cada amostra fica ate ~3m antes/depois da linha),
     * que se soma ao afastamento lateral real e obriga raio inflado. No
     * ponto interpolado a longitudinal e' ~zero por definicao - o que
     * sobra e' o afastamento LATERAL verdadeiro, entao o raio vira
     * literalmente "meia-largura da linha de chegada" (largura da pista
     * + erro de GPS), da pra apertar sem perder cruzamento. */
    double cross_lat = s_prev_lat + (s_state.lat - s_prev_lat) * (double)frac;
    double cross_lon = s_prev_lon + (s_state.lon - s_prev_lon) * (double)frac;
    if (distance_m(cross_lat, cross_lon, gate_lat, gate_lon) > s_gate_radius_m) return false;

    /* direcao de viagem tem que bater com a direcao registrada no gate */
    if (heading_diff(s_state.heading_deg, gate_heading) > GATE_MAX_HEADING_DIFF) return false;

    *out_us = s_prev_us + (int64_t)((double)frac * (double)(cur_us - s_prev_us));
    return true;
}

/* Algoritmo de Howard Hinnant (days-from-civil) - converte data
 * gregoriana pra dias desde epoch sem depender de timegm()/TZ, que
 * variam de disponibilidade entre toolchains embarcadas. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t my_timegm(const struct tm *tm)
{
    int64_t days = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* Atualiza s_best_seg_ms[] (volta ideal) a partir dos splits cumulativos
 * da volta que acabou de fechar. Chamada com s_sector_split_ms/crossed
 * ainda intactos (ANTES do memset que zera pra proxima volta) e com
 * s_state_mutex ja preso (mesmo contexto de process_gate_timing).
 *
 * So atualiza numa volta "limpa": todo setor MARCADO precisa ter sido
 * cruzado nessa volta. Se o piloto pulou um gate (ruido de GPS, saiu da
 * pista, etc) os segmentos ficariam desalinhados com os de outras voltas -
 * melhor nao contaminar a referencia do que guardar um segmento errado. */
static void update_best_segments(uint32_t lap_time_ms)
{
    uint32_t prev_ms = 0;
    int seg = 0;
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (!s_sector_set[i]) continue;
        if (!s_sector_crossed[i]) return; /* volta incompleta - nao mexe em nenhum best */
        uint32_t seg_ms = s_sector_split_ms[i] - prev_ms;
        if (s_best_seg_ms[seg] == 0 || seg_ms < s_best_seg_ms[seg]) s_best_seg_ms[seg] = seg_ms;
        prev_ms = s_sector_split_ms[i];
        seg++;
    }
    uint32_t last_seg_ms = lap_time_ms - prev_ms;
    if (s_best_seg_ms[seg] == 0 || last_seg_ms < s_best_seg_ms[seg]) s_best_seg_ms[seg] = last_seg_ms;
}

/* Soma s_best_seg_ms[] pros segmentos ativos (setores marcados + 1) e
 * retorna a volta ideal, ou 0 se algum segmento ainda nao tem referencia
 * (sessao nova, ainda sem volta limpa completa). Chamada com mutex preso. */
static uint32_t compute_ideal_lap_ms(void)
{
    int active_segs = 1; /* segmento final (ultimo setor -> chegada) sempre existe */
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (s_sector_set[i]) active_segs++;
    }
    uint32_t sum = 0;
    for (int i = 0; i < active_segs; i++) {
        if (s_best_seg_ms[i] == 0) return 0;
        sum += s_best_seg_ms[i];
    }
    return sum;
}

/* ----------------------------------------------------------------------
 * Logica de gate / timing de volta. Chamada com s_state_mutex JA preso.
 * ---------------------------------------------------------------------- */
static void process_gate_timing(int64_t now_us)
{
    /* Distancia percorrida desde a amostra anterior - calculada ANTES de
     * qualquer coisa mexer em s_prev_lat/lon abaixo. So usada pro trace
     * do delta ao vivo; nao interfere na deteccao de cruzamento (que usa
     * check_gate_cross, projecao separada). */
    float step_dist_m = s_prev_valid
        ? distance_m(s_prev_lat, s_prev_lon, s_state.lat, s_state.lon)
        : 0.0f;

    if (!s_finish_line_set) {
        /* guarda amostra mesmo sem linha marcada ainda, pra ja ter
         * historico assim que o piloto marcar */
        s_prev_lat = s_state.lat; s_prev_lon = s_state.lon;
        s_prev_us = now_us; s_prev_valid = true;
        return;
    }

    if (s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement) {
        if (s_state.speed_kmh > RACE_START_SPEED_KMH) {
            s_waiting_for_movement = false;
            s_last_cross_us = now_us; /* largada = cruzamento zero da contagem */
            lap_trace_reset_current(); /* clock zerou aqui - trace tambem */
        }
        s_prev_lat = s_state.lat; s_prev_lon = s_state.lon;
        s_prev_us = now_us; s_prev_valid = true;
        return;
    }

    /* ---- delta ao vivo: acumula distancia/tempo da volta em andamento
     * e compara contra a trajetoria da melhor volta, a cada amostra
     * (mesmo antes de fechar a volta ou cruzar qualquer setor). ---- */
    s_cur_lap_dist_m += step_dist_m;
    uint32_t elapsed_ms = (uint32_t)((now_us - s_last_cross_us) / 1000);
    if (s_cur_trace && s_cur_trace_count < LAP_TRACE_MAX_SAMPLES) {
        s_cur_trace[s_cur_trace_count].dist_m  = s_cur_lap_dist_m;
        s_cur_trace[s_cur_trace_count].time_ms = elapsed_ms;
        s_cur_trace_count++;
    }
    if (s_best_trace && s_best_trace_count > 1) {
        uint32_t ref_ms = lap_trace_interp(s_best_trace, s_best_trace_count, s_cur_lap_dist_m);
        s_live_delta_ms    = (int32_t)elapsed_ms - (int32_t)ref_ms;
        s_live_delta_valid = true;
    } else {
        s_live_delta_valid = false;
    }

    /* ---- verificacao de setores opcionais ----------------------------------------
     * Mesmo algoritmo de cruzamento de linha do gate principal (ver
     * check_gate_cross). Debounce de 3s pra nao registrar cruzamento
     * imediatamente apos a largada/gate. Se o piloto nao marcou nenhum
     * setor, esse loop e no-op inteiro.
     * ---------------------------------------------------------------------------- */
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (!s_sector_set[i] || s_sector_crossed[i]) continue;
        uint32_t split_ms;
        if (s_auto_sectors_active) {
            /* setor automatico: dispara quando a DISTANCIA acumulada da
             * volta cruza o limiar. prev < limiar <= atual. */
            float prev_dist = s_cur_lap_dist_m - step_dist_m;
            if (!(prev_dist < s_auto_sector_dist_m[i] && s_cur_lap_dist_m >= s_auto_sector_dist_m[i]))
                continue;
            split_ms = elapsed_ms; /* tempo desde a largada/gate ate aqui */
        } else {
            /* setor manual: gate geografico (mesmo algoritmo do gate principal) */
            int64_t t_cross;
            if (!check_gate_cross(s_sector_lat[i], s_sector_lon[i], s_sector_heading[i], now_us, &t_cross)) continue;
            int64_t since_ms = (t_cross - s_last_cross_us) / 1000;
            if (since_ms < 3000) continue; /* debounce: < 3s desde ultima passagem pelo gate */
            split_ms = (uint32_t)since_ms;
        }
        s_sector_split_ms[i] = split_ms;
        s_sector_crossed[i]  = true;
        /* delta ao vivo do setor - contra o best ANTES deste cruzamento
         * (calculado antes do update do best logo abaixo, senao um novo
         * recorde compararia consigo mesmo e daria sempre 0) */
        s_sector_delta_valid[i] = (s_best_sector_ms[i] > 0);
        s_sector_delta_ms[i]    = s_sector_delta_valid[i]
                                  ? (int32_t)split_ms - (int32_t)s_best_sector_ms[i] : 0;
        if (s_best_sector_ms[i] == 0 || s_sector_split_ms[i] < s_best_sector_ms[i])
            s_best_sector_ms[i] = s_sector_split_ms[i];
        ESP_LOGI(TAG, "Setor %d%s: %u ms", i + 1,
                 s_auto_sectors_active ? " (auto)" : "", (unsigned)s_sector_split_ms[i]);
    }

    /* ---- gate principal (linha de chegada) ---- */
    int64_t t_cross;
    if (!check_gate_cross(s_finish_lat, s_finish_lon, s_finish_heading, now_us, &t_cross)) {
        s_prev_lat = s_state.lat; s_prev_lon = s_state.lon;
        s_prev_us = now_us; s_prev_valid = true;
        return;
    }

    int64_t since_last_ms = (t_cross - s_last_cross_us) / 1000;
    if (since_last_ms < s_min_lap_time_ms) {
        /* debounce do mesmo cruzamento */
        s_prev_lat = s_state.lat; s_prev_lon = s_state.lon;
        s_prev_us = now_us; s_prev_valid = true;
        return;
    }

    uint32_t lap_time_ms = (uint32_t)since_last_ms;
    int32_t  delta_ms = (s_best_lap_ms > 0) ? (int32_t)lap_time_ms - (int32_t)s_best_lap_ms : 0;
    bool     is_new_best = (s_best_lap_ms == 0 || lap_time_ms < s_best_lap_ms);

    /* Distancia total dessa volta (antes do reset abaixo zerar
     * s_cur_lap_dist_m) - vira a referencia dos setores automaticos.
     * Fixa na primeira volta e reatualiza a cada novo recorde, pra os
     * limiares acompanharem a melhor trajetoria. */
    float closed_lap_dist_m = s_cur_lap_dist_m;

    if (is_new_best) s_best_lap_ms = lap_time_ms;
    s_last_delta_ms = delta_ms;
    s_lap_number++;
    s_last_cross_us = t_cross;

    /* estatisticas da sessao (media/consistencia no popup de encerramento) */
    s_stat_lap_count++;
    s_stat_sum_ms   += lap_time_ms;
    s_stat_sumsq_ms += (uint64_t)lap_time_ms * (uint64_t)lap_time_ms;

    /* Volta que acabou de fechar virou a nova referencia do delta ao
     * vivo? Copia o trace dela pra s_best_trace. Se nao (nao bateu o
     * recorde), s_best_trace continua intacto - referencia so muda
     * quando alguem bate ela de verdade. De qualquer forma, a volta que
     * comeca agora precisa de trace zerado. */
    if (is_new_best && s_cur_trace && s_best_trace) {
        memcpy(s_best_trace, s_cur_trace, sizeof(lap_trace_point_t) * (size_t)s_cur_trace_count);
        s_best_trace_count = s_cur_trace_count;
    }
    lap_trace_reset_current();

    /* Volta ideal - precisa rodar ANTES do memset abaixo, que apaga os
     * splits/cruzamentos dessa volta que acabou de fechar. */
    update_best_segments(lap_time_ms);

    /* reseta estado de setor pra proxima volta */
    memset(s_sector_crossed,  0, sizeof(s_sector_crossed));
    memset(s_sector_split_ms, 0, sizeof(s_sector_split_ms));
    memset(s_sector_delta_valid, 0, sizeof(s_sector_delta_valid));

    /* Setores automaticos: fixa a referencia de distancia na 1a volta e a
     * cada novo recorde, e (re)gera os limiares. build_auto_sectors() ja
     * respeita "so se nao houver setor manual". */
    if (closed_lap_dist_m > 1.0f && (is_new_best || s_ref_lap_dist_m < 1.0f)) {
        s_ref_lap_dist_m = closed_lap_dist_m;
        build_auto_sectors();
    }

    app_event_t evt = {
        .type = APP_EVT_LAP_COMPLETE,
        .source = EVT_SRC_INTERNAL,
    };
    evt.data.lap.lap_number  = s_lap_number;
    evt.data.lap.lap_time_ms = lap_time_ms;
    evt.data.lap.delta_ms    = delta_ms;
    evt.data.lap.is_new_best = is_new_best;
    app_event_post_data(&evt);

    ESP_LOGI(TAG, "Volta %u: %u ms (delta %d ms)%s",
             (unsigned)s_lap_number, (unsigned)lap_time_ms, (int)delta_ms,
             is_new_best ? " - novo best" : "");

    s_prev_lat = s_state.lat; s_prev_lon = s_state.lon;
    s_prev_us = now_us; s_prev_valid = true;
}

/* ----------------------------------------------------------------------
 * Auto-sessao - chamada a cada amostra RMC valida (10Hz), FORA do mutex
 * de estado (recebe snapshot dos campos que precisa). So DETECTA e posta
 * o evento; quem inicia/encerra de verdade e' o dispatcher do main.c,
 * pelo mesmo caminho do botao RESET (guardas de linha-de-chegada, USB,
 * etc. valem identicas). Trava de um-disparo (s_auto_*_fired) evita
 * repostar a 10Hz enquanto o main ainda nao processou o evento.
 * ---------------------------------------------------------------------- */
static void auto_session_check(int64_t now_us, float speed_kmh, bool recording, bool finish_set)
{
    if (!s_auto_session_enabled) {
        s_auto_start_since_us = 0; s_auto_start_fired = false;
        s_auto_stop_since_us  = 0; s_auto_stop_fired  = false;
        return;
    }

    if (!recording) {
        s_auto_stop_since_us = 0;
        s_auto_stop_fired    = false;
        if (finish_set && speed_kmh >= AUTO_SESSION_START_KMH) {
            if (s_auto_start_since_us == 0) s_auto_start_since_us = now_us;
            if (!s_auto_start_fired &&
                (now_us - s_auto_start_since_us) >= (int64_t)AUTO_SESSION_START_HOLD_MS * 1000) {
                s_auto_start_fired = true;
                app_event_post(APP_EVT_AUTO_SESSION_START, EVT_SRC_INTERNAL);
                ESP_LOGI(TAG, "Auto-sessao: movimento sustentado - pedindo inicio de sessao");
            }
        } else {
            s_auto_start_since_us = 0;
            s_auto_start_fired    = false;
        }
    } else {
        s_auto_start_since_us = 0;
        s_auto_start_fired    = false;
        if (speed_kmh <= AUTO_SESSION_STOP_KMH) {
            if (s_auto_stop_since_us == 0) s_auto_stop_since_us = now_us;
            if (!s_auto_stop_fired &&
                (now_us - s_auto_stop_since_us) >= (int64_t)AUTO_SESSION_STOP_HOLD_MS * 1000) {
                s_auto_stop_fired = true;
                app_event_post(APP_EVT_AUTO_SESSION_STOP, EVT_SRC_INTERNAL);
                ESP_LOGI(TAG, "Auto-sessao: parado ha %ds - pedindo encerramento",
                          AUTO_SESSION_STOP_HOLD_MS / 1000);
            }
        } else {
            s_auto_stop_since_us = 0;
            s_auto_stop_fired    = false;
        }
    }
}

/* ----------------------------------------------------------------------
 * Parsing de sentencas
 * ---------------------------------------------------------------------- */
/* BUG REAL corrigido (relatado como "LED do modulo pisca positioning
 * success mas a tela fica em GPS 0"): com fix e o kart PARADO, o u-blox
 * manda o campo COG (curso) VAZIO. O strtok_r antigo colapsava ",," e a
 * DATA escorregava pro lugar do curso -> strlen(f_date) < 6 -> a amostra
 * COM FIX VALIDO era descartada, uma por uma, enquanto o equipamento
 * estivesse parado. Andando (COG preenchido) tudo funcionava - por isso
 * as sessoes na pista gravavam normal e o teste em bancada nunca fixava.
 * Campos RMC: 0=$GxRMC 1=hora 2=status 3=lat 4=N/S 5=lon 6=E/W
 * 7=sog(nos) 8=cog(graus) 9=data. */
static void parse_rmc(char *line)
{
    char *f[16];
    int nf = nmea_split(line, f, 16);
    if (nf < 10) return;
    const char *f_time   = f[1];
    const char *f_status = f[2];
    const char *f_lat    = f[3];
    const char *f_ns     = f[4];
    const char *f_lon    = f[5];
    const char *f_ew     = f[6];
    const char *f_speed  = f[7];
    const char *f_course = f[8];
    const char *f_date   = f[9];

    if (f_status[0] != 'A') {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state.fix_valid = false;
        xSemaphoreGive(s_state_mutex);
        return;
    }
    if (strlen(f_time) < 6 || strlen(f_date) < 6) return;
    if (!f_lat[0] || !f_ns[0] || !f_lon[0] || !f_ew[0]) return;

    double raw_lat = atof(f_lat);
    double raw_lon = atof(f_lon);
    float  speed_kmh = (float)(atof(f_speed) * 1.852); /* vazio -> atof("")=0 */
    /* COG vazio = parado (GPS nao sabe pra onde o kart "aponta" sem se
     * mover). Mantem o ultimo heading conhecido em vez de forcar 0
     * (=norte), senao o filtro de direcao do gate compararia contra um
     * rumo inventado. */
    bool  course_present = (f_course[0] != '\0');
    float course = course_present ? (float)atof(f_course) : 0.0f;

    struct tm utc_tm = {0};
    utc_tm.tm_hour = (f_time[0] - '0') * 10 + (f_time[1] - '0');
    utc_tm.tm_min  = (f_time[2] - '0') * 10 + (f_time[3] - '0');
    utc_tm.tm_sec  = (f_time[4] - '0') * 10 + (f_time[5] - '0');
    utc_tm.tm_mday = (f_date[0] - '0') * 10 + (f_date[1] - '0');
    utc_tm.tm_mon  = (f_date[2] - '0') * 10 + (f_date[3] - '0') - 1;
    utc_tm.tm_year = 100 + (f_date[4] - '0') * 10 + (f_date[5] - '0');

    int64_t now_us = esp_timer_get_time();

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.fix_valid    = true;
    s_state.lat          = nmea_coord_to_decimal(raw_lat, f_ns[0]);
    s_state.lon          = nmea_coord_to_decimal(raw_lon, f_ew[0]);
    s_state.speed_kmh    = speed_kmh;
    if (course_present) s_state.heading_deg = course; /* parado: mantem o ultimo rumo valido */
    s_last_utc_tm         = utc_tm;
    s_has_datetime        = true;

    process_gate_timing(now_us);

    /* Mesmo calculo que gps_get_latest() faz pra UI - precisa estar
     * tambem em s_state porque a entrada do log copia s_state direto
     * (sem isso aqui, a coluna lap_time_ms do CSV saia sempre zero). */
    bool counting = s_finish_line_set && s_recording && !(s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement);
    s_state.lap_time_ms = counting ? (uint32_t)((now_us - s_last_cross_us) / 1000) : 0;

    /* pico de velocidade da sessao (pro popup de estatisticas) */
    if (s_recording && speed_kmh > s_stat_max_speed) s_stat_max_speed = speed_kmh;

    gps_log_entry_t entry;
    bool should_log = s_recording && s_sd_log_queue;
    if (should_log) {
        entry.sample = s_state;
        entry.sample.lap_number    = s_lap_number;
        entry.sample.best_lap_ms   = s_best_lap_ms;
        entry.sample.last_delta_ms = s_last_delta_ms;
        /* splits da volta em andamento - vivem em statics, nao em s_state
         * (s_state so ganha eles em gps_get_latest). Sem essa copia as
         * colunas s1_ms/s2_ms do CSV sairiam sempre zero. */
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            entry.sample.sector_split_ms[i] = s_sector_split_ms[i];
        }
        entry.timestamp_us = now_us;
    }
    /* snapshot pro auto_session_check rodar fora do mutex */
    bool auto_recording  = s_recording;
    bool auto_finish_set = s_finish_line_set;
    xSemaphoreGive(s_state_mutex);

    if (should_log) {
        /* Nao bloqueia o GPS task se o logger de SD atrasar - perder uma
         * amostra ocasional e melhor que travar a leitura do GPS. */
        xQueueSend(s_sd_log_queue, &entry, 0);
    }

    auto_session_check(now_us, speed_kmh, auto_recording, auto_finish_set);
}

/* Campos GGA: 0=id 1=hora 2=lat 3=N/S 4=lon 5=E/W 6=qualidade 7=numSV.
 * Mesmo bug de strtok_r do RMC: sem fix a GGA vem quase toda vazia
 * ("$GNGGA,,,,,,0,00,...") e o colapso de ",," jogava qualidade/numSV
 * em campos errados -> a funcao retornava cedo e "satellites" nunca era
 * atualizado (por isso sats_usados ficava 0 pra sempre no diagnostico
 * enquanto nao houvesse fix). */
static void parse_gga(char *line)
{
    char *f[12];
    int nf = nmea_split(line, f, 12);
    if (nf < 8) return;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state.satellites = (uint8_t)atoi(f[7]);
    if (f[6][0] == '0' || f[6][0] == '\0') s_state.fix_valid = false;
    xSemaphoreGive(s_state_mutex);
}

/* $GxGSV - satelites em vista. So alimenta o diagnostico de RF (contagem
 * por constelacao + melhor SNR); nada aqui entra no timing/fix.
 * Split MANUAL (nao strtok_r): GSV sem fix vem com elev/az VAZIOS
 * ("17,,,27") e strtok_r colapsa ",," num delimitador so, desalinhando
 * os grupos prn/elev/az/cno - o SNR cairia na posicao errada. */
static void parse_gsv(const char *line)
{
    char talker = line[2]; /* $GPGSV -> 'P', GL->'L', GA->'A', GB->'B' */

    const char *f[32];
    int nf = 0;
    const char *p = line;
    while (nf < 32) {
        f[nf++] = p;
        const char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    if (nf < 4) return;

    uint8_t num = (uint8_t)atoi(f[3]); /* f: 0=$GxGSV 1=total_msgs 2=idx 3=sats_em_vista */
    int ci; /* indice da constelacao em s_dbg_trk/s_gsv_trk_tmp */
    switch (talker) {
    case 'P': s_dbg_sv_gps = num; ci = 0; break;
    case 'L': s_dbg_sv_glo = num; ci = 1; s_dbg_glgsv_seen = true; break;
    case 'A': s_dbg_sv_gal = num; ci = 2; break;
    case 'B': s_dbg_sv_bds = num; ci = 3; break;
    default: return;
    }

    uint8_t msg_total = (uint8_t)atoi(f[1]);
    uint8_t msg_idx   = (uint8_t)atoi(f[2]);
    if (msg_idx <= 1) s_gsv_trk_tmp[ci] = 0; /* ciclo novo do talker */

    /* grupos de 4 (prn,elev,az,cno) a partir de f[4]; cno = f[7], f[11]...
     * cno VAZIO = modulo sabe do satelite mas nao ouve ele (o campo so
     * vem preenchido com sinal real). atoi("") = 0, entao cno>0 = ouvindo. */
    for (int i = 7; i < nf; i += 4) {
        uint8_t cno = (uint8_t)atoi(f[i]);
        if (cno > 0) {
            s_gsv_trk_tmp[ci]++;
            if (cno > s_dbg_best_snr) s_dbg_best_snr = cno;
        }
    }

    if (msg_idx >= msg_total) s_dbg_trk[ci] = s_gsv_trk_tmp[ci]; /* ciclo completo - comita */
}

static void gps_handle_line(char *line, size_t len)
{
    strncpy(s_dbg_last_line, line, sizeof(s_dbg_last_line) - 1);
    s_dbg_last_line[sizeof(s_dbg_last_line) - 1] = '\0';

    if (!nmea_checksum_ok(line, len)) {
        s_dbg_lines_bad++;
        ESP_LOGD(TAG, "Checksum invalido, sentenca descartada: %s", line);
        return;
    }
    s_dbg_lines_ok++;

    char *star = strchr(line, '*');
    if (star) *star = '\0';

    if (strstr(line, "RMC")) {
        parse_rmc(line);
    } else if (strstr(line, "GGA")) {
        parse_gga(line);
    } else if (strstr(line, "GSV")) {
        parse_gsv(line);
    }
}

/* ----------------------------------------------------------------------
 * UART + task
 * ---------------------------------------------------------------------- */
static void uart_init_gps(void)
{
    const uart_config_t cfg = {
        .baud_rate  = GPS_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, GPS_RX_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

/* Liga GLONASS via UBX-CFG-VALSET, camada RAM (nao persiste no modulo -
 * reaplicado a cada boot pelo proprio firmware).
 *
 * MOTIVO (diagnostico de campo): dentro da caixa do KartBox existe
 * interferencia de banda estreita em cima de 1575.42 MHz - GPS, Galileo
 * e QZSS (todos em L1) ficam SURDOS enquanto BeiDou (1561 MHz) segue
 * ouvindo. Harmonica de algum clock da placa (DSI/PSRAM/DC-DC). GLONASS
 * opera em 1598-1606 MHz, tambem FORA da frequencia suja - BDS+GLO
 * juntos dao satelites suficientes pra fix mesmo com o L1 comprometido.
 * O M10 suporta 4 constelacoes simultaneas (GPS+GAL+BDS+GLO e' valido).
 *
 * Camada RAM de proposito: o mesmo modulo e' usado no drone do usuario
 * (sem jammer) - la ele volta ao default de fabrica ao ligar.
 * Keys: CFG-SIGNAL-GLO_ENA=0x10310025, CFG-SIGNAL-GLO_L1_ENA=0x10310018
 * (u-blox M10 interface description). */
static void gps_send_enable_glonass(void)
{
    /* QZSS desligado JUNTO: o M10 processa no maximo 4 constelacoes
     * simultaneas - GPS+GAL+BDS+QZSS ja ocupava tudo, e pedir GLONASS
     * por cima fazia o modulo rejeitar (NAK) o VALSET INTEIRO (visto em
     * campo: nenhum $GLGSV aparecia). QZSS e' regional do Japao, zero
     * utilidade no Brasil - sai ele, entra GLONASS. */
    uint8_t frame[] = {
        0xB5, 0x62,                         /* sync UBX */
        0x06, 0x8A,                         /* classe/ID: CFG-VALSET */
        0x13, 0x00,                         /* payload len = 19 (LE) */
        0x00,                               /* version 0 */
        0x01,                               /* layers: so RAM */
        0x00, 0x00,                         /* reservado */
        0x24, 0x00, 0x31, 0x10, 0x00,       /* CFG-SIGNAL-QZSS_ENA   = 0 (libera a 4a vaga) */
        0x25, 0x00, 0x31, 0x10, 0x01,       /* CFG-SIGNAL-GLO_ENA    = 1 (key U4 LE + val L) */
        0x18, 0x00, 0x31, 0x10, 0x01,       /* CFG-SIGNAL-GLO_L1_ENA = 1 */
        0x00, 0x00                          /* checksum Fletcher (preenchido abaixo) */
    };
    uint8_t ck_a = 0, ck_b = 0;
    for (size_t i = 2; i < sizeof(frame) - 2; i++) {
        ck_a = (uint8_t)(ck_a + frame[i]);
        ck_b = (uint8_t)(ck_b + ck_a);
    }
    frame[sizeof(frame) - 2] = ck_a;
    frame[sizeof(frame) - 1] = ck_b;
    uart_write_bytes(GPS_UART_NUM, (const char *)frame, sizeof(frame));
    ESP_LOGI(TAG, "GLONASS habilitado via UBX (RAM) - contorno pro jammer de 1575MHz da caixa");
}

static void gps_task(void *arg)
{
    (void)arg;
    bool glo_cfg_sent = false;
    uint8_t chunk[GPS_UART_CHUNK_SIZE];
    static char line[GPS_LINE_BUF_SIZE]; /* static: nao usa stack da task pra isso */
    size_t line_len = 0;

    for (;;) {
        int n = uart_read_bytes(GPS_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(200));

        /* Resumo periodico de diagnostico - roda mesmo com n<=0 (chunk
         * vazio) pra deixar claro quando o problema e "zero byte chega",
         * nao so "sentenca invalida". */
        int64_t now_ms = esp_timer_get_time() / 1000;

        /* 3s depois do boot o modulo com certeza ja esta de pe' pra
         * receber configuracao - liga GLONASS (uma vez por boot). */
        if (!glo_cfg_sent && now_ms > 3000) {
            glo_cfg_sent = true;
            gps_send_enable_glonass();
        }
        if (now_ms - s_dbg_last_log_ms >= GPS_DEBUG_LOG_PERIOD_MS) {
            uint8_t sats = 0;
            bool fix = false;
            if (s_state_mutex) {
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                sats = s_state.satellites;
                fix  = s_state.fix_valid;
                xSemaphoreGive(s_state_mutex);
            }
            /* LOGI (nao LOGD): o nivel default do projeto e' INFO, entao
             * em LOGD esse resumo NUNCA aparecia no monitor - e ele e'
             * exatamente o que diferencia "GPS mudo" (0 bytes) de "GPS
             * falando mas sem fix" (linhas ok, fix=0) num probe de campo. */
            ESP_LOGI(TAG, "UART: %lu bytes/%ds, %lu linhas ok, %lu invalidas | fix=%d sats_usados=%u | "
                          "EM VISTA gps=%u glo=%u gal=%u bds=%u | OUVINDO gps=%u glo=%u gal=%u bds=%u snr_max=%u | ultima: %s",
                     (unsigned long)s_dbg_bytes_rx, (int)(GPS_DEBUG_LOG_PERIOD_MS / 1000),
                     (unsigned long)s_dbg_lines_ok, (unsigned long)s_dbg_lines_bad,
                     (int)fix, (unsigned)sats,
                     (unsigned)s_dbg_sv_gps, (unsigned)s_dbg_sv_glo,
                     (unsigned)s_dbg_sv_gal, (unsigned)s_dbg_sv_bds,
                     (unsigned)s_dbg_trk[0], (unsigned)s_dbg_trk[1],
                     (unsigned)s_dbg_trk[2], (unsigned)s_dbg_trk[3],
                     (unsigned)s_dbg_best_snr,
                     s_dbg_last_line[0] ? s_dbg_last_line : "(nenhuma ainda)");
            if (glo_cfg_sent && !s_dbg_glgsv_seen) {
                ESP_LOGW(TAG, "GLONASS: nenhum $GLGSV visto ainda - o VALSET pode ter sido rejeitado pelo modulo");
            }
            s_dbg_bytes_rx = 0;
            s_dbg_lines_ok = 0;
            s_dbg_lines_bad = 0;
            s_dbg_best_snr = 0; /* SNR e' "melhor do periodo", zera; em-vista fica (e' estado, nao contador) */
            s_dbg_last_log_ms = now_ms;
        }

        if (n <= 0) continue;

        s_last_uart_rx_ms = esp_timer_get_time() / 1000;
        s_dbg_bytes_rx += (uint32_t)n;

        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (b == '\n' || b == '\r') {
                if (line_len > 0) {
                    line[line_len] = '\0';
                    gps_handle_line(line, line_len);
                    line_len = 0;
                }
                continue;
            }
            if (line_len < GPS_LINE_BUF_SIZE - 1) {
                line[line_len++] = (char)b;
            } else {
                line_len = 0; /* linha absurdamente longa - descarta, provavel lixo */
            }
        }
    }
}

/* ----------------------------------------------------------------------
 * API publica
 * ---------------------------------------------------------------------- */
void gps_init(QueueHandle_t sd_log_queue)
{
    s_sd_log_queue = sd_log_queue;
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Falha ao criar mutex de estado");
        abort();
    }
    memset(&s_state, 0, sizeof(s_state));

    /* Buffers do delta ao vivo em PSRAM (32MB sobrando) - RAM interna e'
     * escassa nesse projeto (ver historico de crash por falta dela com
     * BT/WiFi ligados). Se a alocacao falhar por algum motivo, feature
     * so fica desligada (s_cur_trace/s_best_trace ficam NULL, todo o
     * codigo que usa eles ja checa antes) - nao trava o resto do GPS. */
    s_cur_trace  = heap_caps_malloc(sizeof(lap_trace_point_t) * LAP_TRACE_MAX_SAMPLES, MALLOC_CAP_SPIRAM);
    s_best_trace = heap_caps_malloc(sizeof(lap_trace_point_t) * LAP_TRACE_MAX_SAMPLES, MALLOC_CAP_SPIRAM);
    if (!s_cur_trace || !s_best_trace) {
        ESP_LOGW(TAG, "Falha ao alocar buffer de trace em PSRAM - delta ao vivo desativado");
    }

    uart_init_gps();
    xTaskCreate(gps_task, "gps_task", 4096, NULL, 8, NULL);
    ESP_LOGI(TAG, "GPS pronto (UART%d, %d baud)", GPS_UART_NUM, GPS_BAUD_RATE);
}

void gps_set_log_queue(QueueHandle_t sd_log_queue)
{
    s_sd_log_queue = sd_log_queue;
}

bool gps_inject_agps(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return false;
    /* UBX cru direto na UART do modulo (AssistNow = stream de mensagens
     * UBX-MGA-*). Em blocos com pausa: o buffer de RX do MAX-M10 e'
     * pequeno e a UART nao tem flow control - despejar dezenas de KB de
     * uma vez estoura o buffer e o modulo descarta mensagens no meio,
     * silenciosamente. 512B + 15ms fica bem abaixo do limite a 115200. */
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > 512 ? 512 : remaining;
        int w = uart_write_bytes(GPS_UART_NUM, (const char *)p, chunk);
        if (w <= 0) {
            ESP_LOGE(TAG, "A-GPS: uart_write_bytes falhou no meio da injecao");
            return false;
        }
        p += w;
        remaining -= (size_t)w;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    ESP_LOGI(TAG, "A-GPS: %u bytes UBX injetados no modulo", (unsigned)len);
    return true;
}

bool gps_set_finish_line(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool ok = s_state.fix_valid;
    if (ok) {
        s_finish_lat       = s_state.lat;
        s_finish_lon       = s_state.lon;
        s_finish_heading   = s_state.heading_deg;
        s_finish_line_set  = true;
        s_last_cross_us    = esp_timer_get_time();
        s_waiting_for_movement = (s_mode == GPS_MODE_CORRIDA);
        /* nova referencia de gate = trace/distancia da volta atual nao
         * fazem mais sentido (e a "melhor volta" registrada era contra
         * um gate diferente) */
        s_best_trace_count = 0;
        lap_trace_reset_current();
        ESP_LOGI(TAG, "Linha de chegada marcada (%.6f, %.6f) heading=%.1f",
                 s_finish_lat, s_finish_lon, (double)s_finish_heading);
    } else {
        ESP_LOGW(TAG, "Sem fix valido - linha de chegada nao marcada");
    }
    xSemaphoreGive(s_state_mutex);
    return ok;
}

void gps_load_finish_line(double lat, double lon, float heading_deg)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_finish_lat      = lat;
    s_finish_lon      = lon;
    s_finish_heading  = heading_deg;
    s_finish_line_set = true;
    /* Nao reinicia s_last_cross_us - o timer de volta ja esta rodando;
     * isso e um restore de config, nao um cruzamento de gate. */
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Linha de chegada restaurada (%.6f, %.6f) heading=%.1f",
             lat, lon, (double)heading_deg);
}

bool gps_get_finish_line(double *lat, double *lon, float *heading_deg)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool set = s_finish_line_set;
    if (set) {
        if (lat)         *lat         = s_finish_lat;
        if (lon)         *lon         = s_finish_lon;
        if (heading_deg) *heading_deg = s_finish_heading;
    }
    xSemaphoreGive(s_state_mutex);
    return set;
}

void gps_set_mode(gps_race_mode_t mode)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_mode = mode;
    xSemaphoreGive(s_state_mutex);
}

void gps_session_reset(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_lap_number = 0;
    s_best_lap_ms = 0;
    s_last_delta_ms = 0;
    s_last_cross_us = esp_timer_get_time();
    s_waiting_for_movement = (s_mode == GPS_MODE_CORRIDA) && s_finish_line_set;
    /* zera historico de setores da sessao anterior (pontos de setor MANUAIS
     * sao mantidos; os automaticos sao recalculados do zero na sessao nova) */
    memset(s_sector_crossed,  0, sizeof(s_sector_crossed));
    memset(s_sector_split_ms, 0, sizeof(s_sector_split_ms));
    memset(s_sector_delta_valid, 0, sizeof(s_sector_delta_valid));
    memset(s_best_sector_ms,  0, sizeof(s_best_sector_ms));
    memset(s_best_seg_ms,     0, sizeof(s_best_seg_ms)); /* volta ideal tambem e' por sessao */
    /* Setores automaticos sao por-sessao: some a referencia e os setores
     * gerados. Se havia setor MANUAL, ele permanece (s_sector_set[i] com
     * auto desligado). */
    if (s_auto_sectors_active) {
        s_auto_sectors_active = false;
        memset(s_sector_set, 0, sizeof(s_sector_set));
    }
    s_ref_lap_dist_m = 0.0f;
    /* sessao nova = referencia de delta ao vivo tambem zera, senao ia
     * comparar contra volta de uma sessao anterior (pista/dia diferente) */
    s_best_trace_count = 0;
    lap_trace_reset_current();
    /* estatisticas sao por-sessao */
    s_stat_lap_count = 0;
    s_stat_sum_ms    = 0;
    s_stat_sumsq_ms  = 0;
    s_stat_max_speed = 0.0f;
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Sessao resetada (linha de chegada e setores mantidos se ja marcados)");
}

void gps_session_set_recording(bool recording)
{
    s_recording = recording;
    /* transicao manual (botao RESET) tambem zera os timers da auto-sessao,
     * senao um "parado ha 2min" acumulado antes do start manual encerraria
     * a sessao nova na hora. */
    s_auto_start_since_us = 0; s_auto_start_fired = false;
    s_auto_stop_since_us  = 0; s_auto_stop_fired  = false;
}

void gps_set_auto_session(bool enabled)
{
    s_auto_session_enabled = enabled;
    ESP_LOGI(TAG, "Auto-sessao %s", enabled ? "ligada" : "desligada");
}

void gps_get_session_stats(gps_session_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    out->lap_count     = s_stat_lap_count;
    out->best_ms       = s_best_lap_ms;
    out->max_speed_kmh = s_stat_max_speed;
    if (s_stat_lap_count > 0) {
        uint64_t avg = s_stat_sum_ms / s_stat_lap_count;
        out->avg_ms = (uint32_t)avg;
        /* variancia = E[x^2] - E[x]^2 (tudo em ms^2); clamp em 0 contra
         * erro de arredondamento inteiro quando as voltas sao identicas. */
        uint64_t ex2 = s_stat_sumsq_ms / s_stat_lap_count;
        uint64_t var = (ex2 > avg * avg) ? (ex2 - avg * avg) : 0;
        out->stddev_ms = (uint32_t)sqrtf((float)var);
    }
    xSemaphoreGive(s_state_mutex);
}

void gps_get_rf_diag(gps_rf_diag_t *out)
{
    if (!out) return;
    /* Contadores uint8 escritos pela gps_task sem lock - leitura
     * "rasgada" aqui e' inofensiva (diagnostico visual, valores mudam
     * pouco entre amostras); nao vale segurar o mutex de estado. */
    out->in_view[0] = s_dbg_sv_gps;
    out->in_view[1] = s_dbg_sv_glo;
    out->in_view[2] = s_dbg_sv_gal;
    out->in_view[3] = s_dbg_sv_bds;
    for (int i = 0; i < 4; i++) out->tracked[i] = s_dbg_trk[i];
    out->best_snr = s_dbg_best_snr;
}

void gps_set_utc_offset_min(int16_t offset_min)
{
    s_utc_offset_min = offset_min;
}

void gps_set_gate_radius_m(float meters)
{
    if (meters > 0.0f) s_gate_radius_m = meters;
}

void gps_set_min_lap_time_ms(uint32_t ms)
{
    s_min_lap_time_ms = ms;
}

void gps_get_latest(gps_sample_t *out)
{
    /* UI cria seu timer de refresh (100ms) antes do gps_init() rodar no
     * app_main() — a primeira ou segunda chamada desse timer pode chegar
     * aqui com a mutex ainda nao criada. Sem essa guarda, xSemaphoreTake
     * com handle NULL crasha (assert em queue.c). */
    if (!s_state_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_state;
    out->lap_number    = s_lap_number;
    out->best_lap_ms   = s_best_lap_ms;
    out->last_delta_ms = s_last_delta_ms;
    out->live_delta_ms    = s_live_delta_ms;
    out->live_delta_valid = s_live_delta_valid;

    bool counting = s_finish_line_set && s_recording && !(s_mode == GPS_MODE_CORRIDA && s_waiting_for_movement);
    out->lap_time_ms = counting ? (uint32_t)((esp_timer_get_time() - s_last_cross_us) / 1000) : 0;

    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        out->sector_is_set[i]      = s_sector_set[i];
        out->sector_split_ms[i]    = s_sector_split_ms[i];
        out->best_sector_ms[i]     = s_best_sector_ms[i];
        out->sector_delta_ms[i]    = s_sector_delta_ms[i];
        out->sector_delta_valid[i] = s_sector_delta_valid[i];
    }
    out->sectors_auto = s_auto_sectors_active;
    out->ideal_lap_ms = compute_ideal_lap_ms();
    xSemaphoreGive(s_state_mutex);
}

gps_link_status_t gps_get_link_status(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_uart_rx_ms) > GPS_LINK_TIMEOUT_MS) {
        return GPS_LINK_ERROR;
    }
    if (!s_state_mutex) return GPS_LINK_ERROR; /* task de GPS ainda nao subiu */

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool fixed = s_state.fix_valid;
    xSemaphoreGive(s_state_mutex);

    return fixed ? GPS_LINK_FIXED : GPS_LINK_SEARCHING;
}

bool gps_set_sector_point(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool ok = s_state.fix_valid;
    if (ok) {
        /* Marcar setor MANUAL desliga o modo automatico e limpa os
         * setores auto que estavam ativos (all-or-nothing). */
        if (s_auto_sectors_active) {
            s_auto_sectors_active = false;
            for (int i = 0; i < GPS_MAX_SECTORS; i++) {
                s_sector_set[i]      = false;
                s_sector_crossed[i]  = false;
                s_sector_split_ms[i] = 0;
                s_best_sector_ms[i]  = 0;
            }
        }
        s_sector_lat[idx]     = s_state.lat;
        s_sector_lon[idx]     = s_state.lon;
        s_sector_heading[idx] = s_state.heading_deg;
        s_sector_set[idx]     = true;
        memset(s_best_seg_ms, 0, sizeof(s_best_seg_ms)); /* mapeamento de segmentos mudou */
        ESP_LOGI(TAG, "Setor %d marcado (%.6f, %.6f) heading=%.1f",
                 idx + 1, s_sector_lat[idx], s_sector_lon[idx], (double)s_sector_heading[idx]);
    } else {
        ESP_LOGW(TAG, "Sem fix GPS valido - setor %d nao marcado", idx + 1);
    }
    xSemaphoreGive(s_state_mutex);
    return ok;
}

void gps_load_sector(int idx, double lat, double lon, float heading_deg)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* Setor manual carregado (restore de pista) tem prioridade sobre auto. */
    if (s_auto_sectors_active) {
        s_auto_sectors_active = false;
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            s_sector_set[i]      = false;
            s_sector_crossed[i]  = false;
            s_sector_split_ms[i] = 0;
            s_best_sector_ms[i]  = 0;
        }
    }
    s_sector_lat[idx]     = lat;
    s_sector_lon[idx]     = lon;
    s_sector_heading[idx] = heading_deg;
    s_sector_set[idx]     = true;
    memset(s_best_seg_ms, 0, sizeof(s_best_seg_ms)); /* mapeamento de segmentos mudou */
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Setor %d restaurado (%.6f, %.6f)", idx + 1, lat, lon);
}

void gps_clear_sector_point(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_auto_sectors_active) {
        /* Em modo automatico nao ha setor manual individual pra remover -
         * desliga o auto por inteiro (rebuild acontece na proxima volta se
         * continuar sem manual). */
        s_auto_sectors_active = false;
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            s_sector_set[i]      = false;
            s_sector_crossed[i]  = false;
            s_sector_split_ms[i] = 0;
            s_best_sector_ms[i]  = 0;
        }
        s_ref_lap_dist_m = 0.0f;
    } else {
        s_sector_set[idx]        = false;
        s_sector_crossed[idx]    = false;
        s_sector_split_ms[idx]   = 0;
        s_best_sector_ms[idx]    = 0;
    }
    /* remover um setor muda o numero/mapeamento de segmentos da volta
     * ideal (ver update_best_segments) - os antigos s_best_seg_ms[] nao
     * correspondem mais a nada, zera tudo em vez de tentar remapear. */
    memset(s_best_seg_ms, 0, sizeof(s_best_seg_ms));
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Setor %d removido", idx + 1);
}

bool gps_get_sector_point(int idx, double *lat, double *lon, float *heading_deg)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* Setor automatico nao tem ponto geografico - retorna false pra nao
     * ser persistido em NVS/pista como se fosse manual. */
    bool set = s_sector_set[idx] && !s_auto_sectors_active;
    if (set) {
        if (lat)         *lat         = s_sector_lat[idx];
        if (lon)         *lon         = s_sector_lon[idx];
        if (heading_deg) *heading_deg = s_sector_heading[idx];
    }
    xSemaphoreGive(s_state_mutex);
    return set;
}

bool gps_sector_is_set(int idx)
{
    if (idx < 0 || idx >= GPS_MAX_SECTORS) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    /* So MANUAL conta aqui (usado pela aba Config/persistencia). Setores
     * automaticos aparecem no visor via gps_get_latest().sector_is_set. */
    bool set = s_sector_set[idx] && !s_auto_sectors_active;
    xSemaphoreGive(s_state_mutex);
    return set;
}

bool gps_get_local_datetime(struct tm *out)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool has = s_has_datetime;
    struct tm utc_copy = s_last_utc_tm;
    xSemaphoreGive(s_state_mutex);

    if (!has) return false;

    time_t utc_epoch   = my_timegm(&utc_copy);
    time_t local_epoch = utc_epoch + (int64_t)s_utc_offset_min * 60;
    gmtime_r(&local_epoch, out);
    return true;
}
