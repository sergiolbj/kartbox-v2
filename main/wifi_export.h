/*
 * wifi_export.h - kartbox v2
 *
 * Isso e o conserto do bug que comecou essa reescrita toda: v1 chamava
 * esp_wifi_init() direto, mas o P4 nao tem radio proprio - precisa do
 * esp-hosted falando com o C6. Sem o componente certo (mesma ressalva
 * do ble_telemetry.h), da crash por falta de "memoria" que na verdade
 * era WiFi tentando rodar sem ponte de verdade pro radio.
 *
 * Filosofia: WiFi fica DESLIGADO o resto do tempo (BLE ja cobre
 * telemetria ao vivo, leve). So sobe quando o usuario pede export na
 * aba Config, e desce sozinho depois de WIFI_AP_IDLE_TIMEOUT_MS sem
 * pedido nenhum - nao fica consumindo RAM/CPU/energia a toa numa
 * sessao de kart onde isso nunca vai ser usado.
 *
 * Servidor so serve a pasta de sessoes, leitura, com nome de arquivo
 * sanitizado (so basename, rejeita "..") - a v1 tinha essa brecha de
 * path traversal no codigo que nunca chegou a ser usado.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_export_init(void);

/** @return false se ja tinha SD indisponivel ou WiFi ja estava ativo. */
bool wifi_export_start(void);

void wifi_export_stop(void);
bool wifi_export_is_active(void);

/** SSID atual (gerado com sufixo do MAC pra ser unico por placa). */
const char *wifi_export_get_ssid(void);
const char *wifi_export_get_password(void);

/** @brief Troca a senha do AP (persistida pela aba de configuracoes). */
void wifi_export_set_password(const char *password);

#ifdef __cplusplus
}
#endif
