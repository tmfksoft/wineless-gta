#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int g_log_level = LOG_INFO;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec log_t0;

void log_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &log_t0);
    const char *e = getenv("WINELESS_LOG");
    if (!e) return;
    if (!strcmp(e, "error")) g_log_level = LOG_ERROR;
    else if (!strcmp(e, "warn")) g_log_level = LOG_WARN;
    else if (!strcmp(e, "info")) g_log_level = LOG_INFO;
    else if (!strcmp(e, "debug")) g_log_level = LOG_DEBUG;
    else if (!strcmp(e, "trace")) g_log_level = LOG_TRACE;
}

static void vlog(int level, const char *fmt, va_list ap) {
    static const char *names[] = {"ERR", "WRN", "INF", "DBG", "TRC"};
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (t.tv_sec - log_t0.tv_sec) + (t.tv_nsec - log_t0.tv_nsec) / 1e9;
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[%9.4f %5ld %s] ", s, (long)syscall(SYS_gettid), names[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
}

void log_msg(int level, const char *fmt, ...) {
    if (level > g_log_level) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(LOG_ERROR, fmt, ap);
    va_end(ap);
    _exit(1);
}
