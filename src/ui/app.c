/*
 * Application lifecycle, Bluetooth event handling, call logic, recording.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "app.h"

#include <glib-unix.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "config.h"
#include "core/log.h"
#include "core/paths.h"

app_t *APP;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

void app_toast(const char *text)
{
    if (!APP->toasts) return;
    AdwToast *t = adw_toast_new(text);
    adw_toast_set_timeout(t, 6);
    adw_toast_overlay_add_toast(APP->toasts, t);
}

static void announce(const char *text, GtkAccessibleAnnouncementPriority prio)
{
    if (APP->window) gtk_accessible_announce(GTK_ACCESSIBLE(APP->window), text, prio);
}

const char *app_adapter_status(void)
{
    return APP->bt_status;
}

void app_apply_text_scale(void)
{
    /* Scale relative to the desktop font size so the user's system setting
     * is respected and our own setting comes on top of it. */
    int base = 11;
    char *font = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &font, NULL);
    if (font) {
        const char *sp = strrchr(font, ' ');
        if (sp && atoi(sp + 1) > 0) base = atoi(sp + 1);
        g_free(font);
    }
    double pt = base * APP->settings.text_scale_percent / 100.0;
    /* CSS needs a '.' decimal point whatever the locale (nb_NO uses ',') */
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(num, sizeof num, "%.1f", pt);
    char css[128];
    snprintf(css, sizeof css, ".bpr-main { font-size: %spt; }", num);
    if (!APP->scale_css) {
        APP->scale_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(APP->scale_css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string(APP->scale_css, css);
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

void app_record_start(void)
{
    if (APP->recording) return;
    settings_t *s = &APP->settings;
    if (paths_mkdirs(s->record_dir) != 0) {
        char msg[1200];
        snprintf(msg, sizeof msg, _("Cannot create the recording folder %s"), s->record_dir);
        app_toast(msg);
        return;
    }
    char err[512];
    if (engine_record_start(APP->engine, s->record_dir, s->record_format, err, sizeof err) != 0) {
        char msg[700];
        snprintf(msg, sizeof msg, _("Recording could not start: %s"), err);
        app_toast(msg);
        return;
    }
    APP->recording = true;
    APP->rec_started_us = g_get_monotonic_time();
    if (s->obs_enabled && s->obs_sync_record) obs_ws_start_record(APP->obs);
    announce(_("Recording started"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    app_update_call_ui();
}

void app_record_stop(void)
{
    if (!APP->recording) return;
    engine_record_stop(APP->engine);
    APP->recording = false;
    APP->auto_recording = false;
    if (APP->settings.obs_enabled && APP->settings.obs_sync_record) obs_ws_stop_record(APP->obs);
    const char *dir = recorder_session_dir(engine_recorder(APP->engine));
    char msg[1200];
    snprintf(msg, sizeof msg, _("Recording saved in %s"), dir);
    AdwToast *t = adw_toast_new(msg);
    adw_toast_set_button_label(t, _("Open folder"));
    adw_toast_set_action_name(t, "app.open-session");
    adw_toast_set_timeout(t, 10);
    adw_toast_overlay_add_toast(APP->toasts, t);
    announce(_("Recording stopped and saved"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    app_update_call_ui();
}

void app_add_marker(void)
{
    if (!APP->recording) {
        app_toast(_("Markers can only be added while recording"));
        return;
    }
    static int n = 0;
    char label[64];
    gint64 secs = (g_get_monotonic_time() - APP->rec_started_us) / G_USEC_PER_SEC;
    snprintf(label, sizeof label, _("Marker %d"), ++n);
    recorder_add_marker(engine_recorder(APP->engine), label);
    if (APP->settings.obs_enabled && APP->settings.obs_chapters) obs_ws_chapter(APP->obs, label);
    char msg[128];
    snprintf(msg, sizeof msg, _("%s added at %02d:%02d:%02d"), label, (int)(secs / 3600),
             (int)(secs / 60 % 60), (int)(secs % 60));
    app_toast(msg);
}

gboolean app_set_bluetooth_name(const char *name)
{
    char clean[64];
    snprintf(clean, sizeof clean, "%s", name ? name : "");
    g_strstrip(clean);
    /* snprintf may have cut a multi-byte character in half */
    const char *end = NULL;
    if (!g_utf8_validate(clean, -1, &end)) clean[end - clean] = 0;
    if (!clean[0]) {
        app_toast(_("The Bluetooth name cannot be empty"));
        return FALSE;
    }
    if (strlen(name) > sizeof clean - 1) app_toast(_("The name was shortened to fit Bluetooth"));
    snprintf(APP->settings.local_name, sizeof APP->settings.local_name, "%s", clean);
    settings_save(&APP->settings);
    if (APP->bt_started) bt_cmd_set_name(clean);
    char msg[160];
    snprintf(msg, sizeof msg, _("Bluetooth name changed to \"%s\""), clean);
    app_toast(msg);
    return TRUE;
}

void app_request_restart(void)
{
    APP->restart_requested = true;
    g_application_quit(G_APPLICATION(APP->gapp));
}

void app_save_mixer_settings(void)
{
    settings_t *s = &APP->settings;
    int n = 0;
    for (int c = 1; c < MIXER_MAX_CHANNELS && n < SETTINGS_MAX_INPUTS; c++)
        if (APP->strips[c]) strip_save(APP->strips[c], &s->inputs[n++]);
    s->n_inputs = n;
    mixer_t *m = engine_mixer(APP->engine);
    s->caller_gain_db = atomic_load(&m->ch[ENGINE_CALLER_CH].gain_db);
    s->monitor_gain_db = atomic_load(&m->monitor_gain_db);
    s->call_gain_db = atomic_load(&m->call_gain_db);
    settings_save(s);
}

/* ------------------------------------------------------------------ */
/* Call state                                                          */
/* ------------------------------------------------------------------ */

static void set_call_state(call_state_t st)
{
    call_state_t old = APP->call_state;
    if (st == old) return;
    APP->call_state = st;
    if (st == CALL_INCOMING) {
        char msg[256];
        const char *who = APP->caller_name[0] ? APP->caller_name
                          : APP->caller_number[0] ? APP->caller_number
                                                  : _("unknown number");
        snprintf(msg, sizeof msg, _("Incoming call from %s"), who);
        announce(msg, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
        if (APP->window && !gtk_window_is_active(APP->window)) {
            GNotification *n = g_notification_new(_("Incoming call"));
            g_notification_set_body(n, who);
            g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_URGENT);
            g_notification_add_button(n, _("Answer"), "app.answer");
            g_application_send_notification(G_APPLICATION(APP->gapp), "call", n);
            g_object_unref(n);
        }
    }
    if (st == CALL_ACTIVE) {
        APP->call_started_us = g_get_monotonic_time();
        g_application_withdraw_notification(G_APPLICATION(APP->gapp), "call");
        announce(_("Call connected"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
        if (APP->settings.auto_record && !APP->recording) {
            app_record_start();
            APP->auto_recording = APP->recording;
        }
        /* Some phones keep the audio on the handset when the call was
         * answered there: bring it to the computer. */
        if (!APP->audio_on_computer) bt_cmd_audio_to_computer();
    }
    if (st == CALL_IDLE) {
        g_application_withdraw_notification(G_APPLICATION(APP->gapp), "call");
        if (old == CALL_ACTIVE) announce(_("Call ended"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
        APP->caller_number[0] = APP->caller_name[0] = 0;
        if (APP->auto_recording && APP->settings.auto_stop) app_record_stop();
    }
}

static void recompute_call_state(void)
{
    if (APP->ind_callsetup == 1)
        set_call_state(CALL_INCOMING);
    else if (APP->ind_callsetup == 2 || APP->ind_callsetup == 3)
        set_call_state(CALL_OUTGOING);
    else if (APP->ind_call == 1)
        set_call_state(CALL_ACTIVE);
    else
        set_call_state(CALL_IDLE);
}

/* ------------------------------------------------------------------ */
/* Bluetooth events (arrive on the BT thread, handled here on the UI)  */
/* ------------------------------------------------------------------ */

static void handle_bt_event(const bt_event_t *ev)
{
    switch (ev->type) {
    case BT_EV_STATE:
        APP->bt_state = (bt_state_t)ev->state;
        switch (APP->bt_state) {
        case BT_STATE_STARTING:
            snprintf(APP->bt_status, sizeof APP->bt_status, _("Starting %s"), ev->text);
            break;
        case BT_STATE_READY:
            snprintf(APP->bt_status, sizeof APP->bt_status, _("Ready, %s (%s)"), APP->adapter.name,
                     ev->text);
            break;
        case BT_STATE_FAILED: {
            snprintf(APP->bt_status, sizeof APP->bt_status, _("Adapter failed: %s"), ev->text);
            app_toast(APP->bt_status);
            break;
        }
        case BT_STATE_OFF:
        default:
            snprintf(APP->bt_status, sizeof APP->bt_status, _("Bluetooth off"));
            break;
        }
        app_update_phone_ui();
        break;
    case BT_EV_CONTROLLER_INFO:
        snprintf(APP->controller_report, sizeof APP->controller_report, "%s", ev->text);
        LOGI("bt", "controller:\n%s", ev->text);
        break;
    case BT_EV_PAIR_CONFIRM: dialog_pair_confirm(ev->addr, ev->number); break;
    case BT_EV_PIN_REQUEST: dialog_pin(ev->addr); break;
    case BT_EV_PAIRED:
        if (ev->number == 0) {
            app_toast(_("Phone paired"));
            gtk_switch_set_active(GTK_SWITCH(APP->pair_switch), FALSE);
        } else {
            app_toast(_("Pairing failed. Try again from the phone's Bluetooth settings."));
        }
        break;
    case BT_EV_REMOTE_NAME:
        if (ev->text[0]) {
            settings_set_phone_name(ev->addr, ev->text);
            if (strcmp(ev->addr, APP->phone_addr) == 0)
                snprintf(APP->phone_name, sizeof APP->phone_name, "%s", ev->text);
            window_refresh_paired();
            app_update_phone_ui();
        }
        break;
    case BT_EV_PHONE_CONNECTED: {
        APP->phone_connected = true;
        snprintf(APP->phone_addr, sizeof APP->phone_addr, "%s", ev->addr);
        snprintf(APP->settings.last_phone, sizeof APP->settings.last_phone, "%s", ev->addr);
        settings_save(&APP->settings);
        char *name = settings_phone_name(ev->addr);
        snprintf(APP->phone_name, sizeof APP->phone_name, "%s", name ? name : ev->addr);
        g_free(name);
        APP->battery = APP->signal = -1;
        char msg[200];
        snprintf(msg, sizeof msg, _("%s connected"), APP->phone_name);
        announce(msg, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
        app_update_phone_ui();
        window_refresh_paired();
        break;
    }
    case BT_EV_PHONE_DISCONNECTED: {
        char msg[200];
        snprintf(msg, sizeof msg, _("%s disconnected"),
                 APP->phone_name[0] ? APP->phone_name : _("Phone"));
        app_toast(msg);
        APP->phone_connected = false;
        APP->ind_call = APP->ind_callsetup = APP->ind_callheld = 0;
        APP->audio_on_computer = false;
        recompute_call_state();
        app_update_phone_ui();
        window_refresh_paired();
        break;
    }
    case BT_EV_INDICATOR:
        if (strcmp(ev->text, "call") == 0)
            APP->ind_call = ev->number;
        else if (strcmp(ev->text, "callsetup") == 0)
            APP->ind_callsetup = ev->number;
        else if (strcmp(ev->text, "callheld") == 0)
            APP->ind_callheld = ev->number;
        else if (strcmp(ev->text, "battchg") == 0)
            APP->battery = ev->number;
        else if (strcmp(ev->text, "signal") == 0)
            APP->signal = ev->number;
        recompute_call_state();
        app_update_call_ui();
        break;
    case BT_EV_RING:
        if (APP->call_state == CALL_IDLE) set_call_state(CALL_INCOMING);
        app_update_call_ui();
        break;
    case BT_EV_CALLER_ID:
        snprintf(APP->caller_number, sizeof APP->caller_number, "%s", ev->text);
        snprintf(APP->caller_name, sizeof APP->caller_name, "%s", ev->text2);
        app_update_call_ui();
        break;
    case BT_EV_CALL_STATUS:
        if (ev->text[0] && !APP->caller_number[0])
            snprintf(APP->caller_number, sizeof APP->caller_number, "%s", ev->text);
        app_update_call_ui();
        break;
    case BT_EV_AUDIO_CONNECTED:
        APP->audio_on_computer = true;
        snprintf(APP->codec_name, sizeof APP->codec_name, "%s", ev->text);
        APP->codec_rate = ev->number;
        app_update_call_ui();
        break;
    case BT_EV_AUDIO_DISCONNECTED:
        APP->audio_on_computer = false;
        app_update_call_ui();
        break;
    case BT_EV_AUDIO_SILENT:
        if (APP->adapter.transport == BT_TRANSPORT_USERCHAN)
            app_toast(_("No call audio arrives through the kernel driver mode. This adapter "
                        "may need the direct USB mode for call audio (Choose adapter)."));
        else
            app_toast(_("The call is connected, but no audio arrives from the adapter. Save "
                        "an adapter report and try another adapter or mode."));
        break;
    case BT_EV_OPERATOR:
        snprintf(APP->operator_name, sizeof APP->operator_name, "%s", ev->text);
        app_update_call_ui();
        break;
    case BT_EV_SPEAKER_GAIN: break;
    case BT_EV_PAIRED_LIST: {
        APP->n_paired = 0;
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", ev->text);
        char *save = NULL;
        for (char *l = strtok_r(tmp, "\n", &save); l && APP->n_paired < 16;
             l = strtok_r(NULL, "\n", &save))
            snprintf(APP->paired[APP->n_paired++], 18, "%s", l);
        window_refresh_paired();
        break;
    }
    case BT_EV_ERROR: app_toast(ev->text); break;
    }
}

static gboolean bt_event_idle(gpointer data)
{
    bt_event_t *ev = data;
    if (APP) handle_bt_event(ev);
    g_free(ev);
    return G_SOURCE_REMOVE;
}

static void bt_event_from_thread(const bt_event_t *ev, void *ud)
{
    (void)ud;
    g_idle_add(bt_event_idle, g_memdup2(ev, sizeof *ev));
}

static void start_bluetooth(void)
{
    if (APP->bt_started || !APP->have_adapter) return;
    settings_t *s = &APP->settings;
    bt_config_t cfg = {0};
    cfg.adapter = APP->adapter;
    cfg.local_name = s->local_name;
    cfg.firmware_dir = s->firmware_dir;
    cfg.secure_connections = s->secure_connections;
    cfg.allow_wideband = s->wideband;
    cfg.allow_super_wideband = s->super_wideband;
    cfg.disable_phone_nrec = s->phone_nrec_off;
    snprintf(cfg.auto_connect_addr, sizeof cfg.auto_connect_addr, "%s", s->last_phone);
    cfg.link = engine_phone_link(APP->engine);
    char trace[1024] = "";
    if (s->hci_trace || APP->cli_trace) {
        paths_mkdirs(paths_state_dir());
        GDateTime *now = g_date_time_new_now_local();
        char *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
        snprintf(trace, sizeof trace, "%s/bluetooth-trace-%s.pklg", paths_state_dir(), stamp);
        g_free(stamp);
        g_date_time_unref(now);
        cfg.trace_path = trace;
    }
    if (bt_engine_start(&cfg, bt_event_from_thread, NULL) == 0) APP->bt_started = true;
}

/* ------------------------------------------------------------------ */
/* Periodic UI updates                                                 */
/* ------------------------------------------------------------------ */

static gboolean meter_tick(gpointer data)
{
    (void)data;
    for (int c = 0; c < MIXER_MAX_CHANNELS; c++)
        if (APP->strips[c]) strip_update_meters(APP->strips[c]);
    mixer_t *m = engine_mixer(APP->engine);
    struct {
        GtkWidget *w;
        _Atomic float *v;
    } bus[] = {{APP->meter_full_l, &m->meter_full_l}, {APP->meter_full_r, &m->meter_full_r},
               {APP->meter_call, &m->meter_call}, {APP->meter_monitor, &m->meter_monitor}};
    for (size_t i = 0; i < G_N_ELEMENTS(bus); i++) {
        float peak = atomic_exchange(bus[i].v, 0.0f);
        if (!bus[i].w) continue;
        double db = peak > 1e-6f ? 20.0 * log10(peak) : -60.0;
        double v = (db + 60.0) / 60.0;
        double cur = gtk_level_bar_get_value(GTK_LEVEL_BAR(bus[i].w));
        /* fast rise, ~300 ms fall */
        v = v > cur ? v : cur - 0.035;
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bus[i].w), CLAMP(v, 0.0, 1.0));
    }
    return G_SOURCE_CONTINUE;
}

static void format_duration(char *buf, size_t len, gint64 us)
{
    gint64 s = us / G_USEC_PER_SEC;
    snprintf(buf, len, "%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

static gboolean slow_tick(gpointer data)
{
    (void)data;
    char buf[256];
    gint64 now = g_get_monotonic_time();
    if (APP->recording) {
        format_duration(buf, sizeof buf, now - APP->rec_started_us);
        gtk_label_set_text(GTK_LABEL(APP->record_label), buf);
    }
    if (APP->call_state == CALL_ACTIVE) {
        format_duration(buf, sizeof buf, now - APP->call_started_us);
        gtk_label_set_text(GTK_LABEL(APP->call_timer), buf);
    }

    /* disk space and estimated recording time */
    paths_mkdirs(APP->settings.record_dir);
    int64_t free_b = recorder_free_bytes(APP->settings.record_dir);
    int tracks = 3;
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++)
        if (APP->strips[c]) tracks++;
    double bytes_per_hour = 3600.0 * ENGINE_RATE * tracks *
                            (APP->settings.record_format == REC_FMT_WAV_FLOAT ? 4
                             : APP->settings.record_format == REC_FMT_WAV_24  ? 3
                             : APP->settings.record_format == REC_FMT_FLAC_24 ? 1.6
                                                                              : 0.02);
    if (free_b >= 0) {
        double hours = free_b / bytes_per_hour;
        snprintf(buf, sizeof buf, _("Free space: %.1f GB (about %.0f hours)"), free_b / 1e9,
                 hours);
        gtk_label_set_text(GTK_LABEL(APP->disk_label), buf);
        if (APP->recording && hours < 0.25) {
            static gint64 last_warn;
            if (now - last_warn > 60 * G_USEC_PER_SEC) {
                last_warn = now;
                app_toast(_("Warning: less than 15 minutes of recording space left"));
            }
        }
    }

    engine_stats_t st;
    engine_get_stats(APP->engine, &st);
    phone_link_t *link = engine_phone_link(APP->engine);
    uint64_t overruns = recorder_overruns(engine_recorder(APP->engine));
    uint64_t bad = atomic_load(&link->rx_bad_frames);
    uint64_t problems = st.input_dropouts + st.output_underruns + overruns + st.late_blocks;
    if (problems == 0 && bad == 0)
        snprintf(buf, sizeof buf, _("Audio: no dropouts"));
    else
        snprintf(buf, sizeof buf,
                 _("Audio: %llu dropouts, %llu damaged phone packets repaired"),
                 (unsigned long long)problems, (unsigned long long)bad);
    gtk_label_set_text(GTK_LABEL(APP->health_label), buf);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void act_record(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->recording)
        app_record_stop();
    else
        app_record_start();
}
static void act_marker(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; app_add_marker(); }
static void act_answer(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->call_state == CALL_INCOMING) bt_cmd_answer();
}
static void act_hangup(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->call_state == CALL_INCOMING)
        bt_cmd_reject();
    else if (APP->call_state != CALL_IDLE)
        bt_cmd_hangup();
}
static void act_audio(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->audio_on_computer)
        bt_cmd_audio_to_phone();
    else
        bt_cmd_audio_to_computer();
}
static void act_add_input(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    int n = 1;
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++)
        if (APP->strips[c]) n++;
    char label[64];
    snprintf(label, sizeof label, _("Mic %d"), n);
    mixer_view_add_input(NULL, label);
    app_save_mixer_settings();
}
static void act_prefs(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_preferences(); }
static void act_adapter(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_adapter_chooser(FALSE); }
static void act_report(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_adapter_report(); }
static void act_shortcuts(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_shortcuts(); }
static void act_about(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_about(); }
static void act_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->window) gtk_window_close(APP->window);
}
static void zoom(int delta)
{
    int v = delta == 0 ? 100 : APP->settings.text_scale_percent + delta;
    APP->settings.text_scale_percent = CLAMP(v, 100, 200);
    app_apply_text_scale();
    settings_save(&APP->settings);
}
static void act_zoom_in(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(10); }
static void act_zoom_out(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(-10); }
static void act_zoom_reset(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(0); }
static void act_send(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)d;
    strip_toggle_send(g_variant_get_int32(p));
}
static void act_mute(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)d;
    strip_toggle_mute(g_variant_get_int32(p));
}
static void act_open_session(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    const char *dir = recorder_session_dir(engine_recorder(APP->engine));
    if (!dir || !*dir) return;
    GFile *f = g_file_new_for_path(dir);
    GtkFileLauncher *l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, APP->window, NULL, NULL, NULL);
    g_object_unref(l);
    g_object_unref(f);
}

static const GActionEntry app_actions[] = {
    {"record", act_record, NULL, NULL, NULL, {0}},
    {"marker", act_marker, NULL, NULL, NULL, {0}},
    {"answer", act_answer, NULL, NULL, NULL, {0}},
    {"hangup", act_hangup, NULL, NULL, NULL, {0}},
    {"audio-switch", act_audio, NULL, NULL, NULL, {0}},
    {"add-input", act_add_input, NULL, NULL, NULL, {0}},
    {"preferences", act_prefs, NULL, NULL, NULL, {0}},
    {"adapter", act_adapter, NULL, NULL, NULL, {0}},
    {"report", act_report, NULL, NULL, NULL, {0}},
    {"shortcuts", act_shortcuts, NULL, NULL, NULL, {0}},
    {"about", act_about, NULL, NULL, NULL, {0}},
    {"quit", act_quit, NULL, NULL, NULL, {0}},
    {"zoom-in", act_zoom_in, NULL, NULL, NULL, {0}},
    {"zoom-out", act_zoom_out, NULL, NULL, NULL, {0}},
    {"zoom-reset", act_zoom_reset, NULL, NULL, NULL, {0}},
    {"send-to-call", act_send, "i", NULL, NULL, {0}},
    {"mute", act_mute, "i", NULL, NULL, {0}},
    {"open-session", act_open_session, NULL, NULL, NULL, {0}},
};

static void setup_accels(GtkApplication *app)
{
    struct {
        const char *action;
        const char *accels[3];
    } map[] = {
        {"app.record", {"<Control>r", NULL}},
        {"app.marker", {"<Control>m", "F9", NULL}},
        {"app.answer", {"<Control><Shift>a", NULL}},
        {"app.hangup", {"<Control><Shift>h", NULL}},
        {"app.audio-switch", {"<Control><Shift>t", NULL}},
        {"app.add-input", {"<Control>i", NULL}},
        {"app.preferences", {"<Control>comma", NULL}},
        {"app.shortcuts", {"<Control>question", "F1", NULL}},
        {"app.quit", {"<Control>q", NULL}},
        {"app.zoom-in", {"<Control>plus", "<Control>equal", NULL}},
        {"app.zoom-out", {"<Control>minus", NULL}},
        {"app.zoom-reset", {"<Control>0", NULL}},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        gtk_application_set_accels_for_action(app, map[i].action, map[i].accels);
    for (int n = 1; n <= 9; n++) {
        char detailed[64], accel[32];
        snprintf(detailed, sizeof detailed, "app.send-to-call(%d)", n);
        snprintf(accel, sizeof accel, "<Control>%d", n);
        const char *a1[] = {accel, NULL};
        gtk_application_set_accels_for_action(app, detailed, a1);
        snprintf(detailed, sizeof detailed, "app.mute(%d)", n);
        snprintf(accel, sizeof accel, "<Alt>%d", n);
        const char *a2[] = {accel, NULL};
        gtk_application_set_accels_for_action(app, detailed, a2);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* PipeWire reports device changes in bursts: refresh the lists once, 300 ms
 * after the first change of a burst. */
static atomic_bool devices_refresh_pending;

static gboolean devices_changed_idle(gpointer d)
{
    (void)d;
    atomic_store(&devices_refresh_pending, false);
    if (APP && APP->window) window_refresh_devices();
    return G_SOURCE_REMOVE;
}

static void devices_changed_from_pw(void *ud)
{
    (void)ud;
    if (!atomic_exchange(&devices_refresh_pending, true)) g_timeout_add(300, devices_changed_idle, NULL);
}

static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    LOGI("app", "signal received, shutting down cleanly");
    g_application_quit(G_APPLICATION(APP->gapp));
    return G_SOURCE_REMOVE;
}

static void apply_audio_settings(void)
{
    settings_t *s = &APP->settings;
    mixer_t *m = engine_mixer(APP->engine);
    atomic_store(&m->ch[ENGINE_CALLER_CH].gain_db, (float)s->caller_gain_db);
    atomic_store(&m->monitor_gain_db, (float)s->monitor_gain_db);
    atomic_store(&m->call_gain_db, (float)s->call_gain_db);
    atomic_store(&m->limit_full, s->limit_full);
    engine_set_echo_cancel(APP->engine, s->echo_cancel);
    if (s->monitor_enabled) engine_set_monitor(APP->engine, true, s->monitor_target);
}

/* Debug aid for headless checks: BPR_SCREENSHOT=/path/file.png renders the
 * main window (and any dialog inside it) to a PNG a few seconds after start. */
static gboolean take_screenshot(gpointer path)
{
    GtkWidget *win = GTK_WIDGET(APP->window);
    int w = gtk_widget_get_width(win), h = gtk_widget_get_height(win);
    GdkPaintable *p = gtk_widget_paintable_new(win);
    GtkSnapshot *snap = gtk_snapshot_new();
    gdk_paintable_snapshot(p, snap, w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    if (node) {
        GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(win));
        GdkTexture *tex = gsk_renderer_render_texture(r, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
        gdk_texture_save_to_png(tex, path);
        LOGI("app", "screenshot saved to %s", (const char *)path);
        g_object_unref(tex);
        gsk_render_node_unref(node);
    }
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

static void on_activate(GApplication *gapp, gpointer d)
{
    (void)d;
    if (APP->window) {
        gtk_window_present(APP->window);
        return;
    }
    app_apply_text_scale();

    engine_callbacks_t cb = {.devices_changed = devices_changed_from_pw, .ud = NULL};
    APP->engine = engine_new(&cb);
    char err[256];
    if (!APP->engine || engine_start(APP->engine, err, sizeof err) != 0) {
        AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Audio system unavailable"), NULL));
        adw_alert_dialog_format_body(dlg, _("PipeWire could not be used: %s"),
                                     APP->engine ? err : "out of memory");
        adw_alert_dialog_add_response(dlg, "close", _("Close"));
        GtkWidget *win = adw_application_window_new(GTK_APPLICATION(gapp));
        gtk_window_present(GTK_WINDOW(win));
        adw_dialog_present(ADW_DIALOG(dlg), win);
        return;
    }

    window_build();
    apply_audio_settings();
    APP->strips[ENGINE_CALLER_CH] = strip_new(ENGINE_CALLER_CH);
    gtk_box_append(GTK_BOX(APP->strip_box), strip_widget(APP->strips[ENGINE_CALLER_CH]));
    for (int i = 0; i < APP->settings.n_inputs; i++) {
        settings_input_t *in = &APP->settings.inputs[i];
        int c = mixer_view_add_input(in->target, in->label);
        if (c < 0) continue;
        mixer_channel_t *ch = &engine_mixer(APP->engine)->ch[c];
        atomic_store(&ch->gain_db, (float)in->gain_db);
        atomic_store(&ch->pan, (float)in->pan);
        atomic_store(&ch->send_to_call, in->send_to_call);
        atomic_store(&ch->mute, in->mute);
        atomic_store(&ch->hpf, in->hpf);
        atomic_store(&ch->gate, in->gate);
        atomic_store(&ch->gate_threshold_db, (float)in->gate_threshold_db);
        /* rebuild the strip so its controls show the stored values */
        gtk_box_remove(GTK_BOX(APP->strip_box), strip_widget(APP->strips[c]));
        strip_free(APP->strips[c]);
        APP->strips[c] = strip_new(c);
        gtk_box_append(GTK_BOX(APP->strip_box), strip_widget(APP->strips[c]));
    }
    gtk_window_present(APP->window);

    if (APP->settings.obs_enabled)
        obs_ws_connect(APP->obs, APP->settings.obs_host, APP->settings.obs_port,
                       APP->settings.obs_password);

    g_timeout_add(33, meter_tick, NULL);
    g_timeout_add(500, slow_tick, NULL);
    const char *shot = g_getenv("BPR_SCREENSHOT");
    if (shot && *shot) g_timeout_add(4000, take_screenshot, g_strdup(shot));

    const char *id = APP->cli_adapter[0] ? APP->cli_adapter : APP->settings.adapter_id;
    if (!id[0]) {
        snprintf(APP->bt_status, sizeof APP->bt_status, _("No Bluetooth adapter chosen"));
        app_update_phone_ui();
        dialog_adapter_chooser(TRUE);
    } else if (!bt_adapter_from_id(id, &APP->adapter)) {
        snprintf(APP->bt_status, sizeof APP->bt_status, _("Adapter %s not found"), id);
        app_update_phone_ui();
        app_toast(_("The chosen Bluetooth adapter was not found. Plug it in or choose another."));
        dialog_adapter_chooser(FALSE);
    } else {
        APP->have_adapter = true;
        start_bluetooth();
    }
    app_update_call_ui();
    app_update_phone_ui();
}

static void on_obs_status(gboolean connected, const char *message, gpointer ud)
{
    (void)ud;
    if (!connected && message && APP->settings.obs_enabled) {
        char msg[300];
        snprintf(msg, sizeof msg, _("OBS: %s"), message);
        app_toast(msg);
    } else if (connected) {
        app_toast(_("Connected to OBS"));
    }
}

static void on_startup(GApplication *gapp, gpointer d)
{
    (void)d;
    g_action_map_add_action_entries(G_ACTION_MAP(gapp), app_actions, G_N_ELEMENTS(app_actions),
                                    NULL);
    setup_accels(GTK_APPLICATION(gapp));
    APP->obs = obs_ws_new(on_obs_status, NULL);
}

static void on_shutdown(GApplication *gapp, gpointer d)
{
    (void)gapp;
    (void)d;
    if (APP->engine) {
        app_record_stop();
        app_save_mixer_settings();
    }
    bt_engine_stop(); /* powers the controller off, gives the adapter back */
    obs_ws_free(APP->obs);
    APP->obs = NULL;
    for (int c = 0; c < MIXER_MAX_CHANNELS; c++) {
        strip_free(APP->strips[c]);
        APP->strips[c] = NULL;
    }
    engine_free(APP->engine);
    APP->engine = NULL;
}

int app_run(int argc, char **argv, const char *cli_adapter, bool cli_trace);

int app_run(int argc, char **argv, const char *cli_adapter, bool cli_trace)
{
    APP = g_new0(app_t, 1);
    APP->battery = APP->signal = -1;
    settings_load(&APP->settings);
    if (cli_adapter) snprintf(APP->cli_adapter, sizeof APP->cli_adapter, "%s", cli_adapter);
    APP->cli_trace = cli_trace;

    APP->gapp = adw_application_new(BPR_APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(APP->gapp, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(APP->gapp, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(APP->gapp, "shutdown", G_CALLBACK(on_shutdown), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);

    int status = g_application_run(G_APPLICATION(APP->gapp), argc, argv);
    bool restart = APP->restart_requested;
    g_object_unref(APP->gapp);
    return restart ? 100 : status;
}
