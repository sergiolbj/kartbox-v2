/*
 * ui.c - kartbox v2
 *
 * Implementa o ui.h. Estilo GT3 que fechamos: barra de LED segmentada
 * pro delta (reaproveita a linguagem visual do shift-light de RPM, que
 * kart nao tem, pra mostrar ganho/perda de tempo), velocidade central
 * gigante, grid denso nas laterais, tudo em tela sem aba escondida.
 *
 * Refresh: 1 timer LVGL a 100ms (10Hz) le gps_get_latest() e atualiza
 * tudo que muda em tempo real. UI NAO consome a fila de eventos pra
 * dado continuo - so usa app_event_post() pra MANDAR eventos (toque
 * em botao). Ver gps.h pra explicacao de por que essa troca de fila
 * por polling faz sentido aqui.
 */
#include "ui.h"
#include "fonts.h"
#include "config.h"
#include "settings.h"
#include "app_events.h"
#include "gps.h"
#include "sd_logger.h"
#include "ble_telemetry.h"
#include "battery.h"
#include "display_init.h"

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"

/* ----------------------------------------------------------------------
 * Paleta - preto puro de fundo, verde de marca pra "bom", vermelho pra
 * "ruim", cinza pra info secundaria. Sem gradiente forte, sem app generico
 * - continua visual de instrumento, so que com um pouco mais de
 * profundidade/hierarquia (aprovado com o usuario via preview visual):
 * cards ganham uma superficie quase-preta em vez de so borda fina, cantos
 * levemente arredondados, segundo tom de cinza pra hierarquia de texto, e
 * acentos verde/dourado nas bordas das celulas. LED bar e fonte gigante
 * do velocimetro continuam 100% inalterados de proposito - sao a
 * "essencia" que o usuario pediu pra preservar.
 * ---------------------------------------------------------------------- */
static const lv_color_t COLOR_BG        = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t COLOR_BORDER    = LV_COLOR_MAKE(0x26, 0x26, 0x26);
static const lv_color_t COLOR_TEXT      = LV_COLOR_MAKE(0xF5, 0xF8, 0xF5);
static const lv_color_t COLOR_MUTED     = LV_COLOR_MAKE(0x7A, 0x84, 0x7A);
/* SEMANTICOS - fixos em qualquer tema. COLOR_GREEN/GREEN_DIM aqui
 * significam "ganhando/ok" (delta, LED bar, status conectado, melhor
 * volta), par do COLOR_RED "perdendo/erro". NAO trocam com o tema de
 * proposito: se o usuario escolher tema laranja, um delta "bom" laranja
 * ficaria perigosamente parecido com o vermelho de "ruim", e alertas
 * (COLOR_RED/COLOR_GOLD) poderiam se mascarar com a cor de marca. */
static const lv_color_t COLOR_GREEN     = LV_COLOR_MAKE(0x3E, 0xE0, 0x7A);
static const lv_color_t COLOR_GREEN_DIM = LV_COLOR_MAKE(0x16, 0x39, 0x20);
static const lv_color_t COLOR_RED       = LV_COLOR_MAKE(0xFF, 0x5A, 0x5A);
static const lv_color_t COLOR_RED_DIM   = LV_COLOR_MAKE(0x2A, 0x14, 0x14);
static const lv_color_t COLOR_BLUE      = LV_COLOR_MAKE(0x5F, 0xB8, 0xE8);
static const lv_color_t COLOR_GOLD      = LV_COLOR_MAKE(0xFF, 0xD7, 0x00);

/* Tema modernizado - novos tons (nao mexem na paleta acima, so somam) */
static const lv_color_t COLOR_SURFACE     = LV_COLOR_MAKE(0x0D, 0x0F, 0x0D); /* fundo sutil de card, quase preto */
static const lv_color_t COLOR_SURFACE_HDR = LV_COLOR_MAKE(0x1C, 0x1F, 0x1C); /* divisor/borda sobre COLOR_SURFACE */
static const lv_color_t COLOR_MUTED2      = LV_COLOR_MAKE(0x52, 0x59, 0x4F); /* cinza mais apagado - terciario (ex: BLE quando desligado) */
/* ----------------------------------------------------------------------
 * TEMAVEIS - cor "de marca" escolhida na aba Config > SISTEMA > TEMA
 * (persistida em NVS, ver settings_get_theme). Nascem com o tema Verde
 * (valores historicos) e sao sobrescritas por apply_theme_palette() no
 * comeco de ui_init(), antes de qualquer widget existir. Trocar o tema
 * em runtime so vale apos reinicio - as cores sao aplicadas na
 * construcao dos widgets, nao num estilo compartilhado.
 *   COLOR_PRIMARY        - texto/borda de destaque (ex-COLOR_GREEN de marca)
 *   COLOR_PRIMARY_DIM    - fundo de botao/aba selecionada (ex-GREEN_DIM de marca)
 *   COLOR_PRIMARY_ACCENT - acento lateral de celula / botao pressionado
 * ---------------------------------------------------------------------- */
static lv_color_t COLOR_PRIMARY        = LV_COLOR_MAKE(0x3E, 0xE0, 0x7A);
static lv_color_t COLOR_PRIMARY_DIM    = LV_COLOR_MAKE(0x16, 0x39, 0x20);
static lv_color_t COLOR_PRIMARY_ACCENT = LV_COLOR_MAKE(0x1F, 0x4D, 0x2A); /* acento lateral nas celulas comuns */

/* Paletas por tema - indices casam com settings_get_theme() (0=Verde
 * 1=Azul 2=Ambar 3=Laranja 4=Roxo, ver SETTINGS_THEME_COUNT). Vermelho
 * NAO e' opcao de tema de proposito - e' a cor de alerta/perda e nao
 * pode ser mascarada. DIM/ACCENT derivados a olho pra manter o mesmo
 * contraste da paleta verde original sobre fundo preto. */
typedef struct {
    uint8_t prim[3]; /* r,g,b */
    uint8_t dim[3];
    uint8_t acc[3];
} theme_palette_t;

static const theme_palette_t THEME_PALETTES[SETTINGS_THEME_COUNT] = {
    /* Verde   */ { {0x3E,0xE0,0x7A}, {0x16,0x39,0x20}, {0x1F,0x4D,0x2A} },
    /* Azul    */ { {0x4D,0xA3,0xFF}, {0x12,0x28,0x40}, {0x1C,0x3D,0x63} },
    /* Ambar   */ { {0xFF,0xC9,0x4D}, {0x3B,0x2D,0x0E}, {0x5A,0x44,0x16} },
    /* Laranja */ { {0xFF,0x8A,0x3E}, {0x3B,0x22,0x0E}, {0x5A,0x33,0x16} },
    /* Roxo    */ { {0xB4,0x7D,0xFF}, {0x28,0x1A,0x3D}, {0x3C,0x28,0x5C} },
};

static void apply_theme_palette(void)
{
    uint8_t t = settings_get_theme();
    if (t >= SETTINGS_THEME_COUNT) t = 0; /* settings ja clampa, cinto+suspensorio */
    const theme_palette_t *p = &THEME_PALETTES[t];
    COLOR_PRIMARY        = lv_color_make(p->prim[0], p->prim[1], p->prim[2]);
    COLOR_PRIMARY_DIM    = lv_color_make(p->dim[0],  p->dim[1],  p->dim[2]);
    COLOR_PRIMARY_ACCENT = lv_color_make(p->acc[0],  p->acc[1],  p->acc[2]);
}
static const lv_color_t COLOR_ACCENT_GOLD = LV_COLOR_MAKE(0x4A, 0x3A, 0x0A); /* acento lateral dourado - celula BEST (recorde) */
static const lv_color_t COLOR_CYAN        = LV_COLOR_MAKE(0x3E, 0xC6, 0xE0); /* icone WIFI ativo - COLOR_BLUE ja e' a cor "de BLE" */
#define RADIUS_CARD (8) /* cantos leves em cards/botoes - LED bar e numero do velocimetro ficam de fora de proposito */

/* 160 -> 200 -> 230: pedido do usuario pra preencher mais a tela CORRIDA
 * (fotos do hardware real mostraram preto sobrando nas laterais, e depois
 * ainda sobrava espaco entre os cards e o quadro HUD central). Fonte do
 * valor (font_kartbox_2xl, 48px) NAO mudou dessa vez - "1:39.189" (pior
 * caso real, com minuto) ja usa quase toda a largura do card, entao so o
 * card cresceu, nao o texto (crescer o texto agora arriscaria cortar). */
#define CELL_W              (230)
#define DELTA_LED_PER_SIDE  (8)
/* Escala da barra de LED do delta agora e' CONFIGURAVEL (CONFIG >
 * CORRIDA > ESCALA LED) - ver settings_get_led_scale_ms(). O antigo
 * fixo de 1500ms virou so o default de fabrica. */

/* Mapa da pista - canvas de tamanho fixo (buffer alocado 1x em PSRAM,
 * reaproveitado toda vez que o mapa abre). 760x380 cabe folgado na tela
 * 800x480 descontando o header do overlay (~50px) + padding. */
#define MAP_CANVAS_W (760)
#define MAP_CANVAS_H (380)

/* ----------------------------------------------------------------------
 * Estado / handles dos widgets
 * ---------------------------------------------------------------------- */
static lv_obj_t *s_mode_accent_bar; /* faixa de 3px no topo da tela - cor por modo (verde QUALY / vermelho CORRIDA) */
static lv_obj_t *s_gps_dot, *s_gps_label;
static lv_obj_t *s_mode_pill, *s_mode_label;
static lv_obj_t *s_rec_dot, *s_ble_icon, *s_wifi_icon, *s_usb_icon;
static lv_obj_t *s_batt_icon, *s_batt_lbl;   /* bateria na status bar */

static lv_obj_t *s_delta_segs_left[DELTA_LED_PER_SIDE];
static lv_obj_t *s_delta_segs_right[DELTA_LED_PER_SIDE];
static lv_obj_t *s_delta_led_row; /* linha inteira - so aparece na aba CORRIDA (ver show_tab) */

static lv_obj_t *s_lbl_atual_val, *s_lbl_best_val, *s_lbl_volta_val, *s_lbl_velmax_val;
static lv_obj_t *s_lbl_speed_val, *s_lbl_delta_val;
static lv_obj_t *s_lbl_predicted; /* tempo previsto da volta (best + delta ao vivo), abaixo do delta */
static lv_obj_t *s_flash_overlay;
static lv_obj_t *s_lbl_record;    /* banner "NOVO RECORDE" que pisca no best lap (so tela, sem LED) */
static lv_obj_t *s_race_warn_banner; /* aviso "pista sem linha de chegada" na aba CORRIDA */
static lv_obj_t *s_race_ready_banner; /* aviso "pista carregada, toque RESET" na aba CORRIDA */
static bool s_session_recording_active = false; /* espelha o ultimo valor visto em ui_set_recording_state() */

/* Layouts alternativos da tela CORRIDA (0=completo 1=DELTA gigante
 * 2=VELOCIDADE gigante) - toque na area central cicla; preferencia
 * salva POR MODO (qualy e corrida lembram layouts diferentes). */
static lv_obj_t *s_race_focus;   /* overlay opaco dos layouts 1/2 */
static lv_obj_t *s_focus_hint;   /* "DELTA" / "VELOCIDADE" no topo */
static lv_obj_t *s_focus_big;    /* numero gigante */
static lv_obj_t *s_focus_sub;    /* linha secundaria */
static uint8_t   s_race_layout = 0;
static bool      s_ui_race_mode = false; /* espelho do modo (ver ui_set_mode_label) */

/* show_tab() so e' definido mais pra frente (barra de abas) mas o botao
 * "Ir p/ PISTA" do aviso acima precisa chamar ela - forward decl. */
static void show_tab(int idx);

/* race_banner_refresh() so e' definido mais pra frente (perto de
 * ui_update_pista_status) mas ui_set_recording_state() (definida antes)
 * precisa chamar ela - forward decl. */
static void race_banner_refresh(bool finish_set);

/* apply_wifi_mode_ui() so e' definido mais pra frente (perto de
 * ui_set_wifi_mode_ui) mas build_config_tab() precisa chamar ela pra
 * aplicar o estado inicial (vindo da NVS) direto na construcao dos
 * widgets, sem passar pelo lock publico (build_config_tab ja roda
 * dentro do lock via ui_init()) - forward decl. */
static void apply_wifi_mode_ui(wifi_export_mode_t mode);

/* make_tab_btn() so e' definido perto da tab bar principal (mais pra
 * frente), mas a sub-tab bar da aba CONFIG (ver s_cfgsub_content acima)
 * reaproveita o mesmo helper - forward decl. */
static lv_obj_t *make_tab_btn(lv_obj_t *parent, const char *text, lv_obj_t **out_label);

static lv_obj_t *s_tab_content_pista, *s_tab_content_race, *s_tab_content_laps, *s_tab_content_cfg;
static lv_obj_t *s_tab_lbl_pista, *s_tab_lbl_race, *s_tab_lbl_laps, *s_tab_lbl_cfg;

/* Sub-abas DENTRO da aba CONFIG (Sistema/Corrida/WiFi/BLE) - a aba tinha
 * virado uma coluna unica scrollavel com status de GPS/SD, steppers de
 * corrida, modo pen drive, wifi AP/STA inteiro e BLE tudo empilhado -
 * usabilidade ruim (usuario relatou). Mesmo padrao de show/hide da tab
 * bar principal (ver show_tab), so que num segundo nivel. Ordem:
 * 0=SISTEMA 1=CORRIDA 2=WIFI 3=BLE. */
static lv_obj_t *s_cfgsub_content[4];
static lv_obj_t *s_cfgsub_lbl[4];

/* Widgets da aba PISTA */
static lv_obj_t *s_pista_name_ta;
static lv_obj_t *s_pista_finish_status;
static lv_obj_t *s_pista_finish_coord; /* lat/lon da linha de chegada, abaixo do status */
static lv_obj_t *s_pista_track_dd;
/* Painel de campos (NOME + CHEGADA/S1/S2 + Salvar) - escondido por padrao,
 * so aparece via "Nova Pista" ou "Editar" (ver ui_show_pista_edit_panel). */
static lv_obj_t *s_pista_edit_panel;
/* Row com Carregar/Editar/Excluir - so aparece quando o dropdown tem uma
 * pista de verdade selecionada (nao o placeholder "sem pista salva"). */
static lv_obj_t *s_pista_select_actions;

static lv_obj_t *s_session_dropdown, *s_laps_list, *s_laps_header;
static lv_obj_t *s_map_overlay, *s_map_canvas, *s_map_title_lbl;
static void     *s_map_canvas_buf; /* PSRAM, alocado 1x em ui_init(), NULL se falhar */

/* Estado do mapa em cache - o botao "Ver:" alterna entre sessao inteira
 * e so a melhor volta SEM reler o SD (dados ficam aqui apos
 * ui_show_session_map carregar). */
static sd_track_point_t s_map_pts[SD_MAX_TRACK_POINTS];
static int      s_map_pt_count = 0;
static bool     s_map_have_best = false;
static uint32_t s_map_best_lap = 0;
static bool     s_map_best_only = false; /* false = sessao inteira, true = so melhor volta */
static lv_obj_t *s_map_mode_lbl;         /* label do botao de alternar */
static char     s_map_session_name[SD_SESSION_NAME_LEN];
static void map_draw_canvas(void);       /* definido perto de ui_show_session_map */
static void map_update_title(void);

static lv_obj_t *s_boot_overlay, *s_boot_status_lbl;
static lv_obj_t *s_sd_usage_label, *s_sd_usage_bar, *s_ble_status_label;
static lv_obj_t *s_gps_link_label;
static lv_obj_t *s_gps_rf_label; /* diagnostico RF (em vista/ouvindo/snr) na SISTEMA */
static lv_obj_t *s_health_label; /* autoteste SD/GPS/BLE na SISTEMA */
static lv_obj_t *s_fw_version_label;
static lv_obj_t *s_ble_enable_sw;
static lv_obj_t *s_delete_btn_label;
static lv_obj_t *s_wifi_btn_label, *s_wifi_info_label;
static lv_obj_t *s_wifi_mode_sw, *s_wifi_mode_status_lbl, *s_wifi_sta_box, *s_wifi_ap_box;
static lv_obj_t *s_wifi_scan_dd, *s_wifi_sta_pass_ta, *s_wifi_sta_status_lbl;
static lv_obj_t *s_sta_connect_lbl; /* label do botao dinamico Conectar/Desconectar (modo Cliente) */
static lv_obj_t *s_usb_btn_label;
static lv_obj_t *s_utc_value_label, *s_gate_value_label, *s_min_lap_value_label;
static lv_obj_t *s_ble_name_ta, *s_wifi_pass_ta, *s_keyboard;
static lv_obj_t *s_keyboard_scrim;

/* Copia mutavel de font_kartbox_lg so pro teclado, com fallback pro font
 * padrao da LVGL. Motivo: nossas fontes custom (lv_font_conv) so tem
 * glifos ASCII 0x20-0x7F - os botoes de controle do lv_keyboard (backspace,
 * enter, shift, fechar, setas) usam codepoints da area de simbolos privados
 * da LVGL (LV_SYMBOL_*, faixa 0xF800+), que nao existem na nossa fonte e
 * saiam em branco. Fallback resolve sem precisar regerar a fonte. Precisa
 * ser uma copia em RAM (nao um cast do const original) porque o struct
 * gerado pelo font converter fica em flash/rodata - escrever nele direto
 * pode dar crash de acesso; copiar pra uma struct mutavel e' seguro. */
static lv_font_t s_kb_font;

/* Setores opcionais - CORRIDA tab (strip de splits) */
static lv_obj_t *s_sector_strip;                            /* barra inferior: VEL MAX | PREV | IDEAL */
static lv_obj_t *s_lbl_velmax_strip;                        /* velocidade maxima, na barra inferior */
static lv_obj_t *s_lbl_ideal_lap;                          /* "IDEAL: 1:38.900" - soma dos melhores segmentos */
/* Setores opcionais - PISTA tab */
static lv_obj_t *s_lbl_sector_status[GPS_MAX_SECTORS];     /* "definido" / "---" */
static lv_obj_t *s_lbl_sector_coord[GPS_MAX_SECTORS];      /* lat/lon do ponto, abaixo do status */

static bool       s_delete_armed = false;
static lv_timer_t *s_delete_arm_timer = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static float       s_max_speed_kmh = 0.0f;
static int         s_slow_tick = 0;

/* Toast - popup centralizado de feedback (linha marcada, etc.) */
static lv_obj_t  *s_toast_box;
static lv_obj_t  *s_toast_label;
static lv_timer_t *s_toast_timer;

/* Popup de estatisticas da sessao (aparece no encerramento). O timer de
 * auto-fechar e' definido mais pra frente (perto de make_center_overlay)
 * mas ui_show_session_stats() precisa dele - forward decl. */
static lv_obj_t  *s_stats_overlay;
static lv_obj_t  *s_stats_body_label;
static lv_timer_t *s_stats_timer;
static void stats_overlay_timer_cb(lv_timer_t *t);

/* Comparacao de voltas (aba VOLTAS) - cache da ultima lista carregada +
 * handles das linhas pra selecao por toque. s_cmp_sel = indice da 1a
 * volta selecionada (-1 = nenhuma); tocar numa 2a abre o overlay. */
static sd_lap_summary_t s_laps_cache[SD_MAX_LAPS_LISTED];
static int              s_laps_cache_count = 0;
static lv_obj_t        *s_lap_rows[SD_MAX_LAPS_LISTED];
static int              s_cmp_sel = -1;
static lv_obj_t        *s_cmp_overlay;
static lv_obj_t        *s_cmp_body_label;

/* BLE passkey overlay - mostra o codigo SMP de 6 digitos */
static lv_obj_t  *s_passkey_overlay;
static lv_obj_t  *s_passkey_label;

/* Overlay de OTA em andamento (ver ui_show_ota_progress) */
static lv_obj_t  *s_ota_overlay;

/* Hold-progress - barra que anima enquanto piloto segura RESET pra
 * encerrar OU MODE pra trocar qualy/race (overlay compartilhado; o
 * label troca conforme o botao). */
static lv_obj_t  *s_hold_overlay;
static lv_obj_t  *s_hold_bar;
static lv_obj_t  *s_hold_label;
static lv_timer_t *s_mode_hold_timer; /* carencia anti-flicker do hold do MODE */

/* Troca de modo QUALY/RACE - flash de tela inteira + popup grande central.
 * Vivem no layer_top, entao aparecem por cima de qualquer aba (o pill do
 * status bar sozinho e pequeno demais pra notar guiando). */
static lv_obj_t   *s_mode_flash_overlay;
static lv_obj_t   *s_mode_popup;
static lv_obj_t   *s_mode_popup_label;
static lv_timer_t *s_mode_popup_timer;

/* ----------------------------------------------------------------------
 * Helpers de construcao
 * ---------------------------------------------------------------------- */
static lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* accent_color/accent_on_left: acento de 2px numa borda so (esquerda ou
 * direita, conforme o lado da tela) - verde nas celulas comuns, dourado
 * na BEST (ela sempre mostra o recorde da sessao, entao merece destaque
 * proprio). Card ganhou superficie (COLOR_SURFACE) + cantos leves
 * (RADIUS_CARD) no lugar da antiga linha divisoria reta - parte do tema
 * modernizado aprovado com o usuario via preview visual. */
static lv_obj_t *make_cell(lv_obj_t *parent, const char *label_txt, lv_obj_t **out_value,
                            lv_color_t accent_color, bool accent_on_left)
{
    lv_obj_t *cell = bare(parent);
    lv_obj_set_size(cell, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_bg_color(cell, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell, RADIUS_CARD, 0);
    lv_obj_set_style_border_width(cell, 3, 0); /* 2->3: acompanha o card maior, fica mais "presente" */
    lv_obj_set_style_border_color(cell, accent_color, 0);
    lv_obj_set_style_border_side(cell, accent_on_left ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 6, 0); /* respiro entre o label e o valor, agora maiores */

    /* ATUAL/BEST/VOLTA/VEL MAX - reportado como "praticamente ilegivel"
     * em 22px (font_kartbox_lg) olhando a tela dirigindo. Subiu pro
     * mesmo tamanho do numero de delta (font_kartbox_xl, 36px), e agora
     * (pedido do usuario pra preencher mais a tela CORRIDA, que sobrava
     * espaco vazio nas celulas laterais) subiu de novo pra
     * font_kartbox_2xl (48px, fonte nova gerada so pra esse fim - ver
     * fonts.h). Testado com CELL_W maior (ver abaixo) pra "99.999" /
     * "104.521" (piores casos de largura do formato "%lu.%03lu" de
     * ATUAL/BEST) ainda caberem sem cortar. */
    lv_obj_t *lbl = lv_label_create(cell);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, label_txt);

    lv_obj_t *val = lv_label_create(cell);
    lv_obj_set_style_text_font(val, &font_kartbox_2xl, 0);
    lv_obj_set_style_text_color(val, COLOR_TEXT, 0);
    lv_label_set_text(val, "--");

    *out_value = val;
    return cell;
}

/* BUG REAL reportado: ATUAL/BEST mostravam so segundos corridos, sem
 * rolar minuto ("61...62...150" em vez de "1:01.000...1:30.000"). Formato
 * antigo era "%lu.%03lu" com (ms/1000) direto como "segundos" - correto
 * so' enquanto o valor ficava abaixo de 60s. Helper central usada nos 3
 * lugares que mostram tempo de volta (ATUAL, BEST, splits de setor) pra
 * nao repetir a mesma formatacao 3x e arriscar corrigir so uma. Abaixo de
 * 1 minuto mantem o formato curto "SS.mmm" (mais legivel, cabe mais folgado
 * na celula); a partir de 1 minuto vira "M:SS.mmm". */
static void format_lap_time(char *buf, size_t bufsz, uint32_t ms)
{
    uint32_t total_s = ms / 1000;
    uint32_t mins = total_s / 60;
    uint32_t secs = total_s % 60;
    uint32_t frac = ms % 1000;
    if (mins > 0) {
        snprintf(buf, bufsz, "%lu:%02lu.%03lu",
                 (unsigned long)mins, (unsigned long)secs, (unsigned long)frac);
    } else {
        snprintf(buf, bufsz, "%lu.%03lu", (unsigned long)secs, (unsigned long)frac);
    }
}

/* Cantos leves + feedback visual claro ao pressionar - antes um botao
 * tocado nao mudava de aparencia nenhuma alem do proprio evento disparar
 * (ruim numa tela tocada com luva/sol forte). bg/bg_pressed sao cores
 * completas (nao "clareia X%") de proposito - a maioria dos botoes aqui
 * usa tons escuros bem especificos (*_DIM), clarear por formula genérica
 * ficaria estranho em alguns. Chamar depois de criar o botao e antes de
 * criar o label dele (nao interfere - so estilo do proprio botao). */
static void style_action_button(lv_obj_t *btn, lv_color_t bg, lv_color_t bg_pressed)
{
    lv_obj_set_style_radius(btn, RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, bg_pressed, LV_STATE_PRESSED);
}

