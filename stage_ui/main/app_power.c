#include "app_power.h"

#include "app_display.h"
#include "app_prefs.h"
#include "app_wifi.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define WARN_MS               (30u * 1000u)
#define DEGRADED_CAP_MS       (60u * 1000u)
#define TICK_MS               100

static uint32_t s_scale_num = 1;
static uint32_t s_scale_den = 1;

static uint32_t pwr_scale(uint32_t nominal_ms) {
    if (s_scale_num == s_scale_den) return nominal_ms;
    uint64_t v = (uint64_t)nominal_ms * s_scale_num / s_scale_den;
    return (v > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)v;
}

static const pp_client_iface_t *s_pp;

static app_power_phase_t s_phase = APP_POWER_PHASE_AWAKE;
static lv_timer_t       *s_tick_timer;
static uint32_t          s_phase_entered_at_ms;
static uint32_t          s_awake_started_at_ms;
static bool              s_was_degraded;
static uint8_t           s_saved_brightness_pct = 80;

// Lazily built overlays.
static lv_obj_t *s_warn_overlay;
static lv_obj_t *s_warn_count_label;
static lv_obj_t *s_blank_overlay;

static void enter_awake (void);
static void enter_warn  (void);
static void enter_sleep (void);

// ───────────────────────────────────────────────────────────────────────

static bool degraded_state(void) {
    if (app_wifi_get_state() != APP_WIFI_STATE_CONNECTED) return true;
    if (!s_pp || !s_pp->get_state) return true;
    if (s_pp->get_state() != APP_PP_CONN_CONNECTED)        return true;
    return false;
}

uint32_t app_power_get_effective_timeout_ms(void) {
    uint32_t base = pwr_scale(app_prefs_get_sleep_timeout_sec() * 1000u);
    if (degraded_state()) {
        uint32_t cap = pwr_scale(DEGRADED_CAP_MS);
        if (cap < base) base = cap;
    }
    return base;
}

app_power_phase_t app_power_get_phase(void) { return s_phase; }

void app_power_set_time_scale(uint32_t num, uint32_t den) {
    if (den == 0) return;
    s_scale_num = num;
    s_scale_den = den;
}

void app_power_kick(void) {
    s_awake_started_at_ms = lv_tick_get();
    lv_display_trigger_activity(NULL);
}

// ─── Overlays ──────────────────────────────────────────────────────────

static void on_warn_clicked(lv_event_t *e) {
    (void)e;
    // Tap during warning -> stay awake (reset clock).
    enter_awake();
}

static void on_blank_clicked(lv_event_t *e) {
    (void)e;
    // Tap during sleep -> wake.
    enter_awake();
}

static void build_warn_overlay(void) {
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
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ov, on_warn_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Display sleeping soon");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *cnt = lv_label_create(ov);
    lv_obj_set_style_text_font(cnt, &lv_font_montserrat_28, 0);
    lv_label_set_text(cnt, "Sleeping in 30s");
    lv_obj_align(cnt, LV_ALIGN_CENTER, 0, 0);
    s_warn_count_label = cnt;

    lv_obj_t *hint = lv_label_create(ov);
    lv_label_set_text(hint, "Tap anywhere to keep awake");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_warn_overlay = ov;
}

static void build_blank_overlay(void) {
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
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ov, on_blank_clicked, LV_EVENT_CLICKED, NULL);
    s_blank_overlay = ov;
}

// ─── Phase transitions ────────────────────────────────────────────────

static void enter_awake(void) {
    build_warn_overlay();
    build_blank_overlay();
    if (s_warn_overlay)  lv_obj_add_flag(s_warn_overlay,  LV_OBJ_FLAG_HIDDEN);
    if (s_blank_overlay) lv_obj_add_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);

    if (s_phase == APP_POWER_PHASE_SLEEP || s_phase == APP_POWER_PHASE_WARNING) {
        // Coming out of WARN/SLEEP -- restore the saved brightness.
        app_display_set_backlight_pct(s_saved_brightness_pct);
    }
    s_phase                = APP_POWER_PHASE_AWAKE;
    s_phase_entered_at_ms  = lv_tick_get();
    s_awake_started_at_ms  = s_phase_entered_at_ms;
    s_was_degraded         = degraded_state();
    lv_display_trigger_activity(NULL);
}

