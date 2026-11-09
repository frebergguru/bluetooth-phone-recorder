/*
 * Small real-time DSP blocks: high-pass filter, noise gate, peak limiter,
 * level meter. All work on mono float blocks and never allocate.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_DSP_H
#define BPR_DSP_H

#include <stddef.h>

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

void biquad_highpass(biquad_t *bq, float rate, float freq, float q);
void biquad_process(biquad_t *bq, float *buf, size_t n);
void biquad_reset(biquad_t *bq);

typedef struct {
    float threshold;  /* linear */
    float env;        /* envelope follower */
    float gain;       /* current gate gain */
    float attack, release, env_coef;
    int hold_samples, hold;
    float floor_gain; /* attenuation when closed (linear) */
} gate_t;

void gate_init(gate_t *g, float rate, float threshold_db);
void gate_set_threshold(gate_t *g, float threshold_db);
void gate_process(gate_t *g, float *buf, size_t n);

typedef struct {
    float ceiling; /* linear */
    float env;
    float release;
} limiter_t;

void limiter_init(limiter_t *l, float rate, float ceiling_db);
/* Processes one or two channels with a shared gain (b may be NULL). */
void limiter_process(limiter_t *l, float *a, float *b, size_t n);

/* Returns the block peak (absolute). */
float dsp_peak(const float *buf, size_t n);

static inline float db_to_lin(float db) { return __builtin_powf(10.0f, db / 20.0f); }
float lin_to_db(float lin); /* returns -120 for silence */

#endif
