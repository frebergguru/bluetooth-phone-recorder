/*
 * Dialogs: adapter chooser, preferences, pairing, PIN, report, shortcuts, about.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>

#include "app.h"
#include "config.h"
#include "core/paths.h"

/* ------------------------------------------------------------------ */
/* Pairing                                                             */
/* ------------------------------------------------------------------ */

static void pair_response(AdwAlertDialog *d, const char *response, gpointer addr)
{
    (void)d;
    bt_cmd_pair_reply(addr, strcmp(response, "pair") == 0);
    g_free(addr);
}

void dialog_pair_confirm(const char *addr, int number)
{
    AdwDialog *dlg = adw_alert_dialog_new(_("Pair with this phone?"), NULL);
    char num[16];
    snprintf(num, sizeof num, "%06d", number);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dlg),
                                 _("Check that the phone shows the code %s, then choose Pair on "
                                   "both the phone and here.\n\nPhone address: %s"),
                                 num, addr);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "pair",
                                   _("_Pair"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "pair", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "pair");
    g_signal_connect(dlg, "response", G_CALLBACK(pair_response), g_strdup(addr));
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
    char ann[128];
    snprintf(ann, sizeof ann, _("Pairing request, code %s"), num);
    app_announce(ann,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void pin_response(AdwAlertDialog *d, const char *response, gpointer addr)
{
    GtkWidget *entry = adw_alert_dialog_get_extra_child(d);
    if (strcmp(response, "pair") == 0)
        bt_cmd_pin_reply(addr, gtk_editable_get_text(GTK_EDITABLE(entry)));
    else
        bt_cmd_pin_reply(addr, NULL);
    g_free(addr);
}

void dialog_pin(const char *addr)
{
    AdwDialog *dlg = adw_alert_dialog_new(_("Enter a PIN"), NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dlg),
                              _("This older phone asks for a PIN. Type the same PIN here and on "
                                "the phone, for example 0000."));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_max_length(GTK_ENTRY(entry), 16); /* Bluetooth PINs have at most 16 characters */
    gtk_editable_set_text(GTK_EDITABLE(entry), "0000");
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, _("PIN"), -1);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dlg), entry);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "pair",
                                   _("_Pair"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "pair", ADW_RESPONSE_SUGGESTED);
    g_signal_connect(dlg, "response", G_CALLBACK(pin_response), g_strdup(addr));
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

/* ------------------------------------------------------------------ */
/* Adapter chooser                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    AdwDialog *dialog;
    bt_adapter_t adapters[BT_MAX_ADAPTERS];
    int n;
    GtkWidget *checks[BT_MAX_ADAPTERS + 1];
    GtkWidget *mode_rows[BT_MAX_ADAPTERS];
    GtkWidget *uart_check, *uart_path, *uart_baud, *uart_h5, *uart_flow;
    GtkWidget *list_group;
    GtkWidget *rows[BT_MAX_ADAPTERS];
    int n_rows; /* rows in list_group, may be a placeholder when no adapter */
} chooser_t;

static void chooser_free(gpointer p) { g_free(p); }

static void use_adapter(chooser_t *c, const bt_adapter_t *a)
{
    /* switching adapters restarts the app: never in the middle of a recording */
    if (APP->recording) {
        app_toast(_("Stop the recording first, then restart"));
        return;
    }
    char id[320];
    bt_adapter_to_id(a, id, sizeof id);
    snprintf(APP->settings.adapter_id, sizeof APP->settings.adapter_id, "%s", id);
    APP->cli_adapter[0] = 0;
    settings_save(&APP->settings);
    adw_dialog_close(c->dialog);
    if (APP->bt_started) {
        app_toast(_("Restarting to use the new adapter..."));
        app_request_restart();
    } else {
        APP->adapter = *a;
        APP->have_adapter = true;
        app_request_restart(); /* a clean start makes sure nothing of the old setup lingers */
    }
}

static void confirm_takeover_response(AdwAlertDialog *d, const char *response, gpointer data)
{
    (void)d;
    chooser_t *c = data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d), "idx"));
    if (strcmp(response, "use") == 0) use_adapter(c, &c->adapters[idx]);
}

