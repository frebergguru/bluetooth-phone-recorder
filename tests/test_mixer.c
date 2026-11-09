/*
 * Mixer tests. The key requirement: the "send to call" switch must only ever
 * change the call bus. Direct outputs (per-input recording tracks and OBS
 * virtual sources), the full mix and the monitor must stay bit-identical.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include <string.h>

#include "audio/mixer.h"
#include "testutil.h"

#define N 480
#define BLOCKS 50

typedef struct {
    float direct[3][N * BLOCKS];
    float full_l[N * BLOCKS], full_r[N * BLOCKS];
    float call[N * BLOCKS];
    float mon_l[N * BLOCKS], mon_r[N * BLOCKS];
} capture_t;

static void make_inputs(float in[3][N * BLOCKS])
{
    for (int i = 0; i < N * BLOCKS; i++) {
        in[0][i] = 0.3f * sinf(2.0f * (float)M_PI * 440.0f * i / 48000.0f);  /* caller */
        in[1][i] = 0.2f * sinf(2.0f * (float)M_PI * 220.0f * i / 48000.0f);  /* mic 1 */
        in[2][i] = 0.25f * sinf(2.0f * (float)M_PI * 330.0f * i / 48000.0f); /* mic 2 */
    }
}

/* toggle_block >= 0: switch mic 1's "send to call" off at that block */
static void run(capture_t *cap, int toggle_block, bool mute_mic2_at_10)
{
    static float in[3][N * BLOCKS];
    make_inputs(in);
    mixer_t m;
    mixer_init(&m, 48000.0f);
    mixer_channel_setup(&m, 0, MIX_CH_CALLER);
    mixer_channel_setup(&m, 1, MIX_CH_INPUT);
    mixer_channel_setup(&m, 2, MIX_CH_INPUT);
    for (int b = 0; b < BLOCKS; b++) {
        if (b == toggle_block) atomic_store(&m.ch[1].send_to_call, false);
        if (mute_mic2_at_10 && b == 10) atomic_store(&m.ch[2].mute, true);
        mixer_io_t io = {0};
        for (int c = 0; c < 3; c++) {
            io.in[c] = &in[c][b * N];
            io.direct[c] = &cap->direct[c][b * N];
        }
        io.full_l = &cap->full_l[b * N];
        io.full_r = &cap->full_r[b * N];
        io.call = &cap->call[b * N];
        io.monitor_l = &cap->mon_l[b * N];
        io.monitor_r = &cap->mon_r[b * N];
        mixer_process(&m, &io, N);
    }
}

static float energy(const float *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)x[i] * x[i];
    return (float)(e / n);
}

