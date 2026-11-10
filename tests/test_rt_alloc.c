/*
 * Real-time safety: once created, the adaptive resampler must never allocate,
 * also while it keeps retuning for clock drift. Counts every malloc, calloc
 * and realloc in the process (including inside libspeexdsp) by interposing
 * them.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>

#include "audio/adaptive.h"
#include "testutil.h"

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);

static atomic_int counting;
static atomic_long allocations;

void *malloc(size_t n)
{
    if (atomic_load(&counting)) atomic_fetch_add(&allocations, 1);
    return __libc_malloc(n);
}
void *calloc(size_t a, size_t b)
{
    if (atomic_load(&counting)) atomic_fetch_add(&allocations, 1);
    return __libc_calloc(a, b);
}
void *realloc(void *p, size_t n)
{
    if (atomic_load(&counting)) atomic_fetch_add(&allocations, 1);
    return __libc_realloc(p, n);
}

static void run(uint32_t in_rate, uint32_t out_rate, double drift, bool pull)
{
    ringbuf_t ring;
    CHECK(rb_init(&ring, 65536) == 0);
    size_t target = (pull ? in_rate : out_rate) / 25;
    adaptive_rs_t *rs = ars_new(1, in_rate, out_rate, 10, target, 480);
    CHECK(rs != NULL);
    float in[2048], out[2048];
    double acc = 0, phase = 0;

    atomic_store(&allocations, 0);
    atomic_store(&counting, 1);
    for (int blk = 0; blk < 20000; blk++) { /* 100 s of 5 ms blocks */
        if (blk == 10000) ars_reset(rs); /* a new call */
        if (pull) {
            acc += in_rate / 200.0 * (1.0 + drift);
            int n = (int)acc;
            acc -= n;
            for (int i = 0; i < n; i++, phase += 0.07) in[i] = 0.3f * (float)sin(phase);
            rb_write(&ring, in, (size_t)n);
            ars_pull(rs, &ring, out, 240);
        } else {
            for (int i = 0; i < 240; i++, phase += 0.05) in[i] = 0.3f * (float)sin(phase);
            ars_push(rs, in, 240, &ring);
            acc += out_rate / 200.0 * (1.0 - drift);
            int n = (int)acc;
            acc -= n;
            rb_read(&ring, out, (size_t)n);
        }
    }
    atomic_store(&counting, 0);
    long a = atomic_load(&allocations);
    if (a) fprintf(stderr, "%u -> %u (%s): %ld allocations\n", in_rate, out_rate, pull ? "pull" : "push", a);
    CHECK(a == 0);
    ars_free(rs);
    rb_free(&ring);
}

int main(void)
{
    const uint32_t rates[] = {8000, 16000, 32000};
    for (int i = 0; i < 3; i++) {
        run(rates[i], 48000, 400e-6, true);   /* phone -> mixer, drifting */
        run(rates[i], 48000, -400e-6, true);
        run(48000, rates[i], 400e-6, false);  /* mixer -> phone */
        run(48000, rates[i], -400e-6, false);
    }
    run(48000, 48000, 300e-6, true); /* device streams */
    run(48000, 48000, -300e-6, false);
    TEST_DONE();
}
