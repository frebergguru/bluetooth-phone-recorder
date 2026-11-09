/*
 * Adaptive resampler: converts between two sample rates and slowly trims the
 * ratio so a ring buffer stays at a target fill level. This absorbs clock drift
 * between devices (sound cards, the Bluetooth link, the internal mix clock).
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BPR_ADAPTIVE_H
#define BPR_ADAPTIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/ringbuf.h"

typedef struct adaptive_rs adaptive_rs_t;

/* Rings hold interleaved frames of `channels` samples. target_fill and
 * max_block are in frames. */
adaptive_rs_t *ars_new(int channels, uint32_t in_rate, uint32_t out_rate, int quality,
                       size_t target_fill, size_t max_block);
void ars_free(adaptive_rs_t *ars);
void ars_reset(adaptive_rs_t *ars);

/* Pull mode: read from ring `in` (at in_rate) and produce exactly n_out frames.
 * Missing input is replaced by silence; returns true if a dropout happened. */
bool ars_pull(adaptive_rs_t *ars, ringbuf_t *in, float *out, size_t n_out);

/* Push mode: resample n_in frames and append them to ring `out` (at out_rate).
 * Returns false if the ring was full and samples were dropped. */
bool ars_push(adaptive_rs_t *ars, const float *in, size_t n_in, ringbuf_t *out);

/* Current ratio correction in parts per million (for diagnostics). */
double ars_correction_ppm(const adaptive_rs_t *ars);

#endif
