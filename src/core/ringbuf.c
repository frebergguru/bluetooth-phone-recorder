/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

int rb_init(ringbuf_t *rb, size_t min_capacity)
{
    size_t cap = 1;
    while (cap < min_capacity) cap <<= 1;
    rb->data = calloc(cap, sizeof(float));
    if (!rb->data) return -1;
    rb->capacity = cap;
    rb->mask = cap - 1;
    atomic_store_explicit(&rb->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_pos, 0, memory_order_relaxed);
    return 0;
}

void rb_free(ringbuf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->capacity = rb->mask = 0;
}

size_t rb_available(const ringbuf_t *rb)
{
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    return w - r;
}

size_t rb_space(const ringbuf_t *rb)
{
    return rb->capacity - rb_available(rb);
}

static size_t rb_write_impl(ringbuf_t *rb, const float *src, size_t n)
{
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    size_t space = rb->capacity - (w - r);
    if (n > space) n = space;
    if (n == 0) return 0;
    size_t start = w & rb->mask;
    size_t first = rb->capacity - start;
    if (first > n) first = n;
    if (src) {
        memcpy(rb->data + start, src, first * sizeof(float));
        memcpy(rb->data, src + first, (n - first) * sizeof(float));
    } else {
        memset(rb->data + start, 0, first * sizeof(float));
        memset(rb->data, 0, (n - first) * sizeof(float));
    }
    atomic_store_explicit(&rb->write_pos, w + n, memory_order_release);
    return n;
}

size_t rb_write(ringbuf_t *rb, const float *src, size_t n)
{
    return rb_write_impl(rb, src, n);
}

size_t rb_write_silence(ringbuf_t *rb, size_t n)
{
    return rb_write_impl(rb, NULL, n);
}

size_t rb_peek(const ringbuf_t *rb, float *dst, size_t n)
{
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t avail = w - r;
    if (n > avail) n = avail;
    if (n == 0) return 0;
    size_t start = r & rb->mask;
    size_t first = rb->capacity - start;
    if (first > n) first = n;
    memcpy(dst, rb->data + start, first * sizeof(float));
    memcpy(dst + first, rb->data, (n - first) * sizeof(float));
    return n;
}

size_t rb_read(ringbuf_t *rb, float *dst, size_t n)
{
    n = rb_peek(rb, dst, n);
    if (n) {
        size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
        atomic_store_explicit(&rb->read_pos, r + n, memory_order_release);
    }
    return n;
}

size_t rb_skip(ringbuf_t *rb, size_t n)
{
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t avail = w - r;
    if (n > avail) n = avail;
    atomic_store_explicit(&rb->read_pos, r + n, memory_order_release);
    return n;
}
