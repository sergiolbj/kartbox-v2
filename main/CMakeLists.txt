idf_component_register(
    SRCS
        "main.c"
        "ui.c"
        "app_events.c"
        "battery.c"
        "ble_telemetry.c"
        "display_init.c"
        "gps.c"
        "sd_logger.c"
        "settings.c"
        "track_manager.c"
        "usb_mode.c"
        "wifi_export.c"
        # Fontes geradas pelo LVGL font converter (Montserrat Bold)
        # Gere em https://lvgl.io/tools/fontconverter e coloque nesta pasta:
        "font_kartbox_huge.c"   # 96 px — velocidade
        "font_kartbox_2xl.c"    # 48 px — valores de célula (ATUAL/BEST/VOLTA/VEL MAX)
        "font_kartbox_xl.c"     # 36 px — delta
        "font_kartbox_lg.c"     # 22 px — valores de célula
        "font_kartbox_sm.c"     # 12 px — labels e status bar
    INCLUDE_DIRS "."
    REQUIRES
        # Componentes built-in do ESP-IDF
        freertos
        driver
        esp_adc        # leitura da bateria no GPIO53 (battery.c)
        esp_timer
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        fatfs
        sdmmc
        esp_hw_support
        bt
        # app_update — OTA via WiFi export (esp_ota_* em wifi_export.c/main.c)
        app_update
        # esp_http_client + mbedtls (crt bundle) — download A-GPS (AssistNow,
        # HTTPS obrigatorio) em wifi_export.c
        esp_http_client
        mbedtls
        # espcoredump + esp_partition — salvar coredump pro SD no boot (main.c)
        espcoredump
        esp_partition
        # esp_tinyusb — USB MSC (via component manager, ver idf_component.yml)
        esp_tinyusb
        # mdns — "kartbox.local" (via component manager, ver idf_component.yml)
        mdns
)

# GCC e paranoico com snprintf("%s/%s", dir, de->d_name) — d_name em teoria
# pode ter ate 255 bytes (FAT), entao ele sempre acha que pode truncar,
# mesmo com buffer de sobra pro caso real. Warning vira erro por causa do
# -Werror=all do projeto; deixamos so como warning aqui.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
