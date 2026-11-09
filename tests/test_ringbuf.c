/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <pthread.h>
#include <string.h>

#include "core/ringbuf.h"
#include "testutil.h"

#define TOTAL 2000000

static ringbuf_t rb;

static void *producer(void *arg)
{
    (void)arg;
    float buf[97];
    size_t next = 0;
    while (next < TOTAL) {
        size_t n = sizeof buf / sizeof buf[0];
        if (next + n > TOTAL) n = TOTAL - next;
        for (size_t i = 0; i < n; i++) buf[i] = (float)((next + i) % 65536);
        size_t w = rb_write(&rb, buf, n);
        next += w;
    }
    return NULL;
}

int main(void)
{
    CHECK(rb_init(&rb, 1000) == 0);
    CHECK(rb.capacity == 1024);
    CHECK(rb_available(&rb) == 0);
    CHECK(rb_space(&rb) == 1024);

    /* wrap-around */
    float in[700], out[700];
    for (int i = 0; i < 700; i++) in[i] = (float)i;
    CHECK(rb_write(&rb, in, 700) == 700);
    CHECK(rb_read(&rb, out, 700) == 700);
    CHECK(rb_write(&rb, in, 700) == 700);
    CHECK(rb_peek(&rb, out, 700) == 700);
    CHECK(memcmp(in, out, sizeof in) == 0);
    CHECK(rb_skip(&rb, 100) == 100);
    CHECK(rb_available(&rb) == 600);
    CHECK(rb_write(&rb, in, 700) == 424); /* only space for 424 */
    rb_free(&rb);

    /* concurrent producer/consumer keeps order */
    CHECK(rb_init(&rb, 4096) == 0);
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    size_t got = 0;
    bool ok = true;
    float buf[61];
    while (got < TOTAL) {
        size_t n = rb_read(&rb, buf, 61);
        for (size_t i = 0; i < n; i++)
            if (buf[i] != (float)((got + i) % 65536)) ok = false;
        got += n;
    }
    pthread_join(t, NULL);
    CHECK(ok);
    rb_free(&rb);
    TEST_DONE();
}
