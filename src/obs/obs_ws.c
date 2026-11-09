/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "obs_ws.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "core/log.h"

static char *b64_sha256(const char *a, const char *b)
{
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(ck, (const guchar *)a, (gssize)strlen(a));
    g_checksum_update(ck, (const guchar *)b, (gssize)strlen(b));
    guint8 digest[32];
    gsize len = sizeof digest;
    g_checksum_get_digest(ck, digest, &len);
    g_checksum_free(ck);
    return g_base64_encode(digest, len);
}

char *obs_ws_auth_string(const char *password, const char *salt, const char *challenge)
{
    char *secret = b64_sha256(password, salt);
    char *auth = b64_sha256(secret, challenge);
    g_free(secret);
    return auth;
}

#if HAVE_OBS_WEBSOCKET

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

struct obs_ws {
    SoupSession *session;
    SoupWebsocketConnection *conn;
    GCancellable *cancel;
    char *password;
    gboolean identified;
    guint request_id;
    obs_ws_status_cb cb;
    gpointer ud;
};

static void status(obs_ws_t *o, gboolean connected, const char *msg)
{
    if (o->cb) o->cb(connected, msg, o->ud);
}

static void send_json(obs_ws_t *o, JsonBuilder *b)
{
    if (!o->conn || soup_websocket_connection_get_state(o->conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return;
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    char *text = json_generator_to_data(gen, NULL);
    soup_websocket_connection_send_text(o->conn, text);
    g_free(text);
    json_node_unref(root);
    g_object_unref(gen);
}

static void send_request(obs_ws_t *o, const char *type, const char *chapter_name)
{
    if (!o->identified) return;
    char id[32];
    snprintf(id, sizeof id, "bpr-%u", ++o->request_id);
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "op");
    json_builder_add_int_value(b, 6);
    json_builder_set_member_name(b, "d");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "requestType");
    json_builder_add_string_value(b, type);
    json_builder_set_member_name(b, "requestId");
    json_builder_add_string_value(b, id);
    if (chapter_name) {
        json_builder_set_member_name(b, "requestData");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "chapterName");
        json_builder_add_string_value(b, chapter_name);
        json_builder_end_object(b);
    }
    json_builder_end_object(b);
    json_builder_end_object(b);
    send_json(o, b);
    g_object_unref(b);
}

static void on_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer data)
{
    (void)conn;
    obs_ws_t *o = data;
    if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
    gsize len;
    const char *text = g_bytes_get_data(message, &len);
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, text, (gssize)len, NULL)) {
        g_object_unref(p);
        return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(p));
    gint64 op = json_object_get_int_member_with_default(root, "op", -1);
    JsonObject *d = json_object_has_member(root, "d") ? json_object_get_object_member(root, "d")
                                                      : NULL;
    if (op == 0 && d) { /* Hello */
        JsonBuilder *b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "op");
        json_builder_add_int_value(b, 1);
        json_builder_set_member_name(b, "d");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "rpcVersion");
        json_builder_add_int_value(b, 1);
        json_builder_set_member_name(b, "eventSubscriptions");
        json_builder_add_int_value(b, 0);
        if (json_object_has_member(d, "authentication")) {
            JsonObject *a = json_object_get_object_member(d, "authentication");
            const char *salt = json_object_get_string_member_with_default(a, "salt", "");
            const char *challenge = json_object_get_string_member_with_default(a, "challenge", "");
            char *auth = obs_ws_auth_string(o->password ? o->password : "", salt, challenge);
            json_builder_set_member_name(b, "authentication");
            json_builder_add_string_value(b, auth);
            g_free(auth);
        }
        json_builder_end_object(b);
        json_builder_end_object(b);
        send_json(o, b);
        g_object_unref(b);
    } else if (op == 2) { /* Identified */
        o->identified = TRUE;
        status(o, TRUE, NULL);
        LOGI("obs", "connected to OBS");
    } else if (op == 7 && d) { /* RequestResponse */
        JsonObject *st = json_object_get_object_member(d, "requestStatus");
        if (st && !json_object_get_boolean_member_with_default(st, "result", FALSE)) {
            LOGW("obs", "OBS request %s failed: %s",
                 json_object_get_string_member_with_default(d, "requestType", "?"),
                 json_object_get_string_member_with_default(st, "comment", "no comment"));
        }
    }
    g_object_unref(p);
}

