#include "app_console.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "mbedtls/base64.h"

#include "app_display.h"
#include "app_pp_client.h"
#include "app_pp_state.h"
#include "app_prefs.h"
#include "app_storage.h"
#include "app_wifi.h"
#include "esp_timer.h"
#include "esp_wifi.h"

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
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            printf("%10ld  %s%s\n",
                   (long) st.st_size, e->d_name,
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

// Wrap the b64 emit with an ESP_LOG quiesce — any log that fires from a
// non-console task lands on the same UART and pollutes the b64 stream.
static int cmd_cat_b64(int argc, char **argv)
{
    esp_log_level_t prior = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    int rc = cmd_cat_b64_impl(argc, argv);
    esp_log_level_set("*", prior);
    return rc;
}

// ─────────────────────────────────────────────────────────────────────────
// `coredump-b64` — emit the flash `coredump` partition as base64. Used when
// SD failed to mount and we can't write the dump to /sdcard.
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

static int cmd_coredump_b64(int argc, char **argv)
{
    esp_log_level_t prior = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    int rc = cmd_coredump_b64_impl(argc, argv);
    esp_log_level_set("*", prior);
    return rc;
}

// ─────────────────────────────────────────────────────────────────────────
// `set-bright <0..100>` — verify LCD backlight control. Persists via prefs.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_set_bright(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: set-bright <0..100>\n");
        printf("current: %u%%\n", (unsigned) app_prefs_get_brightness_pct());
        return 1;
    }
    int pct = atoi(argv[1]);
    if (pct < 0 || pct > 100) {
        printf("invalid: %d (must be 0..100)\n", pct);
        return 1;
    }
    app_prefs_set_brightness_pct((uint8_t) pct);
    app_display_set_backlight_pct((uint8_t) pct);
    printf("backlight=%d%%\n", pct);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `prefs-dump` — verify NVS + SD mirror state.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_prefs_dump(int argc, char **argv)
{
    (void) argc; (void) argv;
    app_prefs_debug_dump();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `wifi-stats` — confirm STA association + IP.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_wifi_stats(int argc, char **argv)
{
    (void) argc; (void) argv;
    char ip[16];
    app_wifi_format_ip(ip, sizeof(ip));
    printf("state=%d ssid='%s' ip=%s security=%s\n",
           (int) app_wifi_get_state(),
           app_wifi_get_ssid(),
           ip,
           app_wifi_get_security_str());
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
               ap.rssi,
               ap.bssid[0], ap.bssid[1], ap.bssid[2],
               ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `restart` — soft reset.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_restart(int argc, char **argv)
{
    (void) argc; (void) argv;
    printf("restarting...\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// ProPresenter — connection state + on-demand snapshot.
// ─────────────────────────────────────────────────────────────────────────

static const char *pp_conn_state_str(app_pp_conn_state_t s)
{
    switch (s) {
    case APP_PP_CONN_DISCONNECTED: return "disconnected";
    case APP_PP_CONN_CONNECTING:   return "connecting";
    case APP_PP_CONN_CONNECTED:    return "connected";
    case APP_PP_CONN_RECONNECTING: return "reconnecting";
    default:                       return "?";
    }
}

static const char *pp_timer_state_str(app_pp_timer_state_t s)
{
    switch (s) {
    case APP_PP_TIMER_STOPPED: return "stopped";
    case APP_PP_TIMER_RUNNING: return "running";
    case APP_PP_TIMER_OVERRUN: return "overrun";
    default:                   return "?";
    }
}

static int cmd_pp_status(int argc, char **argv)
{
    (void) argc; (void) argv;
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    printf("conn:    %s\n", pp_conn_state_str(pp->get_state()));

    app_pp_slide_t cur, nxt;
    if (app_pp_state_get_current_slide(&cur)) {
        printf("current: uuid=%s\n         text=\"%s\"\n", cur.uuid, cur.text);
    } else {
        printf("current: (none)\n");
    }
    if (app_pp_state_get_next_slide(&nxt)) {
        printf("next:    uuid=%s\n         text=\"%s\"\n", nxt.uuid, nxt.text);
    } else {
        printf("next:    (none)\n");
    }

    char msg[APP_PP_STAGE_MSG_MAX];
    app_pp_state_get_stage_message(msg, sizeof(msg));
    printf("stage:   \"%s\"\n", msg);

    app_pp_timer_t ts[APP_PP_MAX_TIMERS];
    size_t n = app_pp_state_get_timers(ts, APP_PP_MAX_TIMERS);
    printf("timers:  %u\n", (unsigned) n);
    for (size_t i = 0; i < n; ++i) {
        printf("  [%u] %-24s %s (%s)\n",
               (unsigned) i, ts[i].name, ts[i].time_str,
               pp_timer_state_str(ts[i].state));
    }

    uint64_t last = app_pp_state_last_update_ms();
    if (last == 0) {
        printf("activity: no updates yet\n");
    } else {
        uint64_t now = (uint64_t) (esp_timer_get_time() / 1000);
        uint64_t age = (now >= last) ? (now - last) : 0;
        printf("activity: last update %llu.%03llu s ago\n",
               age / 1000, age % 1000);
    }
    return 0;
}

static int cmd_pp_stage_msg(int argc, char **argv)
{
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    if (argc < 2 || strcmp(argv[1], "get") == 0) {
        char msg[APP_PP_STAGE_MSG_MAX];
        app_pp_state_get_stage_message(msg, sizeof(msg));
        printf("\"%s\"\n", msg);
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        bool ok = pp->stage_message_clear();
        printf("clear: %s\n", ok ? "ok" : "failed");
        return ok ? 0 : 1;
    }
    if (strcmp(argv[1], "put") == 0 && argc >= 3) {
        // Join argv[2..] with spaces for "put hello world"
        char buf[APP_PP_STAGE_MSG_MAX];
        size_t off = 0;
        for (int i = 2; i < argc; ++i) {
            size_t len = strlen(argv[i]);
            if (off + len + 2 >= sizeof(buf)) break;
            if (i > 2) buf[off++] = ' ';
            memcpy(buf + off, argv[i], len);
            off += len;
        }
        buf[off] = '\0';
        bool ok = pp->stage_message_put(buf);
        printf("put: %s\n", ok ? "ok" : "failed");
        return ok ? 0 : 1;
    }
    printf("usage: pp-stage-msg [get|put TEXT|clear]\n");
    return 1;
}

static int cmd_pp_trigger(int argc, char **argv)
{
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    if (argc < 2) {
        printf("usage: pp-trigger [next|previous]\n");
        return 1;
    }
    bool ok = false;
    if (strcmp(argv[1], "next") == 0)          ok = pp->trigger_next();
    else if (strcmp(argv[1], "previous") == 0) ok = pp->trigger_previous();
    else if (strcmp(argv[1], "prev") == 0)     ok = pp->trigger_previous();
    else {
        printf("unknown direction: %s\n", argv[1]);
        return 1;
    }
    printf("%s: %s\n", argv[1], ok ? "ok" : "failed");
    return ok ? 0 : 1;
}

static int cmd_pp_resub(int argc, char **argv)
{
    (void) argc; (void) argv;
    const app_pp_client_iface_t *pp = app_pp_client_tcp();
    pp->resubscribe();
    printf("resubscribe scheduled\n");
    return 0;
}

void app_console_init(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "ls",           .help = "list files in a directory (default /sdcard)", .func = cmd_ls           },
        { .command = "cat-b64",      .help = "base64-print a file framed with markers",     .func = cmd_cat_b64      },
        { .command = "coredump-b64", .help = "base64-print the flash coredump partition",   .func = cmd_coredump_b64 },
        { .command = "set-bright",   .help = "<0..100> -- set LCD backlight %",             .func = cmd_set_bright   },
        { .command = "prefs-dump",   .help = "dump effective + NVS + SD prefs state",       .func = cmd_prefs_dump   },
        { .command = "wifi-stats",   .help = "print STA state, IP, AP record",              .func = cmd_wifi_stats   },
        { .command = "restart",      .help = "soft reset",                                  .func = cmd_restart      },
        { .command = "pp-status",    .help = "PP connection state + cached slide/timer/msg snapshot", .func = cmd_pp_status    },
        { .command = "pp-stage-msg", .help = "[get|put TEXT|clear] -- engineer-to-musician stage msg", .func = cmd_pp_stage_msg },
        { .command = "pp-trigger",   .help = "[next|previous] -- drive active presentation",         .func = cmd_pp_trigger   },
        { .command = "pp-resub",     .help = "tear down + reconnect PP socket (debug)",              .func = cmd_pp_resub     },
    };
    for (size_t i = 0; i < sizeof(cmds)/sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_repl_t        *repl = NULL;
    esp_console_repl_config_t  rc   = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt             = "monpp> ";
    rc.max_cmdline_length = 256;
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &rc, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
