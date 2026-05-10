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

// Nominal real-world durations. All are run through pwr_scale at use
// time so tests can shorten them without per-test patching.
#define DEFAULT_TIMEOUT_MS    (60u * 60u * 1000u)   // 1 hour
#define WARN_MS               (30u * 1000u)         // 30 s
#define DEGRADED_CAP_MS       (60u * 1000u)         // 60 s
#define WAKE_MENU_TIMEOUT_MS  (30u * 1000u)         // 30 s

// Periodic tick. 100 ms is fine-grained enough that the warning
// countdown updates smoothly (visible 1 Hz drop) without dominating
// CPU 0 with timer overhead.
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

// Currently selected user timeout (real ms, pre-scale). Starts at the
// 1 h default; the wake menu's pick replaces it.
static uint32_t s_user_timeout_ms = DEFAULT_TIMEOUT_MS;

static app_power_phase_t s_phase = APP_POWER_PHASE_AWAKE;
static lv_timer_t       *s_tick_timer;
static uint32_t          s_phase_entered_at_ms;
// Absolute lv_tick at which the current AWAKE period began. The user
// picked a duration; this stamp + that duration is the wall-clock-like
// deadline. Touches during AWAKE do NOT reset this (the user's intent
// was "stay awake for X, period"); only an explicit duration pick from
// the wake menu (via warn-tap or post-sleep) restarts it. Re-stamped
// also on transition INTO a degraded state so the cap window starts
// fresh from the link drop.
static uint32_t          s_awake_started_at_ms;
static bool              s_was_degraded;

// Saved brightness pref so unblanking restores the user's last
// chosen value.
static uint8_t           s_saved_brightness_pct;

// Tracks whether enter_sleep stopped the MS client, so enter_awake
// knows to bring it back up. The boot path leaves this false because
// app_main starts MS itself; only the sleep-induced stop sets it.
static bool              s_ms_stopped_for_sleep;

// Lazily built overlays. All live on the active screen; their
// foreground ordering is enforced by lv_obj_move_foreground at each
// phase entry.
static lv_obj_t *s_warn_overlay;
static lv_obj_t *s_warn_count_label;
static lv_obj_t *s_blank_overlay;
static lv_obj_t *s_wake_menu;

static void enter_awake(void);
static void enter_warn(void);
static void enter_sleep(void);
static void enter_wake_menu(void);

// True when the system can't actually drive a mix -- no WiFi, MS WS
// not connected, or MS connected but physical console not attached.
// Treated as a cap on the effective timeout.
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
    uint32_t base = pwr_scale(s_user_timeout_ms);
    if (degraded_state()) {
        uint32_t cap = pwr_scale(DEGRADED_CAP_MS);
        if (cap < base) base = cap;
    }
    return base;
}

uint32_t app_power_get_user_timeout_ms(void)
{
    return s_user_timeout_ms;
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
    // Force the awake clock to restart from now. This is for callers
    // that explicitly want to grant a fresh duration -- it's the
    // equivalent of an in-app "I'm here" signal. Touches DON'T call
    // this; the absolute-from-wake design forbids touch-resets.
    // Tests use it to reset between phases without driving the wake
    // menu.
    s_awake_started_at_ms = lv_tick_get();
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
    lv_label_set_text(title, "Display sleeping soon");
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
    // Make the overlay receive touch (so a tap on a blank screen
    // wakes the device rather than punching through to a fader).
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    s_blank_overlay = ov;
}

typedef struct {
    int          hours;
    const char  *label;
} wake_choice_t;

static const wake_choice_t s_choices[] = {
    {  1, "1h"  },
    {  2, "2h"  },
    {  4, "4h"  },
    {  8, "8h"  },
    { 12, "12h" },
    { 24, "24h" },
};

static void on_wake_menu_pick(lv_event_t *e)
{
    int hours = (int)(intptr_t) lv_event_get_user_data(e);
    if (hours <= 0) hours = 1;
    if (hours > 24) hours = 24;     // hard cap
    s_user_timeout_ms = (uint32_t) hours * 3600u * 1000u;
    ESP_LOGI(TAG, "wake-menu pick: %d h", hours);
    enter_awake();
}

// "Sleep" cancel option in the wake menu -- when the panel woke
// accidentally (touched in a bag, brushed by a guitar strap during
// teardown) the user wants a one-tap return to dark, not a 30 s
// auto-revert wait. Routes back to enter_sleep, which re-mounts the
// blank overlay and drops the backlight.
static void on_wake_menu_sleep(lv_event_t *e)
{
    (void) e;
    ESP_LOGI(TAG, "wake-menu cancel: returning to sleep");
    enter_sleep();
}

