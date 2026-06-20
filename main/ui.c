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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ----------------------------------------------------------------------
 * Paleta - preto puro de fundo, verde de marca pra "bom", vermelho pra
 * "ruim", cinza pra info secundaria. Sem gradiente, borda fina reta -
 * visual de instrumento, nao de app.
 * ---------------------------------------------------------------------- */
static const lv_color_t COLOR_BG        = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t COLOR_BORDER    = LV_COLOR_MAKE(0x26, 0x26, 0x26);
static const lv_color_t COLOR_TEXT      = LV_COLOR_MAKE(0xF5, 0xF8, 0xF5);
static const lv_color_t COLOR_MUTED     = LV_COLOR_MAKE(0x7A, 0x84, 0x7A);
static const lv_color_t COLOR_GREEN     = LV_COLOR_MAKE(0x3E, 0xE0, 0x7A);
static const lv_color_t COLOR_GREEN_DIM = LV_COLOR_MAKE(0x16, 0x39, 0x20);
static const lv_color_t COLOR_RED       = LV_COLOR_MAKE(0xFF, 0x5A, 0x5A);
static const lv_color_t COLOR_RED_DIM   = LV_COLOR_MAKE(0x2A, 0x14, 0x14);
static const lv_color_t COLOR_BLUE      = LV_COLOR_MAKE(0x5F, 0xB8, 0xE8);
static const lv_color_t COLOR_GOLD      = LV_COLOR_MAKE(0xFF, 0xD7, 0x00);

#define CELL_W              (160)
#define DELTA_LED_PER_SIDE  (8)
#define DELTA_LED_MAX_MS    (1500.0f) /* magnitude que acende a barra inteira */

/* ----------------------------------------------------------------------
 * Estado / handles dos widgets
 * ---------------------------------------------------------------------- */
static lv_obj_t *s_gps_dot, *s_gps_label;
static lv_obj_t *s_mode_pill, *s_mode_label;
static lv_obj_t *s_rec_dot, *s_ble_icon;

static lv_obj_t *s_delta_segs_left[DELTA_LED_PER_SIDE];
static lv_obj_t *s_delta_segs_right[DELTA_LED_PER_SIDE];

static lv_obj_t *s_lbl_atual_val, *s_lbl_best_val, *s_lbl_volta_val, *s_lbl_velmax_val;
static lv_obj_t *s_lbl_speed_val, *s_lbl_delta_val;
static lv_obj_t *s_flash_overlay;

static lv_obj_t *s_tab_content_pista, *s_tab_content_race, *s_tab_content_laps, *s_tab_content_cfg;
static lv_obj_t *s_tab_lbl_pista, *s_tab_lbl_race, *s_tab_lbl_laps, *s_tab_lbl_cfg;

/* Widgets da aba PISTA */
static lv_obj_t *s_pista_name_ta;
static lv_obj_t *s_pista_finish_status;
static lv_obj_t *s_pista_track_dd;

static lv_obj_t *s_session_dropdown, *s_laps_list;
static lv_obj_t *s_sd_usage_label, *s_sd_usage_bar, *s_ble_status_label;
static lv_obj_t *s_delete_btn_label;
static lv_obj_t *s_wifi_btn_label, *s_wifi_info_label;
static lv_obj_t *s_usb_btn_label;
static lv_obj_t *s_utc_value_label, *s_gate_value_label, *s_min_lap_value_label;
static lv_obj_t *s_ble_name_ta, *s_wifi_pass_ta, *s_keyboard;

/* Setores opcionais - CORRIDA tab (strip de splits) */
static lv_obj_t *s_sector_strip;                            /* row no fundo da aba corrida */
static lv_obj_t *s_lbl_sector_split[GPS_MAX_SECTORS];      /* "S1: 23.456" */
/* Setores opcionais - PISTA tab */
static lv_obj_t *s_lbl_sector_status[GPS_MAX_SECTORS];     /* "definido" / "---" */

static bool       s_delete_armed = false;
static lv_timer_t *s_delete_arm_timer = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static float       s_max_speed_kmh = 0.0f;
static int         s_slow_tick = 0;

/* Toast - popup centralizado de feedback (linha marcada, etc.) */
static lv_obj_t  *s_toast_box;
static lv_obj_t  *s_toast_label;
static lv_timer_t *s_toast_timer;

/* Hold-progress - barra que anima enquanto piloto segura RESET pra encerrar */
static lv_obj_t  *s_hold_overlay;
static lv_obj_t  *s_hold_bar;

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

static lv_obj_t *make_cell(lv_obj_t *parent, const char *label_txt, lv_obj_t **out_value)
{
    lv_obj_t *cell = bare(parent);
    lv_obj_set_size(cell, lv_pct(100), lv_pct(50));
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(cell);
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, label_txt);

    lv_obj_t *val = lv_label_create(cell);
    lv_obj_set_style_text_font(val, &font_kartbox_lg, 0);
    lv_obj_set_style_text_color(val, COLOR_TEXT, 0);
    lv_label_set_text(val, "--");

    *out_value = val;
    return cell;
}

