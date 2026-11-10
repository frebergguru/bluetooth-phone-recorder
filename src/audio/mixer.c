/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "mixer.h"

#include <math.h>
#include <string.h>

static void meter_update(_Atomic float *meter, float peak)
{
    /* Single writer: keep the maximum until the UI resets it to 0. */
    float cur = atomic_load_explicit(meter, memory_order_relaxed);
    if (peak > cur) atomic_store_explicit(meter, peak, memory_order_relaxed);
}

void mixer_init(mixer_t *m, float rate)
{
    memset(m, 0, sizeof *m);
    m->rate = rate;
    /* ~10 ms ramps: no clicks when a switch is flipped */
    m->smooth = 1.0f - expf(-1.0f / (0.010f * rate));
    atomic_store(&m->monitor_gain_db, 0.0f);
    atomic_store(&m->call_gain_db, 0.0f);
    atomic_store(&m->limit_full, true);
    atomic_store(&m->call_mute, false);
    limiter_init(&m->lim_full, rate, -1.0f);
    limiter_init(&m->lim_call, rate, -1.0f);
    m->g_monitor_s = 1.0f;
    m->g_call_s = 1.0f;
    for (int i = 0; i < MIXER_MAX_CHANNELS; i++) atomic_store(&m->ch[i].active, false);
}

void mixer_channel_setup(mixer_t *m, int idx, mixer_ch_kind_t kind)
{
    if (idx < 0 || idx >= MIXER_MAX_CHANNELS) return;
    mixer_channel_t *c = &m->ch[idx];
    atomic_store(&c->active, false);
    c->kind = kind;
    atomic_store(&c->gain_db, 0.0f);
    atomic_store(&c->pan, 0.0f);
    atomic_store(&c->mute, false);
    atomic_store(&c->solo, false);
    /* Inputs go to the call by default; the caller never does. */
    atomic_store(&c->send_to_call, kind == MIX_CH_INPUT);
    atomic_store(&c->hpf, kind == MIX_CH_INPUT);
    atomic_store(&c->gate, false);
    atomic_store(&c->gate_threshold_db, -50.0f);
    atomic_store(&c->meter_peak, 0.0f);
    c->g_gain = 1.0f;
    c->g_main = 1.0f;
    c->g_call = kind == MIX_CH_INPUT ? 1.0f : 0.0f;
    c->g_mon = 0.0f;
    biquad_highpass(&c->hpf_state, m->rate, 80.0f, 0.707f);
    c->hpf_was_on = false;
    gate_init(&c->gate_state, m->rate, -50.0f);
    c->last_gate_thr = -50.0f;
    atomic_store(&c->active, true);
}

void mixer_channel_release(mixer_t *m, int idx)
{
    if (idx < 0 || idx >= MIXER_MAX_CHANNELS) return;
    atomic_store(&m->ch[idx].active, false);
}

static inline void ramp_step(float *g, float target, float k)
{
    *g += k * (target - *g);
    if (fabsf(*g - target) < 1e-6f) *g = target;
}

