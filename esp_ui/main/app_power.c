#include "app_power.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_display.h"
#include "app_ms_client.h"
#include "app_prefs.h"
#include "app_wifi.h"

static const char *TAG = "app_power";

// Nominal real-world durations. Run through pwr_scale at use time so
// tests can shorten them without per-test patching.
#define DEGRADED_CAP_MS        (60u * 1000u)   // total degraded budget
#define WARN_MS                (30u * 1000u)   // pre-blank warning window
// How often to attempt ms->start() while asleep (only when auto-wake
// is armed). The MS client's internal connect-retry takes over once
// started, so this just needs to be frequent enough that the user
// doesn't wait noticeably after the rig comes back up.
#define MS_RESTART_PROBE_MS    (30u * 1000u)

// 100 ms tick gives smooth (visible 1 Hz) WARN-countdown updates
// without dominating CPU 0.
#define TICK_MS                100

static uint32_t s_scale_num = 1;
static uint32_t s_scale_den = 1;

static uint32_t pwr_scale(uint32_t nominal_ms)
{
    if (s_scale_num == s_scale_den) return nominal_ms;
    uint64_t v = (uint64_t) nominal_ms * s_scale_num / s_scale_den;
    if (v > (uint64_t) UINT32_MAX) return UINT32_MAX;
    return (uint32_t) v;
}

static const ms_client_iface_t *s_ms;

static app_power_phase_t s_phase = APP_POWER_PHASE_AWAKE;
static lv_timer_t       *s_tick_timer;
static uint32_t          s_phase_entered_at_ms;
static bool              s_was_degraded;

// Saved brightness pref so unblanking restores the user's last chosen
// value.
static uint8_t           s_saved_brightness_pct;

// Tracks whether enter_sleep stopped the MS client, so enter_awake
// (or the probe path) knows MS needs to be brought back up. Boot
// leaves this false because app_main starts MS itself; sleep paths
// set it.
static bool              s_ms_stopped_for_sleep;

// Auto-wake gate. True when the device should auto-wake on a degraded
// ─> healthy edge. Armed when sleep entry happened while degraded OR
// when degradation occurs mid-sleep; a manual sleep that happened
// healthy and stayed healthy does NOT auto-wake (the user said "go
// dark"). The auto-sleep path always arms because auto-sleep only
// fires while already degraded.
static bool              s_auto_wake_armed;
static uint32_t          s_last_ms_probe_ms;

// Lazily built overlays; live on the active screen; foreground
// ordering enforced by lv_obj_move_foreground at each phase entry.
static lv_obj_t *s_warn_overlay;
static lv_obj_t *s_warn_count_label;
static lv_obj_t *s_blank_overlay;

static void enter_awake(void);
static void enter_warn(void);
static void enter_sleep_internal(bool manual);

// True when the system can't actually drive a mix -- WiFi missing,
// MS WS not connected, or MS connected but physical console not
// attached.
static bool degraded_state(void)
{
    if (app_wifi_get_state() != APP_WIFI_STATE_CONNECTED) return true;
    if (!s_ms) return true;
    if (s_ms->get_state && s_ms->get_state() != APP_MS_STATE_CONNECTED) return true;
    if (s_ms->is_console_attached && !s_ms->is_console_attached()) return true;
    return false;
}

uint32_t app_power_get_effective_timeout_ms(void)
{
    // Healthy = no timeout (panel stays on); degraded = the 60 s cap.
    // Exposed for tests and surface UI to display "X s before sleep"
    // hints in degraded mode.
    if (!degraded_state()) return 0;
    return pwr_scale(DEGRADED_CAP_MS);
}

app_power_phase_t app_power_get_phase(void)
{
    return s_phase;
}

void app_power_set_time_scale(uint32_t numerator, uint32_t denominator)
{
    if (denominator == 0) return;
    s_scale_num = numerator;
    s_scale_den = denominator;
}

void app_power_kick(void)
{
    // Force the LVGL activity counter to reset (same effect as a
    // touch). Tests use it to clear inactivity between phases.
    lv_display_trigger_activity(NULL);
}

// ─────────────────────────────────────────────────────────────────
// Overlay builders
// ─────────────────────────────────────────────────────────────────

