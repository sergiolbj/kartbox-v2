/*
 * test_pure.c - testes automaticos das funcoes puras (main/kb_pure.h).
 * Roda no PC, sem placa. Ver tests/LEIA-ME.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../main/kb_pure.h"

static int g_total = 0, g_fail = 0;

#define CHECK(cond) do { g_total++; if (!(cond)) { g_fail++; \
    printf("  FALHOU (linha %d): %s\n", __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { g_total++; double _a=(a), _b=(b); \
    if (fabs(_a-_b) > (tol)) { g_fail++; \
    printf("  FALHOU (linha %d): %s=%.6f esperado %.6f (tol %.6f)\n", __LINE__, #a, _a, _b, (double)(tol)); } } while (0)

static void group(const char *n) { printf("[%s]\n", n); }

/* ---------------- NMEA ---------------- */
static void test_nmea(void)
{
    group("NMEA checksum / coordenadas / split");
    const char *ok = "$GNRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,,,A*5A";
    /* calcula o checksum correto na hora pra nao depender de memoria */
    char buf[128]; snprintf(buf, sizeof buf, "$GPGGA,1,2,3");
    uint8_t x = 0; for (const char *p = buf + 1; *p; p++) x ^= (uint8_t)*p;
    char line[160]; snprintf(line, sizeof line, "%s*%02X", buf, x);
    CHECK(kb_nmea_checksum_ok(line, strlen(line)));
    snprintf(line, sizeof line, "%s*%02x", buf, x);               /* hex minusculo */
    CHECK(kb_nmea_checksum_ok(line, strlen(line)));
    snprintf(line, sizeof line, "%s*%02X", buf, (unsigned)(x ^ 1)); /* 1 bit errado */
    CHECK(!kb_nmea_checksum_ok(line, strlen(line)));
    CHECK(!kb_nmea_checksum_ok("GPGGA,1,2,3*00", 14));            /* sem $ */
    CHECK(!kb_nmea_checksum_ok("$GPGGA,1,2,3", 12));              /* sem * */
    CHECK(!kb_nmea_checksum_ok("$GPGGA,1,2,3*0", 14));            /* truncado */
    CHECK(!kb_nmea_checksum_ok("$A*ZZ", 5));                      /* hex invalido */
    CHECK(!kb_nmea_checksum_ok("", 0));
    (void)ok;

    CHECK_NEAR(kb_nmea_coord_to_decimal(4807.038, 'N'), 48.1173, 1e-6);
    CHECK_NEAR(kb_nmea_coord_to_decimal(4807.038, 'S'), -48.1173, 1e-6);
    CHECK_NEAR(kb_nmea_coord_to_decimal(1131.000, 'E'), 11.516666667, 1e-6);   /* dddmm.mmm */
    CHECK_NEAR(kb_nmea_coord_to_decimal(1131.000, 'W'), -11.516666667, 1e-6);
    CHECK_NEAR(kb_nmea_coord_to_decimal(2330.0, 'S'), -23.5, 1e-9);

    /* campo vazio preservado: COG vazio nao pode deslocar a data */
    char rmc[] = "$GNRMC,123519.00,A,4807.038,N,01131.000,E,000.0,,230394,,,A";
    char *f[16];
    int n = kb_nmea_split(rmc, f, 16);
    CHECK(n == 13);
    CHECK(strcmp(f[1], "123519.00") == 0);
    CHECK(f[8][0] == '\0');                 /* COG vazio */
    CHECK(strcmp(f[9], "230394") == 0);     /* data continua no lugar certo */
    char lim[] = "a,b,c,d,e";
    CHECK(kb_nmea_split(lim, f, 3) == 3);   /* respeita maxf */
}

