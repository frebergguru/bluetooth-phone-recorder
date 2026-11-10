/*
 * Mixer view: one strip per channel plus the master section.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * "In call" switch: decides only whether the input is heard by the caller.
 * The input always stays available to OBS (its own virtual source), to the
 * recording and to the monitor. The strip says so in its tooltip.
 */
#include <math.h>
#include <string.h>

#include "app.h"

struct strip {
    int ch;
    GtkWidget *root;
    GtkWidget *name;        /* GtkEditableLabel (inputs) or GtkLabel */
    GtkWidget *device;      /* GtkDropDown, inputs only */
    GtkWidget *meter;       /* GtkLevelBar */
    GtkWidget *meter_label; /* numeric dB */
    GtkWidget *gain;        /* GtkScale */
    GtkWidget *pan;
    GtkWidget *mute, *solo, *send, *hpf, *gate;
    GtkWidget *remove;                /* inputs only */
    GtkWidget *send_icon, *send_text; /* content of the "In call" button */
    char device_names[64][256]; /* node names matching dropdown rows (row 0 = default) */
    int n_devices;
    bool updating;
    double meter_value;
    gint64 clip_until;
};

static mixer_channel_t *mch(strip_t *s)
{
    return &engine_mixer(APP->engine)->ch[s->ch];
}

static void a11y_label(GtkWidget *w, const char *label)
{
    /* A button's visible text is linked as "labelled-by", which screen readers
     * prefer over an explicit label. Drop it so the fuller label is read. */
    gtk_accessible_reset_relation(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_RELATION_LABELLED_BY);
    gtk_accessible_update_property(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static const char *strip_name(strip_t *s)
{
    if (s->ch == ENGINE_CALLER_CH) return _("Caller");
    return engine_input_label(APP->engine, s->ch);
}

static void update_send_button(strip_t *s)
{
    if (!s->send) return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->send));
    /* state as text and icon, never colour alone */
    gtk_label_set_text(GTK_LABEL(s->send_text), on ? _("In call") : _("Not in call"));
    gtk_image_set_from_icon_name(GTK_IMAGE(s->send_icon),
                                 on ? "call-start-symbolic" : "call-stop-symbolic");
    /* screen readers get the full meaning plus the pressed state */
    char a11y[256];
    snprintf(a11y, sizeof a11y, _("Send %s to the caller"), strip_name(s));
    a11y_label(s->send, a11y);
}

static void update_a11y_labels(strip_t *s)
{
    char buf[256];
    const char *n = strip_name(s);
    snprintf(buf, sizeof buf, _("Volume for %s"), n);
    a11y_label(s->gain, buf);
    snprintf(buf, sizeof buf, _("Left-right position for %s"), n);
    a11y_label(s->pan, buf);
    snprintf(buf, sizeof buf, _("Mute %s"), n);
    a11y_label(s->mute, buf);
    snprintf(buf, sizeof buf, _("Solo %s on the monitor"), n);
    a11y_label(s->solo, buf);
    snprintf(buf, sizeof buf, _("Level of %s"), n);
    a11y_label(s->meter, buf);
    if (s->hpf) {
        snprintf(buf, sizeof buf, _("Low cut filter for %s"), n);
        a11y_label(s->hpf, buf);
    }
    if (s->gate) {
        snprintf(buf, sizeof buf, _("Noise gate for %s"), n);
        a11y_label(s->gate, buf);
    }
    if (s->device) {
        snprintf(buf, sizeof buf, _("Input device for %s"), n);
        a11y_label(s->device, buf);
    }
    if (s->remove) {
        /* every strip has a "Remove" button: say which input it removes */
        snprintf(buf, sizeof buf, _("Remove %s"), n);
        a11y_label(s->remove, buf);
    }
    update_send_button(s);
}

/* ---- callbacks ---- */

static void on_gain(GtkRange *r, strip_t *s)
{
    atomic_store(&mch(s)->gain_db, (float)gtk_range_get_value(r));
}

static void on_pan(GtkRange *r, strip_t *s)
{
    atomic_store(&mch(s)->pan, (float)gtk_range_get_value(r));
}

