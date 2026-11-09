/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "phone_link.h"

#include <math.h>

/* one second at the highest HFP rate (LC3-SWB, 32 kHz) */
#define LINK_RING 32768

int phone_link_init(phone_link_t *l)
{
    if (rb_init(&l->rx, LINK_RING) != 0) return -1;
    if (rb_init(&l->tx, LINK_RING) != 0) {
        rb_free(&l->rx);
        return -1;
    }
    atomic_store(&l->rate, 16000);
    atomic_store(&l->generation, 0);
    atomic_store(&l->active, false);
    atomic_store(&l->rx_bad_frames, 0);
    atomic_store(&l->tx_underruns, 0);
    return 0;
}

void phone_link_free(phone_link_t *l)
{
    rb_free(&l->rx);
    rb_free(&l->tx);
}

void phone_link_begin(phone_link_t *l, uint32_t rate)
{
    /* BT thread is the tx reader, so it may drop stale tx audio */
    rb_skip(&l->tx, rb_available(&l->tx));
    atomic_store(&l->rate, rate);
    atomic_fetch_add(&l->generation, 1);
    atomic_store_explicit(&l->active, true, memory_order_release);
}

void phone_link_end(phone_link_t *l)
{
    atomic_store_explicit(&l->active, false, memory_order_release);
}

void phone_link_write_rx_s16(phone_link_t *l, const int16_t *pcm, size_t n)
{
    float buf[512];
    while (n > 0) {
        size_t chunk = n > 512 ? 512 : n;
        for (size_t i = 0; i < chunk; i++) buf[i] = (float)pcm[i] / 32768.0f;
        rb_write(&l->rx, buf, chunk);
        pcm += chunk;
        n -= chunk;
    }
}

void phone_link_read_tx_s16(phone_link_t *l, int16_t *pcm, size_t n)
{
    float buf[512];
    while (n > 0) {
        size_t chunk = n > 512 ? 512 : n;
        size_t got = rb_read(&l->tx, buf, chunk);
        if (got < chunk) atomic_fetch_add(&l->tx_underruns, 1);
        for (size_t i = 0; i < chunk; i++) {
            float v = i < got ? buf[i] : 0.0f;
            long s = lrintf(v * 32767.0f);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcm[i] = (int16_t)s;
        }
        pcm += chunk;
        n -= chunk;
    }
}
