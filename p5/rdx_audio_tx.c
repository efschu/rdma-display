/* SPDX-License-Identifier: MIT
 *
 * P5 audio sender, driven by this host's PipeWire graph (no spinning, no own
 * timer loop). The PipeWire process callback assembles quanta of 64 frames
 * (48 kHz, stereo F32) and posts each with ONE RDMA_WRITE_WITH_IMM into the
 * player's ring (imm = quantum index); only every 32nd WR is signaled. A
 * second thread blocks on the CQ completion channel and handles completions
 * and the player's marker reports, stamping their arrival with our clock:
 *   round   = post of marker quantum k -> report for k arrives
 *   net     = round - player residence (CQE -> handed to PipeWire)
 *   e2e     = round + PipeWire delay on the player (handed over -> device);
 *             the ~1 us return path is not subtracted
 *
 * Sources:
 *   default         deterministic test signal (rdx_audio_test_sample), played
 *                   by an output stream into our own inaudible null sink, so
 *                   the graph clock of this host paces it like real audio
 *   --capture NODE  capture stream from the monitor of sink NODE; "default" follows
 *                   the default sink (what this host is playing right now): the stream has
 *                   no fixed target, so the session manager (WirePlumber) moves it to the new
 *                   default whenever that changes or the sink goes away. A named NODE that
 *                   vanishes is replaced by "default" the same way. Should the stream still
 *                   end in an error or go silent for 0.5 s, it is rebuilt on the default sink
 *                   (one log line); the quantum counter and the timestamps run on, the player
 *                   sees at most one underrun.
 *   --timer         test signal WITHOUT PipeWire: an own SCHED_FIFO thread
 *                   blocks on an absolute timerfd at the exact 48 kHz raster
 *                   (no spinning). Separates transport/player quality from the
 *                   quality of this host's audio server.
 *   --mic SRC       the microphone direction (B -> A, rdx-clientd; p5/rdx_audio_mic.h):
 *                   capture from source SRC ("default" = the default source) ONLY while
 *                   the player says an app on its host records (RDX_AUDIO_CTL_MIC_OPEN /
 *                   _CLOSE); otherwise the capture stream is disconnected, the device
 *                   closed. A named source that is not there is refused (no fallback to
 *                   another microphone). SIGUSR1 mutes (silence is sent), SIGUSR2 unmutes;
 *                   --muted starts muted.
 *   --list-sources  print the sources one could pick as JSON and exit (no peer needed)
 *
 *   rdx_audio_tx --peer 10.0.0.1 [--dev auto] [--port 18670]
 *                [--mtu 4096] [--seconds 300] [--capture NODE] [--pm-qos-us 0]
 *                [--rt-prio 70] [--timer] [--mic SRC [--muted]] [--list-sources]
 *
 * --seconds 0 runs until SIGINT/SIGTERM (session use by rdx-hostd); the player gets
 * its END right behind the last quantum either way.
 */
#define _GNU_SOURCE
#include "../common/rdx_audio.h"
#include "rdx_audio_pw.h"
#include "rdx_audio_mic.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#define LOCAL_Q    256u                 /* local quantum buffers; recycled after LOCAL_Q posts */
#define NRECV      64
#define WR_DATA    0x100u
#define WR_RECV    0x200u
#define WR_END     0x300u
#define TPOST_RING 8192u

struct tx {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct ibv_comp_channel *ch;
    struct ibv_mr *mr, *rmr;
    float *local;
    struct rdx_audio_report *rbuf;
    struct rdx_audio_hello hello;
    _Atomic uint64_t k;                 /* next quantum index (PipeWire thread writes) */
    _Atomic uint64_t t_post[TPOST_RING];
    long unsignaled;
    float acc[RDX_AUDIO_QUANTUM * RDX_AUDIO_CH];
    uint32_t acc_n;
    uint64_t frame;                     /* test signal: next absolute frame */
    _Atomic uint64_t total_q;           /* stop posting after this many quanta */
    _Atomic int failed;
    /* report stats, CQ thread only */
    double *round, *net, *res, *delay, *e2e;
    long nrep, maxrep;
    int32_t ppm_min, ppm_max, ppm_last, fill_min, fill_max;
    uint32_t underruns_last;
    /* PipeWire */
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct rdx_null_sink ns;
    int capture;
    _Atomic int sched_policy;
    int rt_prio;                         /* --rt-prio: SCHED_FIFO priority for the data thread, 0 = leave */
    _Atomic int rt_err;
    uint64_t t_last_cycle;               /* cycle gap instrument (data thread only) */
    _Atomic uint64_t gap_max_ns;
    _Atomic uint32_t gaps4ms;            /* cycles more than 4 ms after the previous one */                  /* errno of pthread_setschedparam, +1 */           /* data thread policy seen in the first cycle, +1 */
    /* --mic */
    const char *mic;                     /* source name or "default", NULL = not the mic direction */
    struct rdx_pw_graph graph;
    int mic_open;
    unsigned mic_opens;
    uint8_t mic_pod[1024];
    _Atomic uint64_t cycles, nobuf, frames;   /* capture instrument: process calls, without buffer, frames */
    const struct spa_pod *mic_params[1];
    /* --capture: the stream is rebuilt after an error (main thread builds, state callback flags) */
    char cap_target[256];                /* "" = follow the default sink, else a fixed sink name */
    uint8_t cap_pod[1024];
    const struct spa_pod *cap_params[1];
    _Atomic int cap_restart;
    uint64_t cap_t_restart, cap_t_progress, cap_frames_seen;
    unsigned cap_restarts, cap_streak;
};

