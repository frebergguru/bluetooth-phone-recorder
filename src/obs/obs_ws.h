/*
 * Minimal obs-websocket (protocol v5) client: start/stop OBS recording and
 * add chapter markers, so OBS and this app record in step.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_OBS_WS_H
#define BPR_OBS_WS_H

#include <glib.h>

typedef struct obs_ws obs_ws_t;

typedef void (*obs_ws_status_cb)(gboolean connected, const char *message, gpointer ud);

obs_ws_t *obs_ws_new(obs_ws_status_cb cb, gpointer ud);
void obs_ws_free(obs_ws_t *o);
/* (Re)connect; does nothing useful if built without libsoup. */
void obs_ws_connect(obs_ws_t *o, const char *host, int port, const char *password);
void obs_ws_disconnect(obs_ws_t *o);
gboolean obs_ws_is_connected(obs_ws_t *o);

void obs_ws_start_record(obs_ws_t *o);
void obs_ws_stop_record(obs_ws_t *o);
void obs_ws_chapter(obs_ws_t *o, const char *name);

/* Exposed for tests: base64(sha256(base64(sha256(password + salt)) + challenge)) */
char *obs_ws_auth_string(const char *password, const char *salt, const char *challenge);

#endif
