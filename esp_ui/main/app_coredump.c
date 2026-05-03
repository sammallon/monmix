#include "app_coredump.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "app_storage.h"

static const char *TAG = "app_coredump";

// Scan the SD root for existing coredump-NNNN.elf and return the next
// available sequence number. Wraps to 1 if 9999 has been hit.
static int next_seq(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return 1;
    }
    int max_seen = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(e->d_name, "coredump-%d.elf", &n) == 1 && n > max_seen) {
            max_seen = n;
        }
    }
    closedir(d);
    return (max_seen >= 9999) ? 1 : (max_seen + 1);
}

bool app_coredump_flush_to_sd(void)
{
    // Silence IDF's "Failed to read data from core dump (260)!" / ESP_ERR_INVALID_SIZE
    // chatter on the no-dump-present case — that's our normal clean-boot state.
    // Restore the prior level afterwards so a real partition-corruption case
    // still gets visibility on the next attempted dump.
    esp_log_level_t prev = esp_log_level_get("esp_core_dump_flash");
    esp_log_level_set("esp_core_dump_flash", ESP_LOG_NONE);
    esp_err_t err = esp_core_dump_image_check();
    esp_log_level_set("esp_core_dump_flash", prev);

    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGI(TAG, "clean boot — no coredump in flash");
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump in flash failed integrity check: %s", esp_err_to_name(err));
        // Leave a corrupt dump in place — operator can pull it manually with
        // `idf.py coredump-info` over UART.
        return false;
    }

    if (!app_storage_is_mounted()) {
        ESP_LOGW(TAG, "coredump present but SD is not mounted — leaving in place");
        return false;
    }

    size_t dump_size = 0;
    size_t dump_addr = 0;
    err = esp_core_dump_image_get(&dump_addr, &dump_size);
    if (err != ESP_OK || dump_size == 0) {
        ESP_LOGE(TAG, "esp_core_dump_image_get: %s, size=%u",
                 esp_err_to_name(err), (unsigned) dump_size);
        return false;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        ESP_LOGE(TAG, "coredump partition not present in partition table");
        return false;
    }

    char path[64];
    int  seq = next_seq(app_storage_mount_point());
    snprintf(path, sizeof(path), "%s/coredump-%04d.elf",
             app_storage_mount_point(), seq);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed", path);
        return false;
    }

    static uint8_t buf[4096];
    size_t off  = 0;
    size_t left = dump_size;
    while (left > 0) {
        size_t n = (left > sizeof(buf)) ? sizeof(buf) : left;
        err = esp_partition_read(part, off, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_read off=%u: %s",
                     (unsigned) off, esp_err_to_name(err));
            fclose(f);
            unlink(path);
            return false;
        }
        if (fwrite(buf, 1, n, f) != n) {
            ESP_LOGE(TAG, "short fwrite to %s", path);
            fclose(f);
            unlink(path);
            return false;
        }
        off  += n;
        left -= n;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "fclose %s failed", path);
        return false;
    }

    ESP_LOGI(TAG, "saved %u-byte coredump to %s", (unsigned) dump_size, path);

    err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_core_dump_image_erase: %s — partition not cleared",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "coredump partition cleared");
    }
    return true;
}
