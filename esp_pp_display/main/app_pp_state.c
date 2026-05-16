#include "app_pp_state.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_pp_state";

#define MAX_OBSERVERS 8

typedef struct {
    app_pp_state_on_change_t cb;
    void *ctx;
} observer_t;

static SemaphoreHandle_t s_mu;
static app_pp_slide_t   s_current;
static app_pp_slide_t   s_next;
static app_pp_timer_t   s_timers[APP_PP_MAX_TIMERS];
static size_t           s_timer_count;
static char             s_stage_msg[APP_PP_STAGE_MSG_MAX];
static observer_t       s_obs[MAX_OBSERVERS];
static size_t           s_obs_count;
// Activity timestamp. Mutex-protected (same mu as state) rather than
// _Atomic uint64_t -- the latter compiles to libatomic calls on RV32 and
// the cost of a mutex acquisition per ~1 Hz write is trivially less than
// the cost of worrying about whether the toolchain emitted a correct
// 64-bit atomic store.
static uint64_t         s_last_update_ms;

static void notify_all(void)
{
    // Snapshot the observer list under the lock; fire callbacks outside
    // it so subscribers can re-enter the state API without deadlocking.
    observer_t snap[MAX_OBSERVERS];
    size_t     n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    n = s_obs_count;
    if (n > MAX_OBSERVERS) n = MAX_OBSERVERS;
    memcpy(snap, s_obs, n * sizeof(observer_t));
    xSemaphoreGive(s_mu);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].cb) snap[i].cb(snap[i].ctx);
    }
}

static void bump_activity(void)
{
    uint64_t now_ms = (uint64_t) (esp_timer_get_time() / 1000);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_last_update_ms = now_ms;
    xSemaphoreGive(s_mu);
}

void app_pp_state_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    memset(&s_current, 0, sizeof(s_current));
    memset(&s_next,    0, sizeof(s_next));
    memset(s_timers,   0, sizeof(s_timers));
    s_timer_count = 0;
    s_stage_msg[0] = '\0';
    s_obs_count    = 0;
    s_last_update_ms = 0;
}

// --- Readers ---

bool app_pp_state_get_current_slide(app_pp_slide_t *out)
{
    if (!out) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_current;
    xSemaphoreGive(s_mu);
    return out->valid;
}

bool app_pp_state_get_next_slide(app_pp_slide_t *out)
{
    if (!out) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_next;
    xSemaphoreGive(s_mu);
    return out->valid;
}

size_t app_pp_state_get_timers(app_pp_timer_t *out, size_t max)
{
    if (!out || max == 0) return 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    size_t n = s_timer_count;
    if (n > max) n = max;
    memcpy(out, s_timers, n * sizeof(app_pp_timer_t));
    xSemaphoreGive(s_mu);
    return n;
}

const char *app_pp_state_get_stage_message(char *out, size_t out_len)
{
    if (!out || out_len == 0) return out;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strncpy(out, s_stage_msg, out_len - 1);
    out[out_len - 1] = '\0';
    xSemaphoreGive(s_mu);
    return out;
}

// --- Writers ---

void app_pp_state_set_slides(const app_pp_slide_t *current,
                             const app_pp_slide_t *next)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (current) s_current = *current;
    else         memset(&s_current, 0, sizeof(s_current));
    if (next)    s_next = *next;
    else         memset(&s_next, 0, sizeof(s_next));
    xSemaphoreGive(s_mu);
    bump_activity();
    notify_all();
}

void app_pp_state_set_timers(const app_pp_timer_t *t, size_t n)
{
    if (n > APP_PP_MAX_TIMERS) n = APP_PP_MAX_TIMERS;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (t && n > 0) {
        memcpy(s_timers, t, n * sizeof(app_pp_timer_t));
    }
    if (n < APP_PP_MAX_TIMERS) {
        memset(&s_timers[n], 0, (APP_PP_MAX_TIMERS - n) * sizeof(app_pp_timer_t));
    }
    s_timer_count = n;
    xSemaphoreGive(s_mu);
    bump_activity();
    notify_all();
}

void app_pp_state_set_stage_message(const char *msg)
{
    const char *src = msg ? msg : "";
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strncpy(s_stage_msg, src, sizeof(s_stage_msg) - 1);
    s_stage_msg[sizeof(s_stage_msg) - 1] = '\0';
    xSemaphoreGive(s_mu);
    bump_activity();
    notify_all();
}

// --- Activity ---

uint64_t app_pp_state_last_update_ms(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    uint64_t v = s_last_update_ms;
    xSemaphoreGive(s_mu);
    return v;
}

// --- Observers ---

void app_pp_state_register_on_change(app_pp_state_on_change_t cb, void *ctx)
{
    if (!cb) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_obs_count < MAX_OBSERVERS) {
        s_obs[s_obs_count].cb  = cb;
        s_obs[s_obs_count].ctx = ctx;
        s_obs_count++;
    } else {
        ESP_LOGW(TAG, "observer table full (%d max)", MAX_OBSERVERS);
    }
    xSemaphoreGive(s_mu);
}