static void build_warn_overlay(void)
{
    if (s_warn_overlay) return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *ov  = lv_obj_create(scr);
    lv_obj_set_size(ov, 460, 200);
    lv_obj_center(ov);
    lv_obj_set_style_radius(ov, 12, 0);
    lv_obj_set_style_border_width(ov, 2, 0);
    lv_obj_set_style_border_color(ov, lv_color_hex(0xE0C040), 0);
    lv_obj_set_style_pad_all(ov, 20, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    s_warn_overlay = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Connection lost - sleeping soon");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cnt = lv_label_create(ov);
    lv_label_set_text(cnt, "Sleeping in 30s");
    lv_obj_align(cnt, LV_ALIGN_CENTER, 0, 0);
    s_warn_count_label = cnt;

    lv_obj_t *hint = lv_label_create(ov);
    lv_label_set_text(hint, "Tap anywhere to keep awake");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void build_blank_overlay(void)
{
    if (s_blank_overlay) return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *ov  = lv_obj_create(scr);
    lv_obj_set_size(ov, lv_obj_get_width(scr), lv_obj_get_height(scr));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    // Overlay catches taps so a tap on a blank screen wakes the
    // device rather than punching through to a fader underneath.
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    s_blank_overlay = ov;
}

// ─────────────────────────────────────────────────────────────────
// Phase transitions
// ─────────────────────────────────────────────────────────────────

static void enter_awake(void)
{
    s_phase = APP_POWER_PHASE_AWAKE;
    s_phase_entered_at_ms = lv_tick_get();
    s_auto_wake_armed = false;
    if (s_warn_overlay)  lv_obj_add_flag(s_warn_overlay,  LV_OBJ_FLAG_HIDDEN);
    if (s_blank_overlay) lv_obj_add_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    // Restore backlight from saved pref. Defensive 0-clamp -- a boot-
    // stage pref read of 0 (shouldn't happen post-init) would leave
    // the panel unreadable.
    uint8_t pct = s_saved_brightness_pct;
    if (pct < 5) pct = app_prefs_get_brightness_pct();
    app_display_set_backlight_pct(pct);

    // Bring MS back if sleep stopped it. Drop lvgl_port_lock around
    // start so the new worker's on_connect callbacks can grab it --
    // same pattern chpick_apply_async uses.
    if (s_ms_stopped_for_sleep && s_ms && s_ms->start) {
        ESP_LOGI(TAG, "wake: restarting MS client");
        lvgl_port_unlock();
        s_ms->start();
        lvgl_port_lock(0);
        s_ms_stopped_for_sleep = false;
    }
    // Reset LVGL inactivity so any post-wake "1 minute degraded
    // window" starts from now, not whenever the last touch was.
    lv_display_trigger_activity(NULL);
}

static void enter_warn(void)
{
    if (s_phase == APP_POWER_PHASE_WARNING) return;
    s_phase = APP_POWER_PHASE_WARNING;
    s_phase_entered_at_ms = lv_tick_get();
    build_warn_overlay();
    lv_obj_remove_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_warn_overlay);
    if (s_warn_count_label) {
        lv_label_set_text(s_warn_count_label, "Sleeping soon");
    }
}

static void enter_sleep_internal(bool manual)
{
    s_phase = APP_POWER_PHASE_SLEEP;
    s_phase_entered_at_ms = lv_tick_get();
    if (s_warn_overlay) lv_obj_add_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
    build_blank_overlay();
    lv_obj_remove_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_blank_overlay);
    s_saved_brightness_pct = app_prefs_get_brightness_pct();
    // Real off, not the 5 % floor. The opaque overlay still captures
    // touches so a tap wakes through; the LED path is fully dark.
    // The pct-setter's floor stays in place for user-facing surfaces
    // (slider, set-bright REPL); only this code path takes the LED
    // all the way down.
    app_display_set_backlight_off();

    // Auto-wake arming. The rule: if the device was degraded at sleep
    // entry (either auto-sleep, which only fires while degraded, OR
    // manual sleep while degraded), arm auto-wake. A manual sleep
    // while everything was healthy doesn't arm -- "go dark, stay
    // dark until I tap". tick_cb may arm later if degradation
    // happens mid-sleep.
    s_auto_wake_armed = degraded_state();
    s_last_ms_probe_ms = s_phase_entered_at_ms;

    ESP_LOGI(TAG, "entering sleep (manual=%d auto_wake_armed=%d)",
             (int) manual, (int) s_auto_wake_armed);

    // Graceful release of the MS connection. Same pattern as the
    // legacy 48ea146 commit: unsubscribe everything, send a 1000
    // NORMAL close, join the worker. Saves CPU + bandwidth (no
    // broadcasts to a panel that can't show them, no zombie subs
    // on MS overnight). Lock dropped around the join so the worker's
    // on_state_change handler can take lvgl_port_lock without
    // deadlocking us.
    if (s_ms && !s_ms_stopped_for_sleep) {
        lvgl_port_unlock();
        if (s_ms->shutdown_graceful) s_ms->shutdown_graceful();
        if (s_ms->stop)              s_ms->stop();
        lvgl_port_lock(0);
        s_ms_stopped_for_sleep = true;
    }
}

void app_power_force_sleep(void)
{
    enter_sleep_internal(true);
}

// ─────────────────────────────────────────────────────────────────
// Tick handler -- runs on the LVGL task via lv_timer.
// ─────────────────────────────────────────────────────────────────

