#pragma once

#include "app_pp_client.h"

// Build the stage display under lvgl_port_lock. Wires observers to
// app_pp_state, the PP client iface (for conn-state icon), and app_wifi
// (for wifi-state icon). Safe to call once after app_display_init.
void app_ui_init(const app_pp_client_iface_t *pp);

// Replace the boot status line (only visible until the first PP update
// arrives). Used by the boot path to show "Connecting WiFi..." etc.
// Once the stage layout takes over, this is a no-op.
void app_ui_set_status(const char *text);
