#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

// Disk-logger for runtime forensics. Each call emits one timestamped line
// to a rolling text file on the SD card (`/sdcard/monmix-NNNN.log`).
// Files rotate at 256 KB; the newest 128 are kept (≈32 MB cap, plenty of
// margin against today's typical 32 GB cards).
//
// Designed for failures M2.5a's coredump pipeline misses: silent
// degradations, hangs, SDIO error storms, anything where the firmware
// kept running but stopped doing useful work. Per the original M2.5
// design rationale, "the latest entries on disk" is what matters; flush
// semantics aim for panic-survival without thrashing the card.
//
// All emit calls are non-blocking and safe from any task. If the internal
// queue is full the call drops the line and increments a dropped-count;
// the next successful flush emits a "[N events dropped]" summary so the
// loss is visible in the log itself.

void app_logd_init(void);

// `lvl` is one of 'T' (trace), 'I' (info), 'W' (warn), 'E' (error).
// Trace is gated by app_logd_get_trace(); the rest always write.
void app_logd_emit(const char *tag, char lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define APP_LOGD_T(tag, fmt, ...) app_logd_emit(tag, 'T', fmt, ##__VA_ARGS__)
#define APP_LOGD_I(tag, fmt, ...) app_logd_emit(tag, 'I', fmt, ##__VA_ARGS__)
#define APP_LOGD_W(tag, fmt, ...) app_logd_emit(tag, 'W', fmt, ##__VA_ARGS__)
#define APP_LOGD_E(tag, fmt, ...) app_logd_emit(tag, 'E', fmt, ##__VA_ARGS__)

// Runtime control of the trace gate. Persisted to NVS (namespace `monmix`,
// key `logtrace`) so the setting survives reboots. Default is ON — the
// expected use is to leave traces flowing and disable only if SD wear or
// log noise becomes a problem.
void app_logd_set_trace(bool on);
bool app_logd_get_trace(void);

// Log-line subscriber. Receives every formatted line (including the
// rolling `[ts] [tag] L ` prefix and trailing newline) that app_logd
// emits. Called from app_logd's worker task, NOT from arbitrary
// emit-call contexts -- so the callback can do moderate work without
// risking a deadlock with the caller. Must still be non-blocking;
// network senders that can't write immediately should drop the line
// or queue it themselves.
typedef void (*app_logd_subscriber_t)(const char *line, size_t len, void *ctx);
bool app_logd_subscribe  (app_logd_subscriber_t cb, void *ctx);
void app_logd_unsubscribe(app_logd_subscriber_t cb, void *ctx);
