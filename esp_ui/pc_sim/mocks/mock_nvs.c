// File-backed NVS for the sim. Every set immediately serializes the
// in-memory key tree to pc_sim_state/nvs.json so prefs survive across
// sim relaunches. The persistence model mirrors the tablet: same key
// names, same types (u8/u16/u32/u64/str/blob), so main/app_prefs.c can
// be compiled unchanged on top of this.
//
// Encoding choices: numbers ride as JSON numbers tagged with the C type
// in a sibling field so we round-trip width-correctly (a u8 read of a
// previously-stored u16 is still ESP_OK with the value truncated, which
// is faithful to ESP-IDF's NVS behaviour for compatible widths). Blobs
// are hex strings; that's compact enough for our tiny channel-color
// blob and doesn't introduce a base64 dependency.
//
// Threading: the same global mutex guards the in-memory tree and the
// disk write. app_prefs.c only ever opens NVS from the LVGL task today,
// but that may change.

// We use the real fopen here, not the sim_compat /sdcard hijack, since
// our NVS path isn't /sdcard-prefixed.
#define SIM_COMPAT_NO_FOPEN_HIJACK 1

#include "nvs.h"

#include <SDL.h>
#include <cJSON.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NVS_PATH "pc_sim_state/nvs.json"

typedef struct {
    char        ns[32];
    bool        in_use;
    bool        readonly;
} nvs_handle_state_t;

#define MAX_HANDLES 8

static SDL_mutex          *g_mtx;
static nvs_handle_state_t  g_handles[MAX_HANDLES];
static cJSON              *g_root;          // top-level: {namespace: {key: {type, value}}}
static bool                g_inited;

static void mkdir_if_needed(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void ensure_inited(void) {
    if (g_inited) return;
    g_inited = true;
    g_mtx = SDL_CreateMutex();

    mkdir_if_needed("pc_sim_state");

    FILE *f = fopen(NVS_PATH, "rb");
    if (!f) { g_root = cJSON_CreateObject(); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
        buf[sz] = 0;
        g_root = cJSON_Parse(buf);
    }
    free(buf);
    fclose(f);
    if (!g_root) g_root = cJSON_CreateObject();
}

static cJSON *ns_object(const char *ns, bool create) {
    cJSON *o = cJSON_GetObjectItem(g_root, ns);
    if (!o && create) {
        o = cJSON_CreateObject();
        cJSON_AddItemToObject(g_root, ns, o);
    }
    return o;
}

static void persist_locked(void) {
    char *txt = cJSON_Print(g_root);
    if (!txt) return;
    FILE *f = fopen(NVS_PATH, "wb");
    if (f) { fwrite(txt, 1, strlen(txt), f); fclose(f); }
    free(txt);
}

static nvs_handle_state_t *handle_state(nvs_handle_t h) {
    if (h == 0 || h > MAX_HANDLES) return NULL;
    nvs_handle_state_t *s = &g_handles[h - 1];
    return s->in_use ? s : NULL;
}

// ───────────────────────────────────────────────────────────────────────────
// nvs API
// ───────────────────────────────────────────────────────────────────────────

esp_err_t nvs_flash_init(void) { ensure_inited(); return ESP_OK; }
esp_err_t nvs_flash_erase(void) {
    ensure_inited();
    SDL_LockMutex(g_mtx);
    cJSON_Delete(g_root);
    g_root = cJSON_CreateObject();
    persist_locked();
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    ensure_inited();
    SDL_LockMutex(g_mtx);
    for (int i = 0; i < MAX_HANDLES; ++i) {
        if (!g_handles[i].in_use) {
            g_handles[i].in_use   = true;
            g_handles[i].readonly = (mode == NVS_READONLY);
            snprintf(g_handles[i].ns, sizeof(g_handles[i].ns), "%s", ns);
            *out = (nvs_handle_t)(i + 1);
            // Make sure the namespace exists for RW; for RO, app_prefs.c
            // tolerates ESP_ERR_NVS_NOT_FOUND on read.
            if (mode == NVS_READWRITE) ns_object(ns, true);
            SDL_UnlockMutex(g_mtx);
            return ESP_OK;
        }
    }
    SDL_UnlockMutex(g_mtx);
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t h) {
    SDL_LockMutex(g_mtx);
    nvs_handle_state_t *s = handle_state(h);
    if (s) s->in_use = false;
    SDL_UnlockMutex(g_mtx);
}

static esp_err_t set_typed_locked(nvs_handle_t h, const char *key,
                                   const char *type, cJSON *value_node) {
    nvs_handle_state_t *s = handle_state(h);
    if (!s || s->readonly) { cJSON_Delete(value_node); return ESP_FAIL; }
    cJSON *ns = ns_object(s->ns, true);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type", type);
    cJSON_AddItemToObject(entry, "value", value_node);
    cJSON_DeleteItemFromObject(ns, key);
    cJSON_AddItemToObject(ns, key, entry);
    return ESP_OK;
}

static esp_err_t set_unum(nvs_handle_t h, const char *key, uint64_t v, const char *type) {
    SDL_LockMutex(g_mtx);
    esp_err_t rc = set_typed_locked(h, key, type, cJSON_CreateNumber((double)v));
    if (rc == ESP_OK) persist_locked();
    SDL_UnlockMutex(g_mtx);
    return rc;
}

esp_err_t nvs_set_u8 (nvs_handle_t h, const char *k, uint8_t  v) { return set_unum(h, k, v, "u8");  }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return set_unum(h, k, v, "u16"); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return set_unum(h, k, v, "u32"); }
esp_err_t nvs_set_u64(nvs_handle_t h, const char *k, uint64_t v) { return set_unum(h, k, v, "u64"); }

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    SDL_LockMutex(g_mtx);
    esp_err_t rc = set_typed_locked(h, key, "str", cJSON_CreateString(value ? value : ""));
    if (rc == ESP_OK) persist_locked();
    SDL_UnlockMutex(g_mtx);
    return rc;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length) {
    char *hex = (char *)calloc(length * 2 + 1, 1);  // zero-fill so length=0 still gives ""
    if (!hex) return ESP_FAIL;
    for (size_t i = 0; i < length; ++i) {
        snprintf(hex + i * 2, 3, "%02x", ((const uint8_t *)value)[i]);
    }
    SDL_LockMutex(g_mtx);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type",   "blob");
    cJSON_AddNumberToObject(entry, "length", (double)length);
    cJSON_AddStringToObject(entry, "value",  hex);
    nvs_handle_state_t *s = handle_state(h);
    esp_err_t rc = ESP_FAIL;
    if (s && !s->readonly) {
        cJSON *ns = ns_object(s->ns, true);
        cJSON_DeleteItemFromObject(ns, key);
        cJSON_AddItemToObject(ns, key, entry);
        persist_locked();
        rc = ESP_OK;
    } else {
        cJSON_Delete(entry);
    }
    SDL_UnlockMutex(g_mtx);
    free(hex);
    return rc;
}

