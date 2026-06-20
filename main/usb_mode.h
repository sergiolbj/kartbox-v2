/*
 * usb_mode.h - kartbox v2
 *
 * Corrige o bug de concorrencia achado na v1: o cartao SD so pode ter
 * UM dono do barramento por vez. v1 so parava de gravar e deixava o
 * FATFS ainda "montado" do lado do firmware enquanto o PC escrevia via
 * MSC - risco real de corromper o sistema de arquivos se o firmware
 * tentasse ler o diretorio nesse meio tempo (a UI faz isso quando
 * atualiza a lista de sessoes).
 *
 * Aqui: usb_mode_enter() desmonta o FS por completo (sd_logger_unmount())
 * antes de expor o cartao cru via USB MSC. usb_mode_exit() remonta
 * depois. Nunca os dois ao mesmo tempo.
 *
 * ATENCAO: a API do componente esp_tinyusb pra MSC sobre SDMMC mudou
 * de nome/assinatura entre versoes do ESP-IDF. As chamadas em
 * usb_mode.c seguem o padrao do exemplo oficial
 * storage/usb_msc_on_sdcard pro IDF 5.x - se a sua versao tiver
 * nomes diferentes (tinyusb_msc_storage_init_sdmmc etc), e so esse
 * arquivo que precisa ajustar, o resto do projeto nao muda.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Instala o driver TinyUSB base. MSC ainda fica inativo ate
 * usb_mode_enter() ser chamado - chame uma vez em app_main(). */
void usb_mode_init(void);

/**
 * @brief Desmonta o FS do firmware e expoe o cartao cru via USB MSC.
 * @return false se ja estava ativo ou se a reinicializacao do cartao falhou
 * (nesse caso tenta devolver o cartao pro firmware automaticamente).
 */
bool usb_mode_enter(void);

/** @brief Encerra o MSC e devolve o cartao pro firmware (remonta o FS). */
void usb_mode_exit(void);

bool usb_mode_is_active(void);

#ifdef __cplusplus
}
#endif
