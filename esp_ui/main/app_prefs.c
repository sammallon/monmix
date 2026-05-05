#include "app_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "app_storage.h"

static const char *TAG = "app_prefs";

// Channel color is sparse -- only channels the user has explicitly tagged
// have an entry. Cap the table at a sane size; the 12-channel default
// config hits 12, room for double that.
#define MAX_COLOR_ENTRIES   32
#define PREFS_PATH          "/sdcard/monmix-prefs.json"
#define PREFS_TMP_PATH      "/sdcard/monmix-prefs.tmp"

// NVS key layout. Key names are capped at 15 chars by NVS; the `_mt`
// siblings carry per-key u64 mtimes. `mtime_floor` survives reboots so
// new mtimes are guaranteed monotonic per source even with no RTC.
#define NVS_NS              "monmix"
#define NVS_K_LVL_FMT       "lvl_fmt"
#define NVS_K_LVL_FMT_MT    "lvl_fmt_mt"
#define NVS_K_SIG_IND       "sig_ind"
#define NVS_K_SIG_IND_MT    "sig_ind_mt"
#define NVS_K_THEME         "theme"
#define NVS_K_THEME_MT      "theme_mt"
#define NVS_K_DISP_ROT      "disp_rot"
#define NVS_K_DISP_ROT_MT   "disp_rot_mt"
#define NVS_K_CHAN_COLOR    "chan_color"
#define NVS_K_CHAN_COLOR_MT "cclr_mt"
#define NVS_K_BRIGHT        "bright"
#define NVS_K_BRIGHT_MT     "bright_mt"
#define NVS_K_SEL_MIX       "sel_mix"
#define NVS_K_SEL_MIX_MT    "sel_mix_mt"
#define NVS_K_WIFI_STATIC    "wifi_static"
#define NVS_K_WIFI_STATIC_MT "wifi_static_mt"
#define NVS_K_WIFI_IP        "wifi_ip"
#define NVS_K_WIFI_IP_MT     "wifi_ip_mt"
#define NVS_K_WIFI_NM        "wifi_nm"
#define NVS_K_WIFI_NM_MT     "wifi_nm_mt"
#define NVS_K_WIFI_GW        "wifi_gw"
#define NVS_K_WIFI_GW_MT     "wifi_gw_mt"
#define NVS_K_WIFI_DNS       "wifi_dns"
#define NVS_K_WIFI_DNS_MT    "wifi_dns_mt"
#define NVS_K_NTP            "ntp"
#define NVS_K_NTP_MT         "ntp_mt"
#define NVS_K_TZ             "tz"
#define NVS_K_TZ_MT          "tz_mt"
#define NVS_K_MTIME_FLOOR   "mt_floor"

// Default IANA timezone name -- the dropdown in settings is populated from
// app_time.c's table, which translates IANA->POSIX at apply time. Backward-
// compat: a stored POSIX string still works (passes through tzset).
#define DEFAULT_NTP_SERVER  "pool.ntp.org"
#define DEFAULT_DISPLAY_TZ  "America/Los_Angeles"

// Backlight floor — a 0% mis-tap renders the panel unreadable with no
// non-touch recovery path. 5% is dim but legible in a darkened venue.
#define BRIGHTNESS_FLOOR_PCT  5
#define BRIGHTNESS_DEFAULT_PCT 80

typedef struct {
    int id;
    int color;     // 0..7
} color_entry_t;

// One bag per source (NVS or SD). `present[k]` tracks whether the source
// had that key at all -- separates "explicit value with mtime 0" from
// "key absent". Boot-time reconciliation picks the newer mtime per key
// and writes the loser back; absent on both -> install default + commit.
typedef enum {
    K_LVL_FMT = 0,
    K_SIG_IND,
    K_THEME,
    K_DISP_ROT,
    K_CHAN_COLOR,
    K_BRIGHT,
    K_SEL_MIX,
    K_WIFI_STATIC,
    K_WIFI_IP,
    K_WIFI_NM,
    K_WIFI_GW,
    K_WIFI_DNS,
    K_NTP_SERVER,
    K_TZ,
    K_COUNT,
} prefs_key_t;

typedef struct {
    bool                   present[K_COUNT];
    uint64_t               mtime[K_COUNT];

    app_level_format_t     level_format;
    app_signal_indicator_t signal_ind;
    app_theme_t            theme;
    app_display_rotation_t disp_rot;
    color_entry_t          colors[MAX_COLOR_ENTRIES];
    size_t                 color_count;
    uint8_t                brightness_pct;
    uint8_t                selected_mix_index;
    bool                   wifi_static;
    char                   wifi_ip [APP_PREFS_IP_STR_MAX];
    char                   wifi_nm [APP_PREFS_IP_STR_MAX];
    char                   wifi_gw [APP_PREFS_IP_STR_MAX];
    char                   wifi_dns[APP_PREFS_IP_STR_MAX];
    char                   ntp_server[APP_PREFS_STR_MAX];
    char                   display_tz[APP_PREFS_STR_MAX];
} prefs_bag_t;

static SemaphoreHandle_t s_mutex;

// Live (effective) state -- the merge result that getters return.
static app_level_format_t      s_level_format = APP_LEVEL_FORMAT_NORM;
static app_signal_indicator_t  s_signal_ind   = APP_SIGNAL_INDICATOR_NONE;
static app_theme_t             s_theme        = APP_THEME_DARK;
static app_display_rotation_t  s_disp_rot     = APP_DISPLAY_ROTATION_0;
static color_entry_t           s_colors[MAX_COLOR_ENTRIES];
static size_t                  s_color_count;
static uint8_t                 s_brightness_pct = BRIGHTNESS_DEFAULT_PCT;
static uint8_t                 s_selected_mix_index;  // 0-based mix bus
static bool                    s_wifi_static;
static char                    s_wifi_ip [APP_PREFS_IP_STR_MAX];
static char                    s_wifi_nm [APP_PREFS_IP_STR_MAX];
static char                    s_wifi_gw [APP_PREFS_IP_STR_MAX];
static char                    s_wifi_dns[APP_PREFS_IP_STR_MAX];
static char                    s_ntp_server[APP_PREFS_STR_MAX] = DEFAULT_NTP_SERVER;
static char                    s_display_tz[APP_PREFS_STR_MAX] = DEFAULT_DISPLAY_TZ;
static uint64_t                s_mtime[K_COUNT];   // mtime of effective value, per key
static uint64_t                s_mtime_floor;      // monotonic counter base across reboots

#define MAX_SUBSCRIBERS 4
static struct {
    app_prefs_on_change_t cb;
    void                 *ctx;
} s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static int find_color_idx(int ms_channel_id)
{
    for (size_t i = 0; i < s_color_count; ++i) {
        if (s_colors[i].id == ms_channel_id) return (int) i;
    }
    return -1;
}

static const char *level_format_to_str(app_level_format_t f)
{
    return (f == APP_LEVEL_FORMAT_DB) ? "db" : "norm";
}

