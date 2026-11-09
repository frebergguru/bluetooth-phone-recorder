/*
 * Main window: header bar with recording controls, phone panel on the left,
 * mixer on the right, status line at the bottom.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>

#include "app.h"

static void set_a11y_label(GtkWidget *w, const char *label)
{
    /* A button's visible text is linked as "labelled-by", which screen readers
     * prefer over an explicit label. Drop it so the fuller label is read. */
    gtk_accessible_reset_relation(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_RELATION_LABELLED_BY);
    gtk_accessible_update_property(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static GtkWidget *icon_text_button(const char *icon, const char *text, const char *action)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *content = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(content), text);
    adw_button_content_set_use_underline(ADW_BUTTON_CONTENT(content), TRUE);
    gtk_button_set_child(GTK_BUTTON(b), content);
    if (action) gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}

/* ------------------------------------------------------------------ */
/* Phone panel                                                         */
/* ------------------------------------------------------------------ */

static void on_pair_switch(GObject *sw, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    bt_cmd_set_discoverable(on);
    if (on) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 _("Pairing mode on. On the phone, open Bluetooth settings and choose \"%s\"."),
                 APP->settings.local_name);
        app_toast(msg);
    }
}

static void on_name_apply(AdwEntryRow *row, gpointer d)
{
    (void)d;
    /* show the name actually in use (cleaned up, or the old one if rejected) */
    app_set_bluetooth_name(gtk_editable_get_text(GTK_EDITABLE(row)));
    gtk_editable_set_text(GTK_EDITABLE(row), APP->settings.local_name);
}

static void on_connect_clicked(GtkButton *b, gpointer addr)
{
    (void)b;
    if (APP->phone_connected && strcmp(APP->phone_addr, addr) == 0)
        bt_cmd_disconnect();
    else
        bt_cmd_connect(addr);
}

static void forget_response(AdwAlertDialog *dlg, const char *response, gpointer addr)
{
    (void)dlg;
    if (strcmp(response, "forget") == 0) bt_cmd_forget(addr);
    g_free(addr);
}

static void on_forget_clicked(GtkButton *b, gpointer addr)
{
    (void)b;
    char *name = settings_phone_name(addr);
    AdwDialog *dlg = adw_alert_dialog_new(_("Forget this phone?"), NULL);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dlg),
                                 _("%s will have to be paired again before it can be used."),
                                 name ? name : (const char *)addr);
    g_free(name);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "forget",
                                   _("_Forget"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "forget",
                                             ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dlg, "response", G_CALLBACK(forget_response), g_strdup(addr));
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

void window_refresh_paired(void)
{
    if (!APP->paired_group) return;
    for (int i = 0; i < 16; i++) {
        if (APP->paired_rows[i]) {
            adw_preferences_group_remove(ADW_PREFERENCES_GROUP(APP->paired_group),
                                         APP->paired_rows[i]);
            APP->paired_rows[i] = NULL;
        }
    }
    for (int i = 0; i < APP->n_paired; i++) {
        const char *addr = APP->paired[i];
        char *name = settings_phone_name(addr);
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name ? name : addr);
        bool connected = APP->phone_connected && strcmp(APP->phone_addr, addr) == 0;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    connected ? _("Connected") : _("Not connected"));
        /* addr strings live in APP->paired until the next refresh, which also
         * destroys these buttons, so passing them directly is safe */
        GtkWidget *conn = gtk_button_new_with_label(connected ? _("Disconnect") : _("Connect"));
        gtk_widget_set_valign(conn, GTK_ALIGN_CENTER);
        char a11y[256];
        snprintf(a11y, sizeof a11y, connected ? _("Disconnect %s") : _("Connect %s"),
                 name ? name : addr);
        set_a11y_label(conn, a11y);
        g_signal_connect(conn, "clicked", G_CALLBACK(on_connect_clicked), (gpointer)addr);
        GtkWidget *forget = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_add_css_class(forget, "flat");
        gtk_widget_set_valign(forget, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(forget, _("Forget this phone"));
        snprintf(a11y, sizeof a11y, _("Forget %s"), name ? name : addr);
        set_a11y_label(forget, a11y);
        g_signal_connect(forget, "clicked", G_CALLBACK(on_forget_clicked), (gpointer)addr);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), conn);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), forget);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->paired_group), row);
        APP->paired_rows[i] = row;
        g_free(name);
    }
    if (APP->n_paired == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("No phones paired yet"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    _("Turn on pairing mode, then pair from the phone."));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->paired_group), row);
        APP->paired_rows[0] = row;
    }
}

