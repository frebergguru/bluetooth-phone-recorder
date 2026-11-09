/*
 * Firmware unpacking: .zst, .xz and plain files come out byte-identical.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <lzma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "bt/firmware.h"
#include "testutil.h"

static unsigned char data[100000];

static int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    return fclose(f);
}

static int same_as_data(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char buf[sizeof data + 10];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return n == sizeof data && memcmp(buf, data, n) == 0;
}

int main(void)
{
    for (size_t i = 0; i < sizeof data; i++) data[i] = (unsigned char)((i * 7919) ^ (i >> 5));

    /* zstd */
    size_t cap = ZSTD_compressBound(sizeof data);
    void *z = malloc(cap);
    size_t zlen = ZSTD_compress(z, cap, data, sizeof data, 3);
    CHECK(!ZSTD_isError(zlen));
    CHECK(write_file("fw-test.bin.zst", z, zlen) == 0);
    CHECK(bt_firmware_unpack_file("fw-test.bin.zst", "fw-test-out-zst") == 0);
    CHECK(same_as_data("fw-test-out-zst"));
    /* truncated .zst files must fail, not hang (a cut inside the frame
     * header used to loop forever) */
    CHECK(write_file("fw-cut.bin.zst", z, 6) == 0);
    CHECK(bt_firmware_unpack_file("fw-cut.bin.zst", "fw-cut-out") != 0);
    CHECK(write_file("fw-cut.bin.zst", z, zlen / 2) == 0);
    CHECK(bt_firmware_unpack_file("fw-cut.bin.zst", "fw-cut-out") != 0);
    free(z);

    /* xz */
    size_t xcap = lzma_stream_buffer_bound(sizeof data);
    uint8_t *x = malloc(xcap);
    size_t xlen = 0;
    CHECK(lzma_easy_buffer_encode(6, LZMA_CHECK_CRC32, NULL, data, sizeof data, x, &xlen, xcap) ==
          LZMA_OK);
    CHECK(write_file("fw-test.bin.xz", x, xlen) == 0);
    CHECK(bt_firmware_unpack_file("fw-test.bin.xz", "fw-test-out-xz") == 0);
    CHECK(same_as_data("fw-test-out-xz"));
    CHECK(write_file("fw-cut.bin.xz", x, xlen / 2) == 0);
    CHECK(bt_firmware_unpack_file("fw-cut.bin.xz", "fw-cut-out") != 0);
    free(x);

    /* plain */
    CHECK(write_file("fw-test.bin", data, sizeof data) == 0);
    CHECK(bt_firmware_unpack_file("fw-test.bin", "fw-test-out-plain") == 0);
    CHECK(same_as_data("fw-test-out-plain"));

    /* corrupt input fails cleanly */
    CHECK(write_file("fw-bad.bin.zst", "not zstd", 8) == 0);
    CHECK(bt_firmware_unpack_file("fw-bad.bin.zst", "fw-bad-out") != 0);
    TEST_DONE();
}
