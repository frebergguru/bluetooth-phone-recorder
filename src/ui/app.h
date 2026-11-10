/*
 * Application state shared by the UI modules.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_APP_H
#define BPR_APP_H

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "audio/engine.h"
#include "bt/adapters.h"
#include "bt/bt_engine.h"
#include "obs/obs_ws.h"
#include "core/vcard.h"
#include "calldata.h"
#include "settings.h"

typedef enum {
    CALL_IDLE,
    CALL_INCOMING,
    CALL_OUTGOING,
    CALL_ACTIVE,
} call_state_t;

typedef struct strip strip_t; /* one mixer channel strip, mixer_view.c */

typedef struct {
    AdwApplication *gapp;
    GtkWindow *window;
    AdwToastOverlay *toasts;
    settings_t settings;
    engine_t *engine;
    obs_ws_t *obs;
    bt_adapter_t adapter;
    bool have_adapter;
    bool bt_started;
    bool restart_requested;
    char cli_adapter[320]; /* --adapter=... overrides settings */
    bool cli_trace;

    /* Bluetooth/phone state (UI thread copy) */
    bt_state_t bt_state;
    char bt_status[256];
    char controller_report[2048];
    char paired[16][18];
    int n_paired;
    bool phone_connected;
    char phone_addr[18];
    char phone_name[128];
    char operator_name[64];
    int battery, signal; /* 0..5, -1 unknown */
    int ind_call, ind_callsetup, ind_callheld;
    call_state_t call_state;
    gint64 call_started_us;
    bool call_timer_running; /* a call is up; stays true while a second call waits */
    char caller_number[64];
    char caller_name[128];
    char waiting_number[64]; /* second caller while a call is active */
    char waiting_name[128];
    /* call log bookkeeping for the call in progress */
    bool log_pending;
    call_dir_t log_dir;
    bool log_answered, log_declined;
    gint64 log_start_wall;  /* unix seconds */
    gint64 log_answer_us;   /* monotonic */
    char dial_number[64], dial_name[128]; /* what we dialled, for the log */
    guint dial_queue_id;    /* queue entry being called, 0 = none */
    gint64 dial_requested_us; /* monotonic time of the last app_dial() */
    contact_t *contacts; /* address book of the current phone, sorted by name */
    int n_contacts;
    bool audio_on_computer;
    char codec_name[16];
    int codec_rate;
    bool auto_recording; /* recording was started by auto-record */

    /* recording */
    bool recording;
    int marker_count; /* markers in the current recording */
    gint64 rec_started_us;

    /* mixer strips; index = mixer channel */
    strip_t *strips[MIXER_MAX_CHANNELS];
    GtkWidget *strip_box;

    /* widgets updated from state (window.c) */
    GtkWidget *title;          /* AdwWindowTitle */
    GtkWidget *record_btn, *record_label, *marker_btn;
    GtkWidget *phone_status_row, *pair_switch, *paired_group, *name_row;
    GtkWidget *paired_rows[16];
    GtkWidget *call_status, *caller_label, *call_timer, *codec_label, *phone_info;
    GtkWidget *answer_btn, *reject_btn, *hangup_btn, *audio_btn, *hold_btn, *mic_btn;
    GtkWidget *queue_group, *queue_rows[64], *fav_group, *fav_rows[64];
    GtkWidget *queue_number, *queue_name;
    GtkWidget *dial_entry, *dial_btn, *redial_btn, *keypad, *contacts_btn;
    GtkWidget *disk_label, *health_label, *session_label;
    GtkWidget *monitor_switch, *monitor_dropdown, *monitor_gain, *call_gain, *echo_switch;
    GtkWidget *meter_full_l, *meter_full_r, *meter_call, *meter_monitor;
    GtkCssProvider *scale_css;
} app_t;

extern app_t *APP;

/* app.c */
void app_toast(const char *text);
/* Tell screen readers (safe when no screen reader backend exists). */
void app_announce(const char *text, GtkAccessibleAnnouncementPriority prio);
void app_record_start(void);
void app_record_stop(void);
void app_add_marker(void);
void app_request_restart(void);
/* Validates, saves and applies the Bluetooth name. Returns false if invalid. */
gboolean app_set_bluetooth_name(const char *name);
void app_apply_text_scale(void);
void app_update_call_ui(void);
void app_update_phone_ui(void);
void app_save_mixer_settings(void);
/* Load the saved address book of a phone (addr NULL = clear). */
void app_load_contacts(const char *addr);
/* Start a call (never automatic). queue_id: the queue entry it comes from, or 0. */
void app_dial(const char *number, const char *name, guint queue_id);
/* Contact name for a number, or NULL. */
const char *app_contact_name(const char *number);
gboolean app_save_screenshot(const char *path);

/* docshots.c: BPR_DOC_SHOTS=<dir> renders the documentation pictures */
void docshots_start(const char *dir);
const char *app_adapter_status(void);

/* window.c */
void window_build(void);
void window_refresh_paired(void);
void window_refresh_devices(void);

/* mixer_view.c */
strip_t *strip_new(int channel);
void strip_free(strip_t *s);
void strip_update_meters(strip_t *s);
void strip_refresh_devices(strip_t *s);
void strip_toggle_send(int ordinal);  /* 1-based ordinal among input strips */
void strip_toggle_mute(int ordinal);
int mixer_view_add_input(const char *target, const char *label); /* channel or -1 */
GtkWidget *mixer_view_master_section(void);
GtkWidget *mixer_view_device_dropdown(void);
GtkWidget *strip_widget(strip_t *s);
void strip_save(strip_t *s, settings_input_t *out);
int strip_channel(strip_t *s);

/* dialogs.c */
void dialog_adapter_chooser(gboolean first_run);
void dialog_preferences(void);
void dialog_pair_confirm(const char *addr, int number);
void dialog_pin(const char *addr);
void dialog_about(void);
void dialog_shortcuts(void);
void dialog_adapter_report(void);
void dialog_contacts(void);
void dialog_call_log(void);

/* calls_view.c: favorites and call queue in the main window */
GtkWidget *calls_view_queue_group(void);
GtkWidget *calls_view_fav_group(void);
void calls_view_refresh(void);
/* Refresh only if the Call buttons must change state (phone or call changed). */
void calls_view_update_callable(void);
/* Buttons used in lists: star (favorite) and add-to-queue for a number. */
GtkWidget *calls_view_star_button(const char *name, const char *number);
GtkWidget *calls_view_queue_button(const char *name, const char *number);

#endif