int main(void)
{
    static capture_t a, b;

    /* 1) send-to-call off must not touch anything but the call bus */
    run(&a, -1, false);
    run(&b, 5, false);
    for (int c = 0; c < 3; c++)
        CHECK(memcmp(a.direct[c], b.direct[c], sizeof a.direct[c]) == 0);
    CHECK(memcmp(a.full_l, b.full_l, sizeof a.full_l) == 0);
    CHECK(memcmp(a.full_r, b.full_r, sizeof a.full_r) == 0);
    CHECK(memcmp(a.mon_l, b.mon_l, sizeof a.mon_l) == 0);
    CHECK(memcmp(a.mon_r, b.mon_r, sizeof a.mon_r) == 0);
    /* ...but the call bus must change */
    CHECK(memcmp(a.call, b.call, sizeof a.call) != 0);

    /* 2) mix-minus: the caller is never in the call bus. With both mics'
     * send switched off at block 0 the call bus must be silent. */
    {
        static float in[3][N * BLOCKS];
        make_inputs(in);
        mixer_t m;
        mixer_init(&m, 48000.0f);
        mixer_channel_setup(&m, 0, MIX_CH_CALLER);
        mixer_channel_setup(&m, 1, MIX_CH_INPUT);
        mixer_channel_setup(&m, 2, MIX_CH_INPUT);
        atomic_store(&m.ch[1].send_to_call, false);
        atomic_store(&m.ch[2].send_to_call, false);
        atomic_store(&m.ch[0].send_to_call, true); /* even if someone sets it */
        float call[N], full_l[N];
        float maxabs = 0;
        for (int blk = 0; blk < BLOCKS; blk++) {
            mixer_io_t io = {0};
            for (int c = 0; c < 3; c++) io.in[c] = &in[c][blk * N];
            io.call = call;
            io.full_l = full_l;
            mixer_process(&m, &io, N);
            if (blk > 20) /* after the 10 ms ramp has fully settled */
                for (int i = 0; i < N; i++)
                    if (fabsf(call[i]) > maxabs) maxabs = fabsf(call[i]);
        }
        CHECK(maxabs < 1e-6f);
    }

    /* 3) mute removes a channel from the full mix but its direct output (OBS
     * per-input source, per-input track) keeps the signal */
    run(&b, -1, true);
    CHECK(memcmp(a.direct[2], b.direct[2], sizeof a.direct[2]) == 0);
    CHECK(memcmp(a.full_l, b.full_l, sizeof a.full_l) != 0);
    CHECK(energy(&b.direct[2][40 * N], N * 10) > 0.01f);

    /* 4) gain: +6 dB doubles the direct level once ramps settle */
    {
        static float in[3][N * BLOCKS];
        make_inputs(in);
        mixer_t m;
        mixer_init(&m, 48000.0f);
        mixer_channel_setup(&m, 1, MIX_CH_INPUT);
        atomic_store(&m.ch[1].hpf, false);
        atomic_store(&m.ch[1].gain_db, 6.0206f);
        float d[N];
        for (int blk = 0; blk < BLOCKS; blk++) {
            mixer_io_t io = {0};
            io.in[1] = &in[1][blk * N];
            io.direct[1] = d;
            mixer_process(&m, &io, N);
        }
        float ratio = sqrtf(energy(d, N) / energy(&in[1][(BLOCKS - 1) * N], N));
        CHECK(fabsf(ratio - 2.0f) < 0.01f);
    }

    /* 5) the limiter keeps the call bus below 0 dBFS even when hot */
    {
        mixer_t m;
        mixer_init(&m, 48000.0f);
        mixer_channel_setup(&m, 1, MIX_CH_INPUT);
        mixer_channel_setup(&m, 2, MIX_CH_INPUT);
        atomic_store(&m.ch[1].gain_db, 12.0f);
        atomic_store(&m.ch[2].gain_db, 12.0f);
        static float in[3][N * BLOCKS];
        make_inputs(in);
        float call[N];
        float peak = 0;
        for (int blk = 0; blk < BLOCKS; blk++) {
            mixer_io_t io = {0};
            io.in[1] = &in[1][blk * N];
            io.in[2] = &in[2][blk * N];
            io.call = call;
            mixer_process(&m, &io, N);
            for (int i = 0; i < N; i++)
                if (fabsf(call[i]) > peak) peak = fabsf(call[i]);
        }
        CHECK(peak <= 1.0f);
    }

    /* 6) a released channel writes silence to its direct output (the engine
     * relies on this so a removed input's recording track gets silence, not
     * its last block over and over) */
    {
        mixer_t m;
        mixer_init(&m, 48000.0f);
        mixer_channel_setup(&m, 1, MIX_CH_INPUT);
        static float in[3][N * BLOCKS];
        make_inputs(in);
        float d[N];
        mixer_io_t io = {0};
        io.in[1] = in[1];
        io.direct[1] = d;
        mixer_process(&m, &io, N);
        CHECK(energy(d, N) > 0.0f);
        mixer_channel_release(&m, 1);
        io.in[1] = NULL;
        mixer_process(&m, &io, N);
        float maxabs = 0;
        for (int i = 0; i < N; i++)
            if (fabsf(d[i]) > maxabs) maxabs = fabsf(d[i]);
        CHECK(maxabs == 0.0f);
    }

    TEST_DONE();
}
