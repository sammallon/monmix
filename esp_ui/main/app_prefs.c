#include "app_prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_storage.h"

static const char *TAG = "app_prefs";

// Channel color is sparse — only channels the user has explicitly tagged
// have an entry. `MAX_COLOR_ENTRIES` caps the table at a sane size; the
// 12-channel default config hits 12, room for double that.
#define MAX_COLOR_ENTRIES   32
#define PREFS_PATH          "/sdcard/monmix-prefs.json"
#define PREFS_TMP_PATH      "/sdcard/monmix-prefs.tmp"

typedef struct {
    int id;
    int color;     // 0..7
} color_entry_t;

static SemaphoreHandle_t       s_mutex;
static app_level_format_t      s_level_format = APP_LEVEL_FORMAT_NORM;
// Default to NONE — the current "signal-present" heuristic only reflects
// "channel is unmuted and has level set", which gives false positives in
// MS offline mode (and on real instances when audio isn't flowing). A
// real implementation requires subscribing to /console/metering/* and
// parsing the binary int16 stream — see the open follow-up task.
static app_signal_indicator_t  s_signal_ind   = APP_SIGNAL_INDICATOR_NONE;
static app_theme_t             s_theme        = APP_THEME_DARK;
static color_entry_t           s_colors[MAX_COLOR_ENTRIES];
static size_t                  s_color_count;

#define MAX_SUBSCRIBERS 4
static struct {
    app_prefs_on_change_t cb;
    void                 *ctx;
} s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count;

// ─────────────────────────────────────────────────────────────────────────
// helpers
// ─────────────────────────────────────────────────────────────────────────

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

static void notify_subscribers(void)
{
    for (size_t i = 0; i < s_subscriber_count; ++i) {
        if (s_subscribers[i].cb) {
            s_subscribers[i].cb(s_subscribers[i].ctx);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Disk I/O
// ─────────────────────────────────────────────────────────────────────────

static bool load_from_disk(void)
{
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
        ESP_LOGW(TAG, "prefs JSON parse failed; using defaults");
        return false;
    }

    cJSON *jlf = cJSON_GetObjectItem(root, "level_format");
    if (cJSON_IsString(jlf)) s_level_format = level_format_from_str(jlf->valuestring);

    cJSON *jsi = cJSON_GetObjectItem(root, "signal_indicator");
    if (cJSON_IsString(jsi)) s_signal_ind = signal_indicator_from_str(jsi->valuestring);

    cJSON *jth = cJSON_GetObjectItem(root, "theme");
    if (cJSON_IsString(jth)) s_theme = theme_from_str(jth->valuestring);

    s_color_count = 0;
    cJSON *jcc = cJSON_GetObjectItem(root, "channel_color");
    if (cJSON_IsObject(jcc)) {
        cJSON *kv = NULL;
        cJSON_ArrayForEach(kv, jcc) {
            if (!cJSON_IsNumber(kv) || s_color_count >= MAX_COLOR_ENTRIES) continue;
            int id    = atoi(kv->string ? kv->string : "-1");
            int color = (int) kv->valuedouble;
            if (id < 0 || color < 0 || color > 7) continue;
            s_colors[s_color_count].id    = id;
            s_colors[s_color_count].color = color;
            s_color_count++;
        }
    }

    cJSON_Delete(root);
    return true;
}

static bool save_to_disk_locked(void)
{
    if (!app_storage_is_mounted()) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "level_format",     level_format_to_str(s_level_format));
    cJSON_AddStringToObject(root, "signal_indicator", signal_indicator_to_str(s_signal_ind));
    cJSON_AddStringToObject(root, "theme",            theme_to_str(s_theme));
    cJSON *jcc = cJSON_AddObjectToObject(root, "channel_color");
    for (size_t i = 0; i < s_color_count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "%d", s_colors[i].id);
        cJSON_AddNumberToObject(jcc, key, s_colors[i].color);
    }

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
            // Atomic-ish swap: remove old, rename new. FATFS doesn't support
            // POSIX rename-overwrite, so we have to unlink first.
            remove(PREFS_PATH);
            if (rename(PREFS_TMP_PATH, PREFS_PATH) != 0) {
                ESP_LOGW(TAG, "rename %s → %s failed", PREFS_TMP_PATH, PREFS_PATH);
                ok = false;
            }
        }
    }
    free(buf);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────

void app_prefs_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool loaded = load_from_disk();
    xSemaphoreGive(s_mutex);
    if (loaded) {
        ESP_LOGI(TAG, "loaded prefs from %s (level=%s indicator=%s theme=%s colors=%u)",
                 PREFS_PATH,
                 level_format_to_str(s_level_format),
                 signal_indicator_to_str(s_signal_ind),
                 theme_to_str(s_theme),
                 (unsigned) s_color_count);
    } else {
        ESP_LOGI(TAG, "using default prefs (level=%s indicator=%s theme=%s)",
                 level_format_to_str(s_level_format),
                 signal_indicator_to_str(s_signal_ind),
                 theme_to_str(s_theme));
    }
}

app_level_format_t app_prefs_get_level_format(void)
{
    return s_level_format;
}

void app_prefs_set_level_format(app_level_format_t f)
{
    if (!s_mutex || s_level_format == f) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_level_format = f;
    bool ok = save_to_disk_locked();
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGW(TAG, "failed to persist level_format");
    notify_subscribers();
}

app_signal_indicator_t app_prefs_get_signal_indicator(void)
{
    return s_signal_ind;
}

void app_prefs_set_signal_indicator(app_signal_indicator_t s)
{
    if (!s_mutex || s_signal_ind == s) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_signal_ind = s;
    bool ok = save_to_disk_locked();
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGW(TAG, "failed to persist signal_indicator");
    notify_subscribers();
}

app_theme_t app_prefs_get_theme(void)
{
    return s_theme;
}

void app_prefs_set_theme(app_theme_t t)
{
    if (!s_mutex || s_theme == t) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_theme = t;
    bool ok = save_to_disk_locked();
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGW(TAG, "failed to persist theme");
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
        // Clear: shift left.
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
    bool ok = save_to_disk_locked();
    xSemaphoreGive(s_mutex);
    if (!ok) ESP_LOGW(TAG, "failed to persist channel_color");
    notify_subscribers();
}

void app_prefs_register_on_change(app_prefs_on_change_t cb, void *ctx)
{
    if (!cb || s_subscriber_count >= MAX_SUBSCRIBERS) return;
    s_subscribers[s_subscriber_count].cb  = cb;
    s_subscribers[s_subscriber_count].ctx = ctx;
    s_subscriber_count++;
}
