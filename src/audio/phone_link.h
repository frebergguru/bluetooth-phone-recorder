/*
 * Audio link between the Bluetooth thread and the mixer thread.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 *   rx: phone -> computer (caller's voice), written by BT, read by mixer
 *   tx: computer -> phone (mix-minus),      written by mixer, read by BT
 * Both rings carry mono float samples at the codec rate (8, 16 or 32 kHz).
 */
#ifndef BPR_PHONE_LINK_H
#define BPR_PHONE_LINK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../core/ringbuf.h"

typedef struct {
    ringbuf_t rx, tx;
    _Atomic uint32_t rate;
    _Atomic uint32_t generation; /* bumped each time a new audio connection starts */
    atomic_bool active;
    _Atomic uint64_t rx_bad_frames; /* packets the codec had to conceal */
    _Atomic uint64_t tx_underruns;  /* SCO packets sent with missing audio */
} phone_link_t;

int phone_link_init(phone_link_t *l);
void phone_link_free(phone_link_t *l);

/* BT thread */
void phone_link_begin(phone_link_t *l, uint32_t rate);
void phone_link_end(phone_link_t *l);
void phone_link_write_rx_s16(phone_link_t *l, const int16_t *pcm, size_t n);
/* Fills n samples of tx audio (silence when the mixer is behind). */
void phone_link_read_tx_s16(phone_link_t *l, int16_t *pcm, size_t n);

#endif