static app_level_format_t level_format_from_str(const char *s)
{
    if (s && strcmp(s, "db") == 0) return APP_LEVEL_FORMAT_DB;
    return APP_LEVEL_FORMAT_NORM;
}

static const char *signal_indicator_to_str(app_signal_indicator_t v)
{
    switch (v) {
        case APP_SIGNAL_INDICATOR_NONE:    return "none";
        case APP_SIGNAL_INDICATOR_METER:   return "meter";
        case APP_SIGNAL_INDICATOR_PRESENT:
        default:                            return "signal-present";
    }
}

static app_signal_indicator_t signal_indicator_from_str(const char *s)
{
    if (s) {
        if (strcmp(s, "none")  == 0) return APP_SIGNAL_INDICATOR_NONE;
        if (strcmp(s, "meter") == 0) return APP_SIGNAL_INDICATOR_METER;
    }
    return APP_SIGNAL_INDICATOR_PRESENT;
}

static const char *theme_to_str(app_theme_t t)
{
    return (t == APP_THEME_LIGHT) ? "light" : "dark";
}

static app_theme_t theme_from_str(const char *s)
{
    if (s && strcmp(s, "light") == 0) return APP_THEME_LIGHT;
    return APP_THEME_DARK;
}

// Anything other than 180 -> 0; the toggle UI only writes one of two values.
static app_display_rotation_t disp_rot_from_u16(uint16_t v)
{
    return (v == 180) ? APP_DISPLAY_ROTATION_180 : APP_DISPLAY_ROTATION_0;
}

static const char *key_name(prefs_key_t k)
{
    switch (k) {
        case K_LVL_FMT:    return "level_format";
        case K_SIG_IND:    return "signal_indicator";
        case K_THEME:      return "theme";
        case K_DISP_ROT:   return "display_rotation";
        case K_CHAN_COLOR: return "channel_color";
        case K_BRIGHT:     return "display_brightness_pct";
        case K_SEL_MIX:    return "selected_mix_index";
        case K_WIFI_STATIC:return "wifi_use_static";
        case K_WIFI_IP:    return "wifi_static_ip";
        case K_WIFI_NM:    return "wifi_static_netmask";
        case K_WIFI_GW:    return "wifi_static_gateway";
        case K_WIFI_DNS:   return "wifi_static_dns";
        case K_NTP_SERVER: return "ntp_server";
        case K_TZ:         return "display_tz";
        default:           return "?";
    }
}