static void on_dial(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    const char *num = gtk_editable_get_text(GTK_EDITABLE(APP->dial_entry));
    if (!num || !*num) {
        app_toast(_("Type a number to call"));
        return;
    }
    bt_cmd_dial(num);
}

static void on_redial(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    bt_cmd_redial();
}

static void on_key(GtkButton *b, gpointer d)
{
    (void)d;
    const char *k = gtk_button_get_label(b);
    if (APP->call_state == CALL_ACTIVE) {
        bt_cmd_dtmf(k[0]); /* tones during a call */
    } else {
        GtkEditable *e = GTK_EDITABLE(APP->dial_entry);
        int pos = -1;
        gtk_editable_insert_text(e, k, 1, &pos);
    }
}

static GtkWidget *build_keypad(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    const char *keys = "123456789*0#";
    for (int i = 0; i < 12; i++) {
        char k[2] = {keys[i], 0};
        GtkWidget *b = gtk_button_new_with_label(k);
        g_signal_connect(b, "clicked", G_CALLBACK(on_key), NULL);
        gtk_grid_attach(GTK_GRID(grid), b, i % 3, i / 3, 1, 1);
    }
    return grid;
}

static GtkWidget *build_phone_panel(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    /* Call */
    GtkWidget *call = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(call), _("Call"));
    GtkWidget *call_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(call_box, "card");
    gtk_widget_set_margin_bottom(call_box, 0);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(inner, 12);
    gtk_widget_set_margin_end(inner, 12);
    gtk_widget_set_margin_top(inner, 12);
    gtk_widget_set_margin_bottom(inner, 12);
    gtk_box_append(GTK_BOX(call_box), inner);

    APP->call_status = gtk_label_new(_("No call"));
    gtk_widget_add_css_class(APP->call_status, "heading");
    gtk_label_set_xalign(GTK_LABEL(APP->call_status), 0);
    gtk_accessible_update_property(GTK_ACCESSIBLE(APP->call_status),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Call status"), -1);
    APP->caller_label = gtk_label_new("");
    gtk_widget_add_css_class(APP->caller_label, "title-2");
    gtk_label_set_xalign(GTK_LABEL(APP->caller_label), 0);
    gtk_label_set_wrap(GTK_LABEL(APP->caller_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(APP->caller_label), TRUE);
    APP->call_timer = gtk_label_new("");
    gtk_widget_add_css_class(APP->call_timer, "numeric");
    gtk_label_set_xalign(GTK_LABEL(APP->call_timer), 0);
    APP->codec_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(APP->codec_label), 0);
    gtk_label_set_wrap(GTK_LABEL(APP->codec_label), TRUE);
    gtk_widget_add_css_class(APP->codec_label, "dim-label");
    gtk_box_append(GTK_BOX(inner), APP->call_status);
    gtk_box_append(GTK_BOX(inner), APP->caller_label);
    gtk_box_append(GTK_BOX(inner), APP->call_timer);
    gtk_box_append(GTK_BOX(inner), APP->codec_label);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(btns), TRUE);
    APP->answer_btn = icon_text_button("call-start-symbolic", _("_Answer"), "app.answer");
    gtk_widget_add_css_class(APP->answer_btn, "suggested-action");
    gtk_widget_set_tooltip_text(APP->answer_btn, _("Answer the call (Ctrl+Shift+A)"));
    APP->reject_btn = icon_text_button("call-stop-symbolic", _("_Decline"), "app.hangup");
    gtk_widget_add_css_class(APP->reject_btn, "destructive-action");
    gtk_widget_set_tooltip_text(APP->reject_btn, _("Decline the call (Ctrl+Shift+H)"));
    APP->hangup_btn = icon_text_button("call-stop-symbolic", _("_Hang up"), "app.hangup");
    gtk_widget_add_css_class(APP->hangup_btn, "destructive-action");
    gtk_widget_set_tooltip_text(APP->hangup_btn, _("End the call (Ctrl+Shift+H)"));
    gtk_box_append(GTK_BOX(btns), APP->answer_btn);
    gtk_box_append(GTK_BOX(btns), APP->reject_btn);
    gtk_box_append(GTK_BOX(btns), APP->hangup_btn);
    gtk_box_append(GTK_BOX(inner), btns);

    APP->audio_btn = icon_text_button("audio-headphones-symbolic", _("Move call audio to the _computer"),
                                      "app.audio-switch");
    gtk_widget_set_tooltip_text(APP->audio_btn,
                                _("Choose whether the call audio goes through this app or the "
                                  "phone itself (Ctrl+Shift+T)"));
    gtk_box_append(GTK_BOX(inner), APP->audio_btn);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(call), call_box);
    gtk_box_append(GTK_BOX(box), call);

    /* Dial */
    GtkWidget *dial = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dial), _("Make a call"));
    GtkWidget *dial_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    APP->dial_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(APP->dial_entry), GTK_INPUT_PURPOSE_PHONE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(APP->dial_entry), _("Phone number"));
    gtk_widget_set_hexpand(APP->dial_entry, TRUE);
    set_a11y_label(APP->dial_entry, _("Phone number to call"));
    g_signal_connect(APP->dial_entry, "activate", G_CALLBACK(on_dial), NULL);
    APP->dial_btn = gtk_button_new_with_mnemonic(_("_Call"));
    gtk_widget_add_css_class(APP->dial_btn, "suggested-action");
    g_signal_connect(APP->dial_btn, "clicked", G_CALLBACK(on_dial), NULL);
    APP->redial_btn = gtk_button_new_with_mnemonic(_("_Redial"));
    gtk_widget_set_tooltip_text(APP->redial_btn, _("Call the last number again"));
    g_signal_connect(APP->redial_btn, "clicked", G_CALLBACK(on_redial), NULL);
    gtk_box_append(GTK_BOX(dial_row), APP->dial_entry);
    gtk_box_append(GTK_BOX(dial_row), APP->dial_btn);
    gtk_box_append(GTK_BOX(dial_row), APP->redial_btn);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(dial), dial_row);
    GtkWidget *exp = gtk_expander_new_with_mnemonic(_("_Keypad (also sends tones during a call)"));
    APP->keypad = build_keypad();
    gtk_widget_set_margin_top(APP->keypad, 6);
    gtk_expander_set_child(GTK_EXPANDER(exp), APP->keypad);
    gtk_widget_set_margin_top(exp, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(dial), exp);
    gtk_box_append(GTK_BOX(box), dial);

    /* Phone connection */
    GtkWidget *phone = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(phone), _("Phone"));
    APP->phone_status_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(APP->phone_status_row), _("No phone connected"));
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(APP->phone_status_row), 3);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(phone), APP->phone_status_row);
    GtkWidget *name_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name_row), _("Bluetooth name"));
    gtk_editable_set_text(GTK_EDITABLE(name_row), APP->settings.local_name);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(name_row), 63);
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(name_row), TRUE);
    gtk_widget_set_tooltip_text(name_row, _("The name phones see when pairing. Phones that are "
                                            "already paired may show the old name until they "
                                            "connect again."));
    g_signal_connect(name_row, "apply", G_CALLBACK(on_name_apply), NULL);
    APP->name_row = name_row;
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(phone), name_row);
    GtkWidget *pair_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pair_row), _("Pairing mode"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(pair_row),
                                _("Lets a new phone find this computer"));
    APP->pair_switch = gtk_switch_new();
    gtk_widget_set_valign(APP->pair_switch, GTK_ALIGN_CENTER);
    set_a11y_label(APP->pair_switch, _("Pairing mode"));
    g_signal_connect(APP->pair_switch, "notify::active", G_CALLBACK(on_pair_switch), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(pair_row), APP->pair_switch);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(pair_row), APP->pair_switch);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(phone), pair_row);
    gtk_box_append(GTK_BOX(box), phone);

    APP->paired_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(APP->paired_group), _("Paired phones"));
    gtk_box_append(GTK_BOX(box), APP->paired_group);
    window_refresh_paired();

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_widget_set_size_request(scroll, 340, -1);
    return scroll;
}

