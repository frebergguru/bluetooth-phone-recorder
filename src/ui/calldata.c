/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "calldata.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/paths.h"
#include "core/vcard.h"

GArray *FAVS, *QUEUE, *CALL_LOG;
static guint next_queue_id = 1;

static char *data_file(const char *name)
{
    return g_build_filename(paths_data_dir(), name, NULL);
}

static void save_keyfile(GKeyFile *kf, const char *name)
{
    paths_mkdirs(paths_data_dir());
    char *path = data_file(name);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    GError *err = NULL;
    /* private data: create with mode 0600 and replace atomically */
    if (!g_file_set_contents_full(path, data, (gssize)len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        LOGE("calls", "cannot save %s: %s", path, err->message);
        g_error_free(err);
    }
    g_free(data);
    g_free(path);
}

static GKeyFile *load_keyfile(const char *name)
{
    char *path = data_file(name);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    return kf;
}

static void get_str(GKeyFile *kf, const char *g, const char *k, char *dst, size_t len)
{
    char *v = g_key_file_get_string(kf, g, k, NULL);
    snprintf(dst, len, "%s", v ? v : "");
    g_free(v);
}

/* ---- favorites ---- */

static void favs_save(void)
{
    GKeyFile *kf = g_key_file_new();
    for (guint i = 0; i < FAVS->len; i++) {
        fav_t *f = &g_array_index(FAVS, fav_t, i);
        char g[32];
        snprintf(g, sizeof g, "favorite-%u", i + 1);
        g_key_file_set_string(kf, g, "name", f->name);
        g_key_file_set_string(kf, g, "number", f->number);
    }
    save_keyfile(kf, "favorites.ini");
    g_key_file_free(kf);
}

static int fav_index(const char *number)
{
    char want[64], have[64];
    phone_normalize(number, want, sizeof want);
    if (!want[0]) return -1;
    for (guint i = 0; i < FAVS->len; i++) {
        phone_normalize(g_array_index(FAVS, fav_t, i).number, have, sizeof have);
        if (strcmp(have, want) == 0) return (int)i;
    }
    return -1;
}

gboolean fav_is(const char *number)
{
    return number && fav_index(number) >= 0;
}

void fav_toggle(const char *name, const char *number)
{
    int i = fav_index(number);
    if (i >= 0) {
        g_array_remove_index(FAVS, (guint)i);
    } else {
        fav_t f = {0};
        snprintf(f.name, sizeof f.name, "%s", name && *name ? name : number);
        snprintf(f.number, sizeof f.number, "%s", number);
        g_array_append_val(FAVS, f);
    }
    favs_save();
}

/* ---- queue ---- */

static void queue_save(void)
{
    GKeyFile *kf = g_key_file_new();
    for (guint i = 0; i < QUEUE->len; i++) {
        queue_item_t *q = &g_array_index(QUEUE, queue_item_t, i);
        char g[32];
        snprintf(g, sizeof g, "item-%u", i + 1);
        g_key_file_set_string(kf, g, "name", q->name);
        g_key_file_set_string(kf, g, "number", q->number);
        g_key_file_set_string(kf, g, "note", q->note);
        g_key_file_set_boolean(kf, g, "done", q->done);
    }
    save_keyfile(kf, "call-queue.ini");
    g_key_file_free(kf);
}

guint queue_add(const char *name, const char *number, const char *note)
{
    queue_item_t q = {0};
    q.id = next_queue_id++;
    snprintf(q.name, sizeof q.name, "%s", name ? name : "");
    snprintf(q.number, sizeof q.number, "%s", number);
    snprintf(q.note, sizeof q.note, "%s", note ? note : "");
    g_array_append_val(QUEUE, q);
    queue_save();
    return q.id;
}

static int queue_index(guint id)
{
    for (guint i = 0; i < QUEUE->len; i++)
        if (g_array_index(QUEUE, queue_item_t, i).id == id) return (int)i;
    return -1;
}

queue_item_t *queue_find(guint id)
{
    int i = queue_index(id);
    return i < 0 ? NULL : &g_array_index(QUEUE, queue_item_t, i);
}

void queue_remove(guint id)
{
    int i = queue_index(id);
    if (i < 0) return;
    g_array_remove_index(QUEUE, (guint)i);
    queue_save();
}

void queue_move(guint id, int delta)
{
    int i = queue_index(id), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)QUEUE->len) return;
    queue_item_t tmp = g_array_index(QUEUE, queue_item_t, i);
    g_array_index(QUEUE, queue_item_t, i) = g_array_index(QUEUE, queue_item_t, j);
    g_array_index(QUEUE, queue_item_t, j) = tmp;
    queue_save();
}

