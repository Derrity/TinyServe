#ifndef TS_LOG_H
#define TS_LOG_H

#define TS_LOG_ERROR 0
#define TS_LOG_WARN  1
#define TS_LOG_INFO  2

void ts_log_init(int level);
void ts_log(int level, const char *fmt, ...);

#define LOG_ERROR(...) ts_log(TS_LOG_ERROR, __VA_ARGS__)
#define LOG_WARN(...)  ts_log(TS_LOG_WARN, __VA_ARGS__)
#define LOG_INFO(...)  ts_log(TS_LOG_INFO, __VA_ARGS__)

#endif /* TS_LOG_H */
