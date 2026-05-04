#include "app_ui.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_prefs.h"
#include "app_state.h"
#include "app_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

// Layout:
//   1024x600 panel (LVGL software-rotated 180°). Each "page" is one tile in
//   an lv_tileview. 12 faders fit across 1024 px (~85 px slot, 72 px box):
//   the default 12-channel config sits on a single page so a musician sees
//   their full mix at a glance. Reconfiguring to >12 channels overflows
//   into a 2nd page and the indicator dots show up automatically.
#define FADERS_PER_PAGE     12
#define MAX_PAGES           ((APP_CONFIG_MAX_CHANNELS + FADERS_PER_PAGE - 1) / FADERS_PER_PAGE)

#define SCREEN_W            1024
#define SCREEN_H            600
#define STATUS_H            32
#define INDICATOR_H         28
#define TILEVIEW_Y          STATUS_H
#define TILEVIEW_H          (SCREEN_H - STATUS_H - INDICATOR_H)
#define FADER_BOX_W         72
#define FADER_BOX_H         500
#define FADER_BOX_PAD       8
#define SLIDER_W            28
// Slider is centered in the box. Vertical budget within the 484 px inner
// area, top-to-bottom: name label (~20) + slider (centered) + mute button
// (32 at bottom-mid offset -28) + value label (~20 at bottom). The LVGL
// slider knob is bigger than the math math (the styled thumb extends past
// the track by more than its bare radius), so even SLIDER_H=320 left the
// knob kissing the MUTE button at value=0. 300 leaves an unambiguous
// gap; we lose 20 px of slider travel which is invisible at the 0–100
// integer scale we display.
#define SLIDER_H            300
#define MUTE_BTN_W          50
#define MUTE_BTN_H          32
#define DOT_SIZE            12
#define DOT_GAP             10

typedef struct {
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
    lv_obj_t *btn_mute;
    lv_obj_t *signal_dot;
} fader_widgets_t;

#define SIGNAL_DOT_SIZE     10
#define DEFAULT_SLIDER_HEX  0x4080E0   // medium blue when no per-channel color set
// Mid-grey for the "no color set" swatch and the picker's clear cell. 0x808080
// reads as a neutral indicator on both dark and light themes (the previous
// 0x303030 blended into the dark-theme row backgrounds).
#define NO_COLOR_SWATCH_HEX 0x808080

// Norm position (0..1) on the slider that corresponds to 0 dB / unity gain.
// Si Expression 2's level range is -138..+10 dB but the curve is non-linear
// (typical PA fader taper), so we can't compute this from the endpoints.
// 0.76 matches the position users typically see for unity on digital-fader
// curves. Replace with a runtime query of `/convert/ch.0.mix.lvl/vton/0`
// once the HTTP client lands (see task #40).
#define NORM_AT_0DB         0.76f

// 8-color palette for the per-channel color tag — see app_prefs / set-color.
// Roughly evenly spaced around the hue wheel; chose hex values that read
// well on both the default (light box) and the future low-light theme.
// Applied to the slider's filled indicator + knob, so the channel's
// identity is visible at a glance from across the stage.
static const uint32_t COLOR_PALETTE[8] = {
    0xE04040,  // red
    0xE09040,  // orange
    0xE0D040,  // yellow
    0x40C040,  // green
    0x40C0E0,  // cyan
    0x4080E0,  // blue
    0xC060E0,  // purple
    0xE060A0,  // pink
};