static lv_obj_t *make_led_seg(lv_obj_t *parent)
{
    lv_obj_t *seg = bare(parent);
    lv_obj_set_size(seg, 16, 18);
    lv_obj_set_style_radius(seg, 0, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(seg, COLOR_GREEN_DIM, 0);
    return seg;
}

/* ----------------------------------------------------------------------
 * Status bar (GPS, modo, gravando, BLE)
 * ---------------------------------------------------------------------- */
static void build_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = bare(parent);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_style_bg_color(bar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_SURFACE_HDR, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *gps_grp = bare(bar);
    lv_obj_set_size(gps_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(gps_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gps_grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gps_grp, 8, 0);

    s_gps_dot = bare(gps_grp);
    lv_obj_set_size(s_gps_dot, 10, 10);
    lv_obj_set_style_radius(s_gps_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_gps_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_gps_dot, COLOR_MUTED, 0);

    s_gps_label = lv_label_create(gps_grp);
    lv_obj_set_style_text_font(s_gps_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_gps_label, COLOR_MUTED, 0);
    lv_label_set_text(s_gps_label, "GPS --");

    s_mode_pill = bare(bar);
    lv_obj_set_size(s_mode_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_mode_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mode_pill, 10, 0);
    lv_obj_set_style_pad_hor(s_mode_pill, 12, 0);
    lv_obj_set_style_pad_ver(s_mode_pill, 4, 0);
    s_mode_label = lv_label_create(s_mode_pill);
    lv_obj_set_style_text_font(s_mode_label, &font_kartbox_lg, 0);
    lv_label_set_text(s_mode_label, "QUALY");

    lv_obj_t *right_grp = bare(bar);
    lv_obj_set_size(right_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_grp, 10, 0);

    /* Bateria (icone + %) - primeiro item do grupo da direita. O icone usa
     * lv_font_montserrat_24 (built-in) pelos glifos LV_SYMBOL_BATTERY_*, que
     * as fontes font_kartbox_* nao trazem (so ASCII 0x20-0x7F, ver fonts.h).
     * O % usa font_kartbox_lg pra combinar com o label de GPS. Preenchido no
     * refresh_timer_cb (slow tick). */
    lv_obj_t *batt_grp = bare(right_grp);
    lv_obj_set_size(batt_grp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(batt_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt_grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt_grp, 4, 0);

    s_batt_icon = lv_label_create(batt_grp);
    lv_obj_set_style_text_font(s_batt_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_batt_icon, COLOR_MUTED, 0);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_EMPTY);

    s_batt_lbl = lv_label_create(batt_grp);
    lv_obj_set_style_text_font(s_batt_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_batt_lbl, COLOR_MUTED, 0);
    lv_label_set_text(s_batt_lbl, "--%");

    s_rec_dot = bare(right_grp);
    lv_obj_set_size(s_rec_dot, 10, 10);
    lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rec_dot, COLOR_MUTED, 0);

    /* Icone WIFI - so aparece (nao so muda de cor, como o BLE) quando o
     * export sob demanda esta de fato ativo/servindo (AP proprio no ar
     * OU cliente conectado). Pisca enquanto ativo (ver wifi_icon_blink_
     * anim_cb/refresh_timer_cb) - pedido do usuario, deixa obvio que o
     * radio esta em uso agora, nao so "disponivel". Cor ciano (COLOR_CYAN)
     * pra nao repetir o azul do BLE.
     *
     * Usa lv_font_montserrat_24 (built-in do LVGL, habilitado so pra isso
     * no sdkconfig) em vez das fontes font_kartbox_* geradas pra esse
     * projeto - essas ultimas foram convertidas so com o range ASCII
     * 0x20-0x7F (ver fonts.h), sem os glifos especiais LV_SYMBOL_* (ficam
     * numa area Unicode privada, tipo 0xF001+), que so os fonts montserrat
     * embutidos do LVGL trazem. 24px (contra o 14px inicial) pra ficar do
     * tamanho dos outros elementos da status bar (pedido do usuario -
     * "icones ficaram muito pequenos"). */
    s_wifi_icon = lv_label_create(right_grp);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_wifi_icon, COLOR_CYAN, 0);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);

    /* Icone USB - mesma linguagem do WIFI: so aparece quando o modo pen
     * drive esta de fato ativo, e pisca enquanto visivel (o SD esta nas
     * maos do computador nesse estado - gravacao parada - entao vale
     * deixar bem obvio). Mostrado/escondido em ui_set_usb_mode_state()
     * (transicao por evento, nao por polling - ao contrario do WIFI, que
     * pode desligar sozinho por timeout e por isso e' verificado no
     * refresh_timer_cb). Cor do TEMA pra nao inventar mais uma cor fixa
     * na status bar. */
    s_usb_icon = lv_label_create(right_grp);
    lv_obj_set_style_text_font(s_usb_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_usb_icon, COLOR_PRIMARY, 0);
    lv_label_set_text(s_usb_icon, LV_SYMBOL_USB);
    lv_obj_add_flag(s_usb_icon, LV_OBJ_FLAG_HIDDEN);

    /* Estado real (oculto/cinza/azul-piscando) e' definido no refresh_timer_cb.
     * Comeca OCULTO pra nao dar flash de "BLE ativo" no boot antes do primeiro
     * slow tick - so aparece se o radio estiver de fato ligado. */
    s_ble_icon = lv_label_create(right_grp);
    lv_obj_set_style_text_font(s_ble_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_ble_icon, COLOR_MUTED, 0);
    lv_label_set_text(s_ble_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_add_flag(s_ble_icon, LV_OBJ_FLAG_HIDDEN);
}

/* ----------------------------------------------------------------------
 * Barra de LED do delta - reaproveita a linguagem de shift-light de
 * RPM (que kart nao tem) pra mostrar ganho/perda de tempo de forma
 * legivel com o rabo do olho, sem precisar focar nos digitos.
 * ---------------------------------------------------------------------- */
static void build_delta_led_bar(lv_obj_t *parent)
{
    lv_obj_t *row = bare(parent);
    s_delta_led_row = row; /* so visivel na aba CORRIDA - ver show_tab() */
    lv_obj_set_size(row, lv_pct(100), 26);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    /* CENTER (nao SPACE_BETWEEN) - com left/right agora content-sized,
     * SPACE_BETWEEN jogaria os dois grupos pras bordas extremas da tela
     * com um vao enorme no meio. CENTER mantem os 2 grupos + divisor
     * juntos, formando uma barra compacta - o divisor fica exatamente
     * no meio porque os dois grupos tem o mesmo tamanho. */
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    /* left/right SEM largura fixa (lv_pct antigo deixava ~355px de caixa
     * pra so ~142px de conteudo real - a folga sobrava sempre do MESMO
     * lado dentro de cada caixa, o que fazia os dois grupos nao ficarem
     * espelhados um do outro visualmente). LV_SIZE_CONTENT faz a caixa
     * abraçar exatamente os 8 segmentos; como os dois lados tem o mesmo
     * numero/tamanho de segmento, ficam identicos em largura e o CENTER
     * do row pai garante simetria perfeita em volta do divisor central. */
    lv_obj_t *left = bare(row);
    lv_obj_set_size(left, LV_SIZE_CONTENT, 20);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 2, 0);
    for (int i = 0; i < DELTA_LED_PER_SIDE; i++) {
        s_delta_segs_left[i] = make_led_seg(left);
        lv_obj_set_style_bg_color(s_delta_segs_left[i], COLOR_RED_DIM, 0);
    }

    lv_obj_t *divider = bare(row);
    lv_obj_set_size(divider, 2, 20);
    lv_obj_set_style_bg_color(divider, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *right = bare(row);
    lv_obj_set_size(right, LV_SIZE_CONTENT, 20);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 2, 0);
    for (int i = 0; i < DELTA_LED_PER_SIDE; i++) {
        s_delta_segs_right[i] = make_led_seg(right);
    }
}

/* ----------------------------------------------------------------------
 * Aba Corrida
 * ---------------------------------------------------------------------- */
static void race_mark_line_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_BTN_SETLINE, EVT_SRC_TOUCH);
}

/* Aplica um layout da tela CORRIDA (ver s_race_layout). Layout 0 =
 * esconde o overlay de foco (tela completa por baixo); 1/2 = overlay
 * opaco por cima com o numero gigante correspondente. */
static void apply_race_layout(uint8_t layout)
{
    s_race_layout = (layout > 2) ? 0 : layout;
    if (s_race_layout == 0) {
        lv_obj_add_flag(s_race_focus, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_focus_hint, s_race_layout == 1 ? "DELTA" : "VELOCIDADE");
        lv_obj_clear_flag(s_race_focus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_race_focus);
    }
}

/* Cicla completo -> delta -> velocidade -> completo, salvando a
 * preferencia DO MODO ATUAL (qualy e corrida independentes). Chamado
 * pelo toque na tela (cb abaixo) e pelo TAP do botao fisico MODE
 * (ui_cycle_race_layout). */
static void race_layout_cycle_step(void)
{
    uint8_t next = (uint8_t)((s_race_layout + 1) % 3);
    settings_set_mode_layout(s_ui_race_mode ? 1 : 0, next);
    apply_race_layout(next);
}

static void race_layout_cycle_cb(lv_event_t *e)
{
    (void)e;
    race_layout_cycle_step();
}

static void race_goto_pista_cb(lv_event_t *e)
{
    (void)e;
    show_tab(0);
}