static void build_wake_menu(void)
{
    if (s_wake_menu) return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *ov  = lv_obj_create(scr);
    // Panel grew from 280 to 360 to fit the Sleep cancel row at the bottom.
    // 6 duration buttons (3 x 2) end at y=196 inside; the cancel sits at
    // y=216 with 50 px height -> y=266. With pad 20 and outer height 360,
    // usable inner is 320 so 266 leaves comfortable room.
    lv_obj_set_size(ov, 540, 360);
    lv_obj_center(ov);
    lv_obj_set_style_radius(ov, 12, 0);
    lv_obj_set_style_border_width(ov, 2, 0);
    lv_obj_set_style_pad_all(ov, 20, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    s_wake_menu = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Stay awake for:");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    const int btn_w = 140;
    const int btn_h = 70;
    const int gap   = 16;
    const int cols  = 3;
    const int n     = sizeof(s_choices) / sizeof(s_choices[0]);
    for (int i = 0; i < n; ++i) {
        int row = i / cols;
        int col = i % cols;
        int x = col * (btn_w + gap) - ((cols - 1) * (btn_w + gap)) / 2;
        int y = 40 + row * (btn_h + gap);
        lv_obj_t *btn = lv_button_create(ov);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, x, y);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_choices[i].label);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, on_wake_menu_pick, LV_EVENT_CLICKED,
                            (void *)(intptr_t) s_choices[i].hours);
        // Note: tag the user_data with the hour count, NOT a button
        // index, so the picker doesn't break if rows/cols ever change.
    }

    // Sleep cancel row at the bottom, full-width minus the panel pad.
    // Distinct dark-grey background so it doesn't look like another
    // duration option, and the LV_SYMBOL_POWER glyph mirrors the
    // top-bar sleep button so the user reads "this puts it back to
    // sleep" at a glance.
    int cancel_y = 40 + ((n + cols - 1) / cols) * (btn_h + gap);
    lv_obj_t *cancel_btn = lv_button_create(ov);
    lv_obj_set_size(cancel_btn, 460, 50);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_MID, 0, cancel_y);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x303030), 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_POWER " Sleep");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, on_wake_menu_sleep, LV_EVENT_CLICKED, NULL);
}

// ─────────────────────────────────────────────────────────────────
// Phase transitions
// ─────────────────────────────────────────────────────────────────

static void enter_awake(void)
{
    s_phase = APP_POWER_PHASE_AWAKE;
    s_phase_entered_at_ms = lv_tick_get();
    // The awake clock starts at every fresh AWAKE entry: boot, post-
    // sleep wake-menu pick, post-warning wake-menu pick. That's the
    // ONLY way the user-picked duration restarts -- in-AWAKE touches
    // do nothing, so "stay awake for 4 h" really means 4 h from this
    // pick.
    s_awake_started_at_ms = s_phase_entered_at_ms;
    if (s_warn_overlay)  lv_obj_add_flag(s_warn_overlay,  LV_OBJ_FLAG_HIDDEN);
    if (s_blank_overlay) lv_obj_add_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_wake_menu)     lv_obj_add_flag(s_wake_menu,     LV_OBJ_FLAG_HIDDEN);
    // Restore backlight from saved pref. Defensive 0-clamp -- if a
    // boot-stage pref read returned 0 (shouldn't happen post-init)
    // we keep the panel readable.
    uint8_t pct = s_saved_brightness_pct;
    if (pct < 5) pct = app_prefs_get_brightness_pct();
    app_display_set_backlight_pct(pct);

    // Bring MS back if sleep stopped it. Drop the lvgl_port_lock
    // around start so the new worker's on_connect callbacks can
    // grab it without us holding -- same pattern chpick uses for
    // its stop/start cycle.
    if (s_ms_stopped_for_sleep && s_ms && s_ms->start) {
        ESP_LOGI(TAG, "wake: restarting MS client");
        lvgl_port_unlock();
        s_ms->start();
        lvgl_port_lock(0);
        s_ms_stopped_for_sleep = false;
    }
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

