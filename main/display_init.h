/*
 * display_init.h
 *
 * Init MINIMO e proprio do display pra placa GUITION JC4880P443C_I_W
 * (ESP32-P4 + ESP32-C6, painel 480x800 ST7701S via MIPI-DSI, touch GT911).
 *
 * Por que isso existe em vez de usar bsp_display_start() da BSP oficial
 * "esp32_p4_function_ev_board" da Espressif:
 *
 *   A BSP oficial so tem 3 presets de painel (Kconfig BSP_LCD_TYPE):
 *     - 1024x600 (ek79007)
 *     - 1280x800 (ili9881c)
 *     - HDMI (lt8912b)
 *   Nenhum dos tres bate com o painel real dessa placa (480x800 ST7701S).
 *   Por isso o vendor demo da Guition usa uma copia LOCAL nao versionada
 *   do componente st7701 + um arquivo main.c proprio (fora da BSP) com os
 *   parametros de timing certos pro painel 480x800 - exatamente o arquivo
 *   que foi usado de referencia pra escrever este modulo.
 *
 *   Este arquivo evita depender de um common_components/ externo nao
 *   commitado no repo (que e por isso que o build da v1 so funcionava na
 *   maquina de quem montou o common_components manualmente).
 *
 * Pinagem confirmada contra o schematic oficial da placa (5-Schematic) e o
 * demo funcional do fabricante (idf_examples/ESP-IDF/lvgl_sw_rotation):
 *
 *   LCD reset........... GPIO5
 *   LCD backlight (PWM).. GPIO23
 *   Touch reset/int...... NC (reset do touch compartilha o reset do LCD,
 *                          GT911 nao precisa de RST/INT proprio aqui)
 *   I2C (touch + MPU).... SDA=GPIO7  SCL=GPIO8   (I2C_NUM_1)
 *   MIPI-DSI PHY power... LDO canal 3 @ 2500mV
 *
 * IMPORTANTE: o barramento I2C (GPIO7/8) e COMPARTILHADO entre o touch
 * GT911 e qualquer sensor I2C externo (MPU6050 incluso). Use
 * display_get_i2c_bus() pra pendurar o MPU no MESMO handle, nunca crie
 * um i2c_master_bus_config_t novo nesses pinos - dois periféricos I2C
 * brigando pelo mesmo par de pinos falha silenciosamente.
 */
#pragma once

#include "lvgl.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_H_RES   (480)
#define DISPLAY_V_RES   (800)

/**
 * @brief Inicializa DSI bus, painel ST7701S, touch GT911 e LVGL.
 *
 * Faz tudo: power do PHY MIPI-DSI, reset/init do painel com a sequencia
 * de comandos real do ST7701S pra esse painel, backlight via LEDC,
 * touch GT911 no I2C, e registra os dois (display + touch) no LVGL
 * atraves do esp_lvgl_port oficial.
 *
 * Backlight comeca DESLIGADO - chame display_backlight_on() depois.
 *
 * @return Ponteiro pro display LVGL pronto pra uso, ou NULL se algo falhou.
 */
lv_display_t *display_init_start(void);

/**
 * @brief Liga o backlight (100% de brilho).
 */
void display_backlight_on(void);

/**
 * @brief Desliga o backlight.
 */
void display_backlight_off(void);

/**
 * @brief Ajusta brilho do backlight.
 * @param percent 0-100
 */
void display_set_brightness(int percent);

/**
 * @brief Retorna o handle do barramento I2C usado pelo touch (GPIO7/8,
 * I2C_NUM_1), pra outros drivers (MPU6050, etc) penduraram seus
 * dispositivos no MESMO barramento em vez de criar um novo.
 *
 * So valido depois de display_init_start() retornar com sucesso.
 */
i2c_master_bus_handle_t display_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif
