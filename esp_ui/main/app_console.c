#include "app_console.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "miniz.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_logd.h"
#include "app_ms_client.h"
#include "app_ms_info.h"
#include "app_prefs.h"
#include "app_storage.h"
#include "app_touch_inject.h"
#include "app_ui.h"

static const char *TAG = "app_console";

// ─────────────────────────────────────────────────────────────────────────
// `ls <path>` — list directory entries with sizes.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : app_storage_mount_point();
    DIR *d = opendir(path);
    if (!d) {
        printf("ls: cannot open %s\n", path);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[600];   // path (≤256) + '/' + d_name (≤256) + NUL
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            printf("%10ld  %s%s\n",
                   (long) st.st_size,
                   e->d_name,
                   S_ISDIR(st.st_mode) ? "/" : "");
        } else {
            printf("       ???  %s\n", e->d_name);
        }
    }
    closedir(d);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `cat-b64 <path>` — base64-encode a file to UART, framed with markers so a
// host script can extract the payload without picking up interleaved log
// lines or shell prompts.
//
// Output:
//   ===BEGIN BASE64 <path> SIZE <bytes> ===
//   <base64 lines, 76 chars each>
//   ===END BASE64===
// ─────────────────────────────────────────────────────────────────────────

static int cmd_cat_b64_impl(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cat-b64 <path>\n");
        return 1;
    }
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cat-b64: cannot open %s\n", path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    printf("===BEGIN BASE64 %s SIZE %ld ===\n", path, total);

    // Read 57 raw bytes per chunk → exactly 76 base64 chars (one line).
    uint8_t  in[57];
    uint8_t  out[80];
    size_t   olen;
    size_t   n;
    while ((n = fread(in, 1, sizeof(in), f)) > 0) {
        if (mbedtls_base64_encode(out, sizeof(out), &olen, in, n) != 0) {
            printf("\ncat-b64: encode failed\n");
            fclose(f);
            printf("===END BASE64===\n");
            return 1;
        }
        fwrite(out, 1, olen, stdout);
        fputc('\n', stdout);
    }
    fclose(f);
    printf("===END BASE64===\n");
    return 0;
}

// Wrap the cat-b64 emit with an ESP_LOG quiesce. Same reason as
// cmd_screenshot: any ESP_LOG that fires from a non-console task
// (ms_ws disconnect/reconnect, hb integrity probe, etc.) lands on the
// same UART and pollutes the b64 stream. We can't recover a partial
// line on the host, so we just suppress logs while the file's payload
// is being emitted. The SD-side APP_LOGD continues normally.
static int cmd_cat_b64(int argc, char **argv)
{
    esp_log_level_t prior = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    int rc = cmd_cat_b64_impl(argc, argv);
    esp_log_level_set("*", prior);
    return rc;
}

// ─────────────────────────────────────────────────────────────────────────
// `coredump-b64` — emit the flash `coredump` partition as base64. Same
// framing as cat-b64. Useful when SD failed to mount and we can't write
// the dump to /sdcard — we read the partition directly over UART instead.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_coredump_b64_impl(int argc, char **argv)
{
    (void) argc; (void) argv;

    esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_ERR_NOT_FOUND) {
        printf("coredump-b64: no dump in flash partition\n");
        return 0;
    }
    if (err != ESP_OK) {
        printf("coredump-b64: integrity check failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        printf("coredump-b64: image_get failed\n");
        return 1;
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        printf("coredump-b64: partition not found\n");
        return 1;
    }

    printf("===BEGIN BASE64 coredump SIZE %u ===\n", (unsigned) size);

    uint8_t  in[57];
    uint8_t  out[80];
    size_t   olen;
    size_t   off  = 0;
    size_t   left = size;
    while (left > 0) {
        size_t n = (left > sizeof(in)) ? sizeof(in) : left;
        if (esp_partition_read(part, off, in, n) != ESP_OK) {
            printf("\ncoredump-b64: partition read failed at %u\n", (unsigned) off);
            printf("===END BASE64===\n");
            return 1;
        }
        if (mbedtls_base64_encode(out, sizeof(out), &olen, in, n) != 0) {
            printf("\ncoredump-b64: encode failed\n");
            printf("===END BASE64===\n");
            return 1;
        }
        fwrite(out, 1, olen, stdout);
        fputc('\n', stdout);
        off  += n;
        left -= n;
    }
    printf("===END BASE64===\n");
    return 0;
}

// Wrap with ESP_LOG quiesce — same reason as cmd_screenshot / cmd_cat_b64.
static int cmd_coredump_b64(int argc, char **argv)
{
    esp_log_level_t prior = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    int rc = cmd_coredump_b64_impl(argc, argv);
    esp_log_level_set("*", prior);
    return rc;
}

// ─────────────────────────────────────────────────────────────────────────
// `screenshot` — capture the current LVGL screen as RGB565, prefix a small
// magic+dimensions header, and base64-stream the lot through the same
// BEGIN/END markers as cat-b64. Decoded host-side by tools/fetch_screenshot.py.
// Useful for closed-loop UI verification: take, send touch, take again, diff.
// ─────────────────────────────────────────────────────────────────────────

static void emit_b64_line(const uint8_t *src, size_t n)
{
    uint8_t out[80];
    size_t  olen = 0;
    if (mbedtls_base64_encode(out, sizeof(out), &olen, src, n) == 0) {
        fwrite(out, 1, olen, stdout);
        fputc('\n', stdout);
    }
}

static int cmd_screenshot_impl(void);

static int cmd_screenshot(int argc, char **argv)
{
    (void) argc; (void) argv;

    // Silence other tasks' UART logging for the duration of the screenshot.
    // The base64 emit takes ~2 s of dense UART writes; any concurrent
    // ESP_LOG output (WS broadcast handlers, wifi events, logd, ...)
    // interleaves with the base64 stream and breaks the host-side
    // BEGIN/END marker parsing. The wrapper layer takes care of restoring
    // the prior level so every return path inside the impl stays simple.
    esp_log_level_t prior = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    int rc = cmd_screenshot_impl();
    esp_log_level_set("*", prior);
    return rc;
}

