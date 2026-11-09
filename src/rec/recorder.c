/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "recorder.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sndfile.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "../core/log.h"
#include "../core/paths.h"
#include "../core/ringbuf.h"

#define RING_SECONDS 20
#define MAX_MARKERS 1000

typedef struct {
    rec_track_spec_t spec;
    ringbuf_t ring;
    SNDFILE *sf;
    char path[1024];
} rec_track_t;

typedef struct {
    uint64_t frame;
    char label[128];
} rec_marker_t;

struct recorder {
    rec_track_t tracks[REC_MAX_TRACKS];
    int n_tracks;
    uint32_t rate;
    rec_format_t fmt;
    char session_dir[1024];

    atomic_bool running;
    _Atomic uint64_t frames;
    _Atomic uint64_t overruns;
    pthread_t writer;
    bool writer_started;
    atomic_bool writer_quit;

    pthread_mutex_t marker_lock;
    rec_marker_t markers[MAX_MARKERS];
    int n_markers;
};

const char *rec_format_extension(rec_format_t fmt)
{
    switch (fmt) {
    case REC_FMT_FLAC_24: return "flac";
    case REC_FMT_OPUS: return "opus";
    case REC_FMT_WAV_24:
    case REC_FMT_WAV_FLOAT:
    default: return "wav";
    }
}

static int sf_format_for(rec_format_t fmt)
{
    switch (fmt) {
    case REC_FMT_WAV_24: return SF_FORMAT_RF64 | SF_FORMAT_PCM_24;
    case REC_FMT_FLAC_24: return SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
    case REC_FMT_OPUS: return SF_FORMAT_OGG | SF_FORMAT_OPUS;
    case REC_FMT_WAV_FLOAT:
    default: return SF_FORMAT_RF64 | SF_FORMAT_FLOAT;
    }
}

recorder_t *recorder_new(void)
{
    recorder_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    pthread_mutex_init(&r->marker_lock, NULL);
    return r;
}

void recorder_free(recorder_t *r)
{
    if (!r) return;
    recorder_stop(r);
    pthread_mutex_destroy(&r->marker_lock);
    free(r);
}

static void sanitize(char *dst, size_t len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c))
            dst[j++] = (char)tolower(c);
        else if (j > 0 && dst[j - 1] != '-')
            dst[j++] = '-';
    }
    while (j > 0 && dst[j - 1] == '-') j--;
    if (j == 0 && len > 6) {
        memcpy(dst, "track", 5);
        j = 5;
    }
    dst[j] = 0;
}

/* Creates base_dir/stamp, or base_dir/stamp-2, -3 ... if a session with the
 * same second already exists, so a quick stop and start never overwrites the
 * previous recording. Returns 0 or -1 with errno set. */