static struct tx T;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_muted;   /* --mic: SIGUSR1 mutes, SIGUSR2 unmutes */

static void on_signal(int sig) { (void)sig; g_stop = 1; }
static void on_mute(int sig) { g_muted = sig == SIGUSR1; }

/* PipeWire thread: post the assembled quantum T.k */

/* level instrument: peak |sample| since the last periodic log line (data thread writes,
 * main thread reads and resets; a lost update only shifts one peak to the next line) */
static volatile float g_peak;
static inline void peak_of(const float *x, size_t n)
{
    float p = g_peak;
    for (size_t i = 0; i < n; i++) { float a = x[i] < 0 ? -x[i] : x[i]; if (a > p) p = a; }
    g_peak = p;
}
static double peak_take_db(void)
{
    float p = g_peak;
    g_peak = 0;
    return p > 0 ? 20.0 * log10(p) : -120.0;
}

static void post_quantum(const float *q)
{
    peak_of(q, (size_t)RDX_AUDIO_QUANTUM * RDX_AUDIO_CH);
    uint64_t k = atomic_load(&T.k);
    uint64_t total = atomic_load(&T.total_q);
    if (k >= total || atomic_load(&T.failed)) return;
    float *dst = T.local + (k % LOCAL_Q) * RDX_AUDIO_QUANTUM * RDX_AUDIO_CH;
    memcpy(dst, q, RDX_AUDIO_QUANTUM * RDX_AUDIO_FRAME_BYTES);
    uint64_t pos = (k * RDX_AUDIO_QUANTUM) % T.hello.ring_frames;
    struct ibv_sge sge = {.addr = (uintptr_t)dst, .length = RDX_AUDIO_QUANTUM * RDX_AUDIO_FRAME_BYTES,
                          .lkey = T.mr->lkey};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = WR_DATA;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.sg_list = &sge; wr.num_sge = 1;
    wr.imm_data = htonl((uint32_t)k);
    if (++T.unsignaled >= 32) { wr.send_flags = IBV_SEND_SIGNALED; T.unsignaled = 0; }
    wr.wr.rdma.remote_addr = T.hello.base_addr + pos * RDX_AUDIO_FRAME_BYTES;
    wr.wr.rdma.rkey = T.hello.rkey;
    atomic_store(&T.t_post[k % TPOST_RING], rdx_now_ns());
    if (ibv_post_send(T.qp, &wr, &bad)) { perror("post_send"); atomic_store(&T.failed, 1); return; }
    atomic_store(&T.k, k + 1);
    /* END right behind the last quantum, from the producing thread: the player must not
     * wait (and count underruns) until the main thread notices */
    if (k + 1 == total) rdx_post_imm_send(T.qp, RDX_AUDIO_IMM_END, WR_END);
}

