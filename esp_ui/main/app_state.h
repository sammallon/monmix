#pragma once

#include <stdbool.h>
#include <stddef.h>

#define APP_STATE_MAX_CHANNELS 3

typedef struct {
    int   id;
    char  name[32];
    float level;
} app_channel_t;

typedef void (*app_state_on_change_t)(size_t idx, void *ctx);

void   app_state_init(void);
size_t app_state_count(void);

bool app_state_get(size_t idx, app_channel_t *out);
void app_state_set_level(size_t idx, float level, bool notify);
void app_state_set_name(size_t idx, const char *name, bool notify);

int app_state_idx_for_id(int ms_channel_id);
int app_state_id_for_idx(size_t idx);

void app_state_register_on_change(app_state_on_change_t cb, void *ctx);