static void enter_warn(void) {
    build_warn_overlay();
    lv_obj_clear_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_warn_overlay);
    s_phase = APP_POWER_PHASE_WARNING;
    s_phase_entered_at_ms = lv_tick_get();
}

static void enter_sleep(void) {
    build_blank_overlay();
    if (s_warn_overlay)  lv_obj_add_flag(s_warn_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_blank_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_blank_overlay);

    // Save current brightness for restore on wake. Read from prefs so a
    // user change while in sleep is honoured.
    s_saved_brightness_pct = app_prefs_get_brightness_pct();
    app_display_set_backlight_off();

    s_phase = APP_POWER_PHASE_SLEEP;
    s_phase_entered_at_ms = lv_tick_get();
}

void app_power_force_sleep(void) { enter_sleep(); }

// ─── Tick ─────────────────────────────────────────────────────────────

static void tick(lv_timer_t *t) {
    (void)t;

    bool deg = degraded_state();
    if (deg != s_was_degraded) {
        s_was_degraded = deg;
        // Re-stamp the awake clock on a degraded-state transition so the
        // 60s cap window starts fresh from the link drop / restore.
        s_awake_started_at_ms = lv_tick_get();
    }

    uint32_t now = lv_tick_get();

    switch (s_phase) {
    case APP_POWER_PHASE_AWAKE: {
        uint32_t since_act = lv_display_get_inactive_time(NULL);
        uint32_t since_wake = now - s_awake_started_at_ms;
        // Healthy use: timer is absolute from wake. Degraded: timer is
        // relative to last touch so user typing into the WiFi panel
        // doesn't get blanked under them.
        uint32_t elapsed = deg ? since_act : since_wake;
        uint32_t budget  = app_power_get_effective_timeout_ms();
        uint32_t warn_ms = pwr_scale(WARN_MS);
        if (budget > warn_ms && elapsed >= budget - warn_ms) {
            enter_warn();
        }
        break;
    }
    case APP_POWER_PHASE_WARNING: {
        uint32_t elapsed = now - s_phase_entered_at_ms;
        uint32_t warn_ms = pwr_scale(WARN_MS);
        // Live countdown in the dialog.
        if (s_warn_count_label) {
            uint32_t remaining_ms = (elapsed < warn_ms) ? warn_ms - elapsed : 0;
            char buf[32];
            snprintf(buf, sizeof(buf), "Sleeping in %us", (unsigned)((remaining_ms + 999) / 1000));
            lv_label_set_text(s_warn_count_label, buf);
        }
        if (elapsed >= warn_ms) enter_sleep();
        break;
    }
    case APP_POWER_PHASE_SLEEP:
        // Wake handled by on_blank_clicked.
        break;
    }
}

// ─── Init ─────────────────────────────────────────────────────────────

static void on_wifi_change(void *ctx) { (void)ctx; /* tick picks it up */ }
static void on_pp_change  (void *ctx) { (void)ctx; /* tick picks it up */ }

void app_power_init(const pp_client_iface_t *pp) {
    s_pp = pp;
    s_saved_brightness_pct = app_prefs_get_brightness_pct();

    app_wifi_register_on_change(on_wifi_change, NULL);
    if (s_pp && s_pp->register_on_change) {
        s_pp->register_on_change(on_pp_change, NULL);
    }

    s_tick_timer = lv_timer_create(tick, TICK_MS, NULL);
    s_awake_started_at_ms = lv_tick_get();
    s_phase_entered_at_ms = s_awake_started_at_ms;
    s_was_degraded        = degraded_state();
}