static int cmd_screenshot_impl(void)
{
    // The default LVGL draw-buffer allocator pulls from DMA-capable internal
    // RAM (so the PPA can hit it during normal flush). At 1024×600×2 bytes =
    // 1.2 MB that's bigger than the entire internal pool. Snapshots don't
    // need DMA-capable memory — we route the allocation to PSRAM ourselves
    // and feed the buffer through lv_snapshot_take_to_draw_buf.
    const uint32_t snap_w = 1024;
    const uint32_t snap_h = 600;
    const size_t   snap_pixels_size = snap_w * snap_h * 2;       // RGB565
    const size_t   snap_alloc_size  = snap_pixels_size + 256;    // alignment slack
    // Trace-level entry log so the SD post-mortem can correlate
    // screenshot attempts with heartbeat/heap state — caller's
    // esp_log_level_set quiesce only suppresses ESP_LOGx, not APP_LOGD
    // (which writes to /sdcard/monmix-NNNN.log directly).
    APP_LOGD_T("screenshot", "begin");

    uint8_t *snap_raw = heap_caps_aligned_alloc(64, snap_alloc_size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap_raw) {
        printf("screenshot: PSRAM alloc failed (need %u bytes)\n",
               (unsigned) snap_alloc_size);
        APP_LOGD_E("screenshot", "PSRAM alloc failed (need %u)",
                   (unsigned) snap_alloc_size);
        return 1;
    }

    lv_draw_buf_t db;
    lv_draw_buf_init(&db, snap_w, snap_h, LV_COLOR_FORMAT_RGB565,
                     0 /* auto stride */, snap_raw, snap_alloc_size);

    if (!lvgl_port_lock(2000)) {
        free(snap_raw);
        printf("screenshot: lvgl_port_lock timeout\n");
        APP_LOGD_E("screenshot", "lvgl_port_lock timeout (2000 ms)");
        return 1;
    }
    lv_obj_t   *scr = lv_screen_active();
    lv_result_t r   = scr
        ? lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &db)
        : LV_RESULT_INVALID;
    lvgl_port_unlock();
    if (r != LV_RESULT_OK) {
        free(snap_raw);
        printf("screenshot: snapshot failed (scr=%p result=%d)\n", scr, (int) r);
        APP_LOGD_E("screenshot", "snapshot failed scr=%p result=%d",
                   scr, (int) r);
        return 1;
    }

    // From here on, treat &db like the buffer that lv_snapshot_take used to
    // return — same fields. The free at the end is on snap_raw, not via
    // lv_draw_buf_destroy (we own the underlying allocation).
    lv_draw_buf_t *buf = &db;

    // Compress pixels with deflate via the ROM miniz. Mostly-solid-color UI
    // screens compress 5–10× — at 1024×600 RGB565 that's roughly 1.2 MB →
    // 150 KB, cutting the b64 transfer at 921600 baud from ~17 s to ~2 s.
    size_t   src_len = buf->data_size;
    size_t   cap     = src_len + 256;   // worst-case over-estimate
    uint8_t *cbuf    = malloc(cap);
    if (!cbuf) {
        free(snap_raw);
        printf("screenshot: out of memory for compress buffer\n");
        APP_LOGD_E("screenshot", "compress buffer alloc failed (need %u)",
                   (unsigned) cap);
        return 1;
    }
    // tdefl_compress_mem_to_mem internally callocs a tdefl_compressor, but
    // ROM-resident miniz is built with MINIZ_NO_MALLOC and that allocator
    // path is broken (returns NULL → function returns 0 = "deflate failed").
    // Pre-allocate the compressor ourselves in PSRAM and drive
    // tdefl_compress directly. Per the header, the tdefl API "does not
    // dynamically allocate memory" — once the compressor struct exists,
    // everything works in user-provided buffers.
    tdefl_compressor *comp = malloc(sizeof(*comp));
    if (!comp) {
        free(cbuf);
        free(snap_raw);
        printf("screenshot: tdefl_compressor alloc failed (need %u bytes)\n",
               (unsigned) sizeof(*comp));
        APP_LOGD_E("screenshot", "tdefl_compressor alloc failed (need %u)",
                   (unsigned) sizeof(*comp));
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    tdefl_init(comp, NULL, NULL,
               TDEFL_WRITE_ZLIB_HEADER | TDEFL_DEFAULT_MAX_PROBES);
    size_t       in_size  = src_len;
    size_t       out_size = cap;
    tdefl_status st = tdefl_compress(comp, buf->data, &in_size,
                                     cbuf, &out_size, TDEFL_FINISH);
    int64_t t_compress_us = esp_timer_get_time() - t0;
    free(comp);
    if (st != TDEFL_STATUS_DONE) {
        printf("screenshot: deflate failed (status=%d in_consumed=%u out_used=%u)\n",
               (int) st, (unsigned) in_size, (unsigned) out_size);
        APP_LOGD_E("screenshot", "deflate failed status=%d in=%u out=%u",
                   (int) st, (unsigned) in_size, (unsigned) out_size);
        free(cbuf);
        free(snap_raw);
        return 1;
    }
    size_t comp_len = out_size;
    // Stats line printed BEFORE the BEGIN marker so the host can pick up
    // device-side timings and compute the full compression accounting.
    printf("screenshot-stats: src=%u comp=%u ratio=%.1f%% compress_us=%lld\n",
           (unsigned) src_len, (unsigned) comp_len,
           100.0 * (double) comp_len / (double) src_len,
           (long long) t_compress_us);

    // 36-byte header, multiple of 3 for clean base64. The host parses it,
    // then zlib-decompresses the rest of the payload.
    struct __attribute__((packed)) {
        char     magic[8];           // "MMSCRN\0\0"
        uint32_t w;
        uint32_t h;
        uint32_t stride;
        uint32_t format;             // lv_color_format_t (RGB565 = 0x12)
        uint32_t uncompressed_size;
        uint32_t compressed_size;
        uint32_t flags;              // bit 0 = zlib-compressed payload
    } hdr;
    memcpy(hdr.magic, "MMSCRN\0\0", 8);
    hdr.w                 = buf->header.w;
    hdr.h                 = buf->header.h;
    hdr.stride            = buf->header.stride;
    hdr.format            = (uint32_t) buf->header.cf;
    hdr.uncompressed_size = (uint32_t) src_len;
    hdr.compressed_size   = (uint32_t) comp_len;
    hdr.flags             = 1;

    size_t total = sizeof(hdr) + comp_len;
    printf("===BEGIN BASE64 screenshot SIZE %u ===\n", (unsigned) total);

    emit_b64_line((const uint8_t *)&hdr, sizeof(hdr));

    size_t off = 0;
    while (off < comp_len) {
        size_t n = comp_len - off;
        if (n > 57) n = 57;
        emit_b64_line(cbuf + off, n);
        off += n;
    }

    free(cbuf);
    free(snap_raw);
    printf("===END BASE64===\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `touch <x> <y> [tap|down|up]` — drive synthetic touches into LVGL via the
// virtual indev registered by app_touch_inject. Coordinates are LVGL
// logical pixels (same frame as `screenshot` output).
//   tap (default): press, hold for one LVGL refresh, release.
//   down:          press and hold (until subsequent `up`).
//   up:            release at the last position.
// ─────────────────────────────────────────────────────────────────────────

#define TOUCH_TAP_HOLD_MS  80   // ≥ one LVGL refresh tick at default 33 ms

static int cmd_touch(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: touch <x> <y> [tap|down|up]\n");
        return 1;
    }
    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    const char *action = (argc >= 4) ? argv[3] : "tap";

    if (strcmp(action, "tap") == 0) {
        app_touch_inject_set(x, y, true);
        vTaskDelay(pdMS_TO_TICKS(TOUCH_TAP_HOLD_MS));
        app_touch_inject_set(x, y, false);
        printf("tap at (%d, %d)\n", x, y);
    } else if (strcmp(action, "down") == 0) {
        app_touch_inject_set(x, y, true);
        printf("down at (%d, %d)\n", x, y);
    } else if (strcmp(action, "up") == 0) {
        app_touch_inject_set(x, y, false);
        printf("up at (%d, %d)\n", x, y);
    } else {
        printf("usage: touch <x> <y> [tap|down|up]\n");
        return 1;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `level-format [norm|db]` — query/set how the per-fader value label is
// rendered. Persisted to /sdcard/monmix-prefs.json.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_level_format(int argc, char **argv)
{
    if (argc < 2) {
        printf("level-format: %s\n",
               app_prefs_get_level_format() == APP_LEVEL_FORMAT_DB ? "db" : "norm");
        return 0;
    }
    if (strcmp(argv[1], "norm") == 0) {
        app_prefs_set_level_format(APP_LEVEL_FORMAT_NORM);
        const ms_client_iface_t *ms = app_ms_client_ws();
        if (ms && ms->set_level_format) ms->set_level_format(APP_LEVEL_FORMAT_NORM);
        printf("level-format: norm\n");
        return 0;
    }
    if (strcmp(argv[1], "db") == 0) {
        app_prefs_set_level_format(APP_LEVEL_FORMAT_DB);
        const ms_client_iface_t *ms = app_ms_client_ws();
        if (ms && ms->set_level_format) ms->set_level_format(APP_LEVEL_FORMAT_DB);
        printf("level-format: db\n");
        return 0;
    }
    printf("usage: level-format [norm|db]\n");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────
// `signal-indicator [none|signal-present|meter]` — query/set the per-fader
// activity overlay. Persisted to /sdcard/monmix-prefs.json.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_signal_indicator(int argc, char **argv)
{
    if (argc < 2) {
        const char *cur =
            (app_prefs_get_signal_indicator() == APP_SIGNAL_INDICATOR_NONE)  ? "none" :
            (app_prefs_get_signal_indicator() == APP_SIGNAL_INDICATOR_METER) ? "meter" :
                                                                                "signal-present";
        printf("signal-indicator: %s\n", cur);
        return 0;
    }
    if (strcmp(argv[1], "none") == 0) {
        app_prefs_set_signal_indicator(APP_SIGNAL_INDICATOR_NONE);
        printf("signal-indicator: none\n");
        return 0;
    }
    if (strcmp(argv[1], "signal-present") == 0) {
        app_prefs_set_signal_indicator(APP_SIGNAL_INDICATOR_PRESENT);
        printf("signal-indicator: signal-present\n");
        return 0;
    }
    if (strcmp(argv[1], "meter") == 0) {
        app_prefs_set_signal_indicator(APP_SIGNAL_INDICATOR_METER);
        printf("signal-indicator: meter\n");
        return 0;
    }
    printf("usage: signal-indicator [none|signal-present|meter]\n");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────
// `theme [dark|light]` — query/set UI theme. Persisted to
// /sdcard/monmix-prefs.json; pref-change subscriber re-applies live.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_theme(int argc, char **argv)
{
    if (argc < 2) {
        printf("theme: %s\n",
               app_prefs_get_theme() == APP_THEME_LIGHT ? "light" : "dark");
        return 0;
    }
    if (strcmp(argv[1], "dark") == 0) {
        app_prefs_set_theme(APP_THEME_DARK);
        printf("theme: dark\n");
        return 0;
    }
    if (strcmp(argv[1], "light") == 0) {
        app_prefs_set_theme(APP_THEME_LIGHT);
        printf("theme: light\n");
        return 0;
    }
    printf("usage: theme [dark|light]\n");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────
// `set-color <ch_id> <0..7|-1>` — paint a colored stripe on the channel's
// scribble strip. Index -1 clears the override (channel renders without
// an accent). Persisted to /sdcard/monmix-prefs.json.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_set_color(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set-color <ch_id> <0..7|-1>\n");
        return 1;
    }
    int ch_id = atoi(argv[1]);
    int idx   = atoi(argv[2]);
    if (ch_id < 0) {
        printf("set-color: bad channel id %d\n", ch_id);
        return 1;
    }
    if (idx < -1 || idx > 7) {
        printf("set-color: index must be -1..7\n");
        return 1;
    }
    app_prefs_set_channel_color(ch_id, idx);
    if (idx < 0) printf("set-color: ch=%d cleared\n", ch_id);
    else         printf("set-color: ch=%d -> %d\n", ch_id, idx);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `channels-reset` — clear the persisted channel selection so the next
// boot picks up the compile-time default. Used to upgrade a device that
// was seeded under an older default (e.g. 12 channels) to a newer one
// (24) without flashing the partition table or running `nvs erase`.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_channels_reset(int argc, char **argv)
{
    (void) argc; (void) argv;
    nvs_handle_t h;
    esp_err_t err = nvs_open("monmix", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        printf("channels-reset: nvs_open failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    nvs_erase_key(h, "chan_ids");
    nvs_commit(h);
    nvs_close(h);
    printf("channels-reset: cleared. Reboot to pick up the default.\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `ms-info` — fetch /console/information from the configured MS host and
// dump the parsed channel architecture. Sanity-checks the HTTP path
// before the discovery flow (#40) wires it into boot.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_ms_info(int argc, char **argv)
{
    (void) argc; (void) argv;
    app_ms_info_t info;
    if (!app_ms_info_fetch(app_config_ms_host(), app_config_ms_port(), &info)) {
        printf("ms-info: fetch failed (see ms_info ESP_LOGW above)\n");
        return 1;
    }
    printf("total=%d\n",            info.total);
    printf("input  count=%-3d offset=%d\n", info.input_count,  info.input_offset);
    printf("aux    count=%-3d offset=%d\n", info.aux_count,    info.aux_offset);
    printf("mix    count=%-3d offset=%d\n", info.mix_count,    info.mix_offset);
    printf("matrix count=%-3d offset=%d\n", info.matrix_count, info.matrix_offset);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `prefs-dump` — print effective + per-source (NVS, SD) prefs state with
// per-key mtimes. Used by P0 verification to confirm conflict-resolution
// + missing-key paths land correctly.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_prefs_dump(int argc, char **argv)
{
    (void) argc; (void) argv;
    app_prefs_debug_dump();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `log-trace [on|off]` — query or toggle the disk-logger's trace gate.
// Persisted in NVS so the choice survives reboots.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_log_trace(int argc, char **argv)
{
    if (argc < 2) {
        printf("log-trace: %s\n", app_logd_get_trace() ? "on" : "off");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        app_logd_set_trace(true);
        printf("log-trace: on\n");
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        app_logd_set_trace(false);
        printf("log-trace: off\n");
        return 0;
    }
    printf("usage: log-trace [on|off]\n");
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────
// `mix-show [on|off]` — diagnostic forced reveal of the mix-bus selector
// button. Bypasses the (ms_connected && mix_list_ready) gate so we can tell
// whether a stuck-hidden button is the gate's fault or a deeper layout/draw
// issue. With no arg, defaults to "on". Call `mix-show off` to release.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_mix_show(int argc, char **argv)
{
    bool on = true;
    if (argc >= 2) {
        if      (strcmp(argv[1], "on")  == 0) on = true;
        else if (strcmp(argv[1], "off") == 0) on = false;
        else { printf("usage: mix-show [on|off]\n"); return 1; }
    }
    app_ui_force_mix_show(on);
    printf("mix-show: %s\n", on ? "on (forced visible)" : "off (gated)");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// REPL bootstrap.
// ─────────────────────────────────────────────────────────────────────────

void app_console_init(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "ls",           .help = "list files in a directory (default /sdcard)",   .func = cmd_ls           },
        { .command = "cat-b64",      .help = "base64-print a file framed with markers",       .func = cmd_cat_b64      },
        { .command = "coredump-b64", .help = "base64-print the flash coredump partition",     .func = cmd_coredump_b64 },
        { .command = "screenshot",   .help = "base64-print an RGB565 screenshot of the UI",   .func = cmd_screenshot   },
        { .command = "touch",        .help = "<x> <y> [tap|down|up] — synthetic LVGL touch",  .func = cmd_touch        },
        { .command = "level-format", .help = "query/set fader value readout: norm | db",      .func = cmd_level_format },
        { .command = "signal-indicator", .help = "query/set: none | signal-present | meter",  .func = cmd_signal_indicator },
        { .command = "theme",        .help = "query/set UI theme: dark | light",              .func = cmd_theme        },
        { .command = "set-color",    .help = "<ch_id> <0..7|-1> — set/clear channel color",   .func = cmd_set_color    },
        { .command = "channels-reset", .help = "clear channel selection NVS, default applies next boot", .func = cmd_channels_reset },
        { .command = "ms-info",      .help = "fetch /console/information from MS, print channel arch", .func = cmd_ms_info      },
        { .command = "log-trace",    .help = "query or toggle disk-log trace level (on|off)", .func = cmd_log_trace    },
        { .command = "prefs-dump",   .help = "dump effective + NVS + SD prefs state (P0 verify)", .func = cmd_prefs_dump   },
        { .command = "mix-show",     .help = "[on|off] force mix-indicator visible (diagnose P5)", .func = cmd_mix_show     },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_repl_t        *repl        = NULL;
    esp_console_repl_config_t  repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt   = "monmix> ";
    repl_config.max_cmdline_length = 256;
    // Default REPL stack is 4 KB. The `screenshot` command runs LVGL's
    // render path on the REPL task and that overflows it (Guru Meditation:
    // stack-protection fault inside lv_snapshot_take_to_draw_buf). 16 KB
    // is comfortable for LVGL render + miniz compress + base64 emit.
    repl_config.task_stack_size = 16 * 1024;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "REPL ready on UART0 — type 'help' for commands");
}