// Per-channel widget arrays move to PSRAM (EXT_RAM_BSS_ATTR) so we can
// scale the channel cap without eating internal SRAM that's already tight
// (the FreeRTOS timer-task-stack alloc tipped over the edge once before
// when the s_mix_names array landed in DRAM, see app_ms_client_ws.c).
EXT_RAM_BSS_ATTR static fader_widgets_t s_widgets[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t                *s_status_label;
static lv_obj_t                *s_tileview;
static lv_obj_t                *s_page_tiles[MAX_PAGES];
static lv_obj_t                *s_page_dots[MAX_PAGES];
static size_t                   s_page_count;
static const ms_client_iface_t *s_ms;

// Settings overlay — declared up here so the gear button's event handler
// can reach it. Overlay is created lazily on first open.
static lv_obj_t *s_settings_overlay;
EXT_RAM_BSS_ATTR static lv_obj_t *s_color_swatches[APP_CONFIG_MAX_CHANNELS];
EXT_RAM_BSS_ATTR static lv_obj_t *s_row_name_labels[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t *s_lvl_norm_btn;
static lv_obj_t *s_lvl_db_btn;
static lv_obj_t *s_sig_buttons[3];   // none / signal-present / meter
static lv_obj_t *s_theme_buttons[2]; // dark / light
static lv_obj_t *s_rot_buttons[2];   // 0 deg / 180 deg
static lv_obj_t *s_bright_slider;
static lv_obj_t *s_bright_value_label;

// Auto-revert dialog state. After a rotation change the user has 10 s to
// confirm (Keep) or revert (Cancel); ignoring the dialog reverts. Without
// this, an accidental tap that flips the screen could leave a user unable
// to find the toggle to undo it.
#define ROT_REVERT_SECONDS  10
static lv_obj_t           *s_rot_confirm;
static lv_obj_t           *s_rot_confirm_msg;     // "Keep this orientation? Reverts in N s"
static lv_timer_t         *s_rot_confirm_timer;   // 1 Hz countdown -> auto-revert at 0
static int                 s_rot_confirm_remaining;
static app_display_rotation_t s_rot_pending_revert;  // value to revert TO if dialog times out

// Color-picker popup. One instance, reused for whichever channel last tapped
// a swatch in the settings overlay. s_picker_target_idx remembers which
// channel index to apply the selection to.
static lv_obj_t *s_picker_popup;
static lv_obj_t *s_picker_title;
static size_t   s_picker_target_idx;

// Mix bus selector — small button in the top bar shows the active mix
// label ("Mix N"); tap opens a grid popup of N mixes. Mix count is set
// by app_main from /console/information after WiFi associates. Active
// mix lives in app_ms_client (s_mix_bus_idx) — the UI is just the surface.
static lv_obj_t *s_mix_indicator;        // status-bar button
static lv_obj_t *s_mix_indicator_label;  // label inside it
static lv_obj_t *s_mix_picker_popup;
// Internal SRAM is tight at this point (we already moved s_mix_names to
// PSRAM via EXT_RAM_BSS_ATTR — see app_ms_client_ws.c). Even 96 bytes of
// static pointers can tip the FreeRTOS timer-task-stack alloc over the
// edge at boot, so keep this in PSRAM too. #42 will move the bigger
// per-channel arrays similarly.
EXT_RAM_BSS_ATTR static lv_obj_t *s_mix_picker_btn_labels[24];
static int       s_mix_count;            // 0 = popup not yet usable

// Rename popup — full-screen modal with a textarea + on-screen keyboard
// for editing a channel's scribble-strip name. The same popup is reused
// for whichever row was last tapped; s_rename_target_idx remembers which
// channel to apply the new name to.
static lv_obj_t *s_rename_popup;
static lv_obj_t *s_rename_title;
static lv_obj_t *s_rename_textarea;
static lv_obj_t *s_rename_keyboard;
static size_t   s_rename_target_idx;

// Network settings overlay — full-screen form with WiFi (SSID + password)
// and Mixing Station (host + port) fields. Edits are written to NVS via
// app_config_set_*; user is prompted to reboot to apply since live re-init
// of WiFi/WS while preserving UI state is more complex than the use case
// warrants.
// WiFi settings panel (entry: WiFi icon). Save = reboot since wifi config
// changes need a full driver re-init.
static lv_obj_t *s_wcfg_overlay;
static lv_obj_t *s_wcfg_ssid_ta;
static lv_obj_t *s_wcfg_pass_ta;
static lv_obj_t *s_wcfg_show_pass_cb;
static lv_obj_t *s_wcfg_keyboard;
static lv_obj_t *s_wcfg_status_label;
static lv_obj_t *s_wcfg_scan_btn;
static lv_obj_t *s_wcfg_scan_btn_label;
static lv_obj_t *s_wcfg_discard_confirm;
static char     s_wcfg_orig_ssid[APP_CONFIG_SSID_MAX];
static char     s_wcfg_orig_pass[APP_CONFIG_PASS_MAX];

// MS connection settings panel (entry: MS icon). Save = ws_reconnect()
// (live), no reboot -- just kicks the WS client to use the new host:port.
static lv_obj_t *s_mcfg_overlay;
static lv_obj_t *s_mcfg_host_ta;
static lv_obj_t *s_mcfg_port_ta;
static lv_obj_t *s_mcfg_keyboard;
static lv_obj_t *s_mcfg_status_label;
static lv_obj_t *s_mcfg_discard_confirm;
static char     s_mcfg_orig_host[APP_CONFIG_HOST_MAX];
static char     s_mcfg_orig_port[8];

// SSID scan results popup. Used only by the WiFi panel.
static lv_obj_t *s_ssid_list_popup;
static lv_obj_t *s_ssid_list;

// Channel picker overlay (entry: Settings -> Edit Channels...).
// Shows every channel on the connected console as a row with a checkbox;
// user picks up to APP_UI_MAX_TRACKED_CHANNELS to track on faders. Save
// persists to NVS via app_config_set_channel_ids and reboots; the rebuild
// path under live broadcast traffic is a known race (see esp_ui_main.c
// safe_max comment), so reboot is the simpler shape.
static int       s_total_channels;        // totalChannels from /console/information
static lv_obj_t *s_chpick_overlay;
static lv_obj_t *s_chpick_list;
static lv_obj_t *s_chpick_status_label;
static lv_obj_t *s_chpick_count_label;
static lv_obj_t *s_chpick_discard_confirm;
// Per-channel checkbox pointers, indexed by MS channel id. Sized to the
// picker-row cap, in PSRAM (could be 80+ on a large console).
EXT_RAM_BSS_ATTR static lv_obj_t *s_chpick_checks[APP_UI_MAX_PICKER_ROWS];
// Edit Channels button in the Settings overlay; tracked here so we can
// reveal it once the channel count arrives from MS.
static lv_obj_t *s_edit_channels_btn;
// Working selection during edit -- bool per id, sized like checks.
EXT_RAM_BSS_ATTR static bool      s_chpick_state[APP_UI_MAX_PICKER_ROWS];
// Originals at open-time, used for has-changes detection.
EXT_RAM_BSS_ATTR static bool      s_chpick_orig[APP_UI_MAX_PICKER_ROWS];

// Spinner shown when MS is not yet CONNECTED — replaces the fader strips
// during boot / outage so the user sees an unambiguous "waiting on the
// console" state instead of strips with no live data. Hidden once MS
// transitions to CONNECTED; reshown on disconnect / error.
static lv_obj_t *s_spinner;
static lv_obj_t *s_spinner_label;

// Mute Enabled — safety toggle that gates mute-button taps. Resets to
// FALSE on every boot so a power-cycle never leaves the device able to
// silence channels by accident. Mute button visuals continue to track
// the live MS state regardless; only the input path is gated.
static bool      s_mute_enabled;
static lv_obj_t *s_mute_en_btn;

// Toast — single floating label used for "mute is disabled" feedback. We
// reuse one object across all toast calls; subsequent toasts cancel the
// pending hide-timer and reset the 2 s window.
static lv_obj_t   *s_toast;
static lv_obj_t   *s_toast_label;
static lv_timer_t *s_toast_timer;

static void toast_show(const char *text);
static void apply_controls_enabled(void);
static void on_mute_en_clicked(lv_event_t *e);

// Status-bar icons for WiFi / MS connection state. Tap opens the read-only
// info panel; the icon color reflects the live state.
static lv_obj_t *s_wifi_icon_label;     // the LV_SYMBOL_WIFI label inside the button
static lv_obj_t *s_wifi_panel;
static lv_obj_t *s_wifi_state_value;
static lv_obj_t *s_wifi_ssid_value;
static lv_obj_t *s_wifi_ip_value;

static lv_obj_t *s_ms_icon_label;       // the LV_SYMBOL_AUDIO label inside the button
static lv_obj_t *s_ms_panel;
static lv_obj_t *s_ms_state_value;
static lv_obj_t *s_ms_host_value;
static lv_obj_t *s_ms_port_value;

static void settings_open(void);
static void settings_close(void);
static void on_gear_clicked(lv_event_t *e);
static void on_reboot_clicked(lv_event_t *e);
static void picker_open(size_t channel_idx);
static void picker_close(void);
static void rename_open(size_t channel_idx);
static void rename_close(void);
static void mix_picker_open(void);
static void mix_picker_close(void);
static void mix_picker_refresh_labels(void);
static void mix_indicator_refresh(void);
static void on_mix_indicator_clicked(lv_event_t *e);
static void on_name_clicked(lv_event_t *e);
static void wcfg_open(void);
static void wcfg_close(void);
static void mcfg_open(void);
static void mcfg_close(void);
static void chpick_open(void);
static void chpick_close(void);
static void on_edit_channels_clicked(lv_event_t *e);

// Reboot confirmation popup — built lazily, modal, two buttons. esp_restart
// is called on the Yes path; Cancel just hides the popup.
static lv_obj_t *s_reboot_confirm;
static void wifi_panel_open(void);
static void wifi_panel_close(void);
static void wifi_panel_refresh(void);
static void wifi_icon_refresh(void);
static void on_wifi_clicked(lv_event_t *e);
static void on_wifi_state_change(void *ctx);
static void ms_panel_open(void);
static void ms_panel_close(void);
static void ms_panel_refresh(void);
static void ms_icon_refresh(void);
static void on_ms_clicked(lv_event_t *e);
static void on_ms_state_change(void *ctx);

// Rate-limit outbound SETs per channel so a fast drag doesn't flood MS
// (each SET produces a broadcast echo, doubling on-wire traffic). 50 ms
// = 20 Hz feels live to the user but keeps the websocket task from
// monopolizing its core.
#define SET_MIN_INTERVAL_MS 50
EXT_RAM_BSS_ATTR static uint32_t s_last_send_ms[APP_CONFIG_MAX_CHANNELS];

static void apply_status(void *arg)
{
    char *text = (char *)arg;
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
    free(text);
}

void app_ui_set_status(const char *text)
{
    if (!text) return;
    char *copy = strdup(text);
    if (!copy) return;
    // lv_async_call mutates LVGL's timer list, so it MUST hold lvgl_port_lock
    // when called from a non-LVGL task. Without it, the WS / wifi tasks race
    // the LVGL task and updates get lost.
    if (!lvgl_port_lock(100)) {
        free(copy);
        return;
    }
    if (lv_async_call(apply_status, copy) != LV_RESULT_OK) {
        free(copy);
    }
    lvgl_port_unlock();
}

static void send_level_now(size_t idx, float level)
{
    int ch_id = app_state_id_for_idx(idx);
    if (s_ms && ch_id >= 0) {
        APP_LOGD_T("app_ui", "fader idx=%u ch=%d -> %.3f",
                   (unsigned) idx, ch_id, (double) level);
        s_ms->set_level(ch_id, level);
    }
    s_last_send_ms[idx] = (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_slider_changed(lv_event_t *e)
{
    size_t    idx    = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int       v      = lv_slider_get_value(slider);
    float     level  = (float)v / 100.0f;

    // Update local state without notifying — UI already reflects this value.
    app_state_set_level(idx, level, false);

    // Rate-limit outbound SETs to ~20 Hz per channel. Each SET produces a
    // server-snap echo on the same WS, so unlimited drag-frequency SETs
    // doubled into a flood that monopolized the websocket task on CPU 1.
    // The final value is sent on slider release, see on_slider_released.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_send_ms[idx] >= SET_MIN_INTERVAL_MS) {
        send_level_now(idx, level);
    }

    // Mid-drag the dB hasn't echoed back yet, so use the slider's
    // immediate value for the readout. The post-drag MS broadcast
    // updates app_state.level_db and apply_pending takes over.
    char buf[12];
    if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
        snprintf(buf, sizeof(buf), "...");
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
    }
    lv_label_set_text(s_widgets[idx].label_val, buf);
}

static void on_slider_released(lv_event_t *e)
{
    size_t    idx    = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int       v      = lv_slider_get_value(slider);
    // Always emit the final value so the rate-limiter can't swallow the
    // last position the user landed on.
    send_level_now(idx, (float)v / 100.0f);
}

static void on_mute_clicked(lv_event_t *e)
{
    size_t    idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

    // Gate 1: MS must be connected. Silent reject — the MS status icon
    // already conveys the connection state, no need to nag.
    bool ms_ok = (s_ms && s_ms->get_state &&
                  s_ms->get_state() == APP_MS_STATE_CONNECTED);
    if (!ms_ok) return;

    // Gate 2: Mute Enabled toggle must be on. Loud reject — this is the
    // user-facing safety; the toast tells them how to enable it.
    if (!s_mute_enabled) {
        toast_show("Mute disabled - tap MUTE EN to enable");
        return;
    }

    // Read canonical state from app_state and toggle. We don't set
    // LV_OBJ_FLAG_CHECKABLE on the button (that fires on press and a
    // drag-off-then-release would still toggle), so we drive the visual
    // CHECKED state ourselves on each click.
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;
    bool new_mute = !ch.mute;

    int ch_id = app_state_id_for_idx(idx);
    APP_LOGD_I("app_ui", "mute idx=%u ch=%d -> %d",
               (unsigned) idx, ch_id, (int) new_mute);
    if (s_ms && s_ms->set_mute && ch_id >= 0) {
        s_ms->set_mute(ch_id, new_mute);
    }
    app_state_set_mute(idx, new_mute, false);

    // Optimistic UI: flip the visual immediately so the press feels live.
    // The MS broadcast echo will hit apply_pending shortly and reconfirm.
    if (new_mute) lv_obj_add_state   (btn, LV_STATE_CHECKED);
    else          lv_obj_remove_state(btn, LV_STATE_CHECKED);
}

// Inbound updates from the WS task are coalesced via a per-channel dirty
// flag plus a single in-flight async sweep. The sweep, running under LVGL's
// task, reads each channel's latest state and applies it to the widgets.
//
// This replaces the original "malloc a snapshot per WS message + queue an
// async per snapshot" pattern, which had two problems:
//   1. lv_async_call mutates LVGL's timer list and was being called from the
//      WS task without lvgl_port_lock → races dropped occasional updates,
//      including the final "user released" broadcast (visible symptom: the
//      device slider not landing on the same value as the MS slider).
//   2. During a drag MS broadcasts at ~100 Hz, so every drag allocated 200+
//      tiny structs that all had to be freed by the LVGL task in order.
//
// The dirty-flag scheme guarantees the last value wins (the sweep reads
// fresh state at apply time) and only one async is ever in flight.
EXT_RAM_BSS_ATTR static volatile bool s_dirty[APP_CONFIG_MAX_CHANNELS];
static volatile bool s_sweep_queued;

static void apply_pending(void *unused)
{
    (void)unused;
    // Clear the queued flag BEFORE iterating so any state change that fires
    // during the sweep (and after we've passed its index) re-arms a fresh
    // sweep rather than getting silently swallowed.
    s_sweep_queued = false;
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        if (!s_dirty[i]) continue;
        s_dirty[i] = false;
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        int v = (int)(ch.level * 100.0f);
        // LV_ANIM_OFF: network echoes can arrive every ~10ms during a drag;
        // queueing/cancelling 200ms animations on each one trashes LVGL.
        lv_slider_set_value(s_widgets[i].slider, v, LV_ANIM_OFF);
        char buf[12];
        if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
            // MS reports min ≈ -138 dB on the Si Expression 2; display "-INF"
            // there since the channel is effectively off. Above the floor we
            // round to the nearest dB — a half-dB step is finer than the
            // mixer's quantization, no need for decimals on a glance-readout.
            if (ch.level_db <= -138.0f) {
                snprintf(buf, sizeof(buf), "-INF");
            } else {
                snprintf(buf, sizeof(buf), "%.0f dB", ch.level_db);
            }
        } else {
            snprintf(buf, sizeof(buf), "%d", v);
        }
        lv_label_set_text(s_widgets[i].label_val, buf);
        lv_label_set_text(s_widgets[i].label_name, ch.name);
        if (s_widgets[i].btn_mute) {
            if (ch.mute) lv_obj_add_state   (s_widgets[i].btn_mute, LV_STATE_CHECKED);
            else         lv_obj_remove_state(s_widgets[i].btn_mute, LV_STATE_CHECKED);
        }
        if (s_widgets[i].slider) {
            int color_idx = app_prefs_get_channel_color(ch.id);
            uint32_t hex = (color_idx >= 0 && color_idx < 8)
                               ? COLOR_PALETTE[color_idx]
                               : DEFAULT_SLIDER_HEX;
            lv_color_t bar_color  = lv_color_hex(hex);
            // Darken the knob ~24% so it reads as a separate piece on top
            // of the filled bar. lvl is 0..255 with 255 being fully black.
            lv_color_t knob_color = lv_color_darken(bar_color, 60);
            lv_obj_set_style_bg_color(s_widgets[i].slider, bar_color,  LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_widgets[i].slider, knob_color, LV_PART_KNOB);
        }
        if (s_widgets[i].signal_dot) {
            app_signal_indicator_t mode = app_prefs_get_signal_indicator();
            bool show = (mode != APP_SIGNAL_INDICATOR_NONE) &&
                        !ch.mute && ch.level > 0.01f;
            if (show) {
                lv_obj_set_style_bg_color(s_widgets[i].signal_dot,
                                          lv_color_hex(0x40C040), 0);
                lv_obj_remove_flag(s_widgets[i].signal_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_widgets[i].signal_dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void on_state_change(size_t idx, void *ctx)
{
    (void)ctx;
    if (idx >= APP_CONFIG_MAX_CHANNELS) return;
    s_dirty[idx] = true;
    // Fast path: if a sweep is already queued, our dirty flag will be picked
    // up by it. No lock, no allocation, no work.
    if (s_sweep_queued) return;

    if (!lvgl_port_lock(100)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
            // Best effort — the next state change retries.
        }
    }
    lvgl_port_unlock();
}

// Pref changes (level format, channel colors, signal indicator, theme) affect
// the rendering of every fader, so we mark all channels dirty and queue a
// single sweep — same plumbing as the per-channel state changes. Theme is
// re-applied here too; lv_theme_default_init is idempotent for an unchanged
// theme so we don't need to track the old value.
static void on_prefs_change(void *ctx)
{
    (void)ctx;
    app_display_apply_theme(app_prefs_get_theme());
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) s_dirty[i] = true;
    if (s_sweep_queued) return;

    if (!lvgl_port_lock(100)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
        }
    }
    lvgl_port_unlock();
}

static void style_dot(lv_obj_t *dot, bool active)
{
    lv_color_t fill = active ? lv_color_hex(0xE0E0E0) : lv_color_hex(0x404040);
    lv_obj_set_style_bg_color(dot, fill, 0);
}

static void on_tile_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *active = lv_tileview_get_tile_active(s_tileview);
    for (size_t i = 0; i < s_page_count; ++i) {
        style_dot(s_page_dots[i], s_page_tiles[i] == active);
    }
}

static void build_fader(lv_obj_t *parent, size_t idx, int slot_x_in_tile)
{
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;

    // Center the box within its slot. slot_w = SCREEN_W / FADERS_PER_PAGE.
    const int slot_w = SCREEN_W / FADERS_PER_PAGE;
    const int box_x  = slot_x_in_tile + (slot_w - FADER_BOX_W) / 2;
    const int box_y  = (TILEVIEW_H - FADER_BOX_H) / 2;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, FADER_BOX_W, FADER_BOX_H);
    lv_obj_set_pos(box, box_x, box_y);
    lv_obj_set_style_pad_all(box, FADER_BOX_PAD, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, ch.name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, FADER_BOX_W - 2 * FADER_BOX_PAD);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    s_widgets[idx].label_name = name;

    // Signal-present indicator — small round dot centered below the name
    // label and above the slider. Visible only when mode != none AND the
    // channel is actively passing audio (!mute && level > 0). Without live
    // meter data from MS (offline test instance) this is a "configured
    // to pass audio" indicator, not strict audio presence — useful for
    // the "do I have a cable problem?" question.
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_set_size(dot, SIGNAL_DOT_SIZE, SIGNAL_DOT_SIZE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    s_widgets[idx].signal_dot = dot;

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, (int)(ch.level * 100.0f), LV_ANIM_OFF);
    lv_obj_set_size(slider, SLIDER_W, SLIDER_H);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, on_slider_changed, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(slider, on_slider_released, LV_EVENT_RELEASED,
                        (void *)(uintptr_t)idx);
    s_widgets[idx].slider = slider;

    // Unity-gain (0 dB) tick to the right of the slider track. The user
    // lines the knob up with this to dial in unity by feel — no need to
    // read the dB readout at the bottom while concentrating on the mix.
    // Slider is centered in the box, value goes UP, so the tick's vertical
    // offset from center is positive when below center (norm < 0.5) and
    // negative when above (norm > 0.5).
    lv_obj_t *tick = lv_obj_create(box);
    lv_obj_set_size(tick, 12, 2);
    lv_obj_set_style_bg_color(tick, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_pad_all(tick, 0, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
    int tick_y_off = (int)((0.5f - NORM_AT_0DB) * (float) SLIDER_H);
    lv_obj_align(tick, LV_ALIGN_CENTER, SLIDER_W / 2 + 10, tick_y_off);

    lv_obj_t *btn_mute = lv_button_create(box);
    // Note: NOT LV_OBJ_FLAG_CHECKABLE — that auto-toggles on press, which
    // then fires VALUE_CHANGED even if the finger drags off before release.
    // We track checked state ourselves and listen for LV_EVENT_CLICKED,
    // which only fires on press-and-release-on-widget (drag-off cancels).
    lv_obj_set_size(btn_mute, MUTE_BTN_W, MUTE_BTN_H);
    lv_obj_align(btn_mute, LV_ALIGN_BOTTOM_MID, 0, -28);
    // Visible "muted" state — saturated red so a glance distinguishes the
    // silenced channels from the live ones in stage lighting.
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0xC00000), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0x303030), LV_STATE_DEFAULT);
    lv_obj_t *btn_label = lv_label_create(btn_mute);
    lv_label_set_text(btn_label, "MUTE");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn_mute, on_mute_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)idx);
    s_widgets[idx].btn_mute = btn_mute;
    if (ch.mute) lv_obj_add_state(btn_mute, LV_STATE_CHECKED);

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "0");
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_widgets[idx].label_val = val;
}

static lv_obj_t *create_page_indicator(lv_obj_t *parent, size_t pages)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    int total_w = (int)(pages * DOT_SIZE + (pages - 1) * DOT_GAP);
    lv_obj_set_size(bar, total_w, DOT_SIZE);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    int x = 0;
    for (size_t i = 0; i < pages; ++i) {
        lv_obj_t *dot = lv_obj_create(bar);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(dot, x, 0);
        lv_obj_set_style_radius(dot, DOT_SIZE / 2, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        style_dot(dot, i == 0);
        s_page_dots[i] = dot;
        x += DOT_SIZE + DOT_GAP;
    }
    return bar;
}

