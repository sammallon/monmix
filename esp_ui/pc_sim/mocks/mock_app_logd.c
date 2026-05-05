// app_logd_emit -> stdout/stderr passthrough. The disk-rotation logic
// from the real implementation isn't needed in the sim.
#include "app_logd.h"

#include <stdio.h>
#include <stdarg.h>

void app_logd_init(void) {}

void app_logd_emit(const char *tag, char lvl, const char *fmt, ...) {
    FILE *out = (lvl == 'E' || lvl == 'W') ? stderr : stdout;
    fprintf(out, "%c (%s) ", lvl, tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

static bool s_trace = true;
void app_logd_set_trace(bool on) { s_trace = on; }
bool app_logd_get_trace(void)    { return s_trace; }