static void push_frames(const float *src, uint32_t n, int muted)
{
    for (uint32_t i = 0; i < n; i++) {
        T.acc[T.acc_n * 2] = muted ? 0.0f : src[i * 2];
        T.acc[T.acc_n * 2 + 1] = muted ? 0.0f : src[i * 2 + 1];
        if (++T.acc_n == RDX_AUDIO_QUANTUM) { T.acc_n = 0; post_quantum(T.acc); }
    }
}

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(T.stream);
    atomic_fetch_add(&T.cycles, 1);
    if (!b) atomic_fetch_add(&T.nobuf, 1);
    {
        uint64_t tc = rdx_now_ns();
        if (T.t_last_cycle) {
            uint64_t gap = tc - T.t_last_cycle;
            if (gap > atomic_load(&T.gap_max_ns)) atomic_store(&T.gap_max_ns, gap);
            if (gap > 4000000ull) atomic_fetch_add(&T.gaps4ms, 1);
        }
        T.t_last_cycle = tc;
    }
    if (!atomic_load(&T.sched_policy)) {
        /* realtime for the data thread ourselves: PipeWire's client module-rt only asks rtkit,
         * which refuses processes outside an active login session */
        if (T.rt_prio > 0 && sched_getscheduler(0) == SCHED_OTHER) {
            struct sched_param sp = {.sched_priority = T.rt_prio};
            atomic_store(&T.rt_err, pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) + 1);
        }
        atomic_store(&T.sched_policy, sched_getscheduler(0) + 1);
    }
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data) { pw_stream_queue_buffer(T.stream, b); return; }
    if (T.capture) {
        const float *src = (const float *)((const uint8_t *)d->data + d->chunk->offset);
        atomic_fetch_add(&T.frames, d->chunk->size / RDX_AUDIO_FRAME_BYTES);
        push_frames(src, d->chunk->size / RDX_AUDIO_FRAME_BYTES, T.mic && g_muted);
    } else {
        /* the graph asks for n frames: generate them, feed the null sink and the network */
        uint32_t n = b->requested ? (uint32_t)b->requested : RDX_AUDIO_QUANTUM;
        uint32_t maxf = d->maxsize / RDX_AUDIO_FRAME_BYTES;
        if (n > maxf) n = maxf;
        float *dst = d->data;
        for (uint32_t i = 0; i < n; i++) {
            dst[i * 2] = rdx_audio_test_sample(T.frame + i, 0);
            dst[i * 2 + 1] = rdx_audio_test_sample(T.frame + i, 1);
        }
        T.frame += n;
        push_frames(dst, n, 0);
        d->chunk->offset = 0;
        d->chunk->stride = RDX_AUDIO_FRAME_BYTES;
        d->chunk->size = n * RDX_AUDIO_FRAME_BYTES;
    }
    pw_stream_queue_buffer(T.stream, b);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    (void)data; (void)old;
    fprintf(stderr, "[atx] stream %s%s%s\n", pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
    /* the capture target went away or the link broke for good: the main thread builds a new stream
     * (cap_check; not from inside the stream's own callback) */
    if (state == PW_STREAM_STATE_ERROR && T.capture && !T.mic) atomic_store(&T.cap_restart, 1);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .process = on_process,
};

/* --timer: one quantum per absolute timerfd expiry, in an own realtime thread */
static void *timer_thread(void *arg)
{
    (void)arg;
    if (T.rt_prio > 0) {
        struct sched_param sp = {.sched_priority = T.rt_prio};
        atomic_store(&T.rt_err, pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) + 1);
    }
    atomic_store(&T.sched_policy, sched_getscheduler(0) + 1);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    uint64_t t0 = rdx_now_ns();
    float q[RDX_AUDIO_QUANTUM * RDX_AUDIO_CH];
    for (uint64_t k = 0; k < atomic_load(&T.total_q) && !atomic_load(&T.failed); k++) {
        uint64_t due = t0 + (k * RDX_AUDIO_QUANTUM * 1000000000ull) / RDX_AUDIO_RATE;
        struct itimerspec its = {.it_value = {.tv_sec = (time_t)(due / 1000000000ull),
                                              .tv_nsec = (long)(due % 1000000000ull)}};
        timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
        uint64_t exp;
        if (read(tfd, &exp, sizeof(exp)) < 0) break;     /* blocks until due */
        uint64_t tc = rdx_now_ns();
        if (T.t_last_cycle) {
            uint64_t gap = tc - T.t_last_cycle;
            if (gap > atomic_load(&T.gap_max_ns)) atomic_store(&T.gap_max_ns, gap);
            if (gap > 4000000ull) atomic_fetch_add(&T.gaps4ms, 1);
        }
        T.t_last_cycle = tc;
        uint64_t f0 = k * RDX_AUDIO_QUANTUM;
        for (unsigned i = 0; i < RDX_AUDIO_QUANTUM; i++) {
            q[i * 2] = rdx_audio_test_sample(f0 + i, 0);
            q[i * 2 + 1] = rdx_audio_test_sample(f0 + i, 1);
        }
        post_quantum(q);
    }
    close(tfd);
    return NULL;
}

/* --mic: a new capture stream for every open. Measured on the test client (PipeWire 1.0.5): connecting the
 * same pw_stream again after pw_stream_disconnect reaches "streaming" but delivers no buffers. */
static struct pw_core *g_mic_core;

static struct pw_stream *mic_stream_new(void)
{
    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%u", RDX_AUDIO_QUANTUM, RDX_AUDIO_RATE);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_NODE_NAME, "rdx-mic-capture", PW_KEY_NODE_DESCRIPTION, "rdma-display microphone to the host",
        PW_KEY_NODE_LATENCY, lat, NULL);
    if (strcmp(T.mic, "default")) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, T.mic);
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
    }
    struct pw_stream *st = pw_stream_new(g_mic_core, "rdx-mic-capture", props);
    static struct spa_hook shook;
    spa_zero(shook);
    if (st) pw_stream_add_listener(st, &shook, &stream_events, NULL);
    return st;
}

