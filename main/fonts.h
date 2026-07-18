/*
 * fonts.h
 *
 * A v1 ja gera fonts Montserrat customizadas via lv_font_conv (vimos os
 * .c gerados no repo, ~2.9MB). Esse header so formaliza quais tamanhos
 * a v2 precisa - se os nomes gerados na sua pipeline forem diferentes
 * desses, troca so aqui (1 lugar), o resto da UI nao muda.
 *
 * Tamanhos pedidos (gerar Montserrat Bold nesses pontos se ainda nao
 * existir, via https://lvgl.io/tools/fontconverter ou lv_font_conv):
 *   font_kartbox_huge -> 96px  (numero de velocidade)
 *   font_kartbox_2xl   -> 48px  (valores de celula: atual/best/volta/velmax)
 *   font_kartbox_xl    -> 36px  (numero de delta)
 *   font_kartbox_lg    -> 22px  (valores de celula: atual/best/volta/velmax)
 *   font_kartbox_sm     -> 12px  (labels, status bar, abas)
 *
 * Numeros usam algarismos da propria Montserrat (sao bem uniformes em
 * largura por padrao, dá o "ar" de telemetria sem precisar de uma
 * familia monoespaçada extra so pra isso).
 *
 * font_kartbox_2xl (48px) foi adicionada depois, so pra aba CORRIDA
 * (celulas ATUAL/BEST/VOLTA/VEL MAX) - pedido do usuario de deixar a
 * tela mais preenchida/com elementos maiores. Gerada com a mesma
 * fonte-base (LiberationSans-Bold) e as mesmas flags das demais.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(font_kartbox_huge)
LV_FONT_DECLARE(font_kartbox_2xl)
LV_FONT_DECLARE(font_kartbox_xl)
LV_FONT_DECLARE(font_kartbox_lg)
LV_FONT_DECLARE(font_kartbox_sm)

#ifdef __cplusplus
}
#endif
