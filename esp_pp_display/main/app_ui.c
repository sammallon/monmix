#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app_pp_client.h"
#include "app_pp_state.h"
#include "app_wifi.h"

static const char *TAG = "app_ui";

// Default timeout for lvgl_port_lock when called from a non-LVGL task.
// Matches esp_ui's LVGL_LOCK_TIMEOUT_MS; 1 s is the empirical floor that
// gives LVGL room to finish any reasonable in-flight render. Try-and-
// fail (0 ms) silently drops state-change notifications when LVGL is
// rendering, which causes the UI to lag a beat behind reality.
#define LVGL_LOCK_TIMEOUT_MS 1000

// Layout — rows stack top-to-bottom. Heights add to 600 (LCD_V_RES).
#define ROW_HEADER_H  50
#define ROW_CURRENT_H 350
#define ROW_NEXT_H    130
#define ROW_FOOTER_H  70

// Held across the whole app's lifetime; observers reach into them under
// lvgl_port_lock. Pointer values are stable -- the widgets are never
// destroyed once built.
static lv_obj_t *s_header_row;
static lv_obj_t *s_wifi_label;
static lv_obj_t *s_pp_label;
static lv_obj_t *s_clock_label;

static lv_obj_t *s_current_label;
static lv_obj_t *s_next_label;
static lv_obj_t *s_footer_label;       // timer or boot-status fallback
static lv_obj_t *s_stage_msg_banner;   // bottom overlay, hidden when empty
static lv_obj_t *s_stage_msg_label;

static const app_pp_client_iface_t *s_pp;

// True once the first PP-driven update has rendered. Until then,
// app_ui_set_status drives the footer text (boot progress); after, the
// footer shows the timer strip and set_status is a no-op.
static bool s_boot_status_visible = true;

// --- Helpers ---------------------------------------------------------------

static const char *wifi_glyph(void)
{
    return (app_wifi_get_state() == APP_WIFI_STATE_CONNECTED)
           ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
}

static const char *pp_glyph(void)
{
    if (!s_pp) return LV_SYMBOL_CLOSE;
    switch (s_pp->get_state()) {
    case APP_PP_CONN_CONNECTED: return LV_SYMBOL_OK;
    case APP_PP_CONN_CONNECTING:
    case APP_PP_CONN_RECONNECTING: return LV_SYMBOL_REFRESH;
    default: return LV_SYMBOL_CLOSE;
    }
}

static lv_color_t pp_color(void)
{
    if (!s_pp) return lv_color_hex(0x808080);
    switch (s_pp->get_state()) {
    case APP_PP_CONN_CONNECTED:    return lv_color_hex(0x4CAF50);
    case APP_PP_CONN_CONNECTING:
    case APP_PP_CONN_RECONNECTING: return lv_color_hex(0xFFC107);
    default:                       return lv_color_hex(0xF44336);
    }
}

static lv_color_t wifi_color(void)
{
    return (app_wifi_get_state() == APP_WIFI_STATE_CONNECTED)
           ? lv_color_hex(0x4CAF50)
           : lv_color_hex(0xF44336);
}

// Format the first-running timer, or first-stopped if nothing's running.
static void format_timer_strip(char *out, size_t out_len)
{
    app_pp_timer_t ts[APP_PP_MAX_TIMERS];
    size_t n = app_pp_state_get_timers(ts, APP_PP_MAX_TIMERS);
    int best = -1;
    for (size_t i = 0; i < n; ++i) {
        if (ts[i].state == APP_PP_TIMER_RUNNING ||
            ts[i].state == APP_PP_TIMER_OVERRUN) {
            best = (int) i;
            break;
        }
    }
    if (best < 0 && n > 0) best = 0;
    if (best < 0) {
        snprintf(out, out_len, " ");
        return;
    }
    const char *state_str =
        ts[best].state == APP_PP_TIMER_RUNNING ? "running" :
        ts[best].state == APP_PP_TIMER_OVERRUN ? "OVER"    : "stopped";
    snprintf(out, out_len, "%s   %s   (%s)",
             ts[best].name, ts[best].time_str, state_str);
}

