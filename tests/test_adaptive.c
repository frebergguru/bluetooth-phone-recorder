/*
 * Adaptive resampler: correct rate conversion, and the ring fill level stays
 * bounded when producer and consumer clocks differ (simulated drift).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include <string.h>

#include "audio/adaptive.h"
#include "testutil.h"

static double tone_freq_estimate(const float *x, int n, double rate)
{
    /* count zero crossings */
    int zc = 0;
    for (int i = 1; i < n; i++)
        if ((x[i - 1] < 0) != (x[i] < 0)) zc++;
    return zc * rate / (2.0 * n);
}

int main(void)
{
    /* 16 kHz phone audio -> 48 kHz mixer, consumer runs 300 ppm fast */
    ringbuf_t ring;
    CHECK(rb_init(&ring, 32768) == 0);
    adaptive_rs_t *rs = ars_new(1, 16000, 48000, 8, 640, 480);
    CHECK(rs != NULL);

    double phase = 0;
    double prod_acc = 0;
    float out[240];
    static float tail[48000];
    int tail_pos = 0;
    size_t min_fill = 1u << 30, max_fill = 0;
    int dropouts = 0;
    for (int blk = 0; blk < 20000; blk++) { /* 100 s */
        /* producer: 16 kHz * (1 - 300 ppm) per 5 ms */
        prod_acc += 80.0 * (1.0 - 300e-6);
        int np = (int)prod_acc;
        prod_acc -= np;
        float in[100];
        for (int i = 0; i < np; i++) {
            in[i] = 0.5f * (float)sin(phase);
            phase += 2.0 * M_PI * 1000.0 / 16000.0;
        }
        rb_write(&ring, in, (size_t)np);
        if (ars_pull(rs, &ring, out, 240) && blk > 400) dropouts++;
        size_t fill = rb_available(&ring);
        if (blk > 2000) {
            if (fill < min_fill) min_fill = fill;
            if (fill > max_fill) max_fill = fill;
        }
        if (blk >= 20000 - 200) {
            memcpy(&tail[tail_pos], out, sizeof out);
            tail_pos += 240;
        }
    }
    CHECK(dropouts == 0);
    CHECK(min_fill > 200);  /* never ran dry */
    CHECK(max_fill < 2000); /* did not grow without bound */
    double f = tone_freq_estimate(tail, tail_pos, 48000.0);
    CHECK(fabs(f - 1000.0) < 5.0);
    ars_free(rs);
    rb_free(&ring);

    /* push mode, stereo, 48k -> 48k with a slow reader */
    CHECK(rb_init(&ring, 65536) == 0);
    rs = ars_new(2, 48000, 48000, 5, 960, 480);
    float st[480];
    double rd_acc = 0;
    size_t maxf = 0;
    for (int blk = 0; blk < 20000; blk++) {
        for (int i = 0; i < 240; i++) st[2 * i] = st[2 * i + 1] = 0.1f;
        ars_push(rs, st, 240, &ring);
        rd_acc += 240.0 * (1.0 - 200e-6);
        int nr = (int)rd_acc;
        rd_acc -= nr;
        float sink[480 * 2];
        rb_read(&ring, sink, (size_t)nr * 2);
        if (blk > 2000 && rb_available(&ring) / 2 > maxf) maxf = rb_available(&ring) / 2;
    }
    CHECK(maxf < 3000);
    ars_free(rs);
    rb_free(&ring);
    TEST_DONE();
}
