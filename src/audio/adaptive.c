/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "adaptive.h"

#include <math.h>
#include <speex/speex_resampler.h>
#include <stdlib.h>
#include <string.h>

/* Ratio correction is limited to +-0.5 %, far above real clock drift
 * (typically below 0.01 %) but small enough to be inaudible. */
#define MAX_CORRECTION 0.005
/* Re-tune the resampler at most every this many calls. */
#define UPDATE_EVERY 16
/* Denominator of the in/out ratio fraction handed to speexdsp. speexdsp
 * multiplies its fractional position by the denominator in 32 bits, so it
 * must stay below 65536. 60000 gives ~17 ppm steps: inaudible, and far finer
 * than the drift we correct. */
#define RATIO_DEN 60000u

struct adaptive_rs {
    SpeexResamplerState *st;
    int ch;
    uint32_t in_rate, out_rate;
    double target;
    double fill_avg;
    double correction;
    uint32_t cur_num;
    int calls;
    bool starved; /* pull mode: waiting for the ring to refill */
    size_t max_block;
    float *tmp;     /* scratch buffer */
    size_t tmp_len; /* in frames */
};

adaptive_rs_t *ars_new(int channels, uint32_t in_rate, uint32_t out_rate, int quality,
                       size_t target_fill, size_t max_block)
{
    adaptive_rs_t *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    int err = 0;
    a->st = speex_resampler_init((spx_uint32_t)channels, in_rate, out_rate, quality, &err);
    if (!a->st || err) {
        free(a);
        return NULL;
    }
    a->ch = channels;
    a->in_rate = in_rate;
    a->out_rate = out_rate;
    a->target = (double)target_fill;
    a->max_block = max_block;
    /* worst case input needed for max_block outputs (or outputs from max_block inputs) */
    double r = (double)in_rate / out_rate;
    if (r < 1.0) r = 1.0 / r;
    a->tmp_len = (size_t)(max_block * r * (1.0 + MAX_CORRECTION)) + 512;
    a->tmp = calloc(a->tmp_len * (size_t)channels, sizeof(float));
    if (!a->tmp) {
        speex_resampler_destroy(a->st);
        free(a);
        return NULL;
    }
    speex_resampler_skip_zeros(a->st);
    /* Pre-warm: speexdsp sizes its filter and history from the ratio and
     * only ever grows them. Visiting the largest ratio the correction can
     * reach allocates the worst case now, so retuning in the real-time
     * thread never allocates. */
    uint32_t warm = (uint32_t)llround((double)in_rate / out_rate * RATIO_DEN * (1.0 + MAX_CORRECTION * 1.25));
    speex_resampler_set_rate_frac(a->st, warm, RATIO_DEN, in_rate, out_rate);
    uint32_t warm_lo = (uint32_t)llround((double)in_rate / out_rate * RATIO_DEN * (1.0 - MAX_CORRECTION * 1.25));
    speex_resampler_set_rate_frac(a->st, warm_lo, RATIO_DEN, in_rate, out_rate);
    ars_reset(a);
    return a;
}

void ars_free(adaptive_rs_t *a)
{
    if (!a) return;
    speex_resampler_destroy(a->st);
    free(a->tmp);
    free(a);
}

void ars_reset(adaptive_rs_t *a)
{
    speex_resampler_reset_mem(a->st);
    a->fill_avg = a->target;
    a->correction = 0.0;
    a->cur_num = (uint32_t)llround((double)a->in_rate / a->out_rate * RATIO_DEN);
    speex_resampler_set_rate_frac(a->st, a->cur_num, RATIO_DEN, a->in_rate, a->out_rate);
    a->calls = 0;
    a->starved = true;
}

static void retune(adaptive_rs_t *a, size_t fill)
{
    /* Smooth the fill level, then apply a proportional correction. Fill above
     * target means "consume input faster" (pull) or "produce output slower"
     * (push); both raise the in/out ratio. */
    a->fill_avg += 0.05 * ((double)fill - a->fill_avg);
    if (++a->calls < UPDATE_EVERY) return;
    a->calls = 0;
    double err = (a->fill_avg - a->target) / (a->target > 1.0 ? a->target : 1.0);
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    a->correction = MAX_CORRECTION * err;
    uint32_t num = (uint32_t)llround((double)a->in_rate / a->out_rate * RATIO_DEN *
                                     (1.0 + a->correction));
    if (num != a->cur_num) {
        a->cur_num = num;
        speex_resampler_set_rate_frac(a->st, num, RATIO_DEN, a->in_rate, a->out_rate);
    }
}

bool ars_pull(adaptive_rs_t *a, ringbuf_t *in, float *out, size_t n_out)
{
    const size_t ch = (size_t)a->ch;
    size_t avail = rb_available(in) / ch;

    if (a->starved) {
        if ((double)avail < a->target) {
            memset(out, 0, n_out * ch * sizeof(float));
            return false; /* still buffering, not counted as a dropout */
        }
        a->starved = false;
        a->fill_avg = (double)avail;
    }
    /* Far too much buffered (e.g. after a stall): jump back to the target. */
    if ((double)avail > a->target * 4.0 + (double)n_out) {
        rb_skip(in, (avail - (size_t)a->target) * ch);
        avail = rb_available(in) / ch;
        a->fill_avg = (double)avail;
    }
    retune(a, avail);

    size_t produced = 0;
    bool dropout = false;
    while (produced < n_out) {
        size_t want_out = n_out - produced;
        if (want_out > a->max_block) want_out = a->max_block;
        size_t need_in = (size_t)ceil(want_out * (double)a->cur_num / RATIO_DEN) + 8;
        if (need_in > a->tmp_len) need_in = a->tmp_len;
        size_t got = rb_peek(in, a->tmp, need_in * ch) / ch;
        if (got == 0) {
            dropout = true;
            break;
        }
        spx_uint32_t in_len = (spx_uint32_t)got;
        spx_uint32_t out_len = (spx_uint32_t)want_out;
        speex_resampler_process_interleaved_float(a->st, a->tmp, &in_len, out + produced * ch,
                                                  &out_len);
        rb_skip(in, in_len * ch);
        produced += out_len;
        if (out_len == 0) {
            dropout = true;
            break;
        }
    }
    if (produced < n_out) {
        memset(out + produced * ch, 0, (n_out - produced) * ch * sizeof(float));
        dropout = true;
    }
    if (dropout) a->starved = true;
    return dropout;
}

bool ars_push(adaptive_rs_t *a, const float *in, size_t n_in, ringbuf_t *out)
{
    const size_t ch = (size_t)a->ch;
    retune(a, rb_available(out) / ch);
    bool ok = true;
    size_t consumed = 0;
    while (consumed < n_in) {
        size_t chunk = n_in - consumed;
        if (chunk > a->max_block) chunk = a->max_block;
        spx_uint32_t in_len = (spx_uint32_t)chunk;
        spx_uint32_t out_len = (spx_uint32_t)a->tmp_len;
        speex_resampler_process_interleaved_float(a->st, in + consumed * ch, &in_len, a->tmp,
                                                  &out_len);
        if (rb_write(out, a->tmp, out_len * ch) < out_len * ch) ok = false;
        if (in_len == 0) break;
        consumed += in_len;
    }
    return ok;
}

double ars_correction_ppm(const adaptive_rs_t *a)
{
    return a->correction * 1e6;
}