static void on_toggle(GtkToggleButton *b, strip_t *s)
{
    if (s->updating) return;
    gboolean on = gtk_toggle_button_get_active(b);
    mixer_channel_t *c = mch(s);
    if (GTK_WIDGET(b) == s->mute)
        atomic_store(&c->mute, on);
    else if (GTK_WIDGET(b) == s->solo)
        atomic_store(&c->solo, on);
    else if (GTK_WIDGET(b) == s->send) {
        atomic_store(&c->send_to_call, on);
        update_send_button(s);
        char msg[256];
        snprintf(msg, sizeof msg, on ? _("%s is now heard by the caller")
                                     : _("%s is no longer heard by the caller (still recorded and "
                                         "available to OBS)"),
                 strip_name(s));
        app_announce(msg,
                                GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    } else if (GTK_WIDGET(b) == s->hpf)
        atomic_store(&c->hpf, on);
    else if (GTK_WIDGET(b) == s->gate)
        atomic_store(&c->gate, on);
    app_save_mixer_settings();
}

static void on_name_changed(GtkEditable *e, GParamSpec *ps, strip_t *s)
{
    (void)ps;
    if (gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(e))) return;
    const char *t = gtk_editable_get_text(e);
    if (!t || !*t) return;
    engine_set_input_label(APP->engine, s->ch, t);
    update_a11y_labels(s);
    app_save_mixer_settings();
}

static void on_device(GtkDropDown *dd, GParamSpec *ps, strip_t *s)
{
    (void)ps;
    if (s->updating) return;
    guint sel = gtk_drop_down_get_selected(dd);
    if (sel == GTK_INVALID_LIST_POSITION || (int)sel >= s->n_devices) return;
    const char *target = s->device_names[sel];
    if (engine_set_input_target(APP->engine, s->ch, target) != 0)
        app_toast(_("Could not open that input"));
    app_save_mixer_settings();
}

static void remove_response(AdwAlertDialog *d, const char *response, strip_t *s)
{
    (void)d;
    if (strcmp(response, "remove") != 0) return;
    int ch = s->ch;
    gtk_box_remove(GTK_BOX(APP->strip_box), s->root);
    APP->strips[ch] = NULL;
    strip_free(s);
    engine_remove_input(APP->engine, ch);
    app_save_mixer_settings();
}

static void on_remove(GtkButton *b, strip_t *s)
{
    (void)b;
    if (APP->recording) {
        app_toast(_("Inputs cannot be removed while recording"));
        return;
    }
    AdwDialog *dlg = adw_alert_dialog_new(_("Remove this input?"), NULL);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dlg), _("%s will be removed from the mixer."),
                                 strip_name(s));
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "cancel", _("_Cancel"), "remove",
                                   _("_Remove"), NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    g_signal_connect(dlg, "response", G_CALLBACK(remove_response), s);
    adw_dialog_present(dlg, GTK_WIDGET(APP->window));
}

static gchar *format_db(GtkScale *scale, double v, gpointer d)
{
    (void)scale;
    (void)d;
    return g_strdup_printf("%+.1f dB", v);
}

/* ---- construction ---- */

static GtkWidget *toggle(const char *label, const char *tooltip, gboolean active, strip_t *s)
{
    GtkWidget *b = gtk_toggle_button_new_with_mnemonic(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), active);
    g_signal_connect(b, "toggled", G_CALLBACK(on_toggle), s);
    return b;
}

