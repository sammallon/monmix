#include "app_prefs.h"

#include <string.h>

// In-memory backing. Hardware-port round swaps this for NVS-primary +
// SD-mirror like esp_ui. The public iface stays identical so callers
// don't notice the switch.

#define MAX_SUBS 4
static struct {
    app_prefs_on_change_t cb;
    void                 *ctx;
} s_subs[MAX_SUBS];
static size_t s_subs_n;

static app_theme_t            s_theme       = APP_THEME_DARK;
static app_display_rotation_t s_rotation    = APP_DISPLAY_ROTATION_0;
static uint8_t                s_brightness  = 80;
static uint32_t               s_sleep_secs  = 60u * 60u;   // 1 h default
static char                   s_ssid[APP_PREFS_STR_MAX]    = "your-ssid-here";
static char                   s_password[APP_PREFS_STR_MAX] = "";
static char                   s_pp_host[APP_PREFS_STR_MAX] = "192.168.0.1";
static uint16_t               s_pp_port    = 49850;        // PP's typical Network API port

static void notify(void) {
    for (size_t i = 0; i < s_subs_n; ++i) {
        if (s_subs[i].cb) s_subs[i].cb(s_subs[i].ctx);
    }
}

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void app_prefs_init(void) {
    // Nothing to load — in-memory defaults are already set above.
}

void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx) {
    if (s_subs_n >= MAX_SUBS) return;
    s_subs[s_subs_n].cb  = cb;
    s_subs[s_subs_n].ctx = ctx;
    ++s_subs_n;
}

app_theme_t app_prefs_get_theme(void)                 { return s_theme; }
void        app_prefs_set_theme(app_theme_t t)        { if (t == s_theme) return; s_theme = t; notify(); }

app_display_rotation_t app_prefs_get_display_rotation(void)             { return s_rotation; }
void                   app_prefs_set_display_rotation(app_display_rotation_t r) {
    if (r != APP_DISPLAY_ROTATION_0 && r != APP_DISPLAY_ROTATION_180) return;
    if (r == s_rotation) return;
    s_rotation = r;
    notify();
}

uint8_t app_prefs_get_brightness_pct(void)            { return s_brightness; }
void    app_prefs_set_brightness_pct(uint8_t pct) {
    if (pct < 5)   pct = 5;
    if (pct > 100) pct = 100;
    if (pct == s_brightness) return;
    s_brightness = pct;
    notify();
}

uint32_t app_prefs_get_sleep_timeout_sec(void)        { return s_sleep_secs; }
void     app_prefs_set_sleep_timeout_sec(uint32_t s) {
    if (s < 15)         s = 15;
    if (s > 30u * 60u)  s = 30u * 60u;
    if (s == s_sleep_secs) return;
    s_sleep_secs = s;
    notify();
}

const char *app_prefs_get_wifi_ssid(char *out, size_t out_len) {
    copy_str(out, out_len, s_ssid);
    return out;
}
void app_prefs_set_wifi_ssid(const char *s) {
    if (!s) s = "";
    if (strcmp(s, s_ssid) == 0) return;
    copy_str(s_ssid, sizeof(s_ssid), s);
    notify();
}

const char *app_prefs_get_wifi_password(char *out, size_t out_len) {
    copy_str(out, out_len, s_password);
    return out;
}
void app_prefs_set_wifi_password(const char *s) {
    if (!s) s = "";
    if (strcmp(s, s_password) == 0) return;
    copy_str(s_password, sizeof(s_password), s);
    notify();
}

const char *app_prefs_get_pp_host(char *out, size_t out_len) {
    copy_str(out, out_len, s_pp_host);
    return out;
}
void app_prefs_set_pp_host(const char *s) {
    if (!s) s = "";
    if (strcmp(s, s_pp_host) == 0) return;
    copy_str(s_pp_host, sizeof(s_pp_host), s);
    notify();
}

uint16_t app_prefs_get_pp_port(void)            { return s_pp_port; }
void     app_prefs_set_pp_port(uint16_t p) {
    if (p == 0) p = 49850;
    if (p == s_pp_port) return;
    s_pp_port = p;
    notify();
}
