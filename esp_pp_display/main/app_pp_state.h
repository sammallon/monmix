#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ProPresenter state container. Single source of truth for what the UI
// should render. Populated by app_pp_client_tcp on inbound subscribe
// broadcasts; read by app_ui (and the power-management activity timer).
//
// Thread-safety: all setters and getters take an internal mutex. Readers
// pass a destination buffer that the implementation fills under lock;
// no pointers to internal state are exposed.

// Single slide -- current OR next. Buffers sized generously for hymn
// stanzas + chord-chart-equivalent notes; trimmed at the boundary so the
// state never grows unbounded.
#define APP_PP_SLIDE_TEXT_MAX  1024
#define APP_PP_SLIDE_NOTES_MAX 512
#define APP_PP_UUID_MAX        40

typedef struct {
    char text[APP_PP_SLIDE_TEXT_MAX];
    char notes[APP_PP_SLIDE_NOTES_MAX];
    char uuid[APP_PP_UUID_MAX];
    bool valid;   // false when PP sends current=null or next=null
} app_pp_slide_t;

typedef enum {
    APP_PP_TIMER_STOPPED = 0,
    APP_PP_TIMER_RUNNING,
    APP_PP_TIMER_OVERRUN,
} app_pp_timer_state_t;

#define APP_PP_TIMER_NAME_MAX  64
#define APP_PP_TIMER_TIME_MAX  16     // "hh:mm:ss" or "-hh:mm:ss"

typedef struct {
    char uuid[APP_PP_UUID_MAX];
    char name[APP_PP_TIMER_NAME_MAX];
    char time_str[APP_PP_TIMER_TIME_MAX];
    app_pp_timer_state_t state;
} app_pp_timer_t;

// Reasonable cap for stage use. PP exposes "timers/current" as an array;
// 8 entries covers segment + preshow + game + a few spares.
#define APP_PP_MAX_TIMERS 8

// Engineer-pushed text shown on stage screens. Bounded to a length that
// fits comfortably across the 1024-wide panel.
#define APP_PP_STAGE_MSG_MAX 256

typedef void (*app_pp_state_on_change_t)(void *ctx);

// Initialise the mutex + clear all state. Idempotent; safe to call once
// during boot before the TCP client starts.
void app_pp_state_init(void);

// --- Readers (caller-owned destination) ---

// Fills `out` with the current slide. Returns out->valid (false when PP
// reported current=null, i.e. nothing live).
bool app_pp_state_get_current_slide(app_pp_slide_t *out);

// Fills `out` with the next slide. Returns out->valid (false at end of
// presentation when PP reports next=null).
bool app_pp_state_get_next_slide(app_pp_slide_t *out);

// Copies up to `max` timers into `out`. Returns the number of valid
// entries (always <= APP_PP_MAX_TIMERS).
size_t app_pp_state_get_timers(app_pp_timer_t *out, size_t max);

// Copies the current stage message into `out`. Returns `out`. When no
// message is set the result is the empty string.
const char *app_pp_state_get_stage_message(char *out, size_t out_len);

// --- Writers (TCP task only) ---

// Each setter copies into internal state under the mutex, bumps the
// activity timestamp, and notifies registered observers exactly once.
// The observer callback fires from the caller's task -- subscribers that
// touch LVGL widgets must take lvgl_port_lock before doing so.
//
// `set_slides` is the combined setter: PP delivers current+next in a
// single `status/slide` broadcast, so we apply them atomically with
// one observer notification rather than firing twice in a row.
// Pass NULL for either side to mean "no slide" (PP's null).
void app_pp_state_set_slides(const app_pp_slide_t *current,
                             const app_pp_slide_t *next);
void app_pp_state_set_timers(const app_pp_timer_t *t, size_t n);
void app_pp_state_set_stage_message(const char *msg);

// --- Activity ---

// Milliseconds since boot at the last setter call. Used by power
// management to decide when to sleep the screen. 0 means "no update
// since boot".
uint64_t app_pp_state_last_update_ms(void);

// --- Observers ---

void app_pp_state_register_on_change(app_pp_state_on_change_t cb, void *ctx);
