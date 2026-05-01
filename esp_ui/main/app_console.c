#include "app_console.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/base64.h"

#include "app_logd.h"
#include "app_storage.h"

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

static int cmd_cat_b64(int argc, char **argv)
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

// ─────────────────────────────────────────────────────────────────────────
// `coredump-b64` — emit the flash `coredump` partition as base64. Same
// framing as cat-b64. Useful when SD failed to mount and we can't write
// the dump to /sdcard — we read the partition directly over UART instead.
// ─────────────────────────────────────────────────────────────────────────

static int cmd_coredump_b64(int argc, char **argv)
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
// REPL bootstrap.
// ─────────────────────────────────────────────────────────────────────────

void app_console_init(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "ls",           .help = "list files in a directory (default /sdcard)", .func = cmd_ls           },
        { .command = "cat-b64",      .help = "base64-print a file framed with markers",     .func = cmd_cat_b64      },
        { .command = "coredump-b64", .help = "base64-print the flash coredump partition",   .func = cmd_coredump_b64 },
        { .command = "log-trace",    .help = "query or toggle disk-log trace level (on|off)", .func = cmd_log_trace  },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_repl_t        *repl        = NULL;
    esp_console_repl_config_t  repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt   = "monmix> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "REPL ready on UART0 — type 'help' for commands");
}
