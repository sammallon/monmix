#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_console.h"
#include "app_coredump.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_prefs.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_touch_inject.h"
#include "app_ui.h"
#include "app_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp_ui";

// Discovery worker stubbed during boot-stability investigation. Restores
// the previous M3 behavior: builds the fader UI from the NVS-seeded list.
static void discovery_task(void *arg)
{
    (void) arg;
    app_ui_present_channels();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "stage monitor mix controller — boot");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_config_init();
    size_t      ch_count = 0;
    const int  *ch_ids   = app_config_channel_ids(&ch_count);
    app_state_init(ch_ids, ch_count);

    // First: bring up the UART REPL. It only depends on UART0 (live since
    // boot) and esp_partition (SPI flash, separate peripheral from SDMMC) —
    // none of the SDMMC-singleton constraints apply. Doing this first means
    // any subsequent panic during wifi/SD/display bring-up still leaves a
    // recoverable path: the next boot's console comes up almost immediately
    // and `coredump-b64` streams the flash partition out over UART.
    //
    // ESP-Hosted's diagnostic commands (`crash`, `reboot`, `mem-dump`, …)
    // get registered later, during esp_wifi_init() inside the radio bring-up
    // below. They land in the same global esp_console registry our REPL is
    // walking, so they appear retroactively without a restart.
    app_console_init();

    // Phase 1 of WiFi: bring up the radio transport (esp_wifi_init triggers
    // ESP-Hosted's SDIO bus to the C6 and sdmmc_host_init() runs once). This
    // returns immediately — it does NOT wait for an SSID. We need the host
    // controller alive before SD mount because IDF v6 only allows one host
    // init and ESP-Hosted is the canonical owner.
    app_wifi_init_radio();

    // Persist any pending panic dump to the SD card before we do anything
    // that could itself crash. If the card is missing, both calls log and
    // the dump stays in the flash partition for the next boot.
    app_storage_init();
    app_coredump_flush_to_sd();

    // Disk-log starts here — once SD is up, every subsequent step gets
    // its WiFi/WS/UI events recorded. No-op if SD didn't mount.
    app_logd_init();

    // Local UI preferences (level format, channel colors, signal indicator).
    // Loads /sdcard/monmix-prefs.json or starts at defaults if SD missing.
    app_prefs_init();

    // Display + UI come up next — the screen should light up before we
    // start waiting on WiFi association.
    if (!app_display_init()) {
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    // Virtual touch indev for the `touch` console command. Must come
    // after app_display_init brings up LVGL.
    app_touch_inject_init();

    const ms_client_iface_t *ms = app_ms_client_ws();
    app_ui_init(ms);
    app_ui_set_status("Connecting WiFi...");

    // Phase 2 of WiFi: block until associated or retries exhausted.
    if (!app_wifi_wait_connected()) {
        ESP_LOGW(TAG, "WiFi unavailable; UI will still render, MS client will retry");
        app_ui_set_status("WiFi unavailable — UI only");
    }

    // ws_start always returns true when the client object initializes; the
    // websocket subsystem itself handles reconnect once WiFi is up.
    ms->start();

    // Discovery worker: waits for the WS to come up, fetches channel
    // architecture from MS, reseeds app_state, and finally builds the
    // fader UI. Until then the user sees the loading spinner mounted by
    // app_ui_init.
    xTaskCreate(discovery_task, "ms_discovery", 16 * 1024,
                (void *) ms, 5, NULL);
}