static void on_use_clicked(GtkButton *b, chooser_t *c)
{
    (void)b;
    if (c->uart_check && gtk_check_button_get_active(GTK_CHECK_BUTTON(c->uart_check))) {
        bt_adapter_t a = {0};
        a.transport = BT_TRANSPORT_UART;
        a.hci_index = -1;
        snprintf(a.uart_path, sizeof a.uart_path, "%s",
                 gtk_editable_get_text(GTK_EDITABLE(c->uart_path)));
        a.uart_baud = (uint32_t)adw_spin_row_get_value(ADW_SPIN_ROW(c->uart_baud));
        a.uart_h5 = adw_switch_row_get_active(ADW_SWITCH_ROW(c->uart_h5));
        a.uart_flow_control = adw_switch_row_get_active(ADW_SWITCH_ROW(c->uart_flow));
        if (!a.uart_path[0]) {
            app_toast(_("Enter the serial port, for example /dev/ttyUSB0"));
            return;
        }
        use_adapter(c, &a);
        return;
    }
    for (int i = 0; i < c->n; i++) {
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(c->checks[i]))) continue;
        bt_adapter_t *a = &c->adapters[i];
        guint mode = adw_combo_row_get_selected(ADW_COMBO_ROW(c->mode_rows[i]));
        a->transport = mode == 1 ? BT_TRANSPORT_USERCHAN : BT_TRANSPORT_USB;
        if (a->transport == BT_TRANSPORT_USERCHAN && a->hci_index < 0) {
            app_toast(_("The kernel driver mode needs the adapter to be enabled in the system "
                        "first (it has no hci device right now)."));
            return;
        }
        if (a->driver[0] && a->transport == BT_TRANSPORT_USB) {
            AdwDialog *dlg = adw_alert_dialog_new(_("Take over this adapter?"), NULL);
            adw_alert_dialog_set_body(
                ADW_ALERT_DIALOG(dlg),
                _("The system uses this adapter now. While Phone Recorder runs, other Bluetooth "
                  "devices on it (headphones, mouse, keyboard) will disconnect. They work again "
                  "when Phone Recorder closes. A separate USB Bluetooth dongle avoids this."));
            adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "use",
                                           _("_Use it"), NULL);
            adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "use",
                                                     ADW_RESPONSE_DESTRUCTIVE);
            g_object_set_data(G_OBJECT(dlg), "idx", GINT_TO_POINTER(i));
            g_signal_connect(dlg, "response", G_CALLBACK(confirm_takeover_response), c);
            adw_dialog_present(dlg, GTK_WIDGET(APP->window));
            return;
        }
        use_adapter(c, a);
        return;
    }
    app_toast(_("Choose an adapter first"));
}

static void chooser_fill(chooser_t *c)
{
    for (int i = 0; i < c->n_rows; i++)
        if (c->rows[i]) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(c->list_group), c->rows[i]);
    memset(c->rows, 0, sizeof c->rows);
    memset(c->checks, 0, sizeof c->checks);
    c->n = bt_adapters_scan(c->adapters, BT_MAX_ADAPTERS);
    GtkWidget *first = NULL;
    for (int i = 0; i < c->n; i++) {
        bt_adapter_t *a = &c->adapters[i];
        GtkWidget *exp = adw_expander_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exp), a->name);
        char sub[512];
        snprintf(sub, sizeof sub, _("%s chip, USB %04x:%04x at %s. %s"), bt_chip_family_name(a->family),
                 a->vid, a->pid, a->sysfs_name,
                 a->driver[0] ? _("In use by the system now.") : _("Free, not used by the system."));
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(exp), sub);
        GtkWidget *check = gtk_check_button_new();
        if (first)
            gtk_check_button_set_group(GTK_CHECK_BUTTON(check), GTK_CHECK_BUTTON(first));
        else
            first = check;
        gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, a->name, -1);
        adw_expander_row_add_prefix(ADW_EXPANDER_ROW(exp), check);
        c->checks[i] = check;

        GtkWidget *mode = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mode), _("Mode"));
        GtkStringList *modes = gtk_string_list_new(NULL);
        gtk_string_list_append(modes, _("Direct USB (recommended)"));
        gtk_string_list_append(modes, _("Kernel driver, raw access"));
        adw_combo_row_set_model(ADW_COMBO_ROW(mode), G_LIST_MODEL(modes));
        g_object_unref(modes);
        adw_combo_row_set_selected(ADW_COMBO_ROW(mode), a->transport == BT_TRANSPORT_USERCHAN ? 1 : 0);
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(mode),
            a->usb_supported
                ? _("Direct USB: the app talks to the adapter itself. Kernel driver: the system "
                    "loads the firmware, the app gets exclusive raw access (needs the helper).")
                : _("This chip needs firmware that only the kernel driver can load, so use the "
                    "kernel driver mode (needs the helper, see README)."));
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(mode), 4);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(exp), mode);
        c->mode_rows[i] = mode;

        /* the saved choice: "usb:<port>" or "userchan:<port>", the port
         * compared exactly ("1-1" must not match "1-1.2") */
        const char *saved = APP->settings.adapter_id;
        bool is_usb = strncmp(saved, "usb:", 4) == 0, is_uc = strncmp(saved, "userchan:", 9) == 0;
        if ((is_usb && strcmp(saved + 4, a->sysfs_name) == 0) ||
            (is_uc && strcmp(saved + 9, a->sysfs_name) == 0)) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
            if (is_uc) adw_combo_row_set_selected(ADW_COMBO_ROW(mode), 1);
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(c->list_group), exp);
        c->rows[i] = exp;
    }
    if (first && c->uart_check) gtk_check_button_set_group(GTK_CHECK_BUTTON(c->uart_check), GTK_CHECK_BUTTON(first));
    if (c->n == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("No USB Bluetooth adapter found"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    _("Plug in a USB Bluetooth dongle and choose Refresh."));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(c->list_group), row);
        c->rows[0] = row;
        c->n_rows = 1; /* the placeholder is removed on refresh */
    } else {
        c->n_rows = c->n;
    }
}

