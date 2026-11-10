/*
 * Favorites, call queue and call log, kept in ~/.local/share/bt-phone-recorder
 * (files readable only by the user: they hold names and phone numbers).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_CALLDATA_H
#define BPR_CALLDATA_H

#include <glib.h>

typedef struct {
    char name[128];
    char number[64];
} fav_t;

typedef struct {
    guint id; /* stable while the app runs */
    char name[128];
    char number[64];
    char note[160];
    gboolean done; /* a call from the queue was answered */
} queue_item_t;

typedef enum { CALL_DIR_IN, CALL_DIR_OUT, CALL_DIR_MISSED, CALL_DIR_DECLINED } call_dir_t;

typedef struct {
    gint64 time; /* unix seconds, start of the call */
    call_dir_t dir;
    char number[64];
    char name[128];
    int duration; /* seconds talked, 0 if not answered */
} log_entry_t;

#define CALL_LOG_MAX 500

/* Each list is a GArray of the struct above, loaded at start. */
extern GArray *FAVS, *QUEUE, *CALL_LOG; /* CALL_LOG: newest first */

void calldata_load(void);
void calldata_free(void);

/* favorites (matched by number, ignoring spaces and dashes) */
gboolean fav_is(const char *number);
void fav_toggle(const char *name, const char *number); /* saves */

/* queue */
guint queue_add(const char *name, const char *number, const char *note); /* saves, returns id */
void queue_remove(guint id);
void queue_move(guint id, int delta); /* -1 up, +1 down */
void queue_mark_done(guint id);
void queue_clear_done(void);
queue_item_t *queue_find(guint id);

/* call log */
void call_log_add(const log_entry_t *e); /* saves, keeps CALL_LOG_MAX newest */
void call_log_clear(void);

#endif
