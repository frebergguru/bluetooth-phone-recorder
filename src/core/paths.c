/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "paths.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
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

/* The folders are worked out once (the environment does not change while
 * the app runs): the UI and the Bluetooth thread both ask for them, and
 * rewriting a shared buffer on every call would be a data race. */
static char config_dir[PATH_MAX], state_dir[PATH_MAX], data_dir[PATH_MAX], cache_dir[PATH_MAX];
static pthread_once_t dirs_once = PTHREAD_ONCE_INIT;

static void dirs_init(void)
{
    xdg(config_dir, sizeof config_dir, "XDG_CONFIG_HOME", ".config");
    xdg(state_dir, sizeof state_dir, "XDG_STATE_HOME", ".local/state");
    xdg(data_dir, sizeof data_dir, "XDG_DATA_HOME", ".local/share");
    xdg(cache_dir, sizeof cache_dir, "XDG_CACHE_HOME", ".cache");
}

const char *paths_config_dir(void)
{
    pthread_once(&dirs_once, dirs_init);
    return config_dir;
}

const char *paths_state_dir(void)
{
    pthread_once(&dirs_once, dirs_init);
    return state_dir;
}

const char *paths_data_dir(void)
{
    pthread_once(&dirs_once, dirs_init);
    return data_dir;
}

const char *paths_cache_dir(void)
{
    pthread_once(&dirs_once, dirs_init);
    return cache_dir;
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
