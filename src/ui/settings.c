/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "settings.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/paths.h"

static char *settings_path(void)
{
    return g_build_filename(paths_config_dir(), "settings.ini", NULL);
}

static char *phones_path(void)
{
    return g_build_filename(paths_config_dir(), "phones.ini", NULL);
}

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->local_name, sizeof s->local_name, "Phone Recorder");
    s->wideband = TRUE;
    s->super_wideband = TRUE;
    s->secure_connections = TRUE;
    s->phone_nrec_off = TRUE;
    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    char *dir = g_build_filename(music ? music : g_get_home_dir(), "Podcast recordings", NULL);
    snprintf(s->record_dir, sizeof s->record_dir, "%s", dir);
    g_free(dir);
    s->record_format = REC_FMT_WAV_FLOAT;
    s->auto_record = FALSE;
    s->auto_stop = TRUE;
    s->monitor_enabled = TRUE;
    s->limit_full = TRUE;
    s->n_inputs = 1;
    snprintf(s->inputs[0].label, sizeof s->inputs[0].label, "Host");
    s->inputs[0].send_to_call = TRUE;
    s->inputs[0].hpf = TRUE;
    s->inputs[0].gate_threshold_db = -50.0;
    snprintf(s->obs_host, sizeof s->obs_host, "localhost");
    s->obs_port = 4455;
    s->obs_sync_record = TRUE;
    s->obs_chapters = TRUE;
    s->text_scale_percent = 100;
}

static void get_str(GKeyFile *kf, const char *g, const char *k, char *dst, size_t len)
{
    char *v = g_key_file_get_string(kf, g, k, NULL);
    if (v) {
        snprintf(dst, len, "%s", v);
        g_free(v);
    }
}

static void get_bool(GKeyFile *kf, const char *g, const char *k, gboolean *dst)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, g, k, &e);
    if (!e)
        *dst = v;
    else
        g_error_free(e);
}

static void get_double(GKeyFile *kf, const char *g, const char *k, double *dst)
{
    GError *e = NULL;
    double v = g_key_file_get_double(kf, g, k, &e);
    if (!e)
        *dst = v;
    else
        g_error_free(e);
}

static void get_int(GKeyFile *kf, const char *g, const char *k, int *dst)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, g, k, &e);
    if (!e)
        *dst = v;
    else
        g_error_free(e);
}

void settings_load(settings_t *s)
{
    settings_defaults(s);
    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_free(path);
        return;
    }
    get_str(kf, "bluetooth", "adapter", s->adapter_id, sizeof s->adapter_id);
    get_str(kf, "bluetooth", "name", s->local_name, sizeof s->local_name);
    get_bool(kf, "bluetooth", "wideband", &s->wideband);
    get_bool(kf, "bluetooth", "super-wideband", &s->super_wideband);
    get_bool(kf, "bluetooth", "secure-connections", &s->secure_connections);
    get_bool(kf, "bluetooth", "phone-nrec-off", &s->phone_nrec_off);
    get_str(kf, "bluetooth", "firmware-dir", s->firmware_dir, sizeof s->firmware_dir);
    get_bool(kf, "bluetooth", "hci-trace", &s->hci_trace);
    get_str(kf, "bluetooth", "last-phone", s->last_phone, sizeof s->last_phone);

    get_str(kf, "recording", "folder", s->record_dir, sizeof s->record_dir);
    int fmt = (int)s->record_format;
    get_int(kf, "recording", "format", &fmt);
    if (fmt >= REC_FMT_WAV_FLOAT && fmt <= REC_FMT_OPUS) s->record_format = (rec_format_t)fmt;
    get_bool(kf, "recording", "auto-record", &s->auto_record);
    get_bool(kf, "recording", "auto-stop", &s->auto_stop);

    get_bool(kf, "audio", "monitor", &s->monitor_enabled);
    get_str(kf, "audio", "monitor-target", s->monitor_target, sizeof s->monitor_target);
    get_double(kf, "audio", "monitor-gain", &s->monitor_gain_db);
    get_double(kf, "audio", "call-gain", &s->call_gain_db);
    get_double(kf, "audio", "caller-gain", &s->caller_gain_db);
    get_bool(kf, "audio", "echo-cancel", &s->echo_cancel);
    get_bool(kf, "audio", "limit-full", &s->limit_full);
    int n = s->n_inputs;
    get_int(kf, "audio", "inputs", &n);
    if (n < 0) n = 0;
    if (n > SETTINGS_MAX_INPUTS) n = SETTINGS_MAX_INPUTS;
    s->n_inputs = n;
    for (int i = 0; i < n; i++) {
        char g[32];
        snprintf(g, sizeof g, "input-%d", i + 1);
        settings_input_t *in = &s->inputs[i];
        if (!in->label[0]) snprintf(in->label, sizeof in->label, "Mic %d", i + 1);
        in->send_to_call = TRUE;
        in->hpf = TRUE;
        in->gate_threshold_db = -50.0;
        get_str(kf, g, "label", in->label, sizeof in->label);
        get_str(kf, g, "target", in->target, sizeof in->target);
        get_double(kf, g, "gain", &in->gain_db);
        get_double(kf, g, "pan", &in->pan);
        get_bool(kf, g, "send-to-call", &in->send_to_call);
        get_bool(kf, g, "mute", &in->mute);
        get_bool(kf, g, "low-cut", &in->hpf);
        get_bool(kf, g, "gate", &in->gate);
        get_double(kf, g, "gate-threshold", &in->gate_threshold_db);
    }

    get_bool(kf, "obs", "enabled", &s->obs_enabled);
    get_str(kf, "obs", "host", s->obs_host, sizeof s->obs_host);
    get_int(kf, "obs", "port", &s->obs_port);
    get_str(kf, "obs", "password", s->obs_password, sizeof s->obs_password);
    get_bool(kf, "obs", "sync-record", &s->obs_sync_record);
    get_bool(kf, "obs", "chapters", &s->obs_chapters);

    get_int(kf, "accessibility", "text-scale", &s->text_scale_percent);
    get_str(kf, "accessibility", "language", s->language, sizeof s->language);
    if (s->text_scale_percent < 100) s->text_scale_percent = 100;
    if (s->text_scale_percent > 200) s->text_scale_percent = 200;

    g_key_file_free(kf);
    g_free(path);
}

