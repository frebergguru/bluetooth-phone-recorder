/*
 * Audio engine: PipeWire I/O around the mixer.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * One mixer thread runs on its own clock (48 kHz, 5 ms blocks). Every device
 * stream, the phone link and every virtual source is connected through an
 * adaptive resampler, so devices with different clocks never drift apart
 * and all recorded tracks stay sample-aligned.
 *
 * Virtual sources (always present while the app runs, visible to OBS and any
 * other PipeWire/PulseAudio app):
 *   "Phone Recorder: Full mix"   stereo
 *   "Phone Recorder: Caller"     mono
 *   "Phone Recorder: Mix-minus"  mono, what the caller hears
 *   "Phone Recorder: <input>"    mono, one per input, unaffected by
 *                                mute and by "send to call"
 */
#ifndef BPR_ENGINE_H
#define BPR_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../rec/recorder.h"
#include "mixer.h"
#include "phone_link.h"

#define ENGINE_RATE 48000
#define ENGINE_BLOCK 240 /* 5 ms */
#define ENGINE_CALLER_CH 0

typedef struct engine engine_t;

typedef struct {
    uint32_t id;
    char name[256];        /* node.name, used as target */
    char description[256]; /* human readable */
} engine_device_t;

typedef struct {
    /* Called from the PipeWire thread when devices appear or disappear. */
    void (*devices_changed)(void *ud);
    void *ud;
} engine_callbacks_t;

/* All functions below except the getters for stats and the mixer are control
 * functions: call them from one thread only (the UI thread). */
engine_t *engine_new(const engine_callbacks_t *cb);
int engine_start(engine_t *e, char *err, size_t err_len);
void engine_free(engine_t *e);

mixer_t *engine_mixer(engine_t *e);
phone_link_t *engine_phone_link(engine_t *e);
recorder_t *engine_recorder(engine_t *e);

/* sources = true lists microphones/inputs, false lists outputs. */
int engine_list_devices(engine_t *e, bool sources, engine_device_t *out, int max);

/* Inputs. target NULL or "" = system default. Returns mixer channel or -1. */
int engine_add_input(engine_t *e, const char *target, const char *label);
void engine_remove_input(engine_t *e, int ch);
int engine_set_input_target(engine_t *e, int ch, const char *target);
const char *engine_input_label(engine_t *e, int ch);
const char *engine_input_target(engine_t *e, int ch); /* "" = system default */
void engine_set_input_label(engine_t *e, int ch, const char *label);

/* Monitor output (headphones). target NULL or "" = system default. */
int engine_set_monitor(engine_t *e, bool enabled, const char *target);

/* Echo cancellation on the call bus, for people using speakers. */
void engine_set_echo_cancel(engine_t *e, bool on);

/* Recording: one track for the full mix, the caller and every input. */
int engine_record_start(engine_t *e, const char *base_dir, rec_format_t fmt, char *err,
                        size_t err_len);
void engine_record_stop(engine_t *e);

typedef struct {
    uint64_t input_dropouts;
    uint64_t output_underruns;
    uint64_t phone_rx_dropouts;
    double phone_rx_ppm, phone_tx_ppm;
    uint64_t late_blocks;
} engine_stats_t;
void engine_get_stats(engine_t *e, engine_stats_t *st);

#endif