/* ---------------- geometria ---------------- */
static void test_geo(void)
{
    group("Geometria");
    CHECK_NEAR(kb_distance_m(-23.5, -46.6, -23.5, -46.6), 0.0, 1e-3);
    CHECK_NEAR(kb_distance_m(0.0, 0.0, 0.001, 0.0), 111.19, 0.5);       /* 0.001 grau lat ~ 111 m */
    CHECK_NEAR(kb_distance_m(-23.5, -46.6, -23.5, -46.601), 102.0, 1.0); /* lon encolhe com cos(lat) */

    CHECK_NEAR(kb_heading_diff(10, 350), 20, 1e-4);
    CHECK_NEAR(kb_heading_diff(350, 10), 20, 1e-4);
    CHECK_NEAR(kb_heading_diff(90, 270), 180, 1e-4);
    CHECK_NEAR(kb_heading_diff(0, 0), 0, 1e-6);
    CHECK_NEAR(kb_heading_diff(359, 1), 2, 1e-4);

    /* gate virado pra Leste (90): quem esta a oeste esta "antes" (negativo) */
    CHECK(kb_signed_gate_dist(-23.5, -46.6001, -23.5, -46.6, 90.0f) < 0);
    CHECK(kb_signed_gate_dist(-23.5, -46.5999, -23.5, -46.6, 90.0f) > 0);
    CHECK_NEAR(kb_signed_gate_dist(-23.5, -46.6, -23.5, -46.6, 90.0f), 0.0, 1e-3);
    /* gate virado pra Norte (0) */
    CHECK(kb_signed_gate_dist(-23.5001, -46.6, -23.5, -46.6, 0.0f) < 0);
    CHECK(kb_signed_gate_dist(-23.4999, -46.6, -23.5, -46.6, 0.0f) > 0);
}

/* Constroi pontos sobre uma reta Oeste->Leste a velocidade v (m/s) */
#define LAT0 (-23.5)
#define LON0 (-46.6)
static double lon_at_east_m(double east_m)
{
    return LON0 + east_m / (111194.9266 * cos(LAT0 * M_PI / 180.0));
}

static void test_gate(void)
{
    group("Cruzamento da linha (interpolacao)");
    float frac = -1;
    /* amostras a -4 m e +6 m (kart a 100 m/s fictício): cruza em 40% */
    bool r = kb_gate_cross_frac(LAT0, lon_at_east_m(-4), LAT0, lon_at_east_m(6),
                                LAT0, LON0, 90.0f, 15.0f, &frac);
    CHECK(r);
    CHECK_NEAR(frac, 0.4, 0.01);

    /* ainda nao cruzou */
    CHECK(!kb_gate_cross_frac(LAT0, lon_at_east_m(-6), LAT0, lon_at_east_m(-1),
                              LAT0, LON0, 90.0f, 15.0f, &frac));
    /* ja passou (as duas depois) */
    CHECK(!kb_gate_cross_frac(LAT0, lon_at_east_m(1), LAT0, lon_at_east_m(6),
                              LAT0, LON0, 90.0f, 15.0f, &frac));
    /* sentido contrario nao conta */
    CHECK(!kb_gate_cross_frac(LAT0, lon_at_east_m(6), LAT0, lon_at_east_m(-4),
                              LAT0, LON0, 90.0f, 15.0f, &frac));
    /* amostra exatamente na linha conta (d_cur >= 0) e frac = 1 */
    CHECK(kb_gate_cross_frac(LAT0, lon_at_east_m(-5), LAT0, lon_at_east_m(0),
                             LAT0, LON0, 90.0f, 15.0f, &frac));
    CHECK_NEAR(frac, 1.0, 0.01);

    /* cruza a 10 m lateral do ponto do gate: dentro de raio 15, fora de raio 5 */
    double dlat10 = 10.0 / 111194.9266;
    CHECK(kb_gate_cross_frac(LAT0 + dlat10, lon_at_east_m(-3), LAT0 + dlat10, lon_at_east_m(3),
                             LAT0, LON0, 90.0f, 15.0f, &frac));
    CHECK(!kb_gate_cross_frac(LAT0 + dlat10, lon_at_east_m(-3), LAT0 + dlat10, lon_at_east_m(3),
                              LAT0, LON0, 90.0f, 5.0f, &frac));
    /* frac NULL nao quebra */
    CHECK(kb_gate_cross_frac(LAT0, lon_at_east_m(-1), LAT0, lon_at_east_m(1),
                             LAT0, LON0, 90.0f, 15.0f, NULL));
}

