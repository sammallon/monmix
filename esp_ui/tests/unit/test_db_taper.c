// Unit tests for the dB taper in app_ms_client.h. Pure math, no IDF
// or LVGL deps -- compiles natively as a tiny exe under MSVC or gcc.
//
// What we're locking down:
//   - db <= APP_DB_MIN            -> pos == 0
//   - db >= APP_DB_MAX            -> pos == 1
//   - 0 dB                        -> pos ~= 0.76 (audio-taper unity)
//   - position_to_db(db_to_position(x)) round-trip within 0.5 dB
//   - the curve is monotonic
//
// Run via tests/unit/run.ps1 (Windows) or tests/unit/run.sh (Linux).

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// app_ms_client.h transitively pulls in app_prefs.h, which is itself
// pure-C with no IDF deps -- safe to include directly here.
#include "app_ms_client.h"

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        if (cond) { ++g_pass; }                                               \
        else { ++g_fail;                                                      \
               fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }\
    } while (0)

#define EXPECT_NEAR(a, b, tol)                                                \
    do {                                                                      \
        double _aa = (double)(a), _bb = (double)(b), _t = (double)(tol);     \
        if (fabs(_aa - _bb) <= _t) { ++g_pass; }                             \
        else { ++g_fail;                                                      \
               fprintf(stderr, "FAIL %s:%d  %g !~ %g (tol %g)\n",            \
                       __FILE__, __LINE__, _aa, _bb, _t); }                  \
    } while (0)

static void test_floor_and_ceiling(void) {
    EXPECT(app_db_to_position(APP_DB_MIN)        == 0.0f);
    EXPECT(app_db_to_position(APP_DB_MIN - 50.0f) == 0.0f);  // clamp below
    EXPECT(app_db_to_position(APP_DB_MAX)        == 1.0f);
    EXPECT(app_db_to_position(APP_DB_MAX + 50.0f) == 1.0f);  // clamp above

    EXPECT(app_position_to_db(0.0f)  == APP_DB_MIN);
    EXPECT(app_position_to_db(-0.5f) == APP_DB_MIN);          // clamp below
    EXPECT(app_position_to_db(1.0f)  == APP_DB_MAX);
    EXPECT(app_position_to_db(2.0f)  == APP_DB_MAX);          // clamp above
}

static void test_unity_at_zero_db(void) {
    // The whole point of this taper is that 0 dB lands near the ~75%
    // mark on the slider. The header comment claims 0 dB -> 0.75; lock
    // it in to within 0.02.
    float pos_at_zero = app_db_to_position(0.0f);
    EXPECT_NEAR(pos_at_zero, 0.75f, 0.02f);
}

static void test_round_trip(void) {
    // For every dB in steps of 1, db -> pos -> db should be within
    // 0.5 dB. The header claims 4-mul precision; this exercises that.
    for (float db = APP_DB_MIN + 1.0f; db <= APP_DB_MAX; db += 1.0f) {
        float pos = app_db_to_position(db);
        float rt  = app_position_to_db(pos);
        EXPECT_NEAR(rt, db, 0.5f);
    }
}

static void test_monotonic(void) {
    // Position must increase strictly with dB across the full range.
    float prev = -1.0f;
    for (float db = APP_DB_MIN; db <= APP_DB_MAX; db += 0.5f) {
        float pos = app_db_to_position(db);
        EXPECT(pos >= prev);
        prev = pos;
    }
    // Same in reverse.
    prev = APP_DB_MIN - 1.0f;
    for (float pos = 0.0f; pos <= 1.0f; pos += 0.01f) {
        float db = app_position_to_db(pos);
        EXPECT(db >= prev);
        prev = db;
    }
}

int main(void) {
    test_floor_and_ceiling();
    test_unity_at_zero_db();
    test_round_trip();
    test_monotonic();

    fprintf(stdout, "%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
