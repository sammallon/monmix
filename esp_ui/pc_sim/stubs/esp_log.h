// Stub of esp_log.h — ESP_LOGx maps to printf with a tag prefix. The
// tablet ships a richer log subsystem, but for the sim we just want
// observability in the terminal.
#pragma once

#include <stdint.h>
#include <stdio.h>

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

#define ESP_LOG_LOCAL_LEVEL ESP_LOG_INFO

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stdout, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fprintf(stdout, "D (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) fprintf(stdout, "V (%s) " fmt "\n", tag, ##__VA_ARGS__)

static inline uint32_t esp_log_timestamp(void) { return 0; }
static inline void     esp_log_level_set(const char *tag, esp_log_level_t level) { (void)tag; (void)level; }