static void tick_cb(lv_timer_t *t)
{
    (void) t;
    uint32_t now           = lv_tick_get();
    uint32_t inactive      = lv_display_get_inactive_time(NULL);
    uint32_t last_touch_ms = (now > inactive) ? (now - inactive) : 0;
    bool deg               = degraded_state();

    // Mid-sleep degradation arms the auto-wake gate so the eventual
    // recovery wakes the panel. Covers "user manually slept while
    // healthy, then situation deteriorated overnight, then came back"
    // -- which is exactly what the auto-wake-on-MS-reconnect feature
    // is supposed to cover.
    if (s_phase == APP_POWER_PHASE_SLEEP && deg && !s_auto_wake_armed) {
        s_auto_wake_armed = true;
    }

    switch (s_phase) {
        case APP_POWER_PHASE_AWAKE:
            if (!deg) {
                // Healthy: pin inactive=0 so the WARN/SLEEP paths
                // never fire while the rig is functioning.
                lv_display_trigger_activity(NULL);
            } else {
                // Degraded: 1-minute window from last touch. Split
                // into normal AWAKE + WARN countdown so the user
                // sees the countdown before the panel goes dark.
                uint32_t cap        = pwr_scale(DEGRADED_CAP_MS);
                uint32_t warn_ms    = pwr_scale(WARN_MS);
                uint32_t warn_start = (cap > warn_ms) ? (cap - warn_ms) : 0;
                if (inactive >= cap)        enter_sleep_internal(false);
                else if (inactive >= warn_start) enter_warn();
            }
            break;

        case APP_POWER_PHASE_WARNING:
            // Touch dismisses warning -> AWAKE. If health recovers
            // mid-warning, demote to AWAKE so the user doesn't see
            // a stale countdown.
            if (!deg) {
                enter_awake();
                break;
            }
            if (last_touch_ms > s_phase_entered_at_ms) {
                enter_awake();
                break;
            }
            {
                uint32_t cap     = pwr_scale(DEGRADED_CAP_MS);
                if (inactive >= cap) {
                    enter_sleep_internal(false);
                } else if (s_warn_count_label) {
                    uint32_t remaining = (inactive < cap) ? (cap - inactive) : 0;
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Sleeping in %us",
                             (unsigned)((remaining + 999) / 1000));
                    lv_label_set_text(s_warn_count_label, buf);
                }
            }
            break;

        case APP_POWER_PHASE_SLEEP:
            // Touch wakes (any sleep type, armed or not).
            if (last_touch_ms > s_phase_entered_at_ms) {
                enter_awake();
                break;
            }
            // Auto-wake on degraded -> healthy edge, only when armed.
            // The s_was_degraded latch is read here so we catch the
            // transition rather than a steady "deg=false" state
            // (which would also fire wake on every tick).
            if (s_auto_wake_armed && s_was_degraded && !deg) {
                ESP_LOGI(TAG, "auto-wake: MS connection became active");
                enter_awake();
                break;
            }
            // Auto-wake probe: restart MS periodically when armed so
            // the iface gets a chance to re-establish. The MS
            // client's internal reconnect-retry takes over once
            // started; the resulting CONNECTED + console_attached
            // transition flips degraded_state, and the edge detector
            // above wakes us on the next tick.
            if (s_auto_wake_armed && s_ms_stopped_for_sleep &&
                (now - s_last_ms_probe_ms) >= pwr_scale(MS_RESTART_PROBE_MS)) {
                s_last_ms_probe_ms = now;
                if (app_wifi_get_state() == APP_WIFI_STATE_CONNECTED &&
                    s_ms && s_ms->start) {
                    ESP_LOGI(TAG, "sleep probe: restarting MS");
                    lvgl_port_unlock();
                    s_ms->start();
                    lvgl_port_lock(0);
                    s_ms_stopped_for_sleep = false;
                }
            }
            break;
    }

    s_was_degraded = deg;
}

// ─────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────

static void on_wifi_or_ms_change(void *ctx)
{
    (void) ctx;
    // No-op for now. tick_cb watches degraded transitions on every
    // 100 ms tick which is fast enough that the wake arrives well
    // before any user-visible delay. Keeping the observer registered
    // (rather than removing the wiring) leaves room for an event-
    // driven path here later without changing the public API.
}

void app_power_init(const ms_client_iface_t *ms)
{
    s_ms = ms;
    s_was_degraded = degraded_state();
    s_saved_brightness_pct = app_prefs_get_brightness_pct();
    s_phase_entered_at_ms = lv_tick_get();
    // Pin LVGL's inactivity baseline so a degraded-at-boot state
    // (WiFi still associating) doesn't immediately count toward the
    // 60 s window. lv_display_get_inactive_time would otherwise
    // report "uptime since LVGL init" until the first touch.
    lv_display_trigger_activity(NULL);

    build_warn_overlay();
    build_blank_overlay();

    // Drive the inactivity sweep at TICK_MS. The timer runs on the
    // LVGL task itself (no async hop needed) so phase transitions
    // can mutate widgets directly.
    s_tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    app_wifi_register_on_change(on_wifi_or_ms_change, NULL);
    if (s_ms && s_ms->register_on_change) {
        s_ms->register_on_change(on_wifi_or_ms_change, NULL);
    }
    ESP_LOGI(TAG, "init: connectivity-driven sleep, degraded_cap=%ums scale=%u/%u",
             (unsigned) DEGRADED_CAP_MS,
             (unsigned) s_scale_num, (unsigned) s_scale_den);
    // Boot lands in AWAKE -- no duration prompt. Healthy state keeps
    // the panel lit; degraded state starts its 60 s window.
}
