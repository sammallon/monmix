#include "app_state.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const int s_initial_ids[APP_STATE_MAX_CHANNELS] = {1, 2, 3};

static app_channel_t        s_channels[APP_STATE_MAX_CHANNELS];
static SemaphoreHandle_t    s_mutex;
static app_state_on_change_t s_on_change;
static void                 *s_on_change_ctx;

void app_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < APP_STATE_MAX_CHANNELS; ++i) {
        s_channels[i].id = s_initial_ids[i];
        snprintf(s_channels[i].name, sizeof(s_channels[i].name), "ch %d", s_initial_ids[i]);
        s_channels[i].level = 0.0f;
    }
}

size_t app_state_count(void)
{
    return APP_STATE_MAX_CHANNELS;
}

bool app_state_get(size_t idx, app_channel_t *out)
{
    if (idx >= APP_STATE_MAX_CHANNELS || !out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_channels[idx];
    xSemaphoreGive(s_mutex);
    return true;
}

void app_state_set_level(size_t idx, float level, bool notify)
{
    if (idx >= APP_STATE_MAX_CHANNELS) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_channels[idx].level = level;
    xSemaphoreGive(s_mutex);

    if (notify && s_on_change) {
        s_on_change(idx, s_on_change_ctx);
    }
}

void app_state_set_name(size_t idx, const char *name)
{
    if (idx >= APP_STATE_MAX_CHANNELS || !name) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_channels[idx].name, name, sizeof(s_channels[idx].name) - 1);
    s_channels[idx].name[sizeof(s_channels[idx].name) - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

int app_state_idx_for_id(int ms_channel_id)
{
    for (size_t i = 0; i < APP_STATE_MAX_CHANNELS; ++i) {
        if (s_channels[i].id == ms_channel_id) return (int)i;
    }
    return -1;
}

int app_state_id_for_idx(size_t idx)
{
    if (idx >= APP_STATE_MAX_CHANNELS) return -1;
    return s_channels[idx].id;
}

void app_state_register_on_change(app_state_on_change_t cb, void *ctx)
{
    s_on_change     = cb;
    s_on_change_ctx = ctx;
}
