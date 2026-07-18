/*
 * usb_mode.c - ver usb_mode.h pro contexto e a ressalva de versao do
 * componente esp_tinyusb.
 */
#include "usb_mode.h"
#include "config.h"
#include "sd_logger.h"

#include <stdlib.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_log.h"

static const char *TAG = "usb_mode";

static bool           s_active = false;
static bool           s_driver_installed = false;
static sdmmc_card_t  *s_usb_card = NULL;

static void mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event->type == TINYUSB_MSC_EVENT_MOUNT_CHANGED) {
        ESP_LOGI(TAG, "Host %s o cartao",
                 event->mount_changed_data.is_mounted ? "montou" : "desmontou");
    }
}

void usb_mode_init(void)
{
    if (s_driver_installed) return;

    const tinyusb_config_t tusb_cfg = {0}; /* defaults do componente */
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install falhou (%s)", esp_err_to_name(err));
        return;
    }
    s_driver_installed = true;
    ESP_LOGI(TAG, "TinyUSB instalado (MSC ainda inativo ate usb_mode_enter)");
}

/* Reinicializa o cartao em modo cru (sem FATFS) pra servir de LUN do
 * MSC. Mesmos pinos/config do slot usados em sd_logger.c - duplicado
 * de proposito aqui porque os dois modos sao mutuamente exclusivos e
 * cada um cuida do proprio ciclo de vida do handle do cartao. */
static bool init_raw_card_for_msc(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* SLOT_1 aqui colidia com o esp_hosted (link SDIO do C6, usado pro BLE
     * "sempre ligado" - ver mesma correcao em sd_logger.c/sd_power_and_mount).
     * BLE fica ativo o boot inteiro, entao o slot 1 nunca esta livre pra
     * reuso - reinicializar ele aqui corrompia o driver SDIO do esp_hosted
     * em pleno uso e derrubava o firmware (reboot) ao entrar em modo pen
     * drive. SLOT_0 e' o mesmo que o cartao usa normalmente (sd_logger.c) -
     * ja foi cedido por sd_logger_unmount() logo antes desta chamada. */
    host.slot = SDMMC_HOST_SLOT_0;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    /* width=1 igual sd_logger.c - ver comentario la (D1-D3 provavelmente
     * marginais nessa placa, escrita em 4-bit falhava). Mantendo os dois
     * modos de uso do cartao consistentes. */
    slot_cfg.width = 1;
    slot_cfg.clk = SD_CLK_PIN;
    slot_cfg.cmd = SD_CMD_PIN;
    slot_cfg.d0  = SD_D0_PIN;
    slot_cfg.d1  = SD_D1_PIN;
    slot_cfg.d2  = SD_D2_PIN;
    slot_cfg.d3  = SD_D3_PIN;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* ESP_ERR_INVALID_STATE = host ja inicializado (pelo esp_hosted, que
     * mantem o slot 1 vivo pro C6, ou por um mount anterior) - nao e'
     * erro aqui, so significa que o periferico ja esta de pe' e basta
     * inicializar o NOSSO slot. */
    esp_err_t host_err = sdmmc_host_init();
    if (host_err != ESP_OK && host_err != ESP_ERR_INVALID_STATE) return false;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot_cfg) != ESP_OK) return false;

    s_usb_card = malloc(sizeof(sdmmc_card_t));
    if (!s_usb_card) return false;

    if (sdmmc_card_init(&host, s_usb_card) != ESP_OK) {
        free(s_usb_card);
        s_usb_card = NULL;
        return false;
    }
    return true;
}

bool usb_mode_enter(void)
{
    if (s_active) return false;
    if (!s_driver_installed) usb_mode_init();

    sd_logger_unmount();

    if (!init_raw_card_for_msc()) {
        ESP_LOGE(TAG, "Falha ao reinicializar cartao em modo cru pro MSC");
        sd_logger_remount();
        return false;
    }

    const tinyusb_msc_sdmmc_config_t msc_cfg = {
        .card = s_usb_card,
        .callback_mount_changed = mount_changed_cb,
    };
    if (tinyusb_msc_storage_init_sdmmc(&msc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar storage MSC - confira a API da sua versao do esp_tinyusb");
        free(s_usb_card);
        s_usb_card = NULL;
        sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0); /* NUNCA sdmmc_host_deinit() - ver usb_mode_exit */
        sd_logger_remount();
        return false;
    }

    s_active = true;
    ESP_LOGI(TAG, "Modo USB pen drive ativo");
    return true;
}

void usb_mode_exit(void)
{
    if (!s_active) return;

    tinyusb_msc_storage_deinit();

    /* BUG REAL corrigido (2a rodada do "sair do pen drive reinicia"):
     * deinit do storage SEM desinstalar o TinyUSB deixava a interface
     * MSC enumerada no host - com o cabo ainda plugado, o PC continua
     * mandando SCSI (TEST UNIT READY/INQUIRY) pra um storage que ja
     * morreu -> crash. E' a MESMA classe de bug ja documentada no boot
     * (por isso o driver nem e' instalado no arranque - ver comentario
     * em main.c). Desinstalar o driver tira o dispositivo do barramento
     * USB por completo; o proximo usb_mode_enter() reinstala. */
    esp_err_t err = tinyusb_driver_uninstall();
    if (err == ESP_OK) {
        s_driver_installed = false;
    } else {
        ESP_LOGW(TAG, "tinyusb_driver_uninstall falhou (%s) - driver segue instalado",
                 esp_err_to_name(err));
    }

    if (s_usb_card) {
        free(s_usb_card);
        s_usb_card = NULL;
    }
    /* BUG REAL corrigido: sdmmc_host_deinit() (global) desligava o
     * periferico SDMMC INTEIRO - inclusive o slot 1, que e' o link SDIO
     * do esp_hosted com o C6 (BLE/WiFi). O driver do hosted percebia o
     * barramento morto ("failed to read registers") e REINICIAVA o P4 de
     * proposito ("Host is resetting itself"). Sintoma: sair do modo pen
     * drive = reboot. A versao _slot() deinicializa SO o slot 0 (cartao
     * SD) e deixa o do C6 em paz; o proprio driver so derruba o host
     * global quando o ultimo slot sai. */
    sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);

    s_active = false;
    sd_logger_remount();
    ESP_LOGI(TAG, "Modo USB encerrado, cartao de volta pro firmware");
}

bool usb_mode_is_active(void)
{
    return s_active;
}
