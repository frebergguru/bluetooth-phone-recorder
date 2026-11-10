/*
 * Persistent settings (~/.config/bt-phone-recorder/settings.ini).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_SETTINGS_H
#define BPR_SETTINGS_H

#include <glib.h>

#include "rec/recorder.h"

#define SETTINGS_MAX_INPUTS 15

typedef struct {
    char label[128];
    char target[256];
    double gain_db;
    double pan;
    gboolean send_to_call;
    gboolean mute;
    gboolean hpf;
    gboolean gate;
    double gate_threshold_db;
} settings_input_t;

typedef struct {
    /* adapter */
    char adapter_id[320]; /* see bt_adapter_to_id(), "" = not chosen yet */
    char local_name[64];
    gboolean wideband, super_wideband, secure_connections, phone_nrec_off;
    gboolean fetch_contacts;
    char firmware_dir[512];
    gboolean hci_trace;
    char last_phone[18];

    /* recording */
    char record_dir[1024];
    rec_format_t record_format;
    gboolean auto_record, auto_stop;

    /* audio */
    gboolean monitor_enabled;
    char monitor_target[256];
    double monitor_gain_db;
    double call_gain_db;
    double caller_gain_db;
    gboolean echo_cancel;
    gboolean limit_full;
    settings_input_t inputs[SETTINGS_MAX_INPUTS];
    int n_inputs;

    /* OBS */
    gboolean obs_enabled;
    char obs_host[128];
    int obs_port;
    char obs_password[128];
    gboolean obs_sync_record;
    gboolean obs_chapters;

    /* accessibility and language */
    int text_scale_percent; /* 100..200 */
    char language[16];      /* "" = follow the system, "en", "nb" */
} settings_t;

void settings_defaults(settings_t *s);
void settings_load(settings_t *s);
void settings_save(const settings_t *s);

/* Remembered phone names, keyed by address. */
char *settings_phone_name(const char *addr); /* g_free() the result, NULL if unknown */
void settings_set_phone_name(const char *addr, const char *name);

#endif
