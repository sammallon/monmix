// Forced into every TU via /FI (MSVC) and -include (gcc/clang). Smaller
// than esp_ui's sim_compat.h because the stage_ui skeleton doesn't pull
// any ESP-IDF source yet — the platform-module ports land in the
// hardware round. This is intentionally a near-empty stub kept around
// so the build flag and the include path are wired correctly from day
// one; future modules can rely on it being there without retro-fitting.

#ifndef SIM_COMPAT_H
#define SIM_COMPAT_H

#ifdef _MSC_VER
#define __attribute__(x)
#endif

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#endif // SIM_COMPAT_H