static int make_session_dir(char *dir, size_t len, const char *base_dir, const char *stamp)
{
    if (paths_mkdirs(base_dir) != 0) return -1;
    for (int i = 1; i < 1000; i++) {
        int w = i == 1 ? snprintf(dir, len, "%s/%s", base_dir, stamp)
                       : snprintf(dir, len, "%s/%s-%d", base_dir, stamp, i);
        if (w < 0 || (size_t)w >= len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (mkdir(dir, 0755) == 0) return 0;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

static size_t drain_track(recorder_t *r, rec_track_t *t)
{
    float buf[8192];
    size_t ch = (size_t)t->spec.channels;
    size_t total = 0;
    for (;;) {
        size_t avail = rb_available(&t->ring);
        size_t want = avail - (avail % ch);
        if (want > sizeof buf / sizeof buf[0]) want = (sizeof buf / sizeof buf[0]) / ch * ch;
        if (want == 0) break;
        size_t got = rb_read(&t->ring, buf, want);
        if (t->sf) {
            sf_count_t w = sf_writef_float(t->sf, buf, (sf_count_t)(got / ch));
            if (w != (sf_count_t)(got / ch))
                LOGE("rec", "write failed for %s: %s", t->path, sf_strerror(t->sf));
        }
        total += got;
    }
    (void)r;
    return total;
}

static void *writer_main(void *arg)
{
    recorder_t *r = arg;
    while (!atomic_load(&r->writer_quit)) {
        size_t written = 0;
        for (int i = 0; i < r->n_tracks; i++) written += drain_track(r, &r->tracks[i]);
        if (written == 0) {
            struct timespec ts = {0, 50 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    /* final drain */
    for (int i = 0; i < r->n_tracks; i++) drain_track(r, &r->tracks[i]);
    return NULL;
}

int recorder_start(recorder_t *r, const char *base_dir, rec_format_t fmt, uint32_t rate,
                   const rec_track_spec_t *tracks, int n_tracks, char *err, size_t err_len)
{
    if (atomic_load(&r->running)) {
        snprintf(err, err_len, "already recording");
        return -1;
    }
    if (n_tracks < 1 || n_tracks > REC_MAX_TRACKS) {
        snprintf(err, err_len, "bad track count");
        return -1;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    if (make_session_dir(r->session_dir, sizeof r->session_dir, base_dir, stamp) != 0) {
        snprintf(err, err_len, "cannot create %s: %s", r->session_dir, strerror(errno));
        return -1;
    }

    r->fmt = fmt;
    r->rate = rate;
    r->n_tracks = 0;
    r->n_markers = 0;
    atomic_store(&r->frames, 0);
    atomic_store(&r->overruns, 0);

    for (int i = 0; i < n_tracks; i++) {
        rec_track_t *t = &r->tracks[i];
        memset(t, 0, sizeof *t);
        t->spec = tracks[i];
        char clean[64];
        sanitize(clean, sizeof clean, tracks[i].name);
        snprintf(t->path, sizeof t->path, "%s/%02d-%s.%s", r->session_dir, i + 1, clean,
                 rec_format_extension(fmt));
        if (rb_init(&t->ring, (size_t)rate * RING_SECONDS * (size_t)t->spec.channels) != 0) {
            snprintf(err, err_len, "out of memory");
            goto fail;
        }
        SF_INFO info = {0};
        info.samplerate = (int)rate;
        info.channels = t->spec.channels;
        info.format = sf_format_for(fmt);
        t->sf = sf_open(t->path, SFM_WRITE, &info);
        if (!t->sf) {
            snprintf(err, err_len, "cannot open %s: %s", t->path, sf_strerror(NULL));
            rb_free(&t->ring);
            goto fail;
        }
        /* Keep the header valid after every write: a crash or power cut loses at
         * most the last fraction of a second, the file stays readable. */
        sf_command(t->sf, SFC_SET_UPDATE_HEADER_AUTO, NULL, SF_TRUE);
        if (fmt == REC_FMT_WAV_FLOAT || fmt == REC_FMT_WAV_24)
            sf_command(t->sf, SFC_RF64_AUTO_DOWNGRADE, NULL, SF_TRUE);
        if (fmt == REC_FMT_FLAC_24) {
            double level = 0.5; /* moderate compression level */
            sf_command(t->sf, SFC_SET_COMPRESSION_LEVEL, &level, sizeof level);
        }
        sf_set_string(t->sf, SF_STR_SOFTWARE, "Bluetooth Phone Recorder");
        sf_set_string(t->sf, SF_STR_TITLE, t->spec.name);
        r->n_tracks++;
    }

    atomic_store(&r->writer_quit, false);
    if (pthread_create(&r->writer, NULL, writer_main, r) != 0) {
        snprintf(err, err_len, "cannot start writer thread");
        goto fail;
    }
    r->writer_started = true;
    atomic_store_explicit(&r->running, true, memory_order_release);
    LOGI("rec", "recording to %s (%d tracks)", r->session_dir, r->n_tracks);
    return 0;

fail:
    for (int i = 0; i < r->n_tracks; i++) {
        if (r->tracks[i].sf) sf_close(r->tracks[i].sf);
        rb_free(&r->tracks[i].ring);
    }
    r->n_tracks = 0;
    return -1;
}

static void fmt_time(char *buf, size_t len, double sec)
{
    int h = (int)(sec / 3600.0);
    int m = (int)((sec - h * 3600.0) / 60.0);
    double s = sec - h * 3600.0 - m * 60.0;
    snprintf(buf, len, "%02d:%02d:%06.3f", h, m, s);
}

static void write_marker_files(recorder_t *r)
{
    if (r->n_markers == 0) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/markers-audacity.txt", r->session_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < r->n_markers; i++) {
            double t = (double)r->markers[i].frame / r->rate;
            fprintf(f, "%.6f\t%.6f\t%s\n", t, t, r->markers[i].label);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/chapters.txt", r->session_dir);
    f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < r->n_markers; i++) {
            char ts[32];
            fmt_time(ts, sizeof ts, (double)r->markers[i].frame / r->rate);
            fprintf(f, "%s %s\n", ts, r->markers[i].label);
        }
        fclose(f);
    }
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* libsndfile accepts cue points only before the first sample is written, but
 * markers are known only at the end. So the "cue " chunk (and a LIST/adtl
 * chunk with the marker names, which editors show as labels) is appended to
 * the closed file here, and the RIFF size (or the RF64 ds64 size) is fixed
 * up. Returns 0 on success. */
static int wav_append_cues(const char *path, const rec_marker_t *markers, int n_markers)
{
    int n = 0;
    for (int i = 0; i < n_markers; i++)
        if (markers[i].frame <= UINT32_MAX) n++; /* cue positions are 32-bit */
    if (n == 0) return 0;

    size_t cue_len = 8 + 4 + 24 * (size_t)n;
    size_t adtl_len = 12; /* "LIST" size "adtl" */
    for (int i = 0; i < n_markers; i++) {
        if (markers[i].frame > UINT32_MAX) continue;
        size_t text = strlen(markers[i].label) + 1;
        adtl_len += 8 + 4 + text + (text & 1);
    }
    unsigned char *buf = calloc(1, cue_len + adtl_len);
    if (!buf) return -1;
    unsigned char *p = buf;
    memcpy(p, "cue ", 4);
    put_le32(p + 4, (uint32_t)(cue_len - 8));
    put_le32(p + 8, (uint32_t)n);
    p += 12;
    uint32_t id = 0;
    for (int i = 0; i < n_markers; i++) {
        if (markers[i].frame > UINT32_MAX) continue;
        id++;
        put_le32(p, id);                              /* cue point id */
        put_le32(p + 4, (uint32_t)markers[i].frame);  /* position */
        memcpy(p + 8, "data", 4);                     /* chunk */
        put_le32(p + 12, 0);                          /* chunk start */
        put_le32(p + 16, 0);                          /* block start */
        put_le32(p + 20, (uint32_t)markers[i].frame); /* sample offset */
        p += 24;
    }
    memcpy(p, "LIST", 4);
    put_le32(p + 4, (uint32_t)(adtl_len - 8));
    memcpy(p + 8, "adtl", 4);
    p += 12;
    id = 0;
    for (int i = 0; i < n_markers; i++) {
        if (markers[i].frame > UINT32_MAX) continue;
        id++;
        size_t text = strlen(markers[i].label) + 1;
        memcpy(p, "labl", 4);
        put_le32(p + 4, (uint32_t)(4 + text));
        put_le32(p + 8, id);
        memcpy(p + 12, markers[i].label, text); /* includes the 0, pad byte is 0 too */
        p += 12 + text + (text & 1);
    }

    int res = -1;
    FILE *f = fopen(path, "r+b");
    if (!f) goto out;
    unsigned char hdr[28];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr + 8, "WAVE", 4) != 0) goto out;
    bool rf64 = memcmp(hdr, "RF64", 4) == 0;
    if (!rf64 && memcmp(hdr, "RIFF", 4) != 0) goto out;
    if (rf64 && (fread(hdr + 12, 1, 16, f) != 16 || memcmp(hdr + 12, "ds64", 4) != 0)) goto out;
    if (fseeko(f, 0, SEEK_END) != 0) goto out;
    off_t end = ftello(f);
    if (end < 12) goto out;
    if ((end & 1) && fputc(0, f) == EOF) goto out; /* chunks start on even offsets */
    if (fwrite(buf, 1, cue_len + adtl_len, f) != cue_len + adtl_len) goto out;
    off_t new_end = ftello(f);
    if (new_end < 0) goto out;
    uint64_t riff_size = (uint64_t)new_end - 8;
    if (rf64) {
        unsigned char sz[8];
        for (int i = 0; i < 8; i++) sz[i] = (unsigned char)(riff_size >> (8 * i));
        if (fseeko(f, 20, SEEK_SET) != 0 || fwrite(sz, 1, 8, f) != 8) goto out;
    } else {
        if (riff_size > UINT32_MAX) goto out;
        unsigned char sz[4];
        put_le32(sz, (uint32_t)riff_size);
        if (fseeko(f, 4, SEEK_SET) != 0 || fwrite(sz, 1, 4, f) != 4) goto out;
    }
    res = 0;
out:
    if (f && fclose(f) != 0) res = -1;
    free(buf);
    return res;
}

void recorder_halt(recorder_t *r)
{
    atomic_store_explicit(&r->running, false, memory_order_release);
}

void recorder_stop(recorder_t *r)
{
    if (!atomic_load(&r->running) && !r->writer_started) return;
    atomic_store_explicit(&r->running, false, memory_order_release);
    if (r->writer_started) {
        atomic_store(&r->writer_quit, true);
        pthread_join(r->writer, NULL);
        r->writer_started = false;
    }
    pthread_mutex_lock(&r->marker_lock);
    bool wav = r->fmt == REC_FMT_WAV_FLOAT || r->fmt == REC_FMT_WAV_24;
    for (int i = 0; i < r->n_tracks; i++) {
        rec_track_t *t = &r->tracks[i];
        bool opened = t->sf != NULL;
        if (t->sf) sf_close(t->sf);
        t->sf = NULL;
        rb_free(&t->ring);
        if (opened && wav && r->n_markers > 0 &&
            wav_append_cues(t->path, r->markers, r->n_markers) != 0)
            LOGW("rec", "could not store cue markers in %s", t->path);
    }
    write_marker_files(r);
    pthread_mutex_unlock(&r->marker_lock);
    LOGI("rec", "stopped, %llu frames, %llu overruns",
         (unsigned long long)atomic_load(&r->frames), (unsigned long long)atomic_load(&r->overruns));
    r->n_tracks = 0;
}

bool recorder_is_running(const recorder_t *r)
{
    return atomic_load_explicit(&r->running, memory_order_acquire);
}

void recorder_push(recorder_t *r, int track, const float *frames, size_t n_frames)
{
    if (!atomic_load_explicit(&r->running, memory_order_acquire)) return;
    if (track < 0 || track >= r->n_tracks) return;
    rec_track_t *t = &r->tracks[track];
    size_t n = n_frames * (size_t)t->spec.channels;
    if (rb_space(&t->ring) < n) {
        atomic_fetch_add(&r->overruns, 1);
        /* keep tracks aligned: write silence of the same length if possible */
        rb_write_silence(&t->ring, n);
        return;
    }
    rb_write(&t->ring, frames, n);
}

void recorder_block_done(recorder_t *r, size_t n_frames)
{
    if (!atomic_load_explicit(&r->running, memory_order_acquire)) return;
    atomic_fetch_add(&r->frames, n_frames);
}

void recorder_add_marker(recorder_t *r, const char *label)
{
    if (!recorder_is_running(r)) return;
    pthread_mutex_lock(&r->marker_lock);
    if (r->n_markers < MAX_MARKERS) {
        rec_marker_t *m = &r->markers[r->n_markers++];
        m->frame = atomic_load(&r->frames);
        snprintf(m->label, sizeof m->label, "%s", label && *label ? label : "Marker");
    }
    pthread_mutex_unlock(&r->marker_lock);
}

uint64_t recorder_frames(const recorder_t *r)
{
    return atomic_load(&r->frames);
}

uint64_t recorder_overruns(const recorder_t *r)
{
    return atomic_load(&r->overruns);
}

const char *recorder_session_dir(const recorder_t *r)
{
    return r->session_dir;
}

int64_t recorder_free_bytes(const char *dir)
{
    struct statvfs st;
    if (statvfs(dir, &st) != 0) return -1;
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}