void app_update_phone_ui(void)
{
    if (!APP->window) return;
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(APP->title), APP->bt_status);
    char title[256], sub[512];
    if (APP->phone_connected) {
        snprintf(title, sizeof title, _("%s connected"), APP->phone_name);
        char bat[64] = "", sig[64] = "";
        if (APP->battery >= 0) snprintf(bat, sizeof bat, _("Battery %d of 5"), APP->battery);
        if (APP->signal >= 0) snprintf(sig, sizeof sig, _("signal %d of 5"), APP->signal);
        snprintf(sub, sizeof sub, "%s%s%s%s%s", bat, bat[0] && sig[0] ? ", " : "", sig,
                 APP->operator_name[0] ? ", " : "", APP->operator_name);
    } else {
        snprintf(title, sizeof title, "%s", _("No phone connected"));
        snprintf(sub, sizeof sub, "%s", APP->bt_status);
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(APP->phone_status_row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(APP->phone_status_row), sub);
    app_update_call_ui();
}

void app_update_call_ui(void)
{
    if (!APP->window) return;
    const char *status;
    switch (APP->call_state) {
    case CALL_INCOMING: status = _("Incoming call"); break;
    case CALL_OUTGOING: status = _("Calling..."); break;
    case CALL_ACTIVE: status = APP->ind_callheld ? _("In call (one call on hold)") : _("In call"); break;
    case CALL_IDLE:
    default: status = APP->phone_connected ? _("No call") : _("Connect a phone to make calls"); break;
    }
    gtk_label_set_text(GTK_LABEL(APP->call_status), status);

    char who[256] = "";
    if (APP->caller_name[0] && APP->caller_number[0])
        snprintf(who, sizeof who, "%s\n%s", APP->caller_name, APP->caller_number);
    else
        snprintf(who, sizeof who, "%s", APP->caller_name[0] ? APP->caller_name : APP->caller_number);
    gtk_label_set_text(GTK_LABEL(APP->caller_label), who);
    gtk_widget_set_visible(APP->caller_label, who[0] != 0);
    gtk_widget_set_visible(APP->call_timer, APP->call_state == CALL_ACTIVE);
    if (APP->call_state != CALL_ACTIVE) gtk_label_set_text(GTK_LABEL(APP->call_timer), "00:00:00");

    char codec[200] = "";
    if (APP->audio_on_computer) {
        const char *quality = APP->codec_rate >= 32000   ? _("super wideband")
                              : APP->codec_rate >= 16000 ? _("HD voice")
                                                         : _("narrowband");
        snprintf(codec, sizeof codec, _("Call audio on this computer: %s, %s (%d kHz)"),
                 APP->codec_name, quality, APP->codec_rate / 1000);
    } else if (APP->call_state != CALL_IDLE) {
        snprintf(codec, sizeof codec, "%s", _("Call audio is on the phone"));
    }
    gtk_label_set_text(GTK_LABEL(APP->codec_label), codec);
    gtk_widget_set_visible(APP->codec_label, codec[0] != 0);

    gtk_widget_set_visible(APP->answer_btn, APP->call_state == CALL_INCOMING);
    gtk_widget_set_visible(APP->reject_btn, APP->call_state == CALL_INCOMING);
    gtk_widget_set_visible(APP->hangup_btn,
                           APP->call_state == CALL_ACTIVE || APP->call_state == CALL_OUTGOING);
    gtk_widget_set_visible(APP->audio_btn, APP->call_state == CALL_ACTIVE);
    AdwButtonContent *ac = ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(APP->audio_btn)));
    adw_button_content_set_label(ac, APP->audio_on_computer ? _("Move call audio to the _phone")
                                                            : _("Move call audio to the _computer"));
    gboolean can_dial = APP->phone_connected && APP->call_state == CALL_IDLE;
    gtk_widget_set_sensitive(APP->dial_btn, can_dial);
    gtk_widget_set_sensitive(APP->redial_btn, can_dial);

    /* recording controls */
    AdwButtonContent *rc = ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(APP->record_btn)));
    adw_button_content_set_label(rc, APP->recording ? _("_Stop") : _("_Record"));
    adw_button_content_set_icon_name(rc, APP->recording ? "media-playback-stop-symbolic"
                                                        : "media-record-symbolic");
    if (APP->recording) {
        gtk_widget_remove_css_class(APP->record_btn, "suggested-action");
        gtk_widget_add_css_class(APP->record_btn, "destructive-action");
    } else {
        gtk_widget_remove_css_class(APP->record_btn, "destructive-action");
        gtk_widget_add_css_class(APP->record_btn, "suggested-action");
        gtk_label_set_text(GTK_LABEL(APP->record_label), _("Not recording"));
    }
    gtk_widget_set_tooltip_text(APP->record_btn, APP->recording ? _("Stop recording (Ctrl+R)")
                                                                : _("Start recording (Ctrl+R)"));
    gtk_widget_set_sensitive(APP->marker_btn, APP->recording);
    if (APP->recording) {
        char s[1200];
        snprintf(s, sizeof s, _("Recording to %s"),
                 recorder_session_dir(engine_recorder(APP->engine)));
        gtk_label_set_text(GTK_LABEL(APP->session_label), s);
    } else {
        gtk_label_set_text(GTK_LABEL(APP->session_label), "");
    }
}