/* ---------------- tempo UTC ---------------- */
static void test_utc(void)
{
    group("Hora UTC do GPS");
    int64_t us; bool fr;
    /* yy so vai de 00 a 99 => anos 2000..2099 */
    CHECK(kb_utc_to_us("000000.00", "010100", &us, &fr));
    CHECK(us == 946684800LL * 1000000LL);                       /* 2000-01-01 00:00:00 UTC */
    CHECK(fr);
    CHECK(kb_utc_to_us("123519.50", "150626", &us, &fr));
    /* 2026-06-15 12:35:19.5 UTC = 1781526919.5 */
    CHECK(us == 1781526919500000LL);
    CHECK(kb_utc_to_us("123519", "150626", &us, &fr));
    CHECK(!fr);                                                  /* sem casas decimais */
    CHECK(us == 1781526919000000LL);
    CHECK(kb_utc_to_us("123519.123456", "150626", &us, &fr));
    CHECK(us == 1781526919123456LL);
    CHECK(kb_utc_to_us("123519.1", "150626", &us, &fr));
    CHECK(us == 1781526919100000LL);

    /* virada de dia / mes / ano: continua monotonico e com passo de 100 ms */
    int64_t a, b;
    kb_utc_to_us("235959.90", "311225", &a, &fr);               /* 31/12/2025 -> 01/01/2026 */
    kb_utc_to_us("000000.00", "010126", &b, &fr);
    CHECK(b - a == 100000);
    kb_utc_to_us("235959.90", "280200", &a, &fr);               /* 2000 e' bissexto: 29/02 existe */
    kb_utc_to_us("000000.00", "290200", &b, &fr);
    CHECK(b - a == 100000);
    kb_utc_to_us("235959.90", "280201", &a, &fr);               /* 2001 nao e' bissexto */
    kb_utc_to_us("000000.00", "010301", &b, &fr);
    CHECK(b - a == 100000);

    /* entradas ruins */
    CHECK(!kb_utc_to_us("", "150626", &us, &fr));
    CHECK(!kb_utc_to_us("12351", "150626", &us, &fr));
    CHECK(!kb_utc_to_us("12x519", "150626", &us, &fr));
    CHECK(!kb_utc_to_us("253519", "150626", &us, &fr));         /* hora 25 */
    CHECK(!kb_utc_to_us("126019", "150626", &us, &fr));         /* min 60 */
    CHECK(!kb_utc_to_us("123519", "321326", &us, &fr));         /* dia 32 */
    CHECK(!kb_utc_to_us("123519", "151326", &us, &fr));         /* mes 13 */
    CHECK(!kb_utc_to_us(NULL, "150626", &us, &fr));
}

/* ---------------- relogio / precisao da volta ---------------- */
static uint32_t rng = 12345;
static double frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

static void test_clock_sync(void)
{
    group("Sincronizacao do relogio (anti-jitter)");
    kb_clock_sync_t s = {0};

    /* 1) sem jitter: tempo corrigido anda exatamente igual ao do GPS */
    int64_t gps = 1000000000000LL, local = 5000000, prev = 0;
    for (int i = 0; i < 100; i++) {
        int64_t c = kb_clock_sync(&s, local, gps);
        if (i > 0) CHECK(c - prev == 100000);
        prev = c; gps += 100000; local += 100000;
    }

    /* 2) com jitter de 0..40 ms: tempo corrigido tem variacao MUITO menor que o cru */
    memset(&s, 0, sizeof s);
    gps = 1000000000000LL; local = 5000000;
    double raw_min = 1e18, raw_max = -1e18, cor_min = 1e18, cor_max = -1e18;
    int64_t last = 0; bool back = false;
    for (int i = 0; i < 1200; i++) {                    /* 120 s a 10 Hz */
        int64_t jit = (int64_t)(frand() * 40000.0);     /* 0..40 ms de atraso */
        int64_t arrive = local + jit;
        int64_t c = kb_clock_sync(&s, arrive, gps);
        if (i >= 100) {                                 /* depois de convergir */
            double raw_err = (double)(arrive - local);              /* erro do carimbo cru */
            double cor_err = (double)(c - local);                   /* erro do corrigido */
            if (raw_err < raw_min) raw_min = raw_err;
            if (raw_err > raw_max) raw_max = raw_err;
            if (cor_err < cor_min) cor_min = cor_err;
            if (cor_err > cor_max) cor_max = cor_err;
        }
        if (i > 0 && c <= last) back = true;
        last = c; gps += 100000; local += 100000;
    }
    double raw_spread = (raw_max - raw_min) / 1000.0, cor_spread = (cor_max - cor_min) / 1000.0;
    printf("  jitter cru: %.1f ms | jitter corrigido: %.2f ms\n", raw_spread, cor_spread);
    CHECK(raw_spread > 30.0);       /* o jitter simulado existe */
    CHECK(cor_spread < 3.0);        /* e foi praticamente eliminado */
    CHECK(!back);                   /* tempo corrigido nunca anda pra tras */

    /* 3) deriva de cristal (50 ppm): corrigido acompanha, sem estourar */
    memset(&s, 0, sizeof s);
    gps = 1000000000000LL; double localf = 5000000.0;
    double worst = 0;
    for (int i = 0; i < 6000; i++) {                    /* 10 min */
        int64_t jit = (int64_t)(frand() * 30000.0);
        int64_t arrive = (int64_t)localf + jit;
        int64_t c = kb_clock_sync(&s, arrive, gps);
        if (i > 300) { double e = fabs((double)(c - (int64_t)localf)); if (e > worst) worst = e; }
        gps += 100000; localf += 100000.0 * (1.0 + 50e-6);
    }
    printf("  pior desvio com deriva 50ppm: %.2f ms\n", worst / 1000.0);
    CHECK(worst / 1000.0 < 40.0);

    /* 4) rajada atrasada no inicio (primeira amostra chega 300 ms atrasada):
     *    converge sem andar pra tras */
    memset(&s, 0, sizeof s);
    gps = 1000000000000LL; local = 5000000; last = 0; back = false;
    for (int i = 0; i < 100; i++) {
        int64_t arrive = local + (i == 0 ? 300000 : 5000);
        int64_t c = kb_clock_sync(&s, arrive, gps);
        if (i > 0 && c <= last) back = true;
        last = c; gps += 100000; local += 100000;
    }
    CHECK(!back);

    /* 5) salto grande (> 2 s): recomeca sem travar */
    int64_t c1 = kb_clock_sync(&s, local, gps);
    int64_t c2 = kb_clock_sync(&s, local + 100000, gps + 100000 + 10000000LL);
    CHECK(c2 > c1);
}