static void enter_sleep(void)
{
    s_phase = APP_POWER_PHASE_SLEEP;
    s_phase_entered_at_ms = lv_tick_get();
    if (s_warn_overlay) lv_obj_add_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_wake_menu)    lv_obj_add_flag(s_wake_menu,    LV_OBJ_FLAG_HIDDEN);
    build_blank_overlay();
    lv_obj_remove_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_blank_overlay);
    s_saved_brightness_pct = app_prefs_get_brightness_pct();
    // Real off, not the 5 % floor -- a visibly-lit panel overnight
    // defeats the point of "sleep". The opaque overlay still
    // captures touches so a tap wakes through to the wake menu; the
    // user sees a dark panel until then. The pct-setter's floor
    // stays in place for the user-facing surfaces (slider,
    // set-bright); only this code path takes the LED all the way
    // down.
    app_display_set_backlight_off();
    ESP_LOGI(TAG, "entering sleep (effective_timeout=%ums)",
             (unsigned) app_power_get_effective_timeout_ms());

    // Gracefully release the MS connection: send /console/data/
    // unsubscribe for every subscription, then close the WS with a
    // proper 1000 NORMAL frame, then join the worker. Saves CPU on
    // both ends (no broadcasts to a panel that can't show them) and
    // keeps MS from holding zombie subscriptions overnight. enter_
    // awake spawns the worker again on the wake-menu pick.
    //
    // Drop the LVGL lock around shutdown_graceful + stop -- the
    // worker may be blocked waiting for the lock to deliver a
    // broadcast through on_state_change, and holding it here would
    // deadlock the join. Same pattern chpick_apply_async uses.
    if (s_ms && !s_ms_stopped_for_sleep) {
        lvgl_port_unlock();
        if (s_ms->shutdown_graceful) s_ms->shutdown_graceful();
        if (s_ms->stop)              s_ms->stop();
        lvgl_port_lock(0);
        s_ms_stopped_for_sleep = true;
    }
}

static void enter_wake_menu(void)
{
    s_phase = APP_POWER_PHASE_WAKE_MENU;
    s_phase_entered_at_ms = lv_tick_get();
    if (s_blank_overlay) lv_obj_add_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    build_wake_menu();
    lv_obj_remove_flag(s_wake_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wake_menu);
    // Re-enable backlight so the menu is actually visible.
    uint8_t pct = s_saved_brightness_pct;
    if (pct < 5) pct = app_prefs_get_brightness_pct();
    app_display_set_backlight_pct(pct);
    lv_display_trigger_activity(NULL);
}

void app_power_force_sleep(void)
{
    enter_sleep();
}

void app_power_set_user_timeout_ms(uint32_t ms)
{
    if (ms == 0) ms = DEFAULT_TIMEOUT_MS;
    s_user_timeout_ms = ms;
    // Semantically equivalent to a wake-menu pick: the user has chosen
    // a duration. If we're currently in the boot wake-menu (or post-
    // sleep wake-menu), commit the choice and transition to AWAKE so
    // the panel goes live. Tests use this hook to dismiss the boot
    // menu without faking a tap on a duration button.
    if (s_phase == APP_POWER_PHASE_WAKE_MENU) {
        enter_awake();
    }
}

// ─────────────────────────────────────────────────────────────────
// Tick handler — runs on the LVGL task via lv_timer.
// ─────────────────────────────────────────────────────────────────

static void tick_cb(lv_timer_t *t)
{
    (void) t;
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    uint32_t now      = lv_tick_get();

    // Watch degraded transitions. Reset s_awake_started_at_ms on
    // either edge so the absolute timer gets a fresh window after
    // a state change. The degraded path uses inactive (last-touch)
    // not the awake-clock, so this reset is mostly for the going-
    // healthy edge: a long degraded run (with touches keeping it
    // alive) shouldn't immediately fire the absolute timeout on
    // recovery. We don't trigger LVGL activity here -- doing so
    // would corrupt the SLEEP-phase tap-to-wake detection (which
    // also reads last_touch_ms via inactive).
    bool deg = degraded_state();
    if (deg != s_was_degraded) {
        s_was_degraded = deg;
        s_awake_started_at_ms = now;
        // If we were already in WARNING due to a long idle and the
        // connection just came back, demote to AWAKE so the user
        // doesn't see a stale countdown.
        if (s_phase == APP_POWER_PHASE_WARNING && !deg) {
            enter_awake();
            return;
        }
    }

    uint32_t timeout    = app_power_get_effective_timeout_ms();
    uint32_t warn_ms    = pwr_scale(WARN_MS);
    uint32_t warn_start = (timeout > warn_ms) ? (timeout - warn_ms) : 0;
    // Healthy: absolute from awake-start (the user-picked duration is a
    // wall-clock contract). Degraded: relative to last touch -- while
    // the user is actively interacting with the WiFi/MS config panels
    // trying to recover the link, the screen must not blank under them.
    uint32_t elapsed    = deg ? inactive : (now - s_awake_started_at_ms);
    // last_touch_ms is the absolute lv_tick of the last input event.
    // On no recent input it grows linearly with `now` (and stays
    // anchored to the previous touch); on touch it jumps to ~now.
    // Comparing to s_phase_entered_at_ms tells us "did the user touch
    // *since* the current phase began?" -- the cancel-on-warn path
    // uses this to avoid mistaking a pre-warn touch for a warn-dismiss.
    uint32_t last_touch_ms = (now > inactive) ? (now - inactive) : 0;

    switch (s_phase) {
        case APP_POWER_PHASE_AWAKE:
            // Healthy: absolute from awake-start; touches do nothing.
            // Degraded: relative to last touch (computed above).
            if (elapsed >= timeout) {
                enter_sleep();
            } else if (elapsed >= warn_start) {
                enter_warn();
            }
            break;

        case APP_POWER_PHASE_WARNING:
            // Touch during warning. In healthy mode -> wake menu (the
            // user picks a fresh absolute duration). In degraded mode
            // -> just demote to AWAKE: the cap is 60 s either way and
            // the menu choice (1h/2h/etc.) doesn't apply, the touch is
            // simply "I'm here, keep the panel lit a bit longer".
            if (last_touch_ms > s_phase_entered_at_ms) {
                if (deg) {
                    enter_awake();
                    return;
                }
                enter_wake_menu();
            } else if (elapsed >= timeout) {
                enter_sleep();
            } else if (s_warn_count_label) {
                uint32_t remaining = (elapsed < timeout) ? (timeout - elapsed) : 0;
                char buf[32];
                snprintf(buf, sizeof(buf), "Sleeping in %us",
                         (unsigned)((remaining + 999) / 1000));
                lv_label_set_text(s_warn_count_label, buf);
            }
            break;

        case APP_POWER_PHASE_SLEEP:
            // A tap on the blank overlay -> wake menu. last_touch_ms
            // > s_phase_entered_at_ms tells us the touch happened
            // after sleep entry (vs being a leftover from the awake
            // session that put us here).
            if (last_touch_ms > s_phase_entered_at_ms) {
                enter_wake_menu();
            }
            break;

        case APP_POWER_PHASE_WAKE_MENU:
            // Auto-return to sleep if the user doesn't pick a duration
            // within the wake-menu window, so an accidental wake
            // doesn't strand the panel running.
            {
                uint32_t in_phase = now - s_phase_entered_at_ms;
                if (in_phase >= pwr_scale(WAKE_MENU_TIMEOUT_MS)) {
                    enter_sleep();
                }
            }
            break;
    }
}

