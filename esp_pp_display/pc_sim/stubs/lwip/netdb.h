// Stub of lwip/netdb.h. app_ui.c includes this header only because the
// SNTP wrapper transitively pulls it in on the tablet build; no socket
// calls land in app_ui.c itself, so this just needs to be a valid
// include with no symbols.
#pragma once