/* Simula 2 passagens pela linha, 60.000 s reais de diferenca, com jitter
 * de chegada. Compara tempo de volta: carimbo cru x carimbo corrigido. */
static double lap_error_ms(bool use_sync, double v_mps, double jitter_ms, uint32_t seed)
{
    rng = seed;
    kb_clock_sync_t s = {0};
    const double lap_true_s = 60.0;
    const double period = 0.1;                 /* 10 Hz */
    int64_t gps0 = 1000000000000LL, local0 = 7000000;

    double cross_t[2]; int n_cross = 0;
    double prev_e = 0, prev_t = 0; bool have_prev = false;
    /* kart passa por east=0 em t=25.0123 s e t=85.0123 s (fase arbitraria) */
    double t_first = 25.0123;        /* GPS ja ligado ha 25 s (aquecimento) */
    for (int i = 0; i < 1100 && n_cross < 2; i++) {
        double t = i * period;                                  /* tempo real da medicao */
        double e = (t < 55.0) ? (t - t_first) * v_mps            /* 1a passagem */
                              : (t - (t_first + lap_true_s)) * v_mps; /* 2a passagem */
        if (t >= 55.0 && t - period < 55.0) have_prev = false;   /* corte de trajeto entre voltas */
        int64_t gps_us = gps0 + (int64_t)llround(t * 1e6);
        int64_t local_true = local0 + (int64_t)llround(t * 1e6);
        int64_t arrive = local_true + (int64_t)(frand() * jitter_ms * 1000.0);
        double stamp_us = use_sync ? (double)kb_clock_sync(&s, arrive, gps_us) : (double)arrive;
        double stamp = stamp_us / 1e6;
        if (have_prev) {
            float fr;
            if (kb_gate_cross_frac(LAT0, lon_at_east_m(prev_e), LAT0, lon_at_east_m(e),
                                   LAT0, LON0, 90.0f, 20.0f, &fr)) {
                cross_t[n_cross++] = prev_t + fr * (stamp - prev_t);
            }
        }
        prev_e = e; prev_t = stamp; have_prev = true;
    }
    if (n_cross < 2) return 1e9;
    return ((cross_t[1] - cross_t[0]) - lap_true_s) * 1000.0;
}

static void test_lap_precision(void)
{
    group("Precisao do tempo de volta (60.000 s reais)");
    double worst_raw = 0, worst_sync = 0;
    for (uint32_t seed = 1; seed <= 200; seed++) {
        double er = fabs(lap_error_ms(false, 18.0, 40.0, seed * 7919u));
        double es = fabs(lap_error_ms(true,  18.0, 40.0, seed * 7919u));
        if (er > worst_raw) worst_raw = er;
        if (es > worst_sync) worst_sync = es;
    }
    printf("  pior erro em 200 voltas (jitter 40 ms): cru %.1f ms | corrigido %.1f ms\n",
           worst_raw, worst_sync);
    CHECK(worst_raw > 15.0);          /* sem a correcao o erro e' grande */
    CHECK(worst_sync < 4.0);          /* com a correcao fica em poucos ms */
    CHECK(worst_sync < worst_raw);

    /* sem jitter nenhum: tem que dar quase exato (erro so da interpolacao linear) */
    double e0 = fabs(lap_error_ms(true, 18.0, 0.0, 99));
    printf("  sem jitter: erro %.3f ms\n", e0);
    CHECK(e0 < 1.0);
}

