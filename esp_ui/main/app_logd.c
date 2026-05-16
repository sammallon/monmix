#include "app_logd.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_storage.h"

static const char *TAG = "app_logd";

#define NVS_NAMESPACE     "monmix"
#define NVS_KEY_TRACE     "logtrace"

// Each queue item is a pre-formatted line. 128 bytes covers the format
// prefix (~25 chars) plus ~100 chars of message — anything longer is
// truncated. 256 slots × 128 bytes = 32 KB, lives in PSRAM under our
// SPIRAM_USE_MALLOC config.
#define LOGD_LINE_MAX          128
#define QUEUE_DEPTH       256

#define FILE_MAX_BYTES    (256 * 1024)
#define KEEP_FILES        128
#define FLUSH_EVERY       16
#define FLUSH_INTERVAL_MS 5000
#define HEARTBEAT_MS      10000

typedef struct {
    char    buf[LOGD_LINE_MAX];
    uint8_t len;
} log_line_t;

static QueueHandle_t      s_queue    = NULL;
static FILE              *s_file     = NULL;
static int                s_seq      = 0;
static size_t             s_bytes    = 0;
static bool               s_trace_on = true;
static volatile uint32_t  s_dropped  = 0;

// Subscriber table. Small (4 slots is plenty -- typical usage: 1 for
// net-console, leaves headroom). Iterated from the worker task only,
// guarded by a mutex for register/unregister races against iteration.
#define LOGD_MAX_SUBSCRIBERS 4
typedef struct {
    app_logd_subscriber_t cb;
    void                 *ctx;
} logd_sub_t;
static logd_sub_t           s_subs[LOGD_MAX_SUBSCRIBERS];
static SemaphoreHandle_t    s_subs_mtx = NULL;

// ─────────────────────────────────────────────────────────────────────────
// File / sequence management
// ─────────────────────────────────────────────────────────────────────────

static int parse_seq(const char *name)
{
    int n = 0;
    if (sscanf(name, "monmix-%d.log", &n) == 1) {
        return n;
    }
    return -1;
}

static void scan_existing(int *out_max, int *out_min, int *out_count)
{
    int max_seq = 0;
    int min_seq = INT32_MAX;
    int count   = 0;
    DIR *d = opendir(app_storage_mount_point());
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = parse_seq(e->d_name);
            if (n >= 0) {
                if (n > max_seq) max_seq = n;
                if (n < min_seq) min_seq = n;
                count++;
            }
        }
        closedir(d);
    }
    if (count == 0) min_seq = 0;
    if (out_max)   *out_max   = max_seq;
    if (out_min)   *out_min   = min_seq;
    if (out_count) *out_count = count;
}

static void prune_oldest(int keep)
{
    while (1) {
        int min_seq = 0;
        int count   = 0;
        scan_existing(NULL, &min_seq, &count);
        if (count <= keep) break;
        char path[64];
        snprintf(path, sizeof(path), "%s/monmix-%04d.log",
                 app_storage_mount_point(), min_seq);
        if (unlink(path) != 0) {
            // Couldn't delete — bail to avoid an infinite loop if FS is wedged.
            ESP_LOGW(TAG, "unlink %s failed", path);
            break;
        }
    }
}

static bool open_new_file(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_seq++;
    char path[64];
    snprintf(path, sizeof(path), "%s/monmix-%04d.log",
             app_storage_mount_point(), s_seq);
    s_file = fopen(path, "wb");
    if (!s_file) {
        ESP_LOGW(TAG, "failed to open %s", path);
        return false;
    }
    s_bytes = 0;
    prune_oldest(KEEP_FILES);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// NVS persistence for the trace flag
// ─────────────────────────────────────────────────────────────────────────

static void load_trace_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, NVS_KEY_TRACE, &v) == ESP_OK) {
        s_trace_on = (v != 0);
    }
    nvs_close(h);
}

static void store_trace_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, NVS_KEY_TRACE, s_trace_on ? 1 : 0) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// ─────────────────────────────────────────────────────────────────────────
// Logger task
// ─────────────────────────────────────────────────────────────────────────

static void emit_dropped_summary_if_any(void)
{
    if (s_dropped == 0) return;
    uint32_t n = s_dropped;
    s_dropped  = 0;
    char hdr[80];
    int len = snprintf(hdr, sizeof(hdr),
                       "[%010u] [logd] W dropped %u event(s)\n",
                       (unsigned) esp_log_timestamp(), (unsigned) n);
    if (len > 0 && s_file) {
        fwrite(hdr, 1, (size_t) len, s_file);
        s_bytes += (size_t) len;
    }
}

