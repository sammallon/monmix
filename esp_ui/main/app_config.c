#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";

#define NVS_NAMESPACE "monmix"
#define NVS_KEY_CHAN  "chan_ids"

// 12 channels covers a typical stage spread (lead vox + 4 mics + drums + DIs)
// across two pages of 6. Easy to override by erasing NVS or by editing this
// list and reflashing — the M2 firmware ships without an on-device editor.
static const int s_default_ids[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
#define DEFAULT_COUNT (sizeof(s_default_ids) / sizeof(s_default_ids[0]))

static int    s_ids[APP_CONFIG_MAX_CHANNELS];
static size_t s_count;

static void use_defaults(void)
{
    s_count = DEFAULT_COUNT;
    memcpy(s_ids, s_default_ids, sizeof(s_default_ids));
}

static bool load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    size_t blob_len = sizeof(s_ids);
    err = nvs_get_blob(h, NVS_KEY_CHAN, s_ids, &blob_len);
    nvs_close(h);

    if (err != ESP_OK) return false;
    if (blob_len == 0 || blob_len % sizeof(int) != 0) {
        ESP_LOGW(TAG, "ignoring malformed NVS blob (%u bytes)", (unsigned)blob_len);
        return false;
    }
    s_count = blob_len / sizeof(int);
    if (s_count > APP_CONFIG_MAX_CHANNELS) s_count = APP_CONFIG_MAX_CHANNELS;
    return true;
}

static void seed_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_CHAN, s_ids, s_count * sizeof(int));
    if (err == ESP_OK) nvs_commit(h);
    else ESP_LOGW(TAG, "seed nvs failed: %s", esp_err_to_name(err));
    nvs_close(h);
}

void app_config_init(void)
{
    if (load_from_nvs()) {
        ESP_LOGI(TAG, "loaded %u channels from NVS", (unsigned)s_count);
        return;
    }
    use_defaults();
    seed_nvs();
    ESP_LOGI(TAG, "seeded NVS with %u default channels", (unsigned)s_count);
}

const int *app_config_channel_ids(size_t *out_count)
{
    if (out_count) *out_count = s_count;
    return s_ids;
}