/* ---------------- nome de pista ---------------- */
static void test_names(void)
{
    group("Nome de pista");
    const size_t MAX = 32;   /* TRACK_NAME_MAX */
    CHECK(kb_name_check("Interlagos", MAX) == KB_NAME_OK);
    CHECK(kb_name_check("Pista A", MAX) == KB_NAME_OK);          /* espaco no meio ok */
    CHECK(kb_name_check("Kartodromo-2 (azul)", MAX) == KB_NAME_OK);
    CHECK(kb_name_check("pista.v2", MAX) == KB_NAME_OK);         /* ponto no meio ok */
    CHECK(kb_name_check("Sao Joao da Boa Vista!", MAX) == KB_NAME_OK);

    CHECK(kb_name_check(NULL, MAX) == KB_NAME_EMPTY);
    CHECK(kb_name_check("", MAX) == KB_NAME_EMPTY);

    const char *bad[] = { ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b",
                          "a<b", "a>b", "a|b", "abc ", "abc.", "a\tb", "a\nb", "../x" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        g_total++;
        if (kb_name_check(bad[i], MAX) != KB_NAME_BAD) {
            g_fail++; printf("  FALHOU: \"%s\" devia ser invalido\n", bad[i]);
        }
    }

    char n31[32], n32[33], n40[41];
    memset(n31, 'a', 31); n31[31] = 0;
    memset(n32, 'a', 32); n32[32] = 0;
    memset(n40, 'a', 40); n40[40] = 0;
    CHECK(kb_name_check(n31, MAX) == KB_NAME_OK);                /* 31 letras + NUL cabe */
    CHECK(kb_name_check(n32, MAX) == KB_NAME_TOO_LONG);
    CHECK(kb_name_check(n40, MAX) == KB_NAME_TOO_LONG);
    /* buffer cheio sem NUL dentro do limite: nao pode ler alem */
    char nonul[32]; memset(nonul, 'b', 32);
    CHECK(kb_name_check(nonul, MAX) == KB_NAME_TOO_LONG);
}

/* ---------------- rotulo de sessao ---------------- */
static void test_session_label(void)
{
    group("Rotulo de sessao (aba VOLTAS)");
    char b[64];
    kb_session_label("20260704_143000.csv", b, sizeof b);
    CHECK(strcmp(b, "04/07/26 14:30") == 0);
    kb_session_label("20260704_143059_Interlagos.csv", b, sizeof b);
    CHECK(strcmp(b, "04/07/26 14:30 Interlagos") == 0);
    kb_session_label("20261231_235959_Kart_Sao_Paulo.csv", b, sizeof b);
    CHECK(strcmp(b, "31/12/26 23:59 Kart Sao Paulo") == 0);
    kb_session_label("20260704_143000", b, sizeof b);                 /* sem .csv */
    CHECK(strcmp(b, "04/07/26 14:30") == 0);
    kb_session_label("sessao_12345_Pista.csv", b, sizeof b);          /* sem data de GPS */
    CHECK(strcmp(b, "sessao 12345 Pista") == 0);
    kb_session_label("20261304_143000.csv", b, sizeof b);             /* mes 13: nao e' data */
    CHECK(strcmp(b, "20261304 143000") == 0);
    kb_session_label("20260704_253000.csv", b, sizeof b);             /* hora 25 */
    CHECK(strcmp(b, "20260704 253000") == 0);
    kb_session_label("2026070x_143000.csv", b, sizeof b);
    CHECK(strcmp(b, "2026070x 143000") == 0);
    kb_session_label("", b, sizeof b);
    CHECK(strcmp(b, "") == 0);
    kb_session_label(NULL, b, sizeof b);
    CHECK(strcmp(b, "") == 0);

    /* buffer pequeno: nunca estoura e sempre termina em NUL */
    char guard[16]; memset(guard, 'X', sizeof guard);
    kb_session_label("20260704_143059_Interlagos.csv", guard, 8);
    CHECK(strcmp(guard, "04/07/2") == 0);
    CHECK(guard[8] == 'X');                                            /* nao escreveu alem */
    kb_session_label("abc.csv", guard, 1);
    CHECK(guard[0] == '\0');
    kb_session_label("abc.csv", NULL, 5);                              /* nao quebra */
    CHECK(1);
}

