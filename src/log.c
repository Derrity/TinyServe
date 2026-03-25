#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static int g_log_level = TS_LOG_INFO;

void ts_log_init(int level) {
    if (level < TS_LOG_ERROR) level = TS_LOG_ERROR;
    if (level > TS_LOG_INFO) level = TS_LOG_INFO;
    g_log_level = level;
}

void ts_log(int level, const char *fmt, ...) {
    if (level > g_log_level) return;

    static const char *prefixes[] = { "ERROR", "WARN", "INFO" };
    const char *prefix = (level >= TS_LOG_ERROR && level <= TS_LOG_INFO)
                         ? prefixes[level] : "???";

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    fprintf(stderr, "[%02d:%02d:%02d] [%s] ",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, prefix);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
