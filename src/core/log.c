/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static int log_verbose;

void log_open(const char *path)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file) fclose(log_file);
    log_file = path ? fopen(path, "a") : NULL;
    pthread_mutex_unlock(&log_mutex);
}

void log_close(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file) fclose(log_file);
    log_file = NULL;
    pthread_mutex_unlock(&log_mutex);
}

void log_set_verbose(int verbose)
{
    log_verbose = verbose;
}

void log_msg(log_level_t lvl, const char *module, const char *fmt, ...)
{
    static const char *names[] = {"debug", "info", "warn", "error"};
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&log_mutex);
    if (lvl >= LOG_LVL_INFO || log_verbose)
        fprintf(stderr, "[%s] %s: %s\n", names[lvl], module, msg);
    if (log_file) {
        fprintf(log_file, "%s.%03ld [%s] %s: %s\n", stamp, ts.tv_nsec / 1000000L, names[lvl],
                module, msg);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}