/* --mic: open / close the capture (main thread, on the player's control messages) */
static void mic_open(void)
{
    pw_thread_loop_lock(T.loop);
    if (!T.mic_open) {
        int dflt = !strcmp(T.mic, "default");
        if (!dflt && !rdx_pw_node_by_name(&T.graph, T.mic, "Audio/Source")) {
            printf("[atx] mic: source \"%s\" is not there, the capture stays closed\n", T.mic);
        } else if (!(T.stream = mic_stream_new())) {
            printf("[atx] mic: cannot create the capture stream\n");
        } else {
            T.acc_n = 0;
            atomic_store(&T.cycles, 0); atomic_store(&T.nobuf, 0); atomic_store(&T.frames, 0);
            int r = pw_stream_connect(T.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                                      (dflt ? 0 : PW_STREAM_FLAG_DONT_RECONNECT), T.mic_params, 1);
            if (r < 0) {
                printf("[atx] mic: capture connect failed: %s\n", strerror(-r));
                pw_stream_destroy(T.stream);
                T.stream = NULL;
            } else {
                T.mic_open = 1;
                T.mic_opens++;
                printf("[atx] mic: capture opened (%s%s)\n", dflt ? "default source" : T.mic, g_muted ? ", muted" : "");
            }
        }
    }
    pw_thread_loop_unlock(T.loop);
    fflush(stdout);
}

static void mic_close(void)
{
    pw_thread_loop_lock(T.loop);
    if (T.mic_open) {
        pw_stream_destroy(T.stream);
        T.stream = NULL;
        T.mic_open = 0;
        printf("[atx] mic: capture closed (%llu cycles, %llu without a buffer, %llu frames, quanta sent %llu)\n",
               (unsigned long long)atomic_load(&T.cycles), (unsigned long long)atomic_load(&T.nobuf),
               (unsigned long long)atomic_load(&T.frames), (unsigned long long)atomic_load(&T.k));
    }
    pw_thread_loop_unlock(T.loop);
    fflush(stdout);
}

static struct pw_core *pw_connect_core(void)
{
    pw_init(NULL, NULL);
    T.loop = pw_thread_loop_new("rdx-audio-tx", NULL);
    struct pw_context *pctx = pw_context_new(pw_thread_loop_get_loop(T.loop), NULL, 0);
    if (pw_thread_loop_start(T.loop)) return NULL;
    pw_thread_loop_lock(T.loop);
    struct pw_core *core = pw_context_connect(pctx, NULL, 0);
    if (!core) { pw_thread_loop_unlock(T.loop); fprintf(stderr, "PipeWire connect failed (XDG_RUNTIME_DIR?)\n"); }
    return core;
}

/* --mic: no stream until RDX_AUDIO_CTL_MIC_OPEN */
static int pw_start_mic(void)
{
    struct pw_core *core = pw_connect_core();
    if (!core) return -1;
    if (rdx_pw_graph_start(&T.graph, T.loop, core)) { pw_thread_loop_unlock(T.loop); return -1; }
    g_mic_core = core;
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(T.mic_pod, sizeof(T.mic_pod));
    struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32, .rate = RDX_AUDIO_RATE,
                                      .channels = RDX_AUDIO_CH,
                                      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
    T.mic_params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info);
    int known = !strcmp(T.mic, "default") || rdx_pw_node_by_name(&T.graph, T.mic, "Audio/Source");
    pw_thread_loop_unlock(T.loop);
    printf("[atx] mic: source %s%s, capture closed until an app on the host records\n", T.mic,
           known ? "" : " (NOT there now: refused while it is missing)");
    return 0;
}

/* --list-sources */
static int list_sources(void)
{
    struct pw_core *core = pw_connect_core();
    if (!core) return 1;
    int r = rdx_pw_graph_start(&T.graph, T.loop, core);
    if (!r) rdx_pw_print_sources(&T.graph, stdout, "rdx-mic-");   /* not our own virtual sources */
    rdx_pw_graph_stop(&T.graph);
    pw_thread_loop_unlock(T.loop);
    pw_thread_loop_stop(T.loop);
    return r ? 1 : 0;
}

static struct pw_core *g_core;

/* --capture: a stream on the monitor of the sink in T.cap_target, or of the default sink when that is
 * empty. No node.dont-reconnect: WirePlumber links a stream without target to the default sink and
 * re-links it when the default changes or its sink disappears (pw-record / Sunshine's pulse capture
 * rely on the same). Call with the thread loop locked. */
