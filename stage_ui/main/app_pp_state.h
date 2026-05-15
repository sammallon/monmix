// app_pp_state — in-memory store of the ProPresenter-side state we
// render: current + next slide payloads, active presentation title,
// system time string, current countdown timer, stage message.
//
// Same shape as esp_ui/main/app_state.h: pure data + a subscriber-notify
// callback list. The state module knows nothing about HTTP or protocol;
// app_pp_client populates it, app_pp_ui reads from it.

#ifndef APP_PP_STATE_H
#define APP_PP_STATE_H

#include <stdbool.h>
#include <stddef.h>

// One slide as it lands from /v1/status/slide. Text is the human-
// readable payload (line-broken at \n on input). image_uuid is reserved
// for the later gallery feature — primary display renders text only.
#define APP_PP_SLIDE_TEXT_MAX 1024
#define APP_PP_SLIDE_UUID_MAX 64

typedef struct {
    bool present;
    char uuid[APP_PP_SLIDE_UUID_MAX];
    char image_uuid[APP_PP_SLIDE_UUID_MAX];
    char text[APP_PP_SLIDE_TEXT_MAX];
} app_pp_slide_t;

#define APP_PP_TITLE_MAX 128
typedef struct {
    bool present;
    char title[APP_PP_TITLE_MAX];
    int  current_index;     // -1 if unknown
    int  total_slides;      // 0 if unknown
} app_pp_presentation_info_t;

#define APP_PP_STAGE_MSG_MAX 256
typedef struct {
    bool present;
    char text[APP_PP_STAGE_MSG_MAX];
} app_pp_stage_message_t;

// Simplest possible clock + countdown surface for the skeleton. Real
// shape (multiple named timers etc.) lands when the live client lands.
typedef struct {
    bool present;
    char wall_clock[16];     // "HH:MM:SS"
    char countdown[16];      // "T-MM:SS" or empty
} app_pp_timing_t;

typedef void (*app_pp_state_on_change_t)(void *ctx);

void app_pp_state_init(void);
void app_pp_state_subscribe(app_pp_state_on_change_t cb, void *ctx);

// Setters — populated by the client. Each setter copies + notifies.
void app_pp_state_set_current_slide  (const app_pp_slide_t *s);
void app_pp_state_set_next_slide     (const app_pp_slide_t *s);
void app_pp_state_set_presentation   (const app_pp_presentation_info_t *p);
void app_pp_state_set_stage_message  (const app_pp_stage_message_t *m);
void app_pp_state_set_timing         (const app_pp_timing_t *t);

// Getters — return pointers to the internal store; valid until the next
// matching setter call. UI is expected to read on the LVGL task only.
const app_pp_slide_t             *app_pp_state_current_slide(void);
const app_pp_slide_t             *app_pp_state_next_slide(void);
const app_pp_presentation_info_t *app_pp_state_presentation(void);
const app_pp_stage_message_t     *app_pp_state_stage_message(void);
const app_pp_timing_t            *app_pp_state_timing(void);

#endif // APP_PP_STATE_H
