// In-memory prefs. Defaults match the on-device boot defaults (dark
// theme, 0° rotation, 100% brightness, NORM level format, signal indicator
// off). The mute toast panic doesn't depend on prefs state — they exist
// here just so app_ui.c's reads don't deref junk.
#include "app_prefs.h"

#include <stdint.h>
#include <string.h>

static app_level_format_t       s_level_fmt   = APP_LEVEL_FORMAT_NORM;
static app_signal_indicator_t   s_signal_ind  = APP_SIGNAL_INDICATOR_NONE;
static app_theme_t              s_theme       = APP_THEME_DARK;
static app_display_rotation_t   s_rotation    = APP_DISPLAY_ROTATION_0;
static uint8_t                  s_brightness  = 100;
static uint8_t                  s_selected_mix = 0;
static int                      s_color[256];   // index by ms_channel_id (clamped)
static bool                     s_color_set[256];
static bool                     s_use_static_ip = false;
static char                     s_ip[16]   = "";
static char                     s_nm[16]   = "";
static char                     s_gw[16]   = "";
static char                     s_dns[16]  = "";
static char                     s_ntp[64]  = "pool.ntp.org";
static char                     s_tz[64]   = "America/Los_Angeles";
static bool                     s_ntp_dhcp = true;

#define MAX_OBSERVERS 8
static struct { app_prefs_on_change_t cb; void *ctx; } s_obs[MAX_OBSERVERS];
static size_t s_obs_n;

static void notify(void) {
    for (size_t i = 0; i < s_obs_n; ++i) s_obs[i].cb(s_obs[i].ctx);
}

void app_prefs_init(void) {}

app_level_format_t app_prefs_get_level_format(void)        { return s_level_fmt; }
void               app_prefs_set_level_format(app_level_format_t f) { s_level_fmt = f; notify(); }

app_signal_indicator_t app_prefs_get_signal_indicator(void)        { return s_signal_ind; }
void                   app_prefs_set_signal_indicator(app_signal_indicator_t s) { s_signal_ind = s; notify(); }

app_theme_t app_prefs_get_theme(void)        { return s_theme; }
void        app_prefs_set_theme(app_theme_t t) { s_theme = t; notify(); }

app_display_rotation_t app_prefs_get_display_rotation(void)        { return s_rotation; }
void                   app_prefs_set_display_rotation(app_display_rotation_t r) { s_rotation = r; notify(); }

uint8_t app_prefs_get_brightness_pct(void)     { return s_brightness; }
void    app_prefs_set_brightness_pct(uint8_t pct) { s_brightness = pct; notify(); }

uint8_t app_prefs_get_selected_mix_index(void) { return s_selected_mix; }
void    app_prefs_set_selected_mix_index(uint8_t idx) { s_selected_mix = idx; notify(); }

int  app_prefs_get_channel_color(int ms_channel_id) {
    if (ms_channel_id < 0 || ms_channel_id >= 256) return -1;
    return s_color_set[ms_channel_id] ? s_color[ms_channel_id] : -1;
}
void app_prefs_set_channel_color(int ms_channel_id, int index) {
    if (ms_channel_id < 0 || ms_channel_id >= 256) return;
    if (index < 0) { s_color_set[ms_channel_id] = false; }
    else           { s_color_set[ms_channel_id] = true; s_color[ms_channel_id] = index; }
    notify();
}

bool app_prefs_get_wifi_use_static(void)      { return s_use_static_ip; }
void app_prefs_set_wifi_use_static(bool on)   { s_use_static_ip = on; notify(); }

static const char *cp(char *out, size_t n, const char *src) {
    if (!out || n == 0) return "";
    strncpy(out, src, n - 1); out[n - 1] = 0; return out;
}
const char *app_prefs_get_wifi_static_ip      (char *o, size_t n) { return cp(o, n, s_ip); }
const char *app_prefs_get_wifi_static_netmask (char *o, size_t n) { return cp(o, n, s_nm); }
const char *app_prefs_get_wifi_static_gateway (char *o, size_t n) { return cp(o, n, s_gw); }
const char *app_prefs_get_wifi_static_dns     (char *o, size_t n) { return cp(o, n, s_dns); }
void        app_prefs_set_wifi_static_ip      (const char *s) { strncpy(s_ip,  s, 15);  s_ip [15] = 0; }
void        app_prefs_set_wifi_static_netmask (const char *s) { strncpy(s_nm,  s, 15);  s_nm [15] = 0; }
void        app_prefs_set_wifi_static_gateway (const char *s) { strncpy(s_gw,  s, 15);  s_gw [15] = 0; }
void        app_prefs_set_wifi_static_dns     (const char *s) { strncpy(s_dns, s, 15);  s_dns[15] = 0; }

const char *app_prefs_get_ntp_server (char *o, size_t n) { return cp(o, n, s_ntp); }
void        app_prefs_set_ntp_server (const char *s) { strncpy(s_ntp, s, 63); s_ntp[63] = 0; }
const char *app_prefs_get_display_tz (char *o, size_t n) { return cp(o, n, s_tz); }
void        app_prefs_set_display_tz (const char *s) { strncpy(s_tz, s, 63); s_tz[63] = 0; }

bool app_prefs_get_ntp_use_dhcp(void)      { return s_ntp_dhcp; }
void app_prefs_set_ntp_use_dhcp(bool on)   { s_ntp_dhcp = on; }

void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx) {
    if (s_obs_n < MAX_OBSERVERS) { s_obs[s_obs_n].cb = cb; s_obs[s_obs_n].ctx = ctx; s_obs_n++; }
}
void app_prefs_debug_dump(void) {}