// Generic bounded string copy. Out is always NUL-terminated.
static void copy_str_bounded(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Copy-with-cap helper for the new string-valued keys. Out is always
// NUL-terminated; caller's buffer must be at least APP_PREFS_IP_STR_MAX.
static void copy_ip_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint8_t clamp_brightness(uint8_t pct)
{
    if (pct < BRIGHTNESS_FLOOR_PCT) return BRIGHTNESS_FLOOR_PCT;
    if (pct > 100)                  return 100;
    return pct;
}

static void notify_subscribers(void)
{
    for (size_t i = 0; i < s_subscriber_count; ++i) {
        if (s_subscribers[i].cb) {
            s_subscribers[i].cb(s_subscribers[i].ctx);
        }
    }
}

// Allocate a fresh mtime that beats anything seen so far. Guarantees
// monotonicity across reboots (s_mtime_floor is persisted in NVS and
// reloaded at init) and across keys this boot.
static uint64_t alloc_mtime(void)
{
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t next   = s_mtime_floor + 1;
    if (now_ms > next) next = now_ms;
    s_mtime_floor = next;
    return next;
}

// ---------------------------------------------------------------------------
// NVS read/write
// ---------------------------------------------------------------------------

static esp_err_t nvs_open_ro(nvs_handle_t *out)
{
    return nvs_open(NVS_NS, NVS_READONLY, out);
}

static esp_err_t nvs_open_rw(nvs_handle_t *out)
{
    return nvs_open(NVS_NS, NVS_READWRITE, out);
}

static bool nvs_get_u64_opt(nvs_handle_t h, const char *key, uint64_t *out)
{
    return nvs_get_u64(h, key, out) == ESP_OK;
}

// Side effect: bumps the global s_mtime_floor if NVS holds a larger value.
// Only called from init() and the debug dumper -- both safe under the
// mutex; the floor only ever advances forward.
static void nvs_load(prefs_bag_t *bag)
{
    memset(bag, 0, sizeof(*bag));
    bag->level_format       = APP_LEVEL_FORMAT_NORM;
    bag->signal_ind         = APP_SIGNAL_INDICATOR_NONE;
    bag->theme              = APP_THEME_DARK;
    bag->disp_rot           = APP_DISPLAY_ROTATION_0;
    bag->brightness_pct     = BRIGHTNESS_DEFAULT_PCT;
    bag->selected_mix_index = 0;
    copy_str_bounded(bag->ntp_server, sizeof(bag->ntp_server), DEFAULT_NTP_SERVER);
    copy_str_bounded(bag->display_tz, sizeof(bag->display_tz), DEFAULT_DISPLAY_TZ);

    nvs_handle_t h;
    if (nvs_open_ro(&h) != ESP_OK) return;

    uint8_t u8;
    if (nvs_get_u8(h, NVS_K_LVL_FMT, &u8) == ESP_OK) {
        bag->present[K_LVL_FMT] = true;
        bag->level_format = (u8 == APP_LEVEL_FORMAT_DB) ? APP_LEVEL_FORMAT_DB : APP_LEVEL_FORMAT_NORM;
        nvs_get_u64_opt(h, NVS_K_LVL_FMT_MT, &bag->mtime[K_LVL_FMT]);
    }
    if (nvs_get_u8(h, NVS_K_SIG_IND, &u8) == ESP_OK) {
        bag->present[K_SIG_IND] = true;
        if      (u8 == APP_SIGNAL_INDICATOR_METER)   bag->signal_ind = APP_SIGNAL_INDICATOR_METER;
        else if (u8 == APP_SIGNAL_INDICATOR_PRESENT) bag->signal_ind = APP_SIGNAL_INDICATOR_PRESENT;
        else                                          bag->signal_ind = APP_SIGNAL_INDICATOR_NONE;
        nvs_get_u64_opt(h, NVS_K_SIG_IND_MT, &bag->mtime[K_SIG_IND]);
    }
    if (nvs_get_u8(h, NVS_K_THEME, &u8) == ESP_OK) {
        bag->present[K_THEME] = true;
        bag->theme = (u8 == APP_THEME_LIGHT) ? APP_THEME_LIGHT : APP_THEME_DARK;
        nvs_get_u64_opt(h, NVS_K_THEME_MT, &bag->mtime[K_THEME]);
    }

    uint16_t u16;
    if (nvs_get_u16(h, NVS_K_DISP_ROT, &u16) == ESP_OK) {
        bag->present[K_DISP_ROT] = true;
        bag->disp_rot = disp_rot_from_u16(u16);
        nvs_get_u64_opt(h, NVS_K_DISP_ROT_MT, &bag->mtime[K_DISP_ROT]);
    }

    // channel_color blob: array of (int id, int color) pairs.
    size_t blob_len = sizeof(color_entry_t) * MAX_COLOR_ENTRIES;
    color_entry_t tmp[MAX_COLOR_ENTRIES];
    esp_err_t err = nvs_get_blob(h, NVS_K_CHAN_COLOR, tmp, &blob_len);
    if (err == ESP_OK && blob_len % sizeof(color_entry_t) == 0) {
        bag->present[K_CHAN_COLOR] = true;
        bag->color_count = blob_len / sizeof(color_entry_t);
        if (bag->color_count > MAX_COLOR_ENTRIES) bag->color_count = MAX_COLOR_ENTRIES;
        memcpy(bag->colors, tmp, bag->color_count * sizeof(color_entry_t));
        nvs_get_u64_opt(h, NVS_K_CHAN_COLOR_MT, &bag->mtime[K_CHAN_COLOR]);
    }

    if (nvs_get_u8(h, NVS_K_BRIGHT, &u8) == ESP_OK) {
        bag->present[K_BRIGHT]   = true;
        bag->brightness_pct      = clamp_brightness(u8);
        nvs_get_u64_opt(h, NVS_K_BRIGHT_MT, &bag->mtime[K_BRIGHT]);
    }

    if (nvs_get_u8(h, NVS_K_SEL_MIX, &u8) == ESP_OK) {
        bag->present[K_SEL_MIX]    = true;
        bag->selected_mix_index    = u8;
        nvs_get_u64_opt(h, NVS_K_SEL_MIX_MT, &bag->mtime[K_SEL_MIX]);
    }

    if (nvs_get_u8(h, NVS_K_WIFI_STATIC, &u8) == ESP_OK) {
        bag->present[K_WIFI_STATIC] = true;
        bag->wifi_static = (u8 != 0);
        nvs_get_u64_opt(h, NVS_K_WIFI_STATIC_MT, &bag->mtime[K_WIFI_STATIC]);
    }
    struct { const char *k; const char *mk; prefs_key_t pk; char *dst; } str_keys[] = {
        { NVS_K_WIFI_IP,  NVS_K_WIFI_IP_MT,  K_WIFI_IP,  bag->wifi_ip  },
        { NVS_K_WIFI_NM,  NVS_K_WIFI_NM_MT,  K_WIFI_NM,  bag->wifi_nm  },
        { NVS_K_WIFI_GW,  NVS_K_WIFI_GW_MT,  K_WIFI_GW,  bag->wifi_gw  },
        { NVS_K_WIFI_DNS, NVS_K_WIFI_DNS_MT, K_WIFI_DNS, bag->wifi_dns },
    };
    for (size_t i = 0; i < sizeof(str_keys) / sizeof(str_keys[0]); ++i) {
        size_t need = APP_PREFS_IP_STR_MAX;
        if (nvs_get_str(h, str_keys[i].k, str_keys[i].dst, &need) == ESP_OK) {
            bag->present[str_keys[i].pk] = true;
            nvs_get_u64_opt(h, str_keys[i].mk, &bag->mtime[str_keys[i].pk]);
        }
    }

    struct { const char *k; const char *mk; prefs_key_t pk; char *dst; } big_str_keys[] = {
        { NVS_K_NTP, NVS_K_NTP_MT, K_NTP_SERVER, bag->ntp_server },
        { NVS_K_TZ,  NVS_K_TZ_MT,  K_TZ,         bag->display_tz },
    };
    for (size_t i = 0; i < sizeof(big_str_keys) / sizeof(big_str_keys[0]); ++i) {
        size_t need = APP_PREFS_STR_MAX;
        if (nvs_get_str(h, big_str_keys[i].k, big_str_keys[i].dst, &need) == ESP_OK) {
            bag->present[big_str_keys[i].pk] = true;
            nvs_get_u64_opt(h, big_str_keys[i].mk, &bag->mtime[big_str_keys[i].pk]);
        }
    }

    uint64_t floor = 0;
    if (nvs_get_u64_opt(h, NVS_K_MTIME_FLOOR, &floor) && floor > s_mtime_floor) {
        s_mtime_floor = floor;
    }

    nvs_close(h);
}

static bool nvs_write_key(prefs_key_t k, const void *value, size_t value_len, uint64_t mtime,
                          uint64_t mtime_floor)
{
    nvs_handle_t h;
    if (nvs_open_rw(&h) != ESP_OK) return false;
    esp_err_t err = ESP_OK;
    switch (k) {
        case K_LVL_FMT:
            err = nvs_set_u8(h, NVS_K_LVL_FMT, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_LVL_FMT_MT, mtime);
            break;
        case K_SIG_IND:
            err = nvs_set_u8(h, NVS_K_SIG_IND, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_SIG_IND_MT, mtime);
            break;
        case K_THEME:
            err = nvs_set_u8(h, NVS_K_THEME, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_THEME_MT, mtime);
            break;
        case K_DISP_ROT:
            err = nvs_set_u16(h, NVS_K_DISP_ROT, *(const uint16_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_DISP_ROT_MT, mtime);
            break;
        case K_CHAN_COLOR:
            err = nvs_set_blob(h, NVS_K_CHAN_COLOR, value, value_len);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_CHAN_COLOR_MT, mtime);
            break;
        case K_BRIGHT:
            err = nvs_set_u8(h, NVS_K_BRIGHT, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_BRIGHT_MT, mtime);
            break;
        case K_SEL_MIX:
            err = nvs_set_u8(h, NVS_K_SEL_MIX, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_SEL_MIX_MT, mtime);
            break;
        case K_WIFI_STATIC:
            err = nvs_set_u8(h, NVS_K_WIFI_STATIC, *(const uint8_t *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_WIFI_STATIC_MT, mtime);
            break;
        case K_WIFI_IP:
            err = nvs_set_str(h, NVS_K_WIFI_IP, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_WIFI_IP_MT, mtime);
            break;
        case K_WIFI_NM:
            err = nvs_set_str(h, NVS_K_WIFI_NM, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_WIFI_NM_MT, mtime);
            break;
        case K_WIFI_GW:
            err = nvs_set_str(h, NVS_K_WIFI_GW, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_WIFI_GW_MT, mtime);
            break;
        case K_WIFI_DNS:
            err = nvs_set_str(h, NVS_K_WIFI_DNS, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_WIFI_DNS_MT, mtime);
            break;
        case K_NTP_SERVER:
            err = nvs_set_str(h, NVS_K_NTP, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_NTP_MT, mtime);
            break;
        case K_TZ:
            err = nvs_set_str(h, NVS_K_TZ, (const char *)value);
            if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_TZ_MT, mtime);
            break;
        default:
            err = ESP_FAIL;
            break;
    }
    if (err == ESP_OK) err = nvs_set_u64(h, NVS_K_MTIME_FLOOR, mtime_floor);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs write %s failed: %s", key_name(k), esp_err_to_name(err));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SD JSON read/write
// ---------------------------------------------------------------------------

static uint64_t json_get_u64(const cJSON *root, const char *key)
{
    const cJSON *j = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsNumber(j)) return 0;
    // cJSON's valuedouble loses precision past 2^53, but we never approach
    // that with uptime-ms counters on this device.
    if (j->valuedouble < 0) return 0;
    return (uint64_t) j->valuedouble;
}

// Parse an SD-side bag. `present[k]` set when the value field appears;
// mtime defaults to 0 if the sibling `_mt` field is absent. Returns true
// if the file existed and parsed (regardless of which keys were inside).
static bool sd_load(prefs_bag_t *bag)
{
    memset(bag, 0, sizeof(*bag));
    bag->level_format       = APP_LEVEL_FORMAT_NORM;
    bag->signal_ind         = APP_SIGNAL_INDICATOR_NONE;
    bag->theme              = APP_THEME_DARK;
    bag->disp_rot           = APP_DISPLAY_ROTATION_0;
    bag->brightness_pct     = BRIGHTNESS_DEFAULT_PCT;
    bag->selected_mix_index = 0;
    copy_str_bounded(bag->ntp_server, sizeof(bag->ntp_server), DEFAULT_NTP_SERVER);
    copy_str_bounded(bag->display_tz, sizeof(bag->display_tz), DEFAULT_DISPLAY_TZ);

    if (!app_storage_is_mounted()) return false;
    FILE *f = fopen(PREFS_PATH, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 64 * 1024) { fclose(f); return false; }
    char *raw = malloc((size_t) len + 1);
    if (!raw) { fclose(f); return false; }
    size_t n = fread(raw, 1, (size_t) len, f);
    fclose(f);
    raw[n] = '\0';

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        ESP_LOGW(TAG, "prefs JSON parse failed");
        return false;
    }

    cJSON *jlf = cJSON_GetObjectItem(root, "level_format");
    if (cJSON_IsString(jlf)) {
        bag->present[K_LVL_FMT] = true;
        bag->level_format = level_format_from_str(jlf->valuestring);
        bag->mtime[K_LVL_FMT] = json_get_u64(root, "level_format_mt");
    }
    cJSON *jsi = cJSON_GetObjectItem(root, "signal_indicator");
    if (cJSON_IsString(jsi)) {
        bag->present[K_SIG_IND] = true;
        bag->signal_ind = signal_indicator_from_str(jsi->valuestring);
        bag->mtime[K_SIG_IND] = json_get_u64(root, "signal_indicator_mt");
    }
    cJSON *jth = cJSON_GetObjectItem(root, "theme");
    if (cJSON_IsString(jth)) {
        bag->present[K_THEME] = true;
        bag->theme = theme_from_str(jth->valuestring);
        bag->mtime[K_THEME] = json_get_u64(root, "theme_mt");
    }
    cJSON *jdr = cJSON_GetObjectItem(root, "display_rotation");
    if (cJSON_IsNumber(jdr)) {
        bag->present[K_DISP_ROT] = true;
        bag->disp_rot = disp_rot_from_u16((uint16_t) jdr->valuedouble);
        bag->mtime[K_DISP_ROT] = json_get_u64(root, "display_rotation_mt");
    }
    cJSON *jcc = cJSON_GetObjectItem(root, "channel_color");
    if (cJSON_IsObject(jcc)) {
        bag->present[K_CHAN_COLOR] = true;
        cJSON *kv = NULL;
        cJSON_ArrayForEach(kv, jcc) {
            if (!cJSON_IsNumber(kv) || bag->color_count >= MAX_COLOR_ENTRIES) continue;
            int id    = atoi(kv->string ? kv->string : "-1");
            int color = (int) kv->valuedouble;
            if (id < 0 || color < 0 || color > 7) continue;
            bag->colors[bag->color_count].id    = id;
            bag->colors[bag->color_count].color = color;
            bag->color_count++;
        }
        bag->mtime[K_CHAN_COLOR] = json_get_u64(root, "channel_color_mt");
    }
    cJSON *jbr = cJSON_GetObjectItem(root, "display_brightness_pct");
    if (cJSON_IsNumber(jbr)) {
        bag->present[K_BRIGHT]   = true;
        int v = (int) jbr->valuedouble;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        bag->brightness_pct = clamp_brightness((uint8_t) v);
        bag->mtime[K_BRIGHT] = json_get_u64(root, "display_brightness_pct_mt");
    }
    cJSON *jsm = cJSON_GetObjectItem(root, "selected_mix_index");
    if (cJSON_IsNumber(jsm)) {
        bag->present[K_SEL_MIX] = true;
        int v = (int) jsm->valuedouble;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        bag->selected_mix_index = (uint8_t) v;
        bag->mtime[K_SEL_MIX]   = json_get_u64(root, "selected_mix_index_mt");
    }

    cJSON *jws = cJSON_GetObjectItem(root, "wifi_use_static");
    if (cJSON_IsBool(jws) || cJSON_IsNumber(jws)) {
        bag->present[K_WIFI_STATIC] = true;
        bag->wifi_static = cJSON_IsBool(jws) ? cJSON_IsTrue(jws)
                                              : (jws->valuedouble != 0);
        bag->mtime[K_WIFI_STATIC] = json_get_u64(root, "wifi_use_static_mt");
    }
    struct { const char *jk; const char *mk; prefs_key_t pk; char *dst; } str_keys[] = {
        { "wifi_static_ip",      "wifi_static_ip_mt",      K_WIFI_IP,  bag->wifi_ip  },
        { "wifi_static_netmask", "wifi_static_netmask_mt", K_WIFI_NM,  bag->wifi_nm  },
        { "wifi_static_gateway", "wifi_static_gateway_mt", K_WIFI_GW,  bag->wifi_gw  },
        { "wifi_static_dns",     "wifi_static_dns_mt",     K_WIFI_DNS, bag->wifi_dns },
    };
    for (size_t i = 0; i < sizeof(str_keys) / sizeof(str_keys[0]); ++i) {
        cJSON *j = cJSON_GetObjectItem(root, str_keys[i].jk);
        if (cJSON_IsString(j)) {
            bag->present[str_keys[i].pk] = true;
            copy_ip_str(str_keys[i].dst, APP_PREFS_IP_STR_MAX, j->valuestring);
            bag->mtime[str_keys[i].pk] = json_get_u64(root, str_keys[i].mk);
        }
    }

    struct { const char *jk; const char *mk; prefs_key_t pk; char *dst; } big_str[] = {
        { "ntp_server",     "ntp_server_mt",     K_NTP_SERVER, bag->ntp_server },
        { "display_timezone","display_timezone_mt", K_TZ,        bag->display_tz },
    };
    for (size_t i = 0; i < sizeof(big_str) / sizeof(big_str[0]); ++i) {
        cJSON *j = cJSON_GetObjectItem(root, big_str[i].jk);
        if (cJSON_IsString(j)) {
            bag->present[big_str[i].pk] = true;
            copy_str_bounded(big_str[i].dst, APP_PREFS_STR_MAX, j->valuestring);
            bag->mtime[big_str[i].pk] = json_get_u64(root, big_str[i].mk);
        }
    }

    cJSON_Delete(root);
    return true;
}