static void on_refresh(GtkButton *b, chooser_t *c)
{
    (void)b;
    chooser_fill(c);
}

void dialog_adapter_chooser(gboolean first_run)
{
    chooser_t *c = g_new0(chooser_t, 1);
    c->dialog = adw_dialog_new();
    adw_dialog_set_title(c->dialog, _("Choose Bluetooth adapter"));
    adw_dialog_set_content_width(c->dialog, 640);
    adw_dialog_set_content_height(c->dialog, 640);
    g_object_set_data_full(G_OBJECT(c->dialog), "chooser", c, chooser_free);

    GtkWidget *tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, _("Search again"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Refresh"), -1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), c);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), refresh);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *intro = adw_preferences_group_new();
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(intro),
        first_run ? _("Welcome! Phone Recorder connects to your phone like a car kit. Choose the "
                      "Bluetooth adapter it should use. A separate USB dongle is best, so your "
                      "other Bluetooth devices keep working.")
                  : _("The app restarts to switch adapters."));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(intro));

    c->list_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(c->list_group), _("USB adapters"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(c->list_group));

    GtkWidget *uart = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(uart), _("Serial (UART) adapter"));
    GtkWidget *uart_exp = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(uart_exp), _("Use a serial adapter"));
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(uart_exp),
                                  _("For Bluetooth modules on a serial port (advanced)"));
    c->uart_check = gtk_check_button_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(c->uart_check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Use a serial adapter"), -1);
    adw_expander_row_add_prefix(ADW_EXPANDER_ROW(uart_exp), c->uart_check);
    c->uart_path = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->uart_path), _("Serial port"));
    gtk_editable_set_text(GTK_EDITABLE(c->uart_path), "/dev/ttyUSB0");
    adw_expander_row_add_row(ADW_EXPANDER_ROW(uart_exp), c->uart_path);
    c->uart_baud = adw_spin_row_new_with_range(9600, 4000000, 100);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->uart_baud), _("Speed (baud)"));
    adw_spin_row_set_value(ADW_SPIN_ROW(c->uart_baud), 115200);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(uart_exp), c->uart_baud);
    c->uart_h5 = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->uart_h5), _("Three-wire protocol (H5)"));
    adw_expander_row_add_row(ADW_EXPANDER_ROW(uart_exp), c->uart_h5);
    c->uart_flow = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(c->uart_flow), _("Hardware flow control"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(c->uart_flow), TRUE);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(uart_exp), c->uart_flow);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(uart), uart_exp);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(uart));

    chooser_fill(c);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bottom, GTK_ALIGN_END);
    gtk_widget_set_margin_start(bottom, 12);
    gtk_widget_set_margin_end(bottom, 12);
    gtk_widget_set_margin_top(bottom, 12);
    gtk_widget_set_margin_bottom(bottom, 12);
    GtkWidget *use = gtk_button_new_with_mnemonic(_("_Use this adapter"));
    gtk_widget_add_css_class(use, "suggested-action");
    gtk_widget_add_css_class(use, "pill");
    g_signal_connect(use, "clicked", G_CALLBACK(on_use_clicked), c);
    gtk_box_append(GTK_BOX(bottom), use);

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), page);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), bottom);
    adw_dialog_set_child(c->dialog, tv);
    adw_dialog_set_default_widget(c->dialog, use);
    adw_dialog_present(c->dialog, GTK_WIDGET(APP->window));
}

/* ------------------------------------------------------------------ */
/* Adapter report                                                      */
/* ------------------------------------------------------------------ */

static void report_saved(GObject *src, GAsyncResult *res, gpointer text)
{
    GError *err = NULL;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (f) {
        if (g_file_replace_contents(f, text, strlen(text), NULL, FALSE, G_FILE_CREATE_NONE, NULL,
                                    NULL, &err))
            app_toast(_("Adapter report saved"));
        g_object_unref(f);
    }
    if (err) {
        if (!g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) app_toast(err->message);
        g_error_free(err);
    }
    g_free(text);
}

