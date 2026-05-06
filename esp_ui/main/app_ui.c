#include "app_ui.h"
#include "app_display.h"
#include "app_logd.h"
#include "app_prefs.h"
#include "app_state.h"
#include "app_time.h"
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
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

// Custom font for level labels — Montserrat 14 plus U+221E (infinity glyph)
// so the floor reads "-INF" with the math symbol instead of the ASCII word.
// Default LVGL Montserrat doesn't ship the glyph; only the level label uses
// this font to keep the flash hit minimal.
extern const lv_font_t font_monmix_level;

static const char *TAG = "app_ui";

// Splash screen logo. Shown over the dark screen bg during boot until the
// fader UI mounts (i.e. until app_ui_present_channels finishes). Without
// this the user sees a long blank period between display init and the first
// real widget; the splash gives them something branded to look at.
LV_IMAGE_DECLARE(splash_logo);
static lv_obj_t *s_splash_screen;
static lv_obj_t *s_splash_logo_img;

// Layout:
//   1024x600 panel (LVGL software-rotated 180°). Each "page" is one tile in
//   an lv_tileview. 12 faders fit across 1024 px (~85 px slot, 72 px box):
//   the default 12-channel config sits on a single page so a musician sees
//   their full mix at a glance. Reconfiguring to >12 channels overflows
//   into a 2nd page and the indicator dots show up automatically.
// 11 input strips per page + the master strip pinned outside the tileview =
// 12 total strips visible at once. The picker cap (22) is 2 pages worth so
// both pages can be filled.
//
// (Earlier 16-per-page trial fit but 2-character channel names wrapped to
// two lines and read poorly during a show.)
#define FADERS_PER_PAGE     11
#define MAX_PAGES           ((APP_CONFIG_MAX_CHANNELS + FADERS_PER_PAGE - 1) / FADERS_PER_PAGE)

#define SCREEN_W            1024
#define SCREEN_H            600
#define STATUS_H            32
#define INDICATOR_H         28
#define TILEVIEW_Y          STATUS_H
#define TILEVIEW_H          (SCREEN_H - STATUS_H - INDICATOR_H)
// Master strip — pinned to the right edge outside the tileview so the mix-
// bus output is always reachable regardless of which input page is showing.
#define MASTER_STRIP_W      88
#define TILEVIEW_W          (SCREEN_W - MASTER_STRIP_W)
// 11 slots in 936 px = 85 px each; box width 78 leaves a 7 px gap.
#define FADER_BOX_W         78
#define FADER_BOX_H         500
#define FADER_BOX_PAD       8
#define SLIDER_W            28
// Slider top-aligned at SLIDER_TOP_Y inside the box (NOT CENTER aligned
// like before — that would push the slider down by half the height
// difference and undo the upward extension). Vertical budget within the
// 484 px inner area, top-to-bottom: name label (3 lines wrap, height 60)
// + 6 px gap + signal dot (10 px at y=66..76) + 6 px gap + slider
// (y=82..382) + 42 px gap (room for the LVGL knob's overhang at
// value=0; the comment-of-record on the prior 320 px slider said even
// 320 had the knob kissing MUTE) + mute button (32 at bottom-mid offset
// -28 -> y=424..456) + value label (~20 at bottom).
#define SLIDER_H            300
#define SLIDER_TOP_Y        82
#define MUTE_BTN_W          50
#define MUTE_BTN_H          32
#define DOT_SIZE            12
#define DOT_GAP             10

typedef struct {
    lv_obj_t *box;            // outer strip box; reorder-mode highlight goes here
    lv_obj_t *slider;
    lv_obj_t *label_name;
    lv_obj_t *label_val;
    lv_obj_t *btn_mute;
    lv_obj_t *signal_dot;     // signal-present mode: small green dot
    lv_obj_t *meter_bar;      // #30 meter mode: vertical fill bar (lv_bar)
} fader_widgets_t;

#define SIGNAL_DOT_SIZE     10
// #30: meter bar dimensions. Sits flush along the slider's left edge so
// it tracks the slider visually without crowding the unity tick on the
// right. Height matches the slider's travel; width kept thin so the
// fader thumb still reads as the dominant control.
#define METER_BAR_W         6
#define METER_BAR_H         SLIDER_H
// dB range mapped onto the bar fill. -60 dB = empty, 0 dB = full. Values
// above 0 dB clamp to full; values below -60 clamp to empty. Si reports
// silence as -90 dB.
#define METER_DB_FLOOR      -60.0f
#define METER_DB_CEIL         0.0f
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
// Master strip — same widget shape as the input strips but a singleton
// (one mix-bus output at a time, retargeted on mix change). Sits outside
// the tileview, anchored to the right edge.
static fader_widgets_t          s_master_widgets;
static lv_obj_t                *s_status_label;
static lv_obj_t                *s_tileview;
static lv_obj_t                *s_page_tiles[MAX_PAGES];
static lv_obj_t                *s_page_dots[MAX_PAGES];
static size_t                   s_page_count;
static const ms_client_iface_t *s_ms;

// Settings overlay — declared up here so the gear button's event handler
// can reach it. Overlay is created lazily on first open.
static lv_obj_t *s_settings_overlay;
static lv_obj_t *s_gear_label;  // gear icon — text color flipped on theme change
EXT_RAM_BSS_ATTR static lv_obj_t *s_color_swatches[APP_CONFIG_MAX_CHANNELS];
EXT_RAM_BSS_ATTR static lv_obj_t *s_row_name_labels[APP_CONFIG_MAX_CHANNELS];
static lv_obj_t *s_lvl_norm_btn;
static lv_obj_t *s_lvl_db_btn;
static lv_obj_t *s_sig_buttons[3];   // none / signal-present / meter
static lv_obj_t *s_theme_buttons[2]; // dark / light
static lv_obj_t *s_rot_buttons[2];   // 0 deg / 180 deg
static lv_obj_t *s_bright_slider;
static lv_obj_t *s_bright_value_label;
static lv_obj_t *s_tz_dropdown;

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
// P5: forced-reveal override for the `mix-show` console command. When true,
// mix_indicator_apply_visibility ignores the (ms_connected && mix_list_ready)
// gate and unconditionally shows the button. Useful for diagnosing whether
// the button is hidden because of state-tracking or a layout/draw bug.
static bool      s_mix_force_show;
// P11: snapshot of the routed-mix mask the popup was last built against.
// When the live MS routing changes, ms_apply_async tears down the popup so
// the next open rebuilds it with the new layout.
static uint32_t  s_picker_routed_mask;

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
// WiFi settings panel (entry: WiFi icon). Save attempts a live reconfigure
// (esp_wifi_disconnect + set_config + connect) but always falls through to
// a Save & Restart confirmation -- the live path is opportunistic; the
// dialog tells the user it MAY reboot.
static lv_obj_t *s_wcfg_overlay;
static lv_obj_t *s_wcfg_ssid_ta;
static lv_obj_t *s_wcfg_pass_ta;
static lv_obj_t *s_wcfg_show_pass_cb;
static lv_obj_t *s_wcfg_keyboard;
static lv_obj_t *s_wcfg_status_label;
static lv_obj_t *s_wcfg_scan_btn;
static lv_obj_t *s_wcfg_scan_btn_label;
static lv_obj_t *s_wcfg_discard_confirm;
static lv_obj_t *s_wcfg_save_confirm;
static char     s_wcfg_orig_ssid[APP_CONFIG_SSID_MAX];
static char     s_wcfg_orig_pass[APP_CONFIG_PASS_MAX];

// Static-IP form group inside the WiFi panel. When the DHCP radio is
// selected the static fields are hidden; flipping to Static reveals them.
static lv_obj_t *s_wcfg_dhcp_btn;
static lv_obj_t *s_wcfg_static_btn;
static lv_obj_t *s_wcfg_static_group;   // holds the four IP textareas + labels
static lv_obj_t *s_wcfg_ip_ta;
static lv_obj_t *s_wcfg_nm_ta;
static lv_obj_t *s_wcfg_gw_ta;
static lv_obj_t *s_wcfg_dns_ta;
static lv_obj_t *s_wcfg_ntp_ta;
static lv_obj_t *s_wcfg_ntp_dhcp_cb;     // checkbox: honor DHCP-supplied NTP
static lv_obj_t *s_wcfg_current_ip_value; // read-only "what IP did we land?"
static bool     s_wcfg_use_static;       // working state of the radio
static bool     s_wcfg_orig_use_static;
static char     s_wcfg_orig_ip [APP_PREFS_IP_STR_MAX];
static char     s_wcfg_orig_nm [APP_PREFS_IP_STR_MAX];
static char     s_wcfg_orig_gw [APP_PREFS_IP_STR_MAX];
static char     s_wcfg_orig_dns[APP_PREFS_IP_STR_MAX];
static char     s_wcfg_orig_ntp[APP_PREFS_STR_MAX];
static bool     s_wcfg_orig_ntp_use_dhcp;

// MS connection settings panel (entry: MS icon). Save = ws_reconnect()
// (live), no reboot -- just kicks the WS client to use the new host:port.
static lv_obj_t *s_mcfg_overlay;
static lv_obj_t *s_mcfg_host_ta;
static lv_obj_t *s_mcfg_port_ta;
static lv_obj_t *s_mcfg_osc_port_ta;
static lv_obj_t *s_mcfg_proto_ws_btn;
static lv_obj_t *s_mcfg_proto_osc_btn;
static int       s_mcfg_proto_staged;  // staged selection until Save
static lv_obj_t *s_mcfg_keyboard;
static lv_obj_t *s_mcfg_status_label;
static lv_obj_t *s_mcfg_discard_confirm;
static lv_obj_t *s_mcfg_reboot_confirm;
static char     s_mcfg_orig_host[APP_CONFIG_HOST_MAX];
static char     s_mcfg_orig_port[8];
static char     s_mcfg_orig_osc_port[8];
static int      s_mcfg_orig_proto;     // app_ms_protocol_t snapshot at open

// SSID scan results popup. Used only by the WiFi panel.
static lv_obj_t *s_ssid_list_popup;
static lv_obj_t *s_ssid_list;
static lv_obj_t *s_ssid_list_spinner;        // shown while scan in progress
static lv_obj_t *s_ssid_list_scanning_label; // "Scanning..."
static bool      s_ssid_list_scanning;       // popup is in scanning state
static bool      s_ssid_list_retried_empty;  // already retried once on empty

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

// Drag-to-reorder. Long-press on a strip's name label enters reorder mode;
// on each pressing event we compare finger X against neighbour-box centres
// and swap entries when a centre is crossed. Persist + rebuild on release.
// State below is touched only from LVGL event callbacks, all under the
// LVGL task -- no extra locking needed.
static bool   s_reorder_active;
static size_t s_reorder_idx;             // logical slot currently being dragged
static size_t s_reorder_page;            // confine swap to one page (visual)
static int    s_reorder_ids[APP_CONFIG_MAX_CHANNELS];
static size_t s_reorder_count;
static void on_strip_long_pressed(lv_event_t *e);
static void on_strip_pressing(lv_event_t *e);
static void on_strip_released(lv_event_t *e);
static void reorder_exit(bool persist_and_rebuild);

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
static void picker_refresh_title(void);
static void rename_open(size_t channel_idx);
static void rename_close(void);
static void mix_picker_open(void);
static void mix_picker_close(void);
static void mix_picker_refresh_labels(void);
static void mix_indicator_refresh(void);
static void mix_indicator_apply_visibility(void);
static void on_mix_indicator_clicked(lv_event_t *e);
static void on_name_clicked(lv_event_t *e);
static void wcfg_open(void);
static void wcfg_close(void);
static void mcfg_open(void);
static void mcfg_close(void);
static void chpick_open(void);
static void chpick_close(void);
static void chpick_refresh_labels(void);
static void on_edit_channels_clicked(lv_event_t *e);

// Reboot confirmation popup — built lazily, modal, two buttons. esp_restart
// is called on the Yes path; Cancel just hides the popup.
static lv_obj_t *s_reboot_confirm;

// Full-screen "Rebooting..." modal shown briefly between the user
// pressing a save/reboot button and esp_restart() firing. Without it
// the save paths only flash a small inline status label that's easy
// to miss in the pre-reset window before the panel goes black.
static lv_obj_t *s_rebooting_overlay;
static void show_rebooting_overlay(void);
static void wifi_panel_open(void);
static void wifi_panel_close(void);
static void wifi_panel_refresh(void);
static void wifi_icon_refresh(void);
static void apply_sntp_config(void);
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

