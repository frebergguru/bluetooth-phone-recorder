/*
 * The mixer. Pure processing, no I/O, so it can be unit tested.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Buses produced every block:
 *   direct[ch]  Each channel after gain and clean-up (HPF, gate), BEFORE mute.
 *               Feeds the per-channel recording track and the per-channel
 *               virtual source for OBS. Never affected by mute, solo or
 *               "send to call".
 *   full L/R    All unmuted channels, panned. Recording mix + OBS "Full mix".
 *   call        Mix-minus sent to the phone: unmuted channels whose
 *               "send to call" switch is on. The caller is never included.
 *   monitor L/R Full mix, or only the soloed channels if any solo is on.
 *
 * The "send to call" switch ONLY changes the call bus. This separation is a
 * hard requirement and is covered by tests/test_mixer.c.
 */
#ifndef BPR_MIXER_H
#define BPR_MIXER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "dsp.h"

#define MIXER_MAX_CHANNELS 16
#define MIXER_MAX_BLOCK 4096

typedef enum { MIX_CH_CALLER, MIX_CH_INPUT } mixer_ch_kind_t;

typedef struct {
    /* Parameters: written by the UI thread, read by the audio thread. */
    _Atomic float gain_db;
    _Atomic float pan; /* -1 left .. +1 right */
    atomic_bool mute;
    atomic_bool solo;
    atomic_bool send_to_call;
    atomic_bool hpf;
    atomic_bool gate;
    _Atomic float gate_threshold_db;
    atomic_bool active; /* slot in use */

    /* Meters: written by the audio thread, read by the UI. Linear peak. */
    _Atomic float meter_peak;

    /* Audio-thread private state */
    mixer_ch_kind_t kind;
    float g_gain;               /* smoothed channel gain (linear) */
    float g_main, g_call, g_mon; /* smoothed 0..1 routing ramps */
    biquad_t hpf_state;
    bool hpf_was_on;
    gate_t gate_state;
    float last_gate_thr;
} mixer_channel_t;

typedef struct {
    float rate;
    mixer_channel_t ch[MIXER_MAX_CHANNELS];
    _Atomic float monitor_gain_db;
    _Atomic float call_gain_db;
    atomic_bool limit_full; /* peak limiter on full mix (default on) */

    /* bus meters, linear peak */
    _Atomic float meter_full_l, meter_full_r, meter_call, meter_monitor;

    /* private */
    limiter_t lim_full, lim_call;
    /* per-block scratch buffers (kept here, not on the real-time stack) */
    float s_tmp[MIXER_MAX_BLOCK], s_full_l[MIXER_MAX_BLOCK], s_full_r[MIXER_MAX_BLOCK];
    float s_call[MIXER_MAX_BLOCK], s_solo_l[MIXER_MAX_BLOCK], s_solo_r[MIXER_MAX_BLOCK];
    float g_monitor_s, g_call_s;
    float smooth; /* one-pole coefficient for gain ramps */
} mixer_t;

typedef struct {
    /* inputs: one mono buffer per channel (NULL = silence) */
    const float *in[MIXER_MAX_CHANNELS];
    /* outputs (any may be NULL) */
    float *direct[MIXER_MAX_CHANNELS];
    float *full_l, *full_r;
    float *call;
    float *monitor_l, *monitor_r;
} mixer_io_t;

void mixer_init(mixer_t *m, float rate);
/* Configure a channel slot. Resets its parameters to defaults. */
void mixer_channel_setup(mixer_t *m, int idx, mixer_ch_kind_t kind);
void mixer_channel_release(mixer_t *m, int idx);
void mixer_process(mixer_t *m, mixer_io_t *io, size_t n);

#endif
