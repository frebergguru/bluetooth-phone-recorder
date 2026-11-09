/*
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "engine.h"

#include <errno.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <sched.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "../core/log.h"
#include "adaptive.h"

#define MAX_DEVICES 128
#define RS_QUALITY 8
#define IO_TARGET_FRAMES (ENGINE_RATE / 50) /* 20 ms buffer per device */
#define PHONE_TARGET_MS 40
#define NODE_PREFIX "bpr."
#define ECHO_TAIL (ENGINE_RATE / 5) /* 200 ms */

typedef enum { EP_INPUT, EP_OUTPUT } ep_dir_t;

typedef struct endpoint {
    engine_t *eng;
    ep_dir_t dir;
    int channels;
    bool is_virtual;
    char node_name[128];
    char description[256];
    char target[256];
    struct pw_stream *stream;
    struct spa_hook listener;
    ringbuf_t ring;
    adaptive_rs_t *rs;
    atomic_bool streaming;
    _Atomic uint64_t xruns;
} endpoint_t;

typedef struct {
    endpoint_t *capture; /* device stream */
    endpoint_t *vsrc;    /* "Phone Recorder: <label>" */
    char label[128];
    char target[256];
} input_t;

struct engine {
    engine_callbacks_t cb;
    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;

    pthread_mutex_t dev_lock;
    struct {
        uint32_t id;
        bool source;
        engine_device_t d;
    } devices[MAX_DEVICES];
    int n_devices;

    /* io_lock protects the endpoint set and recorder start/stop against the
     * mixer thread. Held only briefly. */
    pthread_mutex_t io_lock;
    mixer_t mixer;
    input_t inputs[MIXER_MAX_CHANNELS]; /* index = mixer channel, 0 unused */
    endpoint_t *monitor;
    endpoint_t *v_full, *v_caller, *v_call;

    phone_link_t link;
    uint32_t link_gen_seen;
    adaptive_rs_t *phone_rx_rs, *phone_tx_rs;
    uint32_t phone_rate;
    _Atomic uint64_t phone_rx_dropouts;

    recorder_t *rec;
    int rec_map[REC_MAX_TRACKS]; /* track -> -1 full mix, else mixer channel */
    int rec_tracks;

    atomic_bool echo_cancel;
    SpeexEchoState *echo;
    SpeexPreprocessState *echo_pp;

    pthread_t mix_thread;
    atomic_bool mix_quit;
    bool mix_started;
    int timer_fd;
    _Atomic uint64_t late_blocks;
};

/* ------------------------------------------------------------------ */
/* PipeWire stream callbacks (PipeWire real-time thread)              */
/* ------------------------------------------------------------------ */