void app_ui_init(const ms_client_iface_t *ms)
{
    s_ms = ms;

    // Widget construction must hold the lvgl_port mutex. M1 (3 widgets) got
    // away without it because creation finished within one render tick;
    // M2's tileview + 12 faders + indicator is slow enough that the LVGL
    // render task races in and tries to lay out half-built objects, faulting
    // in get_prop_core on a NULL styles[].style.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "lvgl_port_lock failed; UI not built");
        return;
    }

    lv_obj_t *scr = lv_screen_active();

    // Status line at the top. app_wifi / ms_ws push updates here so the user
    // sees boot progress instead of a static screen.
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Booting...");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    // Mute-Enabled toggle in the top-left of the status bar. Same red-checked
    // / grey-default styling as the per-channel MUTE buttons so the visual
    // language is consistent. Resets to OFF on every boot — see the comment
    // on s_mute_enabled.
    s_mute_en_btn = lv_button_create(scr);
    lv_obj_set_size(s_mute_en_btn, 90, 28);
    lv_obj_align(s_mute_en_btn, LV_ALIGN_TOP_LEFT, 8, 2);
    lv_obj_set_style_bg_color(s_mute_en_btn, lv_color_hex(0xC00000), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_mute_en_btn, lv_color_hex(0x303030), LV_STATE_DEFAULT);
    lv_obj_t *me_lbl = lv_label_create(s_mute_en_btn);
    lv_label_set_text(me_lbl, "MUTE EN");
    lv_obj_center(me_lbl);
    lv_obj_add_event_cb(s_mute_en_btn, on_mute_en_clicked, LV_EVENT_CLICKED, NULL);

    // Settings gear in the top-right corner of the status bar — opens the
    // touch-driven configuration overlay defined further below.
    lv_obj_t *gear = lv_button_create(scr);
    lv_obj_set_size(gear, 28, 28);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 2);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gear, 0, 0);
    lv_obj_t *gear_label = lv_label_create(gear);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_label);
    lv_obj_add_event_cb(gear, on_gear_clicked, LV_EVENT_CLICKED, NULL);

    // WiFi status icon — left of gear. Color reflects connection state and
    // tapping opens the read-only WiFi info panel.
    lv_obj_t *wifi_btn = lv_button_create(scr);
    lv_obj_set_size(wifi_btn, 28, 28);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_RIGHT, -44, 2);
    lv_obj_set_style_radius(wifi_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_btn, 0, 0);
    s_wifi_icon_label = lv_label_create(wifi_btn);
    lv_label_set_text(s_wifi_icon_label, LV_SYMBOL_WIFI);
    lv_obj_center(s_wifi_icon_label);
    lv_obj_add_event_cb(wifi_btn, on_wifi_clicked, LV_EVENT_CLICKED, NULL);

    // MS status icon — left of WiFi. Same color/state pattern; tap opens
    // the read-only MS info panel.
    lv_obj_t *ms_btn = lv_button_create(scr);
    lv_obj_set_size(ms_btn, 28, 28);
    lv_obj_align(ms_btn, LV_ALIGN_TOP_RIGHT, -80, 2);
    lv_obj_set_style_radius(ms_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ms_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ms_btn, 0, 0);
    s_ms_icon_label = lv_label_create(ms_btn);
    lv_label_set_text(s_ms_icon_label, LV_SYMBOL_AUDIO);
    lv_obj_center(s_ms_icon_label);
    lv_obj_add_event_cb(ms_btn, on_ms_clicked, LV_EVENT_CLICKED, NULL);

    // Mix-bus indicator — left of the MS icon. Shows the active mix label
    // ("Mix N"); tap opens the selector popup. Hidden until app_main tells
    // us how many mixes the connected console exposes.
    s_mix_indicator = lv_button_create(scr);
    lv_obj_set_size(s_mix_indicator, 90, 28);
    lv_obj_align(s_mix_indicator, LV_ALIGN_TOP_RIGHT, -118, 2);
    s_mix_indicator_label = lv_label_create(s_mix_indicator);
    lv_label_set_text(s_mix_indicator_label, "Mix 1");
    lv_obj_center(s_mix_indicator_label);
    lv_obj_add_event_cb(s_mix_indicator, on_mix_indicator_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_mix_indicator, LV_OBJ_FLAG_HIDDEN);

    // Loading spinner — shown while we're not connected to MS so the user
    // sees an explicit "waiting" state instead of stale / empty fader
    // strips. Position centered in the fader area; the tileview overlays
    // it once we're connected.
    s_spinner = lv_spinner_create(scr);
    lv_obj_set_size(s_spinner, 80, 80);
    lv_obj_align(s_spinner, LV_ALIGN_CENTER, 0, -10);
    s_spinner_label = lv_label_create(scr);
    lv_label_set_text(s_spinner_label, "Connecting to console...");
    lv_obj_align_to(s_spinner_label, s_spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    app_state_register_on_change(on_state_change, NULL);
    app_prefs_register_on_change(on_prefs_change, NULL);
    app_wifi_register_on_change(on_wifi_state_change, NULL);
    if (s_ms && s_ms->register_on_change) {
        s_ms->register_on_change(on_ms_state_change, NULL);
    }

    wifi_icon_refresh();
    ms_icon_refresh();
    apply_controls_enabled();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI shell mounted; awaiting channel enumeration");
}

// Page indicator widget; recreated on each present_channels() call. Tracked
// here so a rebuild can destroy the previous one before constructing the
// new one.
static lv_obj_t *s_page_indicator;

void app_ui_present_channels(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "present_channels: lvgl_port_lock failed");
        return;
    }

    // Tear down any previous fader UI so a re-call (mix change, channel
    // selection edit) builds clean. lv_obj_clean keeps the tileview but
    // removes its children; lv_obj_delete on the page indicator drops it
    // entirely so we can recreate.
    if (s_tileview) {
        lv_obj_clean(s_tileview);
        memset(s_page_tiles,  0, sizeof(s_page_tiles));
    } else {
        s_tileview = lv_tileview_create(lv_screen_active());
        lv_obj_set_size(s_tileview, SCREEN_W, TILEVIEW_H);
        lv_obj_set_pos(s_tileview, 0, TILEVIEW_Y);
        lv_obj_set_style_border_width(s_tileview, 0, 0);
        // Hidden by default — ms_apply_async unhides on the next transition
        // to CONNECTED. Keeps the spinner-vs-tileview ownership clean and
        // prevents an empty-tileview flash before any data arrives.
        lv_obj_add_flag(s_tileview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_tileview, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (s_page_indicator) {
        lv_obj_delete(s_page_indicator);
        s_page_indicator = NULL;
        memset(s_page_dots, 0, sizeof(s_page_dots));
    }
    memset(s_widgets, 0, sizeof(s_widgets));

    size_t total = app_state_count();
    s_page_count = (total + FADERS_PER_PAGE - 1) / FADERS_PER_PAGE;
    if (s_page_count == 0) s_page_count = 1;
    if (s_page_count > MAX_PAGES) s_page_count = MAX_PAGES;

    for (size_t p = 0; p < s_page_count; ++p) {
        // dir: first page can only swipe right, last page only left, middle
        // pages both. With one page (s_page_count == 1) dir = 0 disables
        // navigation entirely.
        lv_dir_t dir = 0;
        if (p > 0)                    dir |= LV_DIR_LEFT;
        if (p + 1 < s_page_count)     dir |= LV_DIR_RIGHT;

        lv_obj_t *tile = lv_tileview_add_tile(s_tileview, (uint8_t)p, 0, dir);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        s_page_tiles[p] = tile;

        const int slot_w = SCREEN_W / FADERS_PER_PAGE;
        for (size_t slot = 0; slot < FADERS_PER_PAGE; ++slot) {
            size_t idx = p * FADERS_PER_PAGE + slot;
            if (idx >= total) break;
            build_fader(tile, idx, (int)slot * slot_w);
        }
    }

    if (s_page_count > 1) {
        s_page_indicator = create_page_indicator(lv_screen_active(), s_page_count);
    }

    apply_controls_enabled();

    // Sync the spinner ↔ tileview visibility with the current MS state.
    // Without this, the freshly-built tileview would stay hidden until the
    // next state-change event, leaving the spinner overlaid on the strips.
    bool ms_connected = (s_ms && s_ms->get_state &&
                         s_ms->get_state() == APP_MS_STATE_CONNECTED);
    if (ms_connected) {
        if (s_tileview)      lv_obj_remove_flag(s_tileview,      LV_OBJ_FLAG_HIDDEN);
        if (s_spinner)       lv_obj_add_flag   (s_spinner,       LV_OBJ_FLAG_HIDDEN);
        if (s_spinner_label) lv_obj_add_flag   (s_spinner_label, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "faders mounted: %u channels across %u page(s)",
             (unsigned)total, (unsigned)s_page_count);
}

// ─────────────────────────────────────────────────────────────────────────
// Settings overlay — touch-driven configuration of the prefs that today
// only have console commands (level format, signal indicator, channel
// colors). Built lazily on first gear-icon tap; subsequent opens reuse
// the same overlay and re-sync visible state from app_prefs.
// ─────────────────────────────────────────────────────────────────────────

static void on_gear_clicked(lv_event_t *e)
{
    (void) e;
    settings_open();
}

static void on_close_clicked(lv_event_t *e)
{
    (void) e;
    settings_close();
}

static void update_radio_visuals(lv_obj_t **buttons, size_t count, size_t selected)
{
    for (size_t i = 0; i < count; ++i) {
        if (i == selected) lv_obj_add_state   (buttons[i], LV_STATE_CHECKED);
        else               lv_obj_remove_state(buttons[i], LV_STATE_CHECKED);
    }
}

static void on_lvl_norm_clicked(lv_event_t *e)
{
    (void) e;
    app_prefs_set_level_format(APP_LEVEL_FORMAT_NORM);
    lv_obj_t *btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(btns, 2, 0);
}

static void on_lvl_db_clicked(lv_event_t *e)
{
    (void) e;
    app_prefs_set_level_format(APP_LEVEL_FORMAT_DB);
    lv_obj_t *btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(btns, 2, 1);
}

static void on_sig_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    app_prefs_set_signal_indicator((app_signal_indicator_t) which);
    update_radio_visuals(s_sig_buttons, 3, (size_t) which);
}

static void on_theme_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    app_prefs_set_theme((app_theme_t) which);
    update_radio_visuals(s_theme_buttons, 2, (size_t) which);
}

// ─────────────────────────────────────────────────────────────────────────
// Rotation toggle + auto-revert dialog. The toggle applies + persists
// optimistically; a 10 s confirm popup reverts the change if the user
// doesn't tap "Keep" -- prevents an accidental rotation from leaving the
// user unable to find the toggle to undo it.
// ─────────────────────────────────────────────────────────────────────────

