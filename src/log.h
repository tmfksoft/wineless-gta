#pragma once
#include <stdarg.h>

enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_TRACE };

extern int g_log_level;

void log_init(void);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
_Noreturn void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (g_log_level >= LOG_DEBUG) log_msg(LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (g_log_level >= LOG_TRACE) log_msg(LOG_TRACE, __VA_ARGS__); } while (0)