static void on_process(void *data)
{
    endpoint_t *ep = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(ep->stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = &buf->datas[0];
    if (!d->data) {
        pw_stream_queue_buffer(ep->stream, b);
        return;
    }
    const size_t ch = (size_t)ep->channels;
    const uint32_t stride = (uint32_t)(sizeof(float) * ch);

    if (ep->dir == EP_INPUT) {
        uint32_t offset = d->chunk->offset;
        uint32_t size = d->chunk->size;
        if (offset > d->maxsize) offset = d->maxsize;
        if (size > d->maxsize - offset) size = d->maxsize - offset;
        size_t frames = size / stride;
        const float *src = (const float *)((uint8_t *)d->data + offset);
        if (rb_write(&ep->ring, src, frames * ch) < frames * ch) atomic_fetch_add(&ep->xruns, 1);
    } else {
        size_t frames = d->maxsize / stride;
        if (b->requested && b->requested < frames) frames = b->requested;
        size_t avail = rb_available(&ep->ring) / ch;
        /* After a pause (nobody listening) drop stale audio. */
        if (avail > IO_TARGET_FRAMES * 3 + frames) {
            rb_skip(&ep->ring, (avail - IO_TARGET_FRAMES) * ch);
        }
        float *dst = d->data;
        size_t got = rb_read(&ep->ring, dst, frames * ch);
        if (got < frames * ch) {
            memset(dst + got, 0, (frames * ch - got) * sizeof(float));
            if (atomic_load(&ep->streaming)) atomic_fetch_add(&ep->xruns, 1);
        }
        d->chunk->offset = 0;
        d->chunk->stride = (int32_t)stride;
        d->chunk->size = (uint32_t)(frames * stride);
    }
    pw_stream_queue_buffer(ep->stream, b);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
                             const char *error)
{
    (void)old;
    endpoint_t *ep = data;
    atomic_store(&ep->streaming, state == PW_STREAM_STATE_STREAMING);
    if (state == PW_STREAM_STATE_ERROR)
        LOGW("audio", "stream %s error: %s", ep->node_name, error ? error : "?");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

/* Call with the thread loop locked. */
static endpoint_t *endpoint_create(engine_t *e, ep_dir_t dir, int channels, bool is_virtual,
                                   const char *node_name, const char *description,
                                   const char *target)
{
    endpoint_t *ep = calloc(1, sizeof *ep);
    if (!ep) return NULL;
    ep->eng = e;
    ep->dir = dir;
    ep->channels = channels;
    ep->is_virtual = is_virtual;
    snprintf(ep->node_name, sizeof ep->node_name, "%s", node_name);
    snprintf(ep->description, sizeof ep->description, "%s", description);
    snprintf(ep->target, sizeof ep->target, "%s", target ? target : "");
    if (rb_init(&ep->ring, (size_t)ENGINE_RATE * (size_t)channels / 2) != 0) goto fail;

    if (dir == EP_INPUT)
        ep->rs = ars_new(channels, ENGINE_RATE, ENGINE_RATE, RS_QUALITY, IO_TARGET_FRAMES,
                         ENGINE_BLOCK * 2);
    else
        ep->rs = ars_new(channels, ENGINE_RATE, ENGINE_RATE, RS_QUALITY, IO_TARGET_FRAMES,
                         ENGINE_BLOCK * 2);
    if (!ep->rs) goto fail;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, dir == EP_INPUT ? "Capture" : "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_NODE_NAME, node_name,
        PW_KEY_NODE_DESCRIPTION, description,
        PW_KEY_MEDIA_NAME, description,
        NULL);
    /* No PW_KEY_NODE_LATENCY on purpose: forcing a small quantum makes
     * PipeWire reconfigure (re-open) devices, and many USB headsets reset
     * their volume when that happens. The mixer clock buffers 20 ms per
     * device anyway, so the graph's default quantum is fine. */
    if (is_virtual) {
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
        pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    }
    if (target && *target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);

    ep->stream = pw_stream_new(e->core, description, props);
    if (!ep->stream) goto fail;
    pw_stream_add_listener(ep->stream, &ep->listener, &stream_events, ep);

    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
                                                             .rate = ENGINE_RATE,
                                                             .channels = (uint32_t)channels);
    if (channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    enum pw_stream_flags flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
    if (!is_virtual) flags |= PW_STREAM_FLAG_AUTOCONNECT;
    int res = pw_stream_connect(ep->stream,
                                dir == EP_INPUT ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                PW_ID_ANY, flags, params, 1);
    if (res < 0) {
        LOGE("audio", "cannot connect stream %s: %s", node_name, spa_strerror(res));
        goto fail;
    }
    return ep;

fail:
    if (ep->stream) pw_stream_destroy(ep->stream);
    if (ep->rs) ars_free(ep->rs);
    rb_free(&ep->ring);
    free(ep);
    return NULL;
}

/* Call with the thread loop locked and the endpoint already detached from the
 * mixer (io_lock). */
static void endpoint_destroy(endpoint_t *ep)
{
    if (!ep) return;
    if (ep->stream) {
        spa_hook_remove(&ep->listener);
        pw_stream_destroy(ep->stream);
    }
    ars_free(ep->rs);
    rb_free(&ep->ring);
    free(ep);
}

/* ------------------------------------------------------------------ */
/* Registry: keep a list of audio devices for the UI                   */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
                            uint32_t version, const struct spa_dict *props)
{
    (void)permissions;
    (void)version;
    engine_t *e = data;
    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!cls || !name) return;
    if (strncmp(name, NODE_PREFIX, strlen(NODE_PREFIX)) == 0) return; /* our own nodes */
    bool source;
    if (strcmp(cls, "Audio/Source") == 0 || strcmp(cls, "Audio/Source/Virtual") == 0 ||
        strcmp(cls, "Audio/Duplex") == 0)
        source = true;
    else if (strcmp(cls, "Audio/Sink") == 0)
        source = false;
    else
        return;
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    if (!desc) desc = name;

    pthread_mutex_lock(&e->dev_lock);
    if (e->n_devices < MAX_DEVICES) {
        typeof(e->devices[0]) *d = &e->devices[e->n_devices++];
        d->id = id;
        d->source = source;
        d->d.id = id;
        snprintf(d->d.name, sizeof d->d.name, "%s", name);
        snprintf(d->d.description, sizeof d->d.description, "%s", desc);
    }
    pthread_mutex_unlock(&e->dev_lock);
    if (e->cb.devices_changed) e->cb.devices_changed(e->cb.ud);
}