void dialog_adapter_report(void)
{
    GString *s = g_string_new("Bluetooth Phone Recorder adapter report\n");
    g_string_append_printf(s, "App version: %s\n", BPR_VERSION);
    g_string_append_printf(s, "Status: %s\n\n", APP->bt_status);
    g_string_append(s, APP->controller_report[0] ? APP->controller_report
                                                 : "Controller not started.\n");
    g_string_append_printf(s, "\nCall audio codec: %s (%d Hz)\n",
                           APP->codec_name[0] ? APP->codec_name : "none yet", APP->codec_rate);
    phone_link_t *l = engine_phone_link(APP->engine);
    g_string_append_printf(s, "Damaged phone packets repaired: %llu\n",
                           (unsigned long long)atomic_load(&l->rx_bad_frames));
    g_string_append_printf(s, "Send underruns: %llu\n",
                           (unsigned long long)atomic_load(&l->tx_underruns));
    g_string_append(s, "\nAll USB Bluetooth adapters:\n");
    bt_adapter_t list[BT_MAX_ADAPTERS];
    int n = bt_adapters_scan(list, BT_MAX_ADAPTERS);
    for (int i = 0; i < n; i++)
        g_string_append_printf(s, "- %04x:%04x %s, %s, driver '%s', hci%d, usb-direct %s\n",
                               list[i].vid, list[i].pid, list[i].name,
                               bt_chip_family_name(list[i].family), list[i].driver,
                               list[i].hci_index, list[i].usb_supported ? "yes" : "no");
    g_string_append(s, "\nTip: turn on \"Save Bluetooth trace\" in Preferences, reproduce the "
                       "problem and attach the .pklg file from ~/.local/state/bt-phone-recorder.\n");

    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, _("Save adapter report"));
    gtk_file_dialog_set_initial_name(fd, "adapter-report.txt");
    gtk_file_dialog_save(fd, APP->window, NULL, report_saved, g_string_free(s, FALSE));
    g_object_unref(fd);
}

/* ------------------------------------------------------------------ */
/* Preferences                                                         */
/* ------------------------------------------------------------------ */

static void pref_switch(AdwSwitchRow *row, GParamSpec *ps, gboolean *target)
{
    (void)ps;
    *target = adw_switch_row_get_active(row);
    settings_save(&APP->settings);
    mixer_t *m = engine_mixer(APP->engine);
    atomic_store(&m->limit_full, APP->settings.limit_full);
}

static GtkWidget *switch_row(const char *title, const char *subtitle, gboolean *target)
{
    GtkWidget *r = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(r), *target);
    g_signal_connect(r, "notify::active", G_CALLBACK(pref_switch), target);
    return r;
}

typedef struct {
    char *buf;
    size_t len;
} str_target_t;

static void pref_entry(AdwEntryRow *row, str_target_t *t)
{
    snprintf(t->buf, t->len, "%s", gtk_editable_get_text(GTK_EDITABLE(row)));
    settings_save(&APP->settings);
}

static GtkWidget *entry_row(const char *title, char *buf, size_t len, gboolean password)
{
    GtkWidget *r = password ? adw_password_entry_row_new() : adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    gtk_editable_set_text(GTK_EDITABLE(r), buf);
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(r), TRUE);
    str_target_t *t = g_new0(str_target_t, 1);
    t->buf = buf;
    t->len = len;
    g_signal_connect_data(r, "apply", G_CALLBACK(pref_entry), t, (GClosureNotify)(void (*)(void))g_free, 0);
    return r;
}

static void on_pref_name_apply(AdwEntryRow *row, gpointer d)
{
    (void)d;
    app_set_bluetooth_name(gtk_editable_get_text(GTK_EDITABLE(row)));
    gtk_editable_set_text(GTK_EDITABLE(row), APP->settings.local_name);
    if (APP->name_row) gtk_editable_set_text(GTK_EDITABLE(APP->name_row), APP->settings.local_name);
}

static void folder_chosen(GObject *src, GAsyncResult *res, gpointer row)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        char *path = g_file_get_path(f);
        if (path) {
            snprintf(APP->settings.record_dir, sizeof APP->settings.record_dir, "%s", path);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), path);
            settings_save(&APP->settings);
            g_free(path);
        }
        g_object_unref(f);
    }
    g_object_unref(row); /* the Preferences dialog may have closed meanwhile */
}

static void on_choose_folder(GtkButton *b, gpointer row)
{
    (void)b;
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, _("Recording folder"));
    GFile *init = g_file_new_for_path(APP->settings.record_dir);
    gtk_file_dialog_set_initial_folder(fd, init);
    g_object_unref(init);
    gtk_file_dialog_select_folder(fd, APP->window, NULL, folder_chosen, g_object_ref(row));
    g_object_unref(fd);
}

static void on_format(AdwComboRow *row, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    APP->settings.record_format = (rec_format_t)adw_combo_row_get_selected(row);
    settings_save(&APP->settings);
}

static void on_text_scale(AdwSpinRow *row, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    APP->settings.text_scale_percent = (int)adw_spin_row_get_value(row);
    app_apply_text_scale();
    settings_save(&APP->settings);
}