void window_refresh_devices(void)
{
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++)
        if (APP->strips[c]) strip_refresh_devices(APP->strips[c]);
    if (APP->monitor_dropdown) {
        /* rebuild the monitor output list */
        engine_device_t devs[64];
        int n = engine_list_devices(APP->engine, false, devs, 64);
        GtkStringList *list = gtk_string_list_new(NULL);
        gtk_string_list_append(list, _("System default output"));
        guint sel = 0;
        for (int i = 0; i < n; i++) {
            gtk_string_list_append(list, devs[i].description);
            if (strcmp(devs[i].name, APP->settings.monitor_target) == 0) sel = (guint)i + 1;
        }
        g_object_set_data(G_OBJECT(APP->monitor_dropdown), "updating", GINT_TO_POINTER(1));
        gtk_drop_down_set_model(GTK_DROP_DOWN(APP->monitor_dropdown), G_LIST_MODEL(list));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(APP->monitor_dropdown), sel);
        g_object_set_data(G_OBJECT(APP->monitor_dropdown), "updating", NULL);
        g_object_unref(list);
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void close_response(AdwAlertDialog *dlg, const char *response, gpointer d)
{
    (void)dlg;
    (void)d;
    if (strcmp(response, "quit") == 0) {
        app_record_stop();
        gtk_window_destroy(APP->window);
    }
}