static void registry_global_remove(void *data, uint32_t id)
{
    engine_t *e = data;
    bool changed = false;
    pthread_mutex_lock(&e->dev_lock);
    for (int i = 0; i < e->n_devices; i++) {
        if (e->devices[i].id == id) {
            memmove(&e->devices[i], &e->devices[i + 1],
                    (size_t)(e->n_devices - i - 1) * sizeof e->devices[0]);
            e->n_devices--;
            changed = true;
            break;
        }
    }
    pthread_mutex_unlock(&e->dev_lock);
    if (changed && e->cb.devices_changed) e->cb.devices_changed(e->cb.ud);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int engine_list_devices(engine_t *e, bool sources, engine_device_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&e->dev_lock);
    for (int i = 0; i < e->n_devices && n < max; i++)
        if (e->devices[i].source == sources) out[n++] = e->devices[i].d;
    pthread_mutex_unlock(&e->dev_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* Mixer thread                                                        */
/* ------------------------------------------------------------------ */

static void phone_update_rates(engine_t *e)
{
    uint32_t gen = atomic_load(&e->link.generation);
    if (gen == e->link_gen_seen && e->phone_rx_rs) return;
    e->link_gen_seen = gen;
    uint32_t rate = atomic_load(&e->link.rate);
    if (e->phone_rx_rs && rate == e->phone_rate) {
        ars_reset(e->phone_rx_rs);
        ars_reset(e->phone_tx_rs);
    } else {
        ars_free(e->phone_rx_rs);
        ars_free(e->phone_tx_rs);
        size_t target = rate * PHONE_TARGET_MS / 1000;
        e->phone_rx_rs = ars_new(1, rate, ENGINE_RATE, 10, target, ENGINE_BLOCK * 2);
        e->phone_tx_rs = ars_new(1, ENGINE_RATE, rate, 10, target, ENGINE_BLOCK * 2);
        e->phone_rate = rate;
    }
    /* mixer is the rx reader: drop whatever is left from before */
    rb_skip(&e->link.rx, rb_available(&e->link.rx));
    LOGI("audio", "phone audio at %u Hz", rate);
}

/* Mixer thread, io_lock held. The canceller is created by
 * engine_set_echo_cancel() on the UI thread, never here. */
static void echo_cancel_block(engine_t *e, float *call, const float *caller)
{
    if (!e->echo || !e->echo_pp) return;
    spx_int16_t mic[ENGINE_BLOCK], ref[ENGINE_BLOCK], out[ENGINE_BLOCK];
    for (int i = 0; i < ENGINE_BLOCK; i++) {
        float m = call[i] * 32767.0f, r = caller ? caller[i] * 32767.0f : 0.0f;
        mic[i] = (spx_int16_t)(m > 32767.0f ? 32767 : m < -32768.0f ? -32768 : m);
        ref[i] = (spx_int16_t)(r > 32767.0f ? 32767 : r < -32768.0f ? -32768 : r);
    }
    speex_echo_cancellation(e->echo, mic, ref, out);
    speex_preprocess_run(e->echo_pp, out);
    for (int i = 0; i < ENGINE_BLOCK; i++) call[i] = (float)out[i] / 32768.0f;
}

static void push_out(endpoint_t *ep, const float *frames, size_t n)
{
    if (!ep) return;
    ars_push(ep->rs, frames, n, &ep->ring);
}

static void mix_block(engine_t *e)
{
    static float in_buf[MIXER_MAX_CHANNELS][ENGINE_BLOCK];
    static float direct[MIXER_MAX_CHANNELS][ENGINE_BLOCK];
    static float full_l[ENGINE_BLOCK], full_r[ENGINE_BLOCK], call[ENGINE_BLOCK];
    static float mon_l[ENGINE_BLOCK], mon_r[ENGINE_BLOCK];
    static float stereo[ENGINE_BLOCK * 2];
    static float phone_buf[ENGINE_BLOCK];

    mixer_io_t io = {0};
    const size_t n = ENGINE_BLOCK;

    /* caller */
    bool phone = atomic_load_explicit(&e->link.active, memory_order_acquire);
    if (phone) {
        phone_update_rates(e);
        if (e->phone_rx_rs && ars_pull(e->phone_rx_rs, &e->link.rx, phone_buf, n))
            atomic_fetch_add(&e->phone_rx_dropouts, 1);
        io.in[ENGINE_CALLER_CH] = phone_buf;
    }
    io.direct[ENGINE_CALLER_CH] = direct[ENGINE_CALLER_CH];

    /* inputs. Every direct buffer is handed to the mixer, which zeroes the
     * ones of inactive channels: a recording track whose input was removed
     * then gets silence instead of its last block repeated. */
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
        io.direct[c] = direct[c];
        input_t *in = &e->inputs[c];
        if (!in->capture) continue;
        ars_pull(in->capture->rs, &in->capture->ring, in_buf[c], n);
        io.in[c] = in_buf[c];
    }
    io.full_l = full_l;
    io.full_r = full_r;
    io.call = call;
    io.monitor_l = mon_l;
    io.monitor_r = mon_r;
    mixer_process(&e->mixer, &io, n);

    if (phone && atomic_load(&e->echo_cancel)) echo_cancel_block(e, call, direct[ENGINE_CALLER_CH]);

    /* to the phone */
    if (phone && e->phone_tx_rs) ars_push(e->phone_tx_rs, call, n, &e->link.tx);

    /* monitor + virtual sources */
    for (size_t i = 0; i < n; i++) {
        stereo[2 * i] = mon_l[i];
        stereo[2 * i + 1] = mon_r[i];
    }
    push_out(e->monitor, stereo, n);
    for (size_t i = 0; i < n; i++) {
        stereo[2 * i] = full_l[i];
        stereo[2 * i + 1] = full_r[i];
    }
    push_out(e->v_full, stereo, n);
    push_out(e->v_caller, direct[ENGINE_CALLER_CH], n);
    push_out(e->v_call, call, n);
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++)
        if (e->inputs[c].vsrc) push_out(e->inputs[c].vsrc, direct[c], n);

    /* recorder: stereo buffer still holds the full mix. rec_tracks is set
     * only once the recorder is fully started, so all tracks and the frame
     * counter begin on the same block. */
    if (e->rec_tracks > 0 && recorder_is_running(e->rec)) {
        for (int t = 0; t < e->rec_tracks; t++) {
            int src = e->rec_map[t];
            if (src < 0)
                recorder_push(e->rec, t, stereo, n);
            else
                recorder_push(e->rec, t, direct[src], n);
        }
        recorder_block_done(e->rec, n);
    }
}

