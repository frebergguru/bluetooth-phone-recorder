/*
 * Favorites, call queue (main window) and the call log window.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The queue never dials by itself: every call is started with its own Call
 * button. An entry is marked done when its call was answered.
 */
#include <string.h>

#include "app.h"

static void a11y(GtkWidget *w, const char *label)
{
    /* A button's visible text ("Call") is linked as "labelled-by", which
     * screen readers prefer over an explicit label: every queue row would
     * then just say "Call". Drop the link so "Call <name>" is read. */
    gtk_accessible_reset_relation(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_RELATION_LABELLED_BY);
    gtk_accessible_update_property(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static gboolean can_call(void)
{
    return APP->phone_connected && APP->call_state == CALL_IDLE;
}

static const char *who(const char *name, const char *number)
{
    return name && *name ? name : number;
}

/* ---- star and queue buttons (also used by the contacts and log windows) ---- */

static void update_star(GtkWidget *b)
{
    const char *number = g_object_get_data(G_OBJECT(b), "number");
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    gboolean on = fav_is(number);
    gtk_button_set_icon_name(GTK_BUTTON(b), on ? "starred-symbolic" : "non-starred-symbolic");
    char label[300];
    snprintf(label, sizeof label, on ? _("Remove %s from favorites") : _("Add %s to favorites"),
             who(name, number));
    gtk_widget_set_tooltip_text(b, label);
    a11y(b, label);
    gtk_accessible_update_state(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_STATE_PRESSED,
                                on ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE, -1);
}

static void on_star(GtkButton *b, gpointer d)
{
    (void)d;
    fav_toggle(g_object_get_data(G_OBJECT(b), "name"), g_object_get_data(G_OBJECT(b), "number"));
    update_star(GTK_WIDGET(b));
    calls_view_refresh();
}

GtkWidget *calls_view_star_button(const char *name, const char *number)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(b), "number", g_strdup(number), g_free);
    g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name ? name : ""), g_free);
    update_star(b);
    g_signal_connect(b, "clicked", G_CALLBACK(on_star), NULL);
    return b;
}

static void on_queue_add_btn(GtkButton *b, gpointer d)
{
    (void)d;
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    const char *number = g_object_get_data(G_OBJECT(b), "number");
    queue_add(name, number, NULL);
    char msg[256];
    snprintf(msg, sizeof msg, _("%s added to the call queue"), who(name, number));
    app_toast(msg);
    calls_view_refresh();
}

GtkWidget *calls_view_queue_button(const char *name, const char *number)
{
    GtkWidget *b = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    char label[300];
    snprintf(label, sizeof label, _("Add %s to the call queue"), who(name, number));
    gtk_widget_set_tooltip_text(b, label);
    a11y(b, label);
    g_object_set_data_full(G_OBJECT(b), "number", g_strdup(number), g_free);
    g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name ? name : ""), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_queue_add_btn), NULL);
    return b;
}

/* ---- call queue ---- */

static void on_queue_call(GtkButton *b, gpointer d)
{
    (void)d;
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "id"));
    queue_item_t *q = queue_find(id);
    if (q) app_dial(q->number, q->name, id);
}

static void on_queue_up(GtkButton *b, gpointer d)
{
    (void)d;
    queue_move(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "id")), -1);
    calls_view_refresh();
}

static void on_queue_down(GtkButton *b, gpointer d)
{
    (void)d;
    queue_move(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "id")), +1);
    calls_view_refresh();
}

static void on_queue_remove(GtkButton *b, gpointer d)
{
    (void)d;
    queue_remove(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "id")));
    calls_view_refresh();
}

static void on_queue_manual_add(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    const char *number = gtk_editable_get_text(GTK_EDITABLE(APP->queue_number));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(APP->queue_name));
    if (!number || !*number) {
        app_toast(_("Type a number to add to the queue"));
        return;
    }
    char nm[128];
    snprintf(nm, sizeof nm, "%s", name && *name ? name : "");
    if (!nm[0]) {
        const char *c = app_contact_name(number);
        if (c) snprintf(nm, sizeof nm, "%s", c);
    }
    queue_add(nm, number, NULL);
    gtk_editable_set_text(GTK_EDITABLE(APP->queue_number), "");
    gtk_editable_set_text(GTK_EDITABLE(APP->queue_name), "");
    calls_view_refresh();
}