static void rot_confirm_close(void)
{
    if (s_rot_confirm_timer) {
        lv_timer_delete(s_rot_confirm_timer);
        s_rot_confirm_timer = NULL;
    }
    if (s_rot_confirm) {
        lv_obj_add_flag(s_rot_confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rot_confirm_revert(void)
{
    // Apply + persist the original orientation, then refresh the radios so
    // they reflect the actual state.
    app_prefs_set_display_rotation(s_rot_pending_revert);
    app_display_apply_rotation(s_rot_pending_revert);
    update_radio_visuals(s_rot_buttons, 2,
                         s_rot_pending_revert == APP_DISPLAY_ROTATION_180 ? 1 : 0);
    rot_confirm_close();
}

static void rot_confirm_tick(lv_timer_t *t)
{
    (void) t;
    s_rot_confirm_remaining--;
    if (s_rot_confirm_remaining <= 0) {
        rot_confirm_revert();
        return;
    }
    if (s_rot_confirm_msg) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "Keep this orientation?\nReverts in %d s",
                 s_rot_confirm_remaining);
        lv_label_set_text(s_rot_confirm_msg, buf);
    }
}

static void on_rot_keep(lv_event_t *e)
{
    (void) e;
    rot_confirm_close();
}

static void on_rot_cancel(lv_event_t *e)
{
    (void) e;
    rot_confirm_revert();
}

static void rot_confirm_show(app_display_rotation_t revert_to)
{
    s_rot_pending_revert    = revert_to;
    s_rot_confirm_remaining = ROT_REVERT_SECONDS;

    if (!s_rot_confirm) {
        // Build modal centered on the screen so it sits on top of the
        // settings overlay even after rotation flipped the framebuffer.
        lv_obj_t *scr = lv_screen_active();
        lv_obj_t *p = lv_obj_create(scr);
        lv_obj_set_size(p, 460, 220);
        lv_obj_center(p);
        lv_obj_set_style_pad_all(p, 20, 0);
        lv_obj_set_style_radius(p, 12, 0);
        lv_obj_set_style_border_width(p, 2, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        s_rot_confirm = p;

        s_rot_confirm_msg = lv_label_create(p);
        lv_label_set_text(s_rot_confirm_msg, "");
        lv_obj_align(s_rot_confirm_msg, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_text_align(s_rot_confirm_msg, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *cancel = lv_button_create(p);
        lv_obj_set_size(cancel, 160, 50);
        lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_t *cancel_lbl = lv_label_create(cancel);
        lv_label_set_text(cancel_lbl, "Cancel");
        lv_obj_center(cancel_lbl);
        lv_obj_add_event_cb(cancel, on_rot_cancel, LV_EVENT_CLICKED, NULL);

        lv_obj_t *keep = lv_button_create(p);
        lv_obj_set_size(keep, 160, 50);
        lv_obj_align(keep, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(keep, lv_color_hex(0x40C060), 0);
        lv_obj_t *keep_lbl = lv_label_create(keep);
        lv_label_set_text(keep_lbl, "Keep");
        lv_obj_center(keep_lbl);
        lv_obj_add_event_cb(keep, on_rot_keep, LV_EVENT_CLICKED, NULL);
    }

    // Initial label + show.
    char buf[80];
    snprintf(buf, sizeof(buf),
             "Keep this orientation?\nReverts in %d s",
             s_rot_confirm_remaining);
    lv_label_set_text(s_rot_confirm_msg, buf);
    lv_obj_remove_flag(s_rot_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_rot_confirm);

    if (s_rot_confirm_timer) lv_timer_delete(s_rot_confirm_timer);
    s_rot_confirm_timer = lv_timer_create(rot_confirm_tick, 1000, NULL);
}

static void on_rot_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    app_display_rotation_t new_rot = (which == 1)
                                         ? APP_DISPLAY_ROTATION_180
                                         : APP_DISPLAY_ROTATION_0;
    app_display_rotation_t old_rot = app_prefs_get_display_rotation();
    if (new_rot == old_rot) return;

    // Apply optimistically; revert path puts both prefs and framebuffer back
    // if the user doesn't confirm within ROT_REVERT_SECONDS.
    app_prefs_set_display_rotation(new_rot);
    app_display_apply_rotation(new_rot);
    update_radio_visuals(s_rot_buttons, 2, (size_t) which);
    rot_confirm_show(old_rot);
}

// Brightness slider — live LEDC update on every drag step (no network round
// trip), persist on release. Splitting value-changed and released keeps the
// slider responsive while only burning one NVS+SD commit per gesture.
static void on_bright_value_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (v < 5)   v = 5;
    if (v > 100) v = 100;
    app_display_set_backlight_pct((uint8_t) v);
    if (s_bright_value_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(s_bright_value_label, buf);
    }
}

static void on_bright_released(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (v < 5)   v = 5;
    if (v > 100) v = 100;
    app_prefs_set_brightness_pct((uint8_t) v);
}

// Tap a swatch → open the color-picker popup for that channel. The popup
// applies the choice via app_prefs (which fires the dirty sweep, recolouring
// the slider in real time) and refreshes the swatch visual on close.
static void on_swatch_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t) lv_event_get_user_data(e);
    picker_open(idx);
}

// One handler for all picker buttons. user_data carries the selected palette
// index, with -1 meaning "no color".
static void on_picker_choice(lv_event_t *e)
{
    int color = (int)(intptr_t) lv_event_get_user_data(e);
    int ch_id = app_state_id_for_idx(s_picker_target_idx);
    if (ch_id >= 0) app_prefs_set_channel_color(ch_id, color);

    // Update the source swatch immediately — apply_pending only repaints the
    // fader sliders, not anything inside the settings overlay.
    lv_obj_t *swatch = s_color_swatches[s_picker_target_idx];
    if (swatch) {
        if (color < 0) {
            lv_obj_set_style_bg_color(swatch, lv_color_hex(NO_COLOR_SWATCH_HEX), 0);
        } else {
            lv_obj_set_style_bg_color(swatch, lv_color_hex(COLOR_PALETTE[color]), 0);
        }
    }
    picker_close();
}

static void on_picker_close_clicked(lv_event_t *e)
{
    (void) e;
    picker_close();
}

static lv_obj_t *make_radio_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    // Default (unchecked) is muted; CHECKED uses our accent green so the
    // active option reads at a glance. The dark-theme default styled
    // unchecked and checked the same blue, which made the radios look
    // ambiguous on a screenshot.
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x303744), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x40C060), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xC0C0C0), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(0x101010), LV_STATE_CHECKED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void build_settings_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_overlay = ov;

    // Title bar.
    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(ov);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_close_clicked, LV_EVENT_CLICKED, NULL);

    // Reboot button — top-left corner of the settings overlay. Red bg so
    // the user reads it as a destructive action; tap opens a confirmation
    // dialog before actually calling esp_restart().
    lv_obj_t *reboot_btn = lv_button_create(ov);
    lv_obj_set_size(reboot_btn, 110, 36);
    lv_obj_align(reboot_btn, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_set_style_bg_color(reboot_btn, lv_color_hex(0xC04040), 0);
    lv_obj_t *reboot_lbl = lv_label_create(reboot_btn);
    lv_label_set_text(reboot_lbl, LV_SYMBOL_REFRESH " Reboot");
    lv_obj_center(reboot_lbl);
    lv_obj_add_event_cb(reboot_btn, on_reboot_clicked, LV_EVENT_CLICKED, NULL);

    // Section: Level Format
    lv_obj_t *lvl_label = lv_label_create(ov);
    lv_label_set_text(lvl_label, "Level Format");
    lv_obj_align(lvl_label, LV_ALIGN_TOP_LEFT, 0, 56);

    s_lvl_norm_btn = make_radio_button(ov, "0..100");
    lv_obj_set_size(s_lvl_norm_btn, 120, 44);
    lv_obj_align(s_lvl_norm_btn, LV_ALIGN_TOP_LEFT, 180, 50);
    lv_obj_add_event_cb(s_lvl_norm_btn, on_lvl_norm_clicked, LV_EVENT_CLICKED, NULL);

    s_lvl_db_btn = make_radio_button(ov, "dB");
    lv_obj_set_size(s_lvl_db_btn, 120, 44);
    lv_obj_align(s_lvl_db_btn, LV_ALIGN_TOP_LEFT, 312, 50);
    lv_obj_add_event_cb(s_lvl_db_btn, on_lvl_db_clicked, LV_EVENT_CLICKED, NULL);

    // Section: Rotation -- right side of row 1, beside Level Format. Only
    // 0 / 180 supported (the panel is landscape and 90/270 would reflow the
    // entire fader UI).
    lv_obj_t *rot_label = lv_label_create(ov);
    lv_label_set_text(rot_label, "Rotation");
    lv_obj_align(rot_label, LV_ALIGN_TOP_LEFT, 480, 56);

    static const char *rot_text[2] = { "0 deg", "180 deg" };
    for (int i = 0; i < 2; ++i) {
        s_rot_buttons[i] = make_radio_button(ov, rot_text[i]);
        lv_obj_set_size(s_rot_buttons[i], 120, 44);
        lv_obj_align(s_rot_buttons[i], LV_ALIGN_TOP_LEFT, 620 + i * 132, 50);
        lv_obj_add_event_cb(s_rot_buttons[i], on_rot_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
    }

    // Section: Signal Indicator
    lv_obj_t *sig_label = lv_label_create(ov);
    lv_label_set_text(sig_label, "Signal Indicator");
    lv_obj_align(sig_label, LV_ALIGN_TOP_LEFT, 0, 116);

    static const char *sig_text[3] = { "Off", "Signal", "Meter" };
    for (int i = 0; i < 3; ++i) {
        s_sig_buttons[i] = make_radio_button(ov, sig_text[i]);
        lv_obj_set_size(s_sig_buttons[i], 120, 44);
        lv_obj_align(s_sig_buttons[i], LV_ALIGN_TOP_LEFT, 180 + i * 132, 110);
        lv_obj_add_event_cb(s_sig_buttons[i], on_sig_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
    }

    // Section: Theme — Dark / Light radios. Right of Signal Indicator on the
    // same row to keep the panel vertically compact.
    lv_obj_t *theme_label = lv_label_create(ov);
    lv_label_set_text(theme_label, "Theme");
    lv_obj_align(theme_label, LV_ALIGN_TOP_LEFT, 600, 116);

    static const char *theme_text[2] = { "Dark", "Light" };
    for (int i = 0; i < 2; ++i) {
        s_theme_buttons[i] = make_radio_button(ov, theme_text[i]);
        lv_obj_set_size(s_theme_buttons[i], 120, 44);
        lv_obj_align(s_theme_buttons[i], LV_ALIGN_TOP_LEFT, 700 + i * 132, 110);
        lv_obj_add_event_cb(s_theme_buttons[i], on_theme_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
    }

    // Section: Brightness — LEDC PWM slider, 5..100. Live LEDC update on
    // drag (no network round trip), persist on release. The 5% floor at the
    // slider also lives in app_display + app_prefs as defence-in-depth: a
    // 0% mis-tap leaves the panel unreadable with no non-touch recovery.
    lv_obj_t *bright_label = lv_label_create(ov);
    lv_label_set_text(bright_label, "Brightness");
    lv_obj_align(bright_label, LV_ALIGN_TOP_LEFT, 0, 176);

    s_bright_slider = lv_slider_create(ov);
    lv_slider_set_range(s_bright_slider, 5, 100);
    lv_obj_set_size(s_bright_slider, 600, 24);
    lv_obj_align(s_bright_slider, LV_ALIGN_TOP_LEFT, 180, 180);
    lv_obj_add_event_cb(s_bright_slider, on_bright_value_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bright_slider, on_bright_released,
                        LV_EVENT_RELEASED, NULL);

    s_bright_value_label = lv_label_create(ov);
    lv_label_set_text(s_bright_value_label, "");
    lv_obj_align(s_bright_value_label, LV_ALIGN_TOP_LEFT, 800, 176);

    // Section: Channels — 3 columns × N rows in a scrollable container so
    // the same layout works at 12 channels (default) and at the Si Expression
    // 2's full 60-channel count. Row is just [name] + [swatch] for now;
    // selection checkbox will be added with the channel-selection feature.
    lv_obj_t *col_label = lv_label_create(ov);
    lv_label_set_text(col_label, "Channels");
    lv_obj_align(col_label, LV_ALIGN_TOP_LEFT, 0, 224);

    // Edit-channels entry — opens the picker overlay (#33). Button sits to
    // the right of the section label so it doesn't push the existing list
    // layout. The picker is the runtime way to change which channels are
    // tracked; was previously only doable via channels-reset + reflash.
    // Hidden if we don't know the connected console's channel count yet
    // (info fetch hasn't completed or failed) -- the picker would be
    // empty and tapping it would be confusing.
    s_edit_channels_btn = lv_button_create(ov);
    lv_obj_set_size(s_edit_channels_btn, 180, 36);
    lv_obj_align(s_edit_channels_btn, LV_ALIGN_TOP_RIGHT, 0, 216);
    lv_obj_t *edit_lbl = lv_label_create(s_edit_channels_btn);
    lv_label_set_text(edit_lbl, LV_SYMBOL_LIST " Edit Channels...");
    lv_obj_center(edit_lbl);
    lv_obj_add_event_cb(s_edit_channels_btn, on_edit_channels_clicked,
                        LV_EVENT_CLICKED, NULL);
    if (s_total_channels <= 0) {
        lv_obj_add_flag(s_edit_channels_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *list = lv_obj_create(ov);
    lv_obj_set_size(list, SCREEN_W - 32, SCREEN_H - 268);
    lv_obj_set_pos(list, 0, 248);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);

    const int cols      = 3;
    const int row_h     = 32;
    const int row_gap   = 4;
    const int col_gap   = 8;
    const int row_w     = (SCREEN_W - 32 - 2 * 6 - (cols - 1) * col_gap) / cols;
    const int swatch_sz = 22;
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        int col = (int)(i % cols);
        int row = (int)(i / cols);
        int x = col * (row_w + col_gap);
        int y = row * (row_h + row_gap);

        lv_obj_t *row_obj = lv_obj_create(list);
        lv_obj_set_size(row_obj, row_w, row_h);
        lv_obj_set_pos(row_obj, x, y);
        lv_obj_set_style_radius(row_obj, 4, 0);
        lv_obj_set_style_pad_all(row_obj, 4, 0);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row_obj);
        lv_label_set_text(name, ch.name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, row_w - swatch_sz - 16);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        // Tapping the name opens the rename popup. The swatch on the right
        // already has its own handler for color editing — two distinct
        // touch zones avoid an ambiguous "what does this row do?" UX.
        lv_obj_add_flag(name, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(name, on_name_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
        s_row_name_labels[i] = name;

        lv_obj_t *swatch = lv_obj_create(row_obj);
        lv_obj_set_size(swatch, swatch_sz, swatch_sz);
        lv_obj_align(swatch, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(0x808080), 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, on_swatch_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t) i);
        s_color_swatches[i] = swatch;
    }
}

static void settings_refresh_state(void)
{
    lv_obj_t *lvl_btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(lvl_btns, 2,
                         app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB ? 1 : 0);
    update_radio_visuals(s_sig_buttons, 3,
                         (size_t) app_prefs_get_signal_indicator());
    update_radio_visuals(s_theme_buttons, 2,
                         (size_t) app_prefs_get_theme());
    update_radio_visuals(s_rot_buttons, 2,
                         app_prefs_get_display_rotation() == APP_DISPLAY_ROTATION_180 ? 1 : 0);

    if (s_bright_slider) {
        uint8_t pct = app_prefs_get_brightness_pct();
        lv_slider_set_value(s_bright_slider, (int) pct, LV_ANIM_OFF);
        if (s_bright_value_label) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%u%%", (unsigned) pct);
            lv_label_set_text(s_bright_value_label, buf);
        }
    }

    // Refresh row names from app_state — they may have changed via MS
    // scribble-strip broadcasts (or a local rename) since the overlay was
    // built. Same loop also re-paints the swatches from app_prefs.
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        app_channel_t ch;
        if (s_row_name_labels[i] && app_state_get(i, &ch)) {
            lv_label_set_text(s_row_name_labels[i], ch.name);
        }
        if (!s_color_swatches[i]) continue;
        int ch_id = app_state_id_for_idx(i);
        int color = (ch_id >= 0) ? app_prefs_get_channel_color(ch_id) : -1;
        if (color < 0) {
            lv_obj_set_style_bg_color(s_color_swatches[i], lv_color_hex(NO_COLOR_SWATCH_HEX), 0);
        } else {
            lv_obj_set_style_bg_color(s_color_swatches[i],
                                      lv_color_hex(COLOR_PALETTE[color]), 0);
        }
    }
}

static void settings_open(void)
{
    if (!s_settings_overlay) {
        build_settings_overlay();
    }
    settings_refresh_state();
    lv_obj_remove_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_settings_overlay);
}

static void settings_close(void)
{
    if (s_settings_overlay) {
        lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Reboot confirmation — modal popup that gates the destructive action
// behind a deliberate second tap. esp_restart never returns; the LVGL
// task is killed by the chip reset.
// ─────────────────────────────────────────────────────────────────────────

static void on_reboot_yes(lv_event_t *e)
{
    (void) e;
    ESP_LOGW(TAG, "user-initiated reboot");
    esp_restart();
}

static void on_reboot_no(lv_event_t *e)
{
    (void) e;
    if (s_reboot_confirm) {
        lv_obj_add_flag(s_reboot_confirm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_reboot_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 420, 200);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_reboot_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg, "Reboot the device now?");
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 140, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_reboot_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 140, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0xC04040), 0);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, LV_SYMBOL_REFRESH " Reboot");
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes, on_reboot_yes, LV_EVENT_CLICKED, NULL);
}

static void on_reboot_clicked(lv_event_t *e)
{
    (void) e;
    if (!s_reboot_confirm) build_reboot_confirm();
    lv_obj_remove_flag(s_reboot_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_reboot_confirm);
}

// ─────────────────────────────────────────────────────────────────────────
// Color-picker popup — modal panel built lazily on first open. Lives on
// the screen (not as a child of settings_overlay) so move_foreground
// raises it above the settings panel.
// ─────────────────────────────────────────────────────────────────────────