static void *mix_thread_main(void *arg)
{
    engine_t *e = arg;
    struct sched_param sp = {.sched_priority = 20};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        LOGI("audio", "mixer thread runs without real-time priority");

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec its = {
        .it_interval = {0, (long)ENGINE_BLOCK * 1000000000L / ENGINE_RATE},
        .it_value = now,
    };
    timerfd_settime(e->timer_fd, TFD_TIMER_ABSTIME, &its, NULL);

    while (!atomic_load(&e->mix_quit)) {
        uint64_t expirations = 0;
        ssize_t r = read(e->timer_fd, &expirations, sizeof expirations);
        if (r != sizeof expirations) continue;
        if (expirations > 1) atomic_fetch_add(&e->late_blocks, expirations - 1);
        if (expirations > 20) expirations = 20; /* after a long stall, do not spin */
        for (uint64_t i = 0; i < expirations; i++) {
            pthread_mutex_lock(&e->io_lock);
            mix_block(e);
            pthread_mutex_unlock(&e->io_lock);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

engine_t *engine_new(const engine_callbacks_t *cb)
{
    engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (cb) e->cb = *cb;
    pthread_mutex_init(&e->dev_lock, NULL);
    /* The real-time mixer thread takes io_lock every block: priority
     * inheritance keeps a UI thread holding it from being preempted. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&e->io_lock, &ma);
    pthread_mutexattr_destroy(&ma);
    mixer_init(&e->mixer, ENGINE_RATE);
    mixer_channel_setup(&e->mixer, ENGINE_CALLER_CH, MIX_CH_CALLER);
    e->timer_fd = -1;
    if (phone_link_init(&e->link) != 0) {
        pthread_mutex_destroy(&e->dev_lock);
        pthread_mutex_destroy(&e->io_lock);
        free(e);
        return NULL;
    }
    e->rec = recorder_new();
    if (!e->rec) {
        phone_link_free(&e->link);
        pthread_mutex_destroy(&e->dev_lock);
        pthread_mutex_destroy(&e->io_lock);
        free(e);
        return NULL;
    }
    return e;
}

int engine_start(engine_t *e, char *err, size_t err_len)
{
    pw_init(NULL, NULL);
    e->loop = pw_thread_loop_new("bpr-pipewire", NULL);
    if (!e->loop) {
        snprintf(err, err_len, "cannot create PipeWire loop");
        return -1;
    }
    e->context = pw_context_new(pw_thread_loop_get_loop(e->loop), NULL, 0);
    if (!e->context) {
        snprintf(err, err_len, "cannot create PipeWire context");
        return -1;
    }
    if (pw_thread_loop_start(e->loop) < 0) {
        snprintf(err, err_len, "cannot start PipeWire thread");
        return -1;
    }
    pw_thread_loop_lock(e->loop);
    e->core = pw_context_connect(e->context,
                                 pw_properties_new(PW_KEY_APP_NAME, "Bluetooth Phone Recorder",
                                                   NULL),
                                 0);
    if (!e->core) {
        pw_thread_loop_unlock(e->loop);
        snprintf(err, err_len, "cannot connect to PipeWire (is it running?)");
        return -1;
    }
    e->registry = pw_core_get_registry(e->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(e->registry, &e->registry_listener, &registry_events, e);

    e->v_full = endpoint_create(e, EP_OUTPUT, 2, true, NODE_PREFIX "full_mix",
                                "Phone Recorder: Full mix", NULL);
    e->v_caller = endpoint_create(e, EP_OUTPUT, 1, true, NODE_PREFIX "caller",
                                  "Phone Recorder: Caller", NULL);
    e->v_call = endpoint_create(e, EP_OUTPUT, 1, true, NODE_PREFIX "mix_minus",
                                "Phone Recorder: Mix-minus", NULL);
    pw_thread_loop_unlock(e->loop);

    e->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (e->timer_fd < 0) {
        snprintf(err, err_len, "timerfd: %s", strerror(errno));
        return -1;
    }
    atomic_store(&e->mix_quit, false);
    if (pthread_create(&e->mix_thread, NULL, mix_thread_main, e) != 0) {
        snprintf(err, err_len, "cannot start mixer thread");
        return -1;
    }
    e->mix_started = true;
    return 0;
}

void engine_free(engine_t *e)
{
    if (!e) return;
    engine_record_stop(e);
    if (e->mix_started) {
        atomic_store(&e->mix_quit, true);
        pthread_join(e->mix_thread, NULL);
    }
    if (e->timer_fd >= 0) close(e->timer_fd);
    if (e->loop) {
        pw_thread_loop_lock(e->loop);
        for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
            endpoint_destroy(e->inputs[c].capture);
            endpoint_destroy(e->inputs[c].vsrc);
        }
        endpoint_destroy(e->monitor);
        endpoint_destroy(e->v_full);
        endpoint_destroy(e->v_caller);
        endpoint_destroy(e->v_call);
        if (e->registry) {
            spa_hook_remove(&e->registry_listener);
            pw_proxy_destroy((struct pw_proxy *)e->registry);
        }
        if (e->core) pw_core_disconnect(e->core);
        pw_thread_loop_unlock(e->loop);
        pw_thread_loop_stop(e->loop);
        if (e->context) pw_context_destroy(e->context);
        pw_thread_loop_destroy(e->loop);
        pw_deinit();
    }
    ars_free(e->phone_rx_rs);
    ars_free(e->phone_tx_rs);
    if (e->echo_pp) speex_preprocess_state_destroy(e->echo_pp);
    if (e->echo) speex_echo_state_destroy(e->echo);
    recorder_free(e->rec);
    phone_link_free(&e->link);
    pthread_mutex_destroy(&e->dev_lock);
    pthread_mutex_destroy(&e->io_lock);
    free(e);
}

mixer_t *engine_mixer(engine_t *e) { return &e->mixer; }
phone_link_t *engine_phone_link(engine_t *e) { return &e->link; }
recorder_t *engine_recorder(engine_t *e) { return e->rec; }

static void make_vsrc_names(int ch, const char *label, char *node, size_t nlen, char *desc,
                            size_t dlen)
{
    snprintf(node, nlen, NODE_PREFIX "input_%d", ch);
    snprintf(desc, dlen, "Phone Recorder: %s", label);
}

int engine_add_input(engine_t *e, const char *target, const char *label)
{
    int ch = -1;
    pthread_mutex_lock(&e->io_lock);
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
        if (!e->inputs[c].capture && !e->inputs[c].vsrc) {
            ch = c;
            break;
        }
    }
    pthread_mutex_unlock(&e->io_lock);
    if (ch < 0) return -1;

    char node[128], desc[256], cap_node[128];
    make_vsrc_names(ch, label, node, sizeof node, desc, sizeof desc);
    snprintf(cap_node, sizeof cap_node, NODE_PREFIX "capture_%d", ch);

    pw_thread_loop_lock(e->loop);
    endpoint_t *cap = endpoint_create(e, EP_INPUT, 1, false, cap_node, label, target);
    endpoint_t *vs = endpoint_create(e, EP_OUTPUT, 1, true, node, desc, NULL);
    pw_thread_loop_unlock(e->loop);
    if (!cap || !vs) {
        pw_thread_loop_lock(e->loop);
        endpoint_destroy(cap);
        endpoint_destroy(vs);
        pw_thread_loop_unlock(e->loop);
        return -1;
    }

    pthread_mutex_lock(&e->io_lock);
    input_t *in = &e->inputs[ch];
    in->capture = cap;
    in->vsrc = vs;
    snprintf(in->label, sizeof in->label, "%s", label);
    snprintf(in->target, sizeof in->target, "%s", target ? target : "");
    mixer_channel_setup(&e->mixer, ch, MIX_CH_INPUT);
    pthread_mutex_unlock(&e->io_lock);
    return ch;
}

void engine_remove_input(engine_t *e, int ch)
{
    if (ch < 1 || ch >= MIXER_MAX_CHANNELS) return;
    pthread_mutex_lock(&e->io_lock);
    input_t *in = &e->inputs[ch];
    endpoint_t *cap = in->capture, *vs = in->vsrc;
    in->capture = in->vsrc = NULL;
    mixer_channel_release(&e->mixer, ch);
    pthread_mutex_unlock(&e->io_lock);
    pw_thread_loop_lock(e->loop);
    endpoint_destroy(cap);
    endpoint_destroy(vs);
    pw_thread_loop_unlock(e->loop);
}

int engine_set_input_target(engine_t *e, int ch, const char *target)
{
    if (ch < 1 || ch >= MIXER_MAX_CHANNELS || !e->inputs[ch].capture) return -1;
    char cap_node[128];
    snprintf(cap_node, sizeof cap_node, NODE_PREFIX "capture_%d", ch);
    pw_thread_loop_lock(e->loop);
    endpoint_t *cap =
        endpoint_create(e, EP_INPUT, 1, false, cap_node, e->inputs[ch].label, target);
    pw_thread_loop_unlock(e->loop);
    if (!cap) return -1;
    pthread_mutex_lock(&e->io_lock);
    endpoint_t *old = e->inputs[ch].capture;
    e->inputs[ch].capture = cap;
    snprintf(e->inputs[ch].target, sizeof e->inputs[ch].target, "%s", target ? target : "");
    pthread_mutex_unlock(&e->io_lock);
    pw_thread_loop_lock(e->loop);
    endpoint_destroy(old);
    pw_thread_loop_unlock(e->loop);
    return 0;
}

const char *engine_input_label(engine_t *e, int ch)
{
    if (ch == ENGINE_CALLER_CH) return "Caller";
    if (ch < 1 || ch >= MIXER_MAX_CHANNELS) return "";
    return e->inputs[ch].label;
}

const char *engine_input_target(engine_t *e, int ch)
{
    if (ch < 1 || ch >= MIXER_MAX_CHANNELS) return "";
    return e->inputs[ch].target;
}

void engine_set_input_label(engine_t *e, int ch, const char *label)
{
    if (ch < 1 || ch >= MIXER_MAX_CHANNELS || !e->inputs[ch].vsrc) return;
    snprintf(e->inputs[ch].label, sizeof e->inputs[ch].label, "%s", label);
    char desc[256];
    snprintf(desc, sizeof desc, "Phone Recorder: %s", label);
    pw_thread_loop_lock(e->loop);
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_NODE_DESCRIPTION, desc),
        SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_NAME, desc),
    };
    struct spa_dict dict = SPA_DICT_INIT_ARRAY(items);
    pw_stream_update_properties(e->inputs[ch].vsrc->stream, &dict);
    pw_thread_loop_unlock(e->loop);
}

int engine_set_monitor(engine_t *e, bool enabled, const char *target)
{
    endpoint_t *ep = NULL;
    if (enabled) {
        pw_thread_loop_lock(e->loop);
        ep = endpoint_create(e, EP_OUTPUT, 2, false, NODE_PREFIX "monitor",
                             "Phone Recorder monitor", target);
        pw_thread_loop_unlock(e->loop);
        if (!ep) return -1;
    }
    pthread_mutex_lock(&e->io_lock);
    endpoint_t *old = e->monitor;
    e->monitor = ep;
    pthread_mutex_unlock(&e->io_lock);
    pw_thread_loop_lock(e->loop);
    endpoint_destroy(old);
    pw_thread_loop_unlock(e->loop);
    return 0;
}

void engine_set_echo_cancel(engine_t *e, bool on)
{
    if (on) {
        /* Allocate here (UI thread), not in the real-time mixer thread. */
        SpeexEchoState *st = NULL;
        SpeexPreprocessState *pp = NULL;
        if (!e->echo) {
            st = speex_echo_state_init(ENGINE_BLOCK, ECHO_TAIL);
            pp = speex_preprocess_state_init(ENGINE_BLOCK, ENGINE_RATE);
            if (!st || !pp) {
                if (pp) speex_preprocess_state_destroy(pp);
                if (st) speex_echo_state_destroy(st);
                LOGW("audio", "cannot create the echo canceller");
                return;
            }
            int rate = ENGINE_RATE;
            speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
            speex_preprocess_ctl(pp, SPEEX_PREPROCESS_SET_ECHO_STATE, st);
        }
        pthread_mutex_lock(&e->io_lock);
        if (st) {
            e->echo = st;
            e->echo_pp = pp;
        } else {
            /* turned on again: start from a clean filter */
            speex_echo_state_reset(e->echo);
        }
        pthread_mutex_unlock(&e->io_lock);
    }
    atomic_store(&e->echo_cancel, on);
}

int engine_record_start(engine_t *e, const char *base_dir, rec_format_t fmt, char *err,
                        size_t err_len)
{
    rec_track_spec_t specs[REC_MAX_TRACKS];
    int map[REC_MAX_TRACKS];
    int n = 0;
    /* The endpoint set only changes on this (UI) thread, so it can be read
     * without io_lock. Files are created without the lock as well: disk I/O
     * must never stall the real-time mixer. */
    snprintf(specs[n].name, sizeof specs[n].name, "full-mix");
    specs[n].channels = 2;
    map[n++] = -1;
    snprintf(specs[n].name, sizeof specs[n].name, "caller");
    specs[n].channels = 1;
    map[n++] = ENGINE_CALLER_CH;
    for (int c = 1; c < MIXER_MAX_CHANNELS && n < REC_MAX_TRACKS; c++) {
        if (!e->inputs[c].capture) continue;
        snprintf(specs[n].name, sizeof specs[n].name, "%s", e->inputs[c].label);
        specs[n].channels = 1;
        map[n++] = c;
    }
    int res = recorder_start(e->rec, base_dir, fmt, ENGINE_RATE, specs, n, err, err_len);
    if (res == 0) {
        pthread_mutex_lock(&e->io_lock);
        memcpy(e->rec_map, map, sizeof map);
        e->rec_tracks = n;
        pthread_mutex_unlock(&e->io_lock);
    }
    return res;
}

void engine_record_stop(engine_t *e)
{
    if (!e->rec) return;
    /* Halt under io_lock so the mixer is guaranteed to be outside
     * recorder_push; then finish (join writer, close files) without the lock
     * so the audio keeps running while files are closed. */
    pthread_mutex_lock(&e->io_lock);
    e->rec_tracks = 0;
    recorder_halt(e->rec);
    pthread_mutex_unlock(&e->io_lock);
    recorder_stop(e->rec);
}

void engine_get_stats(engine_t *e, engine_stats_t *st)
{
    memset(st, 0, sizeof *st);
    pthread_mutex_lock(&e->io_lock);
    for (int c = 1; c < MIXER_MAX_CHANNELS; c++) {
        if (e->inputs[c].capture) st->input_dropouts += atomic_load(&e->inputs[c].capture->xruns);
    }
    if (e->monitor) st->output_underruns += atomic_load(&e->monitor->xruns);
    st->phone_rx_dropouts = atomic_load(&e->phone_rx_dropouts);
    if (e->phone_rx_rs) st->phone_rx_ppm = ars_correction_ppm(e->phone_rx_rs);
    if (e->phone_tx_rs) st->phone_tx_ppm = ars_correction_ppm(e->phone_tx_rs);
    pthread_mutex_unlock(&e->io_lock);
    st->late_blocks = atomic_load(&e->late_blocks);
}