strip_t *strip_new(int channel)
{
    strip_t *s = g_new0(strip_t, 1);
    s->ch = channel;
    mixer_channel_t *c = mch(s);
    bool input = channel != ENGINE_CALLER_CH;

    s->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(s->root, "card");
    gtk_widget_set_size_request(s->root, 170, -1);
    GtkWidget *in = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(in, 10);
    gtk_widget_set_margin_end(in, 10);
    gtk_widget_set_margin_top(in, 10);
    gtk_widget_set_margin_bottom(in, 10);
    gtk_widget_set_vexpand(in, TRUE);
    gtk_box_append(GTK_BOX(s->root), in);

    if (input) {
        s->name = gtk_editable_label_new(engine_input_label(APP->engine, channel));
        gtk_widget_set_tooltip_text(s->name, _("Click to rename. The name is used for the "
                                                "recording file and the OBS source."));
        g_signal_connect(s->name, "notify::editing", G_CALLBACK(on_name_changed), s);
        gtk_accessible_update_property(GTK_ACCESSIBLE(s->name), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       _("Name of this input"), -1);
    } else {
        s->name = gtk_label_new(_("Caller"));
        gtk_widget_set_tooltip_text(s->name, _("The voice of the person on the phone"));
    }
    gtk_widget_add_css_class(s->name, "heading");
    gtk_box_append(GTK_BOX(in), s->name);

    if (input) {
        s->device = gtk_drop_down_new(NULL, NULL);
        gtk_widget_set_tooltip_text(s->device, _("Which microphone or input this channel uses"));
        g_signal_connect(s->device, "notify::selected", G_CALLBACK(on_device), s);
        gtk_box_append(GTK_BOX(in), s->device);
    } else {
        GtkWidget *l = gtk_label_new(_("From the phone"));
        gtk_widget_add_css_class(l, "dim-label");
        gtk_box_append(GTK_BOX(in), l);
    }

    /* meter + gain side by side */
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_vexpand(mid, TRUE);
    gtk_widget_set_halign(mid, GTK_ALIGN_CENTER);
    s->meter = gtk_level_bar_new_for_interval(0.0, 1.0);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(s->meter), GTK_ORIENTATION_VERTICAL);
    gtk_level_bar_set_inverted(GTK_LEVEL_BAR(s->meter), TRUE);
    /* offsets: -18 dBFS and -6 dBFS (meter covers -60..0 dB) */
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(s->meter), GTK_LEVEL_BAR_OFFSET_LOW, 0.70);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(s->meter), GTK_LEVEL_BAR_OFFSET_HIGH, 0.90);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(s->meter), GTK_LEVEL_BAR_OFFSET_FULL, 1.0);
    gtk_widget_set_size_request(s->meter, 14, 200);
    gtk_widget_set_vexpand(s->meter, TRUE);

    s->gain = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -30.0, 24.0, 0.5);
    gtk_range_set_inverted(GTK_RANGE(s->gain), TRUE);
    gtk_range_set_value(GTK_RANGE(s->gain), atomic_load(&c->gain_db));
    gtk_scale_set_draw_value(GTK_SCALE(s->gain), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(s->gain), GTK_POS_BOTTOM);
    gtk_scale_set_format_value_func(GTK_SCALE(s->gain), format_db, NULL, NULL);
    gtk_scale_add_mark(GTK_SCALE(s->gain), 0.0, GTK_POS_RIGHT, "0");
    gtk_widget_set_vexpand(s->gain, TRUE);
    gtk_widget_set_tooltip_text(s->gain, _("Volume (gain) of this channel"));
    g_signal_connect(s->gain, "value-changed", G_CALLBACK(on_gain), s);
    gtk_box_append(GTK_BOX(mid), s->meter);
    gtk_box_append(GTK_BOX(mid), s->gain);
    gtk_box_append(GTK_BOX(in), mid);

    s->meter_label = gtk_label_new("-inf dB");
    gtk_widget_add_css_class(s->meter_label, "numeric");
    gtk_widget_set_tooltip_text(s->meter_label, _("Peak level. Aim for about -12 dB when speaking; "
                                                  "\"CLIP\" means it is too loud."));
    gtk_box_append(GTK_BOX(in), s->meter_label);

    s->pan = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -1.0, 1.0, 0.05);
    gtk_range_set_value(GTK_RANGE(s->pan), atomic_load(&c->pan));
    gtk_scale_add_mark(GTK_SCALE(s->pan), 0.0, GTK_POS_BOTTOM, NULL);
    gtk_widget_set_tooltip_text(s->pan, _("Left-right position in the stereo mix"));
    g_signal_connect(s->pan, "value-changed", G_CALLBACK(on_pan), s);
    gtk_box_append(GTK_BOX(in), s->pan);

    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_set_homogeneous(GTK_BOX(row1), TRUE);
    s->mute = toggle(_("_Mute"), _("Take this channel out of the mix and the call. Its own OBS "
                                   "source and recording track keep the sound."),
                     atomic_load(&c->mute), s);
    s->solo = toggle(_("_Solo"), _("Listen only to soloed channels on the monitor. Does not "
                                   "change the recording, the call or OBS."),
                     atomic_load(&c->solo), s);
    gtk_box_append(GTK_BOX(row1), s->mute);
    gtk_box_append(GTK_BOX(row1), s->solo);
    gtk_box_append(GTK_BOX(in), row1);

    if (input) {
        s->send = gtk_toggle_button_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
        s->send_icon = gtk_image_new();
        s->send_text = gtk_label_new(NULL);
        gtk_accessible_update_state(GTK_ACCESSIBLE(s->send_icon), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
        gtk_accessible_update_state(GTK_ACCESSIBLE(s->send_text), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
        gtk_box_append(GTK_BOX(content), s->send_icon);
        gtk_box_append(GTK_BOX(content), s->send_text);
        gtk_button_set_child(GTK_BUTTON(s->send), content);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->send), atomic_load(&c->send_to_call));
        gtk_widget_set_tooltip_text(s->send,
                                    _("Whether the caller hears this input. Turning it off "
                                      "never removes it from OBS or the recording."));
        g_signal_connect(s->send, "toggled", G_CALLBACK(on_toggle), s);
        gtk_box_append(GTK_BOX(in), s->send);

        GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_box_set_homogeneous(GTK_BOX(row2), TRUE);
        s->hpf = toggle(_("Low cut"), _("Remove rumble and handling noise below 80 Hz"),
                        atomic_load(&c->hpf), s);
        s->gate = toggle(_("Gate"), _("Turn the channel down when nobody speaks into it"),
                         atomic_load(&c->gate), s);
        gtk_box_append(GTK_BOX(row2), s->hpf);
        gtk_box_append(GTK_BOX(row2), s->gate);
        gtk_box_append(GTK_BOX(in), row2);

        s->remove = gtk_button_new_with_mnemonic(_("Remo_ve"));
        gtk_widget_add_css_class(s->remove, "flat");
        g_signal_connect(s->remove, "clicked", G_CALLBACK(on_remove), s);
        gtk_box_append(GTK_BOX(in), s->remove);
    } else {
        GtkWidget *note = gtk_label_new(_("Never sent back to the caller"));
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_widget_add_css_class(note, "dim-label");
        gtk_box_append(GTK_BOX(in), note);
    }

    update_a11y_labels(s);
    if (input) strip_refresh_devices(s);
    return s;
}