static void logger_task(void *arg)
{
    int        unflushed  = 0;
    TickType_t last_flush = xTaskGetTickCount();
    while (1) {
        log_line_t item;
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_file == NULL || s_bytes >= FILE_MAX_BYTES) {
                if (!open_new_file()) {
                    // Without a file we can't do anything; back off so we
                    // don't spin. The data is already lost.
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }
            emit_dropped_summary_if_any();
            fwrite(item.buf, 1, item.len, s_file);
            s_bytes += item.len;
            unflushed++;
            // Fan out to subscribers. Held under the subs mutex so a
            // concurrent subscribe/unsubscribe can't observe the iteration
            // mid-flight. Subscribers are documented non-blocking.
            if (s_subs_mtx && xSemaphoreTake(s_subs_mtx, 0) == pdTRUE) {
                for (int i = 0; i < LOGD_MAX_SUBSCRIBERS; ++i) {
                    if (s_subs[i].cb) {
                        s_subs[i].cb(item.buf, item.len, s_subs[i].ctx);
                    }
                }
                xSemaphoreGive(s_subs_mtx);
            }
        }
        TickType_t now = xTaskGetTickCount();
        bool time_due = (now - last_flush) >= pdMS_TO_TICKS(FLUSH_INTERVAL_MS);
        if (s_file && unflushed > 0 && (unflushed >= FLUSH_EVERY || time_due)) {
            fflush(s_file);
            fsync(fileno(s_file));
            unflushed  = 0;
            last_flush = now;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Heartbeat — emits a line every 10 s with per-cap heap stats so quiet
// periods still leave time markers in the log AND we can watch
// fragmentation accumulating before it crashes us. The previous
// "free=N min=N" format just showed the dominant pool (usually PSRAM)
// and hid the internal-RAM exhaustion that bites the most.
//
// Once per minute (every 6 heartbeats) we also run
// heap_caps_check_integrity_all so corruption gets reported at the
// source instead of 22 minutes later inside lv_malloc.
// ─────────────────────────────────────────────────────────────────────────

#define HEARTBEAT_INTEGRITY_PERIOD 6

static void heartbeat_task(void *arg)
{
    (void) arg;
    int integ_ctr = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));

        multi_heap_info_t in, sp;
        heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
        heap_caps_get_info(&sp, MALLOC_CAP_SPIRAM);
        APP_LOGD_I("hb",
                   "int free=%u largest=%u min=%u | spi free=%u largest=%u",
                   (unsigned) in.total_free_bytes,
                   (unsigned) in.largest_free_block,
                   (unsigned) in.minimum_free_bytes,
                   (unsigned) sp.total_free_bytes,
                   (unsigned) sp.largest_free_block);

        if (++integ_ctr >= HEARTBEAT_INTEGRITY_PERIOD) {
            integ_ctr = 0;
            // print_errors=true makes the heap component dump a full
            // diagnostic to UART before we even get to log it ourselves;
            // the early dump is what we'd actually use to chase corruption.
            if (!heap_caps_check_integrity_all(true)) {
                APP_LOGD_E("hb", "HEAP CORRUPTION DETECTED");
                ESP_LOGE(TAG, "HEAP CORRUPTION DETECTED");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────

void app_logd_init(void)
{
    if (s_queue) return;   // idempotent
    if (!app_storage_is_mounted()) {
        ESP_LOGW(TAG, "SD not mounted; logd disabled");
        return;
    }

    load_trace_from_nvs();

    int max_seq = 0;
    scan_existing(&max_seq, NULL, NULL);
    s_seq = max_seq;
    if (!open_new_file()) {
        ESP_LOGW(TAG, "could not open log file; logd disabled");
        return;
    }

    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(log_line_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed; logd disabled");
        fclose(s_file);
        s_file = NULL;
        return;
    }

    xTaskCreate(logger_task,    "logd",    4096, NULL, 2, NULL);
    // Heartbeat stack bumped from 2048 — heap_caps_check_integrity_all
    // walks every block in every region and uses noticeable stack for
    // the per-region recursion.
    xTaskCreate(heartbeat_task, "logd_hb", 4096, NULL, 1, NULL);

    ESP_LOGI(TAG, "logd up; current=monmix-%04d.log; trace=%s",
             s_seq, s_trace_on ? "on" : "off");
    APP_LOGD_I("logd", "session start; trace=%s", s_trace_on ? "on" : "off");
}

void app_logd_emit(const char *tag, char lvl, const char *fmt, ...)
{
    if (!s_queue) return;
    if (lvl == 'T' && !s_trace_on) return;

    log_line_t item;
    int n = snprintf(item.buf, sizeof(item.buf), "[%010u] [%s] %c ",
                     (unsigned) esp_log_timestamp(), tag, lvl);
    if (n < 0 || n >= (int) sizeof(item.buf) - 1) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(item.buf + n, sizeof(item.buf) - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;

    int total = n + m;
    if (total >= (int) sizeof(item.buf) - 1) {
        total = sizeof(item.buf) - 2;   // leave room for newline + NUL
    }
    item.buf[total++] = '\n';
    item.len = (uint8_t) total;

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_dropped++;
    }
}

void app_logd_set_trace(bool on)
{
    s_trace_on = on;
    store_trace_to_nvs();
    APP_LOGD_I("logd", "trace=%s", on ? "on" : "off");
}

bool app_logd_get_trace(void)
{
    return s_trace_on;
}

// Subscriber registration. Returns false if the table is full. ctx
// distinguishes multiple registrations of the same callback. Safe to
// call from any task -- guarded by the same mutex the worker takes
// when iterating, so a registration arriving mid-iteration just waits
// one log line.
bool app_logd_subscribe(app_logd_subscriber_t cb, void *ctx)
{
    if (!cb) return false;
    if (!s_subs_mtx) {
        s_subs_mtx = xSemaphoreCreateMutex();
        if (!s_subs_mtx) return false;
    }
    bool added = false;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    for (int i = 0; i < LOGD_MAX_SUBSCRIBERS; ++i) {
        if (s_subs[i].cb == NULL) {
            s_subs[i].cb  = cb;
            s_subs[i].ctx = ctx;
            added = true;
            break;
        }
    }
    xSemaphoreGive(s_subs_mtx);
    return added;
}

void app_logd_unsubscribe(app_logd_subscriber_t cb, void *ctx)
{
    if (!cb || !s_subs_mtx) return;
    xSemaphoreTake(s_subs_mtx, portMAX_DELAY);
    for (int i = 0; i < LOGD_MAX_SUBSCRIBERS; ++i) {
        if (s_subs[i].cb == cb && s_subs[i].ctx == ctx) {
            s_subs[i].cb  = NULL;
            s_subs[i].ctx = NULL;
        }
    }
    xSemaphoreGive(s_subs_mtx);
}