static void on_closed(SoupWebsocketConnection *conn, gpointer data)
{
    obs_ws_t *o = data;
    gushort code = soup_websocket_connection_get_close_code(conn);
    o->identified = FALSE;
    g_clear_object(&o->conn);
    status(o, FALSE, code == 4009 ? "wrong OBS websocket password" : "disconnected from OBS");
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer data)
{
    obs_ws_t *o = data;
    GError *err = NULL;
    SoupWebsocketConnection *conn = soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
    if (!conn) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            status(o, FALSE, err ? err->message : "connection failed");
        g_clear_error(&err);
        return;
    }
    o->conn = conn;
    g_signal_connect(conn, "message", G_CALLBACK(on_message), o);
    g_signal_connect(conn, "closed", G_CALLBACK(on_closed), o);
}

obs_ws_t *obs_ws_new(obs_ws_status_cb cb, gpointer ud)
{
    obs_ws_t *o = g_new0(obs_ws_t, 1);
    o->session = soup_session_new();
    o->cb = cb;
    o->ud = ud;
    return o;
}

void obs_ws_disconnect(obs_ws_t *o)
{
    if (o->cancel) {
        g_cancellable_cancel(o->cancel);
        g_clear_object(&o->cancel);
    }
    if (o->conn) {
        g_signal_handlers_disconnect_by_data(o->conn, o);
        if (soup_websocket_connection_get_state(o->conn) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(o->conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
        g_clear_object(&o->conn);
    }
    o->identified = FALSE;
}

void obs_ws_connect(obs_ws_t *o, const char *host, int port, const char *password)
{
    obs_ws_disconnect(o);
    g_free(o->password);
    o->password = g_strdup(password);
    char *url = g_strdup_printf("ws://%s:%d", host && *host ? host : "localhost", port);
    SoupMessage *msg = soup_message_new("GET", url);
    g_free(url);
    if (!msg) {
        status(o, FALSE, "invalid OBS address");
        return;
    }
    const char *protocols[] = {"obswebsocket.json", NULL};
    o->cancel = g_cancellable_new();
    soup_session_websocket_connect_async(o->session, msg, NULL, (char **)protocols,
                                         G_PRIORITY_DEFAULT, o->cancel, on_connected, o);
    g_object_unref(msg);
}

void obs_ws_free(obs_ws_t *o)
{
    if (!o) return;
    obs_ws_disconnect(o);
    g_object_unref(o->session);
    g_free(o->password);
    g_free(o);
}

gboolean obs_ws_is_connected(obs_ws_t *o) { return o && o->identified; }
void obs_ws_start_record(obs_ws_t *o) { send_request(o, "StartRecord", NULL); }
void obs_ws_stop_record(obs_ws_t *o) { send_request(o, "StopRecord", NULL); }
void obs_ws_chapter(obs_ws_t *o, const char *name) { send_request(o, "CreateRecordChapter", name); }

#else /* built without libsoup / json-glib */

struct obs_ws {
    int unused;
};
obs_ws_t *obs_ws_new(obs_ws_status_cb cb, gpointer ud)
{
    (void)cb;
    (void)ud;
    return g_new0(obs_ws_t, 1);
}
void obs_ws_free(obs_ws_t *o) { g_free(o); }
void obs_ws_connect(obs_ws_t *o, const char *h, int p, const char *pw)
{
    (void)o; (void)h; (void)p; (void)pw;
}
void obs_ws_disconnect(obs_ws_t *o) { (void)o; }
gboolean obs_ws_is_connected(obs_ws_t *o) { (void)o; return FALSE; }
void obs_ws_start_record(obs_ws_t *o) { (void)o; }
void obs_ws_stop_record(obs_ws_t *o) { (void)o; }
void obs_ws_chapter(obs_ws_t *o, const char *n) { (void)o; (void)n; }

#endif
