#pragma once

#include <stddef.h>

// Cap on tracked channels. Si Expression 2 has 24 input channels; 24 covers
// the worst case where a musician wants every channel on a single device.
// Practical defaults are far smaller.
#define APP_CONFIG_MAX_CHANNELS 24

// Load the per-musician channel selection. On first boot (NVS empty) seeds
// NVS with a sensible default (12 channels, MS IDs 0..11) and uses it. On
// later boots, returns whatever was persisted.
//
// Must be called after nvs_flash_init. Always returns a valid list; if NVS
// itself fails, falls back to the in-memory default.
void app_config_init(void);

// The active channel list. The pointer remains valid until the next call to
// app_config_init. M4 will add a setter for runtime reconfiguration.
const int *app_config_channel_ids(size_t *out_count);
