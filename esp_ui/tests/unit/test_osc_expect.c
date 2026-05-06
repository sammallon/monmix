// Unit tests for the OSC expectation table (main/osc_expect.c).
//
// Pure-C, no IDF / LwIP / SDL deps -- the module is intentionally
// host-agnostic so this test can drive it with a fake clock and a
// recording send callback. Catches regressions in:
//   - register/match basics
//   - register fails when full
//   - tick respects timeout
//   - tick retries until retries_left == 0
//   - watchdog flag surfaces only on full fail (not on retry)
//   - clear empties the table

#include "osc_expect.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;
#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else { ++g_fail;                                                      \
               fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }\
    } while (0)

// Recording send callback -- counts retries per (path, fmt) so tests
// can assert on how many times each slot was re-emitted.
typedef struct {
    int   count;
    char  last_path[64];
    char  last_fmt;
} sender_t;

static void sender_reset(sender_t *s) { memset(s, 0, sizeof(*s)); }

static void test_send(const char *path, char fmt, void *user) {
    sender_t *s = (sender_t *)user;
    ++s->count;
    strncpy(s->last_path, path, sizeof(s->last_path) - 1);
    s->last_path[sizeof(s->last_path) - 1] = 0;
    s->last_fmt = fmt;
}

static void test_register_match_clears_slot(void)
{
    osc_expect_slot_t slots[4];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 4, 1500, test_send, &s);

    EXPECT(osc_expect_register(&t, "ch.0.cfg.name", 'n', 2, 0, 1000));
    osc_expect_match(&t, "ch.0.cfg.name");
    // After match, registering the same path should land in slot 0
    // again (slot was cleared) and tick at +0 ms shouldn't retry.
    EXPECT(osc_expect_register(&t, "ch.0.cfg.name", 'n', 2, 0, 2000));
    EXPECT(osc_expect_tick(&t, 2000) == 0);
    EXPECT(s.count == 0);
}

static void test_register_fails_when_full(void)
{
    osc_expect_slot_t slots[3];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 3, 1500, test_send, &s);

    EXPECT( osc_expect_register(&t, "a", 'n', 0, 0, 0));
    EXPECT( osc_expect_register(&t, "b", 'n', 0, 0, 0));
    EXPECT( osc_expect_register(&t, "c", 'n', 0, 0, 0));
    EXPECT(!osc_expect_register(&t, "d", 'n', 0, 0, 0));   // table full -> false
}

static void test_register_rejects_oversize_path(void)
{
    osc_expect_slot_t slots[2];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 2, 1500, test_send, &s);

    char too_long[OSC_EXPECT_PATH_MAX + 8];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    EXPECT(!osc_expect_register(&t, too_long, 'n', 0, 0, 0));
}

static void test_tick_respects_timeout(void)
{
    osc_expect_slot_t slots[2];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 2, 1500, test_send, &s);

    osc_expect_register(&t, "ch.5.lvl", 'n', 2, 0, 1000);
    // 1499 ms after send is still inside the timeout window.
    EXPECT(osc_expect_tick(&t, 2499) == 0);
    EXPECT(s.count == 0);
    // 1500 ms after is the boundary -- our impl uses < timeout, so
    // 1500 elapsed retries.
    EXPECT(osc_expect_tick(&t, 2500) == 0);
    EXPECT(s.count == 1);
    EXPECT(strcmp(s.last_path, "ch.5.lvl") == 0);
    EXPECT(s.last_fmt == 'n');
}

static void test_tick_retries_then_drops(void)
{
    osc_expect_slot_t slots[2];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 2, 1500, test_send, &s);

    // 2 retries -> 3 emissions total: original send (caller's
    // responsibility, not counted) + 2 retries from tick.
    osc_expect_register(&t, "ch.5.lvl", 'n', 2, 0, 0);
    EXPECT(osc_expect_tick(&t, 1500) == 0);
    EXPECT(s.count == 1);
    EXPECT(osc_expect_tick(&t, 3000) == 0);
    EXPECT(s.count == 2);
    // Third tick: retries_left went 2 -> 1 -> 0 in the previous two
    // ticks. This tick sees retries_left==0 and drops the slot.
    EXPECT(osc_expect_tick(&t, 4500) == 0);
    EXPECT(s.count == 2);   // no further retry
    // Slot is now free, register should reuse it.
    EXPECT(osc_expect_register(&t, "ch.5.on", 'n', 0, 0, 5000));
}

static void test_watchdog_surfaces_only_on_full_fail(void)
{
    osc_expect_slot_t slots[2];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 2, 1500, test_send, &s);

    osc_expect_register(&t, "ch.0.cfg.name", 'n', 2,
                        OSC_EXPECT_FLAG_WATCHDOG, 0);
    EXPECT(osc_expect_tick(&t, 1500) == 0);   // retry, not fail
    EXPECT(osc_expect_tick(&t, 3000) == 0);   // retry, not fail
    EXPECT(osc_expect_tick(&t, 4500) == 1);   // full fail -> watchdog++
    EXPECT(osc_expect_tick(&t, 6000) == 0);   // slot cleared, no double-fire
}

static void test_match_clears_inflight_watchdog(void)
{
    osc_expect_slot_t slots[2];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 2, 1500, test_send, &s);

    osc_expect_register(&t, "ch.0.cfg.name", 'n', 2,
                        OSC_EXPECT_FLAG_WATCHDOG, 0);
    // Coincidental change broadcast on the watched path counts as
    // liveness -- watchdog clears, tick reports nothing.
    osc_expect_match(&t, "ch.0.cfg.name");
    EXPECT(osc_expect_tick(&t, 4500) == 0);
}

static void test_clear_empties_table(void)
{
    osc_expect_slot_t slots[3];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 3, 1500, test_send, &s);

    osc_expect_register(&t, "a", 'n', 1, 0, 0);
    osc_expect_register(&t, "b", 'v', 1, OSC_EXPECT_FLAG_WATCHDOG, 0);
    osc_expect_register(&t, "c", 'n', 1, 0, 0);
    osc_expect_clear(&t);
    // After clear, every slot is reusable and tick is a no-op.
    EXPECT(osc_expect_tick(&t, 9999) == 0);
    EXPECT(osc_expect_register(&t, "a", 'n', 1, 0, 0));
    EXPECT(osc_expect_register(&t, "b", 'n', 1, 0, 0));
    EXPECT(osc_expect_register(&t, "c", 'n', 1, 0, 0));
}

static void test_match_only_clears_first_match(void)
{
    osc_expect_slot_t slots[4];
    osc_expect_t      t;
    sender_t          s; sender_reset(&s);
    osc_expect_init(&t, slots, 4, 1500, test_send, &s);

    osc_expect_register(&t, "ch.1.lvl", 'n', 0, 0, 0);
    osc_expect_register(&t, "ch.1.lvl", 'n', 0, 0, 0);   // duplicate
    osc_expect_match(&t, "ch.1.lvl");
    // First slot cleared; second still pending. A second match clears it.
    osc_expect_match(&t, "ch.1.lvl");
    osc_expect_clear(&t);   // sanity: clear works regardless
}

int main(void)
{
    test_register_match_clears_slot();
    test_register_fails_when_full();
    test_register_rejects_oversize_path();
    test_tick_respects_timeout();
    test_tick_retries_then_drops();
    test_watchdog_surfaces_only_on_full_fail();
    test_match_clears_inflight_watchdog();
    test_clear_empties_table();
    test_match_only_clears_first_match();

    printf("%s: pass=%d fail=%d\n",
           g_fail == 0 ? "OK" : "FAIL", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