void mixer_process(mixer_t *m, mixer_io_t *io, size_t n)
{
    if (n > MIXER_MAX_BLOCK) n = MIXER_MAX_BLOCK;
    float *tmp = m->s_tmp;
    float *full_l = m->s_full_l, *full_r = m->s_full_r;
    float *call = m->s_call;
    float *solo_l = m->s_solo_l, *solo_r = m->s_solo_r;
    memset(full_l, 0, n * sizeof(float));
    memset(full_r, 0, n * sizeof(float));
    memset(call, 0, n * sizeof(float));
    memset(solo_l, 0, n * sizeof(float));
    memset(solo_r, 0, n * sizeof(float));

    const float k = m->smooth;
    bool any_solo = false;
    for (int c = 0; c < MIXER_MAX_CHANNELS; c++) {
        mixer_channel_t *ch = &m->ch[c];
        if (atomic_load_explicit(&ch->active, memory_order_acquire) &&
            atomic_load_explicit(&ch->solo, memory_order_relaxed))
            any_solo = true;
    }

    for (int c = 0; c < MIXER_MAX_CHANNELS; c++) {
        mixer_channel_t *ch = &m->ch[c];
        if (!atomic_load_explicit(&ch->active, memory_order_acquire)) {
            if (io->direct[c]) memset(io->direct[c], 0, n * sizeof(float));
            continue;
        }
        if (io->in[c])
            memcpy(tmp, io->in[c], n * sizeof(float));
        else
            memset(tmp, 0, n * sizeof(float));

        /* clean-up stage */
        bool hpf_on = atomic_load_explicit(&ch->hpf, memory_order_relaxed);
        if (hpf_on) {
            if (!ch->hpf_was_on) biquad_reset(&ch->hpf_state);
            biquad_process(&ch->hpf_state, tmp, n);
        }
        ch->hpf_was_on = hpf_on;
        if (atomic_load_explicit(&ch->gate, memory_order_relaxed)) {
            float thr = atomic_load_explicit(&ch->gate_threshold_db, memory_order_relaxed);
            if (thr != ch->last_gate_thr) {
                gate_set_threshold(&ch->gate_state, thr);
                ch->last_gate_thr = thr;
            }
            gate_process(&ch->gate_state, tmp, n);
        }

        /* targets */
        float gain_t = db_to_lin(atomic_load_explicit(&ch->gain_db, memory_order_relaxed));
        bool muted = atomic_load_explicit(&ch->mute, memory_order_relaxed);
        float main_t = muted ? 0.0f : 1.0f;
        bool to_call = ch->kind == MIX_CH_INPUT &&
                       atomic_load_explicit(&ch->send_to_call, memory_order_relaxed) && !muted;
        float call_t = to_call ? 1.0f : 0.0f;
        float mon_t = atomic_load_explicit(&ch->solo, memory_order_relaxed) ? 1.0f : 0.0f;
        float pan = atomic_load_explicit(&ch->pan, memory_order_relaxed);
        if (pan < -1.0f) pan = -1.0f;
        if (pan > 1.0f) pan = 1.0f;
        /* constant-power pan, centre = -3 dB per side, normalised so centre is unity
         * in each side (speech podcasts expect a centred voice at full level) */
        float angle = (pan + 1.0f) * (float)M_PI / 4.0f;
        float pl = cosf(angle) * (float)M_SQRT2;
        float pr = sinf(angle) * (float)M_SQRT2;

        float peak = 0.0f;
        float *direct = io->direct[c];
        for (size_t i = 0; i < n; i++) {
            ramp_step(&ch->g_gain, gain_t, k);
            ramp_step(&ch->g_main, main_t, k);
            ramp_step(&ch->g_call, call_t, k);
            ramp_step(&ch->g_mon, mon_t, k);
            float d = tmp[i] * ch->g_gain;
            float ad = fabsf(d);
            if (ad > peak) peak = ad;
            if (direct) direct[i] = d;
            float mv = d * ch->g_main;
            full_l[i] += mv * pl;
            full_r[i] += mv * pr;
            call[i] += d * ch->g_call;
            float sv = d * ch->g_mon;
            solo_l[i] += sv * pl;
            solo_r[i] += sv * pr;
        }
        meter_update(&ch->meter_peak, peak);
    }

    /* full mix */
    if (atomic_load_explicit(&m->limit_full, memory_order_relaxed))
        limiter_process(&m->lim_full, full_l, full_r, n);
    meter_update(&m->meter_full_l, dsp_peak(full_l, n));
    meter_update(&m->meter_full_r, dsp_peak(full_r, n));
    if (io->full_l) memcpy(io->full_l, full_l, n * sizeof(float));
    if (io->full_r) memcpy(io->full_r, full_r, n * sizeof(float));

    /* call bus (mix-minus) */
    /* "microphones off for the call" only silences this bus: direct outputs,
     * the full mix, OBS and the recording are not touched */
    float call_t = atomic_load_explicit(&m->call_mute, memory_order_relaxed)
                       ? 0.0f
                       : db_to_lin(atomic_load_explicit(&m->call_gain_db, memory_order_relaxed));
    for (size_t i = 0; i < n; i++) {
        ramp_step(&m->g_call_s, call_t, k);
        call[i] *= m->g_call_s;
    }
    limiter_process(&m->lim_call, call, NULL, n);
    meter_update(&m->meter_call, dsp_peak(call, n));
    if (io->call) memcpy(io->call, call, n * sizeof(float));

    /* monitor */
    float mon_t = db_to_lin(atomic_load_explicit(&m->monitor_gain_db, memory_order_relaxed));
    const float *src_l = any_solo ? solo_l : full_l;
    const float *src_r = any_solo ? solo_r : full_r;
    float mpeak = 0.0f;
    for (size_t i = 0; i < n; i++) {
        ramp_step(&m->g_monitor_s, mon_t, k);
        float l = src_l[i] * m->g_monitor_s;
        float r = src_r[i] * m->g_monitor_s;
        if (io->monitor_l) io->monitor_l[i] = l;
        if (io->monitor_r) io->monitor_r[i] = r;
        float p = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        if (p > mpeak) mpeak = p;
    }
    meter_update(&m->meter_monitor, mpeak);
}
