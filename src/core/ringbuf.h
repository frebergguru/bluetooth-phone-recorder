/*
 * Lock-free single-producer / single-consumer ring buffer of float samples.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One thread writes, one thread reads. No locks, safe for real-time threads.
 * The reader may also drop samples (rb_skip) to pull latency back down.
 */
#ifndef BPR_RINGBUF_H
#define BPR_RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float *data;
    size_t capacity; /* power of two */
    size_t mask;
    _Atomic size_t write_pos; /* only the writer stores */
    _Atomic size_t read_pos;  /* only the reader stores */
} ringbuf_t;

/* Capacity is rounded up to a power of two. Returns 0 on success. */
int rb_init(ringbuf_t *rb, size_t min_capacity);
void rb_free(ringbuf_t *rb);

size_t rb_available(const ringbuf_t *rb); /* samples readable */
size_t rb_space(const ringbuf_t *rb);     /* samples writable */

/* Write up to n samples, returns how many were written. Writer thread only. */
size_t rb_write(ringbuf_t *rb, const float *src, size_t n);
/* Write n zero samples (or as many as fit). Writer thread only. */
size_t rb_write_silence(ringbuf_t *rb, size_t n);
/* Read up to n samples, returns how many were read. Reader thread only. */
size_t rb_read(ringbuf_t *rb, float *dst, size_t n);
/* Copy up to n samples without consuming them. Reader thread only. */
size_t rb_peek(const ringbuf_t *rb, float *dst, size_t n);
/* Drop up to n samples. Reader thread only. */
size_t rb_skip(ringbuf_t *rb, size_t n);

#endif
