#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "app_console.h"
#include "app_coredump.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_ms_info.h"
#include "app_prefs.h"
#include "app_config.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_touch_inject.h"
#include "app_ui.h"
#include "app_wifi.h"

static const char *TAG = "esp_ui";

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

    // Channel discovery — done HERE, BEFORE ms->start(), so the fader UI
    // builds exactly once with the final channel list and broadcasts arrive
    // against an already-built UI. An earlier attempt deferred this to a
    // worker that fired after the WS connected; rebuilding the tileview
    // while subscription echoes streamed in starved CPU 0's idle and
    // tripped the task watchdog.
    //
    // /console/information is a plain HTTP GET on the same port as the WS,
    // so we can call it as soon as WiFi has an IP. If the fetch fails (MS
    // unreachable, bad response, etc.) we keep the NVS-seeded fallback list.
    app_ms_info_t info;
    bool info_ok = false;
    if (app_wifi_get_state() == APP_WIFI_STATE_CONNECTED &&
        app_ms_info_fetch(app_config_ms_host(), app_config_ms_port(), &info) &&
        info.input_count > 0) {
        info_ok = true;
        int ids[APP_CONFIG_MAX_CHANNELS];
        // Per-channel arrays now live in PSRAM (#42), so we can safely
        // expand. Si Expression has 24 inputs but the typical monitor mix
        // is 8-16 channels; cap discovery at 16 to keep the boot UI build
        // time bounded. Channel selection (#33) lets the user pick which
        // 16 (or fewer) of the available inputs to track.
        const int safe_max = 16;
        int  n = 0;
        for (int i = 0; i < info.input_count && i < safe_max; ++i) {
            ids[n++] = info.input_offset + i;
        }
        app_state_init(ids, (size_t) n);
        ESP_LOGI(TAG, "discovery: %d MS inputs available, using first %d",
                 info.input_count, n);
    } else {
        ESP_LOGW(TAG, "discovery: /console/information unavailable, using fallback");
    }

    // Hand the mix count to the UI so the selector populates with the
    // right number of buttons. 0 = no MS info → selector stays hidden.
    app_ui_set_mix_count(info_ok ? info.mix_count : 0);
    // Hand the input-channel range to the UI so the channel picker can
    // render a row per available input.
    if (info_ok) app_ui_set_input_range(info.input_offset, info.input_count);

    // Tell the WS client where the mix strips live in the dotted-path
    // namespace so it can subscribe to their scribble-strip names. Names
    // come back via the same broadcast path as channel names; the picker
    // popup uses them in place of "Mix N" placeholders.
    if (info_ok && ms->set_mix_layout) {
        ms->set_mix_layout(info.mix_offset, info.mix_count);
    }

    // Build the fader UI now, BEFORE ms->start. Must happen here so the
    // tileview is fully constructed by the time the WS subscriptions echo
    // initial values back; rebuilding the UI under live broadcast traffic
    // hangs the LVGL task.
    app_ui_present_channels();

    // ws_start always returns true when the client object initializes; the
    // websocket subsystem itself handles reconnect once WiFi is up.
    ms->start();
}