static void on_queue_clear_done(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    queue_clear_done();
    calls_view_refresh();
}

static GtkWidget *small_button(const char *icon, const char *label, guint id, GCallback cb)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(b, label);
    a11y(b, label);
    g_object_set_data(G_OBJECT(b), "id", GUINT_TO_POINTER(id));
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *queue_clear_btn;

static void on_fav_call(GtkButton *b, gpointer d)
{
    (void)d;
    app_dial(g_object_get_data(G_OBJECT(b), "number"), g_object_get_data(G_OBJECT(b), "name"), 0);
}

GtkWidget *calls_view_queue_group(void)
{
    APP->queue_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(APP->queue_group), _("Call queue"));
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(APP->queue_group),
                                          _("People to call, in order. Nothing is dialled until "
                                            "you press Call."));
    queue_clear_btn = gtk_button_new_with_mnemonic(_("Clear _done"));
    gtk_widget_add_css_class(queue_clear_btn, "flat");
    gtk_widget_set_tooltip_text(queue_clear_btn, _("Remove the entries that have been called"));
    g_signal_connect(queue_clear_btn, "clicked", G_CALLBACK(on_queue_clear_done), NULL);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(APP->queue_group), queue_clear_btn);

    GtkWidget *add = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    APP->queue_number = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(APP->queue_number), GTK_INPUT_PURPOSE_PHONE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(APP->queue_number), _("Number"));
    gtk_widget_set_hexpand(APP->queue_number, TRUE);
    a11y(APP->queue_number, _("Number to add to the call queue"));
    APP->queue_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(APP->queue_name), _("Name (optional)"));
    gtk_widget_set_hexpand(APP->queue_name, TRUE);
    a11y(APP->queue_name, _("Name for the queue entry"));
    GtkWidget *addb = gtk_button_new_with_mnemonic(_("A_dd"));
    g_signal_connect(addb, "clicked", G_CALLBACK(on_queue_manual_add), NULL);
    g_signal_connect(APP->queue_number, "activate", G_CALLBACK(on_queue_manual_add), NULL);
    g_signal_connect(APP->queue_name, "activate", G_CALLBACK(on_queue_manual_add), NULL);
    gtk_box_append(GTK_BOX(add), APP->queue_number);
    gtk_box_append(GTK_BOX(add), APP->queue_name);
    gtk_box_append(GTK_BOX(add), addb);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->queue_group), add);
    return APP->queue_group;
}

GtkWidget *calls_view_fav_group(void)
{
    APP->fav_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(APP->fav_group), _("Favorites"));
    return APP->fav_group;
}

static void clear_rows(GtkWidget *group, GtkWidget **rows, int max)
{
    for (int i = 0; i < max; i++) {
        if (rows[i]) adw_preferences_group_remove(ADW_PREFERENCES_GROUP(group), rows[i]);
        rows[i] = NULL;
    }
}

static int last_callable = -1; /* can_call() when the rows were last built */

