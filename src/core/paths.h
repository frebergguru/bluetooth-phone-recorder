/*
 * XDG directories used by the app. All returned strings are static.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_PATHS_H
#define BPR_PATHS_H

#include <stddef.h>

#define BPR_APP_DIR "bt-phone-recorder"

const char *paths_config_dir(void);  /* ~/.config/bt-phone-recorder */
const char *paths_state_dir(void);   /* ~/.local/state/bt-phone-recorder (logs, HCI traces) */
const char *paths_data_dir(void);    /* ~/.local/share/bt-phone-recorder (pairing keys) */
const char *paths_cache_dir(void);   /* ~/.cache/bt-phone-recorder (unpacked firmware) */

/* mkdir -p. Returns 0 on success. */
int paths_mkdirs(const char *path);

#endif