static void on_language(AdwComboRow *row, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    static const char *codes[] = {"", "en", "nb"};
    guint sel = adw_combo_row_get_selected(row);
    if (sel >= G_N_ELEMENTS(codes) || strcmp(APP->settings.language, codes[sel]) == 0) return;
    snprintf(APP->settings.language, sizeof APP->settings.language, "%s", codes[sel]);
    settings_save(&APP->settings);
    AdwToast *t = adw_toast_new(_("The new language is used after a restart"));
    adw_toast_set_button_label(t, _("Restart now"));
    adw_toast_set_action_name(t, "app.restart");
    adw_toast_set_timeout(t, 0);
    adw_toast_overlay_add_toast(APP->toasts, t);
}

static void on_obs_reconnect(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (APP->settings.obs_enabled)
        obs_ws_connect(APP->obs, APP->settings.obs_host, APP->settings.obs_port,
                       APP->settings.obs_password);
    else
        obs_ws_disconnect(APP->obs);
}

static void on_obs_port(AdwSpinRow *row, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    APP->settings.obs_port = (int)adw_spin_row_get_value(row);
    settings_save(&APP->settings);
}

void dialog_preferences(void)
{
    settings_t *s = &APP->settings;
    AdwDialog *dlg = adw_preferences_dialog_new();
    adw_dialog_set_title(dlg, _("Preferences"));

    /* Recording */
    GtkWidget *p1 = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p1), "recording");
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p1), _("Recording"));
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p1), "media-record-symbolic");
    GtkWidget *g1 = adw_preferences_group_new();
    GtkWidget *folder = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder), _("Recording folder"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(folder), s->record_dir);
    GtkWidget *fbtn = gtk_button_new_with_mnemonic(_("_Change..."));
    gtk_widget_set_valign(fbtn, GTK_ALIGN_CENTER);
    g_signal_connect(fbtn, "clicked", G_CALLBACK(on_choose_folder), folder);
    adw_action_row_add_suffix(ADW_ACTION_ROW(folder), fbtn);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), folder);
    GtkWidget *fmt = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fmt), _("File format"));
    GtkStringList *fmts = gtk_string_list_new(NULL);
    gtk_string_list_append(fmts, _("WAV, 32-bit float (best for editing)"));
    gtk_string_list_append(fmts, _("WAV, 24-bit"));
    gtk_string_list_append(fmts, _("FLAC, 24-bit (lossless, smaller)"));
    gtk_string_list_append(fmts, _("Opus (small, for quick sharing)"));
    adw_combo_row_set_model(ADW_COMBO_ROW(fmt), G_LIST_MODEL(fmts));
    g_object_unref(fmts);
    adw_combo_row_set_selected(ADW_COMBO_ROW(fmt), (guint)s->record_format);
    g_signal_connect(fmt, "notify::selected", G_CALLBACK(on_format), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1), fmt);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1),
                              switch_row(_("Start recording when a call starts"), NULL, &s->auto_record));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g1),
                              switch_row(_("Stop automatic recordings when the call ends"), NULL,
                                         &s->auto_stop));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p1), ADW_PREFERENCES_GROUP(g1));

    /* Audio */
    GtkWidget *p2 = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p2), "audio");
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p2), _("Audio"));
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p2), "audio-volume-high-symbolic");
    GtkWidget *g2 = adw_preferences_group_new();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g2),
                              switch_row(_("Protect the full mix from clipping"),
                                         _("A gentle peak limiter at -1 dB on the stereo mix. "
                                           "Single-input tracks are never limited."),
                                         &s->limit_full));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p2), ADW_PREFERENCES_GROUP(g2));

    /* Bluetooth */
    GtkWidget *p3 = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p3), "bluetooth");
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p3), _("Bluetooth"));
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p3), "bluetooth-symbolic");
    GtkWidget *g3 = adw_preferences_group_new();
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g3),
                                          _("The name changes right away. The other settings here take effect after a restart."));
    GtkWidget *name = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name), _("Bluetooth name (shown on the phone)"));
    gtk_editable_set_text(GTK_EDITABLE(name), s->local_name);
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(name), TRUE);
    g_signal_connect(name, "apply", G_CALLBACK(on_pref_name_apply), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3), name);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("HD voice (mSBC, 16 kHz)"), NULL, &s->wideband));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("Super wideband (LC3-SWB, 32 kHz)"),
                                         _("Used when the phone supports it"), &s->super_wideband));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("Ask the phone to skip its own noise reduction"),
                                         _("Your microphones are already clean, so the caller "
                                           "hears you more naturally"),
                                         &s->phone_nrec_off));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("Download the phone's contacts"),
                                         _("Kept only on this computer. Used for the contact list "
                                           "and to show names of callers."),
                                         &s->fetch_contacts));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("Secure Connections"),
                                         _("Turn off only if pairing or call audio fails with an "
                                           "older adapter"),
                                         &s->secure_connections));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              entry_row(_("Extra firmware folder (optional)"), s->firmware_dir,
                                        sizeof s->firmware_dir, FALSE));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g3),
                              switch_row(_("Save Bluetooth trace"),
                                         _("For troubleshooting. Saved in "
                                           "~/.local/state/bt-phone-recorder"),
                                         &s->hci_trace));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p3), ADW_PREFERENCES_GROUP(g3));

    /* OBS */
    GtkWidget *p4 = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p4), "obs");
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p4), _("OBS"));
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p4), "camera-video-symbolic");
    GtkWidget *g4a = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g4a), _("Audio in OBS"));
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(g4a),
        _("Always available, nothing to set up here: in OBS add an \"Audio Input Capture\" "
          "source and choose \"Phone Recorder: Full mix\", \"Phone Recorder: Caller\", "
          "\"Phone Recorder: Mix-minus\" or one of your inputs. Put them on separate OBS tracks "
          "if you want to edit them separately."));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p4), ADW_PREFERENCES_GROUP(g4a));
    GtkWidget *g4 = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g4), _("Remote control (obs-websocket)"));
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g4),
                                          _("In OBS: Tools, WebSocket Server Settings. Enable it "
                                            "and copy the port and password here."));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4),
                              switch_row(_("Control OBS"), NULL, &s->obs_enabled));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4),
                              entry_row(_("Host"), s->obs_host, sizeof s->obs_host, FALSE));
    GtkWidget *port = adw_spin_row_new_with_range(1, 65535, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(port), _("Port"));
    adw_spin_row_set_value(ADW_SPIN_ROW(port), s->obs_port);
    g_signal_connect(port, "notify::value", G_CALLBACK(on_obs_port), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), port);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4),
                              entry_row(_("Password"), s->obs_password, sizeof s->obs_password, TRUE));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4),
                              switch_row(_("Start and stop OBS recording together with this app"),
                                         NULL, &s->obs_sync_record));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4),
                              switch_row(_("Add markers as OBS chapters"),
                                         _("Needs OBS 30.2 or newer and a format with chapters"),
                                         &s->obs_chapters));
    GtkWidget *conn = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(conn), _("Connect now"));
    g_signal_connect(conn, "activated", G_CALLBACK(on_obs_reconnect), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g4), conn);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p4), ADW_PREFERENCES_GROUP(g4));

    /* Accessibility */
    GtkWidget *p5 = adw_preferences_page_new();
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(p5), "accessibility");
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(p5), _("Language and accessibility"));
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(p5), "preferences-desktop-accessibility-symbolic");
    GtkWidget *glang = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(glang), _("Language"));
    GtkWidget *lang = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lang), _("Language of the app"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(lang), _("Takes effect after a restart"));
    GtkStringList *langs = gtk_string_list_new(NULL);
    gtk_string_list_append(langs, _("System default"));
    /* language names are shown in their own language, never translated */
    gtk_string_list_append(langs, "English");
    gtk_string_list_append(langs, "Norsk bokmål");
    adw_combo_row_set_model(ADW_COMBO_ROW(lang), G_LIST_MODEL(langs));
    g_object_unref(langs);
    guint lang_sel = strcmp(s->language, "en") == 0 ? 1 : strcmp(s->language, "nb") == 0 ? 2 : 0;
    adw_combo_row_set_selected(ADW_COMBO_ROW(lang), lang_sel);
    g_signal_connect(lang, "notify::selected", G_CALLBACK(on_language), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(glang), lang);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p5), ADW_PREFERENCES_GROUP(glang));

    GtkWidget *g5 = adw_preferences_group_new();
    GtkWidget *scale = adw_spin_row_new_with_range(100, 200, 10);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scale), _("Text size (percent)"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(scale), _("Also Ctrl+plus and Ctrl+minus"));
    adw_spin_row_set_value(ADW_SPIN_ROW(scale), s->text_scale_percent);
    g_signal_connect(scale, "notify::value", G_CALLBACK(on_text_scale), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g5), scale);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(p5), ADW_PREFERENCES_GROUP(g5));

    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), ADW_PREFERENCES_PAGE(p1));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), ADW_PREFERENCES_PAGE(p2));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), ADW_PREFERENCES_PAGE(p3));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), ADW_PREFERENCES_PAGE(p4));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dlg), ADW_PREFERENCES_PAGE(p5));
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

