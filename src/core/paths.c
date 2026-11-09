/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *home(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static const char *xdg(char *buf, size_t len, const char *env, const char *fallback)
{
    const char *base = getenv(env);
    if (base && *base == '/')
        snprintf(buf, len, "%s/" BPR_APP_DIR, base);
    else
        snprintf(buf, len, "%s/%s/" BPR_APP_DIR, home(), fallback);
    return buf;
}

const char *paths_config_dir(void)
{
    static char buf[PATH_MAX];
    return xdg(buf, sizeof buf, "XDG_CONFIG_HOME", ".config");
}

const char *paths_state_dir(void)
{
    static char buf[PATH_MAX];
    return xdg(buf, sizeof buf, "XDG_STATE_HOME", ".local/state");
}

const char *paths_data_dir(void)
{
    static char buf[PATH_MAX];
    return xdg(buf, sizeof buf, "XDG_DATA_HOME", ".local/share");
}

const char *paths_cache_dir(void)
{
    static char buf[PATH_MAX];
    return xdg(buf, sizeof buf, "XDG_CACHE_HOME", ".cache");
}

int paths_mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}
