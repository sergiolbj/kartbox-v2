/*
 * battery.h - leitura da bateria 1S Li-ion do KartBox v2.
 *
 * Hardware (confirmado no schematic GUITION JC4880P443):
 *   BAT+ --[ R52 68k ]--+--[ R57 100k ]-- GND
 *                       |
 *                    GPIO53 (ADC2_CH4)   + C61 10nF de filtro
 *
 * O IP5306 (U5) faz a carga/boost, mas seus pinos de status (LED1..3)
 * estao desconectados na placa e nao ha via I2C, entao NAO existe sinal
 * digital de "carregando" pro P4. battery_is_charging() e' uma heuristica
 * por tendencia de tensao (best-effort) - ver comentario no .c. Pra deteccao
 * confiavel seria preciso levar USB5V_IN a um GPIO livre por um divisor.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configura o ADC oneshot no GPIO53. Idempotente. Chame uma vez no boot. */
void battery_init(void);

/**
 * @brief Percentual estimado (0..100) da bateria, ja suavizado (EMA).
 * @return 0..100, ou -1 se o ADC nao foi inicializado / leitura invalida.
 */
int battery_get_percent(void);

/** @brief Tensao estimada da celula em volts (ja com o divisor aplicado). */
float battery_get_voltage(void);

/**
 * @brief Heuristica de "esta carregando" (sem pino dedicado no hardware).
 * Baseada em tendencia de subida da tensao. Pode dar falso negativo com
 * bateria cheia (tensao estabiliza). Nao use pra logica critica.
 */
bool battery_is_charging(void);

#ifdef __cplusplus
}
#endif
