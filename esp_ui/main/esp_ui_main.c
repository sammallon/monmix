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

    // Apply persisted brightness. init_backlight seeded with the compile-time
    // default (80%); now that prefs are loaded, switch to the user's choice.
    app_display_set_backlight_pct(app_prefs_get_brightness_pct());

    // Virtual touch indev for the `touch` console command. Must come
    // after app_display_init brings up LVGL.
    app_touch_inject_init();

    const ms_client_iface_t *ms = app_ms_client_ws();
    app_ui_init(ms);
    app_ui_set_status("Connecting WiFi...");

    // Apply DHCP / static-IP choice from prefs now that they're loaded. If
    // the WiFi driver already auto-started DHCP after init_radio, this
    // override stops DHCP and pushes the static IP -- the next GOT_IP event
    // reflects the configured address.
    app_wifi_apply_ip_config();

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
        info.total > 0) {
        info_ok = true;
        // The user's persisted channel selection (NVS via app_config) is
        // authoritative. Discovery is no longer used to overwrite that --
        // before the picker (#33) existed, the auto-pick-first-N here was
        // the only way to scale up to a board with more inputs than the
        // hard-coded default of 12, but now the user picks via the UI and
        // we'd be silently undoing their choice on every boot.
        ESP_LOGI(TAG, "discovery: total=%d on connected console", info.total);
    } else {
        ESP_LOGW(TAG, "discovery: /console/information unavailable");
    }

    // Hand the mix count to the UI so the selector populates with the
    // right number of buttons. 0 = no MS info → selector stays hidden.
    app_ui_set_mix_count(info_ok ? info.mix_count : 0);
    // Hand the total channel count to the UI so the picker can render a
    // row per available channel id (any type -- inputs, mixes, aux, etc).
    // User picks freely from the full set.
    if (info_ok) app_ui_set_channel_total(info.total);

    // Tell the WS client where the mix strips live in the dotted-path
    // namespace so it can subscribe to their scribble-strip names. Names
    // come back via the same broadcast path as channel names; the picker
    // popup uses them in place of "Mix N" placeholders.
    if (info_ok && ms->set_mix_layout) {
        ms->set_mix_layout(info.mix_offset, info.mix_count);
    }

    // P3: prime the all-strip-name cache so the channel picker overlay
    // shows MS-side names from first open. Blocking ~2.4 s on an 80-ch
    // Si Expression; runs once on connect, before ws_start, so the cost
    // hides inside the existing boot wait.
    if (info_ok && info.total > 0 && ms->fetch_all_strip_names) {
        ms->fetch_all_strip_names(info.total);
    }

    // P11: REST-fetch ch.<N>.info.isActive for every mix bus before the
    // saved-index validation below. The WS subscribe path keeps the mask
    // live afterwards; this synchronous prime is just so the boot path
    // can see un-routed mixes the user must be steered away from.
    if (info_ok && info.mix_count > 0 && ms->fetch_mix_routing) {
        ms->fetch_mix_routing();
    }

    // Restore the user's last selected mix bus. If the saved index is out
    // of range for the connected console (e.g. they previously paired with
    // a board that had more mixes), or it points at a now-unrouted mix in
    // MS (P11), fall back to the first routed mix and persist the fallback
    // so the next reboot sees the corrected value.
    if (info_ok && info.mix_count > 0) {
        uint8_t saved = app_prefs_get_selected_mix_index();
        bool out_of_range = saved >= info.mix_count;
        bool unrouted = !out_of_range && ms->is_mix_routed &&
                        !ms->is_mix_routed(saved);
        if (out_of_range || unrouted) {
            uint8_t fallback = 0;
            if (ms->is_mix_routed) {
                for (int i = 0; i < info.mix_count; ++i) {
                    if (ms->is_mix_routed(i)) { fallback = (uint8_t) i; break; }
                }
            }
            app_prefs_set_selected_mix_index(fallback);
            saved = fallback;
        }
        if (ms->set_mix) ms->set_mix(saved);
    }

    // Build the fader UI now, BEFORE ms->start. Must happen here so the
    // tileview is fully constructed by the time the WS subscriptions echo
    // initial values back; rebuilding the UI under live broadcast traffic
    // hangs the LVGL task.
    app_ui_present_channels();

    // ws_start always returns true when the client object initializes; the
    // websocket subsystem itself handles reconnect once WiFi is up.
    ms->start();

    // #30: if the user had the meter indicator on at last shutdown, prime
    // the WS client's "subscribed" flag so the connect handler subscribes
    // metering on first connect. set_meter_enabled is idempotent and the
    // actual /console/metering2/subscribe send is gated on the WS being
    // open, so calling it here before connect just sets the bookkeeping
    // bit and the on-connect handler does the real work.
    if (ms->set_meter_enabled) {
        bool want_meter = (app_prefs_get_signal_indicator() ==
                           APP_SIGNAL_INDICATOR_METER);
        ms->set_meter_enabled(want_meter);
    }
}
