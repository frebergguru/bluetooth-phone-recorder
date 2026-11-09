/*
 * bpr-bt-test: headless hardware test for the Bluetooth side.
 * Uses the same engine as the app, but opens NO real audio devices (no
 * inputs, no monitor; only the virtual sources exist). Turns on pairing,
 * accepts pairing requests (prints the code to compare with the phone),
 * logs every event and records the call (caller + full mix) to a folder.
 *
 * usage: bpr-bt-test <adapter-id> <recording-folder> [seconds] [trace.pklg]
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/engine.h"
#include "bt/adapters.h"
#include "bt/bt_engine.h"
#include "bt/usb_guard.h"
#include "core/log.h"

static atomic_bool quit;
static atomic_bool rec_start, rec_stop;
static atomic_bool want_discoverable;
static engine_t *eng;

static void on_sig(int s)
{
    (void)s;
    atomic_store(&quit, true);
}

static const char *names[] = {
    "STATE", "CONTROLLER_INFO", "PAIR_CONFIRM", "PIN_REQUEST", "PAIRED", "REMOTE_NAME",
    "PHONE_CONNECTED", "PHONE_DISCONNECTED", "INDICATOR", "RING", "CALLER_ID", "CALL_STATUS",
    "AUDIO_CONNECTED", "AUDIO_DISCONNECTED", "AUDIO_SILENT", "OPERATOR", "SPEAKER_GAIN",
    "PAIRED_LIST", "ERROR",
};

static void on_event(const bt_event_t *ev, void *ud)
{
    (void)ud;
    time_t t = time(NULL);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    printf("%s %-18s state=%d addr=%s num=%d v2=%d text=%s %s\n", ts, names[ev->type], ev->state,
           ev->addr, ev->number, ev->value2, ev->text, ev->text2);
    fflush(stdout);
    switch (ev->type) {
    case BT_EV_STATE:
        if (ev->state == BT_STATE_READY) atomic_store(&want_discoverable, true);
        if (ev->state == BT_STATE_FAILED) atomic_store(&quit, true);
        break;
    case BT_EV_PAIR_CONFIRM:
        printf("\n>>> PAIRING CODE %06d: accept on the phone if it shows the same code <<<\n\n",
               ev->number);
        fflush(stdout);
        bt_cmd_pair_reply(ev->addr, true);
        break;
    case BT_EV_PIN_REQUEST: bt_cmd_pin_reply(ev->addr, "0000"); break;
    case BT_EV_AUDIO_CONNECTED: atomic_store(&rec_start, true); break;
    case BT_EV_AUDIO_DISCONNECTED: atomic_store(&rec_stop, true); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <adapter-id> <recording-folder> [seconds] [trace]\n", argv[0]);
        return 2;
    }
    usb_guard_start();
    log_set_verbose(0);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    int seconds = argc > 3 ? atoi(argv[3]) : 600;

    bt_adapter_t a;
    if (!bt_adapter_from_id(argv[1], &a)) {
        fprintf(stderr, "adapter %s not found\n", argv[1]);
        return 1;
    }
    printf("adapter: %s (%s, %s)\n", a.name, bt_chip_family_name(a.family),
           bt_transport_name(a.transport));

    eng = engine_new(NULL);
    char err[256];
    if (!eng || engine_start(eng, err, sizeof err) != 0) {
        fprintf(stderr, "audio engine: %s\n", err);
        return 1;
    }
    /* deliberately no engine_add_input() and no engine_set_monitor() */

    bt_config_t cfg = {0};
    cfg.adapter = a;
    cfg.local_name = "Phone Recorder test";
    cfg.secure_connections = true;
    cfg.allow_wideband = true;
    cfg.allow_super_wideband = true;
    cfg.disable_phone_nrec = true;
    cfg.trace_path = argc > 4 ? argv[4] : NULL;
    cfg.link = engine_phone_link(eng);
    if (bt_engine_start(&cfg, on_event, NULL) != 0) {
        fprintf(stderr, "bt engine start failed\n");
        return 1;
    }

    bool recording = false;
    for (int i = 0; i < seconds * 10 && !atomic_load(&quit); i++) {
        usleep(100000);
        if (atomic_exchange(&want_discoverable, false)) {
            bt_cmd_set_discoverable(true);
            printf(">>> pairing mode ON: pair \"Phone Recorder test\" from the phone <<<\n");
            fflush(stdout);
        }
        if (atomic_exchange(&rec_start, false) && !recording) {
            if (engine_record_start(eng, argv[2], REC_FMT_WAV_FLOAT, err, sizeof err) == 0) {
                recording = true;
                printf("recording to %s\n", recorder_session_dir(engine_recorder(eng)));
            } else {
                printf("recording failed: %s\n", err);
            }
        }
        if (atomic_exchange(&rec_stop, false) && recording) {
            engine_record_stop(eng);
            recording = false;
            phone_link_t *l = engine_phone_link(eng);
            engine_stats_t st;
            engine_get_stats(eng, &st);
            printf("recording stopped: bad frames repaired %llu, tx underruns %llu, rx dropouts "
                   "%llu, drift rx %.0f ppm tx %.0f ppm\n",
                   (unsigned long long)atomic_load(&l->rx_bad_frames),
                   (unsigned long long)atomic_load(&l->tx_underruns),
                   (unsigned long long)st.phone_rx_dropouts, st.phone_rx_ppm, st.phone_tx_ppm);
        }
        fflush(stdout);
    }
    printf("shutting down\n");
    if (recording) engine_record_stop(eng);
    bt_engine_stop();
    engine_free(eng);
    printf("done\n");
    return 0;
}