void settings_save(const settings_t *s)
{
    paths_mkdirs(paths_config_dir());
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "bluetooth", "adapter", s->adapter_id);
    g_key_file_set_string(kf, "bluetooth", "name", s->local_name);
    g_key_file_set_boolean(kf, "bluetooth", "wideband", s->wideband);
    g_key_file_set_boolean(kf, "bluetooth", "super-wideband", s->super_wideband);
    g_key_file_set_boolean(kf, "bluetooth", "secure-connections", s->secure_connections);
    g_key_file_set_boolean(kf, "bluetooth", "phone-nrec-off", s->phone_nrec_off);
    g_key_file_set_string(kf, "bluetooth", "firmware-dir", s->firmware_dir);
    g_key_file_set_boolean(kf, "bluetooth", "hci-trace", s->hci_trace);
    g_key_file_set_string(kf, "bluetooth", "last-phone", s->last_phone);

    g_key_file_set_string(kf, "recording", "folder", s->record_dir);
    g_key_file_set_integer(kf, "recording", "format", (int)s->record_format);
    g_key_file_set_boolean(kf, "recording", "auto-record", s->auto_record);
    g_key_file_set_boolean(kf, "recording", "auto-stop", s->auto_stop);

    g_key_file_set_boolean(kf, "audio", "monitor", s->monitor_enabled);
    g_key_file_set_string(kf, "audio", "monitor-target", s->monitor_target);
    g_key_file_set_double(kf, "audio", "monitor-gain", s->monitor_gain_db);
    g_key_file_set_double(kf, "audio", "call-gain", s->call_gain_db);
    g_key_file_set_double(kf, "audio", "caller-gain", s->caller_gain_db);
    g_key_file_set_boolean(kf, "audio", "echo-cancel", s->echo_cancel);
    g_key_file_set_boolean(kf, "audio", "limit-full", s->limit_full);
    g_key_file_set_integer(kf, "audio", "inputs", s->n_inputs);
    for (int i = 0; i < s->n_inputs; i++) {
        char g[32];
        snprintf(g, sizeof g, "input-%d", i + 1);
        const settings_input_t *in = &s->inputs[i];
        g_key_file_set_string(kf, g, "label", in->label);
        g_key_file_set_string(kf, g, "target", in->target);
        g_key_file_set_double(kf, g, "gain", in->gain_db);
        g_key_file_set_double(kf, g, "pan", in->pan);
        g_key_file_set_boolean(kf, g, "send-to-call", in->send_to_call);
        g_key_file_set_boolean(kf, g, "mute", in->mute);
        g_key_file_set_boolean(kf, g, "low-cut", in->hpf);
        g_key_file_set_boolean(kf, g, "gate", in->gate);
        g_key_file_set_double(kf, g, "gate-threshold", in->gate_threshold_db);
    }

    g_key_file_set_boolean(kf, "obs", "enabled", s->obs_enabled);
    g_key_file_set_string(kf, "obs", "host", s->obs_host);
    g_key_file_set_integer(kf, "obs", "port", s->obs_port);
    g_key_file_set_string(kf, "obs", "password", s->obs_password);
    g_key_file_set_boolean(kf, "obs", "sync-record", s->obs_sync_record);
    g_key_file_set_boolean(kf, "obs", "chapters", s->obs_chapters);

    g_key_file_set_integer(kf, "accessibility", "text-scale", s->text_scale_percent);
    g_key_file_set_string(kf, "accessibility", "language", s->language);

    char *path = settings_path();
    GError *err = NULL;
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    /* The file may hold the OBS password: create it as 0600 right away
     * (atomically replaced), instead of writing it world-readable and
     * tightening the mode afterwards. */
    if (!g_file_set_contents_full(path, data, (gssize)len, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  &err)) {
        LOGE("settings", "cannot save %s: %s", path, err->message);
        g_error_free(err);
    }
    g_free(data);
    g_free(path);
    g_key_file_free(kf);
}

char *settings_phone_name(const char *addr)
{
    char *path = phones_path();
    GKeyFile *kf = g_key_file_new();
    char *name = NULL;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        name = g_key_file_get_string(kf, "phones", addr, NULL);
    g_key_file_free(kf);
    g_free(path);
    return name;
}

void settings_set_phone_name(const char *addr, const char *name)
{
    paths_mkdirs(paths_config_dir());
    char *path = phones_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string(kf, "phones", addr, name);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}
