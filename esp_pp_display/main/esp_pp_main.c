#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_console.h"
#include "app_coredump.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_pp_client.h"
#include "app_pp_state.h"
#include "app_prefs.h"
#include "app_storage.h"
#include "app_time.h"
#include "app_ui.h"
#include "app_wifi.h"

// Backlight is on GPIO 31 (CrowPanel Advanced 10.1"). Active-high LEDC;
// see esp_ui_main.c for the why-low-during-boot rationale (stale framebuffer
// on soft reset). Forcing the line low at the very top keeps the panel
// dark until app_display_init takes over.
#define BL_GPIO 31

static const char *TAG = "esp_pp";

void app_main(void)
{
    gpio_reset_pin(BL_GPIO);
    gpio_set_direction(BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BL_GPIO, 0);

    ESP_LOGI(TAG, "ProPresenter stage display — boot");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_config_init();
    app_pp_state_init();

    // REPL first — same rationale as esp_ui: a panic during wifi/SD/display
    // bring-up still leaves a recoverable path via `coredump-b64` over UART.
    app_console_init();

    // Phase 1 of WiFi: bring up the radio transport (esp_wifi_init triggers
    // ESP-Hosted's SDIO bus to the C6 and sdmmc_host_init() runs once).
    // Required before SD mount because IDF v6's SDMMC host can only be
    // init'd once and ESP-Hosted is the canonical owner.
    app_wifi_init_radio();

    // Persist any pending panic dump to SD before doing anything that could
    // itself crash. If the card is missing, the dump stays in flash and
    // `coredump-b64` can still stream it.
    app_storage_init();
    app_coredump_flush_to_sd();
    app_logd_init();

    // Local UI preferences. Loads /sdcard/monpp-prefs.json or starts at
    // defaults if SD missing.
    app_prefs_init();
    app_time_apply_tz();

    if (!app_display_init()) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    app_ui_init();
    app_ui_set_status("Connecting WiFi...");

    // Backlight stayed at 0 through display+UI bring-up so the user never
    // sees the uninitialised framebuffer. Splash is up by now — ramp to
    // the saved pref.
    app_display_set_backlight_pct(app_prefs_get_brightness_pct());

    app_wifi_apply_ip_config();

    // Spawn the PP client — its connect attempts will fail until WiFi is
    // up, but it backs off cleanly and reconnects when the network arrives.
    // No need to gate on app_wifi_wait_connected.
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    pp->start();

    // Phase 2 of WiFi: block until associated or retries exhausted.
    if (!app_wifi_wait_connected()) {
        ESP_LOGW(TAG, "WiFi unavailable; UI will still render");
        app_ui_set_status("WiFi unavailable");
    } else {
        char ip[16];
        app_wifi_format_ip(ip, sizeof(ip));
        char status[64];
        snprintf(status, sizeof(status), "WiFi connected: %s", ip);
        app_ui_set_status(status);
        ESP_LOGI(TAG, "WiFi ip=%s", ip);
    }

    // SNTP comes up later from app_time once we wire it. ProPresenter
    // client is now running; the splash will be replaced by the stage UI
    // in Phase C.
}