void strip_free(strip_t *s)
{
    g_free(s);
}

GtkWidget *strip_widget(strip_t *s)
{
    return s->root;
}

int strip_channel(strip_t *s)
{
    return s->ch;
}

void strip_update_meters(strip_t *s)
{
    float peak = atomic_exchange(&mch(s)->meter_peak, 0.0f);
    double db = peak > 1e-6f ? 20.0 * log10(peak) : -120.0;
    double v = (CLAMP(db, -60.0, 0.0) + 60.0) / 60.0;
    s->meter_value = v > s->meter_value ? v : s->meter_value - 0.035;
    if (s->meter_value < 0) s->meter_value = 0;
    gtk_level_bar_set_value(GTK_LEVEL_BAR(s->meter), s->meter_value);
    gint64 now = g_get_monotonic_time();
    if (peak >= 0.999f) s->clip_until = now + 2 * G_USEC_PER_SEC;
    char buf[32];
    if (now < s->clip_until)
        snprintf(buf, sizeof buf, "%s", _("CLIP"));
    else if (db <= -60.0)
        snprintf(buf, sizeof buf, "-inf dB");
    else
        snprintf(buf, sizeof buf, "%.0f dB", db);
    if (strcmp(gtk_label_get_text(GTK_LABEL(s->meter_label)), buf) != 0)
        gtk_label_set_text(GTK_LABEL(s->meter_label), buf);
}

void strip_refresh_devices(strip_t *s)
{
    if (!s->device) return;
    engine_device_t devs[63];
    int n = engine_list_devices(APP->engine, true, devs, 63);
    const char *current = engine_input_target(APP->engine, s->ch);
    GtkStringList *list = gtk_string_list_new(NULL);
    gtk_string_list_append(list, _("System default input"));
    s->device_names[0][0] = 0;
    s->n_devices = 1;
    guint sel = 0;
    for (int i = 0; i < n && s->n_devices < 64; i++) {
        gtk_string_list_append(list, devs[i].description);
        snprintf(s->device_names[s->n_devices], sizeof s->device_names[0], "%s", devs[i].name);
        if (current[0] && strcmp(current, devs[i].name) == 0) sel = (guint)s->n_devices;
        s->n_devices++;
    }
    s->updating = true;
    gtk_drop_down_set_model(GTK_DROP_DOWN(s->device), G_LIST_MODEL(list));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->device), sel);
    s->updating = false;
    g_object_unref(list);
}

void strip_save(strip_t *s, settings_input_t *out)
{
    mixer_channel_t *c = mch(s);
    snprintf(out->label, sizeof out->label, "%s", engine_input_label(APP->engine, s->ch));
    /* The device actually in use, not the list row: while a chosen
     * microphone is unplugged the list shows "System default input", and
     * saving that would forget the user's choice. */
    snprintf(out->target, sizeof out->target, "%s", engine_input_target(APP->engine, s->ch));
    out->gain_db = atomic_load(&c->gain_db);
    out->pan = atomic_load(&c->pan);
    out->send_to_call = atomic_load(&c->send_to_call);
    out->mute = atomic_load(&c->mute);
    out->hpf = atomic_load(&c->hpf);
    out->gate = atomic_load(&c->gate);
    out->gate_threshold_db = atomic_load(&c->gate_threshold_db);
}