static int cap_connect(void)
{
    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%u", RDX_AUDIO_QUANTUM, RDX_AUDIO_RATE);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "rdx-audio-tx", PW_KEY_NODE_LATENCY, lat, PW_KEY_STREAM_CAPTURE_SINK, "true", NULL);
    if (*T.cap_target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, T.cap_target);
    T.stream = pw_stream_new(g_core, "rdx-audio-tx", props);
    if (!T.stream) return -1;
    static struct spa_hook shook;
    spa_zero(shook);                     /* the previous stream (and its listener) is destroyed */
    pw_stream_add_listener(T.stream, &shook, &stream_events, NULL);
    int r = pw_stream_connect(T.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                              T.cap_params, 1);
    if (r < 0) { pw_stream_destroy(T.stream); T.stream = NULL; }
    return r;
}

/* --capture, main thread (every 100 ms): rebuild the stream on the monitor of the current default sink
 *   - after a stream error (the fixed target is gone: "target not found"), or
 *   - when no frame arrived for 500 ms although frames came before (measured with a node destroyed from
 *     outside: the stream just sits in "paused", no error), 3 s for the first frame; further attempts
 *     of the same stall are 1 s apart (no sink at all: nothing to connect to, wait quietly).
 * A named target that fails is replaced by "default". The accumulator and the quantum counter stay as
 * they are: the data to the player continues with the next quantum index, the player sees an underrun
 * at most. One log line per stall, one when the data flows again. */
static void cap_check(void)
{
    uint64_t now = rdx_now_ns(), fr = atomic_load(&T.frames);
    if (!T.cap_t_progress) T.cap_t_progress = now;
    if (fr != T.cap_frames_seen) {
        T.cap_frames_seen = fr;
        T.cap_t_progress = now;
        if (T.cap_streak) {
            printf("[atx] capture: data flows again after %u rebuild(s), quanta so far %llu\n", T.cap_streak,
                   (unsigned long long)atomic_load(&T.k));
            fflush(stdout);
            T.cap_streak = 0;
        }
    }
    int err = atomic_load(&T.cap_restart);
    uint64_t stall = now - T.cap_t_progress;
    uint64_t limit = T.cap_streak ? 1000000000ull : fr ? 500000000ull : 3000000000ull;
    if (!err && stall < limit) return;
    if (T.cap_t_restart && now - T.cap_t_restart < 250000000ull) return;
    atomic_store(&T.cap_restart, 0);
    T.cap_t_restart = now;
    char was[sizeof(T.cap_target)];
    snprintf(was, sizeof(was), "%s", *T.cap_target ? T.cap_target : "default sink");
    pw_thread_loop_lock(T.loop);
    if (T.stream) pw_stream_destroy(T.stream);
    T.stream = NULL;
    T.cap_target[0] = 0;
    int r = cap_connect();
    pw_thread_loop_unlock(T.loop);
    T.cap_restarts++;
    T.cap_t_progress = now;
    if (r < 0) atomic_store(&T.cap_restart, 1);
    if (!T.cap_streak++) {
        if (err) printf("[atx] capture: the stream of \"%s\" ended in an error, ", was);
        else printf("[atx] capture: no data from \"%s\" for %.1f s, ", was, stall / 1e9);
        if (r < 0) printf("reconnect to the default sink failed: %s (retrying)\n", strerror(-r));
        else printf("rebuilt on the monitor of the default sink (quanta so far %llu)\n", (unsigned long long)atomic_load(&T.k));
        fflush(stdout);
    }
}

static int pw_start(const char *capture)
{
    struct pw_core *core = pw_connect_core();
    if (!core) return -1;
    g_core = core;
    uint8_t pod_buf[1024];
    struct spa_pod_builder bld = capture ? (struct spa_pod_builder)SPA_POD_BUILDER_INIT(T.cap_pod, sizeof(T.cap_pod))
                                         : (struct spa_pod_builder)SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32, .rate = RDX_AUDIO_RATE,
                                      .channels = RDX_AUDIO_CH,
                                      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *params[1] = {spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info)};
    int r;
    if (capture) {
        T.cap_params[0] = params[0];
        if (strcmp(capture, "default")) snprintf(T.cap_target, sizeof(T.cap_target), "%s", capture);
        r = cap_connect();
        pw_thread_loop_unlock(T.loop);
        return r < 0 ? -1 : 0;
    }
    if (rdx_null_sink_create(&T.ns, core, T.loop, "tx")) { pw_thread_loop_unlock(T.loop); return -1; }
    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%u", RDX_AUDIO_QUANTUM, RDX_AUDIO_RATE);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, "rdx-audio-tx", PW_KEY_NODE_LATENCY, lat,
        PW_KEY_NODE_DONT_RECONNECT, "true", PW_KEY_TARGET_OBJECT, T.ns.name, NULL);
    T.stream = pw_stream_new(core, "rdx-audio-tx", props);
    static struct spa_hook shook;
    pw_stream_add_listener(T.stream, &shook, &stream_events, NULL);
    r = pw_stream_connect(T.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT, params, 1);
    float mute = 1.0f;          /* the null sink is silent anyway; this guards a wrong link */
    pw_stream_set_control(T.stream, SPA_PROP_mute, 1, &mute, 0);
    pw_thread_loop_unlock(T.loop);
    return r < 0 ? -1 : 0;
}

