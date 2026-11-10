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

void app_announce(const char *text, GtkAccessibleAnnouncementPriority prio)
{
    /* gtk_accessible_announce() crashes (NULL vfunc) when GTK runs without
     * its AT-SPI backend (GTK_A11Y=none, Broadway, some sandboxes). Only
     * announce when a real AT-SPI context exists. */
    if (!APP->window) return;
    GtkATContext *ctx = gtk_accessible_get_at_context(GTK_ACCESSIBLE(APP->window));
    if (!ctx) return;
    gboolean atspi = g_str_equal(G_OBJECT_TYPE_NAME(ctx), "GtkAtSpiContext");
    g_object_unref(ctx);
    if (atspi) gtk_accessible_announce(GTK_ACCESSIBLE(APP->window), text, prio);
}

/* Copy text that came from the phone (names, numbers, operator) into a
 * fixed buffer: invalid UTF-8 is repaired and a character cut in half by the
 * buffer size is dropped, so GTK labels and the key files (which refuse
 * invalid UTF-8 when read back) always get valid text. */
static void copy_text(char *dst, size_t len, const char *src)
{
    char *valid = g_utf8_make_valid(src ? src : "", -1);
    snprintf(dst, len, "%s", valid);
    g_free(valid);
    const char *end = NULL;
    if (!g_utf8_validate(dst, -1, &end)) dst[end - dst] = 0;
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
    APP->marker_count = 0;
    APP->rec_started_us = g_get_monotonic_time();
    if (s->obs_enabled && s->obs_sync_record) obs_ws_start_record(APP->obs);
    app_announce(_("Recording started"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
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
    if (APP->toasts) { /* not while shutting down after the window closed */
        char msg[1200];
        snprintf(msg, sizeof msg, _("Recording saved in %s"), dir);
        AdwToast *t = adw_toast_new(msg);
        adw_toast_set_button_label(t, _("Open folder"));
        adw_toast_set_action_name(t, "app.open-session");
        adw_toast_set_timeout(t, 10);
        adw_toast_overlay_add_toast(APP->toasts, t);
    }
    app_announce(_("Recording stopped and saved"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    app_update_call_ui();
}

void app_add_marker(void)
{
    if (!APP->recording) {
        app_toast(_("Markers can only be added while recording"));
        return;
    }
    char label[64];
    gint64 secs = (g_get_monotonic_time() - APP->rec_started_us) / G_USEC_PER_SEC;
    snprintf(label, sizeof label, _("Marker %d"), ++APP->marker_count);
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
    if (name && strlen(name) > sizeof clean - 1) app_toast(_("The name was shortened to fit Bluetooth"));
    snprintf(APP->settings.local_name, sizeof APP->settings.local_name, "%s", clean);
    settings_save(&APP->settings);
    if (APP->bt_started) bt_cmd_set_name(clean);
    char msg[160];
    snprintf(msg, sizeof msg, _("Bluetooth name changed to \"%s\""), clean);
    app_toast(msg);
    return TRUE;
}

void app_load_contacts(const char *addr)
{
    free(APP->contacts);
    APP->contacts = NULL;
    APP->n_contacts = 0;
    if (!addr || !*addr) return;
    char *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(bt_contacts_path(addr), &data, &len, NULL)) {
        APP->n_contacts = vcard_parse(data, len, &APP->contacts);
        g_free(data);
    }
}

const char *app_contact_name(const char *number)
{
    const contact_t *c = contacts_lookup(APP->contacts, APP->n_contacts, number);
    return c ? c->name : NULL;
}

/* Fill in a caller's name from the address book when the phone sent none. */
static void name_from_contacts(const char *number, char *name, size_t len)
{
    if (name[0] && strcmp(name, number) != 0) return;
    const char *n = app_contact_name(number);
    if (n) snprintf(name, len, "%s", n);
}

void app_dial(const char *number, const char *name, guint queue_id)
{
    if (!number || !*number) return;
    if (!APP->phone_connected) {
        app_toast(_("Connect a phone first"));
        return;
    }
    if (APP->call_state != CALL_IDLE) {
        app_toast(_("End the current call first"));
        return;
    }
    char dial[PHONE_DIAL_MAX + 1];
    int r = phone_dial_string(number, dial, sizeof dial);
    if (r == -2) {
        app_toast(_("This number is too long to dial"));
        return;
    }
    if (r <= 0) {
        app_toast(_("This number cannot be dialled: it contains letters or other characters a "
                    "phone cannot dial"));
        return;
    }
    copy_text(APP->dial_number, sizeof APP->dial_number, number);
    const char *n = name && *name ? name : app_contact_name(number);
    copy_text(APP->dial_name, sizeof APP->dial_name, n ? n : "");
    APP->dial_queue_id = queue_id;
    APP->dial_requested_us = g_get_monotonic_time();
    bt_cmd_dial(dial);
}

/* Forget what app_dial() asked for: the next call is not that one. */
static void dial_forget(void)
{
    APP->dial_queue_id = 0;
    APP->dial_number[0] = APP->dial_name[0] = 0;
}

/* Write the finished call into the call log. Called before the caller
 * fields are cleared. */
static void call_log_finish(void)
{
    if (!APP->log_pending) return;
    APP->log_pending = false;
    log_entry_t e = {0};
    e.time = APP->log_start_wall;
    e.dir = APP->log_dir;
    if (e.dir == CALL_DIR_IN && !APP->log_answered)
        e.dir = APP->log_declined ? CALL_DIR_DECLINED : CALL_DIR_MISSED;
    const char *num = APP->log_dir == CALL_DIR_OUT && APP->dial_number[0] ? APP->dial_number
                                                                         : APP->caller_number;
    const char *name = APP->log_dir == CALL_DIR_OUT && APP->dial_name[0] ? APP->dial_name
                                                                        : APP->caller_name;
    snprintf(e.number, sizeof e.number, "%s", num);
    snprintf(e.name, sizeof e.name, "%s", name);
    if (!e.name[0] && e.number[0]) {
        const char *c = app_contact_name(e.number);
        if (c) snprintf(e.name, sizeof e.name, "%s", c);
    }
    if (APP->log_answered)
        e.duration = (int)((g_get_monotonic_time() - APP->log_answer_us) / G_USEC_PER_SEC);
    call_log_add(&e);
    /* only the outgoing call we dialled from the queue marks it done */
    if (APP->dial_queue_id && APP->log_dir == CALL_DIR_OUT && APP->log_answered)
        queue_mark_done(APP->dial_queue_id);
    dial_forget();
    APP->log_declined = false;
    calls_view_refresh();
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
    /* call log: a new call starts (not a waiting second one) */
    if (old == CALL_IDLE && (st == CALL_INCOMING || st == CALL_OUTGOING || st == CALL_ACTIVE)) {
        /* A dial request the phone never acted on (it refused the number,
         * or the call was started on the phone instead) must not give this
         * call its number, name or queue entry. */
        if (st == CALL_INCOMING ||
            g_get_monotonic_time() - APP->dial_requested_us > 30 * G_USEC_PER_SEC)
            dial_forget();
        APP->log_pending = true;
        APP->log_dir = st == CALL_INCOMING ? CALL_DIR_IN : CALL_DIR_OUT;
        APP->log_answered = false;
        APP->log_start_wall = g_get_real_time() / G_USEC_PER_SEC;
        if (st == CALL_OUTGOING && !APP->dial_number[0] && APP->caller_number[0])
            snprintf(APP->dial_number, sizeof APP->dial_number, "%s", APP->caller_number);
    }
    if (st == CALL_ACTIVE && APP->log_pending && !APP->log_answered) {
        APP->log_answered = true;
        APP->log_answer_us = g_get_monotonic_time();
    }
    if (st == CALL_INCOMING) {
        char msg[256];
        const char *who = APP->caller_name[0] ? APP->caller_name
                          : APP->caller_number[0] ? APP->caller_number
                                                  : _("unknown number");
        snprintf(msg, sizeof msg, _("Incoming call from %s"), who);
        app_announce(msg, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
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
        g_application_withdraw_notification(G_APPLICATION(APP->gapp), "call");
        /* Back from a waiting second call: the same call goes on, so keep
         * its timer and do not announce or auto-start anything again. */
        if (!APP->call_timer_running) {
            APP->call_timer_running = true;
            APP->call_started_us = g_get_monotonic_time();
            app_announce(_("Call connected"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
            if (APP->settings.auto_record && !APP->recording) {
                app_record_start();
                APP->auto_recording = APP->recording;
            }
            /* Some phones keep the audio on the handset when the call was
             * answered there: bring it to the computer. */
            if (!APP->audio_on_computer) bt_cmd_audio_to_computer();
        }
    }
    if (st == CALL_IDLE) {
        APP->call_timer_running = false;
        g_application_withdraw_notification(G_APPLICATION(APP->gapp), "call");
        if (old == CALL_ACTIVE) app_announce(_("Call ended"), GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
        call_log_finish();
        atomic_store(&engine_mixer(APP->engine)->call_mute, false); /* next call starts with mics on */
        APP->caller_number[0] = APP->caller_name[0] = 0;
        APP->waiting_number[0] = APP->waiting_name[0] = 0;
        if (APP->auto_recording && APP->settings.auto_stop) app_record_stop();
    }
}

static void recompute_call_state(void)
{
    /* A waiting call stopped ringing: if the phone now holds a call, the
     * waiting one was answered, so it is the caller on the line now. */
    static int last_callsetup;
    if (last_callsetup == 1 && APP->ind_callsetup == 0 && APP->waiting_number[0]) {
        if (APP->ind_call == 1 && APP->ind_callheld) {
            snprintf(APP->caller_number, sizeof APP->caller_number, "%s", APP->waiting_number);
            snprintf(APP->caller_name, sizeof APP->caller_name, "%s", APP->waiting_name);
        }
        APP->waiting_number[0] = APP->waiting_name[0] = 0;
    }
    last_callsetup = APP->ind_callsetup;
    /* The active call ended (a waiting one may still ring). */
    if (APP->ind_call != 1) APP->call_timer_running = false;
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
            char name[sizeof APP->phone_name];
            copy_text(name, sizeof name, ev->text);
            settings_set_phone_name(ev->addr, name);
            if (strcmp(ev->addr, APP->phone_addr) == 0)
                snprintf(APP->phone_name, sizeof APP->phone_name, "%s", name);
            window_refresh_paired();
            app_update_phone_ui();
        }
        break;
    case BT_EV_PHONE_CONNECTED: {
        APP->phone_connected = true;
        snprintf(APP->phone_addr, sizeof APP->phone_addr, "%s", ev->addr);
        app_load_contacts(ev->addr); /* the saved copy, until a fresh one arrives */
        snprintf(APP->settings.last_phone, sizeof APP->settings.last_phone, "%s", ev->addr);
        settings_save(&APP->settings);
        char *name = settings_phone_name(ev->addr);
        copy_text(APP->phone_name, sizeof APP->phone_name, name ? name : ev->addr);
        g_free(name);
        APP->battery = APP->signal = -1;
        /* the engine ends pairing mode when a phone connects: show that */
        if (APP->pair_switch && gtk_switch_get_active(GTK_SWITCH(APP->pair_switch)))
            gtk_switch_set_active(GTK_SWITCH(APP->pair_switch), FALSE);
        char msg[200];
        snprintf(msg, sizeof msg, _("%s connected"), APP->phone_name);
        app_announce(msg, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
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
    case BT_EV_CALL_WAITING: {
        copy_text(APP->waiting_number, sizeof APP->waiting_number, ev->text);
        copy_text(APP->waiting_name, sizeof APP->waiting_name, ev->text2);
        name_from_contacts(APP->waiting_number, APP->waiting_name, sizeof APP->waiting_name);
        char msg[256];
        snprintf(msg, sizeof msg, _("Call waiting from %s"),
                 APP->waiting_name[0] ? APP->waiting_name
                 : APP->waiting_number[0] ? APP->waiting_number : _("unknown number"));
        app_announce(msg, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
        app_update_call_ui();
        break;
    }
    case BT_EV_CONTACTS:
        if (ev->number >= 0) {
            if (strcmp(ev->addr, APP->phone_addr) == 0) app_load_contacts(ev->addr);
            name_from_contacts(APP->caller_number, APP->caller_name, sizeof APP->caller_name);
            char msg[128];
            snprintf(msg, sizeof msg,
                     g_dngettext(NULL, "%d contact loaded from the phone",
                                 "%d contacts loaded from the phone", (gulong)ev->number),
                     ev->number);
            app_toast(msg);
        } else if (ev->state == BT_CONTACTS_REFUSED) {
            app_toast(_("The phone did not share its contacts. On Android, allow access to "
                        "contacts when the phone asks. On iPhone, turn on Sync Contacts under "
                        "Settings, Bluetooth, (i) next to Phone Recorder."));
        } else {
            app_toast(_("The contacts could not be downloaded from the phone."));
        }
        app_update_call_ui();
        break;
    case BT_EV_CALLER_ID:
        /* during a call, a new caller ID belongs to a waiting call */
        if (APP->ind_call == 1 && APP->ind_callsetup == 1 && APP->caller_number[0] &&
            strcmp(ev->text, APP->caller_number) != 0) {
            copy_text(APP->waiting_number, sizeof APP->waiting_number, ev->text);
            copy_text(APP->waiting_name, sizeof APP->waiting_name, ev->text2);
            name_from_contacts(APP->waiting_number, APP->waiting_name, sizeof APP->waiting_name);
            app_update_call_ui();
            break;
        }
        copy_text(APP->caller_number, sizeof APP->caller_number, ev->text);
        copy_text(APP->caller_name, sizeof APP->caller_name, ev->text2);
        name_from_contacts(APP->caller_number, APP->caller_name, sizeof APP->caller_name);
        app_update_call_ui();
        break;
    case BT_EV_CALL_STATUS:
        if (ev->text[0] && !APP->caller_number[0]) {
            copy_text(APP->caller_number, sizeof APP->caller_number, ev->text);
            name_from_contacts(APP->caller_number, APP->caller_name, sizeof APP->caller_name);
        }
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
        copy_text(APP->operator_name, sizeof APP->operator_name, ev->text);
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
    /* Events still queued while the app shuts down (the window and its
     * widgets are gone) are dropped. */
    if (APP && APP->window) handle_bt_event(ev);
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
    cfg.fetch_contacts = s->fetch_contacts;
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
    if (!APP->window) return G_SOURCE_REMOVE; /* window destroyed: shutting down */
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
    if (!APP->window) return G_SOURCE_REMOVE; /* window destroyed: shutting down */
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
    /* measure the folder, or its nearest existing parent (the folder itself
     * is only created when a recording starts) */
    char probe[1024];
    snprintf(probe, sizeof probe, "%s", APP->settings.record_dir);
    while (probe[0] && !g_file_test(probe, G_FILE_TEST_IS_DIR)) {
        char *slash = strrchr(probe, '/');
        if (!slash || slash == probe) {
            snprintf(probe, sizeof probe, "/");
            break;
        }
        *slash = 0;
    }
    int64_t free_b = recorder_free_bytes(probe);
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

    if (engine_monitor_failed(APP->engine))
        app_toast(_("The monitor output stopped working. Choose another output."));

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
    if (APP->call_state != CALL_INCOMING) return;
    if (APP->ind_call == 1)
        bt_cmd_hold_and_accept(); /* waiting call: hold the current call, take the new one */
    else
        bt_cmd_answer();
}
static void act_hangup(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->call_state == CALL_INCOMING && APP->ind_call == 1)
        bt_cmd_reject_waiting(); /* AT+CHUP here would end the call already in progress */
    else if (APP->call_state == CALL_INCOMING) {
        APP->log_declined = true;
        bt_cmd_reject();
    }
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
static void act_restart(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->recording) {
        app_toast(_("Stop the recording first, then restart"));
        return;
    }
    app_request_restart();
}

static void act_contacts(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_contacts(); }
static void act_call_log(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; dialog_call_log(); }
static void act_hold(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    if (APP->call_state != CALL_ACTIVE) return;
    /* AT+CHLD=2: hold the active call, or take back the held one */
    bt_cmd_hold_and_accept();
}
static void act_mic(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a; (void)p; (void)d;
    mixer_t *m = engine_mixer(APP->engine);
    bool off = !atomic_load(&m->call_mute);
    atomic_store(&m->call_mute, off);
    app_announce(off ? _("Microphones off for the caller") : _("Microphones on for the caller"),
                 GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    app_update_call_ui();
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
    {"restart", act_restart, NULL, NULL, NULL, {0}},
    {"contacts", act_contacts, NULL, NULL, NULL, {0}},
    {"call-log", act_call_log, NULL, NULL, NULL, {0}},
    {"hold", act_hold, NULL, NULL, NULL, {0}},
    {"mic-for-call", act_mic, NULL, NULL, NULL, {0}},
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
        {"app.contacts", {"<Control>k", NULL}},
        {"app.call-log", {"<Control>l", NULL}},
        {"app.hold", {"<Control><Shift>p", NULL}},
        {"app.mic-for-call", {"<Control><Shift>m", NULL}},
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
    if (s->monitor_enabled && engine_set_monitor(APP->engine, true, s->monitor_target) != 0)
        app_toast(_("The monitor output could not be opened. Choose another output."));
}

/* Debug aid for headless checks: BPR_SCREENSHOT=/path/file.png renders the
 * main window (and any dialog inside it) to a PNG a few seconds after start. */
gboolean app_save_screenshot(const char *path)
{
    GtkWidget *win = GTK_WIDGET(APP->window);
    /* The whole window, so its background and any open dialog are included.
     * Needs a backend that draws frames (Wayland/X11, not Broadway). */
    GtkWidget *src = win;
    int w = gtk_widget_get_width(src), h = gtk_widget_get_height(src);
    GdkPaintable *p = gtk_widget_paintable_new(src);
    GtkSnapshot *snap = gtk_snapshot_new();
    gdk_paintable_snapshot(p, snap, w, h);
    g_object_unref(p);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    gboolean ok = FALSE;
    if (!node || w <= 0 || h <= 0)
        LOGW("app", "screenshot: nothing to draw (%dx%d, mapped %d, child mapped %d)", w, h,
             gtk_widget_get_mapped(win), gtk_widget_get_mapped(src));
    if (node) {
        /* a private software renderer works on every backend (Broadway's
         * own renderer cannot produce textures) */
        GskRenderer *r = gsk_cairo_renderer_new();
        GError *err = NULL;
        gboolean realized = gsk_renderer_realize_for_display(r, gtk_widget_get_display(win), &err);
        if (!realized) {
            LOGW("app", "screenshot renderer: %s", err ? err->message : "?");
            g_clear_error(&err);
            realized = gsk_renderer_realize(r, NULL, NULL); /* offscreen fallback */
        }
        if (realized) {
            GdkTexture *tex = gsk_renderer_render_texture(r, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
            if (tex) {
                ok = gdk_texture_save_to_png(tex, path);
                g_object_unref(tex);
            }
            gsk_renderer_unrealize(r);
        }
        g_object_unref(r);
        gsk_render_node_unref(node);
    }
    return ok;
}

static gboolean take_screenshot(gpointer path)
{
    if (app_save_screenshot(path)) LOGI("app", "screenshot saved to %s", (const char *)path);
    g_free(path);
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
        bool have_engine = APP->engine != NULL;
        /* No engine means no strips: on_shutdown must not save an empty
         * mixer over the user's saved inputs. */
        engine_free(APP->engine);
        APP->engine = NULL;
        AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Audio system unavailable"), NULL));
        adw_alert_dialog_format_body(dlg, _("PipeWire could not be used: %s"),
                                     have_engine ? err : "out of memory");
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

    const char *docs = g_getenv("BPR_DOC_SHOTS");
    if (docs && *docs) {
        docshots_start(docs); /* demo data, no Bluetooth */
        return;
    }

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
    calldata_load();
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
    /* app_record_stop() above queued "StopRecord" for OBS: give the websocket
     * up to half a second to send it before the connection is closed */
    if (obs_ws_is_connected(APP->obs)) {
        gint64 until = g_get_monotonic_time() + 500 * 1000;
        while (g_get_monotonic_time() < until) {
            if (!g_main_context_iteration(NULL, FALSE)) g_usleep(10 * 1000);
        }
    }
    obs_ws_free(APP->obs);
    APP->obs = NULL;
    app_load_contacts(NULL); /* frees the list */
    calldata_free();
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