static strip_t *nth_input(int ordinal)
{
    int n = 0;
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
        if (!APP->strips[c]) continue;
        if (++n == ordinal) return APP->strips[c];
    }
    return NULL;
}

void strip_toggle_send(int ordinal)
{
    strip_t *s = nth_input(ordinal);
    if (s && s->send)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->send),
                                     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->send)));
}

void strip_toggle_mute(int ordinal)
{
    strip_t *s = nth_input(ordinal);
    if (s)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->mute),
                                     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->mute)));
}

int mixer_view_add_input(const char *target, const char *label)
{
    if (APP->recording) {
        app_toast(_("Inputs cannot be added while recording"));
        return -1;
    }
    int ch = engine_add_input(APP->engine, target, label);
    if (ch < 0) {
        app_toast(_("No more inputs can be added"));
        return -1;
    }
    APP->strips[ch] = strip_new(ch);
    gtk_box_append(GTK_BOX(APP->strip_box), strip_widget(APP->strips[ch]));
    return ch;
}

/* ---- master section ---- */

static void on_monitor_switch(GObject *sw, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    gboolean on = gtk_switch_get_active(GTK_SWITCH(sw));
    APP->settings.monitor_enabled = on;
    if (engine_set_monitor(APP->engine, on, APP->settings.monitor_target) != 0)
        app_toast(_("The monitor output could not be opened. Choose another output."));
    settings_save(&APP->settings);
}

static void on_monitor_device(GtkDropDown *dd, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    if (g_object_get_data(G_OBJECT(dd), "updating")) return;
    guint sel = gtk_drop_down_get_selected(dd);
    /* node names stored with the list rows (window_refresh_devices): a fresh
     * device list could differ from the rows if a device just came or went */
    char **names = g_object_get_data(G_OBJECT(dd), "names");
    guint n = names ? g_strv_length(names) : 0;
    if (sel == 0 || sel == GTK_INVALID_LIST_POSITION || sel >= n)
        APP->settings.monitor_target[0] = 0;
    else
        snprintf(APP->settings.monitor_target, sizeof APP->settings.monitor_target, "%s",
                 names[sel]);
    if (APP->settings.monitor_enabled)
        if (engine_set_monitor(APP->engine, true, APP->settings.monitor_target) != 0)
            app_toast(_("The monitor output could not be opened. Choose another output."));
    settings_save(&APP->settings);
}

static void on_master_gain(GtkRange *r, gpointer which)
{
    mixer_t *m = engine_mixer(APP->engine);
    float v = (float)gtk_range_get_value(r);
    if (GPOINTER_TO_INT(which) == 0)
        atomic_store(&m->monitor_gain_db, v);
    else
        atomic_store(&m->call_gain_db, v);
}

static void on_echo(GObject *sw, GParamSpec *ps, gpointer d)
{
    (void)ps;
    (void)d;
    APP->settings.echo_cancel = gtk_switch_get_active(GTK_SWITCH(sw));
    engine_set_echo_cancel(APP->engine, APP->settings.echo_cancel);
    settings_save(&APP->settings);
}

static GtkWidget *labeled_meter(const char *label, GtkWidget **meter)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "caption");
    *meter = gtk_level_bar_new_for_interval(0.0, 1.0);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(*meter), GTK_LEVEL_BAR_OFFSET_LOW, 0.70);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(*meter), GTK_LEVEL_BAR_OFFSET_HIGH, 0.90);
    a11y_label(*meter, label);
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), *meter);
    return box;
}

