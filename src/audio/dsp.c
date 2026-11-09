/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "dsp.h"

#include <math.h>

float lin_to_db(float lin)
{
    if (lin < 1e-6f) return -120.0f;
    return 20.0f * log10f(lin);
}

void biquad_highpass(biquad_t *bq, float rate, float freq, float q)
{
    /* RBJ cookbook high-pass */
    float w0 = 2.0f * (float)M_PI * freq / rate;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * q);
    float a0 = 1.0f + alpha;
    bq->b0 = ((1.0f + cw) / 2.0f) / a0;
    bq->b1 = (-(1.0f + cw)) / a0;
    bq->b2 = ((1.0f + cw) / 2.0f) / a0;
    bq->a1 = (-2.0f * cw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    biquad_reset(bq);
}

void biquad_reset(biquad_t *bq)
{
    bq->z1 = bq->z2 = 0.0f;
}

void biquad_process(biquad_t *bq, float *buf, size_t n)
{
    /* transposed direct form II */
    float z1 = bq->z1, z2 = bq->z2;
    for (size_t i = 0; i < n; i++) {
        float x = buf[i];
        float y = bq->b0 * x + z1;
        z1 = bq->b1 * x - bq->a1 * y + z2;
        z2 = bq->b2 * x - bq->a2 * y;
        buf[i] = y;
    }
    /* flush denormals */
    if (fabsf(z1) < 1e-20f) z1 = 0.0f;
    if (fabsf(z2) < 1e-20f) z2 = 0.0f;
    bq->z1 = z1;
    bq->z2 = z2;
}

void gate_init(gate_t *g, float rate, float threshold_db)
{
    g->env = 0.0f;
    g->gain = 1.0f;
    g->attack = 1.0f - expf(-1.0f / (0.002f * rate));   /* 2 ms open */
    g->release = 1.0f - expf(-1.0f / (0.150f * rate));  /* 150 ms close */
    g->env_coef = 1.0f - expf(-1.0f / (0.010f * rate)); /* 10 ms detector */
    g->hold_samples = (int)(0.200f * rate);             /* 200 ms hold */
    g->hold = 0;
    g->floor_gain = db_to_lin(-30.0f);
    gate_set_threshold(g, threshold_db);
}

void gate_set_threshold(gate_t *g, float threshold_db)
{
    g->threshold = db_to_lin(threshold_db);
}

void gate_process(gate_t *g, float *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float x = fabsf(buf[i]);
        g->env += g->env_coef * (x - g->env);
        float target;
        if (g->env > g->threshold) {
            g->hold = g->hold_samples;
            target = 1.0f;
        } else if (g->hold > 0) {
            g->hold--;
            target = 1.0f;
        } else {
            target = g->floor_gain;
        }
        float coef = target > g->gain ? g->attack : g->release;
        g->gain += coef * (target - g->gain);
        buf[i] *= g->gain;
    }
}

void limiter_init(limiter_t *l, float rate, float ceiling_db)
{
    l->ceiling = db_to_lin(ceiling_db);
    l->env = 0.0f;
    l->release = 1.0f - expf(-1.0f / (0.080f * rate));
}

void limiter_process(limiter_t *l, float *a, float *b, size_t n)
{
    /* Instant attack peak limiter with smooth release. Without look-ahead the
     * attack is instant, which is inaudible for speech at these levels. */
    for (size_t i = 0; i < n; i++) {
        float p = fabsf(a[i]);
        if (b) {
            float pb = fabsf(b[i]);
            if (pb > p) p = pb;
        }
        if (p > l->env)
            l->env = p;
        else
            l->env += l->release * (p - l->env);
        if (l->env > l->ceiling) {
            float gain = l->ceiling / l->env;
            a[i] *= gain;
            if (b) b[i] *= gain;
        }
    }
}

float dsp_peak(const float *buf, size_t n)
{
    float p = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float x = fabsf(buf[i]);
        if (x > p) p = x;
    }
    return p;
}