// Serialize current effective state + mtimes to the SD card. Atomic-ish:
// write to .tmp, fsync via fclose, unlink old, rename. FATFS doesn't do
// POSIX rename-overwrite, hence the unlink-first dance.
static bool sd_save_locked(void)
{
    if (!app_storage_is_mounted()) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "level_format",      level_format_to_str(s_level_format));
    cJSON_AddNumberToObject(root, "level_format_mt",   (double) s_mtime[K_LVL_FMT]);
    cJSON_AddStringToObject(root, "signal_indicator",  signal_indicator_to_str(s_signal_ind));
    cJSON_AddNumberToObject(root, "signal_indicator_mt", (double) s_mtime[K_SIG_IND]);
    cJSON_AddStringToObject(root, "theme",             theme_to_str(s_theme));
    cJSON_AddNumberToObject(root, "theme_mt",          (double) s_mtime[K_THEME]);
    cJSON_AddNumberToObject(root, "display_rotation",     (double) s_disp_rot);
    cJSON_AddNumberToObject(root, "display_rotation_mt",  (double) s_mtime[K_DISP_ROT]);
    cJSON *jcc = cJSON_AddObjectToObject(root, "channel_color");
    for (size_t i = 0; i < s_color_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "%d", s_colors[i].id);
        cJSON_AddNumberToObject(jcc, key, s_colors[i].color);
    }
    cJSON_AddNumberToObject(root, "channel_color_mt",  (double) s_mtime[K_CHAN_COLOR]);
    cJSON_AddNumberToObject(root, "display_brightness_pct",    (double) s_brightness_pct);
    cJSON_AddNumberToObject(root, "display_brightness_pct_mt", (double) s_mtime[K_BRIGHT]);
    cJSON_AddNumberToObject(root, "selected_mix_index",        (double) s_selected_mix_index);
    cJSON_AddNumberToObject(root, "selected_mix_index_mt",     (double) s_mtime[K_SEL_MIX]);
    cJSON_AddBoolToObject  (root, "wifi_use_static",       s_wifi_static);
    cJSON_AddNumberToObject(root, "wifi_use_static_mt",    (double) s_mtime[K_WIFI_STATIC]);
    cJSON_AddStringToObject(root, "wifi_static_ip",        s_wifi_ip);
    cJSON_AddNumberToObject(root, "wifi_static_ip_mt",     (double) s_mtime[K_WIFI_IP]);
    cJSON_AddStringToObject(root, "wifi_static_netmask",   s_wifi_nm);
    cJSON_AddNumberToObject(root, "wifi_static_netmask_mt",(double) s_mtime[K_WIFI_NM]);
    cJSON_AddStringToObject(root, "wifi_static_gateway",   s_wifi_gw);
    cJSON_AddNumberToObject(root, "wifi_static_gateway_mt",(double) s_mtime[K_WIFI_GW]);
    cJSON_AddStringToObject(root, "wifi_static_dns",       s_wifi_dns);
    cJSON_AddNumberToObject(root, "wifi_static_dns_mt",    (double) s_mtime[K_WIFI_DNS]);
    cJSON_AddStringToObject(root, "ntp_server",            s_ntp_server);
    cJSON_AddNumberToObject(root, "ntp_server_mt",         (double) s_mtime[K_NTP_SERVER]);
    cJSON_AddStringToObject(root, "display_timezone",      s_display_tz);
    cJSON_AddNumberToObject(root, "display_timezone_mt",   (double) s_mtime[K_TZ]);

    char *buf = cJSON_Print(root);
    cJSON_Delete(root);
    if (!buf) return false;

    bool  ok = false;
    FILE *f  = fopen(PREFS_TMP_PATH, "wb");
    if (f) {
        size_t buflen = strlen(buf);
        ok = fwrite(buf, 1, buflen, f) == buflen;
        fclose(f);
        if (ok) {
            remove(PREFS_PATH);
            if (rename(PREFS_TMP_PATH, PREFS_PATH) != 0) {
                ESP_LOGW(TAG, "rename %s -> %s failed", PREFS_TMP_PATH, PREFS_PATH);
                ok = false;
            }
        }
    }
    free(buf);
    return ok;
}

