/*
 * Multitrack recorder. The mixer thread pushes audio blocks (no blocking, no
 * disk I/O); a writer thread drains them to disk.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * All tracks of a session start on the same mixer block, so they line up
 * sample-exactly in any editor.
 */
#ifndef BPR_RECORDER_H
#define BPR_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_MAX_TRACKS 20

typedef enum {
    REC_FMT_WAV_FLOAT, /* 32-bit float WAV, RF64 above 4 GB (default) */
    REC_FMT_WAV_24,    /* 24-bit PCM WAV, RF64 above 4 GB */
    REC_FMT_FLAC_24,   /* 24-bit FLAC, lossless and about half the size */
    REC_FMT_OPUS,      /* Ogg Opus, small files for quick sharing */
} rec_format_t;

typedef struct {
    char name[64]; /* used in the file name, e.g. "caller" */
    int channels;  /* 1 or 2 */
} rec_track_spec_t;

typedef struct recorder recorder_t;

recorder_t *recorder_new(void);
void recorder_free(recorder_t *r);

/* Creates the session folder under base_dir and opens one file per track.
 * Returns 0 on success, otherwise fills err. */
int recorder_start(recorder_t *r, const char *base_dir, rec_format_t fmt, uint32_t rate,
                   const rec_track_spec_t *tracks, int n_tracks, char *err, size_t err_len);
/* Stop accepting audio immediately (cheap, no I/O). */
void recorder_halt(recorder_t *r);
/* Halt, flush everything to disk, write markers and close the files. */
void recorder_stop(recorder_t *r);
bool recorder_is_running(const recorder_t *r);

/* Mixer thread: push one block for a track (interleaved frames). */
void recorder_push(recorder_t *r, int track, const float *frames, size_t n_frames);
/* Mixer thread: true if every track has room for n_frames. If not, call
 * recorder_drop_block() instead of pushing, so all tracks drop the same
 * block and stay aligned. */
bool recorder_block_fits(recorder_t *r, size_t n_frames);
void recorder_drop_block(recorder_t *r);
/* Mixer thread: call once per block after all tracks were pushed. */
void recorder_block_done(recorder_t *r, size_t n_frames);

/* Any thread. */
void recorder_add_marker(recorder_t *r, const char *label);
uint64_t recorder_frames(const recorder_t *r);   /* recorded length */
uint64_t recorder_overruns(const recorder_t *r); /* blocks lost because the disk was too slow */
const char *recorder_session_dir(const recorder_t *r);
/* Free bytes on the recording disk, or -1. */
int64_t recorder_free_bytes(const char *dir);

const char *rec_format_extension(rec_format_t fmt);

#endif
