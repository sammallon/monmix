#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"

typedef struct {
    int   id;
    char  name[32];
    float level;       // 0..1 normalised — what the slider tracks
    float level_db;    // dB value from MS — used by the dB readout
    bool  mute;
} app_channel_t;

typedef void (*app_state_on_change_t)(size_t idx, void *ctx);

// Seed app_state with the channel ID list owned by app_config. count must
// not exceed APP_CONFIG_MAX_CHANNELS — caller is expected to clamp.
void   app_state_init(const int *ids, size_t count);
size_t app_state_count(void);

bool app_state_get(size_t idx, app_channel_t *out);
void app_state_set_level(size_t idx, float level, bool notify);
void app_state_set_level_db(size_t idx, float db, bool notify);
void app_state_set_name(size_t idx, const char *name, bool notify);
void app_state_set_mute(size_t idx, bool mute, bool notify);

int app_state_idx_for_id(int ms_channel_id);
int app_state_id_for_idx(size_t idx);

// Swap two slots in the channel array. Used by the drag-to-reorder UI to
// keep app_state aligned with the working ids while the user is mid-drag,
// so live MS broadcasts indexed by id still land in the right slot.
void app_state_swap_slots(size_t a, size_t b);

void app_state_register_on_change(app_state_on_change_t cb, void *ctx);

// Master strip — the mix-bus output fader for the currently-selected mix.
// On Si Expression 2 these are channels 60..73 (Mix 01..14) etc; the
// channel id and the bus path differ from input strips (no `level` alias,
// uses `mix.lvl` instead of `levelData.<m>.lvl`), so it gets its own
// state slot rather than living in the per-fader array.
typedef void (*app_state_master_on_change_t)(void *ctx);

bool app_state_master_get(app_channel_t *out);
void app_state_master_set_id(int ms_channel_id);
void app_state_master_set_level(float level, bool notify);
void app_state_master_set_level_db(float db, bool notify);
void app_state_master_set_name(const char *name, bool notify);
void app_state_master_set_mute(bool mute, bool notify);

void app_state_master_register_on_change(app_state_master_on_change_t cb, void *ctx);