#define PICKER_BTN_SZ   80
#define PICKER_GAP      10
#define PICKER_COLS     3
#define PICKER_ROWS     3
#define PICKER_INNER_W  (PICKER_COLS * PICKER_BTN_SZ + (PICKER_COLS - 1) * PICKER_GAP)
#define PICKER_INNER_H  (PICKER_ROWS * PICKER_BTN_SZ + (PICKER_ROWS - 1) * PICKER_GAP)
#define PICKER_PAD      20
#define PICKER_HEADER_H 40
#define PICKER_W        (PICKER_INNER_W + 2 * PICKER_PAD)
#define PICKER_H        (PICKER_INNER_H + 2 * PICKER_PAD + PICKER_HEADER_H)

static void build_picker_popup(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, PICKER_W, PICKER_H);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, PICKER_PAD, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_picker_popup = p;

    s_picker_title = lv_label_create(p);
    lv_label_set_text(s_picker_title, "Color");
    lv_obj_align(s_picker_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_button_create(p);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_picker_close_clicked, LV_EVENT_CLICKED, NULL);

    // 3×3 grid: 8 palette colors + 1 "no color" (clear) button.
    for (int i = 0; i < 9; ++i) {
        int row = i / PICKER_COLS;
        int col = i % PICKER_COLS;
        int x   = col * (PICKER_BTN_SZ + PICKER_GAP);
        int y   = PICKER_HEADER_H + row * (PICKER_BTN_SZ + PICKER_GAP);

        lv_obj_t *btn = lv_button_create(p);
        lv_obj_set_size(btn, PICKER_BTN_SZ, PICKER_BTN_SZ);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 6, 0);

        intptr_t color_idx;
        if (i < 8) {
            color_idx = i;
            lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_PALETTE[i]), 0);
        } else {
            // 9th cell — clear / no color. Render with a × glyph so it reads
            // as "remove" rather than "another shade of grey".
            color_idx = -1;
            lv_obj_set_style_bg_color(btn, lv_color_hex(NO_COLOR_SWATCH_HEX), 0);
            lv_obj_t *x_lbl = lv_label_create(btn);
            lv_label_set_text(x_lbl, LV_SYMBOL_CLOSE);
            lv_obj_center(x_lbl);
        }
        lv_obj_add_event_cb(btn, on_picker_choice, LV_EVENT_CLICKED,
                            (void *)(intptr_t) color_idx);
    }
}

static void picker_open(size_t channel_idx)
{
    if (!s_picker_popup) build_picker_popup();
    s_picker_target_idx = channel_idx;

    // Title shows the channel name so a glance confirms the right strip
    // is being recolored — useful when channel labels are dotted off in
    // the compact rows.
    app_channel_t ch;
    if (app_state_get(channel_idx, &ch)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Color: %s", ch.name);
        lv_label_set_text(s_picker_title, buf);
    }

    lv_obj_remove_flag(s_picker_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker_popup);
}

static void picker_close(void)
{
    if (s_picker_popup) {
        lv_obj_add_flag(s_picker_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Mix-bus selector — modal popup of N buttons (one per mix); tap one to
// switch the active mix. The label is "Mix N" until scribble-strip names
// land in a follow-up; the protocol-side mix index is 0-based so the
// label is i+1.
// ─────────────────────────────────────────────────────────────────────────

static void mix_indicator_refresh(void)
{
    if (!s_mix_indicator_label || !s_ms || !s_ms->get_mix) return;
    int cur = s_ms->get_mix();
    const char *name = (s_ms->get_mix_name) ? s_ms->get_mix_name(cur) : NULL;
    char buf[24];
    if (name) {
        snprintf(buf, sizeof(buf), "%s", name);
    } else {
        snprintf(buf, sizeof(buf), "Mix %d", cur + 1);
    }
    lv_label_set_text(s_mix_indicator_label, buf);
}

static void on_mix_choice(lv_event_t *e)
{
    int mix_idx = (int)(intptr_t) lv_event_get_user_data(e);
    if (s_ms && s_ms->set_mix) s_ms->set_mix(mix_idx);
    // Persist so the choice survives reboots. Boot-time validates against
    // the actual mix count from /console/information.
    if (mix_idx >= 0 && mix_idx <= 255) {
        app_prefs_set_selected_mix_index((uint8_t) mix_idx);
    }
    mix_indicator_refresh();
    mix_picker_close();
}

static void on_mix_picker_close_clicked(lv_event_t *e)
{
    (void) e;
    mix_picker_close();
}

static void build_mix_picker_popup(void)
{
    lv_obj_t *scr = lv_screen_active();

    // 4-column grid; cols × rows sized to fit the current mix count.
    const int btn_w  = 110;
    const int btn_h  = 50;
    const int gap    = 10;
    const int pad    = 20;
    const int header = 40;
    const int cols   = 4;
    int rows         = (s_mix_count + cols - 1) / cols;
    if (rows < 1) rows = 1;

    int inner_w = cols * btn_w + (cols - 1) * gap;
    int inner_h = rows * btn_h + (rows - 1) * gap;
    int popup_w = inner_w + 2 * pad;
    int popup_h = inner_h + 2 * pad + header;

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, popup_w, popup_h);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, pad, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_mix_picker_popup = p;

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "Mix");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_button_create(p);
    lv_obj_set_size(close_btn, 32, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_mix_picker_close_clicked,
                        LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < s_mix_count; ++i) {
        int row = i / cols;
        int col = i % cols;
        int x   = col * (btn_w + gap);
        int y   = header + row * (btn_h + gap);

        lv_obj_t *btn = lv_button_create(p);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, x, y);
        const char *name = (s_ms && s_ms->get_mix_name) ? s_ms->get_mix_name(i)
                                                         : NULL;
        char buf[24];
        if (name) snprintf(buf, sizeof(buf), "%s", name);
        else      snprintf(buf, sizeof(buf), "Mix %d", i + 1);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, buf);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, on_mix_choice, LV_EVENT_CLICKED,
                            (void *)(intptr_t) i);
        if (i < (int)(sizeof(s_mix_picker_btn_labels) /
                       sizeof(s_mix_picker_btn_labels[0]))) {
            s_mix_picker_btn_labels[i] = lbl;
        }
    }
}

static void mix_picker_open(void)
{
    if (s_mix_count <= 0) return;
    if (!s_mix_picker_popup) {
        build_mix_picker_popup();
    } else {
        // Pull in any name updates that arrived since the last open. No
        // teardown — labels are mutated in place to keep the LVGL heap
        // quiet over long sessions.
        mix_picker_refresh_labels();
    }
    lv_obj_remove_flag(s_mix_picker_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mix_picker_popup);
}

static void mix_picker_close(void)
{
    if (s_mix_picker_popup) {
        lv_obj_add_flag(s_mix_picker_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_mix_indicator_clicked(lv_event_t *e)
{
    (void) e;
    mix_picker_open();
}

void app_ui_set_mix_count(int count)
{
    if (count < 0) count = 0;
    s_mix_count = count;
    if (!s_mix_indicator) return;
    if (!lvgl_port_lock(1000)) return;
    if (s_mix_count > 0) {
        lv_obj_remove_flag(s_mix_indicator, LV_OBJ_FLAG_HIDDEN);
        mix_indicator_refresh();
    } else {
        lv_obj_add_flag(s_mix_indicator, LV_OBJ_FLAG_HIDDEN);
    }
    // If the popup was already built for an earlier count, drop it so
    // the next open builds a fresh grid sized to the new count. The
    // child labels go with it; clear our cached pointers so we don't
    // dereference freed widgets in mix_picker_refresh_labels.
    if (s_mix_picker_popup) {
        lv_obj_delete(s_mix_picker_popup);
        s_mix_picker_popup = NULL;
        memset(s_mix_picker_btn_labels, 0, sizeof(s_mix_picker_btn_labels));
    }
    lvgl_port_unlock();
}

// ─────────────────────────────────────────────────────────────────────────
// Rename popup — full-screen modal with a textarea and an on-screen
// keyboard for editing a channel's scribble-strip name. Save POSTs the
// new name to MS via the client interface; the existing subscription on
// `ch.<n>.cfg.name` echoes the change back and updates local state.
// ─────────────────────────────────────────────────────────────────────────

static void on_rename_save(lv_event_t *e)
{
    (void) e;
    const char *text = lv_textarea_get_text(s_rename_textarea);
    if (text && *text) {
        int ch_id = app_state_id_for_idx(s_rename_target_idx);
        if (ch_id >= 0 && s_ms && s_ms->set_name) {
            s_ms->set_name(ch_id, text);
        }
    }
    rename_close();
}

static void on_rename_cancel(lv_event_t *e)
{
    (void) e;
    rename_close();
}

static void on_rename_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)  on_rename_save  (e);
    if (code == LV_EVENT_CANCEL) on_rename_cancel(e);
}

static void build_rename_popup(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_rename_popup = p;

    s_rename_title = lv_label_create(p);
    lv_label_set_text(s_rename_title, "Rename channel");
    lv_obj_align(s_rename_title, LV_ALIGN_TOP_LEFT, 4, 4);

    // Cancel + Save buttons in the title row so they're visible above the
    // keyboard's footprint.
    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 110, 36);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -130, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_rename_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save = lv_button_create(p);
    lv_obj_set_size(save, 110, 36);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x40C060), 0);
    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, LV_SYMBOL_OK " Save");
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save, on_rename_save, LV_EVENT_CLICKED, NULL);

    s_rename_textarea = lv_textarea_create(p);
    lv_obj_set_size(s_rename_textarea, SCREEN_W - 32, 60);
    lv_obj_align(s_rename_textarea, LV_ALIGN_TOP_LEFT, 4, 56);
    lv_textarea_set_one_line(s_rename_textarea, true);
    // MS scribble strip names are typically short — 16 chars is plenty
    // and prevents accidental overflow on the textarea label widths.
    lv_textarea_set_max_length(s_rename_textarea, 16);

    s_rename_keyboard = lv_keyboard_create(p);
    lv_obj_set_size(s_rename_keyboard, SCREEN_W - 32, SCREEN_H - 56 - 60 - 32);
    lv_obj_align(s_rename_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_keyboard_set_textarea(s_rename_keyboard, s_rename_textarea);
    // Custom event cb so the keyboard's built-in OK / Close buttons map
    // to our save / cancel actions instead of just dismissing the keyboard.
    lv_obj_add_event_cb(s_rename_keyboard, on_rename_kb_event, LV_EVENT_ALL, NULL);
}

static void rename_open(size_t channel_idx)
{
    if (!s_rename_popup) build_rename_popup();
    s_rename_target_idx = channel_idx;

    app_channel_t ch;
    if (app_state_get(channel_idx, &ch)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Rename: %s", ch.name);
        lv_label_set_text(s_rename_title, buf);
        lv_textarea_set_text(s_rename_textarea, ch.name);
    }

    lv_obj_remove_flag(s_rename_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_rename_popup);
}

