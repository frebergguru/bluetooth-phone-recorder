/*
 * Documentation screenshots.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BPR_DOC_SHOTS=<dir> fills the window with clearly fake demo data (an
 * example phone and caller), opens each dialog in turn and saves a PNG of
 * the window for docs/. Bluetooth is not started. Use together with
 * BPR_NO_DEVICE_LINKS=1 so no real sound device is opened, and render with
 * GTK's Broadway backend so nothing appears on the desktop:
 *     scripts/make-doc-screenshots.sh
 */
#include <math.h>

#include "app.h"
#include "core/log.h"

static char *shot_dir;
static int step;
static guint meter_source;
static double t;

#define DEMO_PHONE_ADDR "00:00:5E:00:53:01" /* documentation address range */

static void shot(const char *name)
{
    char *path = g_build_filename(shot_dir, name, NULL);
    if (app_save_screenshot(path))
        LOGI("docs", "saved %s", path);
    else
        LOGE("docs", "could not save %s", path);
    g_free(path);
}

static void close_dialog(void)
{
    AdwDialog *d = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(APP->window));
    if (d) adw_dialog_force_close(d);
}

static void store_peak(_Atomic float *m, double v)
{
    float cur = atomic_load(m);
    if ((float)v > cur) atomic_store(m, (float)v);
}

/* Speech-like fake levels so the meters show something meaningful. */
static gboolean fake_meters(gpointer d)
{
    (void)d;
    t += 0.033;
    mixer_t *m = engine_mixer(APP->engine);
    double caller = 0.18 + 0.10 * fabs(sin(t * 3.1)) * (sin(t * 0.7) > -0.3);
    double host = 0.22 + 0.12 * fabs(sin(t * 4.3 + 1.0));
    double guest = 0.15 + 0.08 * fabs(sin(t * 2.2 + 2.0));
    store_peak(&m->ch[ENGINE_CALLER_CH].meter_peak, caller);
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
        if (!APP->strips[c]) continue;
        store_peak(&m->ch[c].meter_peak, c == 1 ? host : guest * (c == 2 ? 1.0 : 0.5));
    }
    store_peak(&m->meter_full_l, 0.35 + 0.1 * fabs(sin(t * 3.7)));
    store_peak(&m->meter_full_r, 0.34 + 0.1 * fabs(sin(t * 3.3)));
    store_peak(&m->meter_call, 0.28 + 0.1 * fabs(sin(t * 4.1)));
    store_peak(&m->meter_monitor, 0.30 + 0.1 * fabs(sin(t * 3.5)));
    return G_SOURCE_CONTINUE;
}

static void fill_demo_state(void)
{
    snprintf(APP->bt_status, sizeof APP->bt_status, _("Ready, %s (%s)"), "TP-Link UB500",
             "00:00:5E:00:53:FF");
    APP->bt_state = BT_STATE_READY;
    settings_set_phone_name(DEMO_PHONE_ADDR, "Example Phone");
    settings_set_phone_name("00:00:5E:00:53:02", "Second Phone");
    APP->n_paired = 2;
    snprintf(APP->paired[0], 18, "%s", DEMO_PHONE_ADDR);
    snprintf(APP->paired[1], 18, "%s", "00:00:5E:00:53:02");
    APP->phone_connected = true;
    snprintf(APP->phone_addr, sizeof APP->phone_addr, "%s", DEMO_PHONE_ADDR);
    snprintf(APP->phone_name, sizeof APP->phone_name, "%s", "Example Phone");
    snprintf(APP->operator_name, sizeof APP->operator_name, "%s", "Example Mobile");
    APP->battery = 4;
    APP->signal = 5;
    APP->ind_call = 1;
    APP->call_state = CALL_ACTIVE;
    APP->call_timer_running = true;
    APP->call_started_us = g_get_monotonic_time() - (gint64)754 * G_USEC_PER_SEC;
    snprintf(APP->caller_name, sizeof APP->caller_name, "%s", "Alex (guest)");
    snprintf(APP->caller_number, sizeof APP->caller_number, "%s", "+47 123 45 678");
    APP->audio_on_computer = true;
    snprintf(APP->codec_name, sizeof APP->codec_name, "%s", "mSBC");
    APP->codec_rate = 16000;
    window_refresh_paired();
    app_update_phone_ui();

    app_record_start();
    APP->rec_started_us = g_get_monotonic_time() - (gint64)1012 * G_USEC_PER_SEC;
    app_update_call_ui();
}

static gboolean next_step(gpointer d)
{
    (void)d;
    AdwDialog *dlg;
    switch (step++) {
    case 0:
        shot("main-window.png");
        dialog_adapter_chooser(TRUE);
        break;
    case 1:
        shot("adapter-chooser.png");
        close_dialog();
        dialog_pair_confirm(DEMO_PHONE_ADDR, 482913);
        break;
    case 2:
        shot("pairing-code.png");
        close_dialog();
        dialog_pin(DEMO_PHONE_ADDR);
        break;
    case 3:
        shot("pairing-pin.png");
        close_dialog();
        dialog_preferences();
        break;
    case 4: /* Preferences: switch to a page, take the picture on the next step */
    case 5:
    case 6:
    case 7:
    case 8:
    case 9: {
        static const char *pages[] = {"recording", "audio", "bluetooth", "obs", "accessibility"};
        /* inside "case N", step == N + 1; case 4 shows the first page */
        int i = step - 6; /* page shown since the previous step, -1 = none */
        if (i >= 0) {
            char name[64];
            snprintf(name, sizeof name, "preferences-%s.png", pages[i]);
            shot(name);
        }
        dlg = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(APP->window));
        if (i + 1 < 5) {
            if (dlg) adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(dlg), pages[i + 1]);
        } else {
            close_dialog();
            dialog_shortcuts();
        }
        break;
    }
    case 10:
        shot("keyboard-shortcuts.png");
        close_dialog();
        dialog_about();
        break;
    case 11:
        shot("about.png");
        close_dialog();
        break;
    default:
        if (meter_source) g_source_remove(meter_source);
        app_record_stop();
        LOGI("docs", "all screenshots done");
        g_application_quit(G_APPLICATION(APP->gapp));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void docshots_start(const char *dir)
{
    shot_dir = g_strdup(dir);
    g_mkdir_with_parents(shot_dir, 0755);
    fill_demo_state();
    meter_source = g_timeout_add(33, fake_meters, NULL);
    /* one step every 2 s, so every view has settled before its picture */
    g_timeout_add(2000, next_step, NULL);
}