// ---------------------------------------------------------------------------
// Commit path (single key write)
// ---------------------------------------------------------------------------

// Push the current effective value of `k` to NVS using the per-key mtime
// already in s_mtime[]. Caller holds s_mutex.
static bool prefs_write_nvs_locked(prefs_key_t k)
{
    switch (k) {
        case K_LVL_FMT: {
            uint8_t v = (uint8_t) s_level_format;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_SIG_IND: {
            uint8_t v = (uint8_t) s_signal_ind;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_THEME: {
            uint8_t v = (uint8_t) s_theme;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_DISP_ROT: {
            uint16_t v = (uint16_t) s_disp_rot;
            return nvs_write_key(k, &v, sizeof(v), s_mtime[k], s_mtime_floor);
        }
        case K_CHAN_COLOR:
            return nvs_write_key(k, s_colors,
                                 s_color_count * sizeof(color_entry_t),
                                 s_mtime[k], s_mtime_floor);
        case K_BRIGHT: {
            uint8_t v = s_brightness_pct;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_SEL_MIX: {
            uint8_t v = s_selected_mix_index;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_WIFI_STATIC: {
            uint8_t v = s_wifi_static ? 1 : 0;
            return nvs_write_key(k, &v, 1, s_mtime[k], s_mtime_floor);
        }
        case K_WIFI_IP:
            return nvs_write_key(k, s_wifi_ip,  strlen(s_wifi_ip)  + 1, s_mtime[k], s_mtime_floor);
        case K_WIFI_NM:
            return nvs_write_key(k, s_wifi_nm,  strlen(s_wifi_nm)  + 1, s_mtime[k], s_mtime_floor);
        case K_WIFI_GW:
            return nvs_write_key(k, s_wifi_gw,  strlen(s_wifi_gw)  + 1, s_mtime[k], s_mtime_floor);
        case K_WIFI_DNS:
            return nvs_write_key(k, s_wifi_dns, strlen(s_wifi_dns) + 1, s_mtime[k], s_mtime_floor);
        case K_NTP_SERVER:
            return nvs_write_key(k, s_ntp_server, strlen(s_ntp_server) + 1, s_mtime[k], s_mtime_floor);
        case K_TZ:
            return nvs_write_key(k, s_display_tz, strlen(s_display_tz) + 1, s_mtime[k], s_mtime_floor);
        default:
            return false;
    }
}

// Public setter path: bump mtime, write NVS, write SD mirror. Caller
// holds s_mutex.
static void prefs_commit_locked(prefs_key_t k)
{
    s_mtime[k] = alloc_mtime();
    if (!prefs_write_nvs_locked(k)) ESP_LOGW(TAG, "nvs commit failed for %s", key_name(k));

    bool sd_ok = sd_save_locked();
    if (!sd_ok && app_storage_is_mounted()) {
        ESP_LOGW(TAG, "sd mirror commit failed for %s", key_name(k));
    }
}

// ---------------------------------------------------------------------------
// Boot reconciliation
// ---------------------------------------------------------------------------

// Per-key reconciliation state from merge_bags.
typedef struct {
    bool install_default;   // neither side had it -> install default + commit both
    bool nvs_needs_write;   // SD won (or migration) -> write its value to NVS
    bool sd_needs_write;    // NVS won (or default install) -> write effective state to SD
} merge_action_t;

// Pick the winner per key from NVS vs SD. Decision rule:
//   - Both absent: install default, write to both.
//   - NVS absent, SD present: SD wins -> seed NVS from SD (handles legacy
//     SD-only migration; mtime preserved as-is, may be 0).
//   - NVS present, SD absent: NVS wins -> rewrite SD mirror only.
//   - Both present: newer mtime wins. Tie -> NVS (primary), no rewrite.

static void merge_bags(const prefs_bag_t *nvs, const prefs_bag_t *sd, bool sd_loaded,
                       merge_action_t actions[K_COUNT])
{
    memset(actions, 0, sizeof(merge_action_t) * K_COUNT);

    for (int k = 0; k < K_COUNT; ++k) {
        bool n = nvs->present[k];
        bool s = sd_loaded && sd->present[k];
        const prefs_bag_t *winner = NULL;
        merge_action_t *act = &actions[k];

        if (!n && !s) {
            act->install_default = true;
        } else if (n && !s) {
            winner = nvs;
            // Only rewrite SD if it's mountable; otherwise no-op.
            act->sd_needs_write = sd_loaded;
        } else if (!n && s) {
            winner = sd;
            act->nvs_needs_write = true;     // migration / SD-only edit -> seed NVS
        } else {
            if (sd->mtime[k] > nvs->mtime[k]) {
                winner = sd;
                act->nvs_needs_write = true;
            } else if (nvs->mtime[k] > sd->mtime[k]) {
                winner = nvs;
                act->sd_needs_write = true;
            } else {
                winner = nvs;                 // tie -> NVS
            }
        }

        if (winner) {
            switch (k) {
                case K_LVL_FMT:    s_level_format = winner->level_format; break;
                case K_SIG_IND:    s_signal_ind   = winner->signal_ind;   break;
                case K_THEME:      s_theme        = winner->theme;        break;
                case K_DISP_ROT:   s_disp_rot     = winner->disp_rot;     break;
                case K_CHAN_COLOR:
                    s_color_count = winner->color_count;
                    memcpy(s_colors, winner->colors,
                           winner->color_count * sizeof(color_entry_t));
                    break;
                case K_BRIGHT:     s_brightness_pct = winner->brightness_pct; break;
                case K_SEL_MIX:    s_selected_mix_index = winner->selected_mix_index; break;
                case K_WIFI_STATIC: s_wifi_static = winner->wifi_static; break;
                case K_WIFI_IP:    copy_ip_str(s_wifi_ip,  sizeof(s_wifi_ip),  winner->wifi_ip);  break;
                case K_WIFI_NM:    copy_ip_str(s_wifi_nm,  sizeof(s_wifi_nm),  winner->wifi_nm);  break;
                case K_WIFI_GW:    copy_ip_str(s_wifi_gw,  sizeof(s_wifi_gw),  winner->wifi_gw);  break;
                case K_WIFI_DNS:   copy_ip_str(s_wifi_dns, sizeof(s_wifi_dns), winner->wifi_dns); break;
                case K_NTP_SERVER: copy_str_bounded(s_ntp_server, sizeof(s_ntp_server), winner->ntp_server); break;
                case K_TZ:         copy_str_bounded(s_display_tz, sizeof(s_display_tz), winner->display_tz); break;
                default:           break;
            }
            s_mtime[k] = winner->mtime[k];
        }

        // Track floor across both sides.
        if (n && nvs->mtime[k] > s_mtime_floor) s_mtime_floor = nvs->mtime[k];
        if (s && sd->mtime[k]  > s_mtime_floor) s_mtime_floor = sd->mtime[k];
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void app_prefs_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    prefs_bag_t nvs_bag, sd_bag;
    nvs_load(&nvs_bag);
    bool sd_loaded = sd_load(&sd_bag);

    merge_action_t actions[K_COUNT];
    merge_bags(&nvs_bag, &sd_bag, sd_loaded, actions);

    bool any_write_to_sd = false;

    for (int k = 0; k < K_COUNT; ++k) {
        const merge_action_t *act = &actions[k];
        if (act->install_default) {
            // Defaults are already in s_*. Allocate a fresh mtime and
            // write to both sides. Only fresh-mtime path on this boot.
            s_mtime[k] = alloc_mtime();
            if (!prefs_write_nvs_locked((prefs_key_t) k))
                ESP_LOGW(TAG, "default install: nvs write failed for %s", key_name(k));
            any_write_to_sd = true;
        } else if (act->nvs_needs_write) {
            // SD won (legacy migration or newer SD edit). Seed NVS at the
            // SD-side mtime to preserve the comparison invariant on next
            // boot. Don't rewrite SD -- it's already authoritative.
            if (!prefs_write_nvs_locked((prefs_key_t) k))
                ESP_LOGW(TAG, "seed-from-sd: nvs write failed for %s", key_name(k));
        } else if (act->sd_needs_write) {
            // NVS won. Mark SD for rewrite (handled in single sd_save below).
            any_write_to_sd = true;
        }
    }

    if (any_write_to_sd) {
        if (!sd_save_locked() && app_storage_is_mounted()) {
            ESP_LOGW(TAG, "boot reconcile: sd save failed");
        }
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "prefs ready (level=%s indicator=%s theme=%s rot=%u colors=%u bright=%u%% mix=%u tz='%s' ntp='%s' sd=%s)",
             level_format_to_str(s_level_format),
             signal_indicator_to_str(s_signal_ind),
             theme_to_str(s_theme),
             (unsigned) s_disp_rot,
             (unsigned) s_color_count,
             (unsigned) s_brightness_pct,
             (unsigned) s_selected_mix_index,
             s_display_tz, s_ntp_server,
             sd_loaded ? "loaded" : "absent");
}

app_level_format_t app_prefs_get_level_format(void) { return s_level_format; }

void app_prefs_set_level_format(app_level_format_t f)
{
    if (!s_mutex || s_level_format == f) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_level_format = f;
    prefs_commit_locked(K_LVL_FMT);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

app_signal_indicator_t app_prefs_get_signal_indicator(void) { return s_signal_ind; }

void app_prefs_set_signal_indicator(app_signal_indicator_t s)
{
    if (!s_mutex || s_signal_ind == s) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_signal_ind = s;
    prefs_commit_locked(K_SIG_IND);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

app_theme_t app_prefs_get_theme(void) { return s_theme; }

void app_prefs_set_theme(app_theme_t t)
{
    if (!s_mutex || s_theme == t) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_theme = t;
    prefs_commit_locked(K_THEME);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

app_display_rotation_t app_prefs_get_display_rotation(void) { return s_disp_rot; }

void app_prefs_set_display_rotation(app_display_rotation_t r)
{
    // Coerce anything non-180 to 0 -- the toggle UI only writes valid values
    // but persistence migrations or hand-edited SD JSON could land arbitrary
    // numbers in here.
    app_display_rotation_t coerced = (r == APP_DISPLAY_ROTATION_180)
                                         ? APP_DISPLAY_ROTATION_180
                                         : APP_DISPLAY_ROTATION_0;
    if (!s_mutex || s_disp_rot == coerced) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_disp_rot = coerced;
    prefs_commit_locked(K_DISP_ROT);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

int app_prefs_get_channel_color(int ms_channel_id)
{
    int idx = find_color_idx(ms_channel_id);
    return (idx < 0) ? -1 : s_colors[idx].color;
}

void app_prefs_set_channel_color(int ms_channel_id, int index)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int existing = find_color_idx(ms_channel_id);
    if (index < 0) {
        if (existing >= 0) {
            for (size_t i = (size_t) existing + 1; i < s_color_count; ++i) {
                s_colors[i - 1] = s_colors[i];
            }
            s_color_count--;
        }
    } else if (index <= 7) {
        if (existing >= 0) {
            s_colors[existing].color = index;
        } else if (s_color_count < MAX_COLOR_ENTRIES) {
            s_colors[s_color_count].id    = ms_channel_id;
            s_colors[s_color_count].color = index;
            s_color_count++;
        }
    }
    prefs_commit_locked(K_CHAN_COLOR);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

uint8_t app_prefs_get_brightness_pct(void) { return s_brightness_pct; }

void app_prefs_set_brightness_pct(uint8_t pct)
{
    pct = clamp_brightness(pct);
    if (!s_mutex || s_brightness_pct == pct) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_brightness_pct = pct;
    prefs_commit_locked(K_BRIGHT);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

uint8_t app_prefs_get_selected_mix_index(void) { return s_selected_mix_index; }

void app_prefs_set_selected_mix_index(uint8_t idx)
{
    if (!s_mutex || s_selected_mix_index == idx) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_selected_mix_index = idx;
    prefs_commit_locked(K_SEL_MIX);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

bool app_prefs_get_wifi_use_static(void) { return s_wifi_static; }

void app_prefs_set_wifi_use_static(bool on)
{
    if (!s_mutex || s_wifi_static == on) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_wifi_static = on;
    prefs_commit_locked(K_WIFI_STATIC);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

// Shared body for the 4 IP/netmask/gw/dns string getters. The live state is
// stored in the matching s_wifi_* buffer; we copy out under the mutex so a
// concurrent setter can't truncate mid-read.
static const char *get_wifi_str_locked(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) return out;
    if (!s_mutex) { out[0] = '\0'; return out; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_ip_str(out, out_len, src);
    xSemaphoreGive(s_mutex);
    return out;
}

const char *app_prefs_get_wifi_static_ip(char *out, size_t out_len)
{ return get_wifi_str_locked(out, out_len, s_wifi_ip); }

const char *app_prefs_get_wifi_static_netmask(char *out, size_t out_len)
{ return get_wifi_str_locked(out, out_len, s_wifi_nm); }

const char *app_prefs_get_wifi_static_gateway(char *out, size_t out_len)
{ return get_wifi_str_locked(out, out_len, s_wifi_gw); }

const char *app_prefs_get_wifi_static_dns(char *out, size_t out_len)
{ return get_wifi_str_locked(out, out_len, s_wifi_dns); }

static void set_wifi_str(prefs_key_t k, char *dst, const char *src)
{
    if (!s_mutex || !src) return;
    if (strlen(src) >= APP_PREFS_IP_STR_MAX) return;  // reject overlong
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (strcmp(dst, src) == 0) { xSemaphoreGive(s_mutex); return; }
    copy_ip_str(dst, APP_PREFS_IP_STR_MAX, src);
    prefs_commit_locked(k);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

void app_prefs_set_wifi_static_ip      (const char *s) { set_wifi_str(K_WIFI_IP,  s_wifi_ip,  s); }
void app_prefs_set_wifi_static_netmask (const char *s) { set_wifi_str(K_WIFI_NM,  s_wifi_nm,  s); }
void app_prefs_set_wifi_static_gateway (const char *s) { set_wifi_str(K_WIFI_GW,  s_wifi_gw,  s); }
void app_prefs_set_wifi_static_dns     (const char *s) { set_wifi_str(K_WIFI_DNS, s_wifi_dns, s); }

// Bigger free-form strings (ntp server hostname, POSIX TZ). Same shape as
// set_wifi_str but uses APP_PREFS_STR_MAX as the cap.
static const char *get_big_str_locked(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) return out;
    if (!s_mutex) { out[0] = '\0'; return out; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy_str_bounded(out, out_len, src);
    xSemaphoreGive(s_mutex);
    return out;
}

static void set_big_str(prefs_key_t k, char *dst, const char *src)
{
    if (!s_mutex || !src) return;
    if (strlen(src) >= APP_PREFS_STR_MAX) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (strcmp(dst, src) == 0) { xSemaphoreGive(s_mutex); return; }
    copy_str_bounded(dst, APP_PREFS_STR_MAX, src);
    prefs_commit_locked(k);
    xSemaphoreGive(s_mutex);
    notify_subscribers();
}

const char *app_prefs_get_ntp_server(char *out, size_t out_len)
{ return get_big_str_locked(out, out_len, s_ntp_server); }

void app_prefs_set_ntp_server(const char *s)
{ set_big_str(K_NTP_SERVER, s_ntp_server, s); }

const char *app_prefs_get_display_tz(char *out, size_t out_len)
{ return get_big_str_locked(out, out_len, s_display_tz); }

void app_prefs_set_display_tz(const char *s)
{ set_big_str(K_TZ, s_display_tz, s); }

void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx)
{
    if (!cb || s_subscriber_count >= MAX_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb  = cb;
    s_subscribers[s_subscriber_count].ctx = ctx;
    s_subscriber_count++;
}

// ---------------------------------------------------------------------------
// Diagnostic dump (used by `prefs-dump` console command)
// ---------------------------------------------------------------------------

static void dump_bag(const char *label, const prefs_bag_t *bag, bool loaded)
{
    if (!loaded) {
        printf("  %s: <absent>\n", label);
        return;
    }
    printf("  %s:\n", label);
    if (bag->present[K_LVL_FMT])
        printf("    level_format     = %s   mt=%llu\n",
               level_format_to_str(bag->level_format),
               (unsigned long long) bag->mtime[K_LVL_FMT]);
    else printf("    level_format     = <absent>\n");

    if (bag->present[K_SIG_IND])
        printf("    signal_indicator = %s   mt=%llu\n",
               signal_indicator_to_str(bag->signal_ind),
               (unsigned long long) bag->mtime[K_SIG_IND]);
    else printf("    signal_indicator = <absent>\n");

    if (bag->present[K_THEME])
        printf("    theme            = %s   mt=%llu\n",
               theme_to_str(bag->theme),
               (unsigned long long) bag->mtime[K_THEME]);
    else printf("    theme            = <absent>\n");

    if (bag->present[K_DISP_ROT])
        printf("    display_rotation = %u   mt=%llu\n",
               (unsigned) bag->disp_rot,
               (unsigned long long) bag->mtime[K_DISP_ROT]);
    else printf("    display_rotation = <absent>\n");

    if (bag->present[K_CHAN_COLOR]) {
        printf("    channel_color    = (%u entries) mt=%llu\n",
               (unsigned) bag->color_count,
               (unsigned long long) bag->mtime[K_CHAN_COLOR]);
        for (size_t i = 0; i < bag->color_count; ++i) {
            printf("       ch=%d color=%d\n", bag->colors[i].id, bag->colors[i].color);
        }
    } else {
        printf("    channel_color    = <absent>\n");
    }

    if (bag->present[K_BRIGHT])
        printf("    brightness_pct   = %u   mt=%llu\n",
               (unsigned) bag->brightness_pct,
               (unsigned long long) bag->mtime[K_BRIGHT]);
    else printf("    brightness_pct   = <absent>\n");

    if (bag->present[K_SEL_MIX])
        printf("    selected_mix_idx = %u   mt=%llu\n",
               (unsigned) bag->selected_mix_index,
               (unsigned long long) bag->mtime[K_SEL_MIX]);
    else printf("    selected_mix_idx = <absent>\n");

    if (bag->present[K_WIFI_STATIC])
        printf("    wifi_use_static  = %s   mt=%llu\n",
               bag->wifi_static ? "true" : "false",
               (unsigned long long) bag->mtime[K_WIFI_STATIC]);
    else printf("    wifi_use_static  = <absent>\n");

    struct { const char *label; prefs_key_t pk; const char *val; } sk[] = {
        { "wifi_static_ip ", K_WIFI_IP,  bag->wifi_ip  },
        { "wifi_static_nm ", K_WIFI_NM,  bag->wifi_nm  },
        { "wifi_static_gw ", K_WIFI_GW,  bag->wifi_gw  },
        { "wifi_static_dns", K_WIFI_DNS, bag->wifi_dns },
    };
    for (size_t i = 0; i < sizeof(sk) / sizeof(sk[0]); ++i) {
        if (bag->present[sk[i].pk])
            printf("    %s  = '%s'   mt=%llu\n",
                   sk[i].label, sk[i].val,
                   (unsigned long long) bag->mtime[sk[i].pk]);
        else
            printf("    %s  = <absent>\n", sk[i].label);
    }

    if (bag->present[K_NTP_SERVER])
        printf("    ntp_server       = '%s'   mt=%llu\n",
               bag->ntp_server, (unsigned long long) bag->mtime[K_NTP_SERVER]);
    else printf("    ntp_server       = <absent>\n");
    if (bag->present[K_TZ])
        printf("    display_tz       = '%s'   mt=%llu\n",
               bag->display_tz, (unsigned long long) bag->mtime[K_TZ]);
    else printf("    display_tz       = <absent>\n");
}

void app_prefs_debug_dump(void)
{
    prefs_bag_t nvs_bag, sd_bag;
    nvs_load(&nvs_bag);
    bool sd_loaded = sd_load(&sd_bag);

    printf("prefs effective:\n");
    printf("  level_format     = %s   mt=%llu\n",
           level_format_to_str(s_level_format),
           (unsigned long long) s_mtime[K_LVL_FMT]);
    printf("  signal_indicator = %s   mt=%llu\n",
           signal_indicator_to_str(s_signal_ind),
           (unsigned long long) s_mtime[K_SIG_IND]);
    printf("  theme            = %s   mt=%llu\n",
           theme_to_str(s_theme),
           (unsigned long long) s_mtime[K_THEME]);
    printf("  display_rotation = %u   mt=%llu\n",
           (unsigned) s_disp_rot,
           (unsigned long long) s_mtime[K_DISP_ROT]);
    printf("  channel_color    = (%u entries) mt=%llu\n",
           (unsigned) s_color_count,
           (unsigned long long) s_mtime[K_CHAN_COLOR]);
    for (size_t i = 0; i < s_color_count; ++i) {
        printf("     ch=%d color=%d\n", s_colors[i].id, s_colors[i].color);
    }
    printf("  brightness_pct   = %u   mt=%llu\n",
           (unsigned) s_brightness_pct,
           (unsigned long long) s_mtime[K_BRIGHT]);
    printf("  selected_mix_idx = %u   mt=%llu\n",
           (unsigned) s_selected_mix_index,
           (unsigned long long) s_mtime[K_SEL_MIX]);
    printf("  wifi_use_static  = %s   mt=%llu\n",
           s_wifi_static ? "true" : "false",
           (unsigned long long) s_mtime[K_WIFI_STATIC]);
    printf("  wifi_static_ip   = '%s'   mt=%llu\n",
           s_wifi_ip,  (unsigned long long) s_mtime[K_WIFI_IP]);
    printf("  wifi_static_nm   = '%s'   mt=%llu\n",
           s_wifi_nm,  (unsigned long long) s_mtime[K_WIFI_NM]);
    printf("  wifi_static_gw   = '%s'   mt=%llu\n",
           s_wifi_gw,  (unsigned long long) s_mtime[K_WIFI_GW]);
    printf("  wifi_static_dns  = '%s'   mt=%llu\n",
           s_wifi_dns, (unsigned long long) s_mtime[K_WIFI_DNS]);
    printf("  ntp_server       = '%s'   mt=%llu\n",
           s_ntp_server, (unsigned long long) s_mtime[K_NTP_SERVER]);
    printf("  display_tz       = '%s'   mt=%llu\n",
           s_display_tz, (unsigned long long) s_mtime[K_TZ]);
    printf("  mtime_floor      = %llu\n", (unsigned long long) s_mtime_floor);

    dump_bag("nvs",  &nvs_bag, true);
    dump_bag("sd",   &sd_bag,  sd_loaded);
}
