#pragma once

#include "app_ms_client.h"

void app_ui_init(const ms_client_iface_t *ms);

// Update the status line at the top of the screen. Safe to call from any
// task; uses lv_async_call internally. Calls made before app_ui_init runs
// are no-ops.
void app_ui_set_status(const char *text);
