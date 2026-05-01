#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static app_channel_t        s_channels[APP_CONFIG_MAX_CHANNELS];
static size_t               s_count;
static SemaphoreHandle_t    s_mutex;
static app_state_on_change_t s_on_change;
static void                 *s_on_change_ctx;

void app_state_init(const int *ids, size_t count)
{
    if (count > APP_CONFIG_MAX_CHANNELS) count = APP_CONFIG_MAX_CHANNELS;
    s_count = count;
    s_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < s_count; ++i) {
        s_channels[i].id = ids[i];
        // MS paths are 0-indexed: ch.0 displays as "CH 01" in the MS UI.
        // Show "ch N+1" placeholder until the scribble strip name arrives.
        snprintf(s_channels[i].name, sizeof(s_channels[i].name), "ch %d", ids[i] + 1);
        s_channels[i].level    = 0.0f;
        s_channels[i].level_db = -200.0f;   // sentinel: "below floor" until MS reports
        s_channels[i].mute     = false;
    }
}

size_t app_state_count(void)
{
    return s_count;
}

bool app_state_get(size_t idx, app_channel_t *out)
{
    if (idx >= s_count || !out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_channels[idx];
    xSemaphoreGive(s_mutex);
    return true;
}

void app_state_set_level(size_t idx, float level, bool notify)
{
    if (idx >= s_count) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_channels[idx].level = level;
    xSemaphoreGive(s_mutex);

    if (notify && s_on_change) {
        s_on_change(idx, s_on_change_ctx);
    }
}

void app_state_set_level_db(size_t idx, float db, bool notify)
{
    if (idx >= s_count) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_channels[idx].level_db = db;
    xSemaphoreGive(s_mutex);

    if (notify && s_on_change) {
        s_on_change(idx, s_on_change_ctx);
    }
}

void app_state_set_name(size_t idx, const char *name, bool notify)
{
    if (idx >= s_count || !name) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_channels[idx].name, name, sizeof(s_channels[idx].name) - 1);
    s_channels[idx].name[sizeof(s_channels[idx].name) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    if (notify && s_on_change) {
        s_on_change(idx, s_on_change_ctx);
    }
}

void app_state_set_mute(size_t idx, bool mute, bool notify)
{
    if (idx >= s_count) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (s_channels[idx].mute != mute);
    s_channels[idx].mute = mute;
    xSemaphoreGive(s_mutex);

    if (notify && changed && s_on_change) {
        s_on_change(idx, s_on_change_ctx);
    }
}

int app_state_idx_for_id(int ms_channel_id)
{
    for (size_t i = 0; i < s_count; ++i) {
        if (s_channels[i].id == ms_channel_id) return (int)i;
    }
    return -1;
}

int app_state_id_for_idx(size_t idx)
{
    if (idx >= s_count) return -1;
    return s_channels[idx].id;
}

void app_state_register_on_change(app_state_on_change_t cb, void *ctx)
{
    s_on_change     = cb;
    s_on_change_ctx = ctx;
}
