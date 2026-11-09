/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "firmware.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <lzma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "../core/log.h"
#include "../core/paths.h"

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

static unsigned char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || sz > 64L * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *len = got;
    return buf;
}

static int write_all(const char *path, const unsigned char *buf, size_t len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path);
}

static unsigned char *unzstd(const unsigned char *in, size_t in_len, size_t *out_len)
{
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (!dctx) return NULL;
    size_t cap = in_len * 4 + 65536;
    unsigned char *out = malloc(cap);
    ZSTD_inBuffer ib = {in, in_len, 0};
    size_t pos = 0;
    while (out) {
        ZSTD_outBuffer ob = {out + pos, cap - pos, 0};
        size_t r = ZSTD_decompressStream(dctx, &ob, &ib);
        pos += ob.pos;
        if (ZSTD_isError(r)) {
            free(out);
            out = NULL;
            break;
        }
        if (r == 0 && ib.pos == ib.size) break;
        if (ib.pos == ib.size && ob.pos == 0 && pos < cap) {
            /* input used up, frame not finished, nothing more comes out:
             * truncated file. Without this check the loop never ends. */
            free(out);
            out = NULL;
            break;
        }
        if (pos == cap) {
            cap *= 2;
            unsigned char *n = realloc(out, cap);
            if (!n) {
                free(out);
                out = NULL;
                break;
            }
            out = n;
        }
    }
    ZSTD_freeDCtx(dctx);
    *out_len = pos;
    return out;
}

static unsigned char *unxz(const unsigned char *in, size_t in_len, size_t *out_len)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return NULL;
    size_t cap = in_len * 4 + 65536;
    unsigned char *out = malloc(cap);
    strm.next_in = in;
    strm.avail_in = in_len;
    lzma_ret ret = LZMA_OK;
    while (out) {
        strm.next_out = out + strm.total_out;
        strm.avail_out = cap - strm.total_out;
        ret = lzma_code(&strm, LZMA_FINISH);
        if (ret == LZMA_STREAM_END) break;
        if (ret != LZMA_OK && ret != LZMA_BUF_ERROR) {
            free(out);
            out = NULL;
            break;
        }
        if (strm.avail_out == 0) {
            cap *= 2;
            unsigned char *n = realloc(out, cap);
            if (!n) {
                free(out);
                out = NULL;
                break;
            }
            out = n;
        } else if (ret == LZMA_BUF_ERROR) {
            free(out); /* truncated input */
            out = NULL;
            break;
        }
    }
    *out_len = (size_t)strm.total_out;
    lzma_end(&strm);
    return out;
}

int bt_firmware_unpack_file(const char *src, const char *dst)
{
    size_t in_len = 0, out_len = 0;
    unsigned char *in = read_all(src, &in_len);
    if (!in) return -1;
    unsigned char *out = NULL;
    if (ends_with(src, ".zst"))
        out = unzstd(in, in_len, &out_len);
    else if (ends_with(src, ".xz"))
        out = unxz(in, in_len, &out_len);
    else {
        out = in;
        out_len = in_len;
        in = NULL;
    }
    free(in);
    if (!out) return -1;
    int r = write_all(dst, out, out_len);
    free(out);
    return r;
}

typedef bool (*fw_filter_t)(const char *base_name);

static bool filter_realtek(const char *n) { return strncmp(n, "rtl", 3) == 0; }
static bool filter_intel(const char *n)
{
    return strncmp(n, "ibt-", 4) == 0 && (ends_with(n, ".sfi") || ends_with(n, ".ddc") ||
                                          ends_with(n, ".bseq"));
}
static bool filter_bcm(const char *n) { return ends_with(n, ".hcd"); }

/* Strip ".zst"/".xz" and, if requested, ".bin" (BTstack's Realtek table uses
 * bare names such as "rtl8761bu_fw"). */
static void target_name(const char *src_name, bool strip_bin, char *out, size_t len)
{
    snprintf(out, len, "%s", src_name);
    size_t l = strlen(out);
    if (l > 4 && strcmp(out + l - 4, ".zst") == 0) out[l -= 4] = 0;
    if (l > 3 && strcmp(out + l - 3, ".xz") == 0) out[l -= 3] = 0;
    if (strip_bin && l > 4 && strcmp(out + l - 4, ".bin") == 0) out[l -= 4] = 0;
}

static int unpack_dir(const char *src_dir, const char *dst_dir, fw_filter_t filter, bool strip_bin)
{
    DIR *d = opendir(src_dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char plain[NAME_MAX + 1];
        target_name(de->d_name, false, plain, sizeof plain);
        if (!filter(plain)) continue;
        char tname[NAME_MAX + 1];
        target_name(de->d_name, strip_bin, tname, sizeof tname);
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", src_dir, de->d_name);
        snprintf(dst, sizeof dst, "%s/%s", dst_dir, tname);
        struct stat ss, ds;
        if (stat(src, &ss) != 0 || !S_ISREG(ss.st_mode)) continue;
        /* Unpacked copies carry the source's mtime, so a changed source
         * (firmware package update, or a higher priority folder) is
         * unpacked again. */
        if (stat(dst, &ds) == 0 && ds.st_mtime == ss.st_mtime) {
            count++;
            continue;
        }
        if (bt_firmware_unpack_file(src, dst) == 0) {
            struct timespec times[2] = {ss.st_atim, ss.st_mtim};
            utimensat(AT_FDCWD, dst, times, 0);
            count++;
        } else
            LOGW("firmware", "could not unpack %s", src);
    }
    closedir(d);
    return count;
}

const char *bt_firmware_prepare(bt_chip_family_t family, const char *override_dir)
{
    static char dst[PATH_MAX];
    const char *sub;
    fw_filter_t filter;
    bool strip_bin = false;
    switch (family) {
    case BT_CHIP_REALTEK:
        sub = "rtl_bt";
        filter = filter_realtek;
        strip_bin = true;
        break;
    case BT_CHIP_INTEL:
        sub = "intel";
        filter = filter_intel;
        break;
    case BT_CHIP_BROADCOM:
        sub = "brcm";
        filter = filter_bcm;
        break;
    default:
        return NULL;
    }
    snprintf(dst, sizeof dst, "%s/firmware/%s", paths_cache_dir(), sub);
    if (paths_mkdirs(dst) != 0) {
        LOGE("firmware", "cannot create %s", dst);
        return NULL;
    }
    /* Lowest priority first; later folders overwrite earlier ones. */
    int total = 0;
    const char *roots[] = {"/usr/lib/firmware", "/lib/firmware", "/lib/firmware/updates"};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        char src[PATH_MAX];
        snprintf(src, sizeof src, "%s/%s", roots[i], sub);
        total += unpack_dir(src, dst, filter, strip_bin);
    }
    if (override_dir && *override_dir) total += unpack_dir(override_dir, dst, filter, strip_bin);
    LOGI("firmware", "%d %s firmware files ready in %s", total, bt_chip_family_name(family), dst);
    return dst;
}