static void rename_close(void)
{
    if (s_rename_popup) {
        lv_obj_add_flag(s_rename_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_name_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t) lv_event_get_user_data(e);
    rename_open(idx);
}

// ─────────────────────────────────────────────────────────────────────────
// WiFi + MS settings overlays. Two separate panels with the same UX
// patterns (kb hide on checkmark/non-text, X confirms discard with
// unsaved changes), but different commit semantics:
//   WiFi save -> reboot (driver re-init needed for new SSID/pass).
//   MS   save -> live ws_reconnect() (just kicks the WS client).
// Entry points: WiFi icon -> wcfg_open. MS icon -> mcfg_open.
// ─────────────────────────────────────────────────────────────────────────

// --- shared keyboard helpers (factored as small inline-ish funcs so each
// panel can call them without duplicating the LV_EVENT_READY/CANCEL plumbing)

static void wcfg_hide_keyboard(void)
{
    if (s_wcfg_keyboard) lv_obj_add_flag(s_wcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
}
static void mcfg_hide_keyboard(void)
{
    if (s_mcfg_keyboard) lv_obj_add_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// --- WiFi panel ----------------------------------------------------------

static bool wcfg_has_unsaved_changes(void)
{
    return strcmp(lv_textarea_get_text(s_wcfg_ssid_ta), s_wcfg_orig_ssid) != 0 ||
           strcmp(lv_textarea_get_text(s_wcfg_pass_ta), s_wcfg_orig_pass) != 0;
}

static void build_wcfg_discard_confirm(void);

static void on_wcfg_close(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    if (wcfg_has_unsaved_changes()) {
        if (!s_wcfg_discard_confirm) build_wcfg_discard_confirm();
        lv_obj_remove_flag(s_wcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wcfg_discard_confirm);
        return;
    }
    wcfg_close();
}

static void on_wcfg_discard_yes(lv_event_t *e)
{
    (void)e;
    if (s_wcfg_discard_confirm) lv_obj_add_flag(s_wcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
    wcfg_close();
}

static void on_wcfg_discard_no(lv_event_t *e)
{
    (void)e;
    if (s_wcfg_discard_confirm) lv_obj_add_flag(s_wcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void build_wcfg_discard_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 460, 200);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_wcfg_discard_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg, "Unsaved changes will be lost.\nDiscard and close?");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Keep Editing");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_wcfg_discard_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 160, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0xC04040), 0);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "Discard");
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes, on_wcfg_discard_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void on_wcfg_show_pass_changed(lv_event_t *e)
{
    (void)e;
    bool show = lv_obj_has_state(s_wcfg_show_pass_cb, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_wcfg_pass_ta, !show);
    wcfg_hide_keyboard();
}

static void on_wcfg_textarea_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (s_wcfg_keyboard) {
        lv_keyboard_set_textarea(s_wcfg_keyboard, ta);
        lv_keyboard_set_mode(s_wcfg_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(s_wcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wcfg_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        wcfg_hide_keyboard();
    }
}

static void on_wcfg_save(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    const char *ssid = lv_textarea_get_text(s_wcfg_ssid_ta);
    const char *pass = lv_textarea_get_text(s_wcfg_pass_ta);

    if (strlen(ssid) == 0) {
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 SSID cannot be empty.#");
        return;
    }
    bool ok = app_config_set_wifi_ssid(ssid) && app_config_set_wifi_pass(pass);
    if (!ok) {
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 Save failed (NVS error).#");
        return;
    }

    // Reboot to apply -- new SSID/pass needs a full wifi driver re-init,
    // and disconnecting/reconnecting cleanly mid-session would also drop
    // the WS, the SD logger, and several subscribers; reboot is simpler.
    lv_label_set_text(s_wcfg_status_label, "#40C060 Saved. Rebooting...#");
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// --- SSID scan list (used only by the WiFi panel) ------------------------

static void ssid_list_populate_async(void *arg);
static void on_ssid_row_clicked(lv_event_t *e);

static void on_ssid_list_close(lv_event_t *e)
{
    (void)e;
    if (s_ssid_list_popup) lv_obj_add_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
}

static void on_wifi_scan_done(void *ctx)
{
    (void)ctx;
    if (!lvgl_port_lock(100)) return;
    lv_async_call(ssid_list_populate_async, NULL);
    lvgl_port_unlock();
}

static void build_ssid_list_popup(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 460, 360);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 12, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_ssid_list_popup = p;

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "Pick a network");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_button_create(p);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_ssid_list_close, LV_EVENT_CLICKED, NULL);

    s_ssid_list = lv_list_create(p);
    lv_obj_set_size(s_ssid_list, 436, 290);
    lv_obj_align(s_ssid_list, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void ssid_list_populate_async(void *arg)
{
    (void)arg;
    if (!s_ssid_list_popup) build_ssid_list_popup();
    lv_obj_clean(s_ssid_list);

    char results[APP_WIFI_SCAN_MAX_RESULTS][33];
    size_t n = app_wifi_scan_results(results, APP_WIFI_SCAN_MAX_RESULTS);

    if (n == 0) {
        lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_WARNING,
                                           "No networks found");
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        for (size_t i = 0; i < n; ++i) {
            lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_WIFI,
                                               results[i]);
            lv_obj_add_event_cb(btn, on_ssid_row_clicked, LV_EVENT_CLICKED, NULL);
        }
    }

    if (s_wcfg_scan_btn) {
        lv_obj_remove_state(s_wcfg_scan_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_wcfg_scan_btn_label, "Scan");
    }

    lv_obj_remove_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ssid_list_popup);
}

static void on_ssid_row_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const char *txt = lv_list_get_button_text(s_ssid_list, btn);
    if (txt && s_wcfg_ssid_ta) {
        lv_textarea_set_text(s_wcfg_ssid_ta, txt);
    }
    if (s_ssid_list_popup) {
        lv_obj_add_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wcfg_scan_clicked(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    if (!app_wifi_scan_start(on_wifi_scan_done, NULL)) {
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 Scan already in progress.#");
        return;
    }
    lv_obj_add_state(s_wcfg_scan_btn, LV_STATE_DISABLED);
    lv_label_set_text(s_wcfg_scan_btn_label, "Scanning...");
    lv_label_set_text(s_wcfg_status_label, "");
}

static void build_wcfg_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_wcfg_overlay = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(ov);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_wcfg_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_button_create(ov);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x40C060), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_OK " Save");
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, on_wcfg_save, LV_EVENT_CLICKED, NULL);

    const int field_h    = 36;
    const int row_dy     = 56;
    const int form_w     = SCREEN_W - 32;
    const int scan_btn_w = 110;

    lv_obj_t *ssid_lbl = lv_label_create(ov);
    lv_label_set_text(ssid_lbl, "SSID");
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + 4);

    s_wcfg_ssid_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_wcfg_ssid_ta, form_w - 80 - scan_btn_w - 12, field_h);
    lv_obj_align(s_wcfg_ssid_ta, LV_ALIGN_TOP_LEFT, 80, 56);
    lv_textarea_set_one_line(s_wcfg_ssid_ta, true);
    lv_textarea_set_max_length(s_wcfg_ssid_ta, APP_CONFIG_SSID_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_ssid_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_wcfg_scan_btn = lv_button_create(ov);
    lv_obj_set_size(s_wcfg_scan_btn, scan_btn_w, field_h);
    lv_obj_align(s_wcfg_scan_btn, LV_ALIGN_TOP_LEFT, form_w - scan_btn_w, 56);
    s_wcfg_scan_btn_label = lv_label_create(s_wcfg_scan_btn);
    lv_label_set_text(s_wcfg_scan_btn_label, "Scan");
    lv_obj_center(s_wcfg_scan_btn_label);
    lv_obj_add_event_cb(s_wcfg_scan_btn, on_wcfg_scan_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *pass_lbl = lv_label_create(ov);
    lv_label_set_text(pass_lbl, "Pass");
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy + 4);

    s_wcfg_pass_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_wcfg_pass_ta, form_w - 80, field_h);
    lv_obj_align(s_wcfg_pass_ta, LV_ALIGN_TOP_LEFT, 80, 56 + row_dy);
    lv_textarea_set_one_line(s_wcfg_pass_ta, true);
    lv_textarea_set_max_length(s_wcfg_pass_ta, APP_CONFIG_PASS_MAX - 1);
    lv_textarea_set_password_mode(s_wcfg_pass_ta, true);
    lv_obj_add_event_cb(s_wcfg_pass_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_wcfg_show_pass_cb = lv_checkbox_create(ov);
    lv_checkbox_set_text(s_wcfg_show_pass_cb, "Show password");
    lv_obj_align(s_wcfg_show_pass_cb, LV_ALIGN_TOP_LEFT, 80, 56 + row_dy * 2);
    lv_obj_add_event_cb(s_wcfg_show_pass_cb, on_wcfg_show_pass_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_wcfg_status_label = lv_label_create(ov);
    lv_label_set_text(s_wcfg_status_label, "");
    lv_label_set_recolor(s_wcfg_status_label, true);
    lv_obj_align(s_wcfg_status_label, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy * 3);

    s_wcfg_keyboard = lv_keyboard_create(ov);
    lv_obj_set_size(s_wcfg_keyboard, SCREEN_W - 32, SCREEN_H / 2);
    lv_obj_align(s_wcfg_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_wcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wcfg_keyboard, on_wcfg_keyboard_event, LV_EVENT_ALL, NULL);
}

static void wcfg_open(void)
{
    if (!s_wcfg_overlay) build_wcfg_overlay();
    const char *ssid = app_config_wifi_ssid();
    const char *pass = app_config_wifi_pass();
    lv_textarea_set_text(s_wcfg_ssid_ta, ssid);
    lv_textarea_set_text(s_wcfg_pass_ta, pass);
    strncpy(s_wcfg_orig_ssid, ssid, sizeof(s_wcfg_orig_ssid) - 1);
    s_wcfg_orig_ssid[sizeof(s_wcfg_orig_ssid) - 1] = '\0';
    strncpy(s_wcfg_orig_pass, pass, sizeof(s_wcfg_orig_pass) - 1);
    s_wcfg_orig_pass[sizeof(s_wcfg_orig_pass) - 1] = '\0';
    lv_textarea_set_password_mode(s_wcfg_pass_ta, true);
    lv_obj_remove_state(s_wcfg_show_pass_cb, LV_STATE_CHECKED);
    lv_label_set_text(s_wcfg_status_label, "");
    lv_obj_add_flag(s_wcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wcfg_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wcfg_overlay);
}

static void wcfg_close(void)
{
    if (s_wcfg_overlay) lv_obj_add_flag(s_wcfg_overlay, LV_OBJ_FLAG_HIDDEN);
}

// --- MS panel ------------------------------------------------------------

static bool mcfg_has_unsaved_changes(void)
{
    return strcmp(lv_textarea_get_text(s_mcfg_host_ta), s_mcfg_orig_host) != 0 ||
           strcmp(lv_textarea_get_text(s_mcfg_port_ta), s_mcfg_orig_port) != 0;
}

static void build_mcfg_discard_confirm(void);

static void on_mcfg_close(lv_event_t *e)
{
    (void)e;
    mcfg_hide_keyboard();
    if (mcfg_has_unsaved_changes()) {
        if (!s_mcfg_discard_confirm) build_mcfg_discard_confirm();
        lv_obj_remove_flag(s_mcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mcfg_discard_confirm);
        return;
    }
    mcfg_close();
}

static void on_mcfg_discard_yes(lv_event_t *e)
{
    (void)e;
    if (s_mcfg_discard_confirm) lv_obj_add_flag(s_mcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
    mcfg_close();
}

static void on_mcfg_discard_no(lv_event_t *e)
{
    (void)e;
    if (s_mcfg_discard_confirm) lv_obj_add_flag(s_mcfg_discard_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void build_mcfg_discard_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 460, 200);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_mcfg_discard_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg, "Unsaved changes will be lost.\nDiscard and close?");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Keep Editing");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_mcfg_discard_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 160, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0xC04040), 0);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "Discard");
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes, on_mcfg_discard_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void on_mcfg_textarea_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    if (s_mcfg_keyboard) {
        lv_keyboard_set_textarea(s_mcfg_keyboard, ta);
        lv_keyboard_set_mode(s_mcfg_keyboard,
                             ta == s_mcfg_port_ta ? LV_KEYBOARD_MODE_NUMBER
                                                  : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_mcfg_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        mcfg_hide_keyboard();
    }
}

static void on_mcfg_save(lv_event_t *e)
{
    (void)e;
    mcfg_hide_keyboard();
    const char *host   = lv_textarea_get_text(s_mcfg_host_ta);
    const char *port_s = lv_textarea_get_text(s_mcfg_port_ta);
    long port = strtol(port_s, NULL, 10);
    if (strlen(host) == 0) {
        lv_label_set_text(s_mcfg_status_label,
                          "#FF6060 Host cannot be empty.#");
        return;
    }
    if (port <= 0 || port > 65535) {
        lv_label_set_text(s_mcfg_status_label,
                          "#FF6060 Invalid port (1-65535)#");
        return;
    }
    bool ok = app_config_set_ms_host(host) &&
              app_config_set_ms_port((uint16_t) port);
    if (!ok) {
        lv_label_set_text(s_mcfg_status_label,
                          "#FF6060 Save failed (NVS error).#");
        return;
    }

    // Re-snapshot originals so the X close path doesn't think we still
    // have unsaved changes from the just-committed edit.
    strncpy(s_mcfg_orig_host, host, sizeof(s_mcfg_orig_host) - 1);
    s_mcfg_orig_host[sizeof(s_mcfg_orig_host) - 1] = '\0';
    strncpy(s_mcfg_orig_port, port_s, sizeof(s_mcfg_orig_port) - 1);
    s_mcfg_orig_port[sizeof(s_mcfg_orig_port) - 1] = '\0';

    lv_label_set_text(s_mcfg_status_label,
                      "#40C060 Saved. Reconnecting to MS...#");

    // Live-apply by kicking the WS client to recreate against the new
    // host:port. No reboot needed -- the WS task tears down + spins up
    // with whatever app_config now returns. Status icon will flip to
    // CONNECTING then back to CONNECTED via the existing event path.
    if (s_ms && s_ms->reconnect) s_ms->reconnect();
}

static void build_mcfg_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_mcfg_overlay = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Mixing Station");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(ov);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_mcfg_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_button_create(ov);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x40C060), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_OK " Save");
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, on_mcfg_save, LV_EVENT_CLICKED, NULL);

    const int field_h = 36;
    const int row_dy  = 56;
    const int form_w  = SCREEN_W - 32;

    lv_obj_t *host_lbl = lv_label_create(ov);
    lv_label_set_text(host_lbl, "Host");
    lv_obj_align(host_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + 4);
    s_mcfg_host_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_mcfg_host_ta, form_w - 80, field_h);
    lv_obj_align(s_mcfg_host_ta, LV_ALIGN_TOP_LEFT, 80, 56);
    lv_textarea_set_one_line(s_mcfg_host_ta, true);
    lv_textarea_set_max_length(s_mcfg_host_ta, APP_CONFIG_HOST_MAX - 1);
    lv_obj_add_event_cb(s_mcfg_host_ta, on_mcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    lv_obj_t *port_lbl = lv_label_create(ov);
    lv_label_set_text(port_lbl, "Port");
    lv_obj_align(port_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy + 4);
    s_mcfg_port_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_mcfg_port_ta, 140, field_h);
    lv_obj_align(s_mcfg_port_ta, LV_ALIGN_TOP_LEFT, 80, 56 + row_dy);
    lv_textarea_set_one_line(s_mcfg_port_ta, true);
    lv_textarea_set_max_length(s_mcfg_port_ta, 5);
    lv_obj_add_event_cb(s_mcfg_port_ta, on_mcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_mcfg_status_label = lv_label_create(ov);
    lv_label_set_text(s_mcfg_status_label, "");
    lv_label_set_recolor(s_mcfg_status_label, true);
    lv_obj_align(s_mcfg_status_label, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy * 2 + 4);

    s_mcfg_keyboard = lv_keyboard_create(ov);
    lv_obj_set_size(s_mcfg_keyboard, SCREEN_W - 32, SCREEN_H / 2);
    lv_obj_align(s_mcfg_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_mcfg_keyboard, on_mcfg_keyboard_event, LV_EVENT_ALL, NULL);
}

static void mcfg_open(void)
{
    if (!s_mcfg_overlay) build_mcfg_overlay();
    const char *host = app_config_ms_host();
    char port_s[8];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned) app_config_ms_port());
    lv_textarea_set_text(s_mcfg_host_ta, host);
    lv_textarea_set_text(s_mcfg_port_ta, port_s);
    strncpy(s_mcfg_orig_host, host, sizeof(s_mcfg_orig_host) - 1);
    s_mcfg_orig_host[sizeof(s_mcfg_orig_host) - 1] = '\0';
    strncpy(s_mcfg_orig_port, port_s, sizeof(s_mcfg_orig_port) - 1);
    s_mcfg_orig_port[sizeof(s_mcfg_orig_port) - 1] = '\0';
    lv_label_set_text(s_mcfg_status_label, "");
    lv_obj_add_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_mcfg_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_mcfg_overlay);
}

