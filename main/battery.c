/*
 * battery.c - ver battery.h pro contexto de hardware.
 *
 * Leitura por ADC oneshot (ADC2_CH4 = GPIO53) com calibracao por
 * curve-fitting (esquema do ESP32-P4). ADC2 e' livre no P4 porque o WiFi
 * nao usa ADC interno (radio fica no ESP32-C6 via esp-hosted), ao contrario
 * do ESP32/S2 classico onde ADC2 disputa com o WiFi.
 */
#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

/* ---- Hardware / calibracao ------------------------------------------- */
#define BATT_ADC_UNIT      ADC_UNIT_2
#define BATT_ADC_CHANNEL   ADC_CHANNEL_4      /* GPIO53 = ADC2_CH4 */
#define BATT_ADC_ATTEN     ADC_ATTEN_DB_12    /* ~0..3.1V; pino chega a ~2.5V @4.2V */
#define BATT_ADC_BITWIDTH  ADC_BITWIDTH_DEFAULT

/* Divisor R52(68k)/R57(100k): Vbat = Vpino * (68+100)/100 = Vpino * 1.68 */
#define BATT_DIVIDER       (1.68f)

#define BATT_SAMPLES       (16)     /* media por leitura pra tirar ruido */
#define BATT_EMA_ALPHA     (0.20f)  /* EMA rapida = valor exibido (0..1) */

/* Heuristica de carga (sem pino de status no hardware): mantemos uma EMA
 * LENTA (baseline) alem da rapida. Numa subida sustentada de tensao a rapida
 * "adianta" a lenta por uma margem pequena mas persistente; numa descarga,
 * fica atras. Compara-se as duas com histerese. Detecta a TENDENCIA - nao o
 * plato de bateria cheia (a rapida alcanca a lenta e a margem some). */
#define BATT_SLOW_ALPHA    (0.02f)  /* EMA lenta = baseline de tendencia */
#define CHG_MARGIN_V       (0.010f) /* margem rapida-vs-lenta pra decidir */

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t          s_cali = NULL;
static bool  s_ready = false;
static bool  s_cali_ok = false;

static float s_ema_v   = -1.0f;   /* EMA rapida = tensao exibida (-1 = sem leitura) */
static float s_slow_v  = -1.0f;   /* EMA lenta = baseline de tendencia */
static bool  s_charging = false;

/* Curva de descarga Li-ion 1S (tensao de REPOUSO -> %). Sob carga a tensao
 * afunda, entao o valor exibido tende a subestimar rodando - ok pra um
 * indicador de status bar. Ordem decrescente. */
typedef struct { float v; int pct; } batt_point_t;
static const batt_point_t k_curve[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80},
    {3.98f, 75},  {3.95f, 70}, {3.91f, 65}, {3.87f, 60}, {3.85f, 55},
    {3.84f, 50},  {3.82f, 45}, {3.80f, 40}, {3.79f, 35}, {3.77f, 30},
    {3.75f, 25},  {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},
    {3.50f, 2},   {3.30f, 0},
};
#define K_CURVE_N (sizeof(k_curve) / sizeof(k_curve[0]))

static int volts_to_pct(float v)
{
    if (v >= k_curve[0].v) return 100;
    if (v <= k_curve[K_CURVE_N - 1].v) return 0;
    for (size_t i = 0; i < K_CURVE_N - 1; i++) {
        float vhi = k_curve[i].v, vlo = k_curve[i + 1].v;
        if (v <= vhi && v >= vlo) {
            float t = (v - vlo) / (vhi - vlo);   /* 0..1 */
            int phi = k_curve[i].pct, plo = k_curve[i + 1].pct;
            return plo + (int)((phi - plo) * t + 0.5f);
        }
    }
    return 0;
}

void battery_init(void)
{
    if (s_ready) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit falhou: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_channel falhou: %s", esp_err_to_name(err));
        return;
    }

    /* Calibracao por curve fitting (ESP32-P4). Se falhar, caímos no
     * fallback linear (raw -> mV) em battery_read_mv(). */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATT_ADC_UNIT,
        .chan = BATT_ADC_CHANNEL,
        .atten = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "cali curve-fitting indisponivel, usando fallback linear");
    }

    s_ready = true;
    ESP_LOGI(TAG, "bateria pronta (GPIO53/ADC2_CH4, cali=%d)", (int)s_cali_ok);
}

/* Le a tensao NO PINO em mV (media de BATT_SAMPLES). <0 em erro. */
static int battery_read_pin_mv(void)
{
    if (!s_ready) return -1;

    long acc = 0;
    int  ok = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        int mv;
        if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            acc += mv;
        } else {
            /* Fallback: 12 bits (0..4095) mapeados a ~0..3100mV da atten 12dB. */
            acc += (long)(raw * 3100 / 4095);
        }
        ok++;
    }
    if (ok == 0) return -1;
    return (int)(acc / ok);
}

/* Faz uma leitura, atualiza EMA e a heuristica de carga. */
static void battery_sample(void)
{
    int pin_mv = battery_read_pin_mv();
    if (pin_mv < 0) return;

    float vbat = (pin_mv / 1000.0f) * BATT_DIVIDER;

    if (s_ema_v < 0.0f) {                               /* primeira leitura */
        s_ema_v = vbat;
        s_slow_v = vbat;
        return;
    }
    s_ema_v  = s_ema_v  + BATT_EMA_ALPHA  * (vbat - s_ema_v);
    s_slow_v = s_slow_v + BATT_SLOW_ALPHA * (vbat - s_slow_v);

    /* Rapida acima da lenta = tensao em tendencia de subida = carregando.
     * Histerese pra nao ficar chaveando no ruido. */
    if (s_ema_v - s_slow_v >  CHG_MARGIN_V)      s_charging = true;
    else if (s_slow_v - s_ema_v > CHG_MARGIN_V)  s_charging = false;
}

int battery_get_percent(void)
{
    if (!s_ready) return -1;
    battery_sample();
    if (s_ema_v < 0.0f) return -1;
    return volts_to_pct(s_ema_v);
}

float battery_get_voltage(void)
{
    if (!s_ready || s_ema_v < 0.0f) return 0.0f;
    return s_ema_v;
}

bool battery_is_charging(void)
{
    return s_charging;
}
