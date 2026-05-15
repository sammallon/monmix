#include "app_pp_ui_settings.h"
#include "app_pp_ui_overlay.h"

#include "app_display.h"
#include "app_prefs.h"
#include "app_wifi.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const pp_client_iface_t *s_pp;

void app_pp_ui_settings_init(const pp_client_iface_t *pp) {
    s_pp = pp;
}

// Common helpers for building rows: a label on the left + a widget on
// the right of a flex row.
static lv_obj_t *make_row(lv_obj_t *body, const char *label_text) {
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    return row;
}

// ════════════════════════════════════════════════════════════════════
// GENERAL CONFIG
// ════════════════════════════════════════════════════════════════════

static void on_theme_changed(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool dark = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_theme_t t = dark ? APP_THEME_DARK : APP_THEME_LIGHT;
    app_prefs_set_theme(t);
    app_display_apply_theme(t);
}

static void on_brightness_changed(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if (v < 5)   v = 5;
    if (v > 100) v = 100;
    app_prefs_set_brightness_pct((uint8_t)v);
    app_display_set_backlight_pct((uint8_t)v);
}

static void on_rotation_changed(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target(e);
    bool flipped = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_display_rotation_t r = flipped ? APP_DISPLAY_ROTATION_180 : APP_DISPLAY_ROTATION_0;
    app_prefs_set_display_rotation(r);
    app_display_apply_rotation(r);
}

static void on_sleep_changed(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    // Slider domain is 0..29; map to a humane set of timeout values:
    //   0    -> 15 s
    //   1-5  -> 30 s / 1 / 2 / 5 / 10 min
    //   6-29 -> 15..30 min stepped by 1 min  (then capped)
    static const uint32_t s_steps[] = {
        15, 30, 60, 120, 300, 600, 900, 1200, 1500, 1800
    };
    int n = (int)(sizeof(s_steps) / sizeof(s_steps[0]));
    if (v < 0) v = 0;
    if (v >= n) v = n - 1;
    uint32_t secs = s_steps[v];
    app_prefs_set_sleep_timeout_sec(secs);

    // Live-update the value label sibling so the slider feels coupled.
    lv_obj_t *parent = lv_obj_get_parent(sl);
    lv_obj_t *value_lbl = (lv_obj_t *)lv_obj_get_user_data(sl);
    (void)parent;
    if (value_lbl) {
        char buf[16];
        if (secs < 60)       snprintf(buf, sizeof(buf), "%us", (unsigned)secs);
        else if (secs < 3600) snprintf(buf, sizeof(buf), "%um", (unsigned)(secs / 60));
        else                  snprintf(buf, sizeof(buf), "%uh", (unsigned)(secs / 3600));
        lv_label_set_text(value_lbl, buf);
    }
}