static gboolean on_close_request(GtkWindow *w, gpointer d)
{
    (void)w;
    (void)d;
    if (!APP->recording && APP->call_state == CALL_IDLE) return FALSE;
    AdwDialog *dlg = adw_alert_dialog_new(_("Quit now?"), NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dlg),
                              APP->recording
                                  ? _("The recording will be stopped and saved. The phone call "
                                      "itself continues on the phone.")
                                  : _("The phone call continues on the phone."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "quit",
                                   _("_Quit"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "quit",
                                             ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dlg, "response", G_CALLBACK(close_response), NULL);
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
    return TRUE;
}

static GMenuModel *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, _("_Add input"), "app.add-input");
    g_menu_append(s1, _("Choose Bluetooth _adapter..."), "app.adapter");
    g_menu_append(s1, _("Save adapter _report..."), "app.report");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, _("_Preferences"), "app.preferences");
    g_menu_append(s2, _("_Keyboard shortcuts"), "app.shortcuts");
    g_menu_append(s2, _("_About Phone Recorder"), "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
    g_object_unref(s1);
    g_object_unref(s2);
    return G_MENU_MODEL(menu);
}

void window_build(void)
{
    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(APP->gapp));
    APP->window = GTK_WINDOW(win);
    gtk_window_set_title(APP->window, _("Phone Recorder"));
    gtk_window_set_default_size(APP->window, 1280, 800);
    gtk_widget_add_css_class(win, "bpr-main");
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), NULL);

    GtkWidget *tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    APP->title = adw_window_title_new(_("Phone Recorder"), "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hb), APP->title);

    APP->record_btn = icon_text_button("media-record-symbolic", _("_Record"), "app.record");
    gtk_widget_add_css_class(APP->record_btn, "suggested-action");
    APP->record_label = gtk_label_new(_("Not recording"));
    gtk_widget_add_css_class(APP->record_label, "numeric");
    APP->marker_btn = icon_text_button("bookmark-new-symbolic", _("_Marker"), "app.marker");
    gtk_widget_set_tooltip_text(APP->marker_btn, _("Add a marker at this point (Ctrl+M or F9)"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), APP->record_btn);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), APP->record_label);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), APP->marker_btn);

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), build_menu());
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_btn), TRUE);
    gtk_widget_set_tooltip_text(menu_btn, _("Main menu (F10)"));
    set_a11y_label(menu_btn, _("Main menu"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), menu_btn);
    GtkWidget *add_btn = icon_text_button("list-add-symbolic", _("Add _input"), "app.add-input");
    gtk_widget_set_tooltip_text(add_btn, _("Add another microphone or input (Ctrl+I)"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), add_btn);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

    /* content */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), build_phone_panel());
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    GtkWidget *mix_outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *mix_title = gtk_label_new(_("Mixer"));
    gtk_widget_add_css_class(mix_title, "heading");
    gtk_label_set_xalign(GTK_LABEL(mix_title), 0);
    gtk_widget_set_margin_start(mix_title, 12);
    gtk_widget_set_margin_top(mix_title, 12);
    gtk_box_append(GTK_BOX(mix_outer), mix_title);
    GtkWidget *mix_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(mix_scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(mix_scroll, TRUE);
    gtk_widget_set_hexpand(mix_scroll, TRUE);
    GtkWidget *mix_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(mix_row, 12);
    gtk_widget_set_margin_end(mix_row, 12);
    gtk_widget_set_margin_top(mix_row, 12);
    gtk_widget_set_margin_bottom(mix_row, 12);
    APP->strip_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(mix_row), APP->strip_box);
    gtk_box_append(GTK_BOX(mix_row), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(mix_row), mixer_view_master_section());
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mix_scroll), mix_row);
    gtk_box_append(GTK_BOX(mix_outer), mix_scroll);
    gtk_paned_set_end_child(GTK_PANED(paned), mix_outer);
    gtk_paned_set_position(GTK_PANED(paned), 380);

    APP->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(APP->toasts, paned);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(APP->toasts));

    /* status line */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_widget_set_margin_start(status, 12);
    gtk_widget_set_margin_end(status, 12);
    gtk_widget_set_margin_top(status, 6);
    gtk_widget_set_margin_bottom(status, 6);
    APP->disk_label = gtk_label_new("");
    APP->health_label = gtk_label_new("");
    APP->session_label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(APP->session_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(APP->session_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(APP->session_label), 1);
    gtk_box_append(GTK_BOX(status), APP->disk_label);
    gtk_box_append(GTK_BOX(status), APP->health_label);
    gtk_box_append(GTK_BOX(status), APP->session_label);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), status);
    adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(tv), ADW_TOOLBAR_RAISED_BORDER);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), tv);
}