/* ---------------------------------------------------------------------- */
static void test_agps_helpers(void)
{
    printf("[agps]\n");
    char h[64];
    const uint8_t b[] = { 0xB5, 0x62, 0x00, 0x0F, 0xA5 };
    CHECK(kb_hex_encode(b, 5, h, sizeof h) == 10);
    CHECK(strcmp(h, "B562000FA5") == 0);
    CHECK(kb_hex_encode(b, 5, h, 10) == 0 && h[0] == '\0');     /* nao cabe (precisa 11) */
    CHECK(kb_hex_encode(b, 5, h, 11) == 10);
    CHECK(kb_hex_encode(b, 0, h, sizeof h) == 0 && h[0] == '\0');

    char v[64];
    const char *j = "{\"chipcode\": \"QkRC+/=\" ,\"serviceUrl\":\"https://x/y\",\"allowedData\":[\"ualm\"]}";
    CHECK(kb_json_str(j, "chipcode", v, sizeof v) && strcmp(v, "QkRC+/=") == 0);
    CHECK(kb_json_str(j, "serviceUrl", v, sizeof v) && strcmp(v, "https://x/y") == 0);
    CHECK(!kb_json_str(j, "nada", v, sizeof v));
    CHECK(!kb_json_str(j, "allowedData", v, sizeof v));              /* nao e' string */
    CHECK(!kb_json_str(j, "chipcode", v, 4) && v[0] == '\0');        /* nao coube */
    CHECK(!kb_json_str("{\"chipcode\":\"abc", "chipcode", v, sizeof v)); /* sem aspas final */
    CHECK(!kb_json_str(NULL, "k", v, sizeof v));
    CHECK(!kb_json_str("{\"k\":1}", "k", v, sizeof v));

    char u[64];
    CHECK(kb_url_encode("AbC-1_.~", u, sizeof u) && strcmp(u, "AbC-1_.~") == 0);
    CHECK(kb_url_encode("a+b/c=", u, sizeof u) && strcmp(u, "a%2Bb%2Fc%3D") == 0);
    CHECK(!kb_url_encode("a+b", u, 4) && u[0] == '\0');              /* "a%2B" nao cabe em 4 */
    CHECK(kb_url_encode("a+b", u, 6) && strcmp(u, "a%2Bb") == 0);
}

static void ubx_make(uint8_t *f, size_t *n, uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t len)
{
    f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = (uint8_t)(len & 0xFF); f[5] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) f[6 + i] = pl[i];
    uint8_t a = 0, c = 0;
    for (uint16_t i = 2; i < (uint16_t)(len + 6); i++) { a = (uint8_t)(a + f[i]); c = (uint8_t)(c + a); }
    f[6 + len] = a; f[7 + len] = c;
    *n = (size_t)len + 8;
}

static int ubx_feed_all(kb_ubx_parser_t *p, const uint8_t *d, size_t n, uint8_t *got_cls, uint8_t *got_id)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        if (kb_ubx_feed(p, d[i])) { frames++; *got_cls = p->buf[2]; *got_id = p->buf[3]; }
    }
    return frames;
}

