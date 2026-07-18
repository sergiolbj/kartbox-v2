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
 *
 * Modo AP vs STA (cliente): kartbox pode ser a propria rede
 * ("KartBox-XXXX", sempre funciona, isolada) OU se conectar numa rede
 * ja existente (sua casa, wifi do autodromo) - nesse caso o celular
 * nao precisa trocar de rede pra sincronizar pista/baixar sessao, e o
 * kartbox fica alcancavel em "kartbox.local" (mDNS) alem do IP.
 *
 * NAO suporta os dois ao mesmo tempo (APSTA) de proposito: o link com
 * o coprocessador C6 ja mostrou instabilidade (version mismatch
 * host/slave visto em log real), e AP+STA simultaneo e o tipo de
 * combinacao menos testada em setups esp_hosted - prefiro reduzir
 * superficie de risco a ganhar uma conveniencia marginal.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_EXPORT_MODE_AP  = 0, /* rede propria "KartBox-XXXX" - default, sempre funciona */
    WIFI_EXPORT_MODE_STA = 1, /* conecta numa rede existente (a do usuario) */
} wifi_export_mode_t;

typedef enum {
    WIFI_STA_STATE_IDLE,       /* nunca tentou conectar, ou foi desligado */
    WIFI_STA_STATE_CONNECTING,
    WIFI_STA_STATE_CONNECTED,
    WIFI_STA_STATE_FAILED,
} wifi_sta_state_t;

void wifi_export_init(void);

/** @return false se SD indisponivel, WiFi ja ativo, ou (modo STA) se
 *  nao conseguiu conectar dentro do timeout - ver
 *  wifi_export_get_sta_state() pro motivo exato depois de false. */
bool wifi_export_start(void);

void wifi_export_stop(void);
bool wifi_export_is_active(void);

/** SSID/senha do AP proprio (so fazem sentido no modo AP). */
const char *wifi_export_get_ssid(void);
const char *wifi_export_get_password(void);

/** @brief Troca a senha do AP proprio (persistida pela aba de config). */
void wifi_export_set_password(const char *password);

/* ------------------------------------------------------------------
 * Modo AP vs STA - so pode trocar com o wifi desligado (wifi_export_
 * is_active() == false); chamando com wifi ativo e no-op (log warning).
 * ------------------------------------------------------------------ */
void                wifi_export_set_mode(wifi_export_mode_t mode);
wifi_export_mode_t  wifi_export_get_mode(void);

/* ------------------------------------------------------------------
 * Modo cliente (STA) - conectar na rede do usuario.
 * ------------------------------------------------------------------ */
#define WIFI_SCAN_MAX_RESULTS (16)
#define WIFI_SCAN_SSID_MAX    (33) /* 32 + \0, limite do padrao 802.11 */

/**
 * @brief Escaneia redes wifi visiveis. BLOQUEANTE (~2-4s) - so chame em
 * resposta a acao do usuario (botao "Escanear"), nunca em hot path.
 * Recusa (retorna 0, log warning) se o AP proprio estiver ativo no
 * momento - escanear exige o radio em modo STA, derrubaria o AP; isso
 * e decisao do usuario (desligar o AP antes), nao automatica.
 * @return numero de redes encontradas (0..max_results), copiadas em
 * out_ssids. SSIDs vazios (redes ocultas) sao pulados.
 */
int wifi_export_scan(char out_ssids[][WIFI_SCAN_SSID_MAX], int max_results);

/** @brief Define SSID/senha da rede alvo. Nao conecta ainda - a
 *  conexao de fato acontece dentro de wifi_export_start() quando o
 *  modo atual for WIFI_EXPORT_MODE_STA. Chamador tipico persiste os
 *  mesmos valores via settings_set_wifi_sta_*() antes/depois. */
void wifi_export_set_sta_credentials(const char *ssid, const char *password);
const char *wifi_export_get_sta_ssid(void);

wifi_sta_state_t wifi_export_get_sta_state(void);

/** @brief Preenche out com o IP atual em texto ("192.168.1.47") quando
 *  WIFI_STA_STATE_CONNECTED; string vazia nos demais estados. */
void wifi_export_get_sta_ip(char *out, size_t out_size);

/** @brief Nome mDNS fixo do dispositivo, sem ".local" (== "kartbox").
 *  Resolvivel como http://kartbox.local/ por qualquer aparelho na
 *  mesma rede (AP proprio ou STA), independente do IP atribuido. */
const char *wifi_export_get_mdns_hostname(void);

#ifdef __cplusplus
}
#endif