void queue_mark_done(guint id)
{
    queue_item_t *q = queue_find(id);
    if (!q) return;
    q->done = TRUE;
    queue_save();
}

void queue_clear_done(void)
{
    for (guint i = QUEUE->len; i > 0; i--)
        if (g_array_index(QUEUE, queue_item_t, i - 1).done) g_array_remove_index(QUEUE, i - 1);
    queue_save();
}

/* ---- call log ---- */

static void log_save(void)
{
    GKeyFile *kf = g_key_file_new();
    for (guint i = 0; i < CALL_LOG->len; i++) {
        log_entry_t *e = &g_array_index(CALL_LOG, log_entry_t, i);
        char g[32];
        snprintf(g, sizeof g, "call-%u", i + 1);
        g_key_file_set_int64(kf, g, "time", e->time);
        g_key_file_set_integer(kf, g, "direction", (int)e->dir);
        g_key_file_set_string(kf, g, "number", e->number);
        g_key_file_set_string(kf, g, "name", e->name);
        g_key_file_set_integer(kf, g, "duration", e->duration);
    }
    save_keyfile(kf, "call-log.ini");
    g_key_file_free(kf);
}

void call_log_add(const log_entry_t *e)
{
    g_array_prepend_val(CALL_LOG, *e);
    if (CALL_LOG->len > CALL_LOG_MAX) g_array_set_size(CALL_LOG, CALL_LOG_MAX);
    log_save();
}

void call_log_clear(void)
{
    g_array_set_size(CALL_LOG, 0);
    log_save();
}

/* ---- load ---- */

void calldata_load(void)
{
    calldata_free();
    FAVS = g_array_new(FALSE, TRUE, sizeof(fav_t));
    QUEUE = g_array_new(FALSE, TRUE, sizeof(queue_item_t));
    CALL_LOG = g_array_new(FALSE, TRUE, sizeof(log_entry_t));

    GKeyFile *kf = load_keyfile("favorites.ini");
    gchar **groups = g_key_file_get_groups(kf, NULL);
    for (gchar **g = groups; g && *g; g++) {
        fav_t f = {0};
        get_str(kf, *g, "name", f.name, sizeof f.name);
        get_str(kf, *g, "number", f.number, sizeof f.number);
        if (f.number[0]) g_array_append_val(FAVS, f);
    }
    g_strfreev(groups);
    g_key_file_free(kf);

    kf = load_keyfile("call-queue.ini");
    groups = g_key_file_get_groups(kf, NULL);
    for (gchar **g = groups; g && *g; g++) {
        queue_item_t q = {0};
        q.id = next_queue_id++;
        get_str(kf, *g, "name", q.name, sizeof q.name);
        get_str(kf, *g, "number", q.number, sizeof q.number);
        get_str(kf, *g, "note", q.note, sizeof q.note);
        q.done = g_key_file_get_boolean(kf, *g, "done", NULL);
        if (q.number[0]) g_array_append_val(QUEUE, q);
    }
    g_strfreev(groups);
    g_key_file_free(kf);

    kf = load_keyfile("call-log.ini");
    groups = g_key_file_get_groups(kf, NULL);
    for (gchar **g = groups; g && *g; g++) {
        log_entry_t e = {0};
        e.time = g_key_file_get_int64(kf, *g, "time", NULL);
        e.dir = (call_dir_t)CLAMP(g_key_file_get_integer(kf, *g, "direction", NULL), 0, 3);
        get_str(kf, *g, "number", e.number, sizeof e.number);
        get_str(kf, *g, "name", e.name, sizeof e.name);
        e.duration = g_key_file_get_integer(kf, *g, "duration", NULL);
        if (e.time > 0) g_array_append_val(CALL_LOG, e);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
}

void calldata_free(void)
{
    if (FAVS) g_array_free(FAVS, TRUE);
    if (QUEUE) g_array_free(QUEUE, TRUE);
    if (CALL_LOG) g_array_free(CALL_LOG, TRUE);
    FAVS = QUEUE = CALL_LOG = NULL;
}
