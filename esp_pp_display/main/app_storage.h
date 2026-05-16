#pragma once

#include <stdbool.h>

// Mount the on-board microSD slot (SDMMC slot 0, 1-bit, /sdcard).
// Returns true on success. Logs the failure mode and leaves the rest of the
// firmware running unchanged when the card is missing or unreadable —
// SD-dependent features (coredump persistence today, ring-log later) just
// stay no-op.
bool app_storage_init(void);

bool app_storage_is_mounted(void);

// Absolute path to the mount root (e.g. "/sdcard"). Stable across the
// process lifetime; safe to embed in fopen() paths.
const char *app_storage_mount_point(void);