static cJSON *get_entry_locked(nvs_handle_t h, const char *key) {
    nvs_handle_state_t *s = handle_state(h);
    if (!s) return NULL;
    cJSON *ns = ns_object(s->ns, false);
    if (!ns) return NULL;
    return cJSON_GetObjectItem(ns, key);
}

static esp_err_t get_unum(nvs_handle_t h, const char *key, uint64_t *out) {
    SDL_LockMutex(g_mtx);
    cJSON *entry = get_entry_locked(h, key);
    if (!entry) { SDL_UnlockMutex(g_mtx); return ESP_ERR_NVS_NOT_FOUND; }
    cJSON *v = cJSON_GetObjectItem(entry, "value");
    if (!cJSON_IsNumber(v)) { SDL_UnlockMutex(g_mtx); return ESP_FAIL; }
    *out = (uint64_t)v->valuedouble;
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

esp_err_t nvs_get_u8 (nvs_handle_t h, const char *k, uint8_t  *out) { uint64_t v; esp_err_t r = get_unum(h, k, &v); if (r == ESP_OK) *out = (uint8_t) v; return r; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *out) { uint64_t v; esp_err_t r = get_unum(h, k, &v); if (r == ESP_OK) *out = (uint16_t)v; return r; }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *out) { uint64_t v; esp_err_t r = get_unum(h, k, &v); if (r == ESP_OK) *out = (uint32_t)v; return r; }
esp_err_t nvs_get_u64(nvs_handle_t h, const char *k, uint64_t *out) { return get_unum(h, k, out); }

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *length) {
    SDL_LockMutex(g_mtx);
    cJSON *entry = get_entry_locked(h, key);
    if (!entry) { SDL_UnlockMutex(g_mtx); return ESP_ERR_NVS_NOT_FOUND; }
    cJSON *v = cJSON_GetObjectItem(entry, "value");
    if (!cJSON_IsString(v)) { SDL_UnlockMutex(g_mtx); return ESP_FAIL; }
    size_t need = strlen(v->valuestring) + 1;
    if (out == NULL) {
        // Probe call: ESP-IDF NVS reports the required length.
        if (length) *length = need;
        SDL_UnlockMutex(g_mtx);
        return ESP_OK;
    }
    if (!length || *length < need) {
        if (length) *length = need;
        SDL_UnlockMutex(g_mtx);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, v->valuestring, need);
    *length = need;
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length) {
    SDL_LockMutex(g_mtx);
    cJSON *entry = get_entry_locked(h, key);
    if (!entry) { SDL_UnlockMutex(g_mtx); return ESP_ERR_NVS_NOT_FOUND; }
    cJSON *jl = cJSON_GetObjectItem(entry, "length");
    cJSON *jv = cJSON_GetObjectItem(entry, "value");
    if (!cJSON_IsNumber(jl) || !cJSON_IsString(jv)) { SDL_UnlockMutex(g_mtx); return ESP_FAIL; }
    size_t blen = (size_t)jl->valuedouble;
    if (out == NULL) {
        if (length) *length = blen;
        SDL_UnlockMutex(g_mtx);
        return ESP_OK;
    }
    if (!length || *length < blen) {
        if (length) *length = blen;
        SDL_UnlockMutex(g_mtx);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    const char *hex = jv->valuestring;
    for (size_t i = 0; i < blen; ++i) {
        unsigned int byte;
        sscanf(hex + i * 2, "%02x", &byte);
        ((uint8_t *)out)[i] = (uint8_t)byte;
    }
    *length = blen;
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    SDL_LockMutex(g_mtx);
    nvs_handle_state_t *s = handle_state(h);
    if (!s || s->readonly) { SDL_UnlockMutex(g_mtx); return ESP_FAIL; }
    cJSON *ns = ns_object(s->ns, false);
    if (ns) cJSON_DeleteItemFromObject(ns, key);
    persist_locked();
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h) {
    SDL_LockMutex(g_mtx);
    nvs_handle_state_t *s = handle_state(h);
    if (!s || s->readonly) { SDL_UnlockMutex(g_mtx); return ESP_FAIL; }
    cJSON_DeleteItemFromObject(g_root, s->ns);
    persist_locked();
    SDL_UnlockMutex(g_mtx);
    return ESP_OK;
}

// Set/get already persist immediately; nvs_commit is just a no-op +
// resync barrier for the file-backed store.
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
