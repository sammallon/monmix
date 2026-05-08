#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Backlight is on GPIO 31 (CrowPanel Advanced 10.1"). Active-high LEDC;
// the actual LEDC config happens inside app_display_init. Until then the
// GPIO is whatever the bootloader/ROM left it at -- on soft reset that
// can be the previously-active LEDC duty, which briefly displays the
// panel's stale framebuffer (pale blue) for the seconds between boot
// and app_display_init. Forcing the line low at the very top of
// app_main keeps the panel dark during the entire boot window.
#define BL_GPIO 31

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
#include "app_time.h"
#include "app_touch_inject.h"
#include "app_ui.h"
#include "app_wifi.h"

static const char *TAG = "esp_ui";

// True once try_apply_ms_info has successfully run end-to-end. Boot path
// calls it best-effort; if MS is unreachable then, the on-change callback
// retries on every MS state transition until it lands. Once true the
// retry is a cheap early return.
static bool s_ms_setup_done;

// Run the MS-info-dependent boot setup: fetch /console/information,
// publish mix count + channel total to the UI, prime the strip-name
// cache, probe channel routability, and restore the saved mix index.
// Idempotent via s_ms_setup_done so the on-change callback can retry
// freely while MS is unreachable. Returns silently if WiFi has no IP
// yet, MS hasn't attached to its console, or info_fetch fails. The
// console-attached gate matters because MS happily answers
// /console/information with default-shaped data even before the
// physical console finishes coming up after a power-cycle, and the
// names/totals/routability we'd cache from that pre-link response are
// all wrong. /app/state heartbeat is the truthful gate.
static void try_apply_ms_info(const ms_client_iface_t *ms)
{
    if (s_ms_setup_done) return;
    if (app_wifi_get_state() != APP_WIFI_STATE_CONNECTED) return;
    if (ms->is_console_attached && !ms->is_console_attached()) {
        ESP_LOGI(TAG, "discovery: waiting for console to attach to MS");
        return;
    }

    app_ms_info_t info;
    if (!app_ms_info_fetch(app_config_ms_host(), app_config_ms_port(), &info)
        || info.total <= 0) {
        ESP_LOGW(TAG, "discovery: /console/information unavailable");
        return;
    }
    ESP_LOGI(TAG, "discovery: total=%d on connected console", info.total);
    s_ms_setup_done = true;

    // The user's persisted channel selection (NVS via app_config) is
    // authoritative -- we don't overwrite it from /console/information,
    // we just publish the totals so the UI can populate its picker.
    app_ui_set_mix_count(info.mix_count);
    app_ui_set_channel_total(info.total);

    // Tell the WS client where the mix strips live so it can subscribe to
    // their cfg.name scribble strips. notify_subscribers fires from inside
    // set_mix_layout so the mix-indicator visibility gate flips on cue.
    if (ms->set_mix_layout) {
        ms->set_mix_layout(info.mix_offset, info.mix_count);
    }

    // P3: prime the all-strip-name cache so the channel picker overlay
    // shows MS-side names from first open. ~30 ms per GET on stage WiFi
    // -> ~2.4 s for an 80-channel Si Expression. Blocking; runs on the
    // caller's task (app_main on the boot path, ws-event task on the
    // deferred-CONNECTED retry path).
    if (info.total > 0 && ms->fetch_all_strip_names) {
        ms->fetch_all_strip_names(info.total);
    }

    // W6.1: probe each channel's routability so the picker hides
    // mix/matrix/main strips. Same blocking-sweep pattern.
    if (info.total > 0 && ms->fetch_channel_routability) {
        ms->fetch_channel_routability(info.total);
    }

    // P11: prime the routed-mix mask before the saved-index validation
    // below. WS subscribe keeps it live afterwards.
    if (info.mix_count > 0 && ms->fetch_mix_routing) {
        ms->fetch_mix_routing();
    }

    // Restore the user's last selected mix bus. If the saved index is out
    // of range for the connected console or points at a now-unrouted mix,
    // fall back to the first routed mix and persist the fallback.
    if (info.mix_count > 0) {
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
}

// Fires from the WS event task on every MS state transition AND from
// the heartbeat task when console_attached flips. Either side coming
// good is the trigger to retry the deferred boot setup. After
// s_ms_setup_done flips true the body is a cheap early return.
static void on_ms_state_change_setup(void *ctx)
{
    const ms_client_iface_t *ms = (const ms_client_iface_t *) ctx;
    if (s_ms_setup_done) return;
    if (!ms->get_state || ms->get_state() != APP_MS_STATE_CONNECTED) return;
    if (ms->is_console_attached && !ms->is_console_attached()) return;
    try_apply_ms_info(ms);
}

void app_main(void)
{
    // Drive backlight low immediately so a soft reset doesn't briefly
    // re-illuminate the panel with stale framebuffer content (pale blue)
    // while the rest of boot runs. app_display_init takes over LEDC
    // duty several seconds later.
    gpio_reset_pin(BL_GPIO);
    gpio_set_direction(BL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BL_GPIO, 0);

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

    // Apply the persisted display TZ to the C library before any UI clock
    // formatter runs. SNTP comes up later, after WiFi associates.
    app_time_apply_tz();

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

    // Backlight stayed at 0 through display+UI bring-up so the user
    // never sees the uninitialised framebuffer (bright blue on this
    // panel). Splash widget is up by now -- ramp to the saved pref.
    app_display_set_backlight_pct(app_prefs_get_brightness_pct());

    // Build the fader UI from the NVS-seeded channel list and dismiss the
    // boot splash NOW, before we wait on WiFi. Reason: at a new venue with
    // unknown SSIDs the user needs to reach the WiFi panel to enter creds,
    // and waiting ~20 retries * RETRY_BACKOFF_MS for FAILED would strand
    // them on the splash. The mix indicator + channel-picker totals stay
    // hidden until MS info arrives later; the fader widgets render against
    // the persisted channel selection regardless of WiFi state. Must still
    // happen BEFORE ms->start so subscription echoes never race UI build.
    app_ui_present_channels();

    // Apply DHCP / static-IP choice from prefs now that they're loaded. If
    // the WiFi driver already auto-started DHCP after init_radio, this
    // override stops DHCP and pushes the static IP -- the next GOT_IP event
    // reflects the configured address.
    app_wifi_apply_ip_config();

    // Phase 2 of WiFi: block until associated or retries exhausted. The
    // splash is already gone; the user sees the fader shell with a status
    // line and can open the WiFi panel mid-wait if they need to reconfigure.
    if (!app_wifi_wait_connected()) {
        ESP_LOGW(TAG, "WiFi unavailable; UI will still render, MS client will retry");
        app_ui_set_status("WiFi unavailable — UI only");
    }
    // SNTP comes up later from app_ui's on-IP-up path (start_clock_once),
    // which uses the ntp_server pref via app_prefs_get_ntp_server.

    // Best-effort: if MS+console are both up at boot, this lands the full
    // setup synchronously. Otherwise it returns silently and the registered
    // on-change callback retries on every MS state transition (and on every
    // /app/state heartbeat tick that flips console_attached).
    try_apply_ms_info(ms);

    // ws_start always returns true when the client object initializes; the
    // websocket subsystem itself handles reconnect once WiFi is up. Spawns
    // the heartbeat task too so console_attached starts ticking.
    ms->start();

    // Register the deferred-info handler. notify_subscribers fires on WS
    // state changes AND on console_attached transitions, so this single
    // hook covers both "MS came online" and "console got powered on".
    if (ms->register_on_change) {
        ms->register_on_change(on_ms_state_change_setup, (void *) ms);
    }

    // #30: if the user had the meter indicator on at last shutdown, prime
    // the WS client's "subscribed" flag so the connect handler subscribes
    // metering on first connect. set_meter_enabled is idempotent and the
    // actual /console/metering2/subscribe send is gated on the WS being
    // open, so calling it here before connect just sets the bookkeeping
    // bit and the on-connect handler does the real work.
    if (ms->set_meter_enabled) {
        // Subscribe metering for either PRESENT or METER mode (only NONE
        // skips it). PRESENT debounces the same meter_db stream into the
        // 1 s dot-fade behaviour; METER renders the bar continuously.
        app_signal_indicator_t mode = app_prefs_get_signal_indicator();
        bool want_meter = (mode == APP_SIGNAL_INDICATOR_METER ||
                           mode == APP_SIGNAL_INDICATOR_PRESENT);
        ms->set_meter_enabled(want_meter);
    }
}