static void test_ubx(void)
{
    printf("[ubx]\n");
    kb_ubx_parser_t p; memset(&p, 0, sizeof p);
    uint8_t f[600]; size_t n;
    uint8_t cls = 0, id = 0;

    /* polls oficiais da Thingstream: checksum tem que bater com os da doc */
    const uint8_t poll_uniqid[] = { 0xB5, 0x62, 0x27, 0x03, 0x00, 0x00, 0x2A, 0xA5 };
    const uint8_t poll_monver[] = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };
    CHECK(ubx_feed_all(&p, poll_uniqid, sizeof poll_uniqid, &cls, &id) == 1 && cls == 0x27 && id == 0x03);
    CHECK(ubx_feed_all(&p, poll_monver, sizeof poll_monver, &cls, &id) == 1 && cls == 0x0A && id == 0x04);

    /* UNIQID: 9 bytes de payload */
    const uint8_t uid[9] = { 1, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55 };
    ubx_make(f, &n, 0x27, 0x03, uid, 9);
    CHECK(n == 17);
    CHECK(ubx_feed_all(&p, f, n, &cls, &id) == 1 && cls == 0x27 && id == 0x03);
    CHECK(p.n == 17 && p.buf[10] == 0x11);          /* frame inteiro disponivel */

    /* NMEA no meio + UBX + NMEA: so o UBX conta */
    uint8_t mix[200]; size_t m = 0;
    const char *nm = "$GNGGA,,,,,,0,00,99.99,,,,,,*56\r\n";
    memcpy(mix + m, nm, strlen(nm)); m += strlen(nm);
    memcpy(mix + m, f, n); m += n;
    memcpy(mix + m, nm, strlen(nm)); m += strlen(nm);
    CHECK(ubx_feed_all(&p, mix, m, &cls, &id) == 1);

    /* checksum errado: descartado */
    uint8_t bad[32]; memcpy(bad, f, n); bad[n - 1] ^= 0xFF;
    CHECK(ubx_feed_all(&p, bad, n, &cls, &id) == 0);
    /* e depois um frame bom volta a funcionar */
    CHECK(ubx_feed_all(&p, f, n, &cls, &id) == 1);

    /* lixo antes do header e B5 duplo */
    uint8_t junk[40]; size_t jn = 0;
    junk[jn++] = 0x00; junk[jn++] = 0xB5; junk[jn++] = 0xB5; /* B5 B5 62... deve resincronizar */
    memcpy(junk + jn, f + 1, n - 1); jn += n - 1;          /* f+1 comeca em 62 */
    CHECK(ubx_feed_all(&p, junk, jn, &cls, &id) == 1);

    /* payload grande (MON-VER tipico ~330 bytes) passa; acima do limite e' ignorado */
    uint8_t big[400]; for (int i = 0; i < 400; i++) big[i] = (uint8_t)(i * 7);
    ubx_make(f, &n, 0x0A, 0x04, big, 330);
    CHECK(ubx_feed_all(&p, f, n, &cls, &id) == 1 && cls == 0x0A && p.n == 338);
    uint8_t huge[600]; for (int i = 0; i < 600; i++) huge[i] = 0x55;
    ubx_make(f, &n, 0x0A, 0x04, huge, 560);
    CHECK(ubx_feed_all(&p, f, n, &cls, &id) == 0);

    /* dois frames colados */
    uint8_t two[64]; size_t tn = 0;
    ubx_make(f, &n, 0x27, 0x03, uid, 9); memcpy(two, f, n); tn += n;
    ubx_make(f, &n, 0x0A, 0x04, uid, 9); memcpy(two + tn, f, n); tn += n;
    CHECK(ubx_feed_all(&p, two, tn, &cls, &id) == 2 && cls == 0x0A);

    /* ponta a ponta: frame -> hex continuo (formato do ZTP) */
    char hx[64];
    ubx_make(f, &n, 0x27, 0x03, uid, 9);
    CHECK(kb_hex_encode(f, n, hx, sizeof hx) == 34);
    CHECK(strncmp(hx, "B5622703090001000000112233445", 29) == 0);
}

static void test_fs_path(void)
{
    printf("[fs path]\n");
    CHECK(kb_fs_path_ok("/", 200));
    CHECK(kb_fs_path_ok("/sessions", 200));
    CHECK(kb_fs_path_ok("/sessions/", 200));
    CHECK(kb_fs_path_ok("/a/b/c.csv", 200));
    CHECK(kb_fs_path_ok("/pista final.trk", 200));
    CHECK(kb_fs_path_ok("/.hidden", 200));
    CHECK(kb_fs_path_ok("/a..b", 200));
    CHECK(!kb_fs_path_ok("", 200));
    CHECK(!kb_fs_path_ok(NULL, 200));
    CHECK(!kb_fs_path_ok("sessions", 200));
    CHECK(!kb_fs_path_ok("/..", 200));
    CHECK(!kb_fs_path_ok("/../etc", 200));
    CHECK(!kb_fs_path_ok("/a/../b", 200));
    CHECK(!kb_fs_path_ok("/a/..", 200));
    CHECK(!kb_fs_path_ok("/./a", 200));
    CHECK(!kb_fs_path_ok("/a//b", 200));
    CHECK(!kb_fs_path_ok("//a", 200));
    CHECK(!kb_fs_path_ok("/a\\b", 200));
    CHECK(!kb_fs_path_ok("/a\"b", 200));
    CHECK(!kb_fs_path_ok("/a%2e%2e", 200));
    CHECK(!kb_fs_path_ok("/a\nb", 200));
    CHECK(!kb_fs_path_ok("/abcdef", 7));      /* len 7 >= max */
    CHECK(kb_fs_path_ok("/abcde", 7));
    char b[32];
    CHECK(strcmp(kb_fs_basename("/a/b/c.csv", b, sizeof b), "c.csv") == 0);
    CHECK(strcmp(kb_fs_basename("/a/b/", b, sizeof b), "b") == 0);
    CHECK(strcmp(kb_fs_basename("/x\"y", b, sizeof b), "x_y") == 0);
    CHECK(strcmp(kb_fs_basename("/", b, sizeof b), "") == 0);
}