/* CQ thread: completions and reports */
static int drain_cq(void)
{
    struct ibv_wc wc[32];
    int nwc;
    while ((nwc = ibv_poll_cq(T.cq, 32, wc)) > 0) {
        uint64_t now = rdx_now_ns();
        for (int i = 0; i < nwc; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[i].status),
                        (unsigned long long)wc[i].wr_id);
                return -1;
            }
            if (wc[i].opcode != IBV_WC_RECV) continue;
            unsigned slot = (unsigned)(wc[i].wr_id - WR_RECV);
            struct rdx_audio_report r = T.rbuf[slot];
            struct ibv_sge sge = {.addr = (uintptr_t)&T.rbuf[slot], .length = sizeof(r), .lkey = T.rmr->lkey};
            struct ibv_recv_wr wr = {.wr_id = wc[i].wr_id, .sg_list = &sge, .num_sge = 1}, *bad;
            if (ibv_post_recv(T.qp, &wr, &bad)) { perror("repost"); return -1; }
            if (r.quantum == RDX_AUDIO_CTL_MIC_OPEN || r.quantum == RDX_AUDIO_CTL_MIC_CLOSE) {
                if (!T.mic) continue;
                if (r.quantum == RDX_AUDIO_CTL_MIC_OPEN) mic_open(); else mic_close();
                continue;
            }
            uint64_t tp = atomic_load(&T.t_post[r.quantum % TPOST_RING]);
            if (!tp || T.nrep >= T.maxrep || r.quantum + TPOST_RING <= atomic_load(&T.k)) continue;
            double round = (now - tp) / 1e6;
            T.round[T.nrep] = round;
            T.res[T.nrep] = r.residence_us / 1e3;
            T.net[T.nrep] = round - r.residence_us / 1e3;
            T.delay[T.nrep] = r.delay_us / 1e3;
            T.e2e[T.nrep] = round + r.delay_us / 1e3;
            T.nrep++;
            if (r.ppm_milli < T.ppm_min) T.ppm_min = r.ppm_milli;
            if (r.ppm_milli > T.ppm_max) T.ppm_max = r.ppm_milli;
            if (r.fill_frames < T.fill_min) T.fill_min = r.fill_frames;
            if (r.fill_frames > T.fill_max) T.fill_max = r.fill_frames;
            T.ppm_last = r.ppm_milli;
            T.underruns_last = r.underruns;
        }
    }
    return nwc < 0 ? -1 : 0;
}