static void mcfg_close(void)
{
    if (s_mcfg_overlay) lv_obj_add_flag(s_mcfg_overlay, LV_OBJ_FLAG_HIDDEN);
}

// ─────────────────────────────────────────────────────────────────────────
// Channel picker overlay — pick up to APP_UI_MAX_TRACKED_CHANNELS inputs
// from the full set available on the connected console. Save persists
// to NVS and reboots so the fader UI rebuilds against the new selection
// at boot (the rebuild-while-live path has a known race -- see comment
// in esp_ui_main.c).
// ─────────────────────────────────────────────────────────────────────────

static int chpick_count_selected(void)
{
    int n = 0;
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;
    for (int i = 0; i < bound; ++i) if (s_chpick_state[i]) n++;
    return n;
}

static void chpick_refresh_count_label(void)
{
    if (!s_chpick_count_label) return;
    int n = chpick_count_selected();
    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d selected",
             n, APP_UI_MAX_TRACKED_CHANNELS);
    lv_label_set_text(s_chpick_count_label, buf);
}

// Disable unchecked rows when the cap is hit -- the user can still
// uncheck currently-checked rows to free a slot, but can't add more.
static void chpick_apply_disable_state(void)
{
    bool at_cap = chpick_count_selected() >= APP_UI_MAX_TRACKED_CHANNELS;
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;
    for (int i = 0; i < bound; ++i) {
        if (!s_chpick_checks[i]) continue;
        if (s_chpick_state[i]) {
            lv_obj_remove_state(s_chpick_checks[i], LV_STATE_DISABLED);
        } else if (at_cap) {
            lv_obj_add_state(s_chpick_checks[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_chpick_checks[i], LV_STATE_DISABLED);
        }
    }
}

static bool chpick_has_unsaved_changes(void)
{
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;
    for (int i = 0; i < bound; ++i) {
        if (s_chpick_state[i] != s_chpick_orig[i]) return true;
    }
    return false;
}

static void on_chpick_check_changed(lv_event_t *e)
{
    lv_obj_t *cb  = lv_event_get_target_obj(e);
    int       idx = (int)(intptr_t) lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_total_channels) return;
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
    // Enforce cap: if the user just turned this on and we're now over the
    // cap, refuse the change and revert the visual state.
    if (checked && !s_chpick_state[idx] &&
        chpick_count_selected() >= APP_UI_MAX_TRACKED_CHANNELS) {
        lv_obj_remove_state(cb, LV_STATE_CHECKED);
        lv_label_set_text(s_chpick_status_label,
                          "#FF6060 At maximum -- uncheck another channel first.#");
        return;
    }
    s_chpick_state[idx] = checked;
    lv_label_set_text(s_chpick_status_label, "");
    chpick_refresh_count_label();
    chpick_apply_disable_state();
}

static void build_chpick_discard_confirm(void);

static void on_chpick_close(lv_event_t *e)
{
    (void)e;
    if (chpick_has_unsaved_changes()) {
        if (!s_chpick_discard_confirm) build_chpick_discard_confirm();
        lv_obj_remove_flag(s_chpick_discard_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_chpick_discard_confirm);
        return;
    }
    chpick_close();
}

static void on_chpick_discard_yes(lv_event_t *e)
{
    (void)e;
    if (s_chpick_discard_confirm) lv_obj_add_flag(s_chpick_discard_confirm, LV_OBJ_FLAG_HIDDEN);
    chpick_close();
}

static void on_chpick_discard_no(lv_event_t *e)
{
    (void)e;
    if (s_chpick_discard_confirm) lv_obj_add_flag(s_chpick_discard_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void build_chpick_discard_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 460, 200);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_chpick_discard_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg, "Unsaved channel changes will be lost.\nDiscard and close?");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Keep Editing");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, on_chpick_discard_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 160, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0xC04040), 0);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, "Discard");
    lv_obj_center(yl);
    lv_obj_add_event_cb(yes, on_chpick_discard_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

// Save-confirm popup. Save applies via reboot (the rebuild-while-live
// path has a known race -- see esp_ui_main.c safe_max comment), and
// users would be surprised by a chip reset they didn't agree to. So
// Save shows this confirm first.
static lv_obj_t *s_chpick_save_confirm;

static void on_chpick_save_yes(lv_event_t *e)
{
    (void)e;
    int  ids[APP_CONFIG_MAX_CHANNELS];
    int  out = 0;
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;
    for (int i = 0; i < bound && out < APP_UI_MAX_TRACKED_CHANNELS; ++i) {
        // i is the MS channel id directly -- the picker covers ids 0..total-1
        if (s_chpick_state[i]) ids[out++] = i;
    }
    if (!app_config_set_channel_ids(ids, (size_t) out)) {
        if (s_chpick_save_confirm) lv_obj_add_flag(s_chpick_save_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_chpick_status_label,
                          "#FF6060 Save failed (NVS error).#");
        return;
    }
    lv_label_set_text(s_chpick_status_label, "#40C060 Saved. Restarting...#");
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void on_chpick_save_no(lv_event_t *e)
{
    (void)e;
    if (s_chpick_save_confirm) lv_obj_add_flag(s_chpick_save_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void build_chpick_save_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 480, 220);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_chpick_save_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg,
                      "Save selection and restart the device?\n"
                      "The fader UI will rebuild against the new channels.");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, on_chpick_save_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 200, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x40C060), 0);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, LV_SYMBOL_OK " Save & Restart");
    lv_obj_center(yl);
    lv_obj_add_event_cb(yes, on_chpick_save_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void on_chpick_save(lv_event_t *e)
{
    (void)e;
    int n = chpick_count_selected();
    if (n == 0) {
        lv_label_set_text(s_chpick_status_label,
                          "#FF6060 Pick at least one channel.#");
        return;
    }
    if (!s_chpick_save_confirm) build_chpick_save_confirm();
    lv_obj_remove_flag(s_chpick_save_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_chpick_save_confirm);
}

static void build_chpick_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    s_chpick_overlay = ov;

    lv_obj_t *title = lv_label_create(ov);
    lv_label_set_text(title, "Edit Channels");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(ov);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_chpick_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_button_create(ov);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_TOP_LEFT, 0, -4);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x40C060), 0);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_OK " Save");
    lv_obj_center(save_lbl);
    lv_obj_add_event_cb(save_btn, on_chpick_save, LV_EVENT_CLICKED, NULL);

    s_chpick_count_label = lv_label_create(ov);
    lv_obj_align(s_chpick_count_label, LV_ALIGN_TOP_MID, 0, 30);

    s_chpick_status_label = lv_label_create(ov);
    lv_label_set_text(s_chpick_status_label, "");
    lv_label_set_recolor(s_chpick_status_label, true);
    lv_obj_align(s_chpick_status_label, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    // Tight 4-col layout so 80 channels fit on one screen (20 rows tall).
    // Si Expression: total=80, so 4*20 fills exactly; smaller boards
    // wrap into fewer rows. Status label sits at the bottom edge of
    // the overlay so the list can claim the full vertical band.
    //
    // COLUMN_WRAP layout: cells fill top-to-bottom in the first column,
    // then wrap into the second column, etc. -- channel order reads
    // CH 01..CH 20 down col 1, CH 21..CH 40 down col 2, and so on.
    // Matches the user's mental model of "channel 17 is just below 16,
    // not way over to the right".
    s_chpick_list = lv_obj_create(ov);
    lv_obj_set_size(s_chpick_list, SCREEN_W - 32, SCREEN_H - 92);
    lv_obj_align(s_chpick_list, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_set_style_pad_all(s_chpick_list, 4, 0);
    lv_obj_set_style_pad_row(s_chpick_list, 2, 0);
    lv_obj_set_style_pad_column(s_chpick_list, 6, 0);
    lv_obj_set_layout(s_chpick_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_chpick_list, LV_FLEX_FLOW_COLUMN_WRAP);
}

static void chpick_open(void)
{
    if (s_total_channels <= 0) {
        // No total from MS yet -- the picker would be empty. Bail silently;
        // the Edit Channels button stays harmless.
        return;
    }
    if (!s_chpick_overlay) build_chpick_overlay();

    // Rebuild rows each open so a reconnect to a different console doesn't
    // leave stale row count.
    lv_obj_clean(s_chpick_list);
    memset(s_chpick_checks, 0, sizeof(s_chpick_checks));
    memset(s_chpick_state,  0, sizeof(s_chpick_state));
    memset(s_chpick_orig,   0, sizeof(s_chpick_orig));

    // Clamp to our PSRAM array size. Si Expression: 80; cap is 128.
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;

    // Seed working state from the persisted selection. cur entries are MS
    // channel ids, which index directly into our 0..total-1 row layout.
    size_t cur_count = 0;
    const int *cur = app_config_channel_ids(&cur_count);
    for (int i = 0; i < bound; ++i) {
        for (size_t j = 0; j < cur_count; ++j) {
            if (cur[j] == i) {
                s_chpick_state[i] = true;
                break;
            }
        }
        s_chpick_orig[i] = s_chpick_state[i];
    }

    // 4 columns × 20 rows fits Si Expression's 80 channels with no scroll.
    // Row width math: list inner = SCREEN_W - 32 - 8 (pad_all*2) = 984;
    // minus 3 column gaps × 6 px = 18 → 966 / 4 = 241 per cell.
    // Row height 22 + pad_row 2 = 24 each; 20 × 24 - 2 = 478 < list inner
    // height (~496) so 20 rows fit.
    const int row_w = (SCREEN_W - 32 - 8 - 18) / 4;
    const int row_h = 22;
    for (int i = 0; i < bound; ++i) {
        lv_obj_t *row = lv_obj_create(s_chpick_list);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *cb = lv_checkbox_create(row);
        char buf[24];
        snprintf(buf, sizeof(buf), "CH %02d", i + 1);
        lv_checkbox_set_text(cb, buf);
        if (s_chpick_state[i]) lv_obj_add_state(cb, LV_STATE_CHECKED);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(cb, on_chpick_check_changed,
                            LV_EVENT_VALUE_CHANGED, (void *)(intptr_t) i);
        s_chpick_checks[i] = cb;
    }

    chpick_refresh_count_label();
    chpick_apply_disable_state();
    lv_label_set_text(s_chpick_status_label, "");
    lv_obj_remove_flag(s_chpick_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_chpick_overlay);
}

static void chpick_close(void)
{
    if (s_chpick_overlay) lv_obj_add_flag(s_chpick_overlay, LV_OBJ_FLAG_HIDDEN);
}

void app_ui_set_channel_total(int count)
{
    if (count < 0) count = 0;
    s_total_channels = count;
    // The Settings overlay may have been built before MS info arrived;
    // reveal the Edit Channels button now if so. (If the overlay hasn't
    // been built yet, build_settings_overlay reads s_total_channels and
    // creates the button hidden/visible appropriately.)
    if (s_edit_channels_btn) {
        if (count > 0) lv_obj_remove_flag(s_edit_channels_btn, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag   (s_edit_channels_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_edit_channels_clicked(lv_event_t *e)
{
    (void)e;
    chpick_open();
}

// ─────────────────────────────────────────────────────────────────────────
// Toast — short auto-dismissing message at the center of the screen. Used
// today only by the disabled-mute path; kept generic so it can carry
// other inline feedback (e.g. "Saved" after a settings write) later.
// ─────────────────────────────────────────────────────────────────────────

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    // The timer was created with repeat_count=1 and is about to auto-delete
    // itself once this callback returns. Drop our pointer so the next
    // toast_show creates a fresh one instead of reusing a dangling handle —
    // that bug is what kept "Mute disabled" pinned to the screen.
    s_toast_timer = NULL;
}

static void build_toast(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *t = lv_obj_create(scr);
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_pad_all(t, 16, 0);
    lv_obj_set_style_radius(t, 8, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    // Bright amber on dark text — high contrast against the dark theme's
    // near-black backgrounds and the saturated slider colors. Default theme
    // styling rendered as dark-grey-on-dark-grey, which user reported as
    // hard to see when a slider was nearby.
    lv_obj_set_style_bg_color(t, lv_color_hex(0xF0B030), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);

    s_toast_label = lv_label_create(t);
    lv_label_set_text(s_toast_label, "");
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0x101010), 0);
    lv_obj_center(s_toast_label);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
}

static void toast_show(const char *text)
{
    if (!s_toast) build_toast();
    lv_label_set_text(s_toast_label, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) {
        lv_timer_set_period(s_toast_timer, 2000);
        lv_timer_reset(s_toast_timer);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    } else {
        s_toast_timer = lv_timer_create(toast_hide, 2000, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Controls enabled state — sliders + mute buttons are gated by MS
// connection state; mute additionally requires the user-facing Mute
// Enabled toggle to be ON. Greyed-out (50% opa) is the universal "you
// can't act on this right now" hint.
// ─────────────────────────────────────────────────────────────────────────

static void apply_controls_enabled(void)
{
    bool ms_ok   = (s_ms && s_ms->get_state &&
                    s_ms->get_state() == APP_MS_STATE_CONNECTED);
    bool mute_ok = ms_ok && s_mute_enabled;

    for (size_t i = 0; i < APP_CONFIG_MAX_CHANNELS; ++i) {
        if (s_widgets[i].slider) {
            lv_obj_set_style_opa(s_widgets[i].slider,
                                 ms_ok ? LV_OPA_COVER : LV_OPA_50, 0);
            if (ms_ok) lv_obj_add_flag(s_widgets[i].slider,    LV_OBJ_FLAG_CLICKABLE);
            else       lv_obj_remove_flag(s_widgets[i].slider, LV_OBJ_FLAG_CLICKABLE);
        }
        if (s_widgets[i].btn_mute) {
            // Always clickable so we can show the toast on a disabled tap;
            // the click handler decides whether to act. Greyed when not
            // mute_ok so the visual matches the behavior.
            lv_obj_set_style_opa(s_widgets[i].btn_mute,
                                 mute_ok ? LV_OPA_COVER : LV_OPA_50, 0);
        }
    }

    if (s_mute_en_btn) {
        if (s_mute_enabled) lv_obj_add_state   (s_mute_en_btn, LV_STATE_CHECKED);
        else                lv_obj_remove_state(s_mute_en_btn, LV_STATE_CHECKED);
    }
}

static void on_mute_en_clicked(lv_event_t *e)
{
    (void) e;
    s_mute_enabled = !s_mute_enabled;
    apply_controls_enabled();
}

// ─────────────────────────────────────────────────────────────────────────
// WiFi info panel — read-only summary of the current connection. Editing
// (SSID/password/static IP) is queued for a follow-up that needs the
// on-screen keyboard + creds-in-prefs migration.
// ─────────────────────────────────────────────────────────────────────────

static uint32_t wifi_state_color(app_wifi_state_t s)
{
    switch (s) {
        case APP_WIFI_STATE_CONNECTED:  return 0x40C040;  // green
        case APP_WIFI_STATE_CONNECTING: return 0xE0D040;  // yellow
        case APP_WIFI_STATE_FAILED:     return 0xC04040;  // red
        case APP_WIFI_STATE_BOOT:
        default:                         return 0x808080;  // grey
    }
}

static const char *wifi_state_text(app_wifi_state_t s)
{
    switch (s) {
        case APP_WIFI_STATE_CONNECTED:  return "Connected";
        case APP_WIFI_STATE_CONNECTING: return "Connecting...";
        case APP_WIFI_STATE_FAILED:     return "Failed";
        case APP_WIFI_STATE_BOOT:
        default:                         return "Booting";
    }
}

static void wifi_icon_refresh(void)
{
    if (!s_wifi_icon_label) return;
    lv_obj_set_style_text_color(s_wifi_icon_label,
                                lv_color_hex(wifi_state_color(app_wifi_get_state())),
                                0);
}

static void wifi_panel_refresh(void)
{
    if (!s_wifi_panel) return;
    app_wifi_state_t st = app_wifi_get_state();
    lv_label_set_text(s_wifi_state_value, wifi_state_text(st));
    lv_obj_set_style_text_color(s_wifi_state_value,
                                lv_color_hex(wifi_state_color(st)), 0);
    lv_label_set_text(s_wifi_ssid_value, app_wifi_get_ssid());
    char ip[16];
    app_wifi_format_ip(ip, sizeof(ip));
    lv_label_set_text(s_wifi_ip_value, ip);
}

// Async trampoline for state changes — the wifi event task can't touch LVGL
// directly, so we ride lv_async_call into the LVGL task.
// Clock — replaces the centered status text once SNTP has produced a real
// time. America/Los_Angeles is hardcoded for now; revisit if the device
// ever leaves the West Coast.
static lv_timer_t *s_clock_timer;
static bool        s_sntp_started;

static void clock_tick(lv_timer_t *t)
{
    (void) t;
    if (!s_status_label) return;
    time_t    now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year < (2024 - 1900)) {
        // SNTP hasn't returned yet — show a placeholder so the user sees
        // the slot exists but knows time isn't available.
        lv_label_set_text(s_status_label, "--:-- --");
        return;
    }
    char buf[16];
    // %l = space-padded hour, no leading zero (matches typical 12-hour
    // wall-clock formatting).
    strftime(buf, sizeof(buf), "%l:%M %p", &lt);
    const char *p = (buf[0] == ' ') ? buf + 1 : buf;
    lv_label_set_text(s_status_label, p);
}

static void start_clock_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    // POSIX TZ for America/Los_Angeles with US DST rules. Stored statically
    // so setenv's reference outlives the call.
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Two-server SNTP setup: slot 0 is reserved for the DHCP-supplied NTP
    // server (option 42, gated by CONFIG_LWIP_DHCP_GET_NTP_SRV); slot 1 is
    // a US-pool static fallback for networks that don't advertise an NTP
    // server in DHCP. The static slot is also used when a static IP is
    // configured (no DHCP, no option 42). Renew on every new IP so a lease
    // change picks up a fresh DHCP-supplied server.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("0.us.pool.ntp.org", "0.us.pool.ntp.org"));
    cfg.server_from_dhcp           = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.index_of_first_server      = 1;
    cfg.ip_event_to_renew          = IP_EVENT_STA_GOT_IP;
    esp_netif_sntp_init(&cfg);

    if (!s_clock_timer) {
        s_clock_timer = lv_timer_create(clock_tick, 1000, NULL);
        lv_timer_ready(s_clock_timer);  // run once now so the placeholder appears
    }
}

static void wifi_apply_async(void *unused)
{
    (void)unused;
    wifi_icon_refresh();
    if (s_wifi_panel && !lv_obj_has_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN)) {
        wifi_panel_refresh();
    }
    // SNTP requires a working route; kick it once the first time wifi
    // reaches CONNECTED. The clock label takes over from the boot status
    // text (which was last set to "Connecting WiFi...") at the same time.
    if (app_wifi_get_state() == APP_WIFI_STATE_CONNECTED) {
        start_clock_once();
    }
}

// Same coalescing pattern as ms_apply / apply_pending — wifi reconnect
// retries can fire on_event repeatedly; one async sweep per arrival is
// enough.
static volatile bool s_wifi_apply_queued;

static void wifi_apply_async_wrap(void *unused)
{
    s_wifi_apply_queued = false;
    wifi_apply_async(unused);
}

static void on_wifi_state_change(void *ctx)
{
    (void)ctx;
    if (s_wifi_apply_queued) return;
    if (!lvgl_port_lock(100)) return;
    if (!s_wifi_apply_queued) {
        s_wifi_apply_queued = true;
        if (lv_async_call(wifi_apply_async_wrap, NULL) != LV_RESULT_OK) {
            s_wifi_apply_queued = false;
        }
    }
    lvgl_port_unlock();
}

static void on_wifi_panel_close_clicked(lv_event_t *e)
{
    (void)e;
    wifi_panel_close();
}

static void build_wifi_panel(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 600, 280);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_panel = p;

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "WiFi");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_button_create(p);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_wifi_panel_close_clicked, LV_EVENT_CLICKED, NULL);

    // Three rows: State / SSID / IP. Label on the left, value on the right.
    const int row_y[3] = { 60, 110, 160 };
    const char *labels[3] = { "State", "SSID", "IP Address" };
    lv_obj_t **values[3] = { &s_wifi_state_value, &s_wifi_ssid_value, &s_wifi_ip_value };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *l = lv_label_create(p);
        lv_label_set_text(l, labels[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, row_y[i]);

        lv_obj_t *v = lv_label_create(p);
        lv_label_set_text(v, "—");
        lv_obj_align(v, LV_ALIGN_TOP_LEFT, 180, row_y[i]);
        *(values[i]) = v;
    }

    lv_obj_t *note = lv_label_create(p);
    lv_label_set_text(note, "Editing comes in a follow-up - needs on-screen keyboard");
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void wifi_panel_open(void)
{
    if (!s_wifi_panel) build_wifi_panel();
    wifi_panel_refresh();
    lv_obj_remove_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_panel);
}