static void format_clock(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year < (2024 - 1900)) {
        // SNTP hasn't synced yet -- show uptime in mm:ss, capped at
        // 99:59 (avoids buffer-truncation -Werror on the formatter).
        int64_t s = esp_timer_get_time() / 1000000;
        int min = (int)((s / 60) % 100);
        int sec = (int)(s % 60);
        snprintf(out, out_len, "up %02d:%02d", min, sec);
        return;
    }
    snprintf(out, out_len, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// --- Apply (LVGL-task-only) ------------------------------------------------

// Re-read state + write widgets. Must be invoked with lvgl_port_lock held.
static void apply_all(void)
{
    // Header icons
    if (s_wifi_label) {
        lv_label_set_text(s_wifi_label, wifi_glyph());
        lv_obj_set_style_text_color(s_wifi_label, wifi_color(), 0);
    }
    if (s_pp_label) {
        lv_label_set_text(s_pp_label, pp_glyph());
        lv_obj_set_style_text_color(s_pp_label, pp_color(), 0);
    }
    if (s_clock_label) {
        char buf[16];
        format_clock(buf, sizeof(buf));
        lv_label_set_text(s_clock_label, buf);
    }

    // Slides
    app_pp_slide_t cur, nxt;
    bool have_cur = app_pp_state_get_current_slide(&cur);
    bool have_nxt = app_pp_state_get_next_slide(&nxt);
    if (s_current_label) {
        lv_label_set_text(s_current_label, have_cur ? cur.text : "");
    }
    if (s_next_label) {
        if (have_nxt) {
            char buf[APP_PP_SLIDE_TEXT_MAX + 16];
            snprintf(buf, sizeof(buf), "next: %s", nxt.text);
            lv_label_set_text(s_next_label, buf);
        } else {
            lv_label_set_text(s_next_label, "");
        }
    }

    // Stage message overlay
    char msg[APP_PP_STAGE_MSG_MAX];
    app_pp_state_get_stage_message(msg, sizeof(msg));
    if (s_stage_msg_banner && s_stage_msg_label) {
        if (msg[0]) {
            lv_label_set_text(s_stage_msg_label, msg);
            lv_obj_clear_flag(s_stage_msg_banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_stage_msg_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Footer: timer strip once we've heard from PP, boot status before.
    if (s_footer_label) {
        if (s_boot_status_visible &&
            app_pp_state_last_update_ms() == 0) {
            // Leave whatever set_status wrote.
        } else {
            s_boot_status_visible = false;
            char buf[128];
            format_timer_strip(buf, sizeof(buf));
            lv_label_set_text(s_footer_label, buf);
        }
    }
}

static void apply_async(void *unused)
{
    (void) unused;
    apply_all();
}

// --- Observers -------------------------------------------------------------

// Fires from non-LVGL tasks (TCP task, wifi event task). Schedule the
// apply onto LVGL via lv_async_call -- but per the lvgl_port_lock memory
// rule, lv_async_call itself must run with the port lock held when called
// from a non-LVGL task.
static void on_async_trigger(void *ctx)
{
    (void) ctx;
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "lvgl_port_lock timeout in on_async_trigger");
        return;
    }
    lv_async_call(apply_async, NULL);
    lvgl_port_unlock();
}

// Clock tick. Runs in lv_timer context (LVGL task) so the lock is
// already held and apply can run directly.
static void clock_tick_cb(lv_timer_t *t)
{
    (void) t;
    if (s_clock_label) {
        char buf[16];
        format_clock(buf, sizeof(buf));
        lv_label_set_text(s_clock_label, buf);
    }
}

// --- Build the layout ------------------------------------------------------

static lv_obj_t *make_row(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void app_ui_init(const app_pp_client_iface_t *pp)
{
    s_pp = pp;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS) != true) {
        ESP_LOGE(TAG, "lvgl_port_lock failed");
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xE6E8EB), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header row.
    s_header_row = make_row(scr, ROW_HEADER_H);
    lv_obj_set_style_pad_hor(s_header_row, 16, 0);
    s_wifi_label = lv_label_create(s_header_row);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_CLOSE);
    lv_obj_align(s_wifi_label, LV_ALIGN_LEFT_MID, 0, 0);
    s_pp_label = lv_label_create(s_header_row);
    lv_label_set_text(s_pp_label, LV_SYMBOL_CLOSE);
    lv_obj_align(s_pp_label, LV_ALIGN_CENTER, 0, 0);
    s_clock_label = lv_label_create(s_header_row);
    lv_label_set_text(s_clock_label, "");
    lv_obj_align(s_clock_label, LV_ALIGN_RIGHT_MID, 0, 0);

    // Current slide.
    lv_obj_t *current_row = make_row(scr, ROW_CURRENT_H);
    lv_obj_set_style_pad_hor(current_row, 24, 0);
    s_current_label = lv_label_create(current_row);
    lv_label_set_long_mode(s_current_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_current_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_current_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(s_current_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_current_label);
    lv_label_set_text(s_current_label, "");

    // Next slide.
    lv_obj_t *next_row = make_row(scr, ROW_NEXT_H);
    lv_obj_set_style_pad_hor(next_row, 24, 0);
    s_next_label = lv_label_create(next_row);
    lv_label_set_long_mode(s_next_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_next_label, LV_PCT(100));
    lv_obj_set_style_text_font(s_next_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_next_label, lv_color_hex(0x9AA0A6), 0);
    lv_obj_center(s_next_label);
    lv_label_set_text(s_next_label, "");

    // Footer row -- timer strip (or boot status until first PP update).
    lv_obj_t *footer_row = make_row(scr, ROW_FOOTER_H);
    lv_obj_set_style_pad_hor(footer_row, 24, 0);
    s_footer_label = lv_label_create(footer_row);
    lv_obj_set_style_text_font(s_footer_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_footer_label, lv_color_hex(0xBDBDBD), 0);
    lv_obj_center(s_footer_label);
    lv_label_set_text(s_footer_label, "Boot...");

    // Stage-message overlay banner. Sits at the bottom; hidden when no
    // message is set. Yellow background to draw the musician's eye.
    s_stage_msg_banner = lv_obj_create(scr);
    lv_obj_set_size(s_stage_msg_banner, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(s_stage_msg_banner, lv_color_hex(0xFBC02D), 0);
    lv_obj_set_style_bg_opa(s_stage_msg_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_stage_msg_banner, 0, 0);
    lv_obj_align(s_stage_msg_banner, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_stage_msg_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_stage_msg_banner, LV_OBJ_FLAG_SCROLLABLE);
    s_stage_msg_label = lv_label_create(s_stage_msg_banner);
    lv_obj_set_style_text_color(s_stage_msg_label, lv_color_hex(0x202020), 0);
    lv_obj_set_style_text_font(s_stage_msg_label, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_stage_msg_label, LV_PCT(100));
    lv_label_set_long_mode(s_stage_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_stage_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_stage_msg_label);

    // Clock tick — 30 s is fine; the clock displays HH:MM.
    lv_timer_create(clock_tick_cb, 30000, NULL);

    lvgl_port_unlock();

    // Wire observers. Each callback fires from a non-LVGL task and uses
    // lv_async_call (under lvgl_port_lock) to defer the actual widget
    // work to the LVGL task.
    app_pp_state_register_on_change(on_async_trigger, NULL);
    app_wifi_register_on_change(on_async_trigger, NULL);
    if (s_pp && s_pp->register_on_change) {
        s_pp->register_on_change(on_async_trigger, NULL);
    }
}

void app_ui_set_status(const char *text)
{
    if (!s_footer_label) return;
    if (!s_boot_status_visible) return;  // first PP update has taken over
    const char *s = text ? text : "";
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    lv_label_set_text(s_footer_label, s);
    lvgl_port_unlock();
}
