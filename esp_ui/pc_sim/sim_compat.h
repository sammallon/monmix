// Forced into every TU via /FI on MSVC and -include on gcc/clang. Bridges
// the GCC-isms and ESP-IDF placement attributes that the tablet code
// uses but the PC build can't honor.
#ifndef SIM_COMPAT_H
#define SIM_COMPAT_H

// MSVC doesn't grok GCC's __attribute__((...)) at all. The tablet build
// uses it sparingly — printf-format checks (app_logd.h) and packed
// structs (app_console.c). Stub to nothing on MSVC; let real GCC handle
// it on Linux.
#ifdef _MSC_VER
#define __attribute__(x)
#endif

// EXT_RAM_BSS_ATTR places a large bss in external PSRAM on the ESP32-P4.
// PC has plenty of regular bss and no concept of placement, so swallow it.
// Same goes for the rest of the IDF placement family — most are already
// in stubs/esp_attr.h, but EXT_RAM_BSS_ATTR is defined elsewhere in IDF
// and not in that header, so we redeclare here for safety.
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// FreeRTOS sleep helpers used in app_ui.c around the wifi-reconfigure
// and reboot flows. app_ui.c doesn't include freertos/FreeRTOS.h itself
// (it leans on transitive IDF includes), so the sim stubs under
// stubs/freertos/ never get a chance to define these — drop them in here
// where /FI guarantees the TU sees them.
#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static __inline void vTaskDelay(uint32_t ticks) { Sleep(ticks); }
#else
#include <unistd.h>
static inline void vTaskDelay(uint32_t ticks) { usleep((unsigned)ticks * 1000u); }
#endif
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
#endif

// MSVC ships localtime_s (reversed arg order) but not POSIX localtime_r.
// app_ui.c's clock_tick reads localtime_r(&now, &lt); shim it here.
#ifdef _MSC_VER
#include <time.h>
#include <string.h>
static __inline struct tm *localtime_r(const time_t *t, struct tm *out) {
    return (localtime_s(out, t) == 0) ? out : (struct tm *)0;
}

// MSVC's strftime asserts via the invalid-parameter handler on GNU-only
// format specifiers; app_ui.c's clock_tick uses %l (space-padded 12-hour
// without leading zero), which is a GNU extension. Translate to MSVC's
// %#I (12-hour with leading-zero stripped) before delegating.
static __inline size_t strftime_sim(char *buf, size_t maxsz, const char *fmt, const struct tm *tm) {
    char rewritten[128];
    size_t fi = 0, oi = 0;
    while (fmt[fi] && oi + 4 < sizeof(rewritten)) {
        if (fmt[fi] == '%' && fmt[fi + 1] == 'l') {
            rewritten[oi++] = '%';
            rewritten[oi++] = '#';
            rewritten[oi++] = 'I';
            fi += 2;
        } else {
            rewritten[oi++] = fmt[fi++];
        }
    }
    rewritten[oi] = 0;
    return strftime(buf, maxsz, rewritten, tm);
}
#define strftime strftime_sim
#endif

// ────────────────────────────────────────────────────────────────────────────
// /sdcard/... path hijack for the real main/app_prefs.c. The tablet mounts
// FATFS at /sdcard; the sim has nothing of the sort, so any fopen / remove
// / rename of a "/sdcard/..." path is rewritten to land inside
// pc_sim_state/sdcard/ in the cwd. app_prefs.c writes its SD-mirror JSON
// there without modification. mock_nvs.c sets SIM_COMPAT_NO_FOPEN_HIJACK
// before including this header so its own fopen calls (to a non-/sdcard
// path) bypass the wrapper.
#if !defined(SIM_COMPAT_NO_FOPEN_HIJACK)
#include <stdio.h>
#include <string.h>

static __inline FILE *sim_fopen(const char *path, const char *mode) {
    if (path && strncmp(path, "/sdcard/", 8) == 0) {
        char redirected[512];
        snprintf(redirected, sizeof(redirected), "pc_sim_state/sdcard/%s", path + 8);
        return fopen(redirected, mode);
    }
    return fopen(path, mode);
}

static __inline int sim_remove(const char *path) {
    if (path && strncmp(path, "/sdcard/", 8) == 0) {
        char redirected[512];
        snprintf(redirected, sizeof(redirected), "pc_sim_state/sdcard/%s", path + 8);
        return remove(redirected);
    }
    return remove(path);
}

static __inline int sim_rename(const char *oldp, const char *newp) {
    char r1[512], r2[512];
    const char *o = oldp, *n = newp;
    if (oldp && strncmp(oldp, "/sdcard/", 8) == 0) {
        snprintf(r1, sizeof(r1), "pc_sim_state/sdcard/%s", oldp + 8); o = r1;
    }
    if (newp && strncmp(newp, "/sdcard/", 8) == 0) {
        snprintf(r2, sizeof(r2), "pc_sim_state/sdcard/%s", newp + 8); n = r2;
    }
    return rename(o, n);
}

#define fopen  sim_fopen
#define remove sim_remove
#define rename sim_rename
#endif

#endif // SIM_COMPAT_H