static void build_general_body(lv_obj_t *body, void *ctx) {
    (void)ctx;

    // Theme row.
    {
        lv_obj_t *row = make_row(body, "Theme (Dark)");
        lv_obj_t *sw  = lv_switch_create(row);
        if (app_prefs_get_theme() == APP_THEME_DARK) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_theme_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Brightness row.
    {
        lv_obj_t *row = make_row(body, "Brightness");
        lv_obj_t *sl  = lv_slider_create(row);
        lv_obj_set_width(sl, 260);
        lv_slider_set_range(sl, 5, 100);
        lv_slider_set_value(sl, app_prefs_get_brightness_pct(), LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Rotation row (0 / 180).
    {
        lv_obj_t *row = make_row(body, "Flip 180\xC2\xB0");
        lv_obj_t *sw  = lv_switch_create(row);
        if (app_prefs_get_display_rotation() == APP_DISPLAY_ROTATION_180) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(sw, on_rotation_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Sleep-timeout row.
    {
        lv_obj_t *row = make_row(body, "Sleep timeout");

        // Wrap slider + value label in a horizontal sub-flex so the
        // value text sits to the right of the slider.
        lv_obj_t *wrap = lv_obj_create(row);
        lv_obj_set_size(wrap, 320, 40);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sl = lv_slider_create(wrap);
        lv_obj_set_width(sl, 220);
        lv_slider_set_range(sl, 0, 9);

        lv_obj_t *value_lbl = lv_label_create(wrap);
        lv_obj_set_style_text_font(value_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_user_data(sl, value_lbl);

        // Seed initial slider position from prefs.
        uint32_t cur = app_prefs_get_sleep_timeout_sec();
        int initial = 0;
        static const uint32_t s_steps[] = {15, 30, 60, 120, 300, 600, 900, 1200, 1500, 1800};
        for (int i = 0; i < (int)(sizeof(s_steps) / sizeof(s_steps[0])); ++i) {
            if (cur >= s_steps[i]) initial = i;
        }
        lv_slider_set_value(sl, initial, LV_ANIM_OFF);
        // Trigger the handler once to set the value label text.
        lv_obj_add_event_cb(sl, on_sleep_changed, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

void app_pp_ui_settings_open_general(void) {
    app_pp_ui_overlay_open("General", build_general_body, NULL);
}

// ════════════════════════════════════════════════════════════════════
// WIFI CONFIG
// ════════════════════════════════════════════════════════════════════

static lv_obj_t *s_wifi_ssid_ta;
static lv_obj_t *s_wifi_pass_ta;
static lv_obj_t *s_wifi_status_lbl;

static void wifi_set_status(const char *text) {
    if (s_wifi_status_lbl) lv_label_set_text(s_wifi_status_lbl, text);
}

static void on_wifi_apply(lv_event_t *e) {
    (void)e;
    const char *ssid = s_wifi_ssid_ta ? lv_textarea_get_text(s_wifi_ssid_ta) : "";
    const char *pass = s_wifi_pass_ta ? lv_textarea_get_text(s_wifi_pass_ta) : "";
    app_prefs_set_wifi_ssid    (ssid);
    app_prefs_set_wifi_password(pass);
    if (app_wifi_reconfigure()) {
        wifi_set_status("Reconnecting...");
    } else {
        wifi_set_status("Reconfigure failed");
    }
}

static void on_wifi_scan(lv_event_t *e) {
    (void)e;
    // Sim: fire scan_start which returns immediately with canned results.
    // Hardware-port round wires this to a real scan callback that
    // populates a dropdown.
    char buf[APP_WIFI_SCAN_MAX_RESULTS][33] = {0};
    size_t n = app_wifi_scan_results(buf, APP_WIFI_SCAN_MAX_RESULTS);
    if (n == 0) {
        app_wifi_scan_start(NULL, NULL);
        wifi_set_status("Scanning...");
        return;
    }
    char status[128];
    snprintf(status, sizeof(status), "%u networks found", (unsigned)n);
    wifi_set_status(status);
}

static void build_wifi_body(lv_obj_t *body, void *ctx) {
    (void)ctx;

    // SSID row.
    {
        lv_obj_t *row = make_row(body, "SSID");
        s_wifi_ssid_ta = lv_textarea_create(row);
        lv_obj_set_size(s_wifi_ssid_ta, 360, 40);
        lv_textarea_set_one_line(s_wifi_ssid_ta, true);
        lv_textarea_set_max_length(s_wifi_ssid_ta, 32);
        char ssid_buf[APP_PREFS_STR_MAX];
        app_prefs_get_wifi_ssid(ssid_buf, sizeof(ssid_buf));
        lv_textarea_set_text(s_wifi_ssid_ta, ssid_buf);
        app_pp_ui_overlay_attach_keyboard(s_wifi_ssid_ta, false);
    }

    // Password row.
    {
        lv_obj_t *row = make_row(body, "Password");
        s_wifi_pass_ta = lv_textarea_create(row);
        lv_obj_set_size(s_wifi_pass_ta, 360, 40);
        lv_textarea_set_one_line(s_wifi_pass_ta, true);
        lv_textarea_set_max_length(s_wifi_pass_ta, 63);
        lv_textarea_set_password_mode(s_wifi_pass_ta, true);
        char pass_buf[APP_PREFS_STR_MAX];
        app_prefs_get_wifi_password(pass_buf, sizeof(pass_buf));
        lv_textarea_set_text(s_wifi_pass_ta, pass_buf);
        app_pp_ui_overlay_attach_keyboard(s_wifi_pass_ta, false);
    }

    // Button row: Scan + Apply.
    {
        lv_obj_t *row = make_row(body, "");
        lv_obj_t *scan_btn = lv_button_create(row);
        lv_obj_set_size(scan_btn, 140, 40);
        lv_obj_t *scan_lbl = lv_label_create(scan_btn);
        lv_label_set_text(scan_lbl, "Scan");
        lv_obj_center(scan_lbl);
        lv_obj_add_event_cb(scan_btn, on_wifi_scan, LV_EVENT_CLICKED, NULL);

        lv_obj_t *apply_btn = lv_button_create(row);
        lv_obj_set_size(apply_btn, 140, 40);
        lv_obj_t *apply_lbl = lv_label_create(apply_btn);
        lv_label_set_text(apply_lbl, "Apply");
        lv_obj_center(apply_lbl);
        lv_obj_add_event_cb(apply_btn, on_wifi_apply, LV_EVENT_CLICKED, NULL);
    }

    // Status line.
    {
        s_wifi_status_lbl = lv_label_create(body);
        lv_obj_set_style_text_font(s_wifi_status_lbl, &lv_font_montserrat_14, 0);
        char ip[32];
        app_wifi_format_ip(ip, sizeof(ip));
        char status[96];
        snprintf(status, sizeof(status),
                 "Current: %s @ %s (%s)",
                 app_wifi_get_ssid(),
                 ip,
                 app_wifi_get_security_str());
        lv_label_set_text(s_wifi_status_lbl, status);
    }
}

void app_pp_ui_settings_open_wifi(void) {
    s_wifi_ssid_ta    = NULL;
    s_wifi_pass_ta    = NULL;
    s_wifi_status_lbl = NULL;
    app_pp_ui_overlay_open(LV_SYMBOL_WIFI " WiFi", build_wifi_body, NULL);
}

// ════════════════════════════════════════════════════════════════════
// PP CONFIG
// ════════════════════════════════════════════════════════════════════

static lv_obj_t *s_pp_host_ta;
static lv_obj_t *s_pp_port_ta;
static lv_obj_t *s_pp_status_lbl;

static void pp_set_status(const char *text) {
    if (s_pp_status_lbl) lv_label_set_text(s_pp_status_lbl, text);
}

static void on_pp_apply(lv_event_t *e) {
    (void)e;
    const char *host = s_pp_host_ta ? lv_textarea_get_text(s_pp_host_ta) : "";
    const char *port = s_pp_port_ta ? lv_textarea_get_text(s_pp_port_ta) : "";
    app_prefs_set_pp_host(host);
    if (port && port[0]) {
        long p = strtol(port, NULL, 10);
        if (p > 0 && p <= 65535) app_prefs_set_pp_port((uint16_t)p);
    }
    if (s_pp && s_pp->reconnect) {
        s_pp->reconnect();
        pp_set_status("Reconnecting...");
    } else {
        pp_set_status("Saved (no live client yet)");
    }
}

static void build_pp_body(lv_obj_t *body, void *ctx) {
    (void)ctx;

    // Host row.
    {
        lv_obj_t *row = make_row(body, "Host");
        s_pp_host_ta = lv_textarea_create(row);
        lv_obj_set_size(s_pp_host_ta, 360, 40);
        lv_textarea_set_one_line(s_pp_host_ta, true);
        lv_textarea_set_max_length(s_pp_host_ta, 63);
        char host_buf[APP_PREFS_STR_MAX];
        app_prefs_get_pp_host(host_buf, sizeof(host_buf));
        lv_textarea_set_text(s_pp_host_ta, host_buf);
        app_pp_ui_overlay_attach_keyboard(s_pp_host_ta, false);
    }

    // Port row.
    {
        lv_obj_t *row = make_row(body, "Port");
        s_pp_port_ta = lv_textarea_create(row);
        lv_obj_set_size(s_pp_port_ta, 160, 40);
        lv_textarea_set_one_line(s_pp_port_ta, true);
        lv_textarea_set_max_length(s_pp_port_ta, 5);
        lv_textarea_set_accepted_chars(s_pp_port_ta, "0123456789");
        char port_buf[16];
        snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)app_prefs_get_pp_port());
        lv_textarea_set_text(s_pp_port_ta, port_buf);
        app_pp_ui_overlay_attach_keyboard(s_pp_port_ta, true);
    }

    // Apply button.
    {
        lv_obj_t *row = make_row(body, "");
        lv_obj_t *apply_btn = lv_button_create(row);
        lv_obj_set_size(apply_btn, 200, 40);
        lv_obj_t *apply_lbl = lv_label_create(apply_btn);
        lv_label_set_text(apply_lbl, "Apply & Reconnect");
        lv_obj_center(apply_lbl);
        lv_obj_add_event_cb(apply_btn, on_pp_apply, LV_EVENT_CLICKED, NULL);
    }

    // Status line.
    {
        s_pp_status_lbl = lv_label_create(body);
        lv_obj_set_style_text_font(s_pp_status_lbl, &lv_font_montserrat_14, 0);
        const char *conn = "?";
        if (s_pp && s_pp->get_state) {
            switch (s_pp->get_state()) {
            case APP_PP_CONN_CONNECTED:    conn = "connected";    break;
            case APP_PP_CONN_CONNECTING:   conn = "connecting";   break;
            case APP_PP_CONN_DISCONNECTED: conn = "disconnected"; break;
            case APP_PP_CONN_ERROR:        conn = "error";        break;
            case APP_PP_CONN_BOOT:         conn = "boot";         break;
            }
        }
        char status[96];
        snprintf(status, sizeof(status), "Status: %s", conn);
        lv_label_set_text(s_pp_status_lbl, status);
    }
}

void app_pp_ui_settings_open_pp(void) {
    s_pp_host_ta    = NULL;
    s_pp_port_ta    = NULL;
    s_pp_status_lbl = NULL;
    app_pp_ui_overlay_open(LV_SYMBOL_LIST " ProPresenter", build_pp_body, NULL);
}