GtkWidget *mixer_view_master_section(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(box, 260, -1);
    mixer_t *m = engine_mixer(APP->engine);

    GtkWidget *h = gtk_label_new(_("Outputs"));
    gtk_widget_add_css_class(h, "heading");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_box_append(GTK_BOX(box), h);

    gtk_box_append(GTK_BOX(box), labeled_meter(_("Full mix, left"), &APP->meter_full_l));
    gtk_box_append(GTK_BOX(box), labeled_meter(_("Full mix, right"), &APP->meter_full_r));
    gtk_box_append(GTK_BOX(box), labeled_meter(_("Sent to caller"), &APP->meter_call));
    gtk_box_append(GTK_BOX(box), labeled_meter(_("Monitor"), &APP->meter_monitor));

    /* monitor */
    GtkWidget *mon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ml = gtk_label_new_with_mnemonic(_("_Monitor (headphones)"));
    gtk_widget_set_hexpand(ml, TRUE);
    gtk_label_set_xalign(GTK_LABEL(ml), 0);
    APP->monitor_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(APP->monitor_switch), APP->settings.monitor_enabled);
    gtk_label_set_mnemonic_widget(GTK_LABEL(ml), APP->monitor_switch);
    a11y_label(APP->monitor_switch, _("Monitor on headphones"));
    g_signal_connect(APP->monitor_switch, "notify::active", G_CALLBACK(on_monitor_switch), NULL);
    gtk_box_append(GTK_BOX(mon_row), ml);
    gtk_box_append(GTK_BOX(mon_row), APP->monitor_switch);
    gtk_box_append(GTK_BOX(box), mon_row);

    APP->monitor_dropdown = gtk_drop_down_new(NULL, NULL);
    a11y_label(APP->monitor_dropdown, _("Monitor output device"));
    g_signal_connect(APP->monitor_dropdown, "notify::selected", G_CALLBACK(on_monitor_device), NULL);
    gtk_box_append(GTK_BOX(box), APP->monitor_dropdown);

    GtkWidget *mg_l = gtk_label_new(_("Monitor volume"));
    gtk_label_set_xalign(GTK_LABEL(mg_l), 0);
    APP->monitor_gain = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -40.0, 12.0, 0.5);
    gtk_range_set_value(GTK_RANGE(APP->monitor_gain), atomic_load(&m->monitor_gain_db));
    gtk_scale_set_draw_value(GTK_SCALE(APP->monitor_gain), TRUE);
    gtk_scale_set_format_value_func(GTK_SCALE(APP->monitor_gain), format_db, NULL, NULL);
    a11y_label(APP->monitor_gain, _("Monitor volume"));
    g_signal_connect(APP->monitor_gain, "value-changed", G_CALLBACK(on_master_gain), GINT_TO_POINTER(0));
    gtk_box_append(GTK_BOX(box), mg_l);
    gtk_box_append(GTK_BOX(box), APP->monitor_gain);

    GtkWidget *cg_l = gtk_label_new(_("Level sent to the caller"));
    gtk_label_set_xalign(GTK_LABEL(cg_l), 0);
    APP->call_gain = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -24.0, 12.0, 0.5);
    gtk_range_set_value(GTK_RANGE(APP->call_gain), atomic_load(&m->call_gain_db));
    gtk_scale_set_draw_value(GTK_SCALE(APP->call_gain), TRUE);
    gtk_scale_set_format_value_func(GTK_SCALE(APP->call_gain), format_db, NULL, NULL);
    a11y_label(APP->call_gain, _("Level sent to the caller"));
    g_signal_connect(APP->call_gain, "value-changed", G_CALLBACK(on_master_gain), GINT_TO_POINTER(1));
    gtk_box_append(GTK_BOX(box), cg_l);
    gtk_box_append(GTK_BOX(box), APP->call_gain);

    GtkWidget *ec_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *el = gtk_label_new_with_mnemonic(_("_Echo cancelling"));
    gtk_widget_set_hexpand(el, TRUE);
    gtk_label_set_xalign(GTK_LABEL(el), 0);
    gtk_widget_set_tooltip_text(el, _("Only needed when the caller is played on speakers. "
                                      "Headphones give the best sound."));
    APP->echo_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(APP->echo_switch), APP->settings.echo_cancel);
    gtk_label_set_mnemonic_widget(GTK_LABEL(el), APP->echo_switch);
    a11y_label(APP->echo_switch, _("Echo cancelling for speakers"));
    g_signal_connect(APP->echo_switch, "notify::active", G_CALLBACK(on_echo), NULL);
    gtk_box_append(GTK_BOX(ec_row), el);
    gtk_box_append(GTK_BOX(ec_row), APP->echo_switch);
    gtk_box_append(GTK_BOX(box), ec_row);

    GtkWidget *obs_note = gtk_label_new(
        _("In OBS, add \"Audio Input Capture\" and pick a \"Phone Recorder\" source: Full mix, "
          "Caller, Mix-minus, or any single input."));
    gtk_label_set_wrap(GTK_LABEL(obs_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(obs_note), 0);
    gtk_widget_add_css_class(obs_note, "dim-label");
    gtk_box_append(GTK_BOX(box), obs_note);

    window_refresh_devices();
    return box;
}