// Default timeout for lvgl_port_lock when called from a non-LVGL task.
// 1 s is the floor that empirically gives LVGL room to finish any
// reasonable in-flight render. The previous value (100 ms) silently
// dropped icon-state updates whenever LVGL was mid-render -- the wifi/ms
// icon would stay showing the stale state forever, indistinguishable
// to the user from "still disconnected." A real LVGL deadlock would
// still bound the calling event task within 1 s rather than starving
// it forever.
#define LVGL_LOCK_TIMEOUT_MS 1000
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
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
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
    float     position = (float)v / 100.0f;

    // Single subscription per channel: either lvl/norm (NORM) or lvl/val
    // (DB). Update the matching app_state field so apply_pending picks
    // the right one without waiting for the echo.
    app_level_format_t fmt = app_prefs_get_level_format();
    if (fmt == APP_LEVEL_FORMAT_DB) {
        float db = app_position_to_db(position);
        app_state_set_level_db(idx, db, false);
    } else {
        app_state_set_level(idx, position, false);
    }

    // Rate-limit outbound SETs to ~20 Hz per channel. Each SET produces a
    // server-snap echo on the same WS, so unlimited drag-frequency SETs
    // doubled into a flood that monopolized the websocket task on CPU 1.
    // The final value is sent on slider release, see on_slider_released.
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_last_send_ms[idx] >= SET_MIN_INTERVAL_MS) {
        send_level_now(idx, position);
    }

    // Local readout — value is known immediately from slider position, no
    // need to wait for the echo.
    char buf[12];
    if (fmt == APP_LEVEL_FORMAT_DB) {
        if (v <= 0) {
            snprintf(buf, sizeof(buf), "-\xe2\x88\x9e dB");
        } else {
            snprintf(buf, sizeof(buf), "%.0f dB", app_position_to_db(position));
        }
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

// Master strip rides the same dirty-flag pattern but with a single bit; the
// sweep is folded into apply_pending so a master change still uses one
// queued async (not two).
static volatile bool s_master_dirty;

// #30: meter-only dirty bits. Separate from s_dirty so a 10 Hz meter
// stream doesn't trigger the slider/colour/label rebuild branch on
// every frame. The meter sweep is a single async fold (apply_meter_pending)
// queued only when there's actual meter work to do.
EXT_RAM_BSS_ATTR static volatile bool s_meter_dirty[APP_CONFIG_MAX_CHANNELS];
static volatile bool s_meter_sweep_queued;

// PRESENT mode = "signal in last 1 s" debounce. Updated by
// apply_meter_pending whenever a meter sample exceeds threshold; the
// dot stays lit for HOLD_MS after, then fades. -40 dB threshold keeps
// noise floor from triggering the indicator. Same logic for the master.
#define SIGNAL_PRESENT_THRESHOLD_DB  -40.0f
#define SIGNAL_PRESENT_HOLD_MS       1000
EXT_RAM_BSS_ATTR static uint32_t s_last_signal_ms[APP_CONFIG_MAX_CHANNELS];
static uint32_t s_master_last_signal_ms;
static volatile bool s_master_meter_dirty;

static void apply_pending(void *unused)
{
    (void)unused;
    // Clear the queued flag BEFORE iterating so any state change that fires
    // during the sweep (and after we've passed its index) re-arms a fresh
    // sweep rather than getting silently swallowed.
    s_sweep_queued = false;
    // Settings overlay + color picker show channel names sourced from
    // app_state. P9: when a scribble-strip rename arrives, the same dirty
    // bit that drives the fader-strip refresh below also re-syncs the open
    // overlays so names update live (no overlay re-open required). Gated
    // by visibility so closed overlays cost nothing.
    bool settings_visible = s_settings_overlay &&
        !lv_obj_has_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
    bool picker_visible   = s_picker_popup &&
        !lv_obj_has_flag(s_picker_popup, LV_OBJ_FLAG_HIDDEN);
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        if (!s_dirty[i]) continue;
        s_dirty[i] = false;
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        if (settings_visible && s_row_name_labels[i]) {
            lv_label_set_text(s_row_name_labels[i], ch.name);
        }
        if (picker_visible && i == s_picker_target_idx) {
            picker_refresh_title();
        }
        // LV_ANIM_OFF: network echoes can arrive every ~10ms during a drag;
        // queueing/cancelling 200ms animations on each one trashes LVGL.
        // Slider position is computed from whichever state field matches
        // the active format (only that one is being kept fresh by the
        // single-format subscription).
        app_level_format_t fmt = app_prefs_get_level_format();
        int v;
        char buf[12];
        if (fmt == APP_LEVEL_FORMAT_DB) {
            // MS reports min ~= -138 dB on the Si Expression 2; display
            // "-inf dB" there since the channel is effectively off. Above
            // the floor we round to the nearest dB -- a half-dB step is
            // finer than the mixer's quantization, no need for decimals
            // on a glance-readout. Infinity glyph (U+221E) requires
            // font_monmix_level set on the label by build_fader.
            float db = ch.level_db;
            v = (int)(app_db_to_position(db) * 100.0f);
            if (db <= APP_DB_MIN) {
                snprintf(buf, sizeof(buf), "-\xe2\x88\x9e dB");
            } else {
                if (db > APP_DB_MAX) db = APP_DB_MAX;
                snprintf(buf, sizeof(buf), "%.0f dB", db);
            }
        } else {
            v = (int)(ch.level * 100.0f);
            snprintf(buf, sizeof(buf), "%d", v);
        }
        lv_slider_set_value(s_widgets[i].slider, v, LV_ANIM_OFF);
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
        // Indicator-widget visibility is owned by apply_meter_pending now
        // (it reads meter_db + last_signal_ms with debounce). Here we only
        // handle bar visibility on mode change so a flip from METER to
        // NONE/PRESENT clears the bar in the same frame.
        app_signal_indicator_t mode = app_prefs_get_signal_indicator();
        if (s_widgets[i].meter_bar) {
            if (mode == APP_SIGNAL_INDICATOR_METER) {
                lv_obj_remove_flag(s_widgets[i].meter_bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_widgets[i].meter_bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    // Master strip — same format-aware redraw shape as the input strips.
    if (s_master_dirty && s_master_widgets.slider) {
        s_master_dirty = false;
        app_channel_t m;
        if (app_state_master_get(&m)) {
            app_level_format_t fmt = app_prefs_get_level_format();
            int v;
            char buf[12];
            if (fmt == APP_LEVEL_FORMAT_DB) {
                float db = m.level_db;
                v = (int)(app_db_to_position(db) * 100.0f);
                if (db <= APP_DB_MIN) {
                    snprintf(buf, sizeof(buf), "-\xe2\x88\x9e dB");
                } else {
                    if (db > APP_DB_MAX) db = APP_DB_MAX;
                    snprintf(buf, sizeof(buf), "%.0f dB", db);
                }
            } else {
                v = (int)(m.level * 100.0f);
                snprintf(buf, sizeof(buf), "%d", v);
            }
            lv_slider_set_value(s_master_widgets.slider, v, LV_ANIM_OFF);
            lv_label_set_text(s_master_widgets.label_val, buf);
            lv_label_set_text(s_master_widgets.label_name, m.name);
            if (s_master_widgets.btn_mute) {
                if (m.mute) lv_obj_add_state   (s_master_widgets.btn_mute, LV_STATE_CHECKED);
                else        lv_obj_remove_state(s_master_widgets.btn_mute, LV_STATE_CHECKED);
            }
            if (s_master_widgets.meter_bar) {
                if (app_prefs_get_signal_indicator() == APP_SIGNAL_INDICATOR_METER) {
                    lv_obj_remove_flag(s_master_widgets.meter_bar, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_master_widgets.meter_bar, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

// Meter color picker shared by tracked channels and master. Stage-monitor
// convention: green below -12 dB, yellow -12 to -3, red above -3.
static uint32_t meter_color_for(float db) {
    if (db > -3.0f)  return 0xE04040;  // red
    if (db > -12.0f) return 0xE0D040;  // yellow
    return 0x40C040;                   // green
}

static void update_signal_dot_visible(lv_obj_t *dot, bool show) {
    if (!dot) return;
    if (show) {
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x40C040), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// #30: meter-only redraw. Walks s_meter_dirty[], computes the bar fill from
// each channel's meter_db, and writes lv_bar_set_value. Decoupled from
// apply_pending so a 10 Hz meter stream doesn't trigger the heavier
// slider/label/colour repaint branch on every frame.
//
// Also tracks "signal present in the last HOLD_MS" by sampling each new
// meter db against SIGNAL_PRESENT_THRESHOLD_DB and stamping
// s_last_signal_ms. PRESENT-mode dot visibility is recomputed every
// sweep based on (now - last_signal_ms) so an idle channel fades out
// even if no fresh broadcast arrives (signal_dot_sweep_cb pumps the
// sweep at 4 Hz to drive the fade).
static void apply_meter_pending(void *unused)
{
    (void) unused;
    s_meter_sweep_queued = false;
    uint32_t now = lv_tick_get();
    app_signal_indicator_t mode = app_prefs_get_signal_indicator();
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        bool dirty = s_meter_dirty[i];
        s_meter_dirty[i] = false;
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        // Stamp last-signal time only on dirty samples (a fresh value
        // arrived); the per-tick sweep below still re-evaluates dot
        // visibility against the stored timestamp so it fades cleanly.
        if (dirty && !ch.mute && ch.meter_db > SIGNAL_PRESENT_THRESHOLD_DB) {
            s_last_signal_ms[i] = now;
        }
        if (dirty && s_widgets[i].meter_bar && mode == APP_SIGNAL_INDICATOR_METER) {
            float db = ch.meter_db;
            // Mute squelches the bar — the audio path is silent, so a
            // non-zero meter reading from a stale broadcast would mislead.
            // Sentinel -200 (no sample yet) and the explicit silence
            // flush from ws_set_meter_enabled both clamp to the floor.
            if (ch.mute) db = METER_DB_FLOOR;
            if (db < METER_DB_FLOOR) db = METER_DB_FLOOR;
            if (db > METER_DB_CEIL)  db = METER_DB_CEIL;
            int fill = (int) (((db - METER_DB_FLOOR) /
                               (METER_DB_CEIL - METER_DB_FLOOR)) * 1000.0f);
            lv_bar_set_value(s_widgets[i].meter_bar, fill, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_widgets[i].meter_bar,
                                      lv_color_hex(meter_color_for(db)),
                                      LV_PART_INDICATOR);
        }
        // PRESENT-mode dot — recompute every sweep so an idle channel
        // fades 1 s after its last above-threshold sample.
        if (mode == APP_SIGNAL_INDICATOR_PRESENT && s_widgets[i].signal_dot) {
            bool show = !ch.mute &&
                        (now - s_last_signal_ms[i]) < SIGNAL_PRESENT_HOLD_MS;
            update_signal_dot_visible(s_widgets[i].signal_dot, show);
        }
    }

    // Master strip parity. Same logic as input strips.
    if (s_master_meter_dirty) {
        s_master_meter_dirty = false;
        app_channel_t m;
        if (app_state_master_get(&m)) {
            if (!m.mute && m.meter_db > SIGNAL_PRESENT_THRESHOLD_DB) {
                s_master_last_signal_ms = now;
            }
            if (s_master_widgets.meter_bar && mode == APP_SIGNAL_INDICATOR_METER) {
                float db = m.meter_db;
                if (m.mute) db = METER_DB_FLOOR;
                if (db < METER_DB_FLOOR) db = METER_DB_FLOOR;
                if (db > METER_DB_CEIL)  db = METER_DB_CEIL;
                int fill = (int) (((db - METER_DB_FLOOR) /
                                   (METER_DB_CEIL - METER_DB_FLOOR)) * 1000.0f);
                lv_bar_set_value(s_master_widgets.meter_bar, fill, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(s_master_widgets.meter_bar,
                                          lv_color_hex(meter_color_for(db)),
                                          LV_PART_INDICATOR);
            }
        }
    }
    if (mode == APP_SIGNAL_INDICATOR_PRESENT && s_master_widgets.signal_dot) {
        app_channel_t m;
        bool show = false;
        if (app_state_master_get(&m)) {
            show = !m.mute &&
                   (now - s_master_last_signal_ms) < SIGNAL_PRESENT_HOLD_MS;
        }
        update_signal_dot_visible(s_master_widgets.signal_dot, show);
    }

    // Belt-and-braces hide of dots when mode isn't PRESENT (apply_pending
    // already handles the bar side on mode change).
    if (mode != APP_SIGNAL_INDICATOR_PRESENT) {
        for (size_t i = 0; i < total; ++i) {
            if (s_widgets[i].signal_dot) {
                lv_obj_add_flag(s_widgets[i].signal_dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_master_widgets.signal_dot) {
            lv_obj_add_flag(s_master_widgets.signal_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void on_meter_change(size_t idx, void *ctx)
{
    (void) ctx;
    if (idx >= APP_CONFIG_MAX_CHANNELS) return;
    s_meter_dirty[idx] = true;
    if (s_meter_sweep_queued) return;

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    if (!s_meter_sweep_queued) {
        s_meter_sweep_queued = true;
        if (lv_async_call(apply_meter_pending, NULL) != LV_RESULT_OK) {
            s_meter_sweep_queued = false;
        }
    }
    lvgl_port_unlock();
}

static void on_master_meter_change(void *ctx)
{
    (void) ctx;
    s_master_meter_dirty = true;
    if (s_meter_sweep_queued) return;
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    if (!s_meter_sweep_queued) {
        s_meter_sweep_queued = true;
        if (lv_async_call(apply_meter_pending, NULL) != LV_RESULT_OK) {
            s_meter_sweep_queued = false;
        }
    }
    lvgl_port_unlock();
}

// Periodic LVGL timer that re-runs the meter sweep at ~4 Hz so PRESENT-
// mode dots fade out after their hold window even when MS sends nothing
// (steady silence -> no broadcasts -> apply_meter_pending wouldn't
// otherwise rerun and the dot would stay lit until the next change).
// Cheap: the sweep is a few-hundred-cycle walk over s_meter_dirty.
static void signal_dot_sweep_cb(lv_timer_t *t)
{
    (void)t;
    app_signal_indicator_t mode = app_prefs_get_signal_indicator();
    if (mode != APP_SIGNAL_INDICATOR_PRESENT) return;
    if (s_meter_sweep_queued) return;
    s_meter_sweep_queued = true;
    if (lv_async_call(apply_meter_pending, NULL) != LV_RESULT_OK) {
        s_meter_sweep_queued = false;
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

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
            // Best effort — the next state change retries.
        }
    }
    lvgl_port_unlock();
}

// Master strip rides apply_pending too — same coalescing, same lock
// discipline. Setting s_master_dirty before queuing means a sweep already
// in flight picks it up via the master branch at the end of apply_pending.
static void on_master_state_change(void *ctx)
{
    (void) ctx;
    s_master_dirty = true;
    if (s_sweep_queued) return;

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    if (!s_sweep_queued) {
        s_sweep_queued = true;
        if (lv_async_call(apply_pending, NULL) != LV_RESULT_OK) {
            s_sweep_queued = false;
        }
    }
    lvgl_port_unlock();
}

// Pref changes (level format, channel colors, signal indicator, theme) affect
// the rendering of every fader, so we mark all channels dirty and queue a
// single sweep — same plumbing as the per-channel state changes. Theme is
// re-applied here too; lv_theme_default_init is idempotent for an unchanged
// theme so we don't need to track the old value.
//
// Theme apply is heavy: lv_theme_default_init walks the widget tree
// reassigning styles, which then cascades into invalidations across every
// child. From the LVGL task that's fine (it's already where the work
// belongs), but from the console REPL or any other non-LVGL task, doing
// it synchronously holds lvgl_port_lock for 100s of ms while CPU-bound
// on the caller's core -- IDLE on that core starves and task_wdt fires
// (caught in soak under console-driven `level-format` toggles). Defer
// via lv_async_call so the work lands on the LVGL task regardless of
// where the pref-change observer fires.
static void apply_theme_async(void *unused)
{
    (void)unused;
    app_display_apply_theme(app_prefs_get_theme());
    if (s_gear_label) {
        bool dark = (app_prefs_get_theme() == APP_THEME_DARK);
        lv_obj_set_style_text_color(s_gear_label,
                                    dark ? lv_color_white() : lv_color_black(), 0);
    }
}

static void on_prefs_change(void *ctx)
{
    (void)ctx;
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) s_dirty[i] = true;
    s_master_dirty = true;

    // Defer theme repaint -- it walks the widget tree assigning styles
    // and cascades into invalidations everywhere, which is heavy enough
    // (>5s on a busy core) to starve IDLE if it runs synchronously on
    // a non-LVGL caller. Queueing on the LVGL task keeps the caller's
    // core free to yield.
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        lv_async_call(apply_theme_async, NULL);
        lvgl_port_unlock();
    }

    // #30: chase the signal indicator pref. Subscribe meter feed only when
    // the user opts in; unsubscribe when they switch back. Idempotent on
    // both sides so a no-op pref change (e.g. brightness) leaves the
    // subscription alone. Called from any task — the WS client's
    // set_meter_enabled lands the message on the WS task so locking isn't
    // our problem here.
    if (s_ms && s_ms->set_meter_enabled) {
        // Both PRESENT (single dot) and METER (full bar) drive their
        // visibility from live meter data now -- PRESENT gates a 1 s
        // debounce off the same meter_db stream that METER renders
        // continuously. Subscribe in either mode; only NONE skips it.
        app_signal_indicator_t mode = app_prefs_get_signal_indicator();
        bool want_meter = (mode == APP_SIGNAL_INDICATOR_METER ||
                           mode == APP_SIGNAL_INDICATOR_PRESENT);
        s_ms->set_meter_enabled(want_meter);
        // Force a meter-bar repaint so a mode flip clears any stale fill
        // left over from a previous meter session. apply_pending owns the
        // visibility flag from this same sweep; pairing the two keeps the
        // bar's first frame in sync with its first visible frame.
        for (size_t i = 0; i < total; ++i) s_meter_dirty[i] = true;
        if (!s_meter_sweep_queued) {
            s_meter_sweep_queued = true;
            if (lv_async_call(apply_meter_pending, NULL) != LV_RESULT_OK) {
                s_meter_sweep_queued = false;
            }
        }
    }

    if (s_sweep_queued) return;

    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
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

// ─────────────────────────────────────────────────────────────────────────
// Drag-to-reorder. Long-press on a strip's name label promotes the strip
// into "reorder mode": outline-highlighted, sliders + mute disabled, tile
// swipe suppressed. Pressing events compare finger X against the centres
// of neighbour boxes ON THE SAME PAGE; crossing a centre swaps both the
// working ids array and the live app_state slot. Release persists +
// rebuilds.
// ─────────────────────────────────────────────────────────────────────────

static void reorder_set_highlight(size_t idx, bool on)
{
    if (idx >= APP_CONFIG_MAX_CHANNELS) return;
    lv_obj_t *box = s_widgets[idx].box;
    if (!box) return;
    if (on) {
        // Bright outline -- reads on both dark and light themes. Width of 4
        // is enough to be unambiguous without crowding the slider.
        lv_obj_set_style_outline_width(box, 4, 0);
        lv_obj_set_style_outline_color(box, lv_color_hex(0xE0C040), 0);
        lv_obj_set_style_outline_pad(box, 2, 0);
        lv_obj_set_style_outline_opa(box, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_outline_width(box, 0, 0);
        lv_obj_set_style_outline_opa(box, LV_OPA_TRANSP, 0);
    }
}

static void on_strip_long_pressed(lv_event_t *e)
{
    if (s_reorder_active) return;
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    size_t total = app_state_count();
    if (idx >= total) return;

    // Snapshot current ids into a working buffer; mutated in-place on each
    // swap. Persisted on release.
    s_reorder_count = total;
    for (size_t i = 0; i < total; ++i) {
        s_reorder_ids[i] = app_state_id_for_idx(i);
    }
    s_reorder_idx    = idx;
    s_reorder_page   = idx / FADERS_PER_PAGE;
    s_reorder_active = true;

    reorder_set_highlight(idx, true);

    // Suppress slider drag and tile swipe so neither competes with the
    // reorder gesture. Restored on release.
    for (size_t i = 0; i < total; ++i) {
        if (s_widgets[i].slider) {
            lv_obj_remove_flag(s_widgets[i].slider, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (s_tileview) {
        lv_obj_remove_flag(s_tileview, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_strip_pressing(lv_event_t *e)
{
    if (!s_reorder_active) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    // The pointer's x is in screen coordinates. The strip boxes are
    // children of tile children of the tileview, so their lv_obj_get_x is
    // tile-relative. Compute screen-x of each candidate neighbour box on
    // the same page and compare.
    size_t cur = s_reorder_idx;
    size_t page_first = s_reorder_page * FADERS_PER_PAGE;
    size_t page_last  = page_first + FADERS_PER_PAGE - 1;
    if (page_last >= s_reorder_count) page_last = s_reorder_count - 1;

    // Try left neighbour: swap if finger crossed its centre going left.
    if (cur > page_first) {
        lv_obj_t *nb = s_widgets[cur - 1].box;
        if (nb) {
            int32_t nb_x = lv_obj_get_x(nb);
            int32_t nb_w = lv_obj_get_width(nb);
            int32_t nb_cx = nb_x + nb_w / 2;
            if (p.x < nb_cx) {
                size_t a = cur, b = cur - 1;
                int tmp = s_reorder_ids[a];
                s_reorder_ids[a] = s_reorder_ids[b];
                s_reorder_ids[b] = tmp;
                app_state_swap_slots(a, b);
                reorder_set_highlight(a, false);
                reorder_set_highlight(b, true);
                s_reorder_idx = b;
                on_state_change(a, NULL);
                on_state_change(b, NULL);
                return;
            }
        }
    }
    // Right neighbour.
    if (cur < page_last) {
        lv_obj_t *nb = s_widgets[cur + 1].box;
        if (nb) {
            int32_t nb_x = lv_obj_get_x(nb);
            int32_t nb_w = lv_obj_get_width(nb);
            int32_t nb_cx = nb_x + nb_w / 2;
            if (p.x > nb_cx) {
                size_t a = cur, b = cur + 1;
                int tmp = s_reorder_ids[a];
                s_reorder_ids[a] = s_reorder_ids[b];
                s_reorder_ids[b] = tmp;
                app_state_swap_slots(a, b);
                reorder_set_highlight(a, false);
                reorder_set_highlight(b, true);
                s_reorder_idx = b;
                on_state_change(a, NULL);
                on_state_change(b, NULL);
                return;
            }
        }
    }
}

// Deferred rebuild: present_channels destroys the very label object we
// were dispatching the event into. Punt it to the next LVGL idle.
static void reorder_persist_and_rebuild(void *unused)
{
    (void) unused;
    app_config_set_channel_ids(s_reorder_ids, s_reorder_count);
    app_ui_present_channels();
}

static void reorder_exit(bool persist_and_rebuild)
{
    if (!s_reorder_active) return;
    s_reorder_active = false;

    // Drop the highlight on whichever slot we ended on.
    reorder_set_highlight(s_reorder_idx, false);

    // Restore tile swipe; slider clickability is restored by the rebuild via
    // apply_controls_enabled, or here directly when we don't rebuild.
    if (s_tileview) {
        lv_obj_add_flag(s_tileview, LV_OBJ_FLAG_SCROLLABLE);
    }

    if (persist_and_rebuild) {
        // Defer destroy-and-rebuild until after this event finishes
        // dispatching -- the name label is the event target.
        lv_async_call(reorder_persist_and_rebuild, NULL);
    } else {
        apply_controls_enabled();
    }
}

static void on_strip_released(lv_event_t *e)
{
    (void) e;
    reorder_exit(true);
}

static void build_fader(lv_obj_t *parent, size_t idx, int slot_x_in_tile)
{
    app_channel_t ch;
    if (!app_state_get(idx, &ch)) return;

    // Center the box within its slot. slot_w = TILEVIEW_W / FADERS_PER_PAGE
    // — tileview shrinks for the master strip on the right so use the
    // tileview width here, not the full screen width.
    const int slot_w = TILEVIEW_W / FADERS_PER_PAGE;
    const int box_x  = slot_x_in_tile + (slot_w - FADER_BOX_W) / 2;
    const int box_y  = (TILEVIEW_H - FADER_BOX_H) / 2;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, FADER_BOX_W, FADER_BOX_H);
    lv_obj_set_pos(box, box_x, box_y);
    lv_obj_set_style_pad_all(box, FADER_BOX_PAD, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_widgets[idx].box = box;

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, ch.name);
    // WRAP allows up to 3 lines for long scribble names. Fixed height of 60
    // caps the visible area so a 4+ line name clips into ellipsis-by-truncation
    // territory rather than overflowing into the signal-dot row below. With
    // default Montserrat 14, a line is ~16-18 px so 60 px = ~3 lines.
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(name, FADER_BOX_W - 2 * FADER_BOX_PAD, 60);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    s_widgets[idx].label_name = name;

    // Drag-to-reorder is bound to the name label only -- the slider has its
    // own drag (level), the mute button its own click. PRESS_LOCK keeps the
    // press on this label even when the finger slides off, so PRESSING/
    // RELEASED keep firing through the swap.
    lv_obj_add_flag(name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(name, on_strip_long_pressed, LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t) idx);
    lv_obj_add_event_cb(name, on_strip_pressing, LV_EVENT_PRESSING,
                        (void *)(uintptr_t) idx);
    lv_obj_add_event_cb(name, on_strip_released, LV_EVENT_RELEASED,
                        (void *)(uintptr_t) idx);
    lv_obj_add_event_cb(name, on_strip_released, LV_EVENT_PRESS_LOST,
                        (void *)(uintptr_t) idx);

    // Signal-present indicator — small round dot sitting just above the
    // slider top so multi-line scribble names (up to 3 lines) don't
    // overlap it. y=60 puts the dot at content_y 60..70, slider top at
    // 72 (CENTER align of SLIDER_H=340 in 484 inner). Visible only when
    // mode != none AND the channel is actively passing audio (!mute &&
    // level > 0). Without live meter data from MS this is a "configured
    // to pass audio" indicator, not strict audio presence — useful for
    // the "do I have a cable problem?" question.
    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_set_size(dot, SIGNAL_DOT_SIZE, SIGNAL_DOT_SIZE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    s_widgets[idx].signal_dot = dot;

    // #30: real meter bar. Vertical lv_bar to the immediate left of the
    // slider track. Hidden unless signal_indicator pref is METER and we
    // have a live meter sample. Built here (always) so apply_pending can
    // toggle its visibility cheaply on indicator pref change rather than
    // tearing down/rebuilding the strip.
    lv_obj_t *meter = lv_bar_create(box);
    lv_obj_set_size(meter, METER_BAR_W, METER_BAR_H);
    lv_bar_set_range(meter, 0, 1000);  // 1000 steps over 60 dB; fine enough
    lv_bar_set_value(meter, 0, LV_ANIM_OFF);
    // Pin to the left of the slider track. y offset = SLIDER_TOP_Y minus
    // where a CENTER-aligned bar of equal height would land, so the bar's
    // top tracks the slider's top regardless of any future SLIDER_TOP_Y
    // tweak.
    lv_obj_align(meter, LV_ALIGN_CENTER, -SLIDER_W / 2 - METER_BAR_W - 2,
                 SLIDER_TOP_Y - ((FADER_BOX_H - 2 * FADER_BOX_PAD - METER_BAR_H) / 2));
    lv_obj_set_style_pad_all(meter, 0, 0);
    lv_obj_set_style_radius(meter, 1, 0);
    lv_obj_set_style_radius(meter, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(meter, 0, 0);
    lv_obj_set_style_bg_color(meter, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(meter, lv_color_hex(0x40C040), LV_PART_INDICATOR);
    lv_obj_clear_flag(meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(meter, LV_OBJ_FLAG_HIDDEN);
    s_widgets[idx].meter_bar = meter;

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, 0, 100);
    {
        // Initial value tracks whichever state field the active format
        // keeps fresh; apply_pending will correct on the first broadcast.
        int init_v;
        if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
            init_v = (int)(app_db_to_position(ch.level_db) * 100.0f);
        } else {
            init_v = (int)(ch.level * 100.0f);
        }
        lv_slider_set_value(slider, init_v, LV_ANIM_OFF);
    }
    lv_obj_set_size(slider, SLIDER_W, SLIDER_H);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, SLIDER_TOP_Y);
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
    // Tick is anchored to the slider (not the box center) so it keeps
    // tracking the unity-gain position regardless of slider alignment.
    int tick_y_off = (int)((0.5f - NORM_AT_0DB) * (float) SLIDER_H);
    lv_obj_align_to(tick, slider, LV_ALIGN_RIGHT_MID, 10, tick_y_off);

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
    // Only the level label needs the extended Montserrat (infinity glyph for
    // the floor case). Other labels stay on LV_FONT_DEFAULT.
    lv_obj_set_style_text_font(val, &font_monmix_level, 0);
    s_widgets[idx].label_val = val;
}

// Master strip — same widget pattern as build_fader minus the per-channel
// quirks (no color tag, no reorder gesture, no signal dot — the master is
// visually distinct already). Bound to the singleton master state, retargeted
// to the active mix bus's channel id by the WS subscribe path.
static void on_master_slider_changed(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int   v        = lv_slider_get_value(slider);
    float position = (float) v / 100.0f;

    app_level_format_t fmt = app_prefs_get_level_format();
    if (fmt == APP_LEVEL_FORMAT_DB) {
        float db = app_position_to_db(position);
        app_state_master_set_level_db(db, false);
    } else {
        app_state_master_set_level(position, false);
    }

    // Same 20 Hz rate-limit shape as the per-channel sliders. Master gets
    // its own send timestamp so a fast input drag doesn't gate master
    // updates.
    static uint32_t s_master_last_send_ms;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_master_last_send_ms >= SET_MIN_INTERVAL_MS) {
        if (s_ms && s_ms->set_master_level) s_ms->set_master_level(position);
        s_master_last_send_ms = now;
    }

    char buf[12];
    if (fmt == APP_LEVEL_FORMAT_DB) {
        if (v <= 0) {
            snprintf(buf, sizeof(buf), "-\xe2\x88\x9e dB");
        } else {
            snprintf(buf, sizeof(buf), "%.0f dB", app_position_to_db(position));
        }
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
    }
    lv_label_set_text(s_master_widgets.label_val, buf);
}

static void on_master_slider_released(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    if (s_ms && s_ms->set_master_level) {
        s_ms->set_master_level((float) v / 100.0f);
    }
}

static void on_master_mute_clicked(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);

    bool ms_ok = (s_ms && s_ms->get_state &&
                  s_ms->get_state() == APP_MS_STATE_CONNECTED);
    if (!ms_ok) return;
    if (!s_mute_enabled) {
        toast_show("Mute disabled - tap MUTE EN to enable");
        return;
    }

    app_channel_t m;
    if (!app_state_master_get(&m)) return;
    bool new_mute = !m.mute;

    if (s_ms && s_ms->set_master_mute) s_ms->set_master_mute(new_mute);
    app_state_master_set_mute(new_mute, false);

    if (new_mute) lv_obj_add_state   (btn, LV_STATE_CHECKED);
    else          lv_obj_remove_state(btn, LV_STATE_CHECKED);
}

static void build_master_strip(lv_obj_t *parent)
{
    app_channel_t m;
    app_state_master_get(&m);

    // Anchored to the right of the screen, same vertical band as the
    // tileview. Width matches MASTER_STRIP_W; box centered within that
    // slot using the same FADER_BOX_W as the input strips.
    const int box_x = SCREEN_W - MASTER_STRIP_W + (MASTER_STRIP_W - FADER_BOX_W) / 2;
    const int box_y = TILEVIEW_Y + (TILEVIEW_H - FADER_BOX_H) / 2;

    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, FADER_BOX_W, FADER_BOX_H);
    lv_obj_set_pos(box, box_x, box_y);
    lv_obj_set_style_pad_all(box, FADER_BOX_PAD, 0);
    // Subtle outline so the master reads as a separate group from the
    // input strips. Same accent the reorder mode uses but at lower opacity
    // so it's a visual hint, not a "this is selected" cue.
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xE0C040), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    // Hidden by default — ms_apply_async / present_channels reveals it on
    // the next CONNECTED transition. Mirrors s_tileview's behavior so the
    // master doesn't show stale values during boot / outage.
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    s_master_widgets.box = box;

    lv_obj_t *name = lv_label_create(box);
    lv_label_set_text(name, m.name[0] ? m.name : "Master");
    // Match the input-strip name geometry: 3-line wrap with 60 px height
    // cap. Master mix-bus names ("STAGE LEFT MONITOR MIX") often need it.
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(name, FADER_BOX_W - 2 * FADER_BOX_PAD, 60);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 0);
    s_master_widgets.label_name = name;

    // Master parity with input strips: signal_dot just above slider top,
    // meter_bar pinned to slider's left edge. Same y-positions as
    // build_fader so the master visually reads as the same widget shape.
    lv_obj_t *master_dot = lv_obj_create(box);
    lv_obj_set_size(master_dot, SIGNAL_DOT_SIZE, SIGNAL_DOT_SIZE);
    lv_obj_align(master_dot, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_radius(master_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(master_dot, 0, 0);
    lv_obj_set_style_pad_all(master_dot, 0, 0);
    lv_obj_clear_flag(master_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(master_dot, LV_OBJ_FLAG_HIDDEN);
    s_master_widgets.signal_dot = master_dot;

    lv_obj_t *master_meter = lv_bar_create(box);
    lv_obj_set_size(master_meter, METER_BAR_W, METER_BAR_H);
    lv_bar_set_range(master_meter, 0, 1000);
    lv_bar_set_value(master_meter, 0, LV_ANIM_OFF);
    lv_obj_align(master_meter, LV_ALIGN_CENTER, -SLIDER_W / 2 - METER_BAR_W - 2,
                 SLIDER_TOP_Y - ((FADER_BOX_H - 2 * FADER_BOX_PAD - METER_BAR_H) / 2));
    lv_obj_set_style_pad_all(master_meter, 0, 0);
    lv_obj_set_style_radius(master_meter, 1, 0);
    lv_obj_set_style_radius(master_meter, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(master_meter, 0, 0);
    lv_obj_set_style_bg_color(master_meter, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(master_meter, lv_color_hex(0x40C040), LV_PART_INDICATOR);
    lv_obj_clear_flag(master_meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(master_meter, LV_OBJ_FLAG_HIDDEN);
    s_master_widgets.meter_bar = master_meter;

    lv_obj_t *slider = lv_slider_create(box);
    lv_slider_set_range(slider, 0, 100);
    {
        int init_v;
        if (app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB) {
            init_v = (int)(app_db_to_position(m.level_db) * 100.0f);
        } else {
            init_v = (int)(m.level * 100.0f);
        }
        lv_slider_set_value(slider, init_v, LV_ANIM_OFF);
    }
    lv_obj_set_size(slider, SLIDER_W, SLIDER_H);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, SLIDER_TOP_Y);
    // Master always reads as the unity-gain accent color — no per-channel
    // color tag for the master since there's only one of it visible at a
    // time. Yellow matches the box outline so the master strip reads as a
    // single visual group.
    lv_color_t bar  = lv_color_hex(0xE0C040);
    lv_color_t knob = lv_color_darken(bar, 60);
    lv_obj_set_style_bg_color(slider, bar,  LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, knob, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_master_slider_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, on_master_slider_released,
                        LV_EVENT_RELEASED, NULL);
    s_master_widgets.slider = slider;

    // 0 dB unity tick to mirror the input strips.
    lv_obj_t *tick = lv_obj_create(box);
    lv_obj_set_size(tick, 12, 2);
    lv_obj_set_style_bg_color(tick, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_pad_all(tick, 0, 0);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
    // Tick is anchored to the slider (not the box center) so it keeps
    // tracking the unity-gain position regardless of slider alignment.
    int tick_y_off = (int)((0.5f - NORM_AT_0DB) * (float) SLIDER_H);
    lv_obj_align_to(tick, slider, LV_ALIGN_RIGHT_MID, 10, tick_y_off);

    lv_obj_t *btn_mute = lv_button_create(box);
    lv_obj_set_size(btn_mute, MUTE_BTN_W, MUTE_BTN_H);
    lv_obj_align(btn_mute, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0xC00000), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0x303030), LV_STATE_DEFAULT);
    lv_obj_t *btn_label = lv_label_create(btn_mute);
    lv_label_set_text(btn_label, "MUTE");
    lv_obj_center(btn_label);
    lv_obj_add_event_cb(btn_mute, on_master_mute_clicked, LV_EVENT_CLICKED, NULL);
    s_master_widgets.btn_mute = btn_mute;
    if (m.mute) lv_obj_add_state(btn_mute, LV_STATE_CHECKED);

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "0");
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(val, &font_monmix_level, 0);
    s_master_widgets.label_val = val;
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

    // Splash: full-screen near-black overlay with the logo centered. Built
    // first so all subsequent widgets land underneath; we explicitly bring
    // it back to the foreground in case any later object claims top z-order.
    // Hidden in app_ui_present_channels once the real fader UI is mounted.
    s_splash_screen = lv_obj_create(scr);
    lv_obj_set_size(s_splash_screen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_splash_screen, 0, 0);
    lv_obj_set_style_bg_color(s_splash_screen, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_splash_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash_screen, 0, 0);
    lv_obj_set_style_radius(s_splash_screen, 0, 0);
    lv_obj_set_style_pad_all(s_splash_screen, 0, 0);
    lv_obj_clear_flag(s_splash_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_splash_logo_img = lv_image_create(s_splash_screen);
    lv_image_set_src(s_splash_logo_img, &splash_logo);
    lv_obj_center(s_splash_logo_img);

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
    s_gear_label = lv_label_create(gear);
    lv_label_set_text(s_gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(s_gear_label);
    {
        bool dark = (app_prefs_get_theme() == APP_THEME_DARK);
        lv_obj_set_style_text_color(s_gear_label,
                                    dark ? lv_color_white() : lv_color_black(), 0);
    }
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
    // us how many mixes the connected console exposes. Width is content-
    // sized with a 90 px floor so long names ("STAGE LEFT MONITOR MIX")
    // don't overflow; right edge is anchored, growth pushes left.
    s_mix_indicator = lv_button_create(scr);
    lv_obj_set_size(s_mix_indicator, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_min_width(s_mix_indicator, 90, 0);
    lv_obj_align(s_mix_indicator, LV_ALIGN_TOP_RIGHT, -118, 2);
    s_mix_indicator_label = lv_label_create(s_mix_indicator);
    lv_label_set_text(s_mix_indicator_label, "Mix 1");
    lv_obj_center(s_mix_indicator_label);
    lv_obj_add_event_cb(s_mix_indicator, on_mix_indicator_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_mix_indicator, LV_OBJ_FLAG_HIDDEN);

    // Settings-icon hit pads -- transparent rectangles behind each
    // settings icon (MS / WiFi / gear only; mute-en and mix indicator
    // keep their own click areas). Each pad's x range goes from the
    // midpoint to its left neighbour to the midpoint to its right
    // neighbour (screen edge for gear); y fills 0..STATUS_H so tall
    // taps still register. Positions use the design-time icon coords.
    // For the leftmost settings icon (MS), the mix indicator is the
    // left adjacent button -- its midpoint bounds MS even though the
    // mix indicator itself doesn't get an extended pad.
    {
        struct { int x; int w; lv_event_cb_t cb; } pads[] = {
            { 911, 37, on_ms_clicked   },  // MS   (916..944; midpts 911 and 948)
            { 948, 36, on_wifi_clicked },  // WiFi (952..980; midpts 948 and 984)
            { 984, 40, on_gear_clicked },  // gear (988..1016; midpt 984 to screen edge)
        };
        for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); ++i) {
            lv_obj_t *pad = lv_obj_create(scr);
            lv_obj_set_size(pad, pads[i].w, STATUS_H);
            lv_obj_set_pos(pad, pads[i].x, 0);
            lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(pad, 0, 0);
            lv_obj_set_style_pad_all(pad, 0, 0);
            lv_obj_set_style_radius(pad, 0, 0);
            lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pad, pads[i].cb, LV_EVENT_CLICKED, NULL);
            // Push to back so the visible icon button stays in front and
            // gets touches in its own region (the pad only catches taps
            // OUTSIDE the icon, in the extended hit zone).
            lv_obj_move_to_index(pad, 0);
        }
    }

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
    app_state_register_on_meter(on_meter_change, NULL);
    app_state_master_register_on_change(on_master_state_change, NULL);
    app_state_master_register_on_meter(on_master_meter_change, NULL);
    // Periodic dot-fade sweep -- see signal_dot_sweep_cb. 250 ms / 4 Hz
    // is fine-grained enough for a 1 s hold to look smooth and well below
    // any LVGL timer overhead floor.
    lv_timer_create(signal_dot_sweep_cb, 250, NULL);
    app_prefs_register_on_change(on_prefs_change, NULL);
    app_wifi_register_on_change(on_wifi_state_change, NULL);
    if (s_ms && s_ms->register_on_change) {
        s_ms->register_on_change(on_ms_state_change, NULL);
    }

    wifi_icon_refresh();
    ms_icon_refresh();
    apply_controls_enabled();

    // Keep the splash on top of every shell widget built above. Faders are
    // mounted later in app_ui_present_channels which dismisses the splash
    // explicitly.
    if (s_splash_screen) lv_obj_move_foreground(s_splash_screen);

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
        lv_obj_set_size(s_tileview, TILEVIEW_W, TILEVIEW_H);
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

        const int slot_w = TILEVIEW_W / FADERS_PER_PAGE;
        for (size_t slot = 0; slot < FADERS_PER_PAGE; ++slot) {
            size_t idx = p * FADERS_PER_PAGE + slot;
            if (idx >= total) break;
            build_fader(tile, idx, (int)slot * slot_w);
        }
    }

    if (s_page_count > 1) {
        s_page_indicator = create_page_indicator(lv_screen_active(), s_page_count);
    }

    // Master strip lives outside the tileview (on the screen directly) so
    // it survives lv_obj_clean(s_tileview) and doesn't need to rebuild on
    // channel-list changes. Build once on first present; later calls are
    // no-ops.
    if (!s_master_widgets.box) {
        build_master_strip(lv_screen_active());
        // Initial paint follows the same dirty-flag path as live updates.
        s_master_dirty = true;
        if (lv_async_call(apply_pending, NULL) == LV_RESULT_OK) {
            s_sweep_queued = true;
        }
    }

    apply_controls_enabled();

    // Sync the spinner ↔ tileview visibility with the current MS state.
    // Without this, the freshly-built tileview would stay hidden until the
    // next state-change event, leaving the spinner overlaid on the strips.
    bool ms_connected = (s_ms && s_ms->get_state &&
                         s_ms->get_state() == APP_MS_STATE_CONNECTED);
    if (ms_connected) {
        if (s_tileview)            lv_obj_remove_flag(s_tileview,      LV_OBJ_FLAG_HIDDEN);
        if (s_master_widgets.box)  lv_obj_remove_flag(s_master_widgets.box, LV_OBJ_FLAG_HIDDEN);
        if (s_spinner)             lv_obj_add_flag   (s_spinner,       LV_OBJ_FLAG_HIDDEN);
        if (s_spinner_label)       lv_obj_add_flag   (s_spinner_label, LV_OBJ_FLAG_HIDDEN);
    }

    // The fader UI is now mounted; dismiss the boot splash. We leave the
    // object alive so the LVGL theme doesn't have a half-decoded pointer in
    // its parent list -- just hide it.
    if (s_splash_screen) lv_obj_add_flag(s_splash_screen, LV_OBJ_FLAG_HIDDEN);

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
    if (s_ms && s_ms->set_level_format) s_ms->set_level_format(APP_LEVEL_FORMAT_NORM);
    lv_obj_t *btns[2] = { s_lvl_norm_btn, s_lvl_db_btn };
    update_radio_visuals(btns, 2, 0);
}

static void on_lvl_db_clicked(lv_event_t *e)
{
    (void) e;
    app_prefs_set_level_format(APP_LEVEL_FORMAT_DB);
    if (s_ms && s_ms->set_level_format) s_ms->set_level_format(APP_LEVEL_FORMAT_DB);
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

// TZ dropdown -- pick from the IANA name list curated in app_time.c. Stored
// pref is the IANA name; app_time_apply_tz translates to POSIX at apply time.
static void on_tz_dropdown_changed(lv_event_t *e)
{
    (void) e;
    if (!s_tz_dropdown) return;
    char buf[APP_PREFS_STR_MAX];
    lv_dropdown_get_selected_str(s_tz_dropdown, buf, sizeof(buf));
    app_prefs_set_display_tz(buf);
    app_time_apply_tz();
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

    // Section: Time Zone -- IANA name dropdown. List comes from app_time.c's
    // curated table; selection persists as the IANA name and translates to
    // POSIX at apply time. Logs format from monotonic uptime so they stay
    // TZ-independent regardless of this setting.
    lv_obj_t *tz_label = lv_label_create(ov);
    lv_label_set_text(tz_label, "Time Zone");
    lv_obj_align(tz_label, LV_ALIGN_TOP_LEFT, 0, 224);
    s_tz_dropdown = lv_dropdown_create(ov);
    lv_obj_set_size(s_tz_dropdown, 360, 36);
    lv_obj_align(s_tz_dropdown, LV_ALIGN_TOP_LEFT, 180, 220);
    {
        // Build the option string -- LVGL takes a single \n-separated buffer.
        // Sized for ~30 zones * ~28 chars = ~900 bytes. Stack-local; LVGL
        // copies internally.
        char opts[1024];
        size_t off = 0;
        for (size_t i = 0; i < app_time_zone_count(); ++i) {
            const char *name = app_time_zone_iana(i);
            int n = snprintf(opts + off, sizeof(opts) - off,
                             "%s%s", i == 0 ? "" : "\n", name ? name : "");
            if (n < 0 || (size_t) n >= sizeof(opts) - off) break;
            off += (size_t) n;
        }
        lv_dropdown_set_options(s_tz_dropdown, opts);
    }
    lv_obj_add_event_cb(s_tz_dropdown, on_tz_dropdown_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    // Section: Channels — 3 columns × N rows in a scrollable container so
    // the same layout works at 12 channels (default) and at the Si Expression
    // 2's full 60-channel count. Row is just [name] + [swatch] for now;
    // selection checkbox will be added with the channel-selection feature.
    lv_obj_t *col_label = lv_label_create(ov);
    lv_label_set_text(col_label, "Channels");
    lv_obj_align(col_label, LV_ALIGN_TOP_LEFT, 0, 272);

    // Edit-channels entry — opens the picker overlay (#33). Button sits to
    // the right of the section label so it doesn't push the existing list
    // layout. The picker is the runtime way to change which channels are
    // tracked; was previously only doable via channels-reset + reflash.
    // Hidden if we don't know the connected console's channel count yet
    // (info fetch hasn't completed or failed) -- the picker would be
    // empty and tapping it would be confusing.
    s_edit_channels_btn = lv_button_create(ov);
    lv_obj_set_size(s_edit_channels_btn, 180, 36);
    lv_obj_align(s_edit_channels_btn, LV_ALIGN_TOP_RIGHT, 0, 264);
    lv_obj_t *edit_lbl = lv_label_create(s_edit_channels_btn);
    lv_label_set_text(edit_lbl, LV_SYMBOL_LIST " Edit Channels...");
    lv_obj_center(edit_lbl);
    lv_obj_add_event_cb(s_edit_channels_btn, on_edit_channels_clicked,
                        LV_EVENT_CLICKED, NULL);
    if (s_total_channels <= 0) {
        lv_obj_add_flag(s_edit_channels_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *list = lv_obj_create(ov);
    lv_obj_set_size(list, SCREEN_W - 32, SCREEN_H - 316);
    lv_obj_set_pos(list, 0, 296);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 4, 0);

    const int cols      = 3;
    const int row_h     = 32;
    const int row_gap   = 4;
    const int col_gap   = 8;
    const int row_w     = (SCREEN_W - 32 - 2 * 6 - (cols - 1) * col_gap) / cols;
    const int swatch_sz = 22;
    size_t total = app_state_count();
    // Column-major iteration: fill column 0 top-to-bottom, then column 1,
    // then column 2. Reads more naturally for a list of channel slots.
    int rows_per_col = (int)((total + cols - 1) / cols);
    if (rows_per_col < 1) rows_per_col = 1;
    for (size_t i = 0; i < total; ++i) {
        app_channel_t ch;
        if (!app_state_get(i, &ch)) continue;
        int col = (int)(i / rows_per_col);
        int row = (int)(i % rows_per_col);
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

// Pull the name labels back from app_state. Cheaper than the full
// settings_refresh_state — the dirty-sweep calls this on every channel
// notify so we don't want to thrash the radios/swatches too.
static void settings_refresh_names(void)
{
    size_t total = app_state_count();
    for (size_t i = 0; i < total; ++i) {
        app_channel_t ch;
        if (s_row_name_labels[i] && app_state_get(i, &ch)) {
            lv_label_set_text(s_row_name_labels[i], ch.name);
        }
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

    if (s_tz_dropdown) {
        char tz[APP_PREFS_STR_MAX];
        app_prefs_get_display_tz(tz, sizeof(tz));
        // Map the saved IANA name to a dropdown index. If the saved value
        // isn't in the list (legacy POSIX string from a prior version, or an
        // entry that's been removed), default to America/Los_Angeles.
        uint16_t found = 0;
        bool matched = false;
        for (size_t i = 0; i < app_time_zone_count(); ++i) {
            const char *iana = app_time_zone_iana(i);
            if (iana && strcmp(iana, tz) == 0) {
                found = (uint16_t) i;
                matched = true;
                break;
            }
        }
        if (!matched) {
            for (size_t i = 0; i < app_time_zone_count(); ++i) {
                const char *iana = app_time_zone_iana(i);
                if (iana && strcmp(iana, "America/Los_Angeles") == 0) {
                    found = (uint16_t) i;
                    break;
                }
            }
        }
        lv_dropdown_set_selected(s_tz_dropdown, found);
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
    show_rebooting_overlay();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Build (lazy) and present a full-screen "Rebooting..." overlay so the
// user gets unmistakable feedback before the chip resets. lv_refr_now is
// called inline so the new pixels actually hit the panel before the
// caller delays + restarts.
static void show_rebooting_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!s_rebooting_overlay) {
        lv_obj_t *ov = lv_obj_create(scr);
        lv_obj_set_size(ov, lv_obj_get_width(scr), lv_obj_get_height(scr));
        lv_obj_align(ov, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(ov, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ov, 0, 0);
        lv_obj_set_style_radius(ov, 0, 0);
        lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *spinner = lv_spinner_create(ov);
        lv_obj_set_size(spinner, 80, 80);
        lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t *lbl = lv_label_create(ov);
        lv_label_set_text(lbl, "Rebooting...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align_to(lbl, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

        s_rebooting_overlay = ov;
    } else {
        lv_obj_remove_flag(s_rebooting_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_rebooting_overlay);
    lv_refr_now(NULL);
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

// Title shows the channel name so a glance confirms the right strip
// is being recolored. Pulled out so apply_pending can re-run it when a
// scribble-strip rename lands while the picker is open.
static void picker_refresh_title(void)
{
    if (!s_picker_title) return;
    app_channel_t ch;
    if (app_state_get(s_picker_target_idx, &ch)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Color: %s", ch.name);
        lv_label_set_text(s_picker_title, buf);
    }
}

static void picker_open(size_t channel_idx)
{
    if (!s_picker_popup) build_picker_popup();
    s_picker_target_idx = channel_idx;
    picker_refresh_title();

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

    // P11: count routed mixes only; un-routed ones get skipped entirely
    // so the user can't pick a mix that MS won't honor. The underlying
    // mix index (i) is still passed to on_mix_choice -- the user sees
    // "Mix 5" not "Mix 3" when 1/2 are unrouted, the picker just hides
    // those rows.
    bool has_routed_query = (s_ms && s_ms->is_mix_routed);
    int routed_n = 0;
    for (int i = 0; i < s_mix_count; ++i) {
        if (!has_routed_query || s_ms->is_mix_routed(i)) routed_n++;
    }

    // 4-column grid; cols × rows sized to fit the routed mix count.
    const int btn_w  = 110;
    const int btn_h  = 50;
    const int gap    = 10;
    const int pad    = 20;
    const int header = 40;
    const int cols   = 4;
    int rows         = (routed_n + cols - 1) / cols;
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

    // Snapshot the routed mask the grid was built against so ms_apply_async
    // can detect a change and force a rebuild on next open.
    uint32_t mask = 0;
    for (int i = 0; i < s_mix_count && i < 32; ++i) {
        if (!has_routed_query || s_ms->is_mix_routed(i)) mask |= (1u << i);
    }
    s_picker_routed_mask = mask;

    int slot = 0;  // grid position; advances only for routed mixes
    for (int i = 0; i < s_mix_count; ++i) {
        if (has_routed_query && !s_ms->is_mix_routed(i)) continue;
        int row = slot / cols;
        int col = slot % cols;
        int x   = col * (btn_w + gap);
        int y   = header + row * (btn_h + gap);
        slot++;

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
        // s_mix_picker_btn_labels is indexed by the underlying mix index
        // so mix_picker_refresh_labels can match name broadcasts back to
        // the right button without tracking a parallel slot table.
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
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    // P5: visibility is now driven by mix_indicator_apply_visibility from
    // the ms_apply_async sweep, not from this entry point. The boot path
    // calls this once with the (potentially zero) count from
    // /console/information; the sweep AND-gates that against
    // (ms_connected && mix_list_ready) and shows the button only when
    // both sides are true. A first-boot with MS unreachable now recovers
    // automatically on first WS connect via the post-connect refetch.
    if (s_mix_count > 0) mix_indicator_refresh();
    mix_indicator_apply_visibility();
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

// P5: single source of truth for the mix-indicator's HIDDEN flag. Reads
// (ms_connected && mix_list_ready) plus the s_mix_force_show override.
// Called from the ms_apply_async sweep so visibility tracks both the WS
// state and the mix-list-received bit; never called directly from a WS
// callback (which would race with the LVGL task on the timer list).
static void mix_indicator_apply_visibility(void)
{
    if (!s_mix_indicator) return;
    bool ms_connected = (s_ms && s_ms->get_state &&
                         s_ms->get_state() == APP_MS_STATE_CONNECTED);
    bool list_ready   = (s_ms && s_ms->is_mix_list_ready &&
                         s_ms->is_mix_list_ready());
    bool show = s_mix_force_show ||
                (ms_connected && list_ready && s_mix_count > 0);
    if (show) lv_obj_remove_flag(s_mix_indicator, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag   (s_mix_indicator, LV_OBJ_FLAG_HIDDEN);
}

// P5: forced-reveal toggle for the `mix-show` console command. Called from
// the REPL task — needs lvgl_port_lock since it touches the visibility
// flag directly (the REPL is not the LVGL task).
void app_ui_force_mix_show(bool force)
{
    s_mix_force_show = force;
    if (!s_mix_indicator) return;
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
    mix_indicator_apply_visibility();
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
    // Restore the overlay's full-screen height so the form is fully visible
    // when the keyboard is dismissed. The focus handler shrinks it to
    // SCREEN_H/2 to enable scroll-into-view for fields below the keyboard.
    if (s_wcfg_overlay) {
        lv_obj_set_height(s_wcfg_overlay, SCREEN_H);
        lv_obj_scroll_to_y(s_wcfg_overlay, 0, LV_ANIM_ON);
    }
}
static void mcfg_hide_keyboard(void)
{
    if (s_mcfg_keyboard) lv_obj_add_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (s_mcfg_overlay) {
        lv_obj_set_height(s_mcfg_overlay, SCREEN_H);
        lv_obj_scroll_to_y(s_mcfg_overlay, 0, LV_ANIM_ON);
    }
}

// --- WiFi panel ----------------------------------------------------------

static bool wcfg_has_unsaved_changes(void)
{
    if (strcmp(lv_textarea_get_text(s_wcfg_ssid_ta), s_wcfg_orig_ssid) != 0) return true;
    if (strcmp(lv_textarea_get_text(s_wcfg_pass_ta), s_wcfg_orig_pass) != 0) return true;
    if (s_wcfg_use_static != s_wcfg_orig_use_static) return true;
    if (s_wcfg_ip_ta && strcmp(lv_textarea_get_text(s_wcfg_ip_ta),  s_wcfg_orig_ip)  != 0) return true;
    if (s_wcfg_nm_ta && strcmp(lv_textarea_get_text(s_wcfg_nm_ta),  s_wcfg_orig_nm)  != 0) return true;
    if (s_wcfg_gw_ta && strcmp(lv_textarea_get_text(s_wcfg_gw_ta),  s_wcfg_orig_gw)  != 0) return true;
    if (s_wcfg_dns_ta && strcmp(lv_textarea_get_text(s_wcfg_dns_ta), s_wcfg_orig_dns) != 0) return true;
    if (s_wcfg_ntp_ta && strcmp(lv_textarea_get_text(s_wcfg_ntp_ta), s_wcfg_orig_ntp) != 0) return true;
    if (s_wcfg_ntp_dhcp_cb) {
        bool checked = lv_obj_has_state(s_wcfg_ntp_dhcp_cb, LV_STATE_CHECKED);
        if (checked != s_wcfg_orig_ntp_use_dhcp) return true;
    }
    return false;
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
        // IP / netmask / gateway / DNS fields take numeric input only --
        // dotted IPv4 has digits + dot, but the LVGL number keyboard
        // includes "." so it covers the case.
        bool numeric = (ta == s_wcfg_ip_ta || ta == s_wcfg_nm_ta ||
                        ta == s_wcfg_gw_ta || ta == s_wcfg_dns_ta);
        lv_keyboard_set_mode(s_wcfg_keyboard,
                             numeric ? LV_KEYBOARD_MODE_NUMBER
                                     : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(s_wcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
        // Keyboard is a sibling of the overlay (under the screen) so the
        // overlay can scroll without dragging the keyboard around. But that
        // means the overlay was last in z-order and hides the keyboard --
        // promote it to the front whenever it becomes visible.
        lv_obj_move_foreground(s_wcfg_keyboard);
        // Shrink the overlay to the visible-above-keyboard height so its
        // children that extended into the bottom-half become scrollable.
        // Without this, scroll_to_view sees the whole-screen overlay as
        // "visible" and refuses to scroll.
        lv_obj_set_height(s_wcfg_overlay, SCREEN_H / 2);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void on_wcfg_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        wcfg_hide_keyboard();
    }
}

// Cheap dotted-IPv4 sanity check. We don't need full RFC validation -- the
// platform's ipaddr_addr() rejects garbage at apply time. But "empty when
// static is on" is a UI bug we should catch before the save.
static bool wcfg_ipv4_looks_ok(const char *s)
{
    if (!s || !*s) return false;
    int dots = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '.')      dots++;
        else if (*p < '0' || *p > '9') return false;
    }
    return dots == 3;
}

static void on_wcfg_save_yes(lv_event_t *e);
static void on_wcfg_save_no(lv_event_t *e);
static void build_wcfg_save_confirm(void);

static void on_wcfg_save(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    const char *ssid = lv_textarea_get_text(s_wcfg_ssid_ta);

    if (strlen(ssid) == 0) {
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 SSID cannot be empty.#");
        return;
    }
    if (s_wcfg_use_static) {
        const char *ip = lv_textarea_get_text(s_wcfg_ip_ta);
        const char *nm = lv_textarea_get_text(s_wcfg_nm_ta);
        const char *gw = lv_textarea_get_text(s_wcfg_gw_ta);
        if (!wcfg_ipv4_looks_ok(ip) || !wcfg_ipv4_looks_ok(nm) ||
            !wcfg_ipv4_looks_ok(gw)) {
            lv_label_set_text(s_wcfg_status_label,
                              "#FF6060 IP / Netmask / Gateway must be dotted IPv4.#");
            return;
        }
    }

    if (!s_wcfg_save_confirm) build_wcfg_save_confirm();
    lv_obj_remove_flag(s_wcfg_save_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wcfg_save_confirm);
}

static void on_wcfg_save_no(lv_event_t *e)
{
    (void)e;
    if (s_wcfg_save_confirm) lv_obj_add_flag(s_wcfg_save_confirm, LV_OBJ_FLAG_HIDDEN);
}

static void on_wcfg_save_yes(lv_event_t *e)
{
    (void)e;
    if (s_wcfg_save_confirm) lv_obj_add_flag(s_wcfg_save_confirm, LV_OBJ_FLAG_HIDDEN);

    const char *ssid = lv_textarea_get_text(s_wcfg_ssid_ta);
    const char *pass = lv_textarea_get_text(s_wcfg_pass_ta);
    bool ok = app_config_set_wifi_ssid(ssid) && app_config_set_wifi_pass(pass);
    if (!ok) {
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 Save failed (NVS error).#");
        return;
    }
    // Park the SSID/password in the saved-networks ring so the next venue
    // change can pick this network back without retyping. Promotes if
    // already present; evicts the oldest if the ring is full.
    app_config_wifi_saved_add(ssid, pass);

    // Persist DHCP/static + the four IP fields. app_prefs setters bail if
    // the new value matches the live one, so re-saving an unchanged config
    // is cheap.
    app_prefs_set_wifi_use_static(s_wcfg_use_static);
    if (s_wcfg_use_static) {
        app_prefs_set_wifi_static_ip     (lv_textarea_get_text(s_wcfg_ip_ta));
        app_prefs_set_wifi_static_netmask(lv_textarea_get_text(s_wcfg_nm_ta));
        app_prefs_set_wifi_static_gateway(lv_textarea_get_text(s_wcfg_gw_ta));
        app_prefs_set_wifi_static_dns    (lv_textarea_get_text(s_wcfg_dns_ta));
    }
    if (s_wcfg_ntp_ta) {
        app_prefs_set_ntp_server(lv_textarea_get_text(s_wcfg_ntp_ta));
    }
    if (s_wcfg_ntp_dhcp_cb) {
        app_prefs_set_ntp_use_dhcp(lv_obj_has_state(s_wcfg_ntp_dhcp_cb, LV_STATE_CHECKED));
    }

    // P7b: try the live reconfigure first (no reboot). app_wifi_reconfigure
    // does esp_wifi_disconnect -> esp_wifi_set_config -> esp_wifi_connect
    // (the known-working sequence per reference_c6_wedge_workaround), and
    // app_wifi_apply_ip_config pushes the DHCP/static choice to the netif.
    // If the live dispatch fails outright, fall back to esp_restart -- the
    // user already accepted "may reboot" by tapping the confirm dialog.
    lv_label_set_text(s_wcfg_status_label, "#40C060 Saved. Reconfiguring WiFi...#");
    lv_refr_now(NULL);
    app_wifi_apply_ip_config();
    if (!app_wifi_reconfigure()) {
        show_rebooting_overlay();
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
        return;   // unreached
    }
    // NTP server / DHCP-NTP changes apply live too -- apply_sntp_config
    // does an esp_netif_sntp_deinit + reinit with the new server, no
    // reboot needed. SNTP picks the new server up on the next poll cycle
    // (within ~5 s on a healthy network).
    bool ntp_changed = strcmp(s_wcfg_orig_ntp,
                              lv_textarea_get_text(s_wcfg_ntp_ta)) != 0;
    if (s_wcfg_ntp_dhcp_cb) {
        bool now_use_dhcp = lv_obj_has_state(s_wcfg_ntp_dhcp_cb, LV_STATE_CHECKED);
        if (now_use_dhcp != s_wcfg_orig_ntp_use_dhcp) ntp_changed = true;
    }
    if (ntp_changed) apply_sntp_config();

    vTaskDelay(pdMS_TO_TICKS(600));
    if (s_wcfg_overlay) lv_obj_add_flag(s_wcfg_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void build_wcfg_save_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 480, 220);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_wcfg_save_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg,
                      "Save WiFi settings?\n"
                      "SSID, password, IP and NTP changes apply live --\n"
                      "the fader UI briefly reconnects, no reboot.");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel, on_wcfg_save_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 160, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x40C060), 0);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, LV_SYMBOL_OK " Save");
    lv_obj_center(yl);
    lv_obj_add_event_cb(yes, on_wcfg_save_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

// --- SSID scan list (used only by the WiFi panel) ------------------------

static void ssid_list_populate_async(void *arg);
static void on_ssid_row_clicked(lv_event_t *e);
static void on_wifi_scan_done(void *ctx);

static void on_ssid_list_close(lv_event_t *e)
{
    (void)e;
    if (s_ssid_list_popup) lv_obj_add_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
}

// Toggle just the inline "Scanning..." indicator: list itself is always
// visible so saved networks can be picked without waiting for scan_done.
// (Earlier rev hid the list and showed a centered spinner overlay -- that
// flow couldn't surface saved entries.)
static void ssid_list_set_scanning(bool scanning)
{
    s_ssid_list_scanning = scanning;
    if (!s_ssid_list_popup) return;
    if (s_ssid_list_spinner) lv_obj_add_flag(s_ssid_list_spinner, LV_OBJ_FLAG_HIDDEN);
    if (s_ssid_list_scanning_label) lv_obj_add_flag(s_ssid_list_scanning_label, LV_OBJ_FLAG_HIDDEN);
    if (s_ssid_list) lv_obj_remove_flag(s_ssid_list, LV_OBJ_FLAG_HIDDEN);
}

// Render the picker list: saved networks first (LV_SYMBOL_OK -- "we have the
// password, tap to fill both"), then scanned APs that aren't already saved
// (LV_SYMBOL_WIFI -- "tap to fill SSID, password still required"), then an
// optional "Scanning..." status row when a fresh scan is in flight. The
// list is rebuilt each call -- cheap at ~24 entries cap.
static void ssid_list_render(const char (*scanned)[33], size_t scanned_n,
                             bool scanning)
{
    if (!s_ssid_list) return;
    lv_obj_clean(s_ssid_list);

    char ssid[APP_CONFIG_SSID_MAX];
    char pass[APP_CONFIG_PASS_MAX];

    size_t saved_n = app_config_wifi_saved_count();
    for (size_t i = 0; i < saved_n; ++i) {
        if (!app_config_wifi_saved_get(i, ssid, sizeof(ssid), pass, sizeof(pass))) continue;
        lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_OK, ssid);
        lv_obj_add_event_cb(btn, on_ssid_row_clicked, LV_EVENT_CLICKED, NULL);
    }

    size_t shown_scanned = 0;
    for (size_t i = 0; i < scanned_n; ++i) {
        if (app_config_wifi_saved_lookup(scanned[i], pass, sizeof(pass))) continue;
        lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_WIFI, scanned[i]);
        lv_obj_add_event_cb(btn, on_ssid_row_clicked, LV_EVENT_CLICKED, NULL);
        shown_scanned++;
    }

    if (scanning) {
        lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_REFRESH, "Scanning...");
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else if (saved_n == 0 && shown_scanned == 0) {
        lv_obj_t *btn = lv_list_add_button(s_ssid_list, LV_SYMBOL_WARNING,
                                           "No networks found");
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void on_wifi_scan_done(void *ctx)
{
    (void)ctx;
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
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

    // Scanning-state widgets occupy the same area; toggled by
    // ssid_list_set_scanning. The spinner is parented to the popup so it
    // stays in the foreground stacking when the list is hidden.
    s_ssid_list_spinner = lv_spinner_create(p);
    lv_obj_set_size(s_ssid_list_spinner, 60, 60);
    lv_obj_align(s_ssid_list_spinner, LV_ALIGN_CENTER, 0, -10);
    s_ssid_list_scanning_label = lv_label_create(p);
    lv_label_set_text(s_ssid_list_scanning_label, "Scanning...");
    lv_obj_align_to(s_ssid_list_scanning_label, s_ssid_list_spinner,
                    LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void ssid_list_populate_async(void *arg)
{
    (void)arg;
    if (!s_ssid_list_popup) build_ssid_list_popup();

    char results[APP_WIFI_SCAN_MAX_RESULTS][33];
    size_t n = app_wifi_scan_results(results, APP_WIFI_SCAN_MAX_RESULTS);

    // Empty result on first try -> retry once. The slave can return 0
    // entries while the C6's scan cache is still warming up; a second
    // pass usually populates it. Saved entries already on screen continue
    // to be selectable while we re-scan.
    if (n == 0 && !s_ssid_list_retried_empty) {
        s_ssid_list_retried_empty = true;
        ssid_list_render(NULL, 0, true);
        ssid_list_set_scanning(true);
        lv_obj_remove_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ssid_list_popup);
        (void) app_wifi_scan_start(on_wifi_scan_done, NULL);
        return;
    }

    ssid_list_render(results, n, false);

    if (s_wcfg_scan_btn) {
        lv_obj_remove_state(s_wcfg_scan_btn, LV_STATE_DISABLED);
        lv_label_set_text(s_wcfg_scan_btn_label, "Scan");
    }

    ssid_list_set_scanning(false);
    lv_obj_remove_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ssid_list_popup);
}

static void on_ssid_row_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    const char *txt = lv_list_get_button_text(s_ssid_list, btn);
    if (txt && s_wcfg_ssid_ta) {
        lv_textarea_set_text(s_wcfg_ssid_ta, txt);
        // Saved-network shortcut: if the picked SSID has a stored password,
        // fill the password field too so the user can hit Save without
        // retyping. Tapping a scan-only entry leaves the password alone.
        char pass[APP_CONFIG_PASS_MAX];
        if (s_wcfg_pass_ta &&
            app_config_wifi_saved_lookup(txt, pass, sizeof(pass))) {
            lv_textarea_set_text(s_wcfg_pass_ta, pass);
        }
    }
    if (s_ssid_list_popup) {
        lv_obj_add_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_wcfg_scan_clicked(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    if (!s_ssid_list_popup) build_ssid_list_popup();
    s_ssid_list_retried_empty = false;
    app_wifi_scan_result_t r = app_wifi_scan_start(on_wifi_scan_done, NULL);
    if (r == APP_WIFI_SCAN_FAILED) {
        // Scan dispatch failed (e.g. WiFi never came up). Saved networks
        // remain pickable so a re-association attempt is still one tap away.
        ssid_list_render(NULL, 0, false);
        ssid_list_set_scanning(false);
        lv_obj_remove_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ssid_list_popup);
        lv_label_set_text(s_wcfg_status_label,
                          "#FF6060 Scan failed (WiFi unavailable).#");
        return;
    }
    // Both STARTED and ALREADY_RUNNING land here -- show saved entries
    // immediately so the user can pick one without waiting for SCAN_DONE.
    // Scanned entries get merged in when the scan completes.
    lv_obj_add_state(s_wcfg_scan_btn, LV_STATE_DISABLED);
    lv_label_set_text(s_wcfg_scan_btn_label, "Scanning...");
    lv_label_set_text(s_wcfg_status_label, "");
    ssid_list_render(NULL, 0, true);
    ssid_list_set_scanning(true);
    lv_obj_remove_flag(s_ssid_list_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ssid_list_popup);
}

// Show / hide the static-IP field group based on the radio's working state.
// Called from the radio click handlers and from wcfg_open() so the initial
// visibility matches the persisted pref.
static void wcfg_refresh_current_ip(void)
{
    if (!s_wcfg_current_ip_value) return;
    char ip[16];
    app_wifi_format_ip(ip, sizeof(ip));
    char buf[32];
    snprintf(buf, sizeof(buf), "Current: %s", ip);
    lv_label_set_text(s_wcfg_current_ip_value, buf);
}

static void wcfg_apply_static_visibility(void)
{
    if (!s_wcfg_static_group) return;
    if (s_wcfg_use_static) lv_obj_remove_flag(s_wcfg_static_group, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag   (s_wcfg_static_group, LV_OBJ_FLAG_HIDDEN);
    if (s_wcfg_dhcp_btn && s_wcfg_static_btn) {
        if (s_wcfg_use_static) {
            lv_obj_remove_state(s_wcfg_dhcp_btn,   LV_STATE_CHECKED);
            lv_obj_add_state   (s_wcfg_static_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state   (s_wcfg_dhcp_btn,   LV_STATE_CHECKED);
            lv_obj_remove_state(s_wcfg_static_btn, LV_STATE_CHECKED);
        }
    }
}

static void on_wcfg_dhcp_clicked(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    s_wcfg_use_static = false;
    wcfg_apply_static_visibility();
}

static void on_wcfg_static_clicked(lv_event_t *e)
{
    (void)e;
    wcfg_hide_keyboard();
    s_wcfg_use_static = true;
    wcfg_apply_static_visibility();
}

static void build_wcfg_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    // Form scrolls vertically so the keyboard pop-up doesn't permanently
    // hide bottom-row fields (NTP textarea + DHCP-NTP checkbox). The
    // textarea-focus handler calls lv_obj_scroll_to_view to bring the focused
    // field above the keyboard. Keyboard is parented to the screen (not the
    // overlay) so scrolling the form leaves the keyboard pinned at the
    // bottom of the screen.
    lv_obj_set_scroll_dir(ov, LV_DIR_VER);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
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
    const int row_dy     = 48;
    const int form_w     = SCREEN_W - 32;
    const int scan_btn_w = 110;

    int y = 50;

    // Row 1: SSID label / textarea / Scan button.
    lv_obj_t *ssid_lbl = lv_label_create(ov);
    lv_label_set_text(ssid_lbl, "SSID");
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_LEFT, 0, y + 4);

    s_wcfg_ssid_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_wcfg_ssid_ta, form_w - 80 - scan_btn_w - 12, field_h);
    lv_obj_align(s_wcfg_ssid_ta, LV_ALIGN_TOP_LEFT, 80, y);
    lv_textarea_set_one_line(s_wcfg_ssid_ta, true);
    lv_textarea_set_max_length(s_wcfg_ssid_ta, APP_CONFIG_SSID_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_ssid_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_wcfg_scan_btn = lv_button_create(ov);
    lv_obj_set_size(s_wcfg_scan_btn, scan_btn_w, field_h);
    lv_obj_align(s_wcfg_scan_btn, LV_ALIGN_TOP_LEFT, form_w - scan_btn_w, y);
    s_wcfg_scan_btn_label = lv_label_create(s_wcfg_scan_btn);
    lv_label_set_text(s_wcfg_scan_btn_label, "Scan");
    lv_obj_center(s_wcfg_scan_btn_label);
    lv_obj_add_event_cb(s_wcfg_scan_btn, on_wcfg_scan_clicked,
                        LV_EVENT_CLICKED, NULL);
    y += row_dy;

    // Row 2: Pass + show-password checkbox inline on the right.
    lv_obj_t *pass_lbl = lv_label_create(ov);
    lv_label_set_text(pass_lbl, "Pass");
    lv_obj_align(pass_lbl, LV_ALIGN_TOP_LEFT, 0, y + 4);

    s_wcfg_pass_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_wcfg_pass_ta, form_w - 80 - 200, field_h);
    lv_obj_align(s_wcfg_pass_ta, LV_ALIGN_TOP_LEFT, 80, y);
    lv_textarea_set_one_line(s_wcfg_pass_ta, true);
    lv_textarea_set_max_length(s_wcfg_pass_ta, APP_CONFIG_PASS_MAX - 1);
    lv_textarea_set_password_mode(s_wcfg_pass_ta, true);
    lv_obj_add_event_cb(s_wcfg_pass_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_wcfg_show_pass_cb = lv_checkbox_create(ov);
    lv_checkbox_set_text(s_wcfg_show_pass_cb, "Show password");
    lv_obj_align(s_wcfg_show_pass_cb, LV_ALIGN_TOP_LEFT, form_w - 180, y + 6);
    lv_obj_add_event_cb(s_wcfg_show_pass_cb, on_wcfg_show_pass_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    y += row_dy;

    // Row 3: IP mode radio (DHCP / Static).
    lv_obj_t *ip_lbl = lv_label_create(ov);
    lv_label_set_text(ip_lbl, "IP");
    lv_obj_align(ip_lbl, LV_ALIGN_TOP_LEFT, 0, y + 8);

    s_wcfg_dhcp_btn = make_radio_button(ov, "DHCP");
    lv_obj_set_size(s_wcfg_dhcp_btn, 110, 36);
    lv_obj_align(s_wcfg_dhcp_btn, LV_ALIGN_TOP_LEFT, 80, y);
    lv_obj_add_event_cb(s_wcfg_dhcp_btn, on_wcfg_dhcp_clicked,
                        LV_EVENT_CLICKED, NULL);

    s_wcfg_static_btn = make_radio_button(ov, "Static");
    lv_obj_set_size(s_wcfg_static_btn, 110, 36);
    lv_obj_align(s_wcfg_static_btn, LV_ALIGN_TOP_LEFT, 200, y);
    lv_obj_add_event_cb(s_wcfg_static_btn, on_wcfg_static_clicked,
                        LV_EVENT_CLICKED, NULL);

    // Read-only "Current: <ip>" -- shows whatever the netif actually has,
    // i.e. the DHCP-assigned address when DHCP is on, or the configured
    // static IP otherwise. Updates on every wifi-state change while the
    // overlay is open. Right-aligned to keep the form column tidy.
    s_wcfg_current_ip_value = lv_label_create(ov);
    lv_label_set_text(s_wcfg_current_ip_value, "Current: 0.0.0.0");
    lv_obj_align(s_wcfg_current_ip_value, LV_ALIGN_TOP_LEFT, 320, y + 8);
    y += row_dy;

    // Static IP group -- one container so we can hide/show as a unit. Two
    // rows of two textareas each: IP / Netmask, then Gateway / DNS.
    s_wcfg_static_group = lv_obj_create(ov);
    lv_obj_set_size(s_wcfg_static_group, form_w, row_dy * 2 + 8);
    lv_obj_set_pos(s_wcfg_static_group, 0, y);
    lv_obj_set_style_pad_all(s_wcfg_static_group, 0, 0);
    lv_obj_set_style_border_width(s_wcfg_static_group, 0, 0);
    lv_obj_set_style_bg_opa(s_wcfg_static_group, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_wcfg_static_group, LV_OBJ_FLAG_SCROLLABLE);

    const int ip_field_w = (form_w - 80 - 80 - 24) / 2;  // two side-by-side

    lv_obj_t *ip_field_lbl = lv_label_create(s_wcfg_static_group);
    lv_label_set_text(ip_field_lbl, "Addr");
    lv_obj_align(ip_field_lbl, LV_ALIGN_TOP_LEFT, 0, 4);
    s_wcfg_ip_ta = lv_textarea_create(s_wcfg_static_group);
    lv_obj_set_size(s_wcfg_ip_ta, ip_field_w, field_h);
    lv_obj_align(s_wcfg_ip_ta, LV_ALIGN_TOP_LEFT, 80, 0);
    lv_textarea_set_one_line(s_wcfg_ip_ta, true);
    lv_textarea_set_max_length(s_wcfg_ip_ta, APP_PREFS_IP_STR_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_ip_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    lv_obj_t *nm_lbl = lv_label_create(s_wcfg_static_group);
    lv_label_set_text(nm_lbl, "Mask");
    lv_obj_align(nm_lbl, LV_ALIGN_TOP_LEFT, 80 + ip_field_w + 12, 4);
    s_wcfg_nm_ta = lv_textarea_create(s_wcfg_static_group);
    lv_obj_set_size(s_wcfg_nm_ta, ip_field_w, field_h);
    lv_obj_align(s_wcfg_nm_ta, LV_ALIGN_TOP_LEFT, 80 + ip_field_w + 12 + 64, 0);
    lv_textarea_set_one_line(s_wcfg_nm_ta, true);
    lv_textarea_set_max_length(s_wcfg_nm_ta, APP_PREFS_IP_STR_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_nm_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    lv_obj_t *gw_lbl = lv_label_create(s_wcfg_static_group);
    lv_label_set_text(gw_lbl, "GW");
    lv_obj_align(gw_lbl, LV_ALIGN_TOP_LEFT, 0, row_dy + 4);
    s_wcfg_gw_ta = lv_textarea_create(s_wcfg_static_group);
    lv_obj_set_size(s_wcfg_gw_ta, ip_field_w, field_h);
    lv_obj_align(s_wcfg_gw_ta, LV_ALIGN_TOP_LEFT, 80, row_dy);
    lv_textarea_set_one_line(s_wcfg_gw_ta, true);
    lv_textarea_set_max_length(s_wcfg_gw_ta, APP_PREFS_IP_STR_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_gw_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    lv_obj_t *dns_lbl = lv_label_create(s_wcfg_static_group);
    lv_label_set_text(dns_lbl, "DNS");
    lv_obj_align(dns_lbl, LV_ALIGN_TOP_LEFT, 80 + ip_field_w + 12, row_dy + 4);
    s_wcfg_dns_ta = lv_textarea_create(s_wcfg_static_group);
    lv_obj_set_size(s_wcfg_dns_ta, ip_field_w, field_h);
    lv_obj_align(s_wcfg_dns_ta, LV_ALIGN_TOP_LEFT, 80 + ip_field_w + 12 + 64, row_dy);
    lv_textarea_set_one_line(s_wcfg_dns_ta, true);
    lv_textarea_set_max_length(s_wcfg_dns_ta, APP_PREFS_IP_STR_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_dns_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    y += row_dy * 2 + 8;

    // NTP server row -- hostname or dotted IPv4. Lives outside the static
    // group so it's visible regardless of DHCP/Static. Keyboard defaults to
    // TEXT_LOWER (the focus handler skips the numeric flip when it isn't an
    // IP-form field), which is right for hostnames; a numeric IP server
    // string is also typeable that way.
    lv_obj_t *ntp_lbl = lv_label_create(ov);
    lv_label_set_text(ntp_lbl, "NTP");
    lv_obj_align(ntp_lbl, LV_ALIGN_TOP_LEFT, 0, y + 4);
    s_wcfg_ntp_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_wcfg_ntp_ta, form_w - 80, field_h);
    lv_obj_align(s_wcfg_ntp_ta, LV_ALIGN_TOP_LEFT, 80, y);
    lv_textarea_set_one_line(s_wcfg_ntp_ta, true);
    lv_textarea_set_max_length(s_wcfg_ntp_ta, APP_PREFS_STR_MAX - 1);
    lv_obj_add_event_cb(s_wcfg_ntp_ta, on_wcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);
    y += row_dy;

    // "Honor DHCP NTP" checkbox -- when checked (default), a DHCP-supplied
    // NTP server (option 42) takes priority and the manual server above is
    // the fallback. Unchecked = use only the manual server.
    s_wcfg_ntp_dhcp_cb = lv_checkbox_create(ov);
    lv_checkbox_set_text(s_wcfg_ntp_dhcp_cb, "Use DHCP-provided NTP server when available");
    lv_obj_align(s_wcfg_ntp_dhcp_cb, LV_ALIGN_TOP_LEFT, 80, y);
    y += row_dy;

    s_wcfg_status_label = lv_label_create(ov);
    lv_label_set_text(s_wcfg_status_label, "");
    lv_label_set_recolor(s_wcfg_status_label, true);
    lv_obj_align(s_wcfg_status_label, LV_ALIGN_TOP_LEFT, 0, y);

    // Keyboard is parented to the screen, NOT the overlay, so scrolling the
    // overlay leaves the keyboard pinned at the bottom of the screen. Without
    // this the keyboard would scroll with the form and stop covering the
    // wrong fields.
    s_wcfg_keyboard = lv_keyboard_create(scr);
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

    // Static-IP prefs into the four textareas + sync the radio.
    s_wcfg_use_static = app_prefs_get_wifi_use_static();
    s_wcfg_orig_use_static = s_wcfg_use_static;
    app_prefs_get_wifi_static_ip     (s_wcfg_orig_ip,  sizeof(s_wcfg_orig_ip));
    app_prefs_get_wifi_static_netmask(s_wcfg_orig_nm,  sizeof(s_wcfg_orig_nm));
    app_prefs_get_wifi_static_gateway(s_wcfg_orig_gw,  sizeof(s_wcfg_orig_gw));
    app_prefs_get_wifi_static_dns    (s_wcfg_orig_dns, sizeof(s_wcfg_orig_dns));
    lv_textarea_set_text(s_wcfg_ip_ta,  s_wcfg_orig_ip);
    lv_textarea_set_text(s_wcfg_nm_ta,  s_wcfg_orig_nm);
    lv_textarea_set_text(s_wcfg_gw_ta,  s_wcfg_orig_gw);
    lv_textarea_set_text(s_wcfg_dns_ta, s_wcfg_orig_dns);
    app_prefs_get_ntp_server(s_wcfg_orig_ntp, sizeof(s_wcfg_orig_ntp));
    if (s_wcfg_ntp_ta) lv_textarea_set_text(s_wcfg_ntp_ta, s_wcfg_orig_ntp);
    s_wcfg_orig_ntp_use_dhcp = app_prefs_get_ntp_use_dhcp();
    if (s_wcfg_ntp_dhcp_cb) {
        if (s_wcfg_orig_ntp_use_dhcp) lv_obj_add_state   (s_wcfg_ntp_dhcp_cb, LV_STATE_CHECKED);
        else                          lv_obj_remove_state(s_wcfg_ntp_dhcp_cb, LV_STATE_CHECKED);
    }
    wcfg_refresh_current_ip();
    wcfg_apply_static_visibility();

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

// Custom TEXT_LOWER keymap for the MS-config keyboard. Default LVGL
// alpha keymap requires the user to flip to a sub-screen ("1#") to
// reach digits, and that sub-screen has no '.', so typing an IPv4 like
// "192.168.1.1" needs four mode flips. This single-screen layout puts
// digits, all 26 lowercase letters, '.' and '-' in front of the user
// at once so neither hostnames nor dotted addresses need a flip.
static const char *const mcfg_alphanum_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "z", "x", "c", "v", "b", "n", "m", "-", ".", "\n",
    LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, ""
};

// LV_KB_BTN(w) expands to LV_BUTTONMATRIX_CTRL_POPOVER | w but the macro
// is file-static inside lv_keyboard.c. Redefine locally so the keymap
// sources cleanly.
#define MCFG_KB_BTN(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | (w)))

// Width units per cell: digits 1, backspace 2, space 8, close/ok 2 with
// the keyboard's CTRL_BUTTON_FLAGS so the default event handler treats
// them as cancel/submit.
static const lv_buttonmatrix_ctrl_t mcfg_alphanum_ctrl[] = {
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(2),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1), MCFG_KB_BTN(1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, MCFG_KB_BTN(8), LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};


static bool mcfg_has_unsaved_changes(void)
{
    if (strcmp(lv_textarea_get_text(s_mcfg_host_ta), s_mcfg_orig_host) != 0) return true;
    if (strcmp(lv_textarea_get_text(s_mcfg_port_ta), s_mcfg_orig_port) != 0) return true;
    if (s_mcfg_osc_port_ta &&
        strcmp(lv_textarea_get_text(s_mcfg_osc_port_ta), s_mcfg_orig_osc_port) != 0) return true;
    if (s_mcfg_proto_staged != s_mcfg_orig_proto) return true;
    return false;
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
        // Host accepts a hostname OR a dotted IP, so it needs both alpha
        // and digits. TEXT_LOWER on this keyboard is mapped to a custom
        // single-screen layout (digits row + alpha rows + .) so the user
        // never has to flip subscreens to type "192.168.1.1".
        // Port fields stay on NUMBER mode (LVGL's default 4x4 keypad,
        // which already includes '.').
        bool port_only = (ta == s_mcfg_port_ta || ta == s_mcfg_osc_port_ta);
        lv_keyboard_set_textarea(s_mcfg_keyboard, ta);
        lv_keyboard_set_mode(s_mcfg_keyboard,
                             port_only ? LV_KEYBOARD_MODE_NUMBER
                                       : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
        // Keyboard is a sibling of the overlay; promote to foreground so it
        // isn't drawn behind the overlay.
        lv_obj_move_foreground(s_mcfg_keyboard);
        // Shrink the overlay so children below the visible-above-keyboard
        // edge become scrollable; scroll_to_view treats the full-screen
        // overlay as "all visible" otherwise.
        lv_obj_set_height(s_mcfg_overlay, SCREEN_H / 2);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void on_mcfg_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        mcfg_hide_keyboard();
    }
}

static void build_mcfg_reboot_confirm(void);

static void on_mcfg_save(lv_event_t *e)
{
    (void)e;
    mcfg_hide_keyboard();
    const char *host       = lv_textarea_get_text(s_mcfg_host_ta);
    const char *port_s     = lv_textarea_get_text(s_mcfg_port_ta);
    const char *osc_port_s = s_mcfg_osc_port_ta
                                 ? lv_textarea_get_text(s_mcfg_osc_port_ta) : "";
    long port     = strtol(port_s, NULL, 10);
    long osc_port = osc_port_s[0] ? strtol(osc_port_s, NULL, 10) : 0;
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
    if (osc_port_s[0] && (osc_port <= 0 || osc_port > 65535)) {
        lv_label_set_text(s_mcfg_status_label,
                          "#FF6060 Invalid OSC port (1-65535)#");
        return;
    }
    bool ok = app_config_set_ms_host(host) &&
              app_config_set_ms_port((uint16_t) port);
    if (ok && osc_port > 0) ok = app_config_set_ms_osc_port((uint16_t) osc_port);
    if (!ok) {
        lv_label_set_text(s_mcfg_status_label,
                          "#FF6060 Save failed (NVS error).#");
        return;
    }

    bool proto_changed = (s_mcfg_proto_staged != s_mcfg_orig_proto);
    if (proto_changed) {
        // Persist the new protocol but don't apply it live -- the active
        // backend is chosen at startup. Show a reboot confirm; cancel
        // reverts the persisted value so a "save then back out" leaves
        // the device in its prior state.
        if (!app_config_set_ms_protocol((app_ms_protocol_t) s_mcfg_proto_staged)) {
            lv_label_set_text(s_mcfg_status_label,
                              "#FF6060 Save failed (NVS error).#");
            return;
        }
    }

    // Re-snapshot originals so the X close path doesn't think we still
    // have unsaved changes from the just-committed edit.
    strncpy(s_mcfg_orig_host, host, sizeof(s_mcfg_orig_host) - 1);
    s_mcfg_orig_host[sizeof(s_mcfg_orig_host) - 1] = '\0';
    strncpy(s_mcfg_orig_port, port_s, sizeof(s_mcfg_orig_port) - 1);
    s_mcfg_orig_port[sizeof(s_mcfg_orig_port) - 1] = '\0';
    if (s_mcfg_osc_port_ta) {
        strncpy(s_mcfg_orig_osc_port, osc_port_s, sizeof(s_mcfg_orig_osc_port) - 1);
        s_mcfg_orig_osc_port[sizeof(s_mcfg_orig_osc_port) - 1] = '\0';
    }

    if (proto_changed) {
        lv_label_set_text(s_mcfg_status_label,
                          "#40C060 Saved. Reboot to switch protocol.#");
        if (!s_mcfg_reboot_confirm) build_mcfg_reboot_confirm();
        lv_obj_remove_flag(s_mcfg_reboot_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mcfg_reboot_confirm);
        return;
    }

    s_mcfg_orig_proto = s_mcfg_proto_staged;
    lv_label_set_text(s_mcfg_status_label,
                      "#40C060 Saved. Reconnecting to MS...#");

    // Live-apply for host/port/osc-port within the same protocol. The
    // active client recomposes URLs from app_config_* on its next
    // reconnect cycle -- the iface's reconnect() closes the current
    // connection and the worker re-opens with whatever NVS now returns.
    if (s_ms && s_ms->reconnect) s_ms->reconnect();
}

static void on_mcfg_reboot_yes(lv_event_t *e)
{
    (void)e;
    if (s_mcfg_reboot_confirm) lv_obj_add_flag(s_mcfg_reboot_confirm, LV_OBJ_FLAG_HIDDEN);
    esp_restart();
}

static void on_mcfg_reboot_no(lv_event_t *e)
{
    (void)e;
    if (s_mcfg_reboot_confirm) lv_obj_add_flag(s_mcfg_reboot_confirm, LV_OBJ_FLAG_HIDDEN);
    // Roll back the persisted protocol so dismissing the dialog is
    // truly idempotent -- otherwise the user would unknowingly switch
    // on the next reboot.
    app_config_set_ms_protocol((app_ms_protocol_t) s_mcfg_orig_proto);
    s_mcfg_proto_staged = s_mcfg_orig_proto;
    if (s_mcfg_proto_ws_btn && s_mcfg_proto_osc_btn) {
        lv_obj_t *btns[2] = { s_mcfg_proto_ws_btn, s_mcfg_proto_osc_btn };
        update_radio_visuals(btns, 2, (size_t) s_mcfg_orig_proto);
    }
    lv_label_set_text(s_mcfg_status_label,
                      "#FFB040 Reboot cancelled — protocol unchanged.#");
}

static void build_mcfg_reboot_confirm(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_set_size(p, 460, 200);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    s_mcfg_reboot_confirm = p;

    lv_obj_t *msg = lv_label_create(p);
    lv_label_set_text(msg, "Switching MS protocol requires a reboot.\nReboot now?");
    lv_obj_align(msg, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *cancel = lv_button_create(p);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Not now");
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel, on_mcfg_reboot_no, LV_EVENT_CLICKED, NULL);

    lv_obj_t *yes = lv_button_create(p);
    lv_obj_set_size(yes, 160, 50);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, lv_color_hex(0x40C060), 0);
    lv_obj_t *yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "Reboot");
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes, on_mcfg_reboot_yes, LV_EVENT_CLICKED, NULL);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
}

static void on_mcfg_proto_clicked(lv_event_t *e)
{
    int which = (int)(uintptr_t) lv_event_get_user_data(e);
    s_mcfg_proto_staged = which;
    lv_obj_t *btns[2] = { s_mcfg_proto_ws_btn, s_mcfg_proto_osc_btn };
    update_radio_visuals(btns, 2, (size_t) which);
}

static void build_mcfg_overlay(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 16, 0);
    // Same scroll pattern as build_wcfg_overlay: the keyboard pop-up halves
    // the visible area; making the overlay vertically scrollable + having
    // the focus handler call lv_obj_scroll_to_view brings any covered field
    // above the keyboard. Keyboard parent is the screen so it stays pinned.
    lv_obj_set_scroll_dir(ov, LV_DIR_VER);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
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

    // Protocol toggle. Two-button radio mirrors the level-format pattern
    // elsewhere in the panel; staged in s_mcfg_proto_staged and only
    // persisted on Save.
    lv_obj_t *proto_lbl = lv_label_create(ov);
    lv_label_set_text(proto_lbl, "Protocol");
    lv_obj_align(proto_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy * 2 + 4);

    s_mcfg_proto_ws_btn = make_radio_button(ov, "WS");
    lv_obj_set_size(s_mcfg_proto_ws_btn, 80, field_h);
    lv_obj_align(s_mcfg_proto_ws_btn, LV_ALIGN_TOP_LEFT, 80, 56 + row_dy * 2);
    lv_obj_add_event_cb(s_mcfg_proto_ws_btn, on_mcfg_proto_clicked,
                        LV_EVENT_CLICKED, (void *)(uintptr_t) APP_MS_PROTOCOL_WS);

    s_mcfg_proto_osc_btn = make_radio_button(ov, "OSC");
    lv_obj_set_size(s_mcfg_proto_osc_btn, 80, field_h);
    lv_obj_align(s_mcfg_proto_osc_btn, LV_ALIGN_TOP_LEFT, 168, 56 + row_dy * 2);
    lv_obj_add_event_cb(s_mcfg_proto_osc_btn, on_mcfg_proto_clicked,
                        LV_EVENT_CLICKED, (void *)(uintptr_t) APP_MS_PROTOCOL_OSC);

    lv_obj_t *osc_port_lbl = lv_label_create(ov);
    lv_label_set_text(osc_port_lbl, "OSC Port");
    lv_obj_align(osc_port_lbl, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy * 3 + 4);
    s_mcfg_osc_port_ta = lv_textarea_create(ov);
    lv_obj_set_size(s_mcfg_osc_port_ta, 140, field_h);
    lv_obj_align(s_mcfg_osc_port_ta, LV_ALIGN_TOP_LEFT, 80, 56 + row_dy * 3);
    lv_textarea_set_one_line(s_mcfg_osc_port_ta, true);
    lv_textarea_set_max_length(s_mcfg_osc_port_ta, 5);
    lv_obj_add_event_cb(s_mcfg_osc_port_ta, on_mcfg_textarea_focused,
                        LV_EVENT_FOCUSED, NULL);

    s_mcfg_status_label = lv_label_create(ov);
    lv_label_set_text(s_mcfg_status_label, "");
    lv_label_set_recolor(s_mcfg_status_label, true);
    lv_obj_align(s_mcfg_status_label, LV_ALIGN_TOP_LEFT, 0, 56 + row_dy * 4 + 4);

    s_mcfg_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_mcfg_keyboard, SCREEN_W - 32, SCREEN_H / 2);
    lv_obj_align(s_mcfg_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_mcfg_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_mcfg_keyboard, on_mcfg_keyboard_event, LV_EVENT_ALL, NULL);
    // Override TEXT_LOWER with a single-screen alpha+digits+'.' layout for
    // the host field. Other modes (NUMBER for ports) keep LVGL defaults.
    lv_keyboard_set_map(s_mcfg_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER,
                        mcfg_alphanum_map, mcfg_alphanum_ctrl);
}

static void mcfg_open(void)
{
    if (!s_mcfg_overlay) build_mcfg_overlay();
    const char *host = app_config_ms_host();
    char port_s[8];
    char osc_port_s[8];
    snprintf(port_s,     sizeof(port_s),     "%u", (unsigned) app_config_ms_port());
    snprintf(osc_port_s, sizeof(osc_port_s), "%u", (unsigned) app_config_ms_osc_port());
    lv_textarea_set_text(s_mcfg_host_ta, host);
    lv_textarea_set_text(s_mcfg_port_ta, port_s);
    if (s_mcfg_osc_port_ta) lv_textarea_set_text(s_mcfg_osc_port_ta, osc_port_s);
    strncpy(s_mcfg_orig_host, host, sizeof(s_mcfg_orig_host) - 1);
    s_mcfg_orig_host[sizeof(s_mcfg_orig_host) - 1] = '\0';
    strncpy(s_mcfg_orig_port, port_s, sizeof(s_mcfg_orig_port) - 1);
    s_mcfg_orig_port[sizeof(s_mcfg_orig_port) - 1] = '\0';
    strncpy(s_mcfg_orig_osc_port, osc_port_s, sizeof(s_mcfg_orig_osc_port) - 1);
    s_mcfg_orig_osc_port[sizeof(s_mcfg_orig_osc_port) - 1] = '\0';

    s_mcfg_orig_proto   = (int) app_config_ms_protocol();
    s_mcfg_proto_staged = s_mcfg_orig_proto;
    if (s_mcfg_proto_ws_btn && s_mcfg_proto_osc_btn) {
        lv_obj_t *btns[2] = { s_mcfg_proto_ws_btn, s_mcfg_proto_osc_btn };
        update_radio_visuals(btns, 2, (size_t) s_mcfg_orig_proto);
    }

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
    show_rebooting_overlay();
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
        // W6.1: a stale saved id pointing at a non-routable strip would have
        // no widget to uncheck it; force-clear so save can persist a clean
        // selection without the user having to do anything.
        bool routable = !(s_ms && s_ms->is_channel_routable) ||
                        s_ms->is_channel_routable(i);
        if (routable) {
            for (size_t j = 0; j < cur_count; ++j) {
                if (cur[j] == i) {
                    s_chpick_state[i] = true;
                    break;
                }
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
        // W6.1: skip non-routable channel types (mix/matrix/main). Indices
        // stay aligned to MS channel ids -- s_chpick_state[i] sticks to
        // its semantic meaning, just no widget for the skipped slot.
        if (s_ms && s_ms->is_channel_routable && !s_ms->is_channel_routable(i)) {
            continue;
        }
        lv_obj_t *row = lv_obj_create(s_chpick_list);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *cb = lv_checkbox_create(row);
        char buf[24];
        // P3: prefer the MS-side scribble-strip name; fall back to the
        // CH NN placeholder when MS hasn't named the strip yet (or the
        // initial sweep hasn't reached this id, e.g. on a flaky network).
        const char *name = (s_ms && s_ms->get_strip_name)
                               ? s_ms->get_strip_name(i) : NULL;
        if (name) snprintf(buf, sizeof(buf), "%s", name);
        else      snprintf(buf, sizeof(buf), "CH %02d", i + 1);
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

// P3: re-pull every checkbox label from the all-strip-name cache. Called
// from ms_apply_async on every name broadcast so live renames in MS show
// in the picker without re-opening it. Gated on overlay visibility so a
// background rename is essentially free.
static void chpick_refresh_labels(void)
{
    if (!s_chpick_overlay) return;
    if (lv_obj_has_flag(s_chpick_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    if (!s_ms || !s_ms->get_strip_name) return;
    int bound = s_total_channels < APP_UI_MAX_PICKER_ROWS
                ? s_total_channels : APP_UI_MAX_PICKER_ROWS;
    for (int i = 0; i < bound; ++i) {
        if (!s_chpick_checks[i]) continue;
        char buf[24];
        const char *name = s_ms->get_strip_name(i);
        if (name) snprintf(buf, sizeof(buf), "%s", name);
        else      snprintf(buf, sizeof(buf), "CH %02d", i + 1);
        lv_checkbox_set_text(s_chpick_checks[i], buf);
    }
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
    s_toast = lv_obj_create(scr);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 100);
    lv_obj_set_style_pad_all(s_toast, 16, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    // Bright amber on dark text — high contrast against the dark theme's
    // near-black backgrounds and the saturated slider colors. Default theme
    // styling rendered as dark-grey-on-dark-grey, which user reported as
    // hard to see when a slider was nearby.
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0xF0B030), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);

    s_toast_label = lv_label_create(s_toast);
    lv_label_set_text(s_toast_label, "");
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0x101010), 0);
    lv_obj_center(s_toast_label);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
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

    // Master strip — same gating as the per-channel strips.
    if (s_master_widgets.slider) {
        lv_obj_set_style_opa(s_master_widgets.slider,
                             ms_ok ? LV_OPA_COVER : LV_OPA_50, 0);
        if (ms_ok) lv_obj_add_flag   (s_master_widgets.slider, LV_OBJ_FLAG_CLICKABLE);
        else       lv_obj_remove_flag(s_master_widgets.slider, LV_OBJ_FLAG_CLICKABLE);
    }
    if (s_master_widgets.btn_mute) {
        lv_obj_set_style_opa(s_master_widgets.btn_mute,
                             mute_ok ? LV_OPA_COVER : LV_OPA_50, 0);
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

static void on_sntp_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "sntp synced: epoch=%lld", (long long) tv->tv_sec);
}


// File-static buffer holding the live SNTP server hostname. lwIP
// (sntp_setservername under the wrapper) stores the pointer we pass,
// not a copy, so it must remain valid for the lifetime of the SNTP
// service. Stack-buffered hostnames cause "first sync works, then
// stops" races. Idempotent: re-reading from prefs into the same buffer
// updates the contents in place; the pointer LWIP holds is unchanged.
static char s_ntp_server_static[APP_PREFS_STR_MAX];

// (Re)initialise SNTP from the current ntp_server / ntp_use_dhcp prefs.
// Safe to call before WiFi is up (the wrapper just defers the first
// poll). Safe to call repeatedly -- esp_netif_sntp_deinit + re-init
// applies the new config live, so the user's wifi-config save can
// trigger a server change without a reboot.
static void apply_sntp_config(void)
{
    app_prefs_get_ntp_server(s_ntp_server_static, sizeof(s_ntp_server_static));
    if (s_ntp_server_static[0] == '\0') return;   // no server configured
    bool use_dhcp = app_prefs_get_ntp_use_dhcp();
    esp_netif_sntp_deinit();   // safe if never initialised
    if (use_dhcp) {
        // 2 slots, both pre-filled with the manual server. With
        // server_from_dhcp=true and index_of_first_server=1, lwIP
        // overwrites slot 0 when DHCP option-42 arrives. If the network
        // never provides one, slot 0 keeps its initial value and the
        // SNTP poll still has live targets, so the clock syncs from
        // the manual server.
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST(s_ntp_server_static,
                                    s_ntp_server_static));
        cfg.server_from_dhcp           = true;
        cfg.renew_servers_after_new_IP = true;
        cfg.index_of_first_server      = 1;
        cfg.ip_event_to_renew          = IP_EVENT_STA_GOT_IP;
        esp_netif_sntp_init(&cfg);
    } else {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server_static);
        esp_netif_sntp_init(&cfg);
    }
    ESP_LOGI(TAG, "sntp: server='%s' use_dhcp=%d",
             s_ntp_server_static, use_dhcp);
}

static void start_clock_once(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;

    // Visibility: a stuck "--:-- --" status-bar clock = SNTP never synced.
    // This callback fires once per successful sync (initial sync + later
    // re-syncs). The log line correlates against the user-visible clock.
    sntp_set_time_sync_notification_cb(on_sntp_synced);

    apply_sntp_config();

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
    if (s_wcfg_overlay && !lv_obj_has_flag(s_wcfg_overlay, LV_OBJ_FLAG_HIDDEN)) {
        wcfg_refresh_current_ip();
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
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
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

// MS+console combined state. WS state alone isn't the whole story --
// MS may be reachable but its physical console hasn't been powered on
// yet. /app/state == "connected" gates the second half via
// is_console_attached. Color/label distinguish the three useful cases.
static uint32_t ms_combined_color(app_ms_state_t st)
{
    if (st == APP_MS_STATE_CONNECTED &&
        s_ms && s_ms->is_console_attached &&
        !s_ms->is_console_attached()) {
        return 0xE0D040;  // yellow — MS up, waiting on console
    }
    return ms_state_color(st);
}

static const char *ms_combined_text(app_ms_state_t st)
{
    if (st == APP_MS_STATE_CONNECTED &&
        s_ms && s_ms->is_console_attached &&
        !s_ms->is_console_attached()) {
        return "Console offline";
    }
    return ms_state_text(st);
}

static void ms_icon_refresh(void)
{
    if (!s_ms_icon_label) return;
    lv_obj_set_style_text_color(s_ms_icon_label,
                                lv_color_hex(ms_combined_color(ms_state_now())),
                                0);
}

static void ms_panel_refresh(void)
{
    if (!s_ms_panel) return;
    app_ms_state_t st = ms_state_now();
    lv_label_set_text(s_ms_state_value, ms_combined_text(st));
    lv_obj_set_style_text_color(s_ms_state_value,
                                lv_color_hex(ms_combined_color(st)), 0);
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
    // P5: AND-gated visibility lives here, not in the WS callback. After
    // a reconnect the iface flips both halves of the gate (mix_list_ready
    // re-arms on WEBSOCKET_EVENT_CONNECTED; the connected state via
    // get_state) and a single sweep makes both visible at once.
    mix_indicator_apply_visibility();
    mix_indicator_refresh();
    // P11: routed-set changes invalidate the picker grid (mix-row count
    // and ordering depends on the mask). Tear down so the next open
    // rebuilds — rare event, the heap-churn comment below applies to
    // per-broadcast updates not routing flips.
    if (s_mix_picker_popup && s_ms && s_ms->is_mix_routed) {
        uint32_t live_mask = 0;
        for (int i = 0; i < s_mix_count && i < 32; ++i) {
            if (s_ms->is_mix_routed(i)) live_mask |= (1u << i);
        }
        if (live_mask != s_picker_routed_mask) {
            lv_obj_delete(s_mix_picker_popup);
            s_mix_picker_popup = NULL;
            memset(s_mix_picker_btn_labels, 0, sizeof(s_mix_picker_btn_labels));
        }
    }
    // Update labels in place rather than tearing down the popup. The
    // earlier "delete on every notify" pattern accumulated heap churn
    // (~31 LVGL widgets per rebuild × every mix change) and after a
    // long session the LVGL allocator's free-list looped or
    // corrupted, hanging taskLVGL inside lv_malloc.
    mix_picker_refresh_labels();
    // P3: refresh chpick rows from the all-strip-name cache so a live
    // MS rename shows without re-open. No-op if the overlay is closed.
    chpick_refresh_labels();
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
    if (s_master_widgets.box) {
        if (ms_connected) lv_obj_remove_flag(s_master_widgets.box, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag   (s_master_widgets.box, LV_OBJ_FLAG_HIDDEN);
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
    if (!lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) return;
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