/* ------------------------------------------------------------------ */
/* Shortcuts and about                                                 */
/* ------------------------------------------------------------------ */

void dialog_shortcuts(void)
{
    AdwDialog *dlg = adw_shortcuts_dialog_new();
    struct {
        const char *section;
        const char *title;
        const char *accel;
    } items[] = {
        {N_("Recording"), N_("Start or stop recording"), "<Control>r"},
        {N_("Recording"), N_("Add a marker"), "<Control>m"},
        {N_("Recording"), N_("Add a marker"), "F9"},
        {N_("Calls"), N_("Answer"), "<Control><Shift>a"},
        {N_("Calls"), N_("Decline or hang up"), "<Control><Shift>h"},
        {N_("Calls"), N_("Move call audio between phone and computer"), "<Control><Shift>t"},
        {N_("Calls"), N_("Contacts"), "<Control>k"},
        {N_("Calls"), N_("Call log"), "<Control>l"},
        {N_("Calls"), N_("Hold or resume the call"), "<Control><Shift>p"},
        {N_("Calls"), N_("Microphones on or off for the caller"), "<Control><Shift>m"},
        {N_("Mixer"), N_("Input 1 to 9: heard by the caller on or off"), "<Control>1"},
        {N_("Mixer"), N_("Input 1 to 9: mute on or off"), "<Alt>1"},
        {N_("Mixer"), N_("Add an input"), "<Control>i"},
        {N_("General"), N_("Preferences"), "<Control>comma"},
        {N_("General"), N_("Larger text"), "<Control>plus"},
        {N_("General"), N_("Smaller text"), "<Control>minus"},
        {N_("General"), N_("Normal text size"), "<Control>0"},
        {N_("General"), N_("Keyboard shortcuts"), "F1"},
        {N_("General"), N_("Quit"), "<Control>q"},
    };
    AdwShortcutsSection *sec = NULL;
    const char *cur = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
        if (!cur || strcmp(cur, items[i].section) != 0) {
            if (sec) adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dlg), sec);
            sec = adw_shortcuts_section_new(_(items[i].section));
            cur = items[i].section;
        }
        adw_shortcuts_section_add(sec, adw_shortcuts_item_new(_(items[i].title), items[i].accel));
    }
    if (sec) adw_shortcuts_dialog_add(ADW_SHORTCUTS_DIALOG(dlg), sec);
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

