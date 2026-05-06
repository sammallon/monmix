#include "osc_expect.h"

#include <string.h>

void osc_expect_init(osc_expect_t *t,
                     osc_expect_slot_t *slots, size_t slot_count,
                     uint32_t timeout_ms,
                     osc_expect_send_fn send, void *user)
{
    t->slots      = slots;
    t->slot_count = slot_count;
    t->timeout_ms = timeout_ms;
    t->send       = send;
    t->user       = user;
    memset(slots, 0, sizeof(*slots) * slot_count);
}

bool osc_expect_register(osc_expect_t *t,
                         const char *path, char fmt,
                         uint8_t retries, uint8_t flags,
                         uint32_t now_ms)
{
    for (size_t i = 0; i < t->slot_count; ++i) {
        if (t->slots[i].path[0]) continue;
        // Defensive truncation: if path is longer than the slot, the
        // suffix gets clipped. The match path uses strncmp/strcmp so a
        // truncated entry would never clear; better to drop the
        // expectation than register one that can never resolve.
        if (strlen(path) >= sizeof(t->slots[i].path)) return false;
        strncpy(t->slots[i].path, path, sizeof(t->slots[i].path) - 1);
        t->slots[i].path[sizeof(t->slots[i].path) - 1] = 0;
        t->slots[i].sent_ms      = now_ms;
        t->slots[i].retries_left = retries;
        t->slots[i].flags        = flags;
        t->slots[i].fmt          = fmt;
        return true;
    }
    return false;
}

void osc_expect_match(osc_expect_t *t, const char *dotted)
{
    for (size_t i = 0; i < t->slot_count; ++i) {
        if (t->slots[i].path[0] == 0) continue;
        if (strcmp(t->slots[i].path, dotted) == 0) {
            t->slots[i].path[0] = 0;
            return;
        }
    }
}

int osc_expect_tick(osc_expect_t *t, uint32_t now_ms)
{
    int watchdog_failed = 0;
    for (size_t i = 0; i < t->slot_count; ++i) {
        if (t->slots[i].path[0] == 0) continue;
        if (now_ms - t->slots[i].sent_ms < t->timeout_ms) continue;
        if (t->slots[i].retries_left == 0) {
            if (t->slots[i].flags & OSC_EXPECT_FLAG_WATCHDOG) ++watchdog_failed;
            t->slots[i].path[0] = 0;
            continue;
        }
        // Retry. Caller's send fn re-emits the OSC packet; we just
        // refresh the slot's deadline.
        if (t->send) t->send(t->slots[i].path, t->slots[i].fmt, t->user);
        t->slots[i].retries_left--;
        t->slots[i].sent_ms = now_ms;
    }
    return watchdog_failed;
}

void osc_expect_clear(osc_expect_t *t)
{
    memset(t->slots, 0, sizeof(*t->slots) * t->slot_count);
}
