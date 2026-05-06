// Per-path in-flight expectation tracker for OSC GET requests.
//
// Both prime resilience (UDP loss on initial GETs) and watchdog
// (MS-down detection) ride this single mechanism: register an
// expectation when sending a GET, clear it when the matching reply
// arrives, tick it periodically to retry stale slots and surface
// fully-failed watchdog entries.
//
// Pure-C, no LwIP / FreeRTOS / SDL deps. The clock is passed in by the
// caller (so it works in firmware via esp_timer_get_time, in the sim
// via SDL_GetTicks, and in unit tests via a fake counter). The retry
// path is a caller-provided send callback that takes the path + format
// and emits the OSC packet however the host code wants (LwIP raw udp,
// mongoose mg_send, a test-side recorder).
//
// Single-threaded -- caller is responsible for ensuring all calls into
// this module happen from one task. Both the firmware and sim OSC
// clients call from their worker thread only.

#ifndef MONMIX_OSC_EXPECT_H
#define MONMIX_OSC_EXPECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC_EXPECT_PATH_MAX      48
#define OSC_EXPECT_FLAG_WATCHDOG 0x01

typedef struct {
    char     path[OSC_EXPECT_PATH_MAX];   // empty (path[0]==0) when slot is free
    uint32_t sent_ms;
    uint8_t  retries_left;
    uint8_t  flags;
    char     fmt;                          // 'n' or 'v', preserved for retry
} osc_expect_slot_t;

// Send callback fired by osc_expect_tick when a slot's retry window
// expires and retries_left > 0. Caller-defined; implementation builds
// the wire packet (e.g. "/con/n/<path>" no-args) and emits it.
typedef void (*osc_expect_send_fn)(const char *path, char fmt, void *user);

typedef struct {
    osc_expect_slot_t  *slots;
    size_t              slot_count;
    uint32_t            timeout_ms;
    osc_expect_send_fn  send;
    void               *user;
} osc_expect_t;

// Wire up. The slot array is caller-owned (stays valid for the lifetime
// of this table); the table struct itself is also caller-owned.
void osc_expect_init(osc_expect_t *t,
                     osc_expect_slot_t *slots, size_t slot_count,
                     uint32_t timeout_ms,
                     osc_expect_send_fn send, void *user);

// Add an expectation. Returns true on success, false if the table is
// full -- caller's request still went out, just not tracked.
bool osc_expect_register(osc_expect_t *t,
                         const char *path, char fmt,
                         uint8_t retries, uint8_t flags,
                         uint32_t now_ms);

// Clear any expectation matching this dotted path. Coincidental
// broadcasts on the same path also count -- we treat any inbound from
// MS for a watched path as evidence of liveness.
void osc_expect_match(osc_expect_t *t, const char *dotted);

// Periodic sweep. Returns the count of WATCHDOG-flagged entries that
// fully failed this tick (caller acts on >0 to flip state DISCONNECTED).
// Plain entries just retry up to retries_left, then drop silently.
int osc_expect_tick(osc_expect_t *t, uint32_t now_ms);

// Clear every slot. Used on connection teardown so stale entries don't
// skew the next watchdog window.
void osc_expect_clear(osc_expect_t *t);

#endif