static lv_obj_t *make_led_seg(lv_obj_t *parent)
{
    lv_obj_t *seg = bare(parent);
    lv_obj_set_size(seg, 14, 16);
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
    lv_obj_set_size(bar, lv_pct(100), 24);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *gps_grp = bare(bar);
    lv_obj_set_flex_flow(gps_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gps_grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gps_grp, 6, 0);

    s_gps_dot = bare(gps_grp);
    lv_obj_set_size(s_gps_dot, 6, 6);
    lv_obj_set_style_radius(s_gps_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_gps_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_gps_dot, COLOR_MUTED, 0);

    s_gps_label = lv_label_create(gps_grp);
    lv_obj_set_style_text_font(s_gps_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_gps_label, COLOR_MUTED, 0);
    lv_label_set_text(s_gps_label, "GPS --");

    s_mode_pill = bare(bar);
    lv_obj_set_style_bg_opa(s_mode_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mode_pill, 10, 0);
    lv_obj_set_style_pad_hor(s_mode_pill, 9, 0);
    lv_obj_set_style_pad_ver(s_mode_pill, 2, 0);
    s_mode_label = lv_label_create(s_mode_pill);
    lv_obj_set_style_text_font(s_mode_label, &font_kartbox_sm, 0);
    lv_label_set_text(s_mode_label, "QUALY");

    lv_obj_t *right_grp = bare(bar);
    lv_obj_set_flex_flow(right_grp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_grp, 8, 0);

    s_rec_dot = bare(right_grp);
    lv_obj_set_size(s_rec_dot, 6, 6);
    lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rec_dot, COLOR_MUTED, 0);

    s_ble_icon = lv_label_create(right_grp);
    lv_obj_set_style_text_font(s_ble_icon, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_ble_icon, COLOR_MUTED, 0);
    lv_label_set_text(s_ble_icon, "BLE");
}

/* ----------------------------------------------------------------------
 * Barra de LED do delta - reaproveita a linguagem de shift-light de
 * RPM (que kart nao tem) pra mostrar ganho/perda de tempo de forma
 * legivel com o rabo do olho, sem precisar focar nos digitos.
 * ---------------------------------------------------------------------- */
