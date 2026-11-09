/*
 * Small thread-safe logger: stderr plus a log file in the state directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_LOG_H
#define BPR_LOG_H

typedef enum { LOG_LVL_DEBUG, LOG_LVL_INFO, LOG_LVL_WARN, LOG_LVL_ERROR } log_level_t;

void log_open(const char *path);
void log_close(void);
void log_set_verbose(int verbose);
void log_msg(log_level_t lvl, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGD(mod, ...) log_msg(LOG_LVL_DEBUG, mod, __VA_ARGS__)
#define LOGI(mod, ...) log_msg(LOG_LVL_INFO, mod, __VA_ARGS__)
#define LOGW(mod, ...) log_msg(LOG_LVL_WARN, mod, __VA_ARGS__)
#define LOGE(mod, ...) log_msg(LOG_LVL_ERROR, mod, __VA_ARGS__)

#endif
