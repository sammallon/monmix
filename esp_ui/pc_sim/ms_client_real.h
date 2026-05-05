#ifndef PC_SIM_MS_CLIENT_REAL_H
#define PC_SIM_MS_CLIENT_REAL_H

#include "app_ms_client.h"

// Spin up the real Mixing Station client against host:port and return its
// ms_client_iface_t. The returned interface is owned by the module; do not
// free. Internally starts a worker thread running mongoose's event loop.
const ms_client_iface_t *ms_client_real_create(const char *host, int port);

#endif
