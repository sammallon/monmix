idf_component_register(
    SRCS
        "esp_ui_main.c"
        "app_config.c"
        "app_state.c"
        "app_wifi.c"
        "app_ms_client_ws.c"
        "app_ms_client_osc.c"
        "osc_expect.c"
        "app_ms_info.c"
        "app_display.c"
        "app_ui.c"
        "app_storage.c"
        "app_coredump.c"
        "app_console.c"
        "app_logd.c"
        "app_touch_inject.c"
        "app_prefs.c"
        "app_power.c"
        "app_time.c"
        "images/splash_logo.c"
        "fonts/font_monmix_level.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_lcd
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_sdmmc
        sdmmc
        fatfs
        esp_partition
        espcoredump
        console
        mbedtls
        esp_http_client
        esp_timer
        esp_rom
)
# Mongoose now does the WS pipe + offline-mixer attach POST. esp_http_client
# stays for app_ms_info.c's pre-WS /console/information probe (it runs
# before the WS task is up; quicker to keep one focused REST call than to
# block the WS task on the boot fetch).
