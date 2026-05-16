#pragma once

#include "app_pp_client.h"
#include <stdint.h>

// Start the real PP client. Spawns an SDL_Thread that connects to
// host:port over TCP and feeds broadcasts into app_pp_state. Returns
// the iface so pc_main can pass it to app_ui_init.
const app_pp_client_iface_t *pp_client_real(const char *host, uint16_t port);