static void test_valget(void)
{
    printf("[valget]\n");
    uint8_t f[64], pl[40]; size_t n;
    /* polls de referencia: mesmo frame do ubx_make */
    uint8_t ref[16]; size_t rn;
    ubx_make(ref, &rn, 0x0A, 0x04, NULL, 0);
    n = kb_ubx_build(f, sizeof f, 0x0A, 0x04, NULL, 0);
    CHECK(n == 8 && rn == 8 && memcmp(f, ref, 8) == 0);
    CHECK(f[6] == 0x0E && f[7] == 0x34);                 /* MON-VER poll da doc */
    CHECK(kb_ubx_build(f, 8, 0x06, 0x8B, pl, 4) == 0);   /* nao cabe */

    /* resposta: version=1 layer=0 pos=0, GPS(1f)=1, SBAS(20)=1, GAL(21)=0, GLO(25)=1 */
    size_t o = 0;
    pl[o++] = 1; pl[o++] = 0; pl[o++] = 0; pl[o++] = 0;
    const uint32_t keys[] = { 0x1031001F, 0x10310020, 0x10310021, 0x10310025 };
    const uint8_t vals[]  = { 1, 1, 0, 1 };
    for (int i = 0; i < 4; i++) {
        pl[o++] = (uint8_t)keys[i]; pl[o++] = (uint8_t)(keys[i] >> 8);
        pl[o++] = (uint8_t)(keys[i] >> 16); pl[o++] = (uint8_t)(keys[i] >> 24);
        pl[o++] = vals[i];
    }
    n = kb_ubx_build(f, sizeof f, 0x06, 0x8B, pl, (uint16_t)o);
    uint8_t v = 9;
    CHECK(kb_ubx_valget_u8(f, n, 0x1031001F, &v) && v == 1);
    CHECK(kb_ubx_valget_u8(f, n, 0x10310021, &v) && v == 0);
    CHECK(kb_ubx_valget_u8(f, n, 0x10310025, &v) && v == 1);
    CHECK(!kb_ubx_valget_u8(f, n, 0x10310022, &v));      /* chave ausente */
    CHECK(!kb_ubx_valget_u8(f, n - 1, 0x10310025, &v));  /* truncado */
    /* outra mensagem (ACK) nao e' VALGET */
    n = kb_ubx_build(f, sizeof f, 0x05, 0x01, pl, 2);
    CHECK(!kb_ubx_valget_u8(f, n, 0x1031001F, &v));
    CHECK(!kb_ubx_valget_u8(NULL, 0, 1, &v));
    /* chave com tamanho U2 (0x30...) no meio: pula 2 bytes e acha a seguinte */
    o = 0; pl[o++] = 1; pl[o++] = 0; pl[o++] = 0; pl[o++] = 0;
    pl[o++] = 0x01; pl[o++] = 0x00; pl[o++] = 0x21; pl[o++] = 0x30; pl[o++] = 0xAA; pl[o++] = 0xBB;
    pl[o++] = 0x25; pl[o++] = 0x00; pl[o++] = 0x31; pl[o++] = 0x10; pl[o++] = 1;
    n = kb_ubx_build(f, sizeof f, 0x06, 0x8B, pl, (uint16_t)o);
    CHECK(kb_ubx_valget_u8(f, n, 0x10310025, &v) && v == 1);
}

int main(void)
{
    printf("== testes das funcoes puras do KartBox ==\n");
    test_nmea();
    test_geo();
    test_gate();
    test_utc();
    test_clock_sync();
    test_lap_precision();
    test_names();
    test_session_label();
    test_agps_helpers();
    test_ubx();
    test_fs_path();
    test_valget();
    printf("\n%d verificacoes, %d falhas -> %s\n", g_total, g_fail, g_fail ? "FALHOU" : "TUDO OK");
    return g_fail ? 1 : 0;
}