void calls_view_refresh(void)
{
    if (!APP->queue_group || !QUEUE) return;
    gboolean callable = can_call();
    last_callable = callable;

    /* queue */
    clear_rows(APP->queue_group, APP->queue_rows, 64);
    int done = 0;
    for (guint i = 0; i < QUEUE->len && i < 64; i++) {
        queue_item_t *q = &g_array_index(QUEUE, queue_item_t, i);
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        char title[200];
        snprintf(title, sizeof title, "%u. %s", i + 1, who(q->name, q->number));
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        char sub[260];
        /* "Done" in words, not only a colour or an icon */
        snprintf(sub, sizeof sub, "%s%s%s", q->name[0] ? q->number : "",
                 q->done ? (q->name[0] ? ", " : "") : "", q->done ? _("Done") : "");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        char label[300];
        snprintf(label, sizeof label, _("Move %s up"), who(q->name, q->number));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), small_button("go-up-symbolic", label, q->id, G_CALLBACK(on_queue_up)));
        snprintf(label, sizeof label, _("Move %s down"), who(q->name, q->number));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), small_button("go-down-symbolic", label, q->id, G_CALLBACK(on_queue_down)));
        snprintf(label, sizeof label, _("Remove %s from the queue"), who(q->name, q->number));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), small_button("user-trash-symbolic", label, q->id, G_CALLBACK(on_queue_remove)));
        GtkWidget *call = gtk_button_new_with_label(C_("verb", "Call"));
        gtk_widget_add_css_class(call, "suggested-action");
        gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
        snprintf(label, sizeof label, _("Call %s"), who(q->name, q->number));
        a11y(call, label);
        gtk_widget_set_tooltip_text(call, callable ? label : _("Connect a phone and end any call first"));
        gtk_widget_set_sensitive(call, callable);
        g_object_set_data(G_OBJECT(call), "id", GUINT_TO_POINTER(q->id));
        g_signal_connect(call, "clicked", G_CALLBACK(on_queue_call), NULL);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), call);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->queue_group), row);
        APP->queue_rows[i] = row;
        if (q->done) done++;
    }
    gtk_widget_set_visible(queue_clear_btn, done > 0);

    /* favorites */
    clear_rows(APP->fav_group, APP->fav_rows, 64);
    if (FAVS->len == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("No favorites yet"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    _("Use the star in Contacts or the Call log to add one."));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->fav_group), row);
        APP->fav_rows[0] = row;
    }
    for (guint i = 0; i < FAVS->len && i < 64; i++) {
        fav_t *f = &g_array_index(FAVS, fav_t, i);
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), who(f->name, f->number));
        if (f->name[0]) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), f->number);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_queue_button(f->name, f->number));
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_star_button(f->name, f->number));
        GtkWidget *call = gtk_button_new_from_icon_name("call-start-symbolic");
        gtk_widget_add_css_class(call, "flat");
        gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
        char label[300];
        snprintf(label, sizeof label, _("Call %s"), who(f->name, f->number));
        a11y(call, label);
        gtk_widget_set_tooltip_text(call, callable ? label : _("Connect a phone and end any call first"));
        gtk_widget_set_sensitive(call, callable);
        g_object_set_data_full(G_OBJECT(call), "number", g_strdup(f->number), g_free);
        g_object_set_data_full(G_OBJECT(call), "name", g_strdup(f->name), g_free);
        g_signal_connect(call, "clicked", G_CALLBACK(on_fav_call), NULL);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), call);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(APP->fav_group), row);
        APP->fav_rows[i] = row;
    }
}

void calls_view_update_callable(void)
{
    /* Rebuilding the rows moves keyboard focus away, so only do it when the
     * Call buttons actually change (not on every battery or signal update). */
    if ((int)can_call() != last_callable) calls_view_refresh();
}

/* ---- call log window ---- */

static const char *dir_label(call_dir_t d)
{
    switch (d) {
    case CALL_DIR_IN: return _("Incoming");
    case CALL_DIR_OUT: return _("Outgoing");
    case CALL_DIR_MISSED: return _("Missed");
    case CALL_DIR_DECLINED: return _("Declined");
    default: return "";
    }
}

static const char *dir_icon(call_dir_t d)
{
    switch (d) {
    case CALL_DIR_OUT: return "call-outgoing-symbolic";
    case CALL_DIR_MISSED: return "call-missed-symbolic";
    case CALL_DIR_DECLINED: return "call-stop-symbolic";
    case CALL_DIR_IN:
    default: return "call-incoming-symbolic";
    }
}

typedef struct {
    AdwDialog *dialog;
    GtkWidget *list, *status;
} log_ui_t;

static void log_fill(log_ui_t *ui);

static void on_log_call(GtkButton *b, gpointer ui_ptr)
{
    log_ui_t *ui = ui_ptr;
    const char *number = g_object_get_data(G_OBJECT(b), "number");
    if (!can_call()) {
        app_dial(number, NULL, 0); /* explains why not */
        return;
    }
    app_dial(number, g_object_get_data(G_OBJECT(b), "name"), 0);
    adw_dialog_close(ui->dialog);
}

static void clear_log_response(AdwAlertDialog *d, const char *response, gpointer ui_ptr)
{
    (void)d;
    if (strcmp(response, "clear") != 0) return;
    call_log_clear();
    log_fill(ui_ptr);
}

