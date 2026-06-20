idf_component_register(
    SRCS
        "main.c"
        "ui.c"
        "app_events.c"
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
        "font_kartbox_xl.c"     # 36 px — delta
        "font_kartbox_lg.c"     # 22 px — valores de célula
        "font_kartbox_sm.c"     # 12 px — labels e status bar
    INCLUDE_DIRS "."
    REQUIRES
        # Componentes built-in do ESP-IDF
        freertos
        driver
        esp_timer
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_vfs_fat
        sdmmc
        esp_ldo_regulator
        bt
        # esp_tinyusb — USB MSC (built-in no ESP-IDF 5.x)
        tinyusb
)