void dialog_about(void)
{
    AdwDialog *about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), _("Phone Recorder"));
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), BPR_APP_ID);
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), BPR_VERSION);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
                                  _("Record phone calls over Bluetooth for podcasts. Works like a "
                                    "car kit, with its own Bluetooth stack."));
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_BSD_3);
    adw_about_dialog_add_legal_section(
        ADW_ABOUT_DIALOG(about), "BTstack", "Copyright BlueKitchen GmbH", GTK_LICENSE_CUSTOM,
        _("BTstack is free for non-commercial use only. Because Phone Recorder includes BTstack, "
          "the program as a whole may only be used and shared for non-commercial purposes. "
          "See NOTICE."));
    adw_dialog_present(about, GTK_WIDGET(APP->window));
}

/* ------------------------------------------------------------------ */
/* Contacts (the phone's address book)                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    AdwDialog *dialog;
    GtkWidget *search, *list, *status;
} contacts_ui_t;

static const char *tel_type_label(tel_type_t t)
{
    switch (t) {
    case TEL_MOBILE: return _("Mobile");
    case TEL_HOME: return _("Home");
    case TEL_WORK: return _("Work");
    case TEL_OTHER:
    default: return _("Phone");
    }
}

static void on_contact_call(GtkButton *b, gpointer ui_ptr)
{
    contacts_ui_t *ui = ui_ptr;
    const char *number = g_object_get_data(G_OBJECT(b), "number");
    if (!APP->phone_connected || APP->call_state != CALL_IDLE) {
        app_dial(number, NULL, 0); /* explains why not */
        return;
    }
    app_dial(number, g_object_get_data(G_OBJECT(b), "name"), 0);
    adw_dialog_close(ui->dialog);
}

static GtkWidget *call_button(contacts_ui_t *ui, const contact_t *c, int k)
{
    GtkWidget *b = gtk_button_new_from_icon_name("call-start-symbolic");
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    char a11y[300];
    snprintf(a11y, sizeof a11y, _("Call %s, %s %s"), c->name, tel_type_label(c->numbers[k].type),
             c->numbers[k].number);
    gtk_widget_set_tooltip_text(b, a11y);
    gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, a11y, -1);
    g_object_set_data_full(G_OBJECT(b), "number", g_strdup(c->numbers[k].number), g_free);
    g_object_set_data_full(G_OBJECT(b), "name", g_strdup(c->name), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_contact_call), ui);
    return b;
}

static gboolean contact_filter(GtkListBoxRow *row, gpointer ui_ptr)
{
    contacts_ui_t *ui = ui_ptr;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ui->search));
    if (!q || !*q) return TRUE;
    const char *hay = g_object_get_data(G_OBJECT(row), "search");
    char *fold = g_utf8_casefold(q, -1);
    gboolean hit = hay && strstr(hay, fold) != NULL;
    if (!hit && hay) {
        char digits[64];
        phone_normalize(q, digits, sizeof digits);
        if (strlen(digits) >= 2) hit = strstr(hay, digits) != NULL;
    }
    g_free(fold);
    return hit;
}