static void build_delta_led_bar(lv_obj_t *parent)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_size(row, lv_pct(100), 22);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = bare(row);
    lv_obj_set_size(left, lv_pct(46), 18);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW_REVERSE);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 2, 0);
    for (int i = 0; i < DELTA_LED_PER_SIDE; i++) {
        s_delta_segs_left[i] = make_led_seg(left);
        lv_obj_set_style_bg_color(s_delta_segs_left[i], COLOR_RED_DIM, 0);
    }

    lv_obj_t *divider = bare(row);
    lv_obj_set_size(divider, 2, 18);
    lv_obj_set_style_bg_color(divider, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *right = bare(row);
    lv_obj_set_size(right, lv_pct(46), 18);
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
static void build_race_tab(lv_obj_t *parent)
{
    /* parent vira flex-column: main_row flex-grow=1 + sector_strip fixo em baixo */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *row = bare(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t *left = bare(row);
    lv_obj_set_size(left, CELL_W, lv_pct(100));
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_color(left, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    make_cell(left, "ATUAL", &s_lbl_atual_val);
    make_cell(left, "BEST", &s_lbl_best_val);

    lv_obj_t *center = bare(row);
    lv_obj_set_width(center, lv_pct(100));
    lv_obj_set_height(center, lv_pct(100));
    lv_obj_set_flex_grow(center, 1);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *speed_row = bare(center);
    lv_obj_set_flex_flow(speed_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(speed_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(speed_row, 6, 0);

    s_lbl_speed_val = lv_label_create(speed_row);
    lv_obj_set_style_text_font(s_lbl_speed_val, &font_kartbox_huge, 0);
    lv_obj_set_style_text_color(s_lbl_speed_val, COLOR_TEXT, 0);
    lv_label_set_text(s_lbl_speed_val, "0");

    lv_obj_t *speed_unit = lv_label_create(speed_row);
    lv_obj_set_style_text_font(speed_unit, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(speed_unit, COLOR_MUTED, 0);
    lv_label_set_text(speed_unit, "km/h");

    lv_obj_t *delta_row = bare(center);
    lv_obj_set_flex_flow(delta_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(delta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(delta_row, 8, 0);
    lv_obj_set_style_pad_top(delta_row, 4, 0);

    lv_obj_t *delta_lbl = lv_label_create(delta_row);
    lv_obj_set_style_text_font(delta_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(delta_lbl, COLOR_GREEN, 0);
    lv_label_set_text(delta_lbl, "DELTA");

    s_lbl_delta_val = lv_label_create(delta_row);
    lv_obj_set_style_text_font(s_lbl_delta_val, &font_kartbox_xl, 0);
    lv_obj_set_style_text_color(s_lbl_delta_val, COLOR_MUTED, 0);
    lv_label_set_text(s_lbl_delta_val, "--.--");

    /* overlay de flash - fora do fluxo de layout, cobre o centro
     * inteiro, usado so por ui_flash_lap_complete() */
    s_flash_overlay = bare(center);
    lv_obj_add_flag(s_flash_overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_flash_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_flash_overlay, COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(s_flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_flash_overlay, 0, 0);

    lv_obj_t *right = bare(row);
    lv_obj_set_size(right, CELL_W, lv_pct(100));
    lv_obj_set_style_border_width(right, 1, 0);
    lv_obj_set_style_border_color(right, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(right, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    make_cell(right, "VOLTA", &s_lbl_volta_val);
    make_cell(right, "VEL MAX", &s_lbl_velmax_val);

    /* Strip de splits de setor - visivel so quando ha pelo menos 1 setor definido.
     * Altura inicial 0; ui_update_sector_status() expande pra 24 quando necessario. */
    s_sector_strip = bare(parent);
    lv_obj_set_size(s_sector_strip, lv_pct(100), 0);
    lv_obj_set_style_border_width(s_sector_strip, 1, 0);
    lv_obj_set_style_border_color(s_sector_strip, COLOR_BORDER, 0);
    lv_obj_set_style_border_side(s_sector_strip, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_hor(s_sector_strip, 14, 0);
    lv_obj_set_flex_flow(s_sector_strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sector_strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                           LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        s_lbl_sector_split[i] = lv_label_create(s_sector_strip);
        lv_obj_set_style_text_font(s_lbl_sector_split[i], &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(s_lbl_sector_split[i], COLOR_MUTED, 0);
        lv_label_set_text_fmt(s_lbl_sector_split[i], "S%d: --", i + 1);
    }
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

static void build_laps_tab(lv_obj_t *parent)
{
    lv_obj_t *col = bare(parent);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 12, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, 0);

    lv_obj_t *header = bare(col);
    lv_obj_set_size(header, lv_pct(100), 32);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_session_dropdown = lv_dropdown_create(header);
    lv_obj_set_width(s_session_dropdown, 280);
    lv_obj_set_style_text_font(s_session_dropdown, &font_kartbox_sm, 0);
    lv_dropdown_set_options(s_session_dropdown, "nenhuma sessao");
    lv_obj_add_event_cb(s_session_dropdown, session_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh_btn = lv_button_create(header);
    lv_obj_set_size(refresh_btn, 76, 30);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
    lv_obj_center(refresh_lbl);
    lv_obj_set_style_text_font(refresh_lbl, &font_kartbox_sm, 0);
    lv_label_set_text(refresh_lbl, "atualizar");

    s_laps_list = bare(col);
    lv_obj_set_size(s_laps_list, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_laps_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_laps_list, 4, 0);
    lv_obj_add_flag(s_laps_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_laps_list, LV_DIR_VER);

    /* Leitura de volta-a-volta do CSV da sessao selecionada ainda
     * depende de uma funcao nova em sd_logger.h (ex: sd_read_session_laps).
     * Por enquanto a lista so confirma a selecao - dado completo entra
     * numa proxima etapa. */
    lv_obj_t *placeholder = lv_label_create(s_laps_list);
    lv_obj_set_style_text_font(placeholder, &font_kartbox_sm, 0);
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

static lv_obj_t *make_stepper_row(lv_obj_t *parent, const char *title,
                                   lv_event_cb_t minus_cb, lv_event_cb_t plus_cb,
                                   lv_obj_t **out_value_label)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, title);

    lv_obj_t *ctrl = bare(row);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctrl, 10, 0);

    lv_obj_t *btn_minus = lv_button_create(ctrl);
    lv_obj_set_size(btn_minus, 32, 28);
    lv_obj_add_event_cb(btn_minus, minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *minus_lbl = lv_label_create(btn_minus);
    lv_obj_center(minus_lbl);
    lv_label_set_text(minus_lbl, "-");

    lv_obj_t *val = lv_label_create(ctrl);
    lv_obj_set_style_text_font(val, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(val, COLOR_TEXT, 0);
    lv_obj_set_width(val, 70);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(val, "--");

    lv_obj_t *btn_plus = lv_button_create(ctrl);
    lv_obj_set_size(btn_plus, 32, 28);
    lv_obj_add_event_cb(btn_plus, plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *plus_lbl = lv_label_create(btn_plus);
    lv_obj_center(plus_lbl);
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
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_text_field(lv_obj_t *parent, const char *title, const char *initial)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, title);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, lv_pct(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 63);
    lv_obj_set_style_text_font(ta, &font_kartbox_sm, 0);
    if (initial) lv_textarea_set_text(ta, initial);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
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

static void wifi_btn_event_cb(lv_event_t *e)
{
    (void)e;
    app_event_post(APP_EVT_WIFI_EXPORT_TOGGLE, EVT_SRC_TOUCH);
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

static void pista_delete_cb(lv_event_t *e)
{
    (void)e;
    char name[TRACK_NAME_MAX];
    lv_dropdown_get_selected_str(s_pista_track_dd, name, sizeof(name));
    if (name[0] == '\0' || strcmp(name, "(nenhuma pista salva)") == 0) return;
    app_event_t evt = { .type = APP_EVT_TRACK_DELETE, .source = EVT_SRC_TOUCH };
    strncpy(evt.data.track_name, name, sizeof(evt.data.track_name) - 1);
    app_event_post_data(&evt);
}

static void pista_new_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_pista_name_ta, "");
    app_event_post(APP_EVT_TRACK_NEW, EVT_SRC_TOUCH);
}

/* Fabrica de linha de ponto (label + status + botoes opcionais) */
static lv_obj_t *make_pista_point_row(lv_obj_t *parent, const char *lbl_txt,
                                       lv_obj_t **out_status)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_size(row, lv_pct(100), 27);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_obj_set_width(lbl, 64);
    lv_label_set_text(lbl, lbl_txt);

    *out_status = lv_label_create(row);
    lv_obj_set_style_text_font(*out_status, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(*out_status, COLOR_MUTED, 0);
    lv_obj_set_flex_grow(*out_status, 1);
    lv_label_set_text(*out_status, "nao definida");

    return row;
}

static lv_obj_t *make_pista_btn(lv_obj_t *parent, const char *txt, int w,
                                  lv_color_t bg, lv_color_t fg,
                                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 25);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_label_set_text(lbl, txt);
    return btn;
}

static void build_pista_tab(lv_obj_t *parent)
{
    lv_obj_t *col = bare(parent);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_hor(col, 14, 0);
    lv_obj_set_style_pad_ver(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 7, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    /* ---- Nome da pista ---- */
    lv_obj_t *name_row = bare(col);
    lv_obj_set_size(name_row, lv_pct(100), 30);
    lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(name_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(name_row, 8, 0);
    lv_obj_t *name_lbl = lv_label_create(name_row);
    lv_obj_set_style_text_font(name_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(name_lbl, COLOR_MUTED, 0);
    lv_obj_set_width(name_lbl, 64);
    lv_label_set_text(name_lbl, "NOME");
    s_pista_name_ta = lv_textarea_create(name_row);
    lv_obj_set_flex_grow(s_pista_name_ta, 1);
    lv_obj_set_height(s_pista_name_ta, 28);
    lv_textarea_set_one_line(s_pista_name_ta, true);
    lv_textarea_set_max_length(s_pista_name_ta, TRACK_NAME_MAX - 1);
    lv_obj_set_style_text_font(s_pista_name_ta, &font_kartbox_sm, 0);
    lv_obj_add_event_cb(s_pista_name_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);

    /* ---- Linha de chegada ---- */
    lv_obj_t *finish_row;
    finish_row = make_pista_point_row(col, "CHEGADA", &s_pista_finish_status);
    make_pista_btn(finish_row, "Marcar", 72,
                   COLOR_GREEN_DIM, COLOR_GREEN, pista_setline_cb, NULL);

    /* ---- Setores S1 / S2 ---- */
    static const char *sec_names[GPS_MAX_SECTORS] = { "S1", "S2" };
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        lv_obj_t *srow = make_pista_point_row(col, sec_names[i], &s_lbl_sector_status[i]);
        make_pista_btn(srow, "Marcar", 72,
                       COLOR_GREEN_DIM, COLOR_GREEN, sector_mark_cb, (void *)(intptr_t)i);
        make_pista_btn(srow, "Limpar", 62,
                       COLOR_RED_DIM, COLOR_RED, sector_clear_cb, (void *)(intptr_t)i);
    }

    /* ---- Divider ---- */
    lv_obj_t *div1 = bare(col);
    lv_obj_set_size(div1, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div1, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div1, LV_OPA_COVER, 0);

    /* ---- Dropdown de pistas salvas ---- */
    lv_obj_t *dd_row = bare(col);
    lv_obj_set_size(dd_row, lv_pct(100), 30);
    lv_obj_set_flex_flow(dd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dd_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dd_row, 6, 0);
    s_pista_track_dd = lv_dropdown_create(dd_row);
    lv_obj_set_flex_grow(s_pista_track_dd, 1);
    lv_obj_set_height(s_pista_track_dd, 28);
    lv_obj_set_style_text_font(s_pista_track_dd, &font_kartbox_sm, 0);
    lv_dropdown_set_options(s_pista_track_dd, "(nenhuma pista salva)");
    make_pista_btn(dd_row, "Carregar", 78,
                   COLOR_GREEN_DIM, COLOR_GREEN, pista_load_cb, NULL);
    make_pista_btn(dd_row, "Excluir", 66,
                   COLOR_RED_DIM, COLOR_RED, pista_delete_cb, NULL);

    /* ---- Divider ---- */
    lv_obj_t *div2 = bare(col);
    lv_obj_set_size(div2, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div2, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(div2, LV_OPA_COVER, 0);

    /* ---- Botoes de acao ---- */
    lv_obj_t *act_row = bare(col);
    lv_obj_set_size(act_row, lv_pct(100), 30);
    lv_obj_set_flex_flow(act_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *new_btn = lv_button_create(act_row);
    lv_obj_set_size(new_btn, 160, 28);
    lv_obj_add_event_cb(new_btn, pista_new_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *new_lbl = lv_label_create(new_btn);
    lv_obj_center(new_lbl);
    lv_obj_set_style_text_font(new_lbl, &font_kartbox_sm, 0);
    lv_label_set_text(new_lbl, "Nova Pista");

    lv_obj_t *save_btn = lv_button_create(act_row);
    lv_obj_set_size(save_btn, 200, 28);
    lv_obj_set_style_bg_color(save_btn, COLOR_GREEN_DIM, 0);
    lv_obj_add_event_cb(save_btn, pista_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_obj_center(save_lbl);
    lv_obj_set_style_text_font(save_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(save_lbl, COLOR_GREEN, 0);
    lv_label_set_text(save_lbl, "Salvar Pista");
}

static void build_config_tab(lv_obj_t *parent)
{
    lv_obj_t *col = bare(parent);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 14, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);

    lv_obj_t *sd_row = bare(col);
    lv_obj_set_size(sd_row, lv_pct(100), 28);
    lv_obj_set_flex_flow(sd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sd_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_sd_usage_label = lv_label_create(sd_row);
    lv_obj_set_style_text_font(s_sd_usage_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_sd_usage_label, COLOR_TEXT, 0);
    lv_label_set_text(s_sd_usage_label, "SD --");

    s_sd_usage_bar = lv_bar_create(sd_row);
    lv_obj_set_size(s_sd_usage_bar, 180, 6);
    lv_bar_set_range(s_sd_usage_bar, 0, 100);
    lv_bar_set_value(s_sd_usage_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_sd_usage_bar, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sd_usage_bar, COLOR_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sd_usage_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sd_usage_bar, 3, LV_PART_INDICATOR);

    make_stepper_row(col, "FUSO HORARIO", utc_minus_cb, utc_plus_cb, &s_utc_value_label);
    make_stepper_row(col, "RAIO DO GATE", gate_minus_cb, gate_plus_cb, &s_gate_value_label);
    make_stepper_row(col, "MIN VOLTA",    min_lap_minus_cb, min_lap_plus_cb, &s_min_lap_value_label);
    update_utc_label();
    update_gate_label();
    update_min_lap_label();

    lv_obj_t *usb_btn = lv_button_create(col);
    lv_obj_set_size(usb_btn, lv_pct(100), 38);
    lv_obj_add_event_cb(usb_btn, usb_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_usb_btn_label = lv_label_create(usb_btn);
    lv_obj_center(s_usb_btn_label);
    lv_obj_set_style_text_font(s_usb_btn_label, &font_kartbox_sm, 0);
    lv_label_set_text(s_usb_btn_label, "Modo pen drive (USB)");

    lv_obj_t *wifi_btn = lv_button_create(col);
    lv_obj_set_size(wifi_btn, lv_pct(100), 38);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_wifi_btn_label = lv_label_create(wifi_btn);
    lv_obj_center(s_wifi_btn_label);
    lv_obj_set_style_text_font(s_wifi_btn_label, &font_kartbox_sm, 0);
    lv_label_set_text(s_wifi_btn_label, "Ativar WiFi export");

    s_wifi_info_label = lv_label_create(col);
    lv_obj_set_style_text_font(s_wifi_info_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_wifi_info_label, COLOR_MUTED, 0);
    lv_label_set_text(s_wifi_info_label, "");

    lv_obj_t *del_btn = lv_button_create(col);
    lv_obj_set_size(del_btn, lv_pct(100), 38);
    lv_obj_set_style_bg_color(del_btn, COLOR_RED_DIM, 0);
    lv_obj_add_event_cb(del_btn, delete_btn_event_cb, LV_EVENT_CLICKED, NULL);
    s_delete_btn_label = lv_label_create(del_btn);
    lv_obj_center(s_delete_btn_label);
    lv_obj_set_style_text_font(s_delete_btn_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_delete_btn_label, COLOR_RED, 0);
    lv_label_set_text(s_delete_btn_label, "Apagar tudo (SD)");

    lv_obj_t *ble_row = bare(col);
    lv_obj_set_size(ble_row, lv_pct(100), 30);
    lv_obj_set_flex_flow(ble_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ble_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ble_lbl = lv_label_create(ble_row);
    lv_obj_set_style_text_font(ble_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(ble_lbl, COLOR_MUTED, 0);
    lv_label_set_text(ble_lbl, "BLE telemetria");

    s_ble_status_label = lv_label_create(ble_row);
    lv_obj_set_style_text_font(s_ble_status_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_ble_status_label, COLOR_MUTED, 0);
    lv_label_set_text(s_ble_status_label, "inativo");

    /* Campos de texto editaveis - teclado criado em ui_init() */
    s_ble_name_ta  = make_text_field(col, "NOME BLE",    settings_get_ble_name());
    s_wifi_pass_ta = make_text_field(col, "SENHA WIFI",  settings_get_wifi_password());

    lv_obj_t *wifi_note = lv_label_create(col);
    lv_obj_set_style_text_font(wifi_note, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(wifi_note, COLOR_MUTED, 0);
    lv_label_set_text(wifi_note, "min 8 chars  /  BLE: reinicie pra aplicar");
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
    lv_obj_set_style_text_font(lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(lbl, COLOR_MUTED, 0);
    lv_label_set_text(lbl, text);

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
            lv_obj_set_style_text_color(labels[i], COLOR_GREEN, 0);
        } else {
            lv_obj_add_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(labels[i], COLOR_MUTED, 0);
        }
    }
}

static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_tab(idx);
}

static void build_tab_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = bare(parent);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COLOR_BORDER, 0);
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

/* ----------------------------------------------------------------------
 * Refresh em tempo real (10Hz) - velocidade, delta, LEDs, status bar.
 * Itens lentos (SD/BLE da aba Config) so a cada 20 ticks (~2s).
 * ---------------------------------------------------------------------- */
static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    gps_sample_t s;
    gps_get_latest(&s);

    if (s.fix_valid && s.speed_kmh > s_max_speed_kmh) {
        s_max_speed_kmh = s.speed_kmh;
    }

    lv_label_set_text_fmt(s_lbl_speed_val, "%d", (int)(s.speed_kmh + 0.5f));
    lv_label_set_text_fmt(s_lbl_velmax_val, "%d", (int)(s_max_speed_kmh + 0.5f));
    lv_label_set_text_fmt(s_lbl_volta_val, "%lu", (unsigned long)s.lap_number);

    lv_label_set_text_fmt(s_lbl_atual_val, "%lu.%03lu",
                           (unsigned long)(s.lap_time_ms / 1000),
                           (unsigned long)(s.lap_time_ms % 1000));

    if (s.best_lap_ms > 0) {
        lv_label_set_text_fmt(s_lbl_best_val, "%lu.%03lu",
                               (unsigned long)(s.best_lap_ms / 1000),
                               (unsigned long)(s.best_lap_ms % 1000));
    } else {
        lv_label_set_text(s_lbl_best_val, "--.---");
    }

    bool has_best = s.best_lap_ms > 0;
    if (has_best) {
        lv_label_set_text_fmt(s_lbl_delta_val, "%+.2f", (double)(s.last_delta_ms / 1000.0f));
        lv_obj_set_style_text_color(s_lbl_delta_val,
            (s.last_delta_ms <= 0) ? COLOR_GREEN : COLOR_RED, 0);
    } else {
        lv_label_set_text(s_lbl_delta_val, "--.--");
        lv_obj_set_style_text_color(s_lbl_delta_val, COLOR_MUTED, 0);
    }

    int lit = 0;
    if (has_best) {
        lit = (int)((fabsf((float)s.last_delta_ms) / DELTA_LED_MAX_MS) * DELTA_LED_PER_SIDE);
        if (lit > DELTA_LED_PER_SIDE) lit = DELTA_LED_PER_SIDE;
    }
    bool gaining = has_best && s.last_delta_ms < 0;
    bool losing  = has_best && s.last_delta_ms > 0;
    for (int i = 0; i < DELTA_LED_PER_SIDE; i++) {
        lv_obj_set_style_bg_color(s_delta_segs_right[i],
            (gaining && i < lit) ? COLOR_GREEN : COLOR_GREEN_DIM, 0);
        lv_obj_set_style_bg_color(s_delta_segs_left[i],
            (losing && i < lit) ? COLOR_RED : COLOR_RED_DIM, 0);
    }

    /* Splits de setor - so atualiza labels se o strip estiver visivel (h>0) */
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        if (!s.sector_is_set[i]) continue;
        if (s.sector_split_ms[i] > 0) {
            bool is_best = (s.best_sector_ms[i] > 0 &&
                            s.sector_split_ms[i] <= s.best_sector_ms[i]);
            lv_label_set_text_fmt(s_lbl_sector_split[i], "S%d: %lu.%03lu",
                                   i + 1,
                                   (unsigned long)(s.sector_split_ms[i] / 1000),
                                   (unsigned long)(s.sector_split_ms[i] % 1000));
            lv_obj_set_style_text_color(s_lbl_sector_split[i],
                                         is_best ? COLOR_GREEN : COLOR_MUTED, 0);
        } else {
            lv_label_set_text_fmt(s_lbl_sector_split[i], "S%d: --", i + 1);
            lv_obj_set_style_text_color(s_lbl_sector_split[i], COLOR_MUTED, 0);
        }
    }

    lv_label_set_text_fmt(s_gps_label, "GPS %u", (unsigned)s.satellites);
    lv_obj_set_style_bg_color(s_gps_dot, s.fix_valid ? COLOR_GREEN : COLOR_MUTED, 0);

    if (++s_slow_tick >= 20) {
        s_slow_tick = 0;

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

        bool connected = ble_telemetry_is_connected();
        lv_obj_set_style_text_color(s_ble_icon, connected ? COLOR_BLUE : COLOR_MUTED, 0);
        lv_label_set_text(s_ble_status_label, connected ? "conectado" : "inativo");
        lv_obj_set_style_text_color(s_ble_status_label, connected ? COLOR_GREEN : COLOR_MUTED, 0);
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

void ui_flash_lap_complete(uint32_t lap_number, uint32_t lap_time_ms, int32_t delta_ms, bool is_new_best)
{
    (void)lap_number; (void)lap_time_ms;

    lv_color_t flash_color = is_new_best ? COLOR_GOLD : (delta_ms <= 0 ? COLOR_GREEN : COLOR_RED);
    lv_obj_set_style_bg_color(s_flash_overlay, flash_color, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash_overlay);
    lv_anim_set_values(&a, 90, 0);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, flash_anim_exec_cb);
    lv_anim_start(&a);
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
 * API publica
 * ---------------------------------------------------------------------- */
void ui_init(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    build_status_bar(screen);
    build_delta_led_bar(screen);

    lv_obj_t *content_area = bare(screen);
    lv_obj_set_width(content_area, lv_pct(100));
    lv_obj_set_flex_grow(content_area, 1);

    /* Tab 0: PISTA (padrao no boot) */
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
    show_tab(0);  /* começa na aba PISTA */

    /* Teclado flutuante - vive no layer_top pra aparecer acima de tudo.
     * Compartilhado pelos dois campos de texto da aba Config.
     * Criado aqui onde temos acesso a 'disp'. */
    s_keyboard = lv_keyboard_create(lv_display_get_layer_top(disp));
    lv_obj_set_size(s_keyboard, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_keyboard, &font_kartbox_sm, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

    /* ------------------------------------------------------------------
     * Toast overlay - feedback visual de acoes (linha marcada, etc.)
     * Vive em layer_top para flutuar acima de tudo, inclusive do teclado.
     * ------------------------------------------------------------------ */
    lv_obj_t *layer = lv_display_get_layer_top(disp);

    s_toast_box = bare(layer);
    lv_obj_set_size(s_toast_box, 300, LV_SIZE_CONTENT);
    lv_obj_align(s_toast_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_toast_box, LV_COLOR_MAKE(0x0A, 0x0A, 0x0A), 0);
    lv_obj_set_style_bg_opa(s_toast_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast_box, COLOR_GREEN, 0);
    lv_obj_set_style_border_width(s_toast_box, 1, 0);
    lv_obj_set_style_pad_ver(s_toast_box, 16, 0);
    lv_obj_set_style_pad_hor(s_toast_box, 20, 0);
    lv_obj_set_flex_flow(s_toast_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_toast_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_toast_box, 6, 0);
    /* Faixa verde no topo - identidade visual de "confirmado" */
    lv_obj_t *toast_accent = bare(s_toast_box);
    lv_obj_set_size(toast_accent, lv_pct(100), 2);
    lv_obj_set_style_bg_color(toast_accent, COLOR_GREEN, 0);
    lv_obj_set_style_bg_opa(toast_accent, LV_OPA_COVER, 0);
    s_toast_label = lv_label_create(s_toast_box);
    lv_obj_set_style_text_font(s_toast_label, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(s_toast_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_toast_label, "");
    lv_obj_add_flag(s_toast_box, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;

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
    lv_obj_t *hold_lbl = lv_label_create(s_hold_overlay);
    lv_obj_set_style_text_font(hold_lbl, &font_kartbox_sm, 0);
    lv_obj_set_style_text_color(hold_lbl, COLOR_MUTED, 0);
    lv_label_set_text(hold_lbl, "Segure para encerrar sessao");
    s_hold_bar = lv_bar_create(s_hold_overlay);
    lv_obj_set_size(s_hold_bar, 240, 6);
    lv_obj_set_style_bg_color(s_hold_bar, COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(s_hold_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_hold_bar, COLOR_RED, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_hold_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_hold_bar, 0, 0);
    lv_obj_set_style_radius(s_hold_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_hold_bar, 0, 100);
    lv_bar_set_value(s_hold_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_hold_overlay, LV_OBJ_FLAG_HIDDEN);

    s_refresh_timer = lv_timer_create(refresh_timer_cb, 100, NULL);

    ui_refresh_session_list();
}

void ui_set_mode_label(gps_race_mode_t mode)
{
    bool race = (mode == GPS_MODE_CORRIDA);
    lv_label_set_text(s_mode_label, race ? "RACE" : "QUALY");
    lv_obj_set_style_bg_color(s_mode_pill, race ? COLOR_RED_DIM : COLOR_GREEN_DIM, 0);
    lv_obj_set_style_text_color(s_mode_label, race ? COLOR_RED : COLOR_GREEN, 0);
}

void ui_set_recording_state(bool recording)
{
    if (recording) {
        s_max_speed_kmh = 0.0f; /* sessao nova, zera o pico de velocidade */
    }
    lv_obj_set_style_bg_color(s_rec_dot, recording ? COLOR_RED : COLOR_MUTED, 0);
}

void ui_update_sector_status(void)
{
    /* Delega para ui_update_pista_status que cobre finish + setores + strip */
    ui_update_pista_status();
}

void ui_set_wifi_export_state(bool active, const char *ssid, const char *password)
{
    lv_label_set_text(s_wifi_btn_label, active ? "Desligar WiFi export" : "Ativar WiFi export");
    if (active && ssid && password) {
        lv_label_set_text_fmt(s_wifi_info_label, "%s  /  %s  /  192.168.4.1", ssid, password);
        lv_obj_set_style_text_color(s_wifi_info_label, COLOR_GREEN, 0);
    } else {
        lv_label_set_text(s_wifi_info_label, "");
    }
}

void ui_set_usb_mode_state(bool active)
{
    lv_label_set_text(s_usb_btn_label, active ? "Encerrar modo pen drive" : "Modo pen drive (USB)");
}

void ui_refresh_session_list(void)
{
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);

    if (count == 0) {
        lv_dropdown_set_options(s_session_dropdown, "nenhuma sessao");
        return;
    }

    static char opts_buf[SD_MAX_SESSIONS_LISTED * SD_SESSION_NAME_LEN];
    opts_buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(opts_buf, sessions[i].filename);
        if (i < count - 1) strcat(opts_buf, "\n");
    }
    lv_dropdown_set_options(s_session_dropdown, opts_buf);
}

/* ------------------------------------------------------------------
 * Aba PISTA - funcoes publicas
 * ------------------------------------------------------------------ */
void ui_update_pista_status(void)
{
    /* Linha de chegada */
    double lat, lon; float hdg;
    bool finish_set = gps_get_finish_line(&lat, &lon, &hdg);
    lv_label_set_text(s_pista_finish_status, finish_set ? "definida" : "---");
    lv_obj_set_style_text_color(s_pista_finish_status,
                                finish_set ? COLOR_GREEN : COLOR_MUTED, 0);

    /* Setores (labels na aba PISTA) + strip de splits na aba CORRIDA */
    bool any_set = false;
    for (int i = 0; i < GPS_MAX_SECTORS; i++) {
        bool set = gps_sector_is_set(i);
        if (set) any_set = true;
        lv_label_set_text(s_lbl_sector_status[i], set ? "definido" : "---");
        lv_obj_set_style_text_color(s_lbl_sector_status[i],
                                    set ? COLOR_GREEN : COLOR_MUTED, 0);
        if (!set) {
            lv_label_set_text_fmt(s_lbl_sector_split[i], "S%d: --", i + 1);
            lv_obj_set_style_text_color(s_lbl_sector_split[i], COLOR_MUTED, 0);
        }
    }
    lv_obj_set_height(s_sector_strip, any_set ? 24 : 0);
}

void ui_refresh_track_list(void)
{
    static char names[TRACK_LIST_MAX][TRACK_NAME_MAX];
    int count = track_manager_list(names, TRACK_LIST_MAX);

    if (count == 0) {
        lv_dropdown_set_options(s_pista_track_dd, "(nenhuma pista salva)");
        return;
    }

    static char opts[TRACK_LIST_MAX * (TRACK_NAME_MAX + 1)];
    opts[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(opts, names[i]);
        if (i < count - 1) strcat(opts, "\n");
    }
    lv_dropdown_set_options(s_pista_track_dd, opts);
}

void ui_on_track_loaded(const track_config_t *cfg)
{
    if (cfg) {
        lv_textarea_set_text(s_pista_name_ta, cfg->name);
    } else {
        lv_textarea_set_text(s_pista_name_ta, "");
    }
    ui_update_pista_status();
}

bool ui_get_track_draft(track_config_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->magic = TRACK_MAGIC;

    const char *name = lv_textarea_get_text(s_pista_name_ta);
    if (!name || name[0] == '\0') return false;
    strncpy(out->name, name, TRACK_NAME_MAX - 1);
    out->name[TRACK_NAME_MAX - 1] = '\0';

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

void ui_show_session_laps(uint32_t session_index)
{
    static sd_session_entry_t sessions[SD_MAX_SESSIONS_LISTED];
    int session_count = sd_list_sessions(sessions, SD_MAX_SESSIONS_LISTED);

    lv_obj_clean(s_laps_list);

    if ((int)session_index >= session_count) {
        lv_obj_t *err_lbl = lv_label_create(s_laps_list);
        lv_obj_set_style_text_font(err_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(err_lbl, COLOR_MUTED, 0);
        lv_label_set_text(err_lbl, "Sessao invalida");
        return;
    }

    static sd_lap_summary_t laps[SD_MAX_LAPS_LISTED];
    int lap_count = sd_read_session_laps(sessions[session_index].filename, laps, SD_MAX_LAPS_LISTED);

    if (lap_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_laps_list);
        lv_obj_set_style_text_font(empty_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(empty_lbl, COLOR_MUTED, 0);
        lv_label_set_text(empty_lbl, "Nenhuma volta fechada nessa sessao");
        return;
    }

    uint32_t best = UINT32_MAX;
    for (int i = 0; i < lap_count; i++) {
        if (laps[i].lap_time_ms < best) best = laps[i].lap_time_ms;
    }

    for (int i = 0; i < lap_count; i++) {
        bool is_best = (laps[i].lap_time_ms == best);

        lv_obj_t *row = bare(s_laps_list);
        lv_obj_set_size(row, lv_pct(100), 26);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (is_best) {
            lv_obj_set_style_bg_color(row, COLOR_GREEN_DIM, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }

        lv_obj_t *num_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(num_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(num_lbl, COLOR_TEXT, 0);
        lv_label_set_text_fmt(num_lbl, "Volta %lu", (unsigned long)laps[i].lap_number);

        lv_obj_t *time_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(time_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(time_lbl, COLOR_TEXT, 0);
        lv_label_set_text_fmt(time_lbl, "%lu.%03lu",
                               (unsigned long)(laps[i].lap_time_ms / 1000),
                               (unsigned long)(laps[i].lap_time_ms % 1000));

        lv_obj_t *delta_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(delta_lbl, &font_kartbox_sm, 0);
        if (is_best) {
            lv_obj_set_style_text_color(delta_lbl, COLOR_GREEN, 0);
            lv_label_set_text(delta_lbl, "BEST");
        } else {
            lv_obj_set_style_text_color(delta_lbl, laps[i].delta_ms <= 0 ? COLOR_GREEN : COLOR_RED, 0);
            lv_label_set_text_fmt(delta_lbl, "%+.2f", (double)(laps[i].delta_ms / 1000.0f));
        }

        lv_obj_t *vmax_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(vmax_lbl, &font_kartbox_sm, 0);
        lv_obj_set_style_text_color(vmax_lbl, COLOR_MUTED, 0);
        if (laps[i].max_speed_kmh > 0.5f) {
            lv_label_set_text_fmt(vmax_lbl, "%dkm/h", (int)(laps[i].max_speed_kmh + 0.5f));
        } else {
            lv_label_set_text(vmax_lbl, "--");
        }
    }
}
