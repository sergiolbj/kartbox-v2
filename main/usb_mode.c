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
    host.slot = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 4;
    slot_cfg.clk = SD_CLK_PIN;
    slot_cfg.cmd = SD_CMD_PIN;
    slot_cfg.d0  = SD_D0_PIN;
    slot_cfg.d1  = SD_D1_PIN;
    slot_cfg.d2  = SD_D2_PIN;
    slot_cfg.d3  = SD_D3_PIN;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (sdmmc_host_init() != ESP_OK) return false;
    if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_cfg) != ESP_OK) return false;

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
        sdmmc_host_deinit();
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
    if (s_usb_card) {
        free(s_usb_card);
        s_usb_card = NULL;
    }
    sdmmc_host_deinit();

    s_active = false;
    sd_logger_remount();
    ESP_LOGI(TAG, "Modo USB encerrado, cartao de volta pro firmware");
}

bool usb_mode_is_active(void)
{
    return s_active;
}