static void contacts_fill(contacts_ui_t *ui)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ui->list))) gtk_list_box_remove(GTK_LIST_BOX(ui->list), child);
    for (int i = 0; i < APP->n_contacts; i++) {
        const contact_t *c = &APP->contacts[i];
        /* search text: folded name plus the digits of every number */
        GString *s = g_string_new(NULL);
        char *fold = g_utf8_casefold(c->name, -1);
        g_string_append(s, fold);
        g_free(fold);
        for (int k = 0; k < c->n_numbers; k++) {
            char d[64];
            phone_normalize(c->numbers[k].number, d, sizeof d);
            g_string_append_printf(s, " %s", d);
        }
        GtkWidget *row;
        if (c->n_numbers == 1) {
            row = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), c->name);
            char sub[96];
            snprintf(sub, sizeof sub, "%s: %s", tel_type_label(c->numbers[0].type), c->numbers[0].number);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_star_button(c->name, c->numbers[0].number));
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_queue_button(c->name, c->numbers[0].number));
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), call_button(ui, c, 0));
        } else {
            row = adw_expander_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), c->name);
            char sub[64];
            snprintf(sub, sizeof sub, g_dngettext(NULL, "%d number", "%d numbers", (gulong)c->n_numbers),
                     c->n_numbers);
            adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), sub);
            for (int k = 0; k < c->n_numbers; k++) {
                GtkWidget *nr = adw_action_row_new();
                adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(nr), FALSE);
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(nr), c->numbers[k].number);
                adw_action_row_set_subtitle(ADW_ACTION_ROW(nr), tel_type_label(c->numbers[k].type));
                adw_action_row_add_suffix(ADW_ACTION_ROW(nr), calls_view_star_button(c->name, c->numbers[k].number));
                adw_action_row_add_suffix(ADW_ACTION_ROW(nr), calls_view_queue_button(c->name, c->numbers[k].number));
                adw_action_row_add_suffix(ADW_ACTION_ROW(nr), call_button(ui, c, k));
                adw_expander_row_add_row(ADW_EXPANDER_ROW(row), nr);
            }
        }
        g_object_set_data_full(G_OBJECT(row), "search", g_string_free(s, FALSE), g_free);
        gtk_list_box_append(GTK_LIST_BOX(ui->list), row);
    }
    char status[300];
    if (APP->n_contacts > 0)
        snprintf(status, sizeof status,
                 g_dngettext(NULL, "%d contact from %s", "%d contacts from %s", (gulong)APP->n_contacts),
                 APP->n_contacts, APP->phone_name[0] ? APP->phone_name : _("the phone"));
    else
        snprintf(status, sizeof status, "%s",
                 _("No contacts yet. Connect a phone and choose Refresh. The phone may ask "
                   "whether to share its contacts."));
    gtk_label_set_text(GTK_LABEL(ui->status), status);
    gtk_widget_set_visible(ui->list, APP->n_contacts > 0);
}

static void on_contacts_search(GtkSearchEntry *e, gpointer ui_ptr)
{
    (void)e;
    contacts_ui_t *ui = ui_ptr;
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui->list));
}

static void on_contacts_refresh(GtkButton *b, gpointer ui_ptr)
{
    (void)b;
    (void)ui_ptr;
    if (!APP->phone_connected) {
        app_toast(_("Connect a phone first"));
        return;
    }
    bt_cmd_fetch_contacts();
    app_toast(_("Downloading contacts from the phone..."));
}

void dialog_contacts(void)
{
    contacts_ui_t *ui = g_new0(contacts_ui_t, 1);
    ui->dialog = adw_dialog_new();
    adw_dialog_set_title(ui->dialog, _("Contacts"));
    adw_dialog_set_content_width(ui->dialog, 480);
    adw_dialog_set_content_height(ui->dialog, 640);
    g_object_set_data_full(G_OBJECT(ui->dialog), "ui", ui, g_free);

    GtkWidget *tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, _("Download the contacts from the phone again"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Refresh contacts"), -1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_contacts_refresh), ui);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), refresh);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 12);
    ui->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(ui->search), _("Search by name or number"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(ui->search), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Search contacts"), -1);
    g_signal_connect(ui->search, "search-changed", G_CALLBACK(on_contacts_search), ui);
    ui->status = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(ui->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui->status), 0);
    gtk_widget_add_css_class(ui->status, "dim-label");
    ui->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(ui->list, "boxed-list");
    gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list), contact_filter, ui, NULL);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), ui->search);
    gtk_box_append(GTK_BOX(box), ui->status);
    gtk_box_append(GTK_BOX(box), scroll);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), box);
    adw_dialog_set_child(ui->dialog, tv);
    adw_dialog_set_focus(ui->dialog, ui->search);
    contacts_fill(ui);
    adw_dialog_present(ui->dialog, GTK_WIDGET(APP->window));
}
