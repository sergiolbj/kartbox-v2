/*
 * display_init.c
 *
 * Ver display_init.h pro contexto completo de por que esse arquivo existe
 * em vez de bsp_display_start() da BSP generica da Espressif.
 *
 * Fontes usadas pra validar cada numero abaixo (nada aqui foi chutado):
 *   - Schematic oficial da placa (5-Schematic/*.png, JC4880P443_V1.0.pdf)
 *   - Demo funcional do fabricante:
 *     idf_examples/ESP-IDF/lvgl_sw_rotation/main/lvgl_sw_rotation.c
 *     (essa e a fonte da sequencia de init do ST7701S e do video_timing -
 *     sao valores especificos desse painel, nao genericos do datasheet)
 *   - Componente oficial espressif/esp_lcd_st7701 (esp-iot-solution)
 *   - Componente oficial espressif/esp_lcd_touch_gt911 (esp-bsp)
 */
#include "display_init.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display_init";

/* ---------- Pinagem (ver cabecalho do .h pra fonte de cada valor) ---------- */
#define PIN_LCD_RESET           (GPIO_NUM_5)
#define PIN_LCD_BACKLIGHT       (GPIO_NUM_23)
#define PIN_TOUCH_I2C_SDA       (GPIO_NUM_7)
#define PIN_TOUCH_I2C_SCL       (GPIO_NUM_8)
#define TOUCH_I2C_PORT          (I2C_NUM_1)

#define MIPI_DSI_PHY_LDO_CHAN        (3)
#define MIPI_DSI_PHY_LDO_VOLTAGE_MV  (2500)

#define BACKLIGHT_LEDC_TIMER    (LEDC_TIMER_1)
#define BACKLIGHT_LEDC_CHANNEL  (LEDC_CHANNEL_0)
#define BACKLIGHT_LEDC_FREQ_HZ  (5000)
#define BACKLIGHT_LEDC_RES_BITS (LEDC_TIMER_10_BIT)

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_lcd_panel_handle_t  s_panel   = NULL;
static lv_display_t           *s_disp    = NULL;

/* ----------------------------------------------------------------------
 * Sequencia de init do ST7701S pra esse painel especifico (480x800).
 * Copiada do demo funcional do fabricante - sao registradores internos
 * do controlador (gamma, timing de pixel, polaridade), nao um init
 * generico de datasheet. Mexer aqui sem saber o que cada linha faz pode
 * deixar a imagem distorcida ou com cor errada.
 * ---------------------------------------------------------------------- */
static const st7701_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},

    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09,
                        0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08,
                        0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},

    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},

    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0,
                        0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0,
                        0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
                        0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
                        0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    {0x11, (uint8_t[]){0x00}, 1, 120}, /* Sleep out, espera 120ms */
    {0x29, (uint8_t[]){0x00}, 1, 20},  /* Display ON, espera 20ms */
};

/* ---------- Backlight via LEDC (PWM) ---------- */

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_LEDC_RES_BITS,
        .timer_num       = BACKLIGHT_LEDC_TIMER,
        .freq_hz         = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc timer");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num   = PIN_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BACKLIGHT_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BACKLIGHT_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    return ledc_channel_config(&channel_cfg);
}

void display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (1023 * percent) / 100; /* 10 bits de resolucao */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL);
}

void display_backlight_on(void)  { display_set_brightness(100); }
void display_backlight_off(void) { display_set_brightness(0); }

/* ---------- I2C compartilhado (touch + sensores externos tipo MPU6050) ---------- */

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source     = I2C_CLK_SRC_DEFAULT,
        .i2c_port       = TOUCH_I2C_PORT,
        .sda_io_num     = PIN_TOUCH_I2C_SDA,
        .scl_io_num     = PIN_TOUCH_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

i2c_master_bus_handle_t display_get_i2c_bus(void)
{
    return s_i2c_bus;
}

/* ---------- Painel ST7701S via MIPI-DSI ---------- */

static esp_err_t panel_init(esp_lcd_panel_io_handle_t *out_io,
                             esp_lcd_dsi_bus_handle_t *out_dsi_bus)
{
    /* Liga a alimentacao do PHY MIPI-DSI (precisa estar ligado antes de
     * qualquer coisa tocar no barramento DSI). */
    static esp_ldo_channel_handle_t ldo_phy = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_phy), TAG, "ldo dsi phy");

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_cfg = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_cfg = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io), TAG, "panel io dbi");

    /* Timing real verificado no demo funcional do fabricante pra esse
     * painel 480x800 - NAO sao valores de exemplo genericos. */
    static esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 34,
        .virtual_channel    = 0,
        .pixel_format       = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs            = 1,
        .video_timing = {
            .h_size            = DISPLAY_H_RES,
            .v_size            = DISPLAY_V_RES,
            .hsync_back_porch  = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            .vsync_back_porch  = 8,
            .vsync_pulse_width = 2,
            .vsync_front_porch = 166,
        },
        .flags.use_dma2d = true,
    };

    static st7701_vendor_config_t vendor_cfg;
    vendor_cfg = (st7701_vendor_config_t){
        .init_cmds      = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .flags.use_mipi_interface = 1,
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RESET,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_cfg, &s_panel), TAG, "new panel st7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    *out_io = io;
    *out_dsi_bus = dsi_bus;
    return ESP_OK;
}

/* ---------- Touch GT911 ---------- */

static esp_lcd_touch_handle_t touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 100000;

    if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
        ESP_LOGW(TAG, "Touch IO falhou no endereco 0x5D, tentando 0x14 (backup)");
        tp_io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Touch GT911 nao respondeu em nenhum endereco");
            return NULL;
        }
    }

    /* Reset/int do touch sao NC nessa placa (reset compartilhado com o
     * LCD via GPIO5, ja pulsado em panel_init/esp_lcd_panel_reset). */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_H_RES,
        .y_max = DISPLAY_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    esp_lcd_touch_handle_t tp = NULL;
    if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 falhou");
        return NULL;
    }
    return tp;
}

/* ---------- Entry point ---------- */

lv_display_t *display_init_start(void)
{
    if (backlight_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar backlight");
        return NULL;
    }

    if (i2c_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar I2C (touch/sensores)");
        return NULL;
    }

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_dsi_bus_handle_t  dsi_bus  = NULL;
    if (panel_init(&panel_io, &dsi_bus) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar painel ST7701S");
        return NULL;
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar esp_lvgl_port");
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = panel_io,
        .panel_handle   = s_panel,
        .buffer_size    = DISPLAY_H_RES * DISPLAY_V_RES,
        .double_buffer  = false,
        .hres           = DISPLAY_H_RES,
        .vres           = DISPLAY_V_RES,
        .color_format   = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,  /* 32MB de PSRAM disponivel, sobra de boa */
            .sw_rotate   = true,  /* ST7701S nao tem rotacao por hardware */
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_extra_cfg = {
        .flags.avoid_tearing = false,
    };
    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_extra_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi falhou");
        return NULL;
    }

    esp_lcd_touch_handle_t touch = touch_init();
    if (touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = s_disp,
            .handle = touch,
        };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGW(TAG, "lvgl_port_add_touch falhou - tela funciona sem touch");
        }
    } else {
        ESP_LOGW(TAG, "Touch indisponivel - seguindo so com display (sem toque)");
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "Display 480x800 ST7701S + touch GT911 prontos");
    return s_disp;
}