/* block until the channel fires or timeout_ms passes; then drain */
static int wait_cq(int timeout_ms)
{
    struct pollfd p = {.fd = T.ch->fd, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    if (r > 0) {
        struct ibv_cq *ecq; void *ectx;
        if (ibv_get_cq_event(T.ch, &ecq, &ectx)) { perror("ibv_get_cq_event"); return -1; }
        ibv_ack_cq_events(ecq, 1);
        if (ibv_req_notify_cq(T.cq, 0)) { perror("req_notify"); return -1; }
    }
    return drain_cq();
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer_ip = NULL, *capture = NULL;
    int port = 18670, mtu = 4096, pmqos_us = 0, use_timer = 0, list = 0;
    double seconds = 300;
    T.rt_prio = 70;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"seconds", 1, 0, 's'}, {"capture", 1, 0, 'c'}, {"pm-qos-us", 1, 0, 'Q'}, {"rt-prio", 1, 0, 'T'},
        {"timer", 0, 0, 'I'}, {"mic", 1, 0, 'i'}, {"muted", 0, 0, 'u'}, {"list-sources", 0, 0, 'L'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'p': peer_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        case 'c': capture = optarg; break;
        case 'Q': pmqos_us = atoi(optarg); break;
        case 'T': T.rt_prio = atoi(optarg); break;
        case 'I': use_timer = 1; break;
        case 'i': T.mic = optarg; break;
        case 'u': g_muted = 1; break;
        case 'L': list = 1; break;
        default:  fprintf(stderr, "see the header of rdx_audio_tx.c for options\n"); return 2;
        }
    }
    if (list) return list_sources();
    if (!peer_ip) { fprintf(stderr, "need --peer\n"); return 2; }
    if (T.mic && (capture || use_timer)) { fprintf(stderr, "--mic excludes --capture and --timer\n"); return 2; }
    int qos = rdx_audio_pm_qos(pmqos_us);
    rdx_audio_test_init();
    T.capture = capture != NULL || T.mic != NULL;
    T.ppm_min = INT32_MAX; T.ppm_max = INT32_MIN; T.fill_min = INT32_MAX; T.fill_max = INT32_MIN;
    /* latency samples: the whole run, or the first 300 s of an endless one */
    T.maxrep = (long)((seconds > 0 ? seconds : 300) * 12) + 100;
    T.round = calloc((size_t)T.maxrep, sizeof(double));
    T.net = calloc((size_t)T.maxrep, sizeof(double));
    T.res = calloc((size_t)T.maxrep, sizeof(double));
    T.delay = calloc((size_t)T.maxrep, sizeof(double));
    T.e2e = calloc((size_t)T.maxrep, sizeof(double));
    atomic_store(&T.total_q, seconds > 0 ? (uint64_t)(seconds * RDX_AUDIO_RATE / RDX_AUDIO_QUANTUM) : UINT64_MAX);
    rdx_stop_signals(on_signal);
    if (T.mic) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_mute;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);
    }

    int sock = rdx_tcp_connect(peer_ip, port);
    if (sock < 0) return 1;
    if (rdx_io_all(sock, &T.hello, sizeof(T.hello), 0) || T.hello.magic != RDX_AUDIO_MAGIC ||
        T.hello.rate != RDX_AUDIO_RATE || T.hello.channels != RDX_AUDIO_CH ||
        T.hello.quantum != RDX_AUDIO_QUANTUM || T.hello.ring_frames % RDX_AUDIO_QUANTUM) {
        fprintf(stderr, "bootstrap: incompatible hello\n"); return 1;
    }
    char lip[64] = "";
    rdx_local_ip_for(peer_ip, lip, sizeof(lip));         /* the seat's link IP towards the peer */
    struct ibv_context *ctx = rdx_open_dev_ip(dev, lip);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    T.local = aligned_alloc(4096, LOCAL_Q * RDX_AUDIO_QUANTUM * RDX_AUDIO_FRAME_BYTES);
    T.rbuf = aligned_alloc(4096, 4096);
    T.mr = ibv_reg_mr(pd, T.local, LOCAL_Q * RDX_AUDIO_QUANTUM * RDX_AUDIO_FRAME_BYTES, IBV_ACCESS_LOCAL_WRITE);
    T.rmr = ibv_reg_mr(pd, T.rbuf, 4096, IBV_ACCESS_LOCAL_WRITE);
    T.ch = ibv_create_comp_channel(ctx);
    T.cq = T.ch ? ibv_create_cq(ctx, 1024, NULL, T.ch, 0) : NULL;
    T.qp = T.cq ? rdx_create_qp(pd, T.cq, 512) : NULL;
    if (!T.mr || !T.rmr || !T.qp || rdx_qp_to_init(T.qp)) return 1;
    for (unsigned i = 0; i < NRECV; i++) {
        struct ibv_sge sge = {.addr = (uintptr_t)&T.rbuf[i], .length = sizeof(struct rdx_audio_report),
                              .lkey = T.rmr->lkey};
        struct ibv_recv_wr wr = {.wr_id = WR_RECV + i, .sg_list = &sge, .num_sge = 1}, *bad;
        if (ibv_post_recv(T.qp, &wr, &bad)) { perror("post_recv"); return 1; }
    }
    struct rdx_conn mine;
    memset(&mine, 0, sizeof(mine));
    int gidx = rdx_find_gid_index_ip(ctx, lip, mine.gid);
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    mine.qpn = T.qp->qp_num;
    mine.psn = 0x7123;
    if (rdx_io_all(sock, &mine, sizeof(mine), 1)) return 1;
    if (rdx_qp_to_rtr_rts(T.qp, &T.hello.conn, gidx, mine.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    ibv_req_notify_cq(T.cq, 0);

    uint64_t t0 = rdx_now_ns(), c0 = rdx_audio_cpu_ns();
    pthread_t tth;
    if (use_timer) {
        if (pthread_create(&tth, NULL, timer_thread, NULL)) { perror("pthread_create"); return 1; }
    } else if (T.mic ? pw_start_mic() : pw_start(capture)) {
        return 1;
    }
    printf(seconds > 0 ? "[atx] connected to %s, %s, %.0f s, PM QoS %s\n"
                       : "[atx] connected to %s, %s, until stopped (%.0f), PM QoS %s\n", peer_ip,
           use_timer ? "test signal, own realtime timerfd thread (no PipeWire)" : T.mic ? "microphone on demand" :
           capture ? "PipeWire capture" : "test signal into own null sink (graph-paced)", seconds,
           qos >= 0 ? "on" : "off");
    fflush(stdout);

    uint64_t t_print = t0;
    int said_sched = 0, said_muted = g_muted;
    while (atomic_load(&T.k) < atomic_load(&T.total_q) && !atomic_load(&T.failed)) {
        if (g_stop) {
            /* end behind the quantum being assembled: the producing thread posts END */
            uint64_t k = atomic_load(&T.k);
            atomic_store(&T.total_q, k + 1);
            for (uint64_t ts = rdx_now_ns(); atomic_load(&T.k) < k + 1 && rdx_now_ns() - ts < 100000000ull;)
                if (wait_cq(10)) break;
            if (atomic_load(&T.k) < k + 1) rdx_post_imm_send(T.qp, RDX_AUDIO_IMM_END, WR_END); /* graph idle */
            break;
        }
        if (wait_cq(T.capture && !T.mic ? 100 : 500) && !g_stop) return 1;
        if (T.capture && !T.mic) cap_check();
        if (T.mic && g_muted != said_muted) {
            said_muted = g_muted;
            printf("[atx] mic: %s\n", said_muted ? "muted (silence goes to the host)" : "unmuted");
            fflush(stdout);
        }
        if (!said_sched && atomic_load(&T.sched_policy)) {
            int pol = atomic_load(&T.sched_policy) - 1;
            said_sched = 1;
            printf("[atx] PipeWire data thread: %s (rt-prio %d%s%s)\n", pol == SCHED_FIFO ? "SCHED_FIFO (realtime)" :
                   pol == SCHED_RR ? "SCHED_RR (realtime)" : "SCHED_OTHER - NOT realtime, expect stalls under load",
                   T.rt_prio, atomic_load(&T.rt_err) > 1 ? ", setschedparam: " : "",
                   atomic_load(&T.rt_err) > 1 ? strerror(atomic_load(&T.rt_err) - 1) : "");
            fflush(stdout);
        }
        uint64_t now = rdx_now_ns();
        if (now - t_print >= 10000000000ull) {
            printf("[atx] %6.0f s  quanta %llu  reports %ld  ppm %.3f  fill %d..%d  underruns %u  peak %.1f dBFS\n",
                   (now - t0) / 1e9, (unsigned long long)atomic_load(&T.k), T.nrep, T.ppm_last / 1000.0,
                   T.fill_min, T.fill_max, T.underruns_last, peak_take_db());
            fflush(stdout);
            t_print = now;
        }
        if (!T.mic && !atomic_load(&T.k) && now - t0 > 5000000000ull) { fprintf(stderr, "PipeWire delivers nothing\n"); return 1; }
    }
    double el = (rdx_now_ns() - t0) / 1e9;
    /* END already went out behind the last quantum; collect the remaining reports */
    for (uint64_t te = rdx_now_ns(); rdx_now_ns() - te < 300000000ull;)
        if (wait_cq(100)) break;
    double cpu_pct = 100.0 * (rdx_audio_cpu_ns() - c0) / 1e9 / el;
    if (use_timer) {
        pthread_join(tth, NULL);
    } else {
        pw_thread_loop_lock(T.loop);
        if (T.stream) pw_stream_destroy(T.stream);
        if (T.ns.proxy) pw_proxy_destroy(T.ns.proxy);
        if (T.mic) rdx_pw_graph_stop(&T.graph);
        pw_thread_loop_unlock(T.loop);
        pw_thread_loop_stop(T.loop);
    }

    uint64_t k = atomic_load(&T.k);
    printf("\n[atx] done: %llu quanta in %.1f s (%.2f quanta/s, nominal %.1f)\n",
           (unsigned long long)k, el, k / el, (double)RDX_AUDIO_RATE / RDX_AUDIO_QUANTUM);
    printf("[atx] cpu: %.2f %% of one core (user+sys, all threads, getrusage)\n", cpu_pct);
    if (T.mic) printf("[atx] mic: capture opened %u time(s)\n", T.mic_opens);
    if (T.capture && !T.mic) printf("[atx] capture: stream rebuilt %u time(s)\n", T.cap_restarts);
    printf("[atx] own PipeWire cycles: max gap %.3f ms, %u gaps > 4 ms\n", atomic_load(&T.gap_max_ns) / 1e6,
           atomic_load(&T.gaps4ms));
    printf("[atx] player rate correction: last %.3f ppm, range %.3f .. %.3f ppm; fill %d .. %d frames; "
           "underruns %u\n", T.ppm_last / 1000.0, T.ppm_min / 1000.0, T.ppm_max / 1000.0,
           T.fill_min, T.fill_max, T.underruns_last);
    printf("[atx] latency from %ld marker reports (sender clock, ~1 us return not subtracted):\n", T.nrep);
    rdx_report("post -> report (round)", T.round, (size_t)T.nrep);
    rdx_report("player residence", T.res, (size_t)T.nrep);
    rdx_report("round - residence = net", T.net, (size_t)T.nrep);
    rdx_report("PipeWire delay", T.delay, (size_t)T.nrep);
    rdx_report("post -> device (e2e)", T.e2e, (size_t)T.nrep);
    if (qos >= 0) close(qos);
    close(sock);
    return 0;
}