// ─────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────

static void on_wifi_or_ms_change(void *ctx)
{
    (void) ctx;
    // No-op for now. tick_cb watches degraded transitions on every
    // 100 ms tick, which is fast enough that the reset arrives well
    // before any user-visible blank. Keeping the observer registered
    // (rather than removing the wiring) leaves room for an
    // event-driven path here later without changing the public API.
}

void app_power_init(const ms_client_iface_t *ms)
{
    s_ms = ms;
    s_was_degraded = degraded_state();
    s_saved_brightness_pct = app_prefs_get_brightness_pct();
    // Stamp the awake clock at boot so the first warning fires after
    // (default_timeout - warn_ms) since boot, not since the LVGL tick
    // started. The user's first interaction with the device is right
    // after boot, so anchoring to boot is the right "wake" event.
    s_phase_entered_at_ms = lv_tick_get();
    s_awake_started_at_ms = s_phase_entered_at_ms;
    // Pin LVGL's inactivity baseline so the degraded-relative path
    // sees inactive=0 at boot. Otherwise, booting straight into a
    // degraded state could blank the panel before the user gets a
    // chance to interact (lv_display_get_inactive_time returns time
    // since the last activity tick, which without any touches is
    // however long boot took).
    lv_display_trigger_activity(NULL);

    // Build overlays up front so the first phase transition doesn't
    // block on widget creation while LVGL is mid-render. Hidden by
    // default; phase entries flip the visibility flag.
    build_warn_overlay();
    build_blank_overlay();
    build_wake_menu();

    // Drive the inactivity sweep at TICK_MS. The timer runs on the
    // LVGL task itself (no async hop needed) so phase transitions
    // can mutate widgets directly.
    s_tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    // app_wifi_register_on_change is statically linked from app_wifi.c
    // (the address is always non-NULL; GCC 12+ flags an `if(addr)` as
    // an error). The ms-client function pointer is dispatched through
    // a struct so the NULL guard there is real.
    app_wifi_register_on_change(on_wifi_or_ms_change, NULL);
    if (s_ms && s_ms->register_on_change) {
        s_ms->register_on_change(on_wifi_or_ms_change, NULL);
    }
    ESP_LOGI(TAG, "init: default_timeout=%ums scale=%u/%u",
             (unsigned) DEFAULT_TIMEOUT_MS,
             (unsigned) s_scale_num, (unsigned) s_scale_den);

    // Surface the wake menu on every boot so the user explicitly picks
    // a "stay awake for X" duration up front -- there's no boot-time
    // persistence (user explicitly opted out), and the silent 1 h
    // default surprised the pilot tester. The auto-revert in tick_cb
    // routes back to SLEEP if the user doesn't pick within
    // WAKE_MENU_TIMEOUT_MS, which means a powered-on-and-left device
    // doesn't sit lit forever. Test path: app_power_set_user_timeout_ms
    // commits a duration the same way a button tap would.
    enter_wake_menu();
}