static void build_race_tab(lv_obj_t *parent)
{
    /* parent vira flex-column: banner (topo, opcional) + main_row
     * flex-grow=1 + sector_strip fixo em baixo. */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    /* Aviso "pista sem linha de chegada" - filho direto de 'parent', NAO
     * de 'center' (tentativa anterior): 'center' e' flex CENTER/CENTER e
     * quando o conteudo total passava da altura disponivel, o topo do
     * aviso ficava cortado (o bloco centralizado vazava pra cima do
     * limite do row). Como item normal no topo do COLUMN flow de
     * 'parent', ele reserva o proprio espaco (LV_SIZE_CONTENT) e empurra
     * o resto pra baixo - sem risco de sobrepor/cortar nada. Bonus: aqui
     * ele tem a largura da aba inteira (~800px) em vez de so ~94% da
     * coluna central (~480px), entao o texto quase sempre cabe numa
     * linha so. Some sozinho via ui_update_pista_status() assim que uma
     * linha de chegada valida existe. */
    s_race_warn_banner = bare(parent);
    lv_obj_set_size(s_race_warn_banner, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_race_warn_banner, lv_color_make(0x2A, 0x1E, 0x08), 0);
    lv_obj_set_style_bg_opa(s_race_warn_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_race_warn_banner, COLOR_GOLD, 0);
    lv_obj_set_style_border_width(s_race_warn_banner, 1, 0);
    lv_obj_set_style_border_side(s_race_warn_banner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_ver(s_race_warn_banner, 10, 0);
    lv_obj_set_style_pad_hor(s_race_warn_banner, 14, 0);
    lv_obj_set_flex_flow(s_race_warn_banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_race_warn_banner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_race_warn_banner, 8, 0);

    lv_obj_t *warn_lbl = lv_label_create(s_race_warn_banner);
    lv_obj_set_width(warn_lbl, lv_pct(100));
    lv_obj_set_style_text_font(warn_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(warn_lbl, COLOR_GOLD, 0);
    lv_obj_set_style_text_align(warn_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(warn_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(warn_lbl, "SEM LINHA DE CHEGADA - pista nova ou nao configurada");

    lv_obj_t *warn_btn_row = bare(s_race_warn_banner);
    lv_obj_set_size(warn_btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(warn_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(warn_btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(warn_btn_row, 14, 0);

    lv_obj_t *mark_btn = lv_button_create(warn_btn_row);
    lv_obj_set_size(mark_btn, 210, 44);
    style_action_button(mark_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(mark_btn, race_mark_line_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mark_lbl = lv_label_create(mark_btn);
    lv_obj_center(mark_lbl);
    lv_obj_set_style_text_font(mark_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(mark_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(mark_lbl, "Marcar linha aqui");

    lv_obj_t *goto_btn = lv_button_create(warn_btn_row);
    lv_obj_set_size(goto_btn, 190, 44);
    /* bg/texto explicitos - sem isso o botao herda o estilo default do
     * tema (saiu branco/sem contraste com o texto na pratica, ilegivel). */
    style_action_button(goto_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(goto_btn, race_goto_pista_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *goto_lbl = lv_label_create(goto_btn);
    lv_obj_center(goto_lbl);
    lv_obj_set_style_text_font(goto_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(goto_lbl, COLOR_BLUE, 0);
    lv_label_set_text(goto_lbl, "Ir p/ PISTA");

    /* Comeca escondido - ui_update_pista_status() (chamada logo apos o
     * boot restaurar a pista salva) e' quem decide se aparece, pra nao
     * pisicar visivel um frame antes de sabermos o estado real. */
    lv_obj_add_flag(s_race_warn_banner, LV_OBJ_FLAG_HIDDEN);

    /* Aviso "pista carregada, toque RESET pra iniciar" - mutuamente
     * exclusivo com s_race_warn_banner acima (so um dos dois aparece por
     * vez, ver race_banner_refresh()). Pedido do usuario: depois que uma
     * pista carrega (linha de chegada valida existe) mas a sessao ainda
     * nao comecou a gravar, deixa explicito na propria tela CORRIDA que
     * falta so apertar RESET - antes o piloto so descobria isso indo
     * conferir a aba PISTA ou tentando entender por que ATUAL nao andava. */
    s_race_ready_banner = bare(parent);
    lv_obj_set_size(s_race_ready_banner, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_race_ready_banner, COLOR_PRIMARY_DIM, 0);
    lv_obj_set_style_bg_opa(s_race_ready_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_race_ready_banner, COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_race_ready_banner, 1, 0);
    lv_obj_set_style_border_side(s_race_ready_banner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_ver(s_race_ready_banner, 10, 0);
    lv_obj_set_style_pad_hor(s_race_ready_banner, 14, 0);

    lv_obj_t *ready_lbl = lv_label_create(s_race_ready_banner);
    lv_obj_set_width(ready_lbl, lv_pct(100));
    lv_obj_set_style_text_font(ready_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(ready_lbl, COLOR_PRIMARY, 0);
    lv_obj_set_style_text_align(ready_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ready_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(ready_lbl, "PISTA CARREGADA - toque RESET para iniciar a volta");

    lv_obj_add_flag(s_race_ready_banner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *row = bare(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);

    lv_obj_t *left = bare(row);
    lv_obj_set_size(left, CELL_W, lv_pct(100));
    lv_obj_set_style_pad_right(left, 6, 0);
    lv_obj_set_style_pad_row(left, 6, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    make_cell(left, "ATUAL", &s_lbl_atual_val, COLOR_PRIMARY_ACCENT, true);
    /* BEST sempre mostra o recorde da sessao - acento dourado fixo (nao
     * um "pisca quando bate recorde", ela JA E o recorde por definicao,
     * entao ja nasce destacada). */
    make_cell(left, "BEST", &s_lbl_best_val, COLOR_ACCENT_GOLD, true);

    lv_obj_t *center = bare(row);
    lv_obj_set_width(center, lv_pct(100));
    lv_obj_set_height(center, lv_pct(100));
    lv_obj_set_flex_grow(center, 1);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* toque no centro cicla os layouts da tela (nao ha botoes aqui,
     * so o velocimetro - clique livre) */
    lv_obj_add_event_cb(center, race_layout_cycle_cb, LV_EVENT_CLICKED, NULL);

    /* Vinheta removida - testada no hardware real e a opacidade baixa
     * (LV_OPA_10) nao rendeu como translucida nesse pipeline de desenho
     * (sw_rotate + RGB565), saiu uma bola solida bem visivel em vez de um
     * brilho sutil. Reportado como feio pelo usuario - tirando em vez de
     * investigar o blend (elemento 100% decorativo, nao vale o risco de
     * mexer no pipeline de draw por isso). */

    /* Cantos estilo HUD de jogo de corrida ao redor do velocimetro - so
     * decorativo, 4 "L" pequenos (2 lados de borda cada) enquadrando o
     * numero. LV_OBJ_FLAG_IGNORE_LAYOUT pra nao interferir no
     * center/speed_row/delta_row. */
    static const struct { lv_align_t align; lv_border_side_t sides; } hud_corners[4] = {
        { LV_ALIGN_TOP_LEFT,     LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT },
        { LV_ALIGN_TOP_RIGHT,    LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT },
        { LV_ALIGN_BOTTOM_LEFT,  LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT },
        { LV_ALIGN_BOTTOM_RIGHT, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT },
    };
    /* 190x130 -> 260x180 e cantos 18->26px: pedido do usuario pra
     * preencher mais o centro da tela CORRIDA (o quadro HUD antigo
     * deixava faixas pretas grandes acima/abaixo do numero de
     * velocidade). O numero em si (font_kartbox_huge, 96px) nao muda -
     * so o enquadramento decorativo ao redor cresce. */
    lv_obj_t *hud_frame = bare(center);
    lv_obj_add_flag(hud_frame, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(hud_frame, LV_OBJ_FLAG_CLICKABLE);
    /* 260->310 de largura: velocidade de 3 digitos a 96px + "km/h"
     * alcancava os cantos do quadro - eles pareciam uma "borda preta
     * cortando os numeros" (reportado em campo). */
    lv_obj_set_size(hud_frame, 310, 180);
    lv_obj_align(hud_frame, LV_ALIGN_CENTER, 0, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *corner = bare(hud_frame);
        lv_obj_clear_flag(corner, LV_OBJ_FLAG_CLICKABLE); /* so decorativo - nao engolir toque */
        lv_obj_set_size(corner, 26, 26);
        lv_obj_align(corner, hud_corners[i].align, 0, 0);
        lv_obj_set_style_border_width(corner, 3, 0);
        lv_obj_set_style_border_color(corner, COLOR_PRIMARY_ACCENT, 0);
        lv_obj_set_style_border_side(corner, hud_corners[i].sides, 0);
    }

    lv_obj_t *speed_row = bare(center);
    /* CAUSA RAIZ do corte dos numeros (campo: "8 vira 3", "DELTA sumiu"):
     * essas rows nunca tiveram tamanho definido e o default de lv_obj e'
     * 100x100px - LVGL CLIPA filhos na caixa do pai, entao tudo alem de
     * 100px de largura era cortado. SIZE_CONTENT faz a row abraçar o
     * conteudo (labels de largura fixa) sem clipar nada. */
    lv_obj_set_size(speed_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* NAO clicavel: sem isso a row engolia o toque (sem tratar) e a area
     * util do "toque pra ciclar layout" virava so a nesga vazia do
     * center - reportado em campo como area de clique minuscula. Com o
     * flag limpo, o toque atravessa e cai no center, que trata. */
    lv_obj_clear_flag(speed_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(speed_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(speed_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(speed_row, 8, 0);

    s_lbl_speed_val = lv_label_create(speed_row);
    lv_obj_set_style_text_font(s_lbl_speed_val, &font_kartbox_huge, 0);
    lv_obj_set_style_text_color(s_lbl_speed_val, COLOR_TEXT, 0);
    /* Largura FIXA (pior caso: 3 digitos a 96px) + texto centralizado.
     * Sem isso a caixa do label era recalculada a cada mudanca de
     * numero de digitos a 10Hz - e o glifo saia CORTADO na transicao
     * 1->2 digitos ("o 8 parece um 3", reportado em campo). Caixa fixa
     * nunca re-layouta, nunca corta. */
    lv_obj_set_width(s_lbl_speed_val, 200);
    lv_obj_set_style_text_align(s_lbl_speed_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_speed_val, "0");

    lv_obj_t *speed_unit = lv_label_create(speed_row);
    /* sm(12px) -> lg(22px): unidade "km/h" ficava pequena demais perto do
     * numero grande, contribuindo pra sensacao de tela vazia. */
    lv_obj_set_style_text_font(speed_unit, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(speed_unit, COLOR_MUTED, 0);
    lv_label_set_text(speed_unit, "km/h");

    lv_obj_t *delta_row = bare(center);
    lv_obj_set_size(delta_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT); /* mesma correcao da speed_row */
    lv_obj_clear_flag(delta_row, LV_OBJ_FLAG_CLICKABLE); /* toque atravessa pro center (ciclar layout) */
    lv_obj_set_flex_flow(delta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(delta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(delta_row, 10, 0);
    lv_obj_set_style_pad_top(delta_row, 10, 0);

    lv_obj_t *delta_lbl = lv_label_create(delta_row);
    /* sm(12px) -> lg(22px): "DELTA" ficava pequeno demais perto do valor
     * em xl(36px) - mesmo motivo do speed_unit acima. */
    lv_obj_set_style_text_font(delta_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(delta_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(delta_lbl, "DELTA");

    s_lbl_delta_val = lv_label_create(delta_row);
    lv_obj_set_style_text_font(s_lbl_delta_val, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(s_lbl_delta_val, COLOR_MUTED, 0);
    /* mesma protecao de caixa fixa do velocimetro (pior caso "+99.99") */
    lv_obj_set_width(s_lbl_delta_val, 150);
    lv_obj_set_style_text_align(s_lbl_delta_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_delta_val, "--.--");

    /* (Tempo previsto agora mora na barra inferior, junto de VEL MAX e
     * IDEAL - ver build da strip de setores abaixo.) */

    /* overlay de flash - fora do fluxo de layout, cobre o centro
     * inteiro, usado so por ui_flash_lap_complete() */
    s_flash_overlay = bare(center);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(s_flash_overlay, LV_OBJ_FLAG_CLICKABLE); /* nao engolir toque (mesma licao do s_mode_flash_overlay) */
    lv_obj_set_size(s_flash_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_flash_overlay, COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_flash_overlay, 0, 0);

    /* Banner "NOVO RECORDE" - so tela (sem LED, por escolha do usuario).
     * Fora do layout, centralizado sobre o centro; some via fade em
     * ui_flash_lap_complete() quando is_new_best. */
    s_lbl_record = lv_label_create(center);
    lv_obj_add_flag(s_lbl_record, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(s_lbl_record, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(s_lbl_record, COLOR_GOLD, 0);
    lv_label_set_text(s_lbl_record, "NOVO RECORDE");
    lv_obj_align(s_lbl_record, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_text_opa(s_lbl_record, LV_OPA_TRANSP, 0);

    lv_obj_t *right = bare(row);
    lv_obj_set_size(right, CELL_W, lv_pct(100));
    lv_obj_set_style_pad_left(right, 6, 0);
    lv_obj_set_style_pad_row(right, 6, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    make_cell(right, "VOLTA", &s_lbl_volta_val, COLOR_PRIMARY_ACCENT, false);
    /* A antiga celula "VEL MAX" virou "SETORES" (pedido do usuario): mostra
     * os splits ao vivo (2 se o piloto marcou setores manuais, 3 se estao
     * no modo automatico). Valor multi-linha, entao usa fonte menor que os
     * 48px padrao das outras celulas. s_lbl_velmax_val reaproveitado como
     * handle desse valor. Vel max continua sendo gravado no CSV/export. */
    make_cell(right, "SETORES", &s_lbl_velmax_val, COLOR_PRIMARY_ACCENT, false);
    lv_obj_set_style_text_font(s_lbl_velmax_val, &font_kartbox_lg, 0);
    lv_obj_set_style_text_align(s_lbl_velmax_val, LV_TEXT_ALIGN_CENTER, 0);
    /* recolor ligado: cada setor sai numa cor propria via "#rrggbb ..#"
     * (ver refresh_timer_cb). */
    lv_label_set_recolor(s_lbl_velmax_val, true);
    lv_label_set_text(s_lbl_velmax_val, "#3ec6e0 S1 --#\n#ffd700 S2 --#");

    /* Barra inferior - SEMPRE visivel agora. Os splits de setor sairam
     * daqui (foram pra celula SETORES, ex-VEL MAX); no lugar entram VEL
     * MAX e a VOLTA PREVISTA, ao lado da volta IDEAL. */
    s_sector_strip = bare(parent);
    lv_obj_set_size(s_sector_strip, lv_pct(100), 36);
    lv_obj_set_style_border_width(s_sector_strip, 1, 0);
    lv_obj_set_style_border_color(s_sector_strip, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(s_sector_strip, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_hor(s_sector_strip, 14, 0);
    lv_obj_set_flex_flow(s_sector_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sector_strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Velocidade maxima da sessao (saiu da celula, que virou SETORES). */
    s_lbl_velmax_strip = lv_label_create(s_sector_strip);
    lv_obj_set_style_text_font(s_lbl_velmax_strip, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_lbl_velmax_strip, COLOR_TEXT, 0);
    lv_label_set_text(s_lbl_velmax_strip, "MAX --");

    /* Volta prevista (predictive) = best + delta ao vivo. Verde/vermelho
     * conforme o rumo; preenchida em refresh_timer_cb. */
    s_lbl_predicted = lv_label_create(s_sector_strip);
    lv_obj_set_style_text_font(s_lbl_predicted, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_lbl_predicted, COLOR_MUTED, 0);
    lv_label_set_text(s_lbl_predicted, "PREV --.---");

    /* Volta ideal - soma dos melhores segmentos da sessao. */
    s_lbl_ideal_lap = lv_label_create(s_sector_strip);
    lv_obj_set_style_text_font(s_lbl_ideal_lap, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_lbl_ideal_lap, COLOR_GOLD, 0);
    lv_label_set_text(s_lbl_ideal_lap, "IDEAL: --");

    /* ------------------------------------------------------------------
     * Overlay de FOCO (layouts 1/2) - cobre a aba inteira com UM numero
     * gigante + linha secundaria. Toque nele volta a ciclar. Conteudo
     * preenchido no refresh_timer_cb quando visivel.
     * ------------------------------------------------------------------ */
    s_race_focus = bare(parent);
    lv_obj_add_flag(s_race_focus, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_race_focus, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_race_focus, 0, 0);
    lv_obj_set_style_bg_color(s_race_focus, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_race_focus, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_race_focus, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_race_focus, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_race_focus, 10, 0);
    lv_obj_add_event_cb(s_race_focus, race_layout_cycle_cb, LV_EVENT_CLICKED, NULL);

    s_focus_hint = lv_label_create(s_race_focus);
    lv_obj_set_style_text_font(s_focus_hint, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_focus_hint, COLOR_MUTED, 0);
    lv_label_set_text(s_focus_hint, "DELTA");

    s_focus_big = lv_label_create(s_race_focus);
    lv_obj_set_style_text_font(s_focus_big, &font_kartbox_huge, 0);
    lv_obj_set_style_text_color(s_focus_big, COLOR_TEXT, 0);
    lv_obj_set_width(s_focus_big, 460); /* caixa fixa - mesma protecao anti-corte do velocimetro */
    lv_obj_set_style_text_align(s_focus_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_focus_big, "--");

    s_focus_sub = lv_label_create(s_race_focus);
    lv_obj_set_style_text_font(s_focus_sub, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(s_focus_sub, COLOR_MUTED, 0);
    lv_obj_set_width(s_focus_sub, 560);
    lv_obj_set_style_text_align(s_focus_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_focus_sub, "--");

    lv_obj_add_flag(s_race_focus, LV_OBJ_FLAG_HIDDEN);
}

/* ----------------------------------------------------------------------
 * Aba Voltas
 * ---------------------------------------------------------------------- */
static void session_dropdown_event_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    app_event_t evt = { .type = APP_EVT_SESSION_SELECT, .source = EVT_SRC_TOUCH };
    evt.data.session_index = sel;
    app_event_post_data(&evt);
}

static void refresh_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ui_refresh_session_list();
}

static void map_btn_event_cb(lv_event_t *e)
{
    (void)e;
    uint16_t sel = lv_dropdown_get_selected(s_session_dropdown);
    app_event_t evt = { .type = APP_EVT_SESSION_MAP, .source = EVT_SRC_TOUCH };
    evt.data.session_index = sel;
    app_event_post_data(&evt);
}

static void map_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_map_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* Alterna sessao inteira <-> so a melhor volta e redesenha do cache
 * (sem reler SD). Roda no contexto LVGL (callback de botao). */
static void map_mode_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_map_have_best) return; /* sem volta fechada nao ha o que alternar */
    s_map_best_only = !s_map_best_only;
    map_update_title();
    map_draw_canvas();
}

static void build_laps_tab(lv_obj_t *parent)
{
    lv_obj_t *col = bare(parent);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 12, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, 0);

    lv_obj_t *header = bare(col);
    lv_obj_set_size(header, lv_pct(100), 46);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_session_dropdown = lv_dropdown_create(header);
    lv_obj_set_width(s_session_dropdown, 340);
    lv_obj_set_height(s_session_dropdown, 44);
    lv_obj_set_style_text_font(s_session_dropdown, &font_kartbox_lg, 0);
    lv_dropdown_set_options(s_session_dropdown, "nenhuma sessao");
    lv_obj_add_event_cb(s_session_dropdown, session_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh_btn = lv_button_create(header);
    lv_obj_set_size(refresh_btn, 130, 44);
    /* bg/texto explicitos - mesmo motivo do botao "Ir p/ PISTA" (aba
     * CORRIDA): sem isso o botao fica sem contraste com o tema default. */
    style_action_button(refresh_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_obj_center(refresh_lbl);
    lv_obj_set_style_text_font(refresh_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(refresh_lbl, COLOR_BLUE, 0);
    lv_label_set_text(refresh_lbl, "atualizar");

    /* "Mapa" - abre o tracado da sessao selecionada no dropdown acima
     * (ver ui_show_session_map / s_map_overlay). */
    lv_obj_t *map_btn = lv_button_create(header);
    lv_obj_set_size(map_btn, 110, 44);
    style_action_button(map_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(map_btn, map_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *map_lbl = lv_label_create(map_btn);
    lv_obj_center(map_lbl);
    lv_obj_set_style_text_font(map_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(map_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(map_lbl, "Mapa");

    /* Legenda das colunas da lista de voltas abaixo - mesma ordem/
     * alinhamento dos labels criados em ui_show_session_laps() (numero,
     * tempo, delta/BEST, vel max, vel media). Comeca escondida - so faz
     * sentido depois de escolher uma sessao com voltas de fato. */
    s_laps_header = bare(col);
    lv_obj_set_size(s_laps_header, lv_pct(100), 28);
    lv_obj_set_style_pad_hor(s_laps_header, 6, 0);
    lv_obj_set_flex_flow(s_laps_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_laps_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_laps_header, LV_OBJ_FLAG_HIDDEN);

    static const char *s_laps_col_labels[] = {"VOLTA", "TEMPO", "DELTA", "VEL MAX", "VEL MED"};
    for (size_t i = 0; i < sizeof(s_laps_col_labels) / sizeof(s_laps_col_labels[0]); i++) {
        lv_obj_t *col_lbl = lv_label_create(s_laps_header);
        lv_obj_set_style_text_font(col_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(col_lbl, COLOR_MUTED, 0);
        lv_label_set_text(col_lbl, s_laps_col_labels[i]);
    }

    s_laps_list = bare(col);
    lv_obj_set_size(s_laps_list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_laps_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_laps_list, 6, 0);
    lv_obj_add_flag(s_laps_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_laps_list, LV_DIR_VER);

    /* Leitura de volta-a-volta do CSV da sessao selecionada ainda
     * depende de uma funcao nova em sd_logger.h (ex: sd_read_session_laps).
     * Por enquanto a lista so confirma a selecao - dado completo entra
     * numa proxima etapa. */
    lv_obj_t *placeholder = lv_label_create(s_laps_list);
    lv_obj_set_style_text_font(placeholder, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(placeholder, COLOR_MUTED, 0);
    lv_label_set_text(placeholder, "Selecione uma sessao acima");
}

/* ----------------------------------------------------------------------
 * Aba Config
 * ---------------------------------------------------------------------- */
static void update_utc_label(void)
{
    int16_t off = settings_get_utc_offset_min();
    lv_label_set_text_fmt(s_utc_value_label, "UTC%+d:%02d", off / 60, abs(off % 60));
}

static void update_gate_label(void)
{
    lv_label_set_text_fmt(s_gate_value_label, "%.0fm", (double)settings_get_gate_radius_m());
}

static void utc_minus_cb(lv_event_t *e)
{
    (void)e;
    int16_t v = settings_get_utc_offset_min() - 15;
    if (v < -720) v = -720; /* limites razoaveis (+-12h) */
    settings_set_utc_offset_min(v);
    gps_set_utc_offset_min(v);
    update_utc_label();
}

static void utc_plus_cb(lv_event_t *e)
{
    (void)e;
    int16_t v = settings_get_utc_offset_min() + 15;
    if (v > 720) v = 720;
    settings_set_utc_offset_min(v);
    gps_set_utc_offset_min(v);
    update_utc_label();
}

static void gate_minus_cb(lv_event_t *e)
{
    (void)e;
    float v = settings_get_gate_radius_m() - 1.0f;
    if (v < 5.0f) v = 5.0f; /* abaixo disso o GPS de consumo nao tem precisao confiavel */
    settings_set_gate_radius_m(v);
    gps_set_gate_radius_m(v);
    update_gate_label();
}

static void gate_plus_cb(lv_event_t *e)
{
    (void)e;
    float v = settings_get_gate_radius_m() + 1.0f;
    if (v > 30.0f) v = 30.0f;
    settings_set_gate_radius_m(v);
    gps_set_gate_radius_m(v);
    update_gate_label();
}

static void update_min_lap_label(void)
{
    uint32_t ms = settings_get_min_lap_time_ms();
    lv_label_set_text_fmt(s_min_lap_value_label, "%.0fs", ms / 1000.0);
}

static void min_lap_minus_cb(lv_event_t *e)
{
    (void)e;
    uint32_t v = settings_get_min_lap_time_ms();
    if (v > 5000) v -= 1000;
    settings_set_min_lap_time_ms(v);
    gps_set_min_lap_time_ms(v);
    update_min_lap_label();
}

static void min_lap_plus_cb(lv_event_t *e)
{
    (void)e;
    uint32_t v = settings_get_min_lap_time_ms();
    if (v < 120000) v += 1000;
    settings_set_min_lap_time_ms(v);
    gps_set_min_lap_time_ms(v);
    update_min_lap_label();
}

/* Restaura FUSO/GATE/MIN VOLTA pros defaults de fabrica (config.h). Nao
 * mexe em pista/setores/nome BLE/senha WiFi de proposito - o usuario
 * pediu especificamente sobre os valores da aba de config numerica, e
 * apagar a pista marcada seria destrutivo demais pra um botao de "reset
 * de configuracao". */
static void restore_defaults_cb(lv_event_t *e)
{
    (void)e;
    settings_set_utc_offset_min(DEFAULT_UTC_OFFSET_MIN);
    settings_set_gate_radius_m(GATE_RADIUS_M);
    settings_set_min_lap_time_ms(MIN_LAP_TIME_MS);

    gps_set_utc_offset_min(DEFAULT_UTC_OFFSET_MIN);
    gps_set_gate_radius_m(GATE_RADIUS_M);
    gps_set_min_lap_time_ms(MIN_LAP_TIME_MS);

    update_utc_label();
    update_gate_label();
    update_min_lap_label();

    ui_show_toast("Configuracoes restauradas", 2000);
}

/* Auto-sessao - switch na CONFIG > CORRIDA. Atualiza NVS + detector do
 * GPS em runtime (sem reinicio). */
static void auto_session_switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_auto_session(on ? 1 : 0);
    gps_set_auto_session(on);
}

static lv_obj_t *make_stepper_row(lv_obj_t *parent, const char *title,
                                   lv_event_cb_t minus_cb, lv_event_cb_t plus_cb,
                                   lv_obj_t **out_value_label)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_size(row, lv_pct(100), 46);
    /* Margem de seguranca a direita: aba Config precisa rolar (mais linhas
     * do que cabem na tela) e a scrollbar reserva alguns px na borda
     * direita - sem essa folga o botao "+" (colado na borda via
     * SPACE_BETWEEN) ficava cortado. */
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, title);

    lv_obj_t *ctrl = bare(row);
    /* Tamanho explicito de conteudo - sem isso o grupo pode nao abraçar
     * os 2 botoes + valor corretamente e o botao da direita fica cortado. */
    lv_obj_set_size(ctrl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 12, 0);

    lv_obj_t *btn_minus = lv_button_create(ctrl);
    lv_obj_set_size(btn_minus, 46, 42);
    style_action_button(btn_minus, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(btn_minus, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_lbl = lv_label_create(btn_minus);
    lv_obj_center(minus_lbl);
    lv_obj_set_style_text_font(minus_lbl, &font_kartbox_lg, 0);
    lv_label_set_text(minus_lbl, "-");

    lv_obj_t *val = lv_label_create(ctrl);
    lv_obj_set_style_text_font(val, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(val, COLOR_TEXT, 0);
    /* 90px nao cabia "UTC-3:00" (9 chars) numa linha so - quebrava em
     * 2 linhas e crescia a altura da linha inteira. 130px cobre o pior
     * caso (ex: "UTC+12:00") sem quebrar. */
    lv_obj_set_width(val, 130);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(val, "--");

    lv_obj_t *btn_plus = lv_button_create(ctrl);
    lv_obj_set_size(btn_plus, 46, 42);
    style_action_button(btn_plus, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(btn_plus, plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_label_create(btn_plus);
    lv_obj_center(plus_lbl);
    lv_obj_set_style_text_font(plus_lbl, &font_kartbox_lg, 0);
    lv_label_set_text(plus_lbl, "+");

    *out_value_label = val;
    return row;
}

/* ----------------------------------------------------------------------
 * Teclado flutuante - compartilhado entre os campos de texto da Config.
 * Parented em lv_display_get_layer_top() pra flutuar acima de tudo.
 * ---------------------------------------------------------------------- */
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;

    if (code == LV_EVENT_READY) {
        lv_obj_t *ta = lv_keyboard_get_textarea(s_keyboard);
        const char *text = lv_textarea_get_text(ta);
        if (ta == s_ble_name_ta) {
            if (text && text[0]) settings_set_ble_name(text);
        } else if (ta == s_wifi_pass_ta) {
            /* settings_set_wifi_password ja valida o minimo de 8 chars */
            if (text) settings_set_wifi_password(text);
        }
    }
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_keyboard_scrim, LV_OBJ_FLAG_HIDDEN);
}

/* BUG CORRIGIDO: teclado (layer_top, cobre ~62% da tela embaixo) nao
 * tinha nenhum jeito de fechar batendo fora dele - so sumia apertando o
 * check/X do proprio teclado virtual. Na aba PISTA isso escondia o
 * botao "Salvar Pista" atras do teclado; usuario digitava o nome, tocava
 * onde achava que era o botao e o toque caia no teclado por baixo dele
 * (mais alto na pilha de z-order), sem gerar evento nenhum - sintoma
 * relatado como "clico em salvar pista e nada acontece". Fix: scrim
 * (fundo invisivel clicavel, cobre a tela inteira, fica ATRAS do teclado
 * mas NA FRENTE do conteudo da aba) que fecha o teclado ao primeiro
 * toque fora dele - revela o botao de verdade pro segundo toque. */
static void keyboard_scrim_click_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_keyboard_scrim, LV_OBJ_FLAG_HIDDEN);
}

/* Abre o teclado pro campo TOCADO. Registrado em LV_EVENT_CLICKED, NAO
 * em FOCUSED - correcao de UX reportada em campo: o LVGL foca o widget
 * no instante do PRESS, antes de saber se o gesto vai virar scroll,
 * entao qualquer rolagem da aba WIFI que começasse com o dedo sobre um
 * campo abria o teclado sem querer. CLICKED so dispara no release SEM
 * movimento - scroll nunca abre teclado, toque intencional continua
 * abrindo normal. */
static void ta_clicked_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard); /* garante teclado acima do scrim */
}

static lv_obj_t *make_text_field(lv_obj_t *parent, const char *title, const char *initial)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, title);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_height(ta, 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 63);
    lv_obj_set_style_text_font(ta, &font_kartbox_lg, 0);
    if (initial) lv_textarea_set_text(ta, initial);
    lv_obj_add_event_cb(ta, ta_clicked_cb, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void sector_mark_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_event_t evt = { .type = APP_EVT_SECTOR_MARK, .source = EVT_SRC_TOUCH };
    evt.data.param = (uint32_t)idx;
    app_event_post_data(&evt);
}

static void sector_clear_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_event_t evt = { .type = APP_EVT_SECTOR_CLEAR, .source = EVT_SRC_TOUCH };
    evt.data.param = (uint32_t)idx;
    app_event_post_data(&evt);
}

static void usb_btn_event_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_USB_MODE_TOGGLE, EVT_SRC_TOUCH);
}

/* Escala da barra de LED do delta - stepper na CONFIG > CORRIDA, passos
 * de 250ms. O valor e' a magnitude que acende a barra INTEIRA; o label
 * mostra tambem quanto vale cada LED (escala/8). Aplica na hora (o
 * refresh le o setting a cada tick). */
static lv_obj_t *s_led_scale_value_label;

static void update_led_scale_label(void)
{
    uint16_t ms = settings_get_led_scale_ms();
    lv_label_set_text_fmt(s_led_scale_value_label, "%.2fs (%ums/led)",
                          (double)(ms / 1000.0), (unsigned)(ms / DELTA_LED_PER_SIDE));
}

static void led_scale_minus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_led_scale_ms() - 250;
    settings_set_led_scale_ms((uint16_t)(v < 500 ? 500 : v));
    update_led_scale_label();
}

static void led_scale_plus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_led_scale_ms() + 250;
    settings_set_led_scale_ms((uint16_t)(v > 3000 ? 3000 : v));
    update_led_scale_label();
}

/* Brilho do display - stepper na SISTEMA, passos de 10%, aplica AO VIVO
 * (feedback imediato) e persiste. Minimo 20% (ver settings.h). */
static lv_obj_t *s_bright_value_label;

static void update_bright_label(void)
{
    lv_label_set_text_fmt(s_bright_value_label, "%u%%", (unsigned)settings_get_brightness());
}

static void bright_minus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_brightness() - 10;
    settings_set_brightness((uint8_t)(v < 20 ? 20 : v));
    display_set_brightness(settings_get_brightness());
    update_bright_label();
}

static void bright_plus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_brightness() + 10;
    settings_set_brightness((uint8_t)(v > 100 ? 100 : v));
    display_set_brightness(settings_get_brightness());
    update_bright_label();
}

/* Protetor de tela - switch de ativar + 2 steppers (escurecer/apagar).
 * Aplica so no NVS; o refresh_timer_cb (screensaver_tick) le e age. */
static lv_obj_t *s_scr_dim_value_label;
static lv_obj_t *s_scr_off_value_label;

/* Formata segundos de forma legivel: "45s", "1min", "2min30s", "nunca". */
static void fmt_duration_s(char *buf, size_t n, uint32_t sec, bool zero_is_never)
{
    if (zero_is_never && sec == 0) { snprintf(buf, n, "nunca"); return; }
    uint32_t m = sec / 60, s = sec % 60;
    if (m == 0)      snprintf(buf, n, "%us", (unsigned)s);
    else if (s == 0) snprintf(buf, n, "%umin", (unsigned)m);
    else             snprintf(buf, n, "%umin%02us", (unsigned)m, (unsigned)s);
}

static void update_scr_dim_label(void)
{
    char b[24];
    fmt_duration_s(b, sizeof(b), settings_get_screensaver_dim_s(), false);
    lv_label_set_text(s_scr_dim_value_label, b);
}

static void update_scr_off_label(void)
{
    char b[24];
    fmt_duration_s(b, sizeof(b), settings_get_screensaver_off_s(), true);
    lv_label_set_text(s_scr_off_value_label, b);
}

static void scr_enabled_switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_set_screensaver_enabled(on ? 1 : 0);
    /* desligou: garante tela acesa ja (nao espera o proximo tick) */
    if (!on) ui_screensaver_notify_activity();
}

static void scr_dim_minus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_screensaver_dim_s() - SCREENSAVER_DIM_STEP_S;
    settings_set_screensaver_dim_s((uint16_t)(v < SCREENSAVER_DIM_MIN_S ? SCREENSAVER_DIM_MIN_S : v));
    update_scr_dim_label();
}

static void scr_dim_plus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_screensaver_dim_s() + SCREENSAVER_DIM_STEP_S;
    settings_set_screensaver_dim_s((uint16_t)(v > SCREENSAVER_DIM_MAX_S ? SCREENSAVER_DIM_MAX_S : v));
    update_scr_dim_label();
}

static void scr_off_minus_cb(lv_event_t *e)
{
    (void)e;
    /* Passo pra baixo; ao chegar no minimo, cai pra 0 = "nunca". */
    int v = (int)settings_get_screensaver_off_s() - SCREENSAVER_OFF_STEP_S;
    if (v < SCREENSAVER_OFF_STEP_S) v = 0;
    settings_set_screensaver_off_s((uint16_t)v);
    update_scr_off_label();
}

static void scr_off_plus_cb(lv_event_t *e)
{
    (void)e;
    int v = (int)settings_get_screensaver_off_s() + SCREENSAVER_OFF_STEP_S;
    settings_set_screensaver_off_s((uint16_t)(v > SCREENSAVER_OFF_MAX_S ? SCREENSAVER_OFF_MAX_S : v));
    update_scr_off_label();
}

/* Backup/restore de config - I/O de SD roda no dispatcher (main), o
 * botao so posta o evento. */
static void cfg_backup_btn_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_CFG_BACKUP, EVT_SRC_TOUCH);
}

static void cfg_restore_btn_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_CFG_RESTORE, EVT_SRC_TOUCH);
}

/* Tema de cor - salva direto na NVS; aplicar ao vivo exigiria re-setar
 * estilo em dezenas de widgets ja construidos (as cores entram na
 * construcao, nao num estilo compartilhado), entao a troca vale no
 * proximo boot - toast avisa. */
static void theme_dd_event_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint8_t sel = (uint8_t)lv_dropdown_get_selected(dd);
    if (sel == settings_get_theme()) return;
    settings_set_theme(sel);
    ui_show_toast("Tema salvo - reinicie para aplicar", 2500);
}

static void wifi_btn_event_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_WIFI_EXPORT_TOGGLE, EVT_SRC_TOUCH);
}

static void wifi_mode_switch_cb(lv_event_t *e)
{
    (void)e;
    bool checked = lv_obj_has_state(s_wifi_mode_sw, LV_STATE_CHECKED);
    app_event_t evt = {
        .type = APP_EVT_WIFI_MODE_SET,
        .source = EVT_SRC_TOUCH,
    };
    evt.data.param = checked ? 1 : 0;
    app_event_post_data(&evt);
}

static void wifi_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_WIFI_SCAN, EVT_SRC_TOUCH);
}

/* Placeholders que o dropdown de redes pode estar mostrando quando NAO
 * ha SSID de verdade selecionado. */
static bool wifi_dd_is_placeholder(const char *s)
{
    return !s[0] ||
           strcmp(s, "toque em Escanear redes...") == 0 ||
           strcmp(s, "nenhuma rede encontrada") == 0;
}

/* Botao DINAMICO do modo Cliente (feedback de campo: "Conectar" +
 * "Desligar WiFi export" separados eram redundantes e confundiam):
 *   WiFi ativo    -> Desconectar (mesmo caminho do antigo botao mestre)
 *   WiFi desligado -> Conectar na rede escolhida no dropdown + senha.
 * O SSID vem SO do dropdown (o campo de texto REDE duplicado foi
 * removido) - com fallback pra rede salva na NVS quando o dropdown
 * ainda mostra placeholder (boot sem escanear). */
static void wifi_connect_btn_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_export_is_active()) {
        app_event_post(APP_EVT_WIFI_EXPORT_TOGGLE, EVT_SRC_TOUCH);
        return;
    }

    char ssid[WIFI_SCAN_SSID_MAX] = {0};
    lv_dropdown_get_selected_str(s_wifi_scan_dd, ssid, sizeof(ssid));
    if (wifi_dd_is_placeholder(ssid)) {
        strncpy(ssid, settings_get_wifi_sta_ssid(), sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
    }
    if (!ssid[0]) {
        ui_show_toast("Escaneie e escolha uma rede primeiro", 2500);
        return;
    }

    app_event_t evt = {
        .type = APP_EVT_WIFI_STA_CONNECT,
        .source = EVT_SRC_TOUCH,
    };
    const char *pass = lv_textarea_get_text(s_wifi_sta_pass_ta);
    strncpy(evt.data.wifi_sta.ssid, ssid, sizeof(evt.data.wifi_sta.ssid) - 1);
    evt.data.wifi_sta.ssid[sizeof(evt.data.wifi_sta.ssid) - 1] = '\0';
    strncpy(evt.data.wifi_sta.password, pass ? pass : "", sizeof(evt.data.wifi_sta.password) - 1);
    evt.data.wifi_sta.password[sizeof(evt.data.wifi_sta.password) - 1] = '\0';
    app_event_post_data(&evt);
}

static void ble_enable_switch_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_BLE_RADIO_TOGGLE, EVT_SRC_TOUCH);
}

static void delete_arm_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_delete_armed = false;
    lv_label_set_text(s_delete_btn_label, "Apagar tudo (SD)");
    s_delete_arm_timer = NULL;
}

static void delete_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (!s_delete_armed) {
        s_delete_armed = true;
        lv_label_set_text(s_delete_btn_label, "Toque de novo pra confirmar");
        if (s_delete_arm_timer) lv_timer_del(s_delete_arm_timer);
        s_delete_arm_timer = lv_timer_create(delete_arm_timeout_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_delete_arm_timer, 1);
    } else {
        s_delete_armed = false;
        if (s_delete_arm_timer) { lv_timer_del(s_delete_arm_timer); s_delete_arm_timer = NULL; }
        lv_label_set_text(s_delete_btn_label, "Apagar tudo (SD)");
        app_event_post(APP_EVT_SD_DELETE_ALL, EVT_SRC_TOUCH);
    }
}

/* ---------------------------------------------------------------------- *
 * Aba PISTA — configuracao e gerenciamento de pistas salvas no SD        *
 * ---------------------------------------------------------------------- */
static void pista_setline_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_BTN_SETLINE, EVT_SRC_TOUCH);
}

static void pista_load_cb(lv_event_t *e)
{
    (void)e;
    char name[TRACK_NAME_MAX];
    lv_dropdown_get_selected_str(s_pista_track_dd, name, sizeof(name));
    if (name[0] == '\0' || strcmp(name, "(nenhuma pista salva)") == 0) return;
    app_event_t evt = { .type = APP_EVT_TRACK_LOAD, .source = EVT_SRC_TOUCH };
    strncpy(evt.data.track_name, name, sizeof(evt.data.track_name) - 1);
    app_event_post_data(&evt);
}

static void pista_save_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_TRACK_SAVE, EVT_SRC_TOUCH);
}

/* Excluir pista salva - antes apagava com 1 toque so, sem confirmacao
 * (diferente de "Apagar tudo (SD)" na aba CONFIG, que ja pede 2 toques).
 * Padronizado pro mesmo esquema aqui: 1o toque arma e troca o texto do
 * botao por "Confirmar?" por 3s, 2o toque dentro da janela executa. */
static bool          s_pista_delete_armed = false;
static lv_timer_t    *s_pista_delete_arm_timer = NULL;
static lv_obj_t      *s_pista_delete_btn_label;

static void pista_delete_arm_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_pista_delete_armed = false;
    lv_label_set_text(s_pista_delete_btn_label, "Excluir");
    s_pista_delete_arm_timer = NULL;
}

static void pista_delete_cb(lv_event_t *e)
{
    (void)e;
    char name[TRACK_NAME_MAX];
    lv_dropdown_get_selected_str(s_pista_track_dd, name, sizeof(name));
    if (name[0] == '\0' || strcmp(name, "(nenhuma pista salva)") == 0) return;

    if (!s_pista_delete_armed) {
        s_pista_delete_armed = true;
        lv_label_set_text(s_pista_delete_btn_label, "Confirmar?");
        if (s_pista_delete_arm_timer) lv_timer_del(s_pista_delete_arm_timer);
        s_pista_delete_arm_timer = lv_timer_create(pista_delete_arm_timeout_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_pista_delete_arm_timer, 1);
        return;
    }

    s_pista_delete_armed = false;
    if (s_pista_delete_arm_timer) { lv_timer_del(s_pista_delete_arm_timer); s_pista_delete_arm_timer = NULL; }
    lv_label_set_text(s_pista_delete_btn_label, "Excluir");

    app_event_t evt = { .type = APP_EVT_TRACK_DELETE, .source = EVT_SRC_TOUCH };
    strncpy(evt.data.track_name, name, sizeof(evt.data.track_name) - 1);
    app_event_post_data(&evt);
}

/* Editar pista existente - mesma aplicacao ao GPS que "Carregar" (feita
 * no dispatcher, main.c), mas abre o painel de campos na propria aba
 * PISTA em vez de navegar pra CORRIDA (ver APP_EVT_TRACK_EDIT). */
static void pista_edit_cb(lv_event_t *e)
{
    (void)e;
    char name[TRACK_NAME_MAX];
    lv_dropdown_get_selected_str(s_pista_track_dd, name, sizeof(name));
    if (name[0] == '\0' || strcmp(name, "(nenhuma pista salva)") == 0) return;
    app_event_t evt = { .type = APP_EVT_TRACK_EDIT, .source = EVT_SRC_TOUCH };
    strncpy(evt.data.track_name, name, sizeof(evt.data.track_name) - 1);
    app_event_post_data(&evt);
}

/* Mostra/esconde a row de Carregar/Editar/Excluir conforme o dropdown
 * ter (ou nao) uma pista de verdade selecionada. Chamada tanto ao trocar
 * selecao quanto apos repopular a lista (ui_refresh_track_list), que nao
 * dispara LV_EVENT_VALUE_CHANGED sozinho. */
static void pista_update_select_actions_visibility(void)
{
    char name[TRACK_NAME_MAX];
    lv_dropdown_get_selected_str(s_pista_track_dd, name, sizeof(name));
    bool has_track = (name[0] != '\0' && strcmp(name, "(nenhuma pista salva)") != 0);
    if (has_track) {
        lv_obj_clear_flag(s_pista_select_actions, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pista_select_actions, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Troca de selecao no dropdown enquanto "Excluir" esta armado (esperando
 * o 2o toque) desarma a confirmacao - senao o 2o toque apagaria a pista
 * ATUALMENTE selecionada, que pode nao ser mais a que o usuario confirmou
 * no 1o toque. Tambem atualiza a visibilidade da row Carregar/Editar/
 * Excluir pra pista recem-selecionada. */
static void pista_track_dd_change_cb(lv_event_t *e)
{
    (void)e;
    if (s_pista_delete_armed) {
        s_pista_delete_armed = false;
        if (s_pista_delete_arm_timer) { lv_timer_del(s_pista_delete_arm_timer); s_pista_delete_arm_timer = NULL; }
        lv_label_set_text(s_pista_delete_btn_label, "Excluir");
    }
    pista_update_select_actions_visibility();
}

static void pista_new_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_pista_name_ta, "");
    app_event_post(APP_EVT_TRACK_NEW, EVT_SRC_TOUCH);
}

/* Fabrica de linha de ponto (label + status + coordenada + botoes opcionais).
 * out_coord e' opcional (passe NULL se o chamador nao precisar mostrar
 * coordenada - nenhum chamador atual faz isso, mas deixa a funcao generica).
 * Pedido do usuario: mostrar lat/lon de CHEGADA/S1/S2 na aba PISTA, nao so
 * "definida"/"nao definida" - status e coordenada agora ficam em coluna,
 * status em cima (fonte normal) e coordenada embaixo (fonte pequena,
 * cor mais apagada - informacao secundaria de conferencia, nao o foco
 * principal da linha). Row virou LV_SIZE_CONTENT (era 44 fixo) pra caber
 * as duas linhas de texto sem cortar. */
static lv_obj_t *make_pista_point_row(lv_obj_t *parent, const char *lbl_txt,
                                       lv_obj_t **out_status, lv_obj_t **out_coord)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_obj_set_width(lbl, 118);
    lv_label_set_text(lbl, lbl_txt);

    lv_obj_t *status_col = bare(row);
    lv_obj_set_width(status_col, lv_pct(100));
    lv_obj_set_height(status_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(status_col, 1);
    lv_obj_set_flex_flow(status_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    *out_status = lv_label_create(status_col);
    lv_obj_set_style_text_font(*out_status, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(*out_status, COLOR_MUTED, 0);
    lv_label_set_text(*out_status, "nao definida");

    if (out_coord) {
        *out_coord = lv_label_create(status_col);
        lv_obj_set_style_text_font(*out_coord, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(*out_coord, COLOR_MUTED2, 0);
        lv_label_set_text(*out_coord, "");
    }

    return row;
}

static lv_obj_t *make_pista_btn(lv_obj_t *parent, const char *txt, int w,
                                  lv_color_t bg, lv_color_t fg,
                                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 42);
    /* helper generico com bg variavel por chamada - clareamento
     * percentual (lv_color_lighten) faz sentido aqui, ao contrario dos
     * botoes com tons *_DIM especificos (ver style_action_button). */
    style_action_button(btn, bg, lv_color_lighten(bg, 40));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_label_set_text(lbl, txt);
    return btn;
}

/* Cabecalho de secao numerado + dica curta de 1 linha - usado pra deixar
 * o fluxo da aba PISTA guiado passo-a-passo (pedido do usuario: hoje e'
 * so uma lista de campos, sem indicacao de ordem/o que fazer). hint pode
 * ser NULL pra secoes autoexplicativas. */
static void make_section_header(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *title_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(title_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_BLUE, 0);
    lv_label_set_text(title_lbl, title);

    if (hint) {
        lv_obj_t *hint_lbl = lv_label_create(parent);
        lv_obj_set_width(hint_lbl, lv_pct(100));
        lv_label_set_long_mode(hint_lbl, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(hint_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(hint_lbl, COLOR_MUTED, 0);
        lv_label_set_text(hint_lbl, hint);
    }
}

static void build_pista_tab(lv_obj_t *parent)
{
    lv_obj_t *col = bare(parent);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_hor(col, 14, 0);
    lv_obj_set_style_pad_ver(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    /* Aba reorganizada (2a rodada, pedido do usuario): antes os campos de
     * NOME/CHEGADA/S1/S2 ficavam sempre visiveis, com o dropdown de pistas
     * salvas so' aparecendo por ultimo la embaixo. Agora o ponto de entrada
     * e' "Nova Pista" + dropdown, sempre no topo; os campos de edicao so
     * aparecem quando ha algo pra editar de verdade (pista nova ou
     * "Editar" numa existente) - ver s_pista_edit_panel/
     * ui_show_pista_edit_panel(). Selecionar uma pista no dropdown revela
     * Carregar/Editar/Excluir (s_pista_select_actions). */

    make_section_header(col, "PISTAS",
                         "Escolha uma pista salva pra carregar ou editar, ou toque Nova Pista pra comecar do zero.");

    lv_obj_t *new_btn_row = bare(col);
    lv_obj_set_size(new_btn_row, lv_pct(100), 44);
    lv_obj_set_flex_flow(new_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(new_btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *new_btn = lv_button_create(new_btn_row);
    lv_obj_set_size(new_btn, 170, 44);
    style_action_button(new_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(new_btn, pista_new_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_obj_center(new_lbl);
    lv_obj_set_style_text_font(new_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(new_lbl, COLOR_BLUE, 0);
    lv_label_set_text(new_lbl, "Nova Pista");

    /* Legenda dedicada do dropdown - antes so havia a dica geral da secao
     * "PISTAS" la em cima, que ficava visualmente mais associada ao botao
     * Nova Pista (primeira coisa da linha) do que ao dropdown em si.
     * Pedido do usuario: deixar claro que e' ALI que se escolhe a pista
     * a carregar/editar - mesmo padrao label-fixo-a-esquerda usado em
     * NOME/CHEGADA/S1/S2 mais abaixo. */
    lv_obj_t *dd_row = bare(col);
    lv_obj_set_size(dd_row, lv_pct(100), 46);
    lv_obj_set_flex_flow(dd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dd_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dd_row, 10, 0);

    lv_obj_t *dd_lbl = lv_label_create(dd_row);
    lv_obj_set_style_text_font(dd_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(dd_lbl, COLOR_MUTED, 0);
    lv_obj_set_width(dd_lbl, 118);
    lv_label_set_text(dd_lbl, "PISTA");

    s_pista_track_dd = lv_dropdown_create(dd_row);
    lv_obj_set_flex_grow(s_pista_track_dd, 1);
    lv_obj_set_height(s_pista_track_dd, 44);
    lv_obj_set_style_text_font(s_pista_track_dd, &font_kartbox_lg, 0);
    lv_dropdown_set_options(s_pista_track_dd, "(nenhuma pista salva)");
    lv_obj_add_event_cb(s_pista_track_dd, pista_track_dd_change_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- Carregar / Editar / Excluir - so' aparece com pista selecionada ---- */
    s_pista_select_actions = bare(col);
    lv_obj_set_size(s_pista_select_actions, lv_pct(100), 48);
    lv_obj_set_flex_flow(s_pista_select_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pista_select_actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_pista_select_actions, LV_OBJ_FLAG_HIDDEN);

    make_pista_btn(s_pista_select_actions, "Carregar", 130,
                   COLOR_PRIMARY_DIM, COLOR_PRIMARY, pista_load_cb, NULL);
    make_pista_btn(s_pista_select_actions, "Editar", 120,
                   lv_color_make(0x12, 0x28, 0x38), COLOR_BLUE, pista_edit_cb, NULL);
    lv_obj_t *delete_btn = make_pista_btn(s_pista_select_actions, "Excluir", 118,
                   COLOR_RED_DIM, COLOR_RED, pista_delete_cb, NULL);
    s_pista_delete_btn_label = lv_obj_get_child(delete_btn, 0);

    /* ---- Divider ---- */
    lv_obj_t *div0 = bare(col);
    lv_obj_set_size(div0, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div0, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div0, LV_OPA_COVER, 0);

    /* ---- Painel de edicao - escondido ate "Nova Pista"/"Editar" ---- */
    s_pista_edit_panel = bare(col);
    lv_obj_set_size(s_pista_edit_panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_pista_edit_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pista_edit_panel, 10, 0);
    lv_obj_add_flag(s_pista_edit_panel, LV_OBJ_FLAG_HIDDEN);

    /* ---- 1. Nome da pista ---- */
    make_section_header(s_pista_edit_panel, "1. NOME DA PISTA",
                         "De um nome pra essa pista - vai aparecer na lista de pistas salvas.");

    lv_obj_t *name_row = bare(s_pista_edit_panel);
    lv_obj_set_size(name_row, lv_pct(100), 46);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(name_row, 10, 0);
    lv_obj_t *name_lbl = lv_label_create(name_row);
    lv_obj_set_style_text_font(name_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_MUTED, 0);
    lv_obj_set_width(name_lbl, 118);
    lv_label_set_text(name_lbl, "NOME");
    s_pista_name_ta = lv_textarea_create(name_row);
    lv_obj_set_flex_grow(s_pista_name_ta, 1);
    lv_obj_set_height(s_pista_name_ta, 44);
    lv_textarea_set_one_line(s_pista_name_ta, true);
    lv_textarea_set_max_length(s_pista_name_ta, TRACK_NAME_MAX - 1);
    lv_obj_set_style_text_font(s_pista_name_ta, &font_kartbox_lg, 0);
    lv_obj_add_event_cb(s_pista_name_ta, ta_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* ---- Divider ---- */
    lv_obj_t *div1 = bare(s_pista_edit_panel);
    lv_obj_set_size(div1, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div1, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_COVER, 0);

    /* ---- 2. Linha de chegada e setores ---- */
    make_section_header(s_pista_edit_panel, "2. LINHA DE CHEGADA E SETORES",
                         "Dirija ate cada ponto e toque Marcar. S1/S2 sao opcionais - so servem pra ver tempos parciais durante a corrida.");

    lv_obj_t *finish_row;
    finish_row = make_pista_point_row(s_pista_edit_panel, "CHEGADA", &s_pista_finish_status, &s_pista_finish_coord);
    make_pista_btn(finish_row, "Marcar", 96,
                   COLOR_PRIMARY_DIM, COLOR_PRIMARY, pista_setline_cb, NULL);

    static const char *sec_names[GPS_MAX_SECTORS] = { "S1", "S2" };
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        lv_obj_t *srow = make_pista_point_row(s_pista_edit_panel, sec_names[i], &s_lbl_sector_status[i], &s_lbl_sector_coord[i]);
        make_pista_btn(srow, "Marcar", 96,
                       COLOR_PRIMARY_DIM, COLOR_PRIMARY, sector_mark_cb, (void *)(intptr_t)i);
        make_pista_btn(srow, "Limpar", 84,
                       COLOR_RED_DIM, COLOR_RED, sector_clear_cb, (void *)(intptr_t)i);
    }

    /* ---- Divider ---- */
    lv_obj_t *div2 = bare(s_pista_edit_panel);
    lv_obj_set_size(div2, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div2, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);

    /* ---- Salvar ---- */
    lv_obj_t *act_row = bare(s_pista_edit_panel);
    lv_obj_set_size(act_row, lv_pct(100), 48);
    lv_obj_set_flex_flow(act_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *save_btn = lv_button_create(act_row);
    lv_obj_set_size(save_btn, 240, 46);
    style_action_button(save_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(save_btn, pista_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_font(save_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(save_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(save_lbl, "Salvar Pista");
}

/* ----------------------------------------------------------------------
 * Sub-abas da aba CONFIG (Sistema/Corrida/WiFi/BLE) - mesma logica de
 * show/hide da tab bar principal (show_tab), aplicada num segundo nivel
 * so dentro do Config pra organizar os controles que foram se acumulando
 * numa coluna unica (relatado como usabilidade ruim).
 * ---------------------------------------------------------------------- */
static void show_config_subtab(int idx)
{
    for (int i = 0; i < 4; i++) {
        if (i == idx) {
            lv_obj_clear_flag(s_cfgsub_content[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_cfgsub_lbl[i], COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_color(s_cfgsub_lbl[i], COLOR_PRIMARY_DIM, 0);
            lv_obj_set_style_bg_opa(s_cfgsub_lbl[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_add_flag(s_cfgsub_content[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_cfgsub_lbl[i], COLOR_MUTED, 0);
            lv_obj_set_style_bg_opa(s_cfgsub_lbl[i], LV_OPA_TRANSP, 0);
        }
    }
}

static void config_subtab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_config_subtab(idx);
}

static void build_config_tab(lv_obj_t *parent)
{
    lv_obj_t *root = bare(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    /* Sub-tab bar - reaproveita make_tab_btn() da barra principal (mesmo
     * visual), so que dentro da propria aba Config. */
    lv_obj_t *subbar = bare(root);
    lv_obj_set_size(subbar, lv_pct(100), 48);
    lv_obj_set_style_bg_color(subbar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(subbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(subbar, 1, 0);
    lv_obj_set_style_border_color(subbar, COLOR_SURFACE_HDR, 0);
    lv_obj_set_style_border_side(subbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(subbar, LV_FLEX_FLOW_ROW);

    static const char *subtab_names[4] = { "SISTEMA", "CORRIDA", "WIFI", "BLE" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = make_tab_btn(subbar, subtab_names[i], &s_cfgsub_lbl[i]);
        lv_obj_add_event_cb(btn, config_subtab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    lv_obj_t *pages = bare(root);
    lv_obj_set_width(pages, lv_pct(100));
    lv_obj_set_flex_grow(pages, 1);

    /* ------------------------------------------------------------------
     * SISTEMA - status GPS, uso do SD, modo pen drive, apagar tudo.
     * ------------------------------------------------------------------ */
    lv_obj_t *col_sys = bare(pages);
    lv_obj_set_size(col_sys, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col_sys, 14, 0);
    lv_obj_set_flex_flow(col_sys, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_sys, 10, 0);
    lv_obj_add_flag(col_sys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col_sys, LV_DIR_VER);
    s_cfgsub_content[0] = col_sys;

    lv_obj_t *gps_link_row = bare(col_sys);
    lv_obj_set_size(gps_link_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(gps_link_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gps_link_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *gps_link_lbl = lv_label_create(gps_link_row);
    lv_obj_set_style_text_font(gps_link_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(gps_link_lbl, COLOR_MUTED, 0);
    lv_label_set_text(gps_link_lbl, "Status GPS");

    s_gps_link_label = lv_label_create(gps_link_row);
    lv_obj_set_style_text_font(s_gps_link_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_gps_link_label, COLOR_MUTED, 0);
    lv_label_set_text(s_gps_link_label, "--");

    /* Diagnostico de RF - mesmos numeros do log serial, na tela: SABE
     * (almanaque/A-GPS) vs OUVE (SNR real) por constelacao. Permite
     * diagnosticar antena/EMI/posicionamento no kartodromo sem laptop.
     * Regra de bolso no proprio rotulo pro piloto: ouvindo 8+ e snr 38+
     * = saudavel; ouvindo 0-2 = sinal nao chega (antena/EMI). */
    lv_obj_t *rf_row = bare(col_sys);
    lv_obj_set_size(rf_row, lv_pct(100), 64);
    lv_obj_set_flex_flow(rf_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rf_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *rf_lbl = lv_label_create(rf_row);
    lv_obj_set_style_text_font(rf_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(rf_lbl, COLOR_MUTED, 0);
    lv_label_set_text(rf_lbl, "RF GPS\n(gps/glo/gal/bds)");

    s_gps_rf_label = lv_label_create(rf_row);
    lv_obj_set_style_text_font(s_gps_rf_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_gps_rf_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(s_gps_rf_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_gps_rf_label, "sabe --\nouve --");

    /* Autoteste - "por que nao esta gravando?" respondido em 5s no
     * kartodromo: SD montado, GPS falando, host BLE sincronizado com o
     * C6 (proxy da saude do link SDIO/esp_hosted). Recolor por item. */
    lv_obj_t *health_row = bare(col_sys);
    lv_obj_set_size(health_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(health_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(health_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *health_lbl = lv_label_create(health_row);
    lv_obj_set_style_text_font(health_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(health_lbl, COLOR_MUTED, 0);
    lv_label_set_text(health_lbl, "AUTOTESTE");

    s_health_label = lv_label_create(health_row);
    lv_obj_set_style_text_font(s_health_label, &font_kartbox_lg, 0);
    lv_label_set_recolor(s_health_label, true);
    lv_label_set_text(s_health_label, "--");

    lv_obj_t *sd_row = bare(col_sys);
    lv_obj_set_size(sd_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(sd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sd_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_sd_usage_label = lv_label_create(sd_row);
    lv_obj_set_style_text_font(s_sd_usage_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_sd_usage_label, COLOR_TEXT, 0);
    lv_label_set_text(s_sd_usage_label, "SD --");

    s_sd_usage_bar = lv_bar_create(sd_row);
    lv_obj_set_size(s_sd_usage_bar, 220, 10);
    lv_bar_set_range(s_sd_usage_bar, 0, 100);
    lv_bar_set_value(s_sd_usage_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sd_usage_bar, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sd_usage_bar, COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sd_usage_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sd_usage_bar, 4, LV_PART_INDICATOR);

    /* Tema de cor da UI - dropdown com as paletas de THEME_PALETTES.
     * Ordem das opcoes DEVE casar com os indices da tabela (e com o
     * comentario em settings.h). Sem opcao vermelha de proposito. */
    lv_obj_t *theme_row = bare(col_sys);
    lv_obj_set_size(theme_row, lv_pct(100), 48);
    lv_obj_set_flex_flow(theme_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(theme_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *theme_lbl = lv_label_create(theme_row);
    lv_obj_set_style_text_font(theme_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(theme_lbl, COLOR_MUTED, 0);
    lv_label_set_text(theme_lbl, "TEMA (cor da UI)");

    lv_obj_t *theme_dd = lv_dropdown_create(theme_row);
    lv_obj_set_size(theme_dd, 220, 44);
    lv_obj_set_style_text_font(theme_dd, &font_kartbox_lg, 0);
    lv_dropdown_set_options(theme_dd, "Verde\nAzul\nAmbar\nLaranja\nRoxo");
    lv_dropdown_set_selected(theme_dd, settings_get_theme());
    lv_obj_add_event_cb(theme_dd, theme_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *usb_btn = lv_button_create(col_sys);
    lv_obj_set_size(usb_btn, lv_pct(100), 52);
    style_action_button(usb_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(usb_btn, usb_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_usb_btn_label = lv_label_create(usb_btn);
    lv_obj_center(s_usb_btn_label);
    lv_obj_set_style_text_font(s_usb_btn_label, &font_kartbox_lg, 0);
    lv_label_set_text(s_usb_btn_label, "Modo pen drive (USB)");

    lv_obj_t *del_btn = lv_button_create(col_sys);
    lv_obj_set_size(del_btn, lv_pct(100), 52);
    style_action_button(del_btn, COLOR_RED_DIM, lv_color_lighten(COLOR_RED_DIM, 50));
    lv_obj_add_event_cb(del_btn, delete_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_delete_btn_label = lv_label_create(del_btn);
    lv_obj_center(s_delete_btn_label);
    lv_obj_set_style_text_font(s_delete_btn_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_delete_btn_label, COLOR_RED, 0);
    lv_label_set_text(s_delete_btn_label, "Apagar tudo (SD)");

    /* Brilho do display - ao vivo + NVS */
    make_stepper_row(col_sys, "BRILHO DA TELA", bright_minus_cb, bright_plus_cb, &s_bright_value_label);
    update_bright_label();

    /* Protetor de tela - economia de bateria. Switch + tempos pra escurecer
     * e apagar (o kart andando ou sessao gravando sempre mantem aceso). */
    lv_obj_t *scr_row = bare(col_sys);
    lv_obj_set_size(scr_row, lv_pct(100), 46);
    lv_obj_set_style_pad_right(scr_row, 14, 0);
    lv_obj_set_flex_flow(scr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *scr_lbl = lv_label_create(scr_row);
    lv_obj_set_style_text_font(scr_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(scr_lbl, COLOR_MUTED, 0);
    lv_label_set_text(scr_lbl, "PROTETOR DE TELA");

    lv_obj_t *scr_sw = lv_switch_create(scr_row);
    lv_obj_set_style_bg_color(scr_sw, COLOR_PRIMARY_DIM, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (settings_get_screensaver_enabled()) lv_obj_add_state(scr_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(scr_sw, scr_enabled_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_stepper_row(col_sys, "ESCURECER APOS", scr_dim_minus_cb, scr_dim_plus_cb, &s_scr_dim_value_label);
    make_stepper_row(col_sys, "APAGAR APOS",    scr_off_minus_cb, scr_off_plus_cb, &s_scr_off_value_label);
    update_scr_dim_label();
    update_scr_off_label();

    /* Backup/restore das configuracoes no SD - util antes de OTA grande,
     * troca de placa ou reset. Arquivo texto legivel na raiz do cartao. */
    lv_obj_t *cfg_bkp_row = bare(col_sys);
    lv_obj_set_size(cfg_bkp_row, lv_pct(100), 52);
    lv_obj_set_flex_flow(cfg_bkp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cfg_bkp_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cfg_bkp_row, 10, 0);

    lv_obj_t *bkp_btn = lv_button_create(cfg_bkp_row);
    lv_obj_set_size(bkp_btn, lv_pct(48), 52);
    style_action_button(bkp_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(bkp_btn, cfg_backup_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bkp_lbl = lv_label_create(bkp_btn);
    lv_obj_center(bkp_lbl);
    lv_obj_set_style_text_font(bkp_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(bkp_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(bkp_lbl, "Salvar config no SD");

    lv_obj_t *rst_btn = lv_button_create(cfg_bkp_row);
    lv_obj_set_size(rst_btn, lv_pct(48), 52);
    style_action_button(rst_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(rst_btn, cfg_restore_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rst_lbl = lv_label_create(rst_btn);
    lv_obj_center(rst_lbl);
    lv_obj_set_style_text_font(rst_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(rst_lbl, COLOR_BLUE, 0);
    lv_label_set_text(rst_lbl, "Restaurar do SD");

    /* Versao do firmware - com OTA em uso, "que versao esta rodando?"
     * precisa ter resposta na propria tela (a pagina do export tambem
     * mostra, mas exige ligar o WiFi). Vem do esp_app_desc (hash do git
     * via idf.py, ex: "8cf1fb3-dirty"). */
    lv_obj_t *fw_row = bare(col_sys);
    lv_obj_set_size(fw_row, lv_pct(100), 36);
    lv_obj_set_flex_flow(fw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *fw_lbl = lv_label_create(fw_row);
    lv_obj_set_style_text_font(fw_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(fw_lbl, COLOR_MUTED, 0);
    lv_label_set_text(fw_lbl, "FIRMWARE");

    s_fw_version_label = lv_label_create(fw_row);
    lv_obj_set_style_text_font(s_fw_version_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_fw_version_label, COLOR_MUTED2, 0);
    lv_label_set_text_fmt(s_fw_version_label, "%s (%s)",
                          esp_app_get_description()->version,
                          esp_app_get_description()->date);

    /* ------------------------------------------------------------------
     * CORRIDA - fuso horario, raio do gate, tempo minimo de volta.
     * ------------------------------------------------------------------ */
    lv_obj_t *col_race = bare(pages);
    lv_obj_set_size(col_race, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col_race, 14, 0);
    lv_obj_set_flex_flow(col_race, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_race, 10, 0);
    lv_obj_add_flag(col_race, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col_race, LV_DIR_VER);
    lv_obj_add_flag(col_race, LV_OBJ_FLAG_HIDDEN);
    s_cfgsub_content[1] = col_race;

    make_stepper_row(col_race, "FUSO HORARIO", utc_minus_cb, utc_plus_cb, &s_utc_value_label);
    make_stepper_row(col_race, "RAIO DO GATE", gate_minus_cb, gate_plus_cb, &s_gate_value_label);
    make_stepper_row(col_race, "MIN VOLTA",    min_lap_minus_cb, min_lap_plus_cb, &s_min_lap_value_label);
    make_stepper_row(col_race, "ESCALA LED (delta)", led_scale_minus_cb, led_scale_plus_cb, &s_led_scale_value_label);
    update_utc_label();
    update_gate_label();
    update_min_lap_label();
    update_led_scale_label();

    /* Auto-sessao - inicia sozinho quando o kart anda (pista carregada),
     * encerra sozinho depois de muito tempo parado. Ver AUTO_SESSION_*
     * em config.h pros limiares. */
    lv_obj_t *auto_row = bare(col_race);
    lv_obj_set_size(auto_row, lv_pct(100), 46);
    lv_obj_set_style_pad_right(auto_row, 14, 0);
    lv_obj_set_flex_flow(auto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_lbl = lv_label_create(auto_row);
    lv_obj_set_style_text_font(auto_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(auto_lbl, COLOR_MUTED, 0);
    lv_label_set_text(auto_lbl, "AUTO-SESSAO (inicia/encerra sozinho)");

    lv_obj_t *auto_sw = lv_switch_create(auto_row);
    lv_obj_set_style_bg_color(auto_sw, COLOR_PRIMARY_DIM, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (settings_get_auto_session()) lv_obj_add_state(auto_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(auto_sw, auto_session_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *restore_btn = lv_button_create(col_race);
    lv_obj_set_size(restore_btn, lv_pct(100), 42);
    style_action_button(restore_btn, lv_color_make(0x1E, 0x1A, 0x08), COLOR_ACCENT_GOLD);
    lv_obj_add_event_cb(restore_btn, restore_defaults_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restore_lbl = lv_label_create(restore_btn);
    lv_obj_center(restore_lbl);
    lv_obj_set_style_text_font(restore_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(restore_lbl, COLOR_GOLD, 0);
    lv_label_set_text(restore_lbl, "Restaurar padroes (fuso/gate/min volta)");

    /* ------------------------------------------------------------------
     * WIFI - Modo AP proprio (rede isolada "KartBox-XXXX", sempre
     * funciona) vs Cliente/STA (conecta na rede do usuario -
     * kartbox.local, celular nem precisa trocar de rede pra
     * sincronizar). So pode trocar com o wifi desligado - ver guarda em
     * APP_EVT_WIFI_MODE_SET no main.c.
     *
     * Layout revisado (usabilidade ruim reportada - tudo numa coluna so
     * sem separacao visual nenhuma). Ordem agora: 1) escolha de modo no
     * topo, 2) divisor, 3) UM bloco de configuracao especifico do modo
     * escolhido (AP ou Cliente - so um visivel por vez, com titulo de
     * secao proprio), 4) divisor, 5) controle mestre (ligar/desligar o
     * export) sempre visivel embaixo, igual em qualquer modo.
     * ------------------------------------------------------------------ */
    lv_obj_t *col_wifi = bare(pages);
    lv_obj_set_size(col_wifi, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col_wifi, 14, 0);
    lv_obj_set_flex_flow(col_wifi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_wifi, 14, 0);
    lv_obj_add_flag(col_wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col_wifi, LV_DIR_VER);
    lv_obj_add_flag(col_wifi, LV_OBJ_FLAG_HIDDEN);
    s_cfgsub_content[2] = col_wifi;

    lv_obj_t *wifi_mode_row = bare(col_wifi);
    lv_obj_set_size(wifi_mode_row, lv_pct(100), 44);
    lv_obj_set_flex_flow(wifi_mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_mode_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wifi_mode_lbl = lv_label_create(wifi_mode_row);
    lv_obj_set_style_text_font(wifi_mode_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(wifi_mode_lbl, COLOR_MUTED, 0);
    lv_label_set_text(wifi_mode_lbl, "Modo WiFi");

    s_wifi_mode_status_lbl = lv_label_create(wifi_mode_row);
    lv_obj_set_style_text_font(s_wifi_mode_status_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_wifi_mode_status_lbl, COLOR_MUTED, 0);
    lv_label_set_text(s_wifi_mode_status_lbl, "AP proprio");

    s_wifi_mode_sw = lv_switch_create(wifi_mode_row);
    lv_obj_set_style_bg_color(s_wifi_mode_sw, COLOR_PRIMARY_DIM, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_wifi_mode_sw, wifi_mode_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *div_wifi1 = bare(col_wifi);
    lv_obj_set_size(div_wifi1, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div_wifi1, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div_wifi1, LV_OPA_COVER, 0);

    /* ---- Bloco AP proprio - so aparece no modo AP. ui_set_wifi_mode_ui()
     * (via apply_wifi_mode_ui) controla qual dos dois blocos (esse ou o
     * de Cliente logo abaixo) fica visivel. ---- */
    s_wifi_ap_box = bare(col_wifi);
    lv_obj_set_size(s_wifi_ap_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi_ap_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_ap_box, 10, 0);

    lv_obj_t *ap_title = lv_label_create(s_wifi_ap_box);
    lv_obj_set_style_text_font(ap_title, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(ap_title, COLOR_MUTED, 0);
    lv_label_set_text(ap_title, "REDE PROPRIA (AP)");

    s_wifi_info_label = lv_label_create(s_wifi_ap_box);
    lv_obj_set_style_text_font(s_wifi_info_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_wifi_info_label, COLOR_MUTED, 0);
    lv_label_set_text(s_wifi_info_label, "");

    s_wifi_pass_ta = make_text_field(s_wifi_ap_box, "SENHA DO AP", settings_get_wifi_password());

    lv_obj_t *wifi_note = lv_label_create(s_wifi_ap_box);
    lv_obj_set_style_text_font(wifi_note, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(wifi_note, COLOR_MUTED, 0);
    lv_label_set_text(wifi_note, "minimo 8 caracteres");

    /* Botao liga/desliga do AP - mora DENTRO do bloco AP agora (o antigo
     * "controle mestre" sempre-visivel no rodape saiu: no modo Cliente
     * ele duplicava o Conectar/Desconectar - feedback de campo). */
    lv_obj_t *wifi_btn = lv_button_create(s_wifi_ap_box);
    lv_obj_set_size(wifi_btn, lv_pct(100), 52);
    style_action_button(wifi_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_wifi_btn_label = lv_label_create(wifi_btn);
    lv_obj_center(s_wifi_btn_label);
    lv_obj_set_style_text_font(s_wifi_btn_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_wifi_btn_label, COLOR_PRIMARY, 0);
    lv_label_set_text(s_wifi_btn_label, "Ligar WiFi (AP proprio)");

    /* ---- Bloco Cliente (STA) - so aparece no modo Cliente: escanear,
     * escolher rede, senha da rede, conectar. ---- */
    s_wifi_sta_box = bare(col_wifi);
    lv_obj_set_size(s_wifi_sta_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi_sta_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_sta_box, 10, 0);

    lv_obj_t *sta_title = lv_label_create(s_wifi_sta_box);
    lv_obj_set_style_text_font(sta_title, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(sta_title, COLOR_MUTED, 0);
    lv_label_set_text(sta_title, "CONECTAR EM REDE EXISTENTE");

    /* REDE numa linha so: dropdown (que E' o seletor de SSID - o campo
     * de texto duplicado saiu) + botao Escanear ao lado. */
    lv_obj_t *net_row = bare(s_wifi_sta_box);
    lv_obj_set_size(net_row, lv_pct(100), 44);
    lv_obj_set_flex_flow(net_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(net_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(net_row, 10, 0);

    s_wifi_scan_dd = lv_dropdown_create(net_row);
    lv_obj_set_height(s_wifi_scan_dd, 44);
    lv_obj_set_flex_grow(s_wifi_scan_dd, 1);
    lv_obj_set_style_text_font(s_wifi_scan_dd, &font_kartbox_lg, 0);
    /* rede salva da ultima vez ja aparece selecionada - conectar de novo
     * na mesma rede nao exige escanear nada */
    if (settings_get_wifi_sta_ssid()[0]) {
        lv_dropdown_set_options(s_wifi_scan_dd, settings_get_wifi_sta_ssid());
    } else {
        lv_dropdown_set_options(s_wifi_scan_dd, "toque em Escanear redes...");
    }

    lv_obj_t *scan_btn = lv_button_create(net_row);
    lv_obj_set_size(scan_btn, 170, 44);
    style_action_button(scan_btn, lv_color_make(0x12, 0x28, 0x38), lv_color_make(0x1A, 0x38, 0x4C));
    lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_obj_center(scan_lbl);
    lv_obj_set_style_text_font(scan_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(scan_lbl, COLOR_BLUE, 0);
    lv_label_set_text(scan_lbl, "Escanear");

    s_wifi_sta_pass_ta = make_text_field(s_wifi_sta_box, "SENHA DA REDE", settings_get_wifi_sta_password());

    /* Botao DINAMICO: Conectar <-> Desconectar (ver wifi_connect_btn_cb).
     * Substitui o par redundante Conectar + "Desligar WiFi export". */
    lv_obj_t *connect_btn = lv_button_create(s_wifi_sta_box);
    lv_obj_set_size(connect_btn, lv_pct(100), 52);
    style_action_button(connect_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(connect_btn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);
    s_sta_connect_lbl = lv_label_create(connect_btn);
    lv_obj_center(s_sta_connect_lbl);
    lv_obj_set_style_text_font(s_sta_connect_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_sta_connect_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(s_sta_connect_lbl, "Conectar");

    s_wifi_sta_status_lbl = lv_label_create(s_wifi_sta_box);
    lv_obj_set_style_text_font(s_wifi_sta_status_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_wifi_sta_status_lbl, COLOR_MUTED, 0);
    lv_label_set_text(s_wifi_sta_status_lbl, "desconectado");

    /* Estado inicial vem da NVS (settings.c ja carregada nesse ponto -
     * ui_init() e chamado depois de settings_init() em app_main()).
     * apply_wifi_mode_ui() direto (sem lock) porque ja estamos dentro
     * do lock aqui - ver forward decl no topo do arquivo.
     * PRECISA vir depois de s_wifi_info_label existir - bug real ja
     * pego em log: modo STA salvo de sessao anterior fazia essa chamada
     * cair no branch "if (is_sta) lv_label_set_text(s_wifi_info_label,...)"
     * com o label ainda NULL (criado so mais abaixo), travando o boot
     * (watchdog IDLE0, MEPC preso dentro de lv_label_set_text). */
    apply_wifi_mode_ui((wifi_export_mode_t)settings_get_wifi_mode());

    /* ------------------------------------------------------------------
     * BLE - telemetria ao vivo (on/off) + nome do dispositivo.
     * ------------------------------------------------------------------ */
    lv_obj_t *col_ble = bare(pages);
    lv_obj_set_size(col_ble, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col_ble, 14, 0);
    lv_obj_set_flex_flow(col_ble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col_ble, 10, 0);
    lv_obj_add_flag(col_ble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col_ble, LV_DIR_VER);
    lv_obj_add_flag(col_ble, LV_OBJ_FLAG_HIDDEN);
    s_cfgsub_content[3] = col_ble;

    lv_obj_t *ble_row = bare(col_ble);
    lv_obj_set_size(ble_row, lv_pct(100), 44);
    lv_obj_set_flex_flow(ble_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ble_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ble_lbl = lv_label_create(ble_row);
    lv_obj_set_style_text_font(ble_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(ble_lbl, COLOR_MUTED, 0);
    lv_label_set_text(ble_lbl, "BLE telemetria");

    s_ble_status_label = lv_label_create(ble_row);
    lv_obj_set_style_text_font(s_ble_status_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_ble_status_label, COLOR_MUTED, 0);
    lv_label_set_text(s_ble_status_label, "desligado");

    /* Desliga so o anuncio/conexao BLE (nao desliga o radio C6 de
     * verdade - ver doc em ble_telemetry.h). Default DESLIGADO agora
     * (usuario decidiu deixar fora por padrao - ver ble_telemetry.c) -
     * liga sob demanda tocando o switch. */
    s_ble_enable_sw = lv_switch_create(ble_row);
    lv_obj_set_style_bg_color(s_ble_enable_sw, COLOR_PRIMARY_DIM, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_ble_enable_sw, ble_enable_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Campo de texto editavel - teclado criado em ui_init() */
    s_ble_name_ta = make_text_field(col_ble, "NOME BLE", settings_get_ble_name());

    lv_obj_t *ble_note = lv_label_create(col_ble);
    lv_obj_set_style_text_font(ble_note, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(ble_note, COLOR_MUTED, 0);
    lv_label_set_text(ble_note, "reinicie o kartbox pra aplicar o nome novo");

    /* Comeca na sub-aba SISTEMA - mesma escolha da tab bar principal
     * (mostra o mais "informativo pra checar rapido" primeiro). Tambem
     * corrige a cor do label da sub-aba ativa (make_tab_btn deixa todos
     * COLOR_MUTED por padrao). */
    show_config_subtab(0);
}

/* ----------------------------------------------------------------------
 * Tab bar (texto, nao icone - fonte custom do projeto e so digitos/texto,
 * sem faixa de simbolos LV_SYMBOL_* garantida)
 * ---------------------------------------------------------------------- */
static lv_obj_t *make_tab_btn(lv_obj_t *parent, const char *text, lv_obj_t **out_label)
{
    lv_obj_t *btn = bare(parent);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, text);
    /* Chip de fundo pra aba ativa (padding/raio sempre aplicados, so o
     * bg_opa alterna em show_tab()/show_config_subtab() - mantendo o
     * padding fixo nos dois estados evita a aba "pular" de tamanho ao
     * trocar de ativa pra inativa). */
    lv_obj_set_style_pad_hor(lbl, 14, 0);
    lv_obj_set_style_pad_ver(lbl, 4, 0);
    lv_obj_set_style_radius(lbl, 10, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);

    *out_label = lbl;
    return btn;
}

static void show_tab(int idx)
{
    /* Ordem: 0=PISTA, 1=CORRIDA, 2=VOLTAS, 3=CONFIG */
    lv_obj_t *contents[4] = { s_tab_content_pista, s_tab_content_race,
                               s_tab_content_laps,  s_tab_content_cfg };
    lv_obj_t *labels[4]   = { s_tab_lbl_pista, s_tab_lbl_race,
                               s_tab_lbl_laps,  s_tab_lbl_cfg };
    for (int i = 0; i < 4; i++) {
        if (i == idx) {
            lv_obj_clear_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(labels[i], COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_color(labels[i], COLOR_PRIMARY_DIM, 0);
            lv_obj_set_style_bg_opa(labels[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_add_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(labels[i], COLOR_MUTED, 0);
            lv_obj_set_style_bg_opa(labels[i], LV_OPA_TRANSP, 0);
        }
    }

    /* Barra de LED do delta so faz sentido durante a pilotagem (aba
     * CORRIDA, idx 1) - antes ficava fixa em cima em qualquer aba,
     * ocupando espaco/atencao a toa em PISTA/VOLTAS/CONFIG. */
    if (idx == 1) {
        lv_obj_clear_flag(s_delta_led_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_delta_led_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    /* Pedido do usuario: sessao gravando (CORRIDA ou QUALY rodando) trava
     * navegacao pras outras abas, pra evitar toque acidental em
     * PISTA/VOLTAS/CONFIG no meio de uma volta (ex: mexer sem querer numa
     * pista salva, ou destravar o teclado de nome). So a propria aba
     * CORRIDA (idx 1) fica acessivel - e onde a sessao ativa e' mostrada
     * e onde o RESET pra parar fica. s_session_recording_active e'
     * atualizado por ui_set_recording_state(), chamada pelo main.c em
     * start_session()/stop_session(). */
    if (s_session_recording_active && idx != 1) {
        ui_show_toast("Pare a sessao (RESET) antes de trocar de aba", 2500);
        return;
    }
    show_tab(idx);
}

static void build_tab_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = bare(parent);
    lv_obj_set_size(bar, lv_pct(100), 56);
    lv_obj_set_style_bg_color(bar, COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_SURFACE_HDR, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);

    /* 4 abas de 120px cada: PISTA | CORRIDA | VOLTAS | CONFIG */
    lv_obj_t *b0 = make_tab_btn(bar, "PISTA",   &s_tab_lbl_pista);
    lv_obj_add_event_cb(b0, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    lv_obj_t *b1 = make_tab_btn(bar, "CORRIDA", &s_tab_lbl_race);
    lv_obj_add_event_cb(b1, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    lv_obj_t *b2 = make_tab_btn(bar, "VOLTAS",  &s_tab_lbl_laps);
    lv_obj_add_event_cb(b2, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);

    lv_obj_t *b3 = make_tab_btn(bar, "CONFIG",  &s_tab_lbl_cfg);
    lv_obj_add_event_cb(b3, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)3);
}

/* Pisca o icone WIFI da status bar enquanto o export sob demanda estiver
 * de fato ativo - pedido do usuario, deixa obvio que o radio esta "em
 * uso agora" (nao so "disponivel"). Anim de opacidade infinita, ida-e-
 * volta; iniciada/parada em refresh_timer_cb() so na transicao visivel<->
 * escondido (nao a cada tick de 2s, senao reiniciaria a animacao do
 * zero toda vez). */
static void wifi_icon_blink_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* ----------------------------------------------------------------------
 * Protetor de tela - escurece e depois apaga o backlight apos um tempo
 * parado (sem toque/botao e kart abaixo de SCREENSAVER_MOVE_KMH). Nunca
 * dorme durante sessao. Fonte de "ultima atividade": a inatividade nativa
 * do LVGL (lv_display_get_inactive_time), que ja conta o TOQUE; botoes
 * fisicos e movimento do GPS resetam via lv_display_trigger_activity().
 *
 * No estagio OFF um overlay transparente no layer de topo "engole" o
 * primeiro toque - assim tocar a tela apagada so acorda, sem disparar o
 * botao que estava embaixo (ex: nao marca linha sem querer).
 * ---------------------------------------------------------------------- */
typedef enum { SCR_AWAKE, SCR_DIM, SCR_OFF } scr_state_t;
static scr_state_t s_scr_state = SCR_AWAKE;
static lv_obj_t   *s_scr_wake_overlay = NULL;

static void scr_wake_overlay_cb(lv_event_t *e)
{
    (void)e;
    lv_display_trigger_activity(NULL);   /* zera o timer de inatividade */
    /* A transicao de estado (restaura brilho + remove overlay) acontece no
     * proximo tick do refresh, ao ver a inatividade zerada. Nao mexemos no
     * overlay aqui dentro do proprio callback dele. */
}

static void screensaver_apply(scr_state_t st)
{
    if (st == s_scr_state) return;

    switch (st) {
    case SCR_AWAKE:
        display_set_brightness(settings_get_brightness());
        if (s_scr_wake_overlay) {
            lv_obj_delete(s_scr_wake_overlay);
            s_scr_wake_overlay = NULL;
        }
        break;
    case SCR_DIM:
        /* saindo do OFF de volta pra DIM (off desabilitado no meio): tira o
         * overlay tambem */
        if (s_scr_wake_overlay) {
            lv_obj_delete(s_scr_wake_overlay);
            s_scr_wake_overlay = NULL;
        }
        display_set_brightness(SCREENSAVER_DIM_LEVEL);
        break;
    case SCR_OFF:
        display_set_brightness(0);
        if (!s_scr_wake_overlay) {
            s_scr_wake_overlay = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(s_scr_wake_overlay);
            lv_obj_set_size(s_scr_wake_overlay, lv_pct(100), lv_pct(100));
            lv_obj_set_style_bg_opa(s_scr_wake_overlay, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(s_scr_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_scr_wake_overlay, scr_wake_overlay_cb, LV_EVENT_PRESSED, NULL);
        }
        break;
    }
    s_scr_state = st;
}

/* Chamada a cada tick do refresh (100ms). `moving`/`recording` bloqueiam o
 * sleep resetando a inatividade. */
static void screensaver_tick(bool moving, bool recording)
{
    if (!settings_get_screensaver_enabled()) {
        screensaver_apply(SCR_AWAKE);
        return;
    }
    if (moving || recording) {
        lv_display_trigger_activity(NULL);
    }

    uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    uint32_t dim_ms  = (uint32_t)settings_get_screensaver_dim_s() * 1000u;
    uint32_t off_s   = settings_get_screensaver_off_s();
    uint32_t off_ms  = off_s * 1000u;

    scr_state_t want;
    if (off_s != 0 && idle_ms >= off_ms)      want = SCR_OFF;
    else if (dim_ms != 0 && idle_ms >= dim_ms) want = SCR_DIM;
    else                                       want = SCR_AWAKE;
    screensaver_apply(want);
}

void ui_screensaver_notify_activity(void)
{
    if (!lvgl_port_lock(0)) return;
    lv_display_trigger_activity(NULL);
    screensaver_apply(SCR_AWAKE);   /* acorda na hora, sem esperar o tick */
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Refresh em tempo real (10Hz) - velocidade, delta, LEDs, status bar.
 * Itens lentos (SD/BLE da aba Config) so a cada 20 ticks (~2s).
 * ---------------------------------------------------------------------- */
/* Ticks (de 100ms) por meia-fase do piscar do indicador de GPS. 3 ->
 * 300ms ligado / 300ms apagado, ~1.6Hz. */
#define GPS_DOT_BLINK_TICKS 3
static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    gps_sample_t s;
    gps_get_latest(&s);

    if (s.fix_valid && s.speed_kmh > s_max_speed_kmh) {
        s_max_speed_kmh = s.speed_kmh;
    }

    /* Protetor de tela - mantem acordado se: kart andando, sessao gravando,
     * ou WiFi export ativo (inclui a janela de OTA - a tela NAO pode apagar
     * no meio de "nao desligue"). Rodamos aqui (100ms) porque ja temos o `s`
     * fresco e estamos na taskLVGL (mutex recursivo, seguro mexer no brilho). */
    screensaver_tick(s.fix_valid && s.speed_kmh > SCREENSAVER_MOVE_KMH,
                     s_session_recording_active || wifi_export_is_active());

    lv_label_set_text_fmt(s_lbl_speed_val, "%d", (int)(s.speed_kmh + 0.5f));
    lv_label_set_text_fmt(s_lbl_volta_val, "%lu", (unsigned long)s.lap_number);

    /* Celula SETORES (ex-"VEL MAX"): splits ao vivo dos setores ATIVOS -
     * os marcados manualmente (ate 2 no uso tipico) ou os 3 automaticos.
     * Adapta a quantidade de linhas ao que estiver ativo. */
    {
        /* Uma cor por setor (recolor ligado no build): S1 ciano, S2 dourado. */
        static const char *sec_hex[GPS_MAX_SECTORS] = { "3ec6e0", "ffd700" };
        char sec_txt[192];
        int pos = 0, shown = 0;
        for (int i = 0; i < GPS_MAX_SECTORS; i++) {
            if (!s.sector_is_set[i]) continue;
            char sb[16];
            if (s.sector_split_ms[i] > 0) format_lap_time(sb, sizeof(sb), s.sector_split_ms[i]);
            else                          snprintf(sb, sizeof(sb), "--");
            pos += snprintf(sec_txt + pos, sizeof(sec_txt) - (size_t)pos,
                            "%s#%s S%d %s#", shown ? "\n" : "", sec_hex[i], i + 1, sb);
            /* delta AO VIVO vs melhor setor da sessao - verde ganhando,
             * vermelho perdendo, no instante do cruzamento. E' o feedback
             * mais acionavel da tela: "perdi 0.3 no S1, ataca o resto". */
            if (s.sector_split_ms[i] > 0 && s.sector_delta_valid[i] &&
                pos < (int)sizeof(sec_txt) - 1) {
                pos += snprintf(sec_txt + pos, sizeof(sec_txt) - (size_t)pos,
                                " #%s (%+.2f)#",
                                s.sector_delta_ms[i] <= 0 ? "3ee07a" : "ff5a5a",
                                (double)(s.sector_delta_ms[i] / 1000.0));
            }
            shown++;
            if (pos >= (int)sizeof(sec_txt) - 1) break;
        }
        if (shown == 0) snprintf(sec_txt, sizeof(sec_txt), "sem setor");
        lv_label_set_text(s_lbl_velmax_val, sec_txt);
    }

    char time_buf[16];
    format_lap_time(time_buf, sizeof(time_buf), s.lap_time_ms);
    lv_label_set_text(s_lbl_atual_val, time_buf);

    /* Overlay de foco (layouts 1/2) - so atualiza quando visivel */
    if (s_race_layout == 1) {
        if (s.live_delta_valid) {
            lv_label_set_text_fmt(s_focus_big, "%+.2f", (double)(s.live_delta_ms / 1000.0f));
            lv_obj_set_style_text_color(s_focus_big,
                s.live_delta_ms <= 0 ? COLOR_GREEN : COLOR_RED, 0);
        } else {
            lv_label_set_text(s_focus_big, "--.--");
            lv_obj_set_style_text_color(s_focus_big, COLOR_MUTED, 0);
        }
        lv_label_set_text_fmt(s_focus_sub, "ATUAL %s    VOLTA %lu",
                              time_buf, (unsigned long)s.lap_number);
    } else if (s_race_layout == 2) {
        lv_label_set_text_fmt(s_focus_big, "%d", (int)(s.speed_kmh + 0.5f));
        lv_obj_set_style_text_color(s_focus_big, COLOR_TEXT, 0);
        if (s.live_delta_valid) {
            lv_label_set_text_fmt(s_focus_sub, "km/h    DELTA %+.2f",
                                  (double)(s.live_delta_ms / 1000.0f));
            lv_obj_set_style_text_color(s_focus_sub,
                s.live_delta_ms <= 0 ? COLOR_GREEN : COLOR_RED, 0);
        } else {
            lv_label_set_text_fmt(s_focus_sub, "km/h    ATUAL %s", time_buf);
            lv_obj_set_style_text_color(s_focus_sub, COLOR_MUTED, 0);
        }
    }

    if (s.best_lap_ms > 0) {
        format_lap_time(time_buf, sizeof(time_buf), s.best_lap_ms);
        lv_label_set_text(s_lbl_best_val, time_buf);
    } else {
        lv_label_set_text(s_lbl_best_val, "--.---");
    }

    /* DELTA ao vivo (tipo carro de corrida) - atualiza a cada amostra de
     * GPS comparando a distancia ja percorrida na volta atual contra o
     * tempo que a melhor volta levou pra chegar nessa mesma distancia.
     * Continua mostrando o numero mesmo depois de fechar a volta (nao
     * mais um valor fixo "congelado" ate a proxima volta - live_delta_ms
     * ja reflete o inicio da volta nova a partir da primeira amostra). */
    bool has_live = s.live_delta_valid;
    if (has_live) {
        lv_label_set_text_fmt(s_lbl_delta_val, "%+.2f", (double)(s.live_delta_ms / 1000.0f));
        lv_obj_set_style_text_color(s_lbl_delta_val,
            (s.live_delta_ms <= 0) ? COLOR_GREEN : COLOR_RED, 0);
    } else {
        lv_label_set_text(s_lbl_delta_val, "--.--");
        lv_obj_set_style_text_color(s_lbl_delta_val, COLOR_MUTED, 0);
    }

    /* Tempo previsto da volta atual = best + delta ao vivo. So faz sentido
     * com uma volta de referencia (best) e delta valido. Verde se no rumo
     * de bater o best (delta <= 0), vermelho se perdendo. */
    if (has_live && s.best_lap_ms > 0) {
        int32_t pred = (int32_t)s.best_lap_ms + s.live_delta_ms;
        if (pred < 0) pred = 0;
        char pbuf[16];
        format_lap_time(pbuf, sizeof(pbuf), (uint32_t)pred);
        lv_label_set_text_fmt(s_lbl_predicted, "PREV %s", pbuf);
        lv_obj_set_style_text_color(s_lbl_predicted,
            (s.live_delta_ms <= 0) ? COLOR_GREEN : COLOR_RED, 0);
    } else {
        lv_label_set_text(s_lbl_predicted, "PREV --.---");
        lv_obj_set_style_text_color(s_lbl_predicted, COLOR_MUTED, 0);
    }

    int lit = 0;
    if (has_live) {
        float scale_ms = (float)settings_get_led_scale_ms();
        lit = (int)((fabsf((float)s.live_delta_ms) / scale_ms) * DELTA_LED_PER_SIDE);
        if (lit > DELTA_LED_PER_SIDE) lit = DELTA_LED_PER_SIDE;
    }
    bool gaining = has_live && s.live_delta_ms < 0;
    bool losing  = has_live && s.live_delta_ms > 0;
    for (int i = 0; i < DELTA_LED_PER_SIDE; i++) {
        lv_obj_set_style_bg_color(s_delta_segs_right[i],
            (gaining && i < lit) ? COLOR_GREEN : COLOR_GREEN_DIM, 0);
        lv_obj_set_style_bg_color(s_delta_segs_left[i],
            (losing && i < lit) ? COLOR_RED : COLOR_RED_DIM, 0);
    }

    /* Barra inferior: velocidade maxima da sessao (os splits de setor
     * agora vivem na celula SETORES; previsto e ideal tem blocos proprios). */
    lv_label_set_text_fmt(s_lbl_velmax_strip, "MAX %d", (int)(s_max_speed_kmh + 0.5f));

    /* Volta ideal - 0 = ainda sem volta "limpa" completa nessa sessao
     * (ver update_best_segments() em gps.c). */
    if (s.ideal_lap_ms > 0) {
        char ideal_buf[16];
        format_lap_time(ideal_buf, sizeof(ideal_buf), s.ideal_lap_ms);
        lv_label_set_text_fmt(s_lbl_ideal_lap, "IDEAL: %s", ideal_buf);
    } else {
        lv_label_set_text(s_lbl_ideal_lap, "IDEAL: --");
    }

    /* Indicador de GPS no topo (dot + label), 3 estados:
     *  - fix valido        -> VERDE estatico
     *  - 1+ sats, sem fix   -> AMARELO estatico (ja ve satelite, buscando fix)
     *  - 0 sats             -> VERMELHO piscando (nada ainda)
     * So fica verde/"FIX" com fix_valid de verdade (status 'A' do RMC) -
     * satelites visiveis (GGA) nao bastam, o modulo pode ver satelite sem
     * ainda ter fix. O piscar usa o proprio tick de 100ms deste timer. */
    static uint8_t s_gps_blink = 0;
    s_gps_blink++;
    lv_color_t gps_color;
    bool gps_blink_off = false;
    if (s.fix_valid) {
        gps_color = COLOR_GREEN;
        lv_label_set_text_fmt(s_gps_label, "GPS FIX: %u", (unsigned)s.satellites);
    } else if (s.satellites >= 1) {
        gps_color = COLOR_GOLD;
        lv_label_set_text_fmt(s_gps_label, "GPS %u", (unsigned)s.satellites);
    } else {
        gps_color = COLOR_RED;
        /* fase "apagada" a cada GPS_DOT_BLINK_TICKS ticks -> pisca ~1.6Hz */
        gps_blink_off = ((s_gps_blink / GPS_DOT_BLINK_TICKS) % 2) != 0;
        lv_label_set_text(s_gps_label, "GPS 0");
    }
    lv_color_t gps_shown = gps_blink_off ? COLOR_RED_DIM : gps_color;
    lv_obj_set_style_text_color(s_gps_label, gps_shown, 0);
    lv_obj_set_style_bg_color(s_gps_dot, gps_shown, 0);

    if (++s_slow_tick >= 20) {
        s_slow_tick = 0;

        switch (gps_get_link_status()) {
        case GPS_LINK_ERROR:
            lv_label_set_text(s_gps_link_label, "SEM COMUNICACAO - cheque modulo/fiacao");
            lv_obj_set_style_text_color(s_gps_link_label, COLOR_RED, 0);
            break;
        case GPS_LINK_SEARCHING:
            lv_label_set_text_fmt(s_gps_link_label, "buscando sinal (%u sats)", (unsigned)s.satellites);
            lv_obj_set_style_text_color(s_gps_link_label, COLOR_GOLD, 0);
            break;
        case GPS_LINK_FIXED:
            lv_label_set_text_fmt(s_gps_link_label, "OK - fix (%u sats)", (unsigned)s.satellites);
            lv_obj_set_style_text_color(s_gps_link_label, COLOR_GREEN, 0);
            break;
        }

        /* Card de RF (SISTEMA) - "sabe" (almanaque) vs "ouve" (SNR real).
         * Verde quando esta ouvindo o suficiente pra fix solido. */
        gps_rf_diag_t rf;
        gps_get_rf_diag(&rf);
        uint8_t total_trk = rf.tracked[0] + rf.tracked[1] + rf.tracked[2] + rf.tracked[3];
        lv_label_set_text_fmt(s_gps_rf_label,
            "sabe %u/%u/%u/%u\nouve %u/%u/%u/%u  snr %u",
            rf.in_view[0], rf.in_view[1], rf.in_view[2], rf.in_view[3],
            rf.tracked[0], rf.tracked[1], rf.tracked[2], rf.tracked[3],
            rf.best_snr);
        lv_obj_set_style_text_color(s_gps_rf_label,
            (total_trk >= 6 && rf.best_snr >= 35) ? COLOR_GREEN :
            (total_trk >= 1) ? COLOR_GOLD : COLOR_RED, 0);

        /* Autoteste - cada item verde (ok) ou vermelho (falha) via recolor */
        bool hc_sd  = sd_logger_is_mounted();
        bool hc_gps = (gps_get_link_status() != GPS_LINK_ERROR);
        bool hc_ble = ble_telemetry_host_synced();
        lv_label_set_text_fmt(s_health_label,
            "#%s SD# #%s GPS# #%s BLE/C6#",
            hc_sd  ? "3ee07a" : "ff5a5a",
            hc_gps ? "3ee07a" : "ff5a5a",
            hc_ble ? "3ee07a" : "ff5a5a");

        uint64_t total = 0, free_b = 0;
        if (sd_get_card_info(&total, &free_b)) {
            uint64_t used = total - free_b;
            lv_label_set_text_fmt(s_sd_usage_label, "SD %.1fGB / %.1fGB",
                                   used / (1024.0 * 1024 * 1024),
                                   total / (1024.0 * 1024 * 1024));
            int pct = (total > 0) ? (int)((used * 100) / total) : 0;
            lv_bar_set_value(s_sd_usage_bar, pct, LV_ANIM_OFF);
        } else {
            lv_label_set_text(s_sd_usage_label, "SD indisponivel");
        }

        /* Bateria - icone (nivel) + %. Cor por faixa: verde >50, dourado
         * 20-50, vermelho <20. Se a heuristica achar que esta carregando,
         * troca o glifo pelo raio (LV_SYMBOL_CHARGE). -1 = ADC indisponivel. */
        {
            int bp = battery_get_percent();
            if (bp < 0) {
                lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
                lv_label_set_text(s_batt_lbl, "--%");
                lv_obj_set_style_text_color(s_batt_icon, COLOR_MUTED, 0);
                lv_obj_set_style_text_color(s_batt_lbl, COLOR_MUTED, 0);
            } else {
                bool chg = battery_is_charging();
                const char *sym = chg ? LV_SYMBOL_CHARGE
                    : bp >= 80 ? LV_SYMBOL_BATTERY_FULL
                    : bp >= 55 ? LV_SYMBOL_BATTERY_3
                    : bp >= 30 ? LV_SYMBOL_BATTERY_2
                    : bp >= 10 ? LV_SYMBOL_BATTERY_1
                    :            LV_SYMBOL_BATTERY_EMPTY;
                lv_color_t bc = chg ? COLOR_GREEN
                    : bp > 50 ? COLOR_GREEN
                    : bp >= 20 ? COLOR_GOLD
                    :            COLOR_RED;
                lv_label_set_text(s_batt_icon, sym);
                lv_label_set_text_fmt(s_batt_lbl, "%d%%", bp);
                lv_obj_set_style_text_color(s_batt_icon, bc, 0);
                lv_obj_set_style_text_color(s_batt_lbl, bc, 0);
            }
        }

        /* Icone BLE no topo, 3 estados (pedido do usuario):
         *  - radio DESLIGADO na CONFIG      -> oculto (mesma linguagem do
         *    WIFI/USB: nao aparece quando inativo);
         *  - ligado, sem aparelho conectado -> cinza estatico;
         *  - conectado a um aparelho        -> azul PISCANDO (anim de
         *    opacidade, reusa wifi_icon_blink_anim_cb).
         * O antigo codigo deixava o icone sempre "aceso" (MUTED2/BLUE fixo),
         * dando a impressao de BLE ativo mesmo desligado. */
        static bool s_ble_icon_blinking = false;
        if (!ble_telemetry_is_enabled()) {
            lv_obj_add_flag(s_ble_icon, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_ble_status_label, "desligado");
            lv_obj_set_style_text_color(s_ble_status_label, COLOR_MUTED, 0);
        } else {
            lv_obj_clear_flag(s_ble_icon, LV_OBJ_FLAG_HIDDEN);
            bool connected = ble_telemetry_is_connected();
            if (connected) {
                lv_obj_set_style_text_color(s_ble_icon, COLOR_BLUE, 0);
                if (!s_ble_icon_blinking) {
                    s_ble_icon_blinking = true;
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, s_ble_icon);
                    lv_anim_set_exec_cb(&a, wifi_icon_blink_anim_cb);
                    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
                    lv_anim_set_time(&a, 600);
                    lv_anim_set_playback_time(&a, 600);
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_start(&a);
                }
            } else {
                if (s_ble_icon_blinking) {
                    s_ble_icon_blinking = false;
                    lv_anim_delete(s_ble_icon, wifi_icon_blink_anim_cb);
                    lv_obj_set_style_text_opa(s_ble_icon, LV_OPA_COVER, 0);
                }
                lv_obj_set_style_text_color(s_ble_icon, COLOR_MUTED, 0);
            }
            lv_label_set_text(s_ble_status_label, connected ? "conectado" : "inativo");
            lv_obj_set_style_text_color(s_ble_status_label, connected ? COLOR_GREEN : COLOR_MUTED, 0);
        }

        /* Icone WIFI - so aparece com o export sob demanda realmente no
         * ar (AP proprio ligado OU cliente ja conectado); some sozinho
         * quando desliga (manual ou timeout de ociosidade). Pisca
         * enquanto visivel (ver wifi_icon_blink_anim_cb acima). */
        static bool s_wifi_icon_blinking = false;
        if (wifi_export_is_active()) {
            lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
            if (!s_wifi_icon_blinking) {
                s_wifi_icon_blinking = true;
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_wifi_icon);
                lv_anim_set_exec_cb(&a, wifi_icon_blink_anim_cb);
                lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
                lv_anim_set_time(&a, 600);
                lv_anim_set_playback_time(&a, 600);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_start(&a);
            }
        } else {
            lv_obj_add_flag(s_wifi_icon, LV_OBJ_FLAG_HIDDEN);
            if (s_wifi_icon_blinking) {
                s_wifi_icon_blinking = false;
                lv_anim_delete(s_wifi_icon, wifi_icon_blink_anim_cb);
                lv_obj_set_style_text_opa(s_wifi_icon, LV_OPA_COVER, 0); /* reset pra proxima vez que aparecer */
            }
        }
    }
}

/* ----------------------------------------------------------------------
 * Flash visual numa volta fechada - overlay independente do refresh,
 * nao disputa com o timer de 100ms.
 * ---------------------------------------------------------------------- */
static void flash_anim_exec_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* NOTA sobre os lvgl_port_lock() espalhados nas funcoes publicas daqui
 * pra baixo: TODA funcao ui_* chamada pelo dispatcher de eventos do
 * main.c (task "main") precisa do lock - taskLVGL roda lv_timer_handler
 * continuamente (refresh de 100ms + animacoes) e mexer na arvore de
 * objetos de outra task sem lock corrompe estado interno da LVGL.
 * Sintoma real visto em campo (2x): task watchdog com taskLVGL presa em
 * loop infinito dentro de lv_text_get_size/lv_inv_area. O mutex do
 * esp_lvgl_port e' RECURSIVO, entao chamar essas funcoes de dentro da
 * propria taskLVGL (callbacks de botao) ou dentro de um lock externo
 * (boot em main.c) continua seguro. */
void ui_flash_lap_complete(uint32_t lap_number, uint32_t lap_time_ms, int32_t delta_ms, bool is_new_best)
{
    (void)lap_number; (void)lap_time_ms;

    if (!lvgl_port_lock(0)) return;
    lv_color_t flash_color = is_new_best ? COLOR_GOLD : (delta_ms <= 0 ? COLOR_GREEN : COLOR_RED);
    lv_obj_set_style_bg_color(s_flash_overlay, flash_color, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash_overlay);
    lv_anim_set_values(&a, 90, 0);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, flash_anim_exec_cb);
    lv_anim_start(&a);

    /* Banner "NOVO RECORDE" so aparece quando bate o best - fade de
     * opaco a transparente ao longo de ~1.4s (mais lento que o flash pra
     * dar tempo de ler). Reusa wifi_icon_blink_anim_cb, que mexe no
     * text_opa do alvo. */
    if (is_new_best) {
        lv_anim_t r;
        lv_anim_init(&r);
        lv_anim_set_var(&r, s_lbl_record);
        lv_anim_set_values(&r, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&r, 1400);
        lv_anim_set_exec_cb(&r, wifi_icon_blink_anim_cb);
        lv_anim_start(&r);
    }
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Toast - popup de feedback de acao
 * ---------------------------------------------------------------------- */
static void toast_dismiss_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(s_toast_timer);
    s_toast_timer = NULL;
}

static void mode_popup_dismiss_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s_mode_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(s_mode_popup_timer);
    s_mode_popup_timer = NULL;
}

void ui_show_toast(const char *msg, uint32_t duration_ms)
{
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_toast_label, msg);
    lv_obj_clear_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) {
        /* ja ha um toast visivel - reinicia o timer */
        lv_timer_reset(s_toast_timer);
        lv_timer_set_period(s_toast_timer, duration_ms);
    } else {
        s_toast_timer = lv_timer_create(toast_dismiss_cb, duration_ms, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
    lvgl_port_unlock();
}

void ui_show_session_stats(const gps_session_stats_t *stats)
{
    if (!stats || stats->lap_count == 0) return;

    char best_buf[16], avg_buf[16];
    format_lap_time(best_buf, sizeof(best_buf), stats->best_ms);
    format_lap_time(avg_buf,  sizeof(avg_buf),  stats->avg_ms);

    if (!lvgl_port_lock(0)) return;
    lv_label_set_text_fmt(s_stats_body_label,
        "VOLTAS        %u\n"
        "BEST          %s\n"
        "MEDIA         %s\n"
        "CONSISTENCIA  +/- %.3fs\n"
        "VEL MAX       %d km/h\n"
        "\n"
        "(toque para fechar)",
        (unsigned)stats->lap_count, best_buf, avg_buf,
        (double)(stats->stddev_ms / 1000.0),
        (int)(stats->max_speed_kmh + 0.5f));
    lv_obj_clear_flag(s_stats_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_stats_overlay);
    if (s_stats_timer) {
        lv_timer_reset(s_stats_timer);
    } else {
        /* 12s e some sozinho - piloto pode estar com luva, nao obriga toque */
        s_stats_timer = lv_timer_create(stats_overlay_timer_cb, 12000, NULL);
        lv_timer_set_repeat_count(s_stats_timer, 1);
    }
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Countdown pos-carregar-pista - reusa o toast, mas com dismiss proprio
 * (contagem 3..2..1, nao tempo fixo) que termina indo pra aba CORRIDA
 * sozinho. Pedido do usuario: depois de carregar uma pista na aba PISTA,
 * deixar obvio que carregou E levar direto pro lugar onde se usa (aba
 * CORRIDA), sem precisar trocar de aba na mao.
 * ---------------------------------------------------------------------- */
static lv_timer_t *s_track_loaded_timer = NULL;
static int          s_track_loaded_countdown;
static char          s_track_loaded_name[TRACK_NAME_MAX];

static void track_loaded_countdown_cb(lv_timer_t *t)
{
    (void)t;
    s_track_loaded_countdown--;
    if (s_track_loaded_countdown <= 0) {
        lv_timer_delete(s_track_loaded_timer);
        s_track_loaded_timer = NULL;
        lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
        show_tab(1); /* CORRIDA */
        return;
    }
    char buf[TRACK_NAME_MAX + 64];
    snprintf(buf, sizeof(buf), "Pista \"%s\" carregada - indo pra CORRIDA em %d...",
             s_track_loaded_name, s_track_loaded_countdown);
    lv_label_set_text(s_toast_label, buf);
}

void ui_show_track_loaded_countdown(const char *track_name)
{
    if (!lvgl_port_lock(0)) return;

    strncpy(s_track_loaded_name, track_name, sizeof(s_track_loaded_name) - 1);
    s_track_loaded_name[sizeof(s_track_loaded_name) - 1] = '\0';
    s_track_loaded_countdown = 3;

    /* Cancela o timer normal de auto-dismiss do toast (esse aqui fecha
     * sozinho quando o countdown zera, nao num tempo fixo) e qualquer
     * countdown anterior ainda rodando (ex: carregar 2 pistas em
     * sequencia rapido). */
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_track_loaded_timer) { lv_timer_delete(s_track_loaded_timer); s_track_loaded_timer = NULL; }

    char buf[TRACK_NAME_MAX + 48];
    snprintf(buf, sizeof(buf), "Pista \"%s\" carregada - indo pra CORRIDA em %d...",
             track_name, s_track_loaded_countdown);
    lv_label_set_text(s_toast_label, buf);
    lv_obj_clear_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);

    s_track_loaded_timer = lv_timer_create(track_loaded_countdown_cb, 1000, NULL);

    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Hold-progress - barra de "segure pra encerrar sessao"
 * ---------------------------------------------------------------------- */
static void hold_bar_anim_exec_cb(void *var, int32_t v)
{
    lv_bar_set_value((lv_obj_t *)var, (int32_t)v, LV_ANIM_OFF);
}

void ui_show_hold_progress(bool start)
{
    if (!lvgl_port_lock(0)) return;
    if (start) {
        lv_label_set_text(s_hold_label, "Segure para encerrar sessao"); /* overlay e' compartilhado com o hold do MODE */
        lv_bar_set_value(s_hold_bar, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_hold_bar);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, BTN_RESET_HOLD_MS);
        lv_anim_set_exec_cb(&a, hold_bar_anim_exec_cb);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(s_hold_bar, NULL);
        lv_bar_set_value(s_hold_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Barra de hold do MODE - mesmo overlay do RESET, com uma CARENCIA de
 * 250ms antes de aparecer: todo tap do MODE (ciclar layout) passa por um
 * aperto rapido, e sem a carencia a barra piscaria na tela a cada tap.
 * Se o dedo soltar dentro da carencia, o timer e' cancelado e nada
 * aparece; passou dela, a barra surge ja em ~25% e anima o restante ate
 * o BTN_MODE_HOLD_MS.
 * ---------------------------------------------------------------------- */
#define MODE_HOLD_GRACE_MS (250)

static void mode_hold_grace_cb(lv_timer_t *t)
{
    (void)t;
    s_mode_hold_timer = NULL; /* repeat_count=1 - LVGL apaga o timer sozinho */
    lv_label_set_text(s_hold_label, "Segure para trocar QUALY / RACE");
    int start_pct = (100 * MODE_HOLD_GRACE_MS) / BTN_MODE_HOLD_MS;
    lv_bar_set_value(s_hold_bar, start_pct, LV_ANIM_OFF);
    lv_obj_clear_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_hold_bar);
    lv_anim_set_values(&a, start_pct, 100);
    lv_anim_set_time(&a, BTN_MODE_HOLD_MS - MODE_HOLD_GRACE_MS);
    lv_anim_set_exec_cb(&a, hold_bar_anim_exec_cb);
    lv_anim_start(&a);
}

void ui_show_mode_hold_progress(bool start)
{
    if (!lvgl_port_lock(0)) return;
    if (start) {
        if (!s_mode_hold_timer) {
            s_mode_hold_timer = lv_timer_create(mode_hold_grace_cb, MODE_HOLD_GRACE_MS, NULL);
            lv_timer_set_repeat_count(s_mode_hold_timer, 1);
        }
    } else {
        if (s_mode_hold_timer) {
            lv_timer_delete(s_mode_hold_timer); /* soltou na carencia - barra nem chegou a aparecer */
            s_mode_hold_timer = NULL;
        }
        lv_anim_delete(s_hold_bar, NULL);
        lv_bar_set_value(s_hold_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * API publica
 * ---------------------------------------------------------------------- */
/* ----------------------------------------------------------------------
 * Overlays centrais (estatisticas de sessao / comparacao de voltas) -
 * scrim escuro + caixa central com titulo e corpo. Fecham no toque em
 * qualquer lugar (scrim e caixa tem o mesmo callback de fechar).
 * ---------------------------------------------------------------------- */
static void stats_overlay_click_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_stats_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_stats_timer) {
        lv_timer_delete(s_stats_timer);
        s_stats_timer = NULL;
    }
}

static void stats_overlay_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s_stats_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(s_stats_timer);
    s_stats_timer = NULL;
}

static void cmp_overlay_click_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_cmp_overlay, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_center_overlay(lv_obj_t *layer, const char *title,
                                     lv_obj_t **out_body, lv_event_cb_t close_cb)
{
    lv_obj_t *scrim = bare(layer);
    lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(scrim, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_add_event_cb(scrim, close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = bare(scrim);
    lv_obj_set_size(box, 460, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_make(0x0A, 0x0A, 0x0A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(box, 22, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_add_event_cb(box, close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title_lbl = lv_label_create(box);
    lv_obj_set_style_text_font(title_lbl, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(title_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(title_lbl, title);

    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_style_text_font(body, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(body, COLOR_TEXT, 0);
    lv_obj_set_style_text_line_space(body, 8, 0);
    lv_label_set_text(body, "");
    *out_body = body;

    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    return scrim;
}

void ui_init(lv_display_t *disp)
{
    /* Antes de qualquer widget: carrega a paleta do tema salvo (NVS) nas
     * variaveis COLOR_PRIMARY* - todos os builds abaixo ja usam a cor
     * certa. settings_init() ja rodou no app_main() nesse ponto. */
    apply_theme_palette();

    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    /* Faixa de acento no topo - reforca o modo atual (QUALY/CORRIDA) num
     * relance, mesmo de rabo de olho, sem depender so do pill pequeno da
     * status bar. Cor real e' setada em ui_set_mode_label(). */
    s_mode_accent_bar = bare(screen);
    lv_obj_set_size(s_mode_accent_bar, lv_pct(100), 3);
    lv_obj_set_style_bg_color(s_mode_accent_bar, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_mode_accent_bar, LV_OPA_COVER, 0);

    build_status_bar(screen);
    build_delta_led_bar(screen);

    lv_obj_t *content_area = bare(screen);
    lv_obj_set_width(content_area, lv_pct(100));
    lv_obj_set_flex_grow(content_area, 1);

    /* Tab 0: PISTA */
    s_tab_content_pista = bare(content_area);
    lv_obj_set_size(s_tab_content_pista, lv_pct(100), lv_pct(100));
    build_pista_tab(s_tab_content_pista);

    /* Tab 1: CORRIDA */
    s_tab_content_race = bare(content_area);
    lv_obj_set_size(s_tab_content_race, lv_pct(100), lv_pct(100));
    build_race_tab(s_tab_content_race);
    lv_obj_add_flag(s_tab_content_race, LV_OBJ_FLAG_HIDDEN);

    /* Tab 2: VOLTAS */
    s_tab_content_laps = bare(content_area);
    lv_obj_set_size(s_tab_content_laps, lv_pct(100), lv_pct(100));
    build_laps_tab(s_tab_content_laps);
    lv_obj_add_flag(s_tab_content_laps, LV_OBJ_FLAG_HIDDEN);

    /* Tab 3: CONFIG */
    s_tab_content_cfg = bare(content_area);
    lv_obj_set_size(s_tab_content_cfg, lv_pct(100), lv_pct(100));
    build_config_tab(s_tab_content_cfg);
    lv_obj_add_flag(s_tab_content_cfg, LV_OBJ_FLAG_HIDDEN);

    build_tab_bar(screen);
    show_tab(1);  /* começa na aba CORRIDA - e o painel de uso durante a pilotagem */

    /* Teclado flutuante - vive no layer_top pra aparecer acima de tudo.
     * Compartilhado pelos campos de texto da aba PISTA e da aba Config.
     * Criado aqui onde temos acesso a 'disp'.
     *
     * BUG CORRIGIDO: LV_SIZE_CONTENT na altura nao funciona direito pro
     * lv_keyboard - o widget e' um lv_buttonmatrix por baixo, e as linhas
     * do grid interno sao fracoes (1fr) da altura do PAI, nao do conteudo.
     * Com altura "de conteudo" nao ha altura de pai pra dividir, entao o
     * LVGL colapsa a maioria das linhas e so a ultima fileira de teclas
     * (embaixo) fica com tamanho utilizavel - exatamente o sintoma
     * reportado ("so aparecem algumas teclas na parte de baixo"). Fix:
     * altura fixa explicita, grande o suficiente pras 5 fileiras do mapa
     * padrao (numeros + 3 de letras + barra de espaco) com a fonte maior. */
    s_kb_font = font_kartbox_lg;
    s_kb_font.fallback = LV_FONT_DEFAULT;

    /* Scrim atras do teclado - cobre a tela inteira, clicavel, so pra
     * fechar o teclado ao tocar fora dele (ver comentario em
     * keyboard_scrim_click_cb). Criado ANTES do teclado de proposito -
     * mesmo parent (layer_top), ordem de criacao define z-order, entao
     * o teclado fica por cima do scrim. */
    s_keyboard_scrim = bare(lv_display_get_layer_top(disp));
    lv_obj_set_size(s_keyboard_scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_keyboard_scrim, 0, 0);
    lv_obj_set_style_bg_opa(s_keyboard_scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_keyboard_scrim, 0, 0);
    lv_obj_add_flag(s_keyboard_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_keyboard_scrim, keyboard_scrim_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_keyboard_scrim, LV_OBJ_FLAG_HIDDEN);

    s_keyboard = lv_keyboard_create(lv_display_get_layer_top(disp));
    lv_obj_set_size(s_keyboard, lv_pct(100), 300);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_keyboard, &s_kb_font, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Toast overlay - feedback visual de acoes (linha marcada, etc.)
     * Vive em layer_top para flutuar acima de tudo, inclusive do teclado.
     * ------------------------------------------------------------------ */
    lv_obj_t *layer = lv_display_get_layer_top(disp);

    s_toast_box = bare(layer);
    lv_obj_set_size(s_toast_box, 420, LV_SIZE_CONTENT);
    lv_obj_align(s_toast_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_toast_box, lv_color_make(0x0A, 0x0A, 0x0A), 0);
    lv_obj_set_style_bg_opa(s_toast_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast_box, COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_toast_box, 1, 0);
    lv_obj_set_style_pad_ver(s_toast_box, 18, 0);
    lv_obj_set_style_pad_hor(s_toast_box, 24, 0);
    lv_obj_set_flex_flow(s_toast_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_toast_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_toast_box, 8, 0);
    /* Faixa verde no topo - identidade visual de "confirmado" */
    lv_obj_t *toast_accent = bare(s_toast_box);
    lv_obj_set_size(toast_accent, lv_pct(100), 2);
    lv_obj_set_style_bg_color(toast_accent, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(toast_accent, LV_OPA_COVER, 0);
    s_toast_label = lv_label_create(s_toast_box);
    /* toast_box tem largura FIXA (420) - sem largura+wrap explicitos no
     * label, mensagens mais longas (ex: "Marque a linha de chegada antes
     * de iniciar") tentam desenhar numa linha so, ficam mais largas que
     * a caixa e saem cortadas nas bordas em vez de quebrar linha. */
    lv_obj_set_width(s_toast_label, lv_pct(100));
    lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_toast_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_toast_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_toast_label, "");
    lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;

    /* ------------------------------------------------------------------
     * Popup de estatisticas da sessao + overlay de comparacao de voltas -
     * ambos: scrim escuro clicavel (fecha no toque em qualquer lugar) +
     * caixa central. Construidos por make_center_overlay() pra nao
     * duplicar; preenchidos em ui_show_session_stats() /
     * show_lap_compare().
     * ------------------------------------------------------------------ */
    s_stats_overlay = make_center_overlay(layer, "SESSAO ENCERRADA", &s_stats_body_label,
                                          stats_overlay_click_cb);
    s_stats_timer = NULL;
    s_cmp_overlay = make_center_overlay(layer, "COMPARAR VOLTAS", &s_cmp_body_label,
                                        cmp_overlay_click_cb);

    /* Overlay de OTA - tela inteira, OPACO (esconde tudo = LVGL para de
     * redesenhar a UI por baixo enquanto a flash grava). Sem botao de
     * fechar de proposito: quem controla e' o ota_post_handler. */
    s_ota_overlay = bare(layer);
    lv_obj_set_size(s_ota_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_ota_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_ota_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_ota_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ota_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ota_overlay, 16, 0);
    lv_obj_t *ota_title = lv_label_create(s_ota_overlay);
    lv_obj_set_style_text_font(ota_title, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(ota_title, COLOR_PRIMARY, 0);
    lv_label_set_text(ota_title, "ATUALIZANDO FIRMWARE");
    lv_obj_t *ota_sub = lv_label_create(s_ota_overlay);
    lv_obj_set_style_text_font(ota_sub, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(ota_sub, COLOR_MUTED, 0);
    lv_obj_set_style_text_align(ota_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ota_sub, "NAO DESLIGUE\nreinicia sozinho ao terminar\n(a tela pode piscar - e' normal)");
    lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Hold-progress overlay - cobre a tela inteira durante o hold do RESET
     * pra bloquear toques acidentais enquanto o piloto segura o botao.
     * ------------------------------------------------------------------ */
    s_hold_overlay = bare(layer);
    lv_obj_set_size(s_hold_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_hold_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_hold_overlay, LV_OPA_70, 0);
    lv_obj_set_flex_flow(s_hold_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_hold_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_hold_overlay, 12, 0);
    s_hold_label = lv_label_create(s_hold_overlay);
    lv_obj_set_style_text_font(s_hold_label, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_hold_label, COLOR_MUTED, 0);
    lv_label_set_text(s_hold_label, "Segure para encerrar sessao");
    s_hold_bar = lv_bar_create(s_hold_overlay);
    lv_obj_set_size(s_hold_bar, 300, 10);
    lv_obj_set_style_bg_color(s_hold_bar, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(s_hold_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hold_bar, COLOR_RED, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_hold_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_hold_bar, 0, 0);
    lv_obj_set_style_radius(s_hold_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_hold_bar, 0, 100);
    lv_bar_set_value(s_hold_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Troca de modo QUALY/RACE - flash de tela inteira + popup grande.
     * ------------------------------------------------------------------ */
    s_mode_flash_overlay = bare(layer);
    lv_obj_add_flag(s_mode_flash_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    /* CLICKABLE por padrao em todo lv_obj_create - como esse aqui cobre a
     * tela inteira e fica em layer_top (por cima de TUDO, inclusive a
     * barra de abas), sem tirar essa flag ele engolia todo toque pra
     * sempre (mesmo transparente/invisivel na maior parte do tempo) -
     * era isso que travava PISTA/CORRIDA/VOLTAS/CONFIG depois que esse
     * overlay foi criado. */
    lv_obj_clear_flag(s_mode_flash_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_mode_flash_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_mode_flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_mode_flash_overlay, COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_mode_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_mode_flash_overlay, 0, 0);

    s_mode_popup = bare(layer);
    lv_obj_set_size(s_mode_popup, 380, LV_SIZE_CONTENT);
    lv_obj_align(s_mode_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_mode_popup, lv_color_make(0x0A, 0x0A, 0x0A), 0);
    lv_obj_set_style_bg_opa(s_mode_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mode_popup, 3, 0);
    lv_obj_set_style_pad_ver(s_mode_popup, 24, 0);
    lv_obj_set_style_pad_hor(s_mode_popup, 28, 0);
    lv_obj_set_flex_flow(s_mode_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_mode_popup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_mode_popup, 8, 0);
    lv_obj_t *mode_popup_hint = lv_label_create(s_mode_popup);
    lv_obj_set_style_text_font(mode_popup_hint, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(mode_popup_hint, COLOR_MUTED, 0);
    lv_label_set_text(mode_popup_hint, "MODO ALTERADO");
    s_mode_popup_label = lv_label_create(s_mode_popup);
    lv_obj_set_style_text_font(s_mode_popup_label, &font_kartbox_xl, 0);
    lv_label_set_text(s_mode_popup_label, "QUALY");
    lv_obj_add_flag(s_mode_popup, LV_OBJ_FLAG_HIDDEN);
    s_mode_popup_timer = NULL;

    /* Passkey overlay — exibe codigo SMP de 6 digitos pra pareamento BLE */
    s_passkey_overlay = bare(layer);
    lv_obj_set_size(s_passkey_overlay, 360, LV_SIZE_CONTENT);
    lv_obj_align(s_passkey_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_passkey_overlay, lv_color_make(0x08, 0x0F, 0x18), 0);
    lv_obj_set_style_bg_opa(s_passkey_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_passkey_overlay, COLOR_BLUE, 0);
    lv_obj_set_style_border_width(s_passkey_overlay, 2, 0);
    lv_obj_set_style_pad_ver(s_passkey_overlay, 24, 0);
    lv_obj_set_style_pad_hor(s_passkey_overlay, 28, 0);
    lv_obj_set_flex_flow(s_passkey_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_passkey_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_passkey_overlay, 12, 0);
    lv_obj_t *pk_title = lv_label_create(s_passkey_overlay);
    lv_obj_set_style_text_font(pk_title, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(pk_title, COLOR_BLUE, 0);
    lv_label_set_text(pk_title, "BLUETOOTH PASSKEY");
    lv_obj_t *pk_hint = lv_label_create(s_passkey_overlay);
    lv_obj_set_style_text_font(pk_hint, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(pk_hint, COLOR_MUTED, 0);
    lv_label_set_text(pk_hint, "Digite no celular:");
    s_passkey_label = lv_label_create(s_passkey_overlay);
    lv_obj_set_style_text_font(s_passkey_label, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(s_passkey_label, COLOR_TEXT, 0);
    lv_label_set_text(s_passkey_label, "------");
    lv_obj_add_flag(s_passkey_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Mapa da pista - overlay full-screen com o tracado de uma sessao ja
     * GRAVADA (historico, nao ao vivo), colorido por velocidade. Le
     * pontos (posicao local em metros + velocidade) de
     * sd_read_session_track() e desenha como segmentos de linha num
     * canvas LVGL. Aberto pelo botao "Mapa" na aba VOLTAS - ver
     * ui_show_session_map().
     * ------------------------------------------------------------------ */
    s_map_overlay = bare(layer);
    lv_obj_set_size(s_map_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_map_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_map_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_map_overlay, 10, 0);
    lv_obj_set_style_pad_row(s_map_overlay, 8, 0);
    lv_obj_set_flex_flow(s_map_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_map_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *map_header = bare(s_map_overlay);
    lv_obj_set_size(map_header, lv_pct(100), 40);
    lv_obj_set_flex_flow(map_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(map_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_map_title_lbl = lv_label_create(map_header);
    lv_obj_set_style_text_font(s_map_title_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_map_title_lbl, COLOR_TEXT, 0);
    lv_label_set_text(s_map_title_lbl, "Mapa da pista");

    /* Botao de alternar a visao do mapa - sessao inteira (media) vs so a
     * melhor volta. Substituiu o "ghost" cinza sob o heatmap (feedback do
     * usuario: a sobreposicao atrapalhava a leitura). */
    lv_obj_t *map_mode_btn = lv_button_create(map_header);
    lv_obj_set_size(map_mode_btn, 220, 36);
    style_action_button(map_mode_btn, COLOR_PRIMARY_DIM, COLOR_PRIMARY_ACCENT);
    lv_obj_add_event_cb(map_mode_btn, map_mode_btn_cb, LV_EVENT_CLICKED, NULL);
    s_map_mode_lbl = lv_label_create(map_mode_btn);
    lv_obj_center(s_map_mode_lbl);
    lv_obj_set_style_text_font(s_map_mode_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_map_mode_lbl, COLOR_PRIMARY, 0);
    lv_label_set_text(s_map_mode_lbl, "Ver: sessao");

    lv_obj_t *map_close_btn = lv_button_create(map_header);
    lv_obj_set_size(map_close_btn, 110, 36);
    style_action_button(map_close_btn, COLOR_RED_DIM, COLOR_RED);
    lv_obj_add_event_cb(map_close_btn, map_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *map_close_lbl = lv_label_create(map_close_btn);
    lv_obj_center(map_close_lbl);
    lv_obj_set_style_text_font(map_close_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(map_close_lbl, COLOR_RED, 0);
    lv_label_set_text(map_close_lbl, "Fechar");

    /* Buffer do canvas em PSRAM (32MB sobrando, mesma escolha de sempre
     * nesse projeto pra qualquer alocacao grande) - alocado 1x aqui,
     * reaproveitado toda vez que o mapa abre (nunca 2 mapas ao mesmo
     * tempo). RGB565 = 2 bytes/pixel. Se falhar (sem PSRAM por algum
     * motivo), s_map_canvas_buf fica NULL e ui_show_session_map() avisa
     * via toast em vez de crashar tentando desenhar num buffer nulo. */
    s_map_canvas = lv_canvas_create(s_map_overlay);
    lv_obj_set_size(s_map_canvas, MAP_CANVAS_W, MAP_CANVAS_H);
    s_map_canvas_buf = heap_caps_malloc((size_t)MAP_CANVAS_W * MAP_CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (s_map_canvas_buf) {
        lv_canvas_set_buffer(s_map_canvas, s_map_canvas_buf, MAP_CANVAS_W, MAP_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    } else {
        ESP_LOGE("ui", "Falha ao alocar buffer do mapa da pista (%d bytes)", MAP_CANVAS_W * MAP_CANVAS_H * 2);
    }

    lv_obj_add_flag(s_map_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Tela de loading no boot - criada por ULTIMO em layer_top de proposito
     * (ordem de criacao = z-order dentro do mesmo pai; fica por cima de
     * toast/hold/passkey/mapa e, claro, das abas). Comeca VISIVEL (sem
     * LV_OBJ_FLAG_HIDDEN) - as abas ja estao todas montadas nesse ponto
     * (ui_init esta quase terminando), entao sem esse overlay o usuario
     * veria a aba CORRIDA com "sem linha de chegada" piscando/mudando
     * enquanto GPS/SD/pistas/BLE/WiFi ainda estao subindo no app_main().
     * main.c chama ui_boot_set_status() entre cada etapa e ui_boot_finish()
     * no fim, escondendo esse overlay e revelando a tela real ja pronta. */
    s_boot_overlay = bare(layer);
    lv_obj_set_size(s_boot_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_boot_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_boot_overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_boot_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_boot_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_boot_overlay, 16, 0);

    lv_obj_t *boot_title = lv_label_create(s_boot_overlay);
    lv_obj_set_style_text_font(boot_title, &font_kartbox_2xl, 0);
    lv_obj_set_style_text_color(boot_title, COLOR_TEXT, 0);
    lv_label_set_text(boot_title, "KARTBOX");

    lv_obj_t *boot_spinner = lv_spinner_create(s_boot_overlay);
    lv_obj_set_size(boot_spinner, 64, 64);
    lv_obj_set_style_arc_color(boot_spinner, COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(boot_spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(boot_spinner, COLOR_MUTED2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(boot_spinner, 6, LV_PART_MAIN);

    s_boot_status_lbl = lv_label_create(s_boot_overlay);
    lv_obj_set_style_text_font(s_boot_status_lbl, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(s_boot_status_lbl, COLOR_MUTED, 0);
    lv_label_set_text(s_boot_status_lbl, "Iniciando...");

    /* 97ms e nao 100 DE PROPOSITO (anti-aliasing do cronometro): com o
     * periodo em exatos 100ms, o tempo exibido avanca em passos de
     * 100ms e o digito dos MILESIMOS nunca muda entre refreshes - parece
     * travado num numero fixo (reportado em campo: "12.345 -> 12.445 ->
     * 12.545"). 97 nao divide nenhuma casa decimal, entao todos os
     * digitos variam a cada tick. O s_slow_tick (>= 20) vira ~1.9s em
     * vez de 2s - irrelevante pro que ele atualiza. */
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 97, NULL);

    ui_refresh_session_list();
}

void ui_set_mode_label(gps_race_mode_t mode)
{
    if (!lvgl_port_lock(0)) return;
    bool race = (mode == GPS_MODE_CORRIDA);
    lv_label_set_text(s_mode_label, race ? "RACE" : "QUALY");
    /* QUALY usa a cor do tema; RACE continua SEMPRE vermelho fixo -
     * vermelho aqui e' sinal de "modo corrida pra valer", nao pode
     * depender do tema (e vermelho nem e' opcao de tema por isso). */
    lv_obj_set_style_bg_color(s_mode_pill, race ? COLOR_RED_DIM : COLOR_PRIMARY_DIM, 0);
    lv_obj_set_style_text_color(s_mode_label, race ? COLOR_RED : COLOR_PRIMARY, 0);

    /* cada modo lembra SEU layout da tela CORRIDA (qualy pode rodar com
     * a tela completa e corrida com o delta gigante, por exemplo) */
    s_ui_race_mode = race;
    apply_race_layout(settings_get_mode_layout(race ? 1 : 0));
    /* Numero de velocidade fica sempre branco - usuario preferiu assim,
     * o flash + popup abaixo ja avisam da troca sem precisar mexer nele. */

    lv_color_t mode_color = race ? COLOR_RED : COLOR_PRIMARY;

    lv_obj_set_style_bg_color(s_mode_accent_bar, mode_color, 0);

    /* Flash de tela inteira (layer_top - aparece em qualquer aba, nao so
     * na CORRIDA) + popup grande central com o nome do novo modo. */
    lv_obj_set_style_bg_color(s_mode_flash_overlay, mode_color, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mode_flash_overlay);
    lv_anim_set_values(&a, 100, 0);
    lv_anim_set_time(&a, 450);
    lv_anim_set_exec_cb(&a, flash_anim_exec_cb);
    lv_anim_start(&a);

    lv_label_set_text(s_mode_popup_label, race ? "RACE" : "QUALY");
    lv_obj_set_style_text_color(s_mode_popup_label, mode_color, 0);
    lv_obj_set_style_border_color(s_mode_popup, mode_color, 0);
    lv_obj_clear_flag(s_mode_popup, LV_OBJ_FLAG_HIDDEN);
    if (s_mode_popup_timer) {
        lv_timer_reset(s_mode_popup_timer);
    } else {
        s_mode_popup_timer = lv_timer_create(mode_popup_dismiss_cb, 1200, NULL);
        lv_timer_set_repeat_count(s_mode_popup_timer, 1);
    }
    lvgl_port_unlock();
}

void ui_set_recording_state(bool recording)
{
    if (recording) {
        s_max_speed_kmh = 0.0f; /* sessao nova, zera o pico de velocidade */
    }
    /* gps_get_finish_line ANTES do lock - nao precisa dele e segura o
     * mutex do estado GPS; melhor nao empilhar os dois locks a toa. */
    double lat, lon; float hdg;
    bool finish_set = gps_get_finish_line(&lat, &lon, &hdg);

    if (!lvgl_port_lock(0)) return;
    lv_obj_set_style_bg_color(s_rec_dot, recording ? COLOR_RED : COLOR_MUTED, 0);
    s_session_recording_active = recording;
    race_banner_refresh(finish_set);
    lvgl_port_unlock();
}

void ui_update_sector_status(void)
{
    /* Delega para ui_update_pista_status que cobre finish + setores + strip */
    ui_update_pista_status();
}

void ui_set_wifi_export_state(bool active, const char *ssid, const char *password)
{
    if (!lvgl_port_lock(0)) return;
    /* os DOIS botoes dinamicos refletem o estado, cada um no seu bloco
     * (so um bloco esta visivel por vez, conforme o modo) */
    lv_label_set_text(s_wifi_btn_label, active ? "Desligar WiFi" : "Ligar WiFi (AP proprio)");
    lv_label_set_text(s_sta_connect_lbl, active ? "Desconectar" : "Conectar");
    if (active && ssid && password) {
        lv_label_set_text_fmt(s_wifi_info_label, "%s  /  %s  /  192.168.4.1", ssid, password);
        lv_obj_set_style_text_color(s_wifi_info_label, COLOR_GREEN, 0);
    } else {
        lv_label_set_text(s_wifi_info_label, "");
    }
    lvgl_port_unlock();
}

/* Nucleo sem lock - usado tanto pela API publica (que trava sozinha,
 * chamada fora do contexto LVGL pelo dispatcher em main.c) quanto por
 * build_config_tab() (que ja roda dentro do lock, via ui_init()). */
static void apply_wifi_mode_ui(wifi_export_mode_t mode)
{
    bool is_sta = (mode == WIFI_EXPORT_MODE_STA);
    if (is_sta) lv_obj_add_state(s_wifi_mode_sw, LV_STATE_CHECKED);
    else        lv_obj_clear_state(s_wifi_mode_sw, LV_STATE_CHECKED);
    lv_label_set_text(s_wifi_mode_status_lbl, is_sta ? "Cliente" : "AP proprio");

    /* So um dos dois blocos fica visivel por vez - mesma logica de
     * antes, agora simetrica (o bloco AP tambem esconde, nao so o de
     * Cliente). */
    if (is_sta) lv_obj_clear_flag(s_wifi_sta_box, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(s_wifi_sta_box, LV_OBJ_FLAG_HIDDEN);

    if (is_sta) lv_obj_add_flag(s_wifi_ap_box, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(s_wifi_ap_box, LV_OBJ_FLAG_HIDDEN);

    /* SSID/senha do AP proprio nao fazem sentido mostrar enquanto o
     * modo atual e cliente - limpa pra nao confundir. */
    if (is_sta) lv_label_set_text(s_wifi_info_label, "");
}

void ui_set_wifi_mode_ui(wifi_export_mode_t mode)
{
    if (!lvgl_port_lock(0)) return;
    apply_wifi_mode_ui(mode);
    lvgl_port_unlock();
}

void ui_set_wifi_scan_results(char ssids[][WIFI_SCAN_SSID_MAX], int count)
{
    if (!lvgl_port_lock(0)) return;

    if (count <= 0) {
        lv_dropdown_set_options(s_wifi_scan_dd, "nenhuma rede encontrada");
        lvgl_port_unlock();
        return;
    }

    /* "ssid1\nssid2\n..." - buffer folgado pro maximo de resultados. */
    static char opts[WIFI_SCAN_MAX_RESULTS * (WIFI_SCAN_SSID_MAX + 1) + 1];
    size_t used = 0;
    opts[0] = '\0';
    for (int i = 0; i < count; i++) {
        int n = snprintf(opts + used, sizeof(opts) - used, "%s%s", i > 0 ? "\n" : "", ssids[i]);
        if (n < 0 || (size_t)n >= sizeof(opts) - used) break;
        used += (size_t)n;
    }
    lv_dropdown_set_options(s_wifi_scan_dd, opts);

    lvgl_port_unlock();
}

void ui_set_wifi_sta_state(wifi_sta_state_t state, const char *ip)
{
    if (!lvgl_port_lock(0)) return;

    lv_label_set_text(s_sta_connect_lbl,
        state == WIFI_STA_STATE_CONNECTED  ? "Desconectar" :
        state == WIFI_STA_STATE_CONNECTING ? "Conectando..." : "Conectar");

    switch (state) {
    case WIFI_STA_STATE_CONNECTED:
        lv_label_set_text_fmt(s_wifi_sta_status_lbl, "conectado - %s ou %s.local",
                               ip ? ip : "?", wifi_export_get_mdns_hostname());
        lv_obj_set_style_text_color(s_wifi_sta_status_lbl, COLOR_GREEN, 0);
        break;
    case WIFI_STA_STATE_CONNECTING:
        lv_label_set_text(s_wifi_sta_status_lbl, "conectando...");
        lv_obj_set_style_text_color(s_wifi_sta_status_lbl, COLOR_GOLD, 0);
        break;
    case WIFI_STA_STATE_FAILED:
        lv_label_set_text(s_wifi_sta_status_lbl, "falha ao conectar - confira rede/senha");
        lv_obj_set_style_text_color(s_wifi_sta_status_lbl, COLOR_RED, 0);
        break;
    case WIFI_STA_STATE_IDLE:
    default:
        lv_label_set_text(s_wifi_sta_status_lbl, "desconectado");
        lv_obj_set_style_text_color(s_wifi_sta_status_lbl, COLOR_MUTED, 0);
        break;
    }

    lvgl_port_unlock();
}

void ui_set_usb_mode_state(bool active)
{
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_usb_btn_label, active ? "Encerrar modo pen drive" : "Modo pen drive (USB)");

    /* Icone USB piscando na status bar enquanto o pen drive estiver
     * ativo - mesmo padrao de animacao do icone WIFI (reusa
     * wifi_icon_blink_anim_cb, que so mexe no text_opa do var). Start/
     * stop aqui mesmo porque esta funcao ja e' chamada exatamente na
     * transicao (main.c, APP_EVT_USB_MODE_TOGGLE) - sem polling. */
    static bool s_usb_icon_blinking = false;
    if (active) {
        lv_obj_clear_flag(s_usb_icon, LV_OBJ_FLAG_HIDDEN);
        if (!s_usb_icon_blinking) {
            s_usb_icon_blinking = true;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_usb_icon);
            lv_anim_set_exec_cb(&a, wifi_icon_blink_anim_cb);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
            lv_anim_set_time(&a, 600);
            lv_anim_set_playback_time(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else {
        lv_obj_add_flag(s_usb_icon, LV_OBJ_FLAG_HIDDEN);
        if (s_usb_icon_blinking) {
            s_usb_icon_blinking = false;
            lv_anim_delete(s_usb_icon, wifi_icon_blink_anim_cb);
            lv_obj_set_style_text_opa(s_usb_icon, LV_OPA_COVER, 0); /* reset pra proxima vez */
        }
    }
    lvgl_port_unlock();
}

void ui_set_ble_radio_state(bool enabled)
{
    if (!lvgl_port_lock(0)) return;
    if (enabled) {
        lv_obj_add_state(s_ble_enable_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_ble_enable_sw, LV_STATE_CHECKED);
        lv_label_set_text(s_ble_status_label, "desligado");
        lv_obj_set_style_text_color(s_ble_status_label, COLOR_MUTED, 0);
    }
    lvgl_port_unlock();
}

void ui_refresh_session_list(void)
{
    /* Leitura do SD fica FORA do lock - I/O lento nao pode segurar a
     * taskLVGL parada mais que o necessario. */
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);

    if (count == 0) {
        if (!lvgl_port_lock(0)) return;
        lv_dropdown_set_options(s_session_dropdown, "nenhuma sessao");
        lvgl_port_unlock();
        return;
    }

    static char opts_buf[SD_MAX_SESSIONS_LISTED * SD_SESSION_NAME_LEN];
    opts_buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(opts_buf, sessions[i].filename);
        if (i < count - 1) strcat(opts_buf, "\n");
    }
    if (!lvgl_port_lock(0)) return;
    lv_dropdown_set_options(s_session_dropdown, opts_buf);
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------
 * Aba PISTA - funcoes publicas
 * ------------------------------------------------------------------ */
/* Decide qual dos dois banners da aba CORRIDA aparece (nunca os dois ao
 * mesmo tempo): sem linha de chegada -> aviso dourado "marcar linha";
 * linha de chegada ok mas sessao ainda parada -> aviso verde "toque
 * RESET"; sessao gravando -> nenhum dos dois (tela limpa pra corrida). */
static void race_banner_refresh(bool finish_set)
{
    if (!finish_set) {
        lv_obj_clear_flag(s_race_warn_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_race_ready_banner, LV_OBJ_FLAG_HIDDEN);
    } else if (!s_session_recording_active) {
        lv_obj_add_flag(s_race_warn_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_race_ready_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_race_warn_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_race_ready_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_pista_status(void)
{
    /* Linha de chegada */
    double lat, lon; float hdg;
    bool finish_set = gps_get_finish_line(&lat, &lon, &hdg);
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_pista_finish_status, finish_set ? "definida" : "---");
    lv_obj_set_style_text_color(s_pista_finish_status,
                                finish_set ? COLOR_GREEN : COLOR_MUTED, 0);
    /* Coordenada abaixo do status - pedido do usuario pra conferir onde
     * exatamente a linha/setor foi marcado sem precisar abrir o SD/editor
     * web. 6 casas decimais ~= 11cm de resolucao, sobra pra conferencia
     * visual (nao e' pra navegacao de precisao). */
    if (finish_set) {
        lv_label_set_text_fmt(s_pista_finish_coord, "%.6f, %.6f", lat, lon);
    } else {
        lv_label_set_text(s_pista_finish_coord, "");
    }

    /* Aviso na aba CORRIDA - qual dos dois banners aparece */
    race_banner_refresh(finish_set);

    /* Setores - so os labels da aba PISTA (config). A barra inferior da aba
     * CORRIDA agora e fixa (VEL MAX/PREV/IDEAL) e nao depende de setor. */
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        bool set = gps_sector_is_set(i);
        lv_label_set_text(s_lbl_sector_status[i], set ? "definido" : "---");
        lv_obj_set_style_text_color(s_lbl_sector_status[i],
                                    set ? COLOR_GREEN : COLOR_MUTED, 0);
        if (set && gps_get_sector_point(i, &lat, &lon, &hdg)) {
            lv_label_set_text_fmt(s_lbl_sector_coord[i], "%.6f, %.6f", lat, lon);
        } else {
            lv_label_set_text(s_lbl_sector_coord[i], "");
        }
    }
    lvgl_port_unlock();
}

void ui_refresh_track_list(void)
{
    /* Leitura do SD fora do lock (mesmo motivo de ui_refresh_session_list) */
    static char names[TRACK_LIST_MAX][TRACK_NAME_MAX];
    int count = track_manager_list(names, TRACK_LIST_MAX);

    if (count == 0) {
        if (!lvgl_port_lock(0)) return;
        lv_dropdown_set_options(s_pista_track_dd, "(nenhuma pista salva)");
        /* lv_dropdown_set_options() nao dispara LV_EVENT_VALUE_CHANGED
         * sozinho - atualiza a visibilidade da row Carregar/Editar/Excluir
         * na mao (senao ela pode ficar visivel apontando pra uma pista
         * que acabou de ser excluida, por ex). */
        pista_update_select_actions_visibility();
        lvgl_port_unlock();
        return;
    }

    static char opts[TRACK_LIST_MAX * (TRACK_NAME_MAX + 1)];
    opts[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(opts, names[i]);
        if (i < count - 1) strcat(opts, "\n");
    }
    if (!lvgl_port_lock(0)) return;
    lv_dropdown_set_options(s_pista_track_dd, opts);
    pista_update_select_actions_visibility();
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Tela de loading no boot - ver overlay criado no fim de ui_init().
 *
 * Historico: essas duas foram as PRIMEIRAS a ganhar lvgl_port_lock(),
 * depois de um watchdog em campo (hang em lv_inv_area) - na epoca a
 * teoria era que so o spinner do boot criava concorrencia real entre a
 * task main e a taskLVGL. Um segundo watchdog em campo (taskLVGL presa
 * em lv_text_get_size, via ui_show_session_laps sem lock) provou que a
 * janela existe SEMPRE (refresh timer de 100ms + animacoes rodam o tempo
 * todo), entao hoje TODA funcao ui_* publica chamada de fora da taskLVGL
 * trava sozinha - ver nota em ui_flash_lap_complete. */
void ui_boot_set_status(const char *msg)
{
    if (!s_boot_status_lbl) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_boot_status_lbl, msg);
    lvgl_port_unlock();
}

void ui_boot_finish(void)
{
    if (!s_boot_overlay) return;
    if (!lvgl_port_lock(0)) return;
    /* lv_obj_delete() em vez de so esconder: o spinner (lv_spinner) tem
     * uma lv_anim_t propria que continua rodando pra sempre enquanto o
     * objeto existir, esconder ou nao - so escondendo, taskLVGL ficaria
     * animando esse spinner (agora invisivel) pelo resto da vida do
     * aparelho, mantendo o MESMO risco de corrida que causou o hang no
     * boot toda vez que o dispatcher de eventos (tambem sem lock, ver
     * app_main()/handle_event()) rodasse em paralelo. Deletar remove o
     * objeto e a anim junto, de vez. */
    lv_obj_delete(s_boot_overlay);
    s_boot_overlay = NULL;
    s_boot_status_lbl = NULL;
    lvgl_port_unlock();
}

void ui_on_track_loaded(const track_config_t *cfg)
{
    if (!lvgl_port_lock(0)) return;
    if (cfg) {
        lv_textarea_set_text(s_pista_name_ta, cfg->name);
    } else {
        lv_textarea_set_text(s_pista_name_ta, "");
    }
    lvgl_port_unlock();
    ui_update_pista_status(); /* trava sozinha */
}

void ui_show_pista_edit_panel(bool show)
{
    if (!s_pista_edit_panel) return;
    if (!lvgl_port_lock(0)) return;
    if (show) {
        lv_obj_clear_flag(s_pista_edit_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pista_edit_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

bool ui_get_track_draft(track_config_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->magic = TRACK_MAGIC;

    /* lv_textarea_get_text le a arvore LVGL - mesmo sendo so leitura,
     * o ponteiro retornado pode ser realocado pela taskLVGL no meio da
     * copia. Copia sob lock. */
    if (!lvgl_port_lock(0)) return false;
    const char *name = lv_textarea_get_text(s_pista_name_ta);
    if (!name || name[0] == '\0') {
        lvgl_port_unlock();
        return false;
    }
    strncpy(out->name, name, TRACK_NAME_MAX - 1);
    out->name[TRACK_NAME_MAX - 1] = '\0';
    lvgl_port_unlock();

    double lat, lon; float hdg;
    out->finish.is_set = gps_get_finish_line(&lat, &lon, &hdg);
    if (out->finish.is_set) {
        out->finish.lat = lat;
        out->finish.lon = lon;
        out->finish.heading_deg = hdg;
    }

    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        out->sectors[i].is_set = gps_get_sector_point(i, &lat, &lon, &hdg);
        if (out->sectors[i].is_set) {
            out->sectors[i].lat = lat;
            out->sectors[i].lon = lon;
            out->sectors[i].heading_deg = hdg;
        }
    }

    return true;
}

/* ----------------------------------------------------------------------
 * Comparacao de voltas - tocar numa linha seleciona (contorno azul),
 * tocar numa segunda abre o overlay com a comparacao lado a lado.
 * Tocar de novo na mesma linha desmarca. Selecao zera quando a lista e'
 * recarregada (troca de sessao / refresh).
 * ---------------------------------------------------------------------- */
static void lap_row_set_selected(int idx, bool sel)
{
    if (idx < 0 || idx >= s_laps_cache_count || !s_lap_rows[idx]) return;
    lv_obj_set_style_outline_color(s_lap_rows[idx], COLOR_BLUE, 0);
    lv_obj_set_style_outline_width(s_lap_rows[idx], sel ? 2 : 0, 0);
}

/* Converte os splits CUMULATIVOS (s1, s2 desde a largada) da volta em
 * tempos POR SEGMENTO (largada->S1, S1->S2, S2->chegada), em segundos.
 * Retorna false se a volta nao tem os 2 splits (setores nao cruzados ou
 * CSV antigo, gravado antes das colunas existirem). */
static bool lap_segments_s(const sd_lap_summary_t *L, float seg[3])
{
    if (L->sector_ms[0] == 0 || L->sector_ms[1] == 0 ||
        L->sector_ms[1] <= L->sector_ms[0] || L->lap_time_ms <= L->sector_ms[1]) return false;
    seg[0] = L->sector_ms[0] / 1000.0f;
    seg[1] = (L->sector_ms[1] - L->sector_ms[0]) / 1000.0f;
    seg[2] = (L->lap_time_ms - L->sector_ms[1]) / 1000.0f;
    return true;
}

static void show_lap_compare(int a, int b)
{
    const sd_lap_summary_t *A = &s_laps_cache[a];
    const sd_lap_summary_t *B = &s_laps_cache[b];
    char ta[16], tb[16];
    format_lap_time(ta, sizeof(ta), A->lap_time_ms);
    format_lap_time(tb, sizeof(tb), B->lap_time_ms);
    /* diff = volta B - volta A (ordem do toque): positivo = 2a volta
     * tocada e' mais lenta que a 1a. */
    int32_t diff_ms = (int32_t)B->lap_time_ms - (int32_t)A->lap_time_ms;

    static char body[512];
    int off = snprintf(body, sizeof(body),
        "VOLTA %-3lu     %s\n"
        "VOLTA %-3lu     %s\n"
        "\n"
        "DIFERENCA     %+.3fs\n"
        "VEL MAX       %d x %d km/h\n"
        "VEL MED       %d x %d km/h\n",
        (unsigned long)A->lap_number, ta,
        (unsigned long)B->lap_number, tb,
        (double)(diff_ms / 1000.0),
        (int)(A->max_speed_kmh + 0.5f), (int)(B->max_speed_kmh + 0.5f),
        (int)(A->avg_speed_kmh + 0.5f), (int)(B->avg_speed_kmh + 0.5f));

    /* Comparacao POR SEGMENTO - so quando as duas voltas tem splits
     * completos (sessao com setores + CSV novo). Mostra onde exatamente
     * o tempo foi ganho/perdido. */
    float sa[3], sb[3];
    if (lap_segments_s(A, sa) && lap_segments_s(B, sb)) {
        static const char *seg_name[3] = { "S1", "S2", "S3" };
        for (int i = 0; i < 3 && off < (int)sizeof(body); i++) {
            off += snprintf(body + off, sizeof(body) - off,
                "%s   %.3f x %.3f  (%+.3f)\n",
                seg_name[i], (double)sa[i], (double)sb[i], (double)(sb[i] - sa[i]));
        }
    }

    if (off < (int)sizeof(body)) {
        snprintf(body + off, sizeof(body) - off, "\n(toque para fechar)");
    }
    lv_label_set_text(s_cmp_body_label, body);
    lv_obj_clear_flag(s_cmp_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cmp_overlay);
}

static void lap_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_laps_cache_count) return;

    if (s_cmp_sel < 0) {
        s_cmp_sel = idx;
        lap_row_set_selected(idx, true);
    } else if (s_cmp_sel == idx) {
        lap_row_set_selected(idx, false);
        s_cmp_sel = -1;
    } else {
        int first = s_cmp_sel;
        lap_row_set_selected(first, false);
        s_cmp_sel = -1;
        show_lap_compare(first, idx);
    }
}

void ui_show_session_laps(uint32_t session_index)
{
    /* TODO o I/O de SD primeiro, SEM lock (parse do CSV inteiro pode
     * levar centenas de ms num cartao lento - segurar a taskLVGL isso
     * tudo congelaria a tela). So depois, com os dados em maos, trava e
     * reconstroi os widgets. */
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int session_count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);

    bool session_valid = ((int)session_index < session_count);

    int lap_count = 0;
    if (session_valid) {
        lap_count = sd_read_session_laps(sessions[session_index].filename, s_laps_cache, SD_MAX_LAPS_LISTED);
    }
    const sd_lap_summary_t *laps = s_laps_cache;

    if (!lvgl_port_lock(0)) return;

    /* lista nova = selecao de comparacao anterior nao vale mais */
    s_laps_cache_count = lap_count;
    s_cmp_sel = -1;
    memset(s_lap_rows, 0, sizeof(s_lap_rows));

    lv_obj_clean(s_laps_list);
    lv_obj_add_flag(s_laps_header, LV_OBJ_FLAG_HIDDEN);

    if (!session_valid) {
        lv_obj_t *err_lbl = lv_label_create(s_laps_list);
        lv_obj_set_style_text_font(err_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(err_lbl, COLOR_MUTED, 0);
        lv_label_set_text(err_lbl, "Sessao invalida");
        lvgl_port_unlock();
        return;
    }

    if (lap_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_laps_list);
        lv_obj_set_style_text_font(empty_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(empty_lbl, COLOR_MUTED, 0);
        lv_label_set_text(empty_lbl, "Nenhuma volta fechada nessa sessao");
        lvgl_port_unlock();
        return;
    }

    lv_obj_clear_flag(s_laps_header, LV_OBJ_FLAG_HIDDEN);

    uint32_t best = UINT32_MAX;
    for (int i = 0; i < lap_count; i++) {
        if (laps[i].lap_time_ms < best) best = laps[i].lap_time_ms;
    }

    for (int i = 0; i < lap_count; i++) {
        bool is_best = (laps[i].lap_time_ms == best);

        lv_obj_t *row = bare(s_laps_list);
        lv_obj_set_size(row, lv_pct(100), 42);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        /* comparacao de voltas: linha clicavel (ver lap_row_click_cb) */
        s_lap_rows[i] = row;
        lv_obj_add_event_cb(row, lap_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (is_best) {
            /* Pedido do usuario: destacar melhor volta com mais forca -
             * so o fundo escuro (bem proximo do preto de fundo da lista)
             * passava batido. Soma uma faixa de acento na borda esquerda
             * (mesma linguagem visual do acento lateral das celulas
             * ATUAL/BEST na aba CORRIDA) + borda fina ao redor. Cor do
             * TEMA (nao verde fixo) - e' destaque de marca, igual ao
             * acento das celulas; so o delta +/- ali embaixo e' que e'
             * semantico verde/vermelho. */
            lv_obj_set_style_bg_color(row, COLOR_PRIMARY_DIM, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(row, 5, 0);
            lv_obj_set_style_border_color(row, COLOR_PRIMARY, 0);
            lv_obj_set_style_pad_left(row, 12, 0); /* compensa a borda, senao o texto cola nela */
            lv_obj_set_style_radius(row, 4, 0);
        }

        lv_obj_t *num_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(num_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(num_lbl, is_best ? COLOR_PRIMARY : COLOR_TEXT, 0);
        lv_label_set_text_fmt(num_lbl, "Volta %lu", (unsigned long)laps[i].lap_number);

        lv_obj_t *time_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(time_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(time_lbl, is_best ? COLOR_PRIMARY : COLOR_TEXT, 0);
        char lap_time_buf[16];
        format_lap_time(lap_time_buf, sizeof(lap_time_buf), laps[i].lap_time_ms);
        lv_label_set_text(time_lbl, lap_time_buf);

        lv_obj_t *delta_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(delta_lbl, &font_kartbox_lg, 0);
        if (is_best) {
            lv_obj_set_style_text_color(delta_lbl, COLOR_PRIMARY, 0);
            lv_label_set_text(delta_lbl, "BEST");
        } else {
            /* BUG CORRIGIDO: mostrava laps[i].delta_ms (vindo do CSV),
             * que e' o delta contra o best DA EPOCA em que a volta
             * fechou - toda volta que foi novo recorde na hora saia
             * negativa (ex: -0.70) mesmo sendo mais LENTA que o best
             * final da sessao. Na lista o que interessa e' comparar
             * contra o best da sessao inteira (ja calculado acima como
             * 'best'), entao recalcula aqui e ignora o delta gravado.
             * Como is_best cobre o minimo, aqui rel_ms e' sempre > 0. */
            int32_t rel_ms = (int32_t)laps[i].lap_time_ms - (int32_t)best;
            lv_obj_set_style_text_color(delta_lbl, rel_ms <= 0 ? COLOR_GREEN : COLOR_RED, 0);
            lv_label_set_text_fmt(delta_lbl, "%+.2f", (double)(rel_ms / 1000.0f));
        }

        lv_obj_t *vmax_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(vmax_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(vmax_lbl, COLOR_MUTED, 0);
        if (laps[i].max_speed_kmh > 0.5f) {
            lv_label_set_text_fmt(vmax_lbl, "max %dkm/h", (int)(laps[i].max_speed_kmh + 0.5f));
        } else {
            lv_label_set_text(vmax_lbl, "--");
        }

        lv_obj_t *vavg_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(vavg_lbl, &font_kartbox_lg, 0);
        lv_obj_set_style_text_color(vavg_lbl, COLOR_MUTED, 0);
        if (laps[i].avg_speed_kmh > 0.5f) {
            lv_label_set_text_fmt(vavg_lbl, "med %dkm/h", (int)(laps[i].avg_speed_kmh + 0.5f));
        } else {
            lv_label_set_text(vavg_lbl, "--");
        }
    }
    lvgl_port_unlock();
}

/* ----------------------------------------------------------------------
 * Mapa da pista - ver overlay criado em ui_init() (s_map_overlay/
 * s_map_canvas). Desenha o tracado como segmentos de linha, cor
 * interpolada verde->amarelo->vermelho conforme a velocidade RELATIVA
 * aquela sessao (min/max da propria sessao, nao um valor absoluto fixo -
 * assim qualquer pista fica colorida por inteiro, nao so os trechos
 * onde bateu recorde de velocidade absoluta).
 * ---------------------------------------------------------------------- */
static void map_update_title(void)
{
    if (s_map_have_best && s_map_best_only) {
        lv_label_set_text_fmt(s_map_title_lbl, "%s  [so volta %lu]",
                              s_map_session_name, (unsigned long)s_map_best_lap);
        lv_label_set_text(s_map_mode_lbl, "Ver: melhor volta");
    } else {
        lv_label_set_text(s_map_title_lbl, s_map_session_name);
        lv_label_set_text(s_map_mode_lbl, s_map_have_best ? "Ver: sessao" : "(sem volta fechada)");
    }
}

void ui_show_session_map(uint32_t session_index)
{
    if (!s_map_canvas_buf) {
        ui_show_toast("Mapa indisponivel (memoria)", 2000);
        return;
    }

    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int session_count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);
    if ((int)session_index >= session_count) {
        ui_show_toast("Sessao invalida", 2000);
        return;
    }

    int count = sd_read_session_track(sessions[session_index].filename, s_map_pts, SD_MAX_TRACK_POINTS);
    if (count < 2) {
        ui_show_toast("Sem dados de posicao nessa sessao", 2500);
        return;
    }
    s_map_pt_count = count;
    strncpy(s_map_session_name, sessions[session_index].filename, sizeof(s_map_session_name) - 1);
    s_map_session_name[sizeof(s_map_session_name) - 1] = '\0';

    /* Melhor volta da sessao - alimenta o botao "Ver: melhor volta".
     * Buffer proprio (nao s_laps_cache) pra nao bagunçar a lista/selecao
     * de comparacao da aba VOLTAS. */
    static sd_lap_summary_t map_laps[SD_MAX_LAPS_LISTED];
    int map_lap_count = sd_read_session_laps(sessions[session_index].filename, map_laps, SD_MAX_LAPS_LISTED);
    s_map_have_best = false;
    s_map_best_lap  = 0;
    uint32_t best_ms = UINT32_MAX;
    for (int i = 0; i < map_lap_count; i++) {
        if (map_laps[i].lap_time_ms > 0 && map_laps[i].lap_time_ms < best_ms) {
            best_ms = map_laps[i].lap_time_ms;
            s_map_best_lap = map_laps[i].lap_number;
            s_map_have_best = true;
        }
    }
    s_map_best_only = false; /* sempre abre na visao da sessao inteira */

    /* Daqui pra baixo e' tudo LVGL (label + canvas + overlay) - chamado
     * da task main via evento, precisa do lock (ver nota em
     * ui_flash_lap_complete). I/O de SD ja aconteceu acima, fora dele. */
    if (!lvgl_port_lock(0)) return;
    map_update_title();
    map_draw_canvas();
    lv_obj_clear_flag(s_map_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

/* Desenha o canvas do mapa a partir do cache (s_map_pts/...), no modo
 * atual: sessao inteira OU so a melhor volta - sempre em heatmap de
 * velocidade (o "ghost" cinza sob o heatmap foi removido: sobreposicao
 * atrapalhava a leitura, feedback de campo). Escala/limites calculados
 * SO sobre os pontos do modo ativo - a visao "melhor volta" ganha zoom
 * justo nela. Chamar com o lock do LVGL em maos (ou de callback LVGL). */
static void map_draw_canvas(void)
{
    /* indice dos pontos participantes do modo atual */
    static int idx[SD_MAX_TRACK_POINTS];
    int m = 0;
    for (int i = 0; i < s_map_pt_count; i++) {
        if (s_map_best_only && s_map_have_best && s_map_pts[i].lap != s_map_best_lap) continue;
        idx[m++] = i;
    }
    if (m < 2) {
        lv_canvas_fill_bg(s_map_canvas, COLOR_BG, LV_OPA_COVER);
        return;
    }

    const sd_track_point_t *pts = s_map_pts;
    float min_x = pts[idx[0]].x_m, max_x = pts[idx[0]].x_m;
    float min_y = pts[idx[0]].y_m, max_y = pts[idx[0]].y_m;
    float min_speed = pts[idx[0]].speed_kmh, max_speed = pts[idx[0]].speed_kmh;
    for (int k = 1; k < m; k++) {
        const sd_track_point_t *p = &pts[idx[k]];
        if (p->x_m < min_x) min_x = p->x_m;
        if (p->x_m > max_x) max_x = p->x_m;
        if (p->y_m < min_y) min_y = p->y_m;
        if (p->y_m > max_y) max_y = p->y_m;
        if (p->speed_kmh < min_speed) min_speed = p->speed_kmh;
        if (p->speed_kmh > max_speed) max_speed = p->speed_kmh;
    }

    float span_x = max_x - min_x;
    float span_y = max_y - min_y;
    if (span_x < 1.0f) span_x = 1.0f;
    if (span_y < 1.0f) span_y = 1.0f;
    float speed_span = max_speed - min_speed;
    if (speed_span < 1.0f) speed_span = 1.0f;

    /* Encaixa o tracado no canvas preservando proporcao (usa o menor dos
     * dois fatores de escala, senao a pista sai esticada/achatada). */
    const float margin = 30.0f;
    float avail_w = (float)MAP_CANVAS_W - 2.0f * margin;
    float avail_h = (float)MAP_CANVAS_H - 2.0f * margin;
    float scale = avail_w / span_x;
    float scale_y = avail_h / span_y;
    if (scale_y < scale) scale = scale_y;

    float draw_w = span_x * scale;
    float draw_h = span_y * scale;
    float off_x = margin + (avail_w - draw_w) / 2.0f;
    float off_y = margin + (avail_h - draw_h) / 2.0f;

    lv_canvas_fill_bg(s_map_canvas, COLOR_BG, LV_OPA_COVER);

    /* Tentativa anterior: fechar/reabrir a layer a cada lote (init_layer/
     * finish_layer varias vezes por desenho) pra nao acumular ~800 draw
     * tasks no pool TLSF de uma vez. Isso SO TROCOU o sintoma: em vez do
     * pool estourar (Guru Meditation em lv_tlsf.c), o dispatcher de desenho
     * trava (taskLVGL preso pra sempre em lv_draw_dispatch_wait_for_request,
     * watchdog dispara). init_layer/finish_layer nao sao feitos pra serem
     * chamados repetidas vezes dentro de UM desenho - CHILD_CREATED/
     * CHILD_DELETED sao eventos que a unidade de desenho por software usa
     * pra contar layers ativas, e reabrir no meio bagunça essa contagem
     * (por isso a tela "piscava" - invalidations parciais no meio do
     * desenho - antes de travar de vez).
     *
     * Fix real: manter UM UNICO bracket init_layer/finish_layer (como o
     * uso normal do canvas pede), e em vez disso reduzir quantas
     * lv_draw_line() de fato acontecem, decimando o tracado pra no maximo
     * MAP_MAX_SEGMENTS segmentos - de sobra pra um canvas de 760x380 (nao
     * da pra perceber mais que ~200 segmentos num traçado desse tamanho),
     * e uma fracao pequena o suficiente do que os ~800 originais pra nao
     * pressionar o pool de 256KB da LVGL. */
    #define MAP_MAX_SEGMENTS (180)
    int step = (m - 1 + (MAP_MAX_SEGMENTS - 1)) / MAP_MAX_SEGMENTS;
    if (step < 1) step = 1;

    lv_layer_t canvas_layer;
    lv_canvas_init_layer(s_map_canvas, &canvas_layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = 4;
    dsc.round_start = 1;
    dsc.round_end = 1;

    /* Amarelo usado como ponto medio da escala verde->vermelho. */
    lv_color_t c_slow = COLOR_GREEN;
    lv_color_t c_mid  = lv_color_make(0xE6, 0xC8, 0x1E);
    lv_color_t c_fast = COLOR_RED;

    for (int k = step; k < m; k += step) {
        int prev = k - step;
        /* Ultimo lote: garante que o traçado sempre termina no ultimo
         * ponto de verdade, mesmo quando m-1 nao e' multiplo de step
         * (senao sobra um pedaco desenhado faltando no fim da volta). */
        int cur = (k + step >= m) ? (m - 1) : k;
        const sd_track_point_t *A = &pts[idx[prev]];
        const sd_track_point_t *B = &pts[idx[cur]];

        /* Na visao "sessao inteira" existe a emenda volta N -> N+1 (fim
         * e comeco no gate, continuo); na visao "melhor volta" o indice
         * filtrado pode juntar pontos NAO adjacentes se a volta cruzar o
         * limite do buffer decimado - segmento entre voltas diferentes
         * nao existe fisicamente, pula. */
        if (s_map_best_only && A->lap != B->lap) continue;

        /* Y da tela cresce pra baixo, Y do mundo (norte) cresce pra cima -
         * inverte (draw_h - ...) senao o mapa sai de cabeca pra baixo. */
        float x1 = off_x + (A->x_m - min_x) * scale;
        float y1 = off_y + (draw_h - (A->y_m - min_y) * scale);
        float x2 = off_x + (B->x_m - min_x) * scale;
        float y2 = off_y + (draw_h - (B->y_m - min_y) * scale);

        float speed_avg = (A->speed_kmh + B->speed_kmh) * 0.5f;
        float t = (speed_avg - min_speed) / speed_span;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        lv_color_t seg_color;
        if (t < 0.5f) {
            float u = t * 2.0f;
            seg_color = lv_color_mix(c_mid, c_slow, (uint8_t)(u * 255.0f));
        } else {
            float u = (t - 0.5f) * 2.0f;
            seg_color = lv_color_mix(c_fast, c_mid, (uint8_t)(u * 255.0f));
        }

        dsc.color = seg_color;
        dsc.p1.x = (int32_t)(x1 + 0.5f);
        dsc.p1.y = (int32_t)(y1 + 0.5f);
        dsc.p2.x = (int32_t)(x2 + 0.5f);
        dsc.p2.y = (int32_t)(y2 + 0.5f);
        lv_draw_line(&canvas_layer, &dsc);
    }

    lv_canvas_finish_layer(s_map_canvas, &canvas_layer);
    #undef MAP_MAX_SEGMENTS
}

/* ----------------------------------------------------------------------
 * BLE passkey overlay (Parte B da seguranca)
 * ---------------------------------------------------------------------- */
void ui_cycle_race_layout(void)
{
    if (!lvgl_port_lock(0)) return;
    race_layout_cycle_step();
    lvgl_port_unlock();
}

void ui_show_ota_progress(bool show)
{
    if (!lvgl_port_lock(0)) return;
    if (show) {
        lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ota_overlay);
    } else {
        lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void ui_show_ble_passkey(uint32_t passkey)
{
    if (!lvgl_port_lock(0)) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%06" PRIu32, passkey);
    lv_label_set_text(s_passkey_label, buf);
    lv_obj_clear_flag(s_passkey_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    ESP_LOGI("ui", "Passkey BLE exibido: %s", buf);
}

void ui_hide_ble_passkey(void)
{
    if (!lvgl_port_lock(0)) return;
    lv_obj_add_flag(s_passkey_overlay, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