static void wifi_panel_close(void)
{
    if (s_wifi_panel) {
        lv_obj_add_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wifi_clicked(lv_event_t *e)
{
    (void)e;
    // WiFi icon -> WiFi-only editor. The old read-only wifi_panel build
    // code is still around but unreferenced (cleanup is a follow-up).
    wcfg_open();
}

// ─────────────────────────────────────────────────────────────────────────
// MS info panel — same shape as the WiFi panel: read-only state / host /
// port. Editable fields land alongside the WiFi editor in the follow-up
// task (#38).
// ─────────────────────────────────────────────────────────────────────────

static uint32_t ms_state_color(app_ms_state_t s)
{
    switch (s) {
        case APP_MS_STATE_CONNECTED:    return 0x40C040;  // green
        case APP_MS_STATE_CONNECTING:   return 0xE0D040;  // yellow
        case APP_MS_STATE_DISCONNECTED: return 0xC04040;  // red — same hue as
        case APP_MS_STATE_ERROR:        return 0xC04040;  // wifi-failed
        case APP_MS_STATE_BOOT:
        default:                         return 0x808080;  // grey
    }
}

static const char *ms_state_text(app_ms_state_t s)
{
    switch (s) {
        case APP_MS_STATE_CONNECTED:    return "Connected";
        case APP_MS_STATE_CONNECTING:   return "Connecting...";
        case APP_MS_STATE_DISCONNECTED: return "Disconnected";
        case APP_MS_STATE_ERROR:        return "Error";
        case APP_MS_STATE_BOOT:
        default:                         return "Booting";
    }
}

static app_ms_state_t ms_state_now(void)
{
    return (s_ms && s_ms->get_state) ? s_ms->get_state() : APP_MS_STATE_BOOT;
}

static void ms_icon_refresh(void)
{
    if (!s_ms_icon_label) return;
    lv_obj_set_style_text_color(s_ms_icon_label,
                                lv_color_hex(ms_state_color(ms_state_now())),
                                0);
}

static void ms_panel_refresh(void)
{
    if (!s_ms_panel) return;
    app_ms_state_t st = ms_state_now();
    lv_label_set_text(s_ms_state_value, ms_state_text(st));
    lv_obj_set_style_text_color(s_ms_state_value,
                                lv_color_hex(ms_state_color(st)), 0);
    const char *host = (s_ms && s_ms->get_host) ? s_ms->get_host() : "—";
    int port         = (s_ms && s_ms->get_port) ? s_ms->get_port() : 0;
    lv_label_set_text(s_ms_host_value, host);
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    lv_label_set_text(s_ms_port_value, portbuf);
}

static void mix_picker_refresh_labels(void)
{
    if (!s_mix_picker_popup || !s_ms || !s_ms->get_mix_name) return;
    for (int i = 0; i < s_mix_count && i < (int)(sizeof(s_mix_picker_btn_labels) /
                                                  sizeof(s_mix_picker_btn_labels[0])); ++i) {
        if (!s_mix_picker_btn_labels[i]) continue;
        const char *name = s_ms->get_mix_name(i);
        char buf[24];
        if (name) snprintf(buf, sizeof(buf), "%s", name);
        else      snprintf(buf, sizeof(buf), "Mix %d", i + 1);
        lv_label_set_text(s_mix_picker_btn_labels[i], buf);
    }
}

static void ms_apply_async(void *unused)
{
    (void)unused;
    ms_icon_refresh();
    mix_indicator_refresh();
    // Update labels in place rather than tearing down the popup. The
    // earlier "delete on every notify" pattern accumulated heap churn
    // (~31 LVGL widgets per rebuild × every mix change) and after a
    // long session the LVGL allocator's free-list looped or
    // corrupted, hanging taskLVGL inside lv_malloc.
    mix_picker_refresh_labels();
    if (s_ms_panel && !lv_obj_has_flag(s_ms_panel, LV_OBJ_FLAG_HIDDEN)) {
        ms_panel_refresh();
    }
    // Faders + mute buttons are gated by MS connection — re-evaluate every
    // time the state transitions so the user can't drag a slider into the
    // void during an outage.
    apply_controls_enabled();

    // Toggle the spinner ↔ fader tileview based on MS connection. Hidden
    // strips during an outage prevent the user from interacting with stale
    // values; the spinner makes the waiting state explicit.
    bool ms_connected = (s_ms && s_ms->get_state &&
                         s_ms->get_state() == APP_MS_STATE_CONNECTED);
    if (s_tileview) {
        if (ms_connected) lv_obj_remove_flag(s_tileview, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag   (s_tileview, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_spinner) {
        if (ms_connected) lv_obj_add_flag   (s_spinner, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_remove_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_spinner_label) {
        if (ms_connected) lv_obj_add_flag   (s_spinner_label, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_remove_flag(s_spinner_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// Coalesce ms_apply_async — a burst of MS broadcasts (e.g. 14 mix-name
// initial values arriving back-to-back at startup) would otherwise queue
// 14 redundant async sweeps. apply_pending uses the same s_sweep_queued
// pattern for the per-channel state-change path.
static volatile bool s_ms_apply_queued;

static void ms_apply_async_wrap(void *unused)
{
    s_ms_apply_queued = false;
    ms_apply_async(unused);
}

static void on_ms_state_change(void *ctx)
{
    (void)ctx;
    if (s_ms_apply_queued) return;
    if (!lvgl_port_lock(100)) return;
    if (!s_ms_apply_queued) {
        s_ms_apply_queued = true;
        if (lv_async_call(ms_apply_async_wrap, NULL) != LV_RESULT_OK) {
            s_ms_apply_queued = false;
        }
    }
    lvgl_port_unlock();
}

static void on_ms_panel_close_clicked(lv_event_t *e)
{
    (void)e;
    ms_panel_close();
}

static void build_ms_panel(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 600, 280);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_ms_panel = p;

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "Mixing Station");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close_btn = lv_button_create(p);
    lv_obj_set_size(close_btn, 36, 36);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_add_event_cb(close_btn, on_ms_panel_close_clicked, LV_EVENT_CLICKED, NULL);

    const int row_y[3] = { 60, 110, 160 };
    const char *labels[3] = { "State", "Host", "Port" };
    lv_obj_t **values[3] = { &s_ms_state_value, &s_ms_host_value, &s_ms_port_value };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *l = lv_label_create(p);
        lv_label_set_text(l, labels[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, row_y[i]);

        lv_obj_t *v = lv_label_create(p);
        lv_label_set_text(v, "—");
        lv_obj_align(v, LV_ALIGN_TOP_LEFT, 180, row_y[i]);
        *(values[i]) = v;
    }

    lv_obj_t *note = lv_label_create(p);
    lv_label_set_text(note, "Editing comes in a follow-up - needs on-screen keyboard");
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void ms_panel_open(void)
{
    if (!s_ms_panel) build_ms_panel();
    ms_panel_refresh();
    lv_obj_remove_flag(s_ms_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ms_panel);
}

static void ms_panel_close(void)
{
    if (s_ms_panel) {
        lv_obj_add_flag(s_ms_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_ms_clicked(lv_event_t *e)
{
    (void)e;
    // MS icon -> MS-only editor. Save here is a live ws_reconnect rather
    // than a reboot; host/port can be applied without a full chip reset.
    mcfg_open();
}