static void on_log_clear(GtkButton *b, gpointer ui_ptr)
{
    (void)b;
    AdwDialog *dlg = adw_alert_dialog_new(_("Clear the call log?"), NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dlg), _("All entries are deleted from this computer. "
                                                       "The phone's own call history is not changed."));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "clear", _("C_lear"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "clear", ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dlg, "response", G_CALLBACK(clear_log_response), ui_ptr);
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

static void log_fill(log_ui_t *ui)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ui->list))) gtk_list_box_remove(GTK_LIST_BOX(ui->list), child);
    for (guint i = 0; i < CALL_LOG->len; i++) {
        log_entry_t *e = &g_array_index(CALL_LOG, log_entry_t, i);
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        const char *num = e->number[0] ? e->number : _("unknown number");
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), e->name[0] ? e->name : num);
        GDateTime *dt = g_date_time_new_from_unix_local(e->time);
        char *when = dt ? g_date_time_format(dt, "%x %H:%M") : g_strdup("");
        if (dt) g_date_time_unref(dt);
        char dur[32] = "";
        if (e->duration > 0)
            snprintf(dur, sizeof dur, ", %d:%02d:%02d", e->duration / 3600, e->duration / 60 % 60,
                     e->duration % 60);
        char sub[300];
        /* direction in words (the icon is only an extra) */
        snprintf(sub, sizeof sub, "%s, %s%s%s%s", dir_label(e->dir), when, dur,
                 e->name[0] ? ", " : "", e->name[0] ? num : "");
        g_free(when);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        GtkWidget *icon = gtk_image_new_from_icon_name(dir_icon(e->dir));
        gtk_accessible_update_state(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
        if (e->number[0]) {
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_star_button(e->name, e->number));
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), calls_view_queue_button(e->name, e->number));
            GtkWidget *call = gtk_button_new_from_icon_name("call-start-symbolic");
            gtk_widget_add_css_class(call, "flat");
            gtk_widget_set_valign(call, GTK_ALIGN_CENTER);
            char label[300];
            snprintf(label, sizeof label, _("Call %s back"), e->name[0] ? e->name : num);
            a11y(call, label);
            gtk_widget_set_tooltip_text(call, label);
            g_object_set_data_full(G_OBJECT(call), "number", g_strdup(e->number), g_free);
            g_object_set_data_full(G_OBJECT(call), "name", g_strdup(e->name), g_free);
            g_signal_connect(call, "clicked", G_CALLBACK(on_log_call), ui);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), call);
        }
        gtk_list_box_append(GTK_LIST_BOX(ui->list), row);
    }
    gtk_label_set_text(GTK_LABEL(ui->status),
                       CALL_LOG->len ? _("Calls made and received through Phone Recorder, newest first.")
                                     : _("No calls yet."));
    gtk_widget_set_visible(ui->list, CALL_LOG->len > 0);
}

void dialog_call_log(void)
{
    log_ui_t *ui = g_new0(log_ui_t, 1);
    ui->dialog = adw_dialog_new();
    adw_dialog_set_title(ui->dialog, _("Call log"));
    adw_dialog_set_content_width(ui->dialog, 520);
    adw_dialog_set_content_height(ui->dialog, 640);
    g_object_set_data_full(G_OBJECT(ui->dialog), "ui", ui, g_free);

    GtkWidget *tv = adw_toolbar_view_new();
    GtkWidget *hb = adw_header_bar_new();
    GtkWidget *clear = gtk_button_new_with_mnemonic(_("C_lear"));
    gtk_widget_set_tooltip_text(clear, _("Delete the call log from this computer"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_log_clear), ui);
    adw_header_bar_pack_start(ADW_HEADER_BAR(hb), clear);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), hb);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 12);
    ui->status = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(ui->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui->status), 0);
    gtk_widget_add_css_class(ui->status, "dim-label");
    ui->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(ui->list, "boxed-list");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ui->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), ui->status);
    gtk_box_append(GTK_BOX(box), scroll);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), box);
    adw_dialog_set_child(ui->dialog, tv);
    log_fill(ui);
    adw_dialog_present(ui->dialog, GTK_WIDGET(APP->window));
}
