/* SPDX-License-Identifier: MIT
 *
 * P5 audio player. Exposes a ring in host RAM as RDMA target, receives one
 * RDMA_WRITE_WITH_IMM per quantum (imm = quantum index) and plays the ring
 * through a PipeWire playback stream.
 *
 * Clock drift between sender and player is absorbed by keeping the ring fill
 * at --target-quanta (default 2 = 128 frames): a spa_dll loop on the fill
 * error yields a rate correction `corr`, applied like PipeWire's own
 * module-rtp-source does it (fixed bandwidth, default SPA_DLL_BW_MIN; the fill
 * includes frames already inside PipeWire's resampler, spa_io_rate_match.delay):
 *   resampler pw   PipeWire's adapter resampler via the SPA_IO_RateMatch area
 *                  (rate = 1/corr), used when the stream gets that area
 *   resampler src  libsamplerate (src_process, ratio = 1/corr) otherwise, or
 *                  when forced with --resampler src
 *
 * --pw-quantum 128 halves the player's wake-ups (CPU) for +1.33 ms latency; the
 * network quanta stay at 64 frames.
 *
 * Adaptive fill (default on, --fill-max-quanta 32, 0 = fixed): a device whose graph cycles come in
 * bursts or at a wandering rate (measured: a Bluetooth LDAC speaker on the test client, cycle gaps to 5.7 ms
 * at a 2.67 ms quantum, rate corrections to -2300 ppm) drains the fixed 3-quanta fill and plays a
 * crackle of 30-50 underruns per second. The first underrun sets the target at once to a stable
 * floor (--fill-jump-ms, default 25 ms; measured: the seats' audio senders of a multiseat host wake up to
 * 17 ms late, and tasting up one quantum per underrun cost a crackle each, for minutes);
 * every further underrun adds one quantum (at most every 100 ms, up to the maximum). The player
 * buffers up to the new target before it plays on (one short silence instead of a crackle); its
 * rate DLL keeps what it learned (a sender's clock may really be off). After 30 s without an underrun it gives one
 * quantum back, never below that floor once it was needed. Wired devices never underrun and keep
 * the low latency of --target-quanta.
 *
 * Safety: without --audible the player creates its OWN null sink (timer driven,
 * removed when the player exits) and plays into it, muted (tests). --audible
 * plays to the user's default device (sessions, rdx-clientd); --target NODE
 * needs --audible on top.
 *
 * Microphone (--mic NAME, rdx-hostd, the client's microphone on this host; p5/rdx_audio_mic.h):
 * the player creates the virtual source NAME, plays into it and makes it the default source
 * while it runs (the previous default comes back at the end, also on SIGTERM). The sender's
 * capture is opened on demand: when the first app records from NAME the player sends
 * RDX_AUDIO_CTL_MIC_OPEN, when the last one stops RDX_AUDIO_CTL_MIC_CLOSE; peak meters
 * (stream.monitor) do not count. Without a recording app the client's microphone stays closed
 * and the player's stream is inactive (no cycles).
 *
 * No spinning, no per-quantum wake-ups: the PipeWire process callback drains
 * the CQ non-blocking at the start of each cycle and does the playback work;
 * the main thread only prints once per second. Arrival is therefore seen at
 * cycle granularity (residence = seen -> handed over); end-to-end latency is
 * measured by the sender with its own clock and is unaffected. --pm-qos-us N holds /dev/cpu_dma_latency
 * at N us while running, against deep C-state wake-ups.
 *
 * Every RDX_AUDIO_MARK_EVERY quanta it reports to the sender: residence
 * (CQE -> handed to PipeWire, player clock), PipeWire delay to the device,
 * fill, rate correction, underruns.
 *
 *   rdx_audio_rx [--dev auto] [--bind 10.0.0.1] [--port 18670] [--mtu 4096]
 *                [--target-quanta 2] [--resampler auto|pw|src] [--pm-qos-us 0]
 *                [--log FILE] [--dll-bw 0.016] [--resample-quality 1]
 *                [--rt-prio 70] [--pw-quantum 128] [--target NODE --audible]
 *                [--mic "RDMA Display Mic"] [--fill-max-quanta 32] [--fill-jump-ms 25]
 *                [--sink-quantum N]   tests only: our own null sink forces graph quantum N
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
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <samplerate.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/utils/dll.h>

#define NRECV      1024                 /* 1.37 s of quanta: slack if a cycle stalls */
#define WR_RECV    0x200u
#define WR_REPORT  0x400u
#define TCQE_RING  8192u
#define MAX_ERR    (4.0 * RDX_AUDIO_QUANTUM)
#define RESYNC_FRAMES (8 * (int64_t)RDX_AUDIO_QUANTUM)   /* fill above target + this -> drop excess */

enum { RS_AUTO, RS_PW, RS_SRC };

struct rx {
    /* ring */
    float *ring;
    uint32_t ring_frames;
    _Atomic uint64_t write_pos;          /* frames, (k + 1) * quantum of the newest quantum */
    _Atomic uint64_t first_frame;        /* frame of the first quantum seen, +1 (0 = none yet) */
    uint64_t t_cqe[TCQE_RING];
    /* RDMA */
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    long report_unsignaled;
    /* PipeWire */
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct rdx_null_sink ns;
    struct spa_io_rate_match *rm;
    int resampler;                       /* requested mode */
    _Atomic int rs_used;                 /* RS_PW or RS_SRC once playing */
    SRC_STATE *src;
    struct spa_dll dll;
    double bw;                           /* DLL bandwidth (--dll-bw) */
    uint32_t target;
    uint64_t rp;
    int started;
    /* stats, written by the PipeWire thread */
    _Atomic int32_t fill_min, fill_max, ppm_milli;
    _Atomic uint32_t underruns, overruns, reports, resyncs;
    _Atomic int streaming;
    /* CQ state, owned by the PipeWire thread */
    uint64_t expect, gaps, bad, quanta;
    int have_first;
    _Atomic int ended;
    int efd;                             /* eventfd: wakes the main thread at END */
    _Atomic int sched_policy;
    int rt_prio;                         /* --rt-prio: SCHED_FIFO priority for the data thread, 0 = leave */
    int audible;                         /* --audible: plays to a real device at full stream volume */
    _Atomic int rt_err;
    uint64_t t_last_cycle;               /* cycle gap instrument (data thread only) */
    _Atomic uint64_t gap_max_ns;
    _Atomic uint32_t gaps4ms;            /* cycles more than 4 ms after the previous one */                  /* errno of pthread_setschedparam, +1 */            /* data thread policy seen in the first cycle, +1 */
    _Atomic uint64_t quanta_seen, gaps_seen, bad_seen;
    /* --mic: the virtual source and its demand (loop thread), restart of the fill (data thread) */
    int mic;
    struct rdx_pw_graph graph;
    struct rdx_vmic vmic;
    int mic_open;
    _Atomic int restart;
    int restarted;
    uint64_t restart_wp;
    _Atomic uint32_t mic_opens;
    _Atomic int cq_busy;
    /* adaptive fill (data thread), target_now / raises / lowers for the main thread */
    uint32_t target_base, target_max;
    uint32_t target_jump, target_floor;   /* first underrun: straight to target_jump; never lower again */
    uint64_t t_last_under, t_last_raise, t_last_lower;
    _Atomic uint32_t target_now, raises, lowers;                 /* --mic: the CQ is drained by the data thread or, while the
                                          * stream is inactive, by the main thread; never both */
};

static struct rx R;

/* --mic: SIGTERM/SIGINT end the player cleanly (the default source comes back) */
static void on_stop(int sig)
{
    (void)sig;
    atomic_store(&R.ended, 3);
    uint64_t one = 1;
    if (write(R.efd, &one, sizeof(one)) < 0) { /* the main loop polls every second anyway */ }
}

/* ---------------- PipeWire ---------------- */

static void ring_copy(float *dst, uint64_t from, uint32_t n)
{
    uint32_t pos = (uint32_t)(from % R.ring_frames);
    uint32_t first = R.ring_frames - pos < n ? R.ring_frames - pos : n;
    memcpy(dst, R.ring + (size_t)pos * RDX_AUDIO_CH, (size_t)first * RDX_AUDIO_FRAME_BYTES);
    if (first < n) memcpy(dst + (size_t)first * RDX_AUDIO_CH, R.ring, (size_t)(n - first) * RDX_AUDIO_FRAME_BYTES);
}

static void send_report(uint32_t q, uint64_t now, uint64_t delay_ns, int64_t fill, int32_t ppm_milli)
{
    struct rdx_audio_report r = {
        .quantum = q,
        .residence_us = (uint32_t)((now - R.t_cqe[q % TCQE_RING]) / 1000),
        .delay_us = (uint32_t)(delay_ns / 1000),
        .fill_frames = (int32_t)fill,
        .ppm_milli = ppm_milli,
        .underruns = atomic_load(&R.underruns),
    };
    struct ibv_sge sge = {.addr = (uintptr_t)&r, .length = sizeof(r), .lkey = 0};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = WR_REPORT;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge; wr.num_sge = 1;
    wr.send_flags = IBV_SEND_INLINE;
    if (++R.report_unsignaled >= 16) { wr.send_flags |= IBV_SEND_SIGNALED; R.report_unsignaled = 0; }
    if (ibv_post_send(R.qp, &wr, &bad) == 0) atomic_fetch_add(&R.reports, 1);
}

/* player -> sender control (--mic), from the loop thread; always signaled, it is rare */
static void send_ctl(uint32_t code)
{
    struct rdx_audio_report r = {.quantum = code};
    struct ibv_sge sge = {.addr = (uintptr_t)&r, .length = sizeof(r), .lkey = 0};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = WR_REPORT;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge; wr.num_sge = 1;
    wr.send_flags = IBV_SEND_INLINE | IBV_SEND_SIGNALED;
    if (ibv_post_send(R.qp, &wr, &bad)) perror("[arx] mic control send");
}

/* PipeWire thread, start of every cycle: take what arrived since the last
 * cycle. Non-blocking; no extra thread wakes up per quantum. */
static void drain_cq(void)
{
    struct ibv_wc wc[64];
    int nwc;
    while ((nwc = ibv_poll_cq(R.cq, 64, wc)) > 0) {
        uint64_t now = rdx_now_ns();
        for (int i = 0; i < nwc; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[i].status),
                        (unsigned long long)wc[i].wr_id);
                atomic_store(&R.ended, 2);
                return;
            }
            if (wc[i].opcode == IBV_WC_RECV) {
                if (ntohl(wc[i].imm_data) == RDX_AUDIO_IMM_END) {
                    atomic_store(&R.ended, 1);
                    uint64_t one = 1;
                    if (write(R.efd, &one, sizeof(one)) < 0) { /* main thread falls back to its 1 s poll */ }
                }
                rdx_post_zero_recv(R.qp, WR_RECV);
                continue;
            }
            if (wc[i].opcode != IBV_WC_RECV_RDMA_WITH_IMM) continue;   /* report sends */
            uint64_t k = ntohl(wc[i].imm_data);
            rdx_post_zero_recv(R.qp, WR_RECV);
            R.t_cqe[k % TCQE_RING] = now;
            if (!R.have_first) { R.have_first = 1; R.expect = k; atomic_store(&R.first_frame, k * RDX_AUDIO_QUANTUM + 1); }
            if (k != R.expect) R.gaps += k > R.expect ? k - R.expect : 1;
            R.expect = k + 1;
            R.quanta++;
            /* content check (test signal only): first and last frame of the quantum, both channels */
            if (R.mic) { atomic_store(&R.write_pos, (k + 1) * RDX_AUDIO_QUANTUM); continue; }
            uint64_t f0 = k * RDX_AUDIO_QUANTUM, f1 = f0 + RDX_AUDIO_QUANTUM - 1;
            const float *p0 = R.ring + (size_t)(f0 % R.ring_frames) * RDX_AUDIO_CH;
            const float *p1 = R.ring + (size_t)(f1 % R.ring_frames) * RDX_AUDIO_CH;
            if (p0[0] != rdx_audio_test_sample(f0, 0) || p0[1] != rdx_audio_test_sample(f0, 1) ||
                p1[0] != rdx_audio_test_sample(f1, 0) || p1[1] != rdx_audio_test_sample(f1, 1))
                R.bad++;
            atomic_store(&R.write_pos, (k + 1) * RDX_AUDIO_QUANTUM);
        }
    }
    atomic_store(&R.quanta_seen, R.quanta);
    atomic_store(&R.gaps_seen, R.gaps);
    atomic_store(&R.bad_seen, R.bad);
}

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

/* data thread: an underrun happened in a cycle that asked for n frames -> a larger target
 * (rate-limited), buffer up again. A device asking for more per cycle than the target holds gets at
 * once one cycle plus one network quantum; otherwise the target grows by one quantum. */
static void fill_underrun(uint64_t now, uint32_t n)
{
    R.t_last_under = now;
    if (!R.target_max) return;                       /* --fill-max-quanta 0: fixed fill as before */
    if (R.target < R.target_max && now - R.t_last_raise > 100000000ull) {
        uint32_t cyc = (n + RDX_AUDIO_QUANTUM - 1) / RDX_AUDIO_QUANTUM * RDX_AUDIO_QUANTUM + RDX_AUDIO_QUANTUM;
        uint32_t t = R.target + RDX_AUDIO_QUANTUM > cyc ? R.target + RDX_AUDIO_QUANTUM : cyc;
        if (t < R.target_jump) t = R.target_jump;         /* the first underrun: the stable floor at once */
        if (t > R.target_max) t = R.target_max;
        R.target = t;
        R.target_floor = R.target_jump;                    /* needed once: never give it back */
        R.t_last_raise = now;
        atomic_store(&R.target_now, R.target);
        atomic_fetch_add(&R.raises, 1);
        /* the DLL keeps its rate: a sender whose graph loses cycles under load runs slow for real
         * (measured: 747.2 instead of 750 quanta/s = -3700 ppm), and a restart at 1.0 drained
         * the fill into the next underrun; the rebuffering below makes the jump no rate error */
    }
    /* play on only once the (new) target is buffered again: one gap, not a crackle */
    R.started = 0;
    R.restarted = 1;
    R.restart_wp = R.rp;
}

/* data thread, every cycle while playing: give a quantum back after 30 s without an underrun */
static void fill_steady(uint64_t now)
{
    if (!R.target_max || R.target <= R.target_base || R.target <= R.target_floor) return;
    if (now - R.t_last_under < 30000000000ull || now - R.t_last_lower < 30000000000ull) return;
    R.target -= RDX_AUDIO_QUANTUM;
    R.t_last_lower = now;
    atomic_store(&R.target_now, R.target);
    atomic_fetch_add(&R.lowers, 1);
}

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(R.stream);
    if (!atomic_exchange(&R.cq_busy, 1)) {      /* the main thread holds it only for a moment */
        drain_cq();
        atomic_store(&R.cq_busy, 0);
    }
    if (atomic_exchange(&R.restart, 0)) {
        /* the microphone opens again: what arrived before is old, start from the next data */
        R.started = 0;
        R.restarted = 1;
        R.restart_wp = atomic_load(&R.write_pos);
    }
    {
        uint64_t tc = rdx_now_ns();
        if (R.t_last_cycle) {
            uint64_t gap = tc - R.t_last_cycle;
            if (gap > atomic_load(&R.gap_max_ns)) atomic_store(&R.gap_max_ns, gap);
            if (gap > 4000000ull) atomic_fetch_add(&R.gaps4ms, 1);
        }
        R.t_last_cycle = tc;
    }
    if (!atomic_load(&R.sched_policy)) {
        /* realtime for the data thread ourselves: PipeWire's client module-rt only asks rtkit,
         * which refuses processes outside an active login session */
        if (R.rt_prio > 0 && sched_getscheduler(0) == SCHED_OTHER) {
            struct sched_param sp = {.sched_priority = R.rt_prio};
            atomic_store(&R.rt_err, pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) + 1);
        }
        atomic_store(&R.sched_policy, sched_getscheduler(0) + 1);
    }
    if (!b) return;
    struct spa_data *d = &b->buffer->datas[0];
    float *dst = d->data;
    if (!dst) { pw_stream_queue_buffer(R.stream, b); return; }
    uint32_t maxf = d->maxsize / RDX_AUDIO_FRAME_BYTES;
    uint32_t n = b->requested ? (uint32_t)b->requested : RDX_AUDIO_QUANTUM;
    if (n > maxf) n = maxf;
    uint64_t now = rdx_now_ns();
    uint64_t wp = atomic_load(&R.write_pos), ff = atomic_load(&R.first_frame);

    if (atomic_load(&R.ended)) {             /* stream over: silence, not an underrun */
        memset(dst, 0, (size_t)n * RDX_AUDIO_FRAME_BYTES);
        goto out;
    }
    if (!R.started) {
        /* start with exactly `target` frames buffered */
        uint64_t base = R.restarted ? R.restart_wp : ff - 1;
        if ((ff || R.restarted) && wp - base >= R.target) {
            R.rp = wp - R.target;
            R.started = 1;
            atomic_store(&R.rs_used, (R.rm && R.resampler != RS_SRC) ? RS_PW : RS_SRC);
        } else {
            memset(dst, 0, (size_t)n * RDX_AUDIO_FRAME_BYTES);
            goto out;
        }
    }
    int64_t avail = (int64_t)(wp - R.rp);
    /* frames already pulled into PipeWire's resampler are still "buffered" for
     * the fill loop; without them the start looks like a deficit of ~30 frames */
    int64_t in_rs = (atomic_load(&R.rs_used) == RS_PW && R.rm) ? (int64_t)R.rm->delay : 0;
    if (avail > (int64_t)R.ring_frames - 2 * (int64_t)RDX_AUDIO_QUANTUM) {
        atomic_fetch_add(&R.overruns, 1);              /* sender lapped us: resync */
        R.rp = wp - R.target;
        avail = R.target;
    } else if (avail > (int64_t)R.target + RESYNC_FRAMES) {
        /* a burst after a stall (we already played silence for it): drop the excess at
         * once instead of resampling it away for many seconds, like module-rtp does */
        atomic_fetch_add(&R.resyncs, 1);
        R.rp = wp - R.target;
        avail = R.target;
    }
    double err = (double)R.target - (double)(avail + in_rs);
    if (err > MAX_ERR) err = MAX_ERR;
    if (err < -MAX_ERR) err = -MAX_ERR;
    double corr = spa_dll_update(&R.dll, err);
    double ratio = 1.0 / corr;
    int32_t ppm_milli = (int32_t)((ratio - 1.0) * 1e9);
    atomic_store(&R.ppm_milli, ppm_milli);
    if (avail + in_rs < atomic_load(&R.fill_min)) atomic_store(&R.fill_min, (int32_t)(avail + in_rs));
    if (avail + in_rs > atomic_load(&R.fill_max)) atomic_store(&R.fill_max, (int32_t)(avail + in_rs));

    uint64_t rp0 = R.rp;
    if (atomic_load(&R.rs_used) == RS_PW) {
        SPA_FLAG_SET(R.rm->flags, SPA_IO_RATE_MATCH_FLAG_ACTIVE);
        R.rm->rate = ratio;
        uint32_t take = avail < (int64_t)n ? (uint32_t)(avail > 0 ? avail : 0) : n;
        if (take) ring_copy(dst, R.rp, take);
        R.rp += take;
        if (take < n) {
            memset(dst + (size_t)take * RDX_AUDIO_CH, 0, (size_t)(n - take) * RDX_AUDIO_FRAME_BYTES);
            atomic_fetch_add(&R.underruns, 1);
            fill_underrun(now, n);
        }
    } else {
        /* libsamplerate: produce n output frames, consume ~n / ratio input frames */
        static float in[4 * 8192 * RDX_AUDIO_CH];
        uint32_t want = (uint32_t)(n / ratio) + 8;
        uint32_t have = avail > 0 ? (uint32_t)avail : 0;
        if (have > want) have = want;
        if (have) ring_copy(in, R.rp, have);
        SRC_DATA sd = {.data_in = in, .data_out = dst, .input_frames = have, .output_frames = n,
                       .end_of_input = 0, .src_ratio = ratio};
        if (src_process(R.src, &sd)) { memset(dst, 0, (size_t)n * RDX_AUDIO_FRAME_BYTES); sd.output_frames_gen = 0; }
        R.rp += (uint64_t)sd.input_frames_used;
        if ((uint32_t)sd.output_frames_gen < n) {
            memset(dst + (size_t)sd.output_frames_gen * RDX_AUDIO_CH, 0,
                   (size_t)(n - sd.output_frames_gen) * RDX_AUDIO_FRAME_BYTES);
            atomic_fetch_add(&R.underruns, 1);
            fill_underrun(now, n);
        }
    }

    if (R.started) fill_steady(now);
    /* marker quanta whose first frame left the ring in this cycle */
    uint64_t q = (rp0 + RDX_AUDIO_QUANTUM - 1) / RDX_AUDIO_QUANTUM;
    for (; q * RDX_AUDIO_QUANTUM < R.rp; q++) {
        if (q % RDX_AUDIO_MARK_EVERY) continue;
        struct pw_time pt;
        uint64_t delay_ns = 0;
        if (pw_stream_get_time_n(R.stream, &pt, sizeof(pt)) == 0 && pt.rate.denom) {
            delay_ns = (uint64_t)(pt.delay > 0 ? pt.delay : 0) * 1000000000ull * pt.rate.num / pt.rate.denom;
            delay_ns += (pt.buffered + (q * RDX_AUDIO_QUANTUM - rp0)) * 1000000000ull / RDX_AUDIO_RATE;
        }
        send_report((uint32_t)q, now, delay_ns, avail, ppm_milli);
    }
out:
    peak_of(dst, (size_t)n * RDX_AUDIO_CH);
    d->chunk->offset = 0;
    d->chunk->stride = RDX_AUDIO_FRAME_BYTES;
    d->chunk->size = n * RDX_AUDIO_FRAME_BYTES;
    pw_stream_queue_buffer(R.stream, b);
}

static void on_io_changed(void *data, uint32_t id, void *area, uint32_t size)
{
    (void)data; (void)size;
    if (id == SPA_IO_RateMatch) R.rm = area;
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    (void)data; (void)old;
    fprintf(stderr, "[arx] stream %s%s%s\n", pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
    atomic_store(&R.streaming, state == PW_STREAM_STATE_STREAMING);
    if (R.mic && R.vmic.stream_node == SPA_ID_INVALID &&
        (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)) {
        R.vmic.stream_node = pw_stream_get_node_id(R.stream);
        rdx_vmic_update(&R.vmic);
    }
    if (state == PW_STREAM_STATE_STREAMING && R.audible) {
        /* the session manager restores a per-stream volume it remembered (earlier test runs
         * left 0.0); an audible session always starts at full stream volume, the user's
         * device volume stays the one to turn */
        float vol = 1.0f, ch[2] = {1.0f, 1.0f};
        pw_stream_set_control(R.stream, SPA_PROP_volume, 1, &vol, SPA_PROP_channelVolumes, 2, ch, 0);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .io_changed = on_io_changed,
    .process = on_process,
};

/* --mic: an app started or stopped recording from the virtual source (loop thread) */
static void mic_use(int consumers, void *user)
{
    (void)user;
    int open = consumers > 0;
    printf("[arx] mic: %d recording app(s)%s\n", consumers,
           open == R.mic_open ? "" : open ? ", capture opened on the sender" : ", capture closed on the sender");
    fflush(stdout);
    if (open == R.mic_open) return;
    R.mic_open = open;
    if (open) { atomic_store(&R.restart, 1); atomic_fetch_add(&R.mic_opens, 1); }
    /* every capture starts clean: the buffer queued in the last cycle before the stream went
     * inactive would otherwise reach the next recording first (measured: 128 frames, 2.7 ms, of
     * the previous capture). Flush (drop, no drain) at the close and again before reactivating. */
    pw_stream_flush(R.stream, false);
    pw_stream_set_active(R.stream, open);
    send_ctl(open ? RDX_AUDIO_CTL_MIC_OPEN : RDX_AUDIO_CTL_MIC_CLOSE);
}

static void mic_graph_changed(void *user)
{
    (void)user;
    rdx_vmic_update(&R.vmic);
}

static const char *rs_quality = "1";   /* --resample-quality: corrections are ppm-small */
static uint32_t pw_quantum = 2 * RDX_AUDIO_QUANTUM; /* --pw-quantum: graph quantum of our node; 128 keeps the player below 2 % CPU on the i7-8700K, 64 = lowest latency */
static int no_mute;                     /* --no-mute: only for measuring the cost of the safety mute */

static int pw_start(const char *target, int audible, const char *mic, int port)
{
    R.audible = audible;
    pw_init(NULL, NULL);
    R.loop = pw_thread_loop_new("rdx-audio-rx", NULL);
    struct pw_context *pctx = pw_context_new(pw_thread_loop_get_loop(R.loop), NULL, 0);
    if (pw_thread_loop_start(R.loop)) return -1;
    pw_thread_loop_lock(R.loop);
    struct pw_core *core = pw_context_connect(pctx, NULL, 0);
    if (!core) { pw_thread_loop_unlock(R.loop); fprintf(stderr, "PipeWire connect failed (XDG_RUNTIME_DIR?)\n"); return -1; }

    if (mic) {
        /* the client's microphone: our own virtual source, linked below by rdx_vmic_update */
        if (rdx_pw_graph_start(&R.graph, R.loop, core) || rdx_vmic_create(&R.vmic, &R.graph, mic, port)) {
            pw_thread_loop_unlock(R.loop);
            return -1;
        }
        R.graph.changed = mic_graph_changed;
        rdx_vmic_take_default(&R.vmic);
    } else if (!target && audible) {
        /* session use (rdx-clientd passes --audible): play to the user's default device */
    } else if (!target) {
        /* tests: own inaudible sink, timer driven, gone when we disconnect */
        if (rdx_null_sink_create(&R.ns, core, R.loop, "rx")) { pw_thread_loop_unlock(R.loop); return -1; }
        target = R.ns.name;
    } else if (!audible) {
        pw_thread_loop_unlock(R.loop);
        fprintf(stderr, "--target needs --audible (it may play to a real device)\n");
        return -1;
    }

    char lat[32];
    snprintf(lat, sizeof(lat), "%u/%u", pw_quantum, RDX_AUDIO_RATE);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, mic ? "Communication" : "Music",
        PW_KEY_NODE_NAME, mic ? "rdx-mic-player" : "rdx-audio-rx", PW_KEY_NODE_LATENCY, lat, "resample.quality", rs_quality, NULL);
    /* a fixed target stays fixed; the default device may change (e.g. a Bluetooth speaker
     * connects), then the session manager moves the stream along */
    if (target && !mic) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
    }
    /* --mic: nobody but rdx_vmic_update links this stream (into the virtual source only) */
    if (mic) pw_properties_set(props, PW_KEY_NODE_AUTOCONNECT, "false");
    R.stream = pw_stream_new(core, "rdx-audio-rx", props);
    static struct spa_hook shook;
    pw_stream_add_listener(R.stream, &shook, &stream_events, NULL);
    uint8_t pod_buf[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32, .rate = RDX_AUDIO_RATE,
                                      .channels = RDX_AUDIO_CH,
                                      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *params[1] = {spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info)};
    int r = pw_stream_connect(R.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                              (mic ? PW_STREAM_FLAG_INACTIVE : PW_STREAM_FLAG_AUTOCONNECT) | PW_STREAM_FLAG_MAP_BUFFERS |
                              PW_STREAM_FLAG_RT_PROCESS | (target ? PW_STREAM_FLAG_DONT_RECONNECT : 0),
                              params, 1);
    if (mic) {
        /* links appear with the stream's ports; an app already recording opens the capture */
        R.vmic.on_use = mic_use;
        rdx_pw_graph_sync(&R.graph);
        rdx_vmic_update(&R.vmic);
    } else if (!audible && !no_mute) {
        /* second safety: even a wrong link stays silent */
        float mute = 1.0f;
        pw_stream_set_control(R.stream, SPA_PROP_mute, 1, &mute, 0);
    }
    pw_thread_loop_unlock(R.loop);
    return r < 0 ? -1 : 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "10.0.0.1", *target = NULL, *logpath = NULL, *mic = NULL;
    int port = 18670, mtu = 4096, audible = 0, pmqos_us = 0;
    R.resampler = RS_AUTO;
    R.rt_prio = 70;
    uint32_t target_q = 2, fill_max_q = 32;
    double fill_jump_ms = 25.0;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"target-quanta", 1, 0, 'q'}, {"resampler", 1, 0, 'r'}, {"pm-qos-us", 1, 0, 'Q'},
        {"log", 1, 0, 'l'}, {"target", 1, 0, 't'}, {"audible", 0, 0, 'A'}, {"dll-bw", 1, 0, 'w'},
        {"resample-quality", 1, 0, 'R'}, {"rt-prio", 1, 0, 'T'}, {"pw-quantum", 1, 0, 'U'},
        {"no-mute", 0, 0, 'M'}, {"mic", 1, 0, 'i'}, {"fill-max-quanta", 1, 0, 'X'},
        {"sink-quantum", 1, 0, 'K'}, {"fill-jump-ms", 1, 0, 'J'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'q': target_q = (uint32_t)atoi(optarg); break;
        case 'r': R.resampler = !strcmp(optarg, "pw") ? RS_PW : !strcmp(optarg, "src") ? RS_SRC : RS_AUTO; break;
        case 'Q': pmqos_us = atoi(optarg); break;
        case 'l': logpath = optarg; break;
        case 't': target = optarg; break;
        case 'A': audible = 1; break;
        case 'w': R.bw = atof(optarg); break;
        case 'R': rs_quality = optarg; break;
        case 'T': R.rt_prio = atoi(optarg); break;
        case 'U': pw_quantum = (uint32_t)atoi(optarg); break;
        case 'M': no_mute = 1; break;
        case 'i': mic = optarg; break;
        case 'X': fill_max_q = (uint32_t)atoi(optarg); break;
        case 'J': fill_jump_ms = atof(optarg); break;
        case 'K': rdx_null_sink_quantum = atoi(optarg); break;     /* tests: own null sink only */
        default:  fprintf(stderr, "see the header of rdx_audio_rx.c for options\n"); return 2;
        }
    }
    if (pw_quantum < RDX_AUDIO_QUANTUM) pw_quantum = RDX_AUDIO_QUANTUM;
    /* keep at least one network quantum beyond what one graph cycle takes */
    if (target_q * RDX_AUDIO_QUANTUM < pw_quantum + RDX_AUDIO_QUANTUM)
        target_q = (pw_quantum + RDX_AUDIO_QUANTUM) / RDX_AUDIO_QUANTUM;
    R.target = target_q * RDX_AUDIO_QUANTUM;
    R.target_base = R.target;
    R.target_max = fill_max_q ? (fill_max_q > target_q ? fill_max_q : target_q) * RDX_AUDIO_QUANTUM : 0;
    /* the floor after the first underrun, in whole network quanta, within base .. max */
    R.target_jump = (uint32_t)(fill_jump_ms > 0 ? fill_jump_ms * RDX_AUDIO_RATE / 1000.0 : 0);
    R.target_jump = (R.target_jump + RDX_AUDIO_QUANTUM - 1) / RDX_AUDIO_QUANTUM * RDX_AUDIO_QUANTUM;
    if (R.target_jump > R.target_max) R.target_jump = R.target_max;
    atomic_store(&R.target_now, R.target);
    R.efd = eventfd(0, EFD_CLOEXEC);
    rdx_audio_test_init();
    R.ring_frames = RDX_AUDIO_RING_FRAMES;
    atomic_store(&R.fill_min, INT32_MAX);
    atomic_store(&R.fill_max, INT32_MIN);
    spa_dll_init(&R.dll);
    if (R.bw <= 0) R.bw = SPA_DLL_BW_MIN;
    spa_dll_set_bw(&R.dll, R.bw, RDX_AUDIO_QUANTUM, RDX_AUDIO_RATE);
    int serr = 0;
    R.src = src_new(SRC_SINC_FASTEST, RDX_AUDIO_CH, &serr);
    if (!R.src) { fprintf(stderr, "src_new: %s\n", src_strerror(serr)); return 1; }
    FILE *logf = logpath ? fopen(logpath, "w") : NULL;
    if (logf) fprintf(logf, "t_s fill_min fill_max ppm underruns overruns gaps bad dll_bw resyncs\n");

    size_t ring_bytes = (size_t)R.ring_frames * RDX_AUDIO_FRAME_BYTES;
    R.ring = aligned_alloc(4096, ring_bytes);
    memset(R.ring, 0, ring_bytes);

    struct ibv_context *ctx = rdx_open_dev_ip(dev, bind_ip);   /* dev "auto": the device carrying bind_ip */
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_mr *mr = ibv_reg_mr(pd, R.ring, ring_bytes, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    int qos = rdx_audio_pm_qos(pmqos_us);
    R.cq = ibv_create_cq(ctx, 4096, NULL, NULL, 0);
    R.qp = R.cq ? rdx_create_qp_inline(pd, R.cq, NRECV + 64, 64) : NULL;
    if (!mr || !R.qp || rdx_qp_to_init(R.qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(R.qp, WR_RECV)) { perror("post_recv"); return 1; }


    struct rdx_audio_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_AUDIO_MAGIC;
    hello.rate = RDX_AUDIO_RATE; hello.channels = RDX_AUDIO_CH; hello.quantum = RDX_AUDIO_QUANTUM;
    hello.ring_frames = R.ring_frames;
    hello.base_addr = (uintptr_t)R.ring; hello.rkey = mr->rkey;
    int gidx = rdx_find_gid_index_ip(ctx, bind_ip, hello.conn.gid);   /* the GID of the seat's link IP */
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    hello.conn.qpn = R.qp->qp_num;
    hello.conn.psn = 0x7321;
    int sock = rdx_tcp_listen_accept_as(bind_ip, port, "arx");
    if (sock < 0) return 1;
    struct rdx_conn peer;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) || rdx_io_all(sock, &peer, sizeof(peer), 0)) return 1;
    if (rdx_qp_to_rtr_rts(R.qp, &peer, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    /* the stream (and with it the CQ draining) starts only now, so no data waits in a dead CQ */
    R.mic = mic != NULL;
    if (mic) rdx_stop_signals(on_stop);     /* the default source must come back on SIGTERM */
    if (pw_start(target, audible, mic, port)) {
        if (mic) { pw_thread_loop_lock(R.loop); rdx_vmic_restore_default(&R.vmic); pw_thread_loop_unlock(R.loop); }
        return 1;
    }
    printf("[arx] connected, target fill %u frames (%u quanta), resampler request %s, CQ drained in the "
           "PipeWire cycle, PM QoS %s\n", R.target, target_q, R.resampler == RS_PW ? "pw" : R.resampler == RS_SRC ? "src" : "auto",
           qos >= 0 ? "on" : "off");
    fflush(stdout);

    uint64_t t0 = rdx_now_ns(), c0 = rdx_audio_cpu_ns();
    int said_rs = 0;
    /* the main thread only reports, once per second; all data work runs in the PipeWire cycle */
    struct pollfd epfd = {.fd = R.efd, .events = POLLIN};
    int said_sched = 0;
    long last_sec = -1;
    uint32_t said_raises = 0, said_lowers = 0;
    while (!atomic_load(&R.ended)) {
        poll(&epfd, 1, R.mic ? 200 : 1000);
        /* --mic: while no app records the stream is inactive and nothing drains the CQ in the
         * PipeWire cycle; the sender's END (it stops) must still be seen */
        if (R.mic && !atomic_load(&R.streaming) && !atomic_exchange(&R.cq_busy, 1)) {
            drain_cq();
            atomic_store(&R.cq_busy, 0);
        }
        if (!said_sched && atomic_load(&R.sched_policy)) {
            int pol = atomic_load(&R.sched_policy) - 1;
            said_sched = 1;
            printf("[arx] PipeWire data thread: %s (rt-prio %d%s%s)\n", pol == SCHED_FIFO ? "SCHED_FIFO (realtime)" :
                   pol == SCHED_RR ? "SCHED_RR (realtime)" : "SCHED_OTHER - NOT realtime, expect stalls under load",
                   R.rt_prio, atomic_load(&R.rt_err) > 1 ? ", setschedparam: " : "",
                   atomic_load(&R.rt_err) > 1 ? strerror(atomic_load(&R.rt_err) - 1) : "");
        }
        uint64_t now = rdx_now_ns();
        if (!said_rs && atomic_load(&R.rs_used)) {
            said_rs = 1;
            printf("[arx] playing, resampler in use: %s (RateMatch IO %s)\n",
                   atomic_load(&R.rs_used) == RS_PW ? "PipeWire adapter (SPA_IO_RateMatch)" : "libsamplerate",
                   R.rm ? "present" : "absent");
        }
        double ts = (now - t0) / 1e9;
        if (atomic_load(&R.raises) != said_raises || atomic_load(&R.lowers) != said_lowers) {
            int up = atomic_load(&R.raises) != said_raises;
            said_raises = atomic_load(&R.raises);
            said_lowers = atomic_load(&R.lowers);
            printf("[arx] %6.1f s  fill target %u frames (%.1f ms) after %s (raised %u, lowered %u times)\n", ts,
                   atomic_load(&R.target_now), atomic_load(&R.target_now) * 1000.0 / RDX_AUDIO_RATE,
                   up ? "underruns" : "30 s without one", said_raises, said_lowers);
        }
        if ((long)ts == last_sec) continue;   /* the stats stay once per second (the mic poll is 200 ms) */
        last_sec = (long)ts;
        int32_t fmin = atomic_exchange(&R.fill_min, INT32_MAX), fmax = atomic_exchange(&R.fill_max, INT32_MIN);
        if (logf) {
            fprintf(logf, "%.1f %d %d %.3f %u %u %llu %llu %.4f %u\n", ts, fmin, fmax,
                    atomic_load(&R.ppm_milli) / 1000.0, atomic_load(&R.underruns), atomic_load(&R.overruns),
                    (unsigned long long)atomic_load(&R.gaps_seen), (unsigned long long)atomic_load(&R.bad_seen), R.bw,
                    atomic_load(&R.resyncs));
            fflush(logf);
        }
        if (last_sec % 10 == 0)
            printf("[arx] %5.0f s  quanta %llu  fill %d..%d  ppm %.3f  underruns %u  overruns %u  gaps %llu  bad %llu  peak %.1f dBFS\n",
                   ts, (unsigned long long)atomic_load(&R.quanta_seen), fmin, fmax,
                   atomic_load(&R.ppm_milli) / 1000.0, atomic_load(&R.underruns), atomic_load(&R.overruns),
                   (unsigned long long)atomic_load(&R.gaps_seen), (unsigned long long)atomic_load(&R.bad_seen),
                   peak_take_db());
        fflush(stdout);
    }
    double el = (rdx_now_ns() - t0) / 1e9, cpu_pct = 100.0 * (rdx_audio_cpu_ns() - c0) / 1e9 / el;
    pw_thread_loop_lock(R.loop);
    if (R.mic) {
        R.graph.changed = NULL;
        R.vmic.on_use = NULL;
        rdx_vmic_restore_default(&R.vmic);
    }
    pw_stream_destroy(R.stream);
    if (R.mic) { rdx_vmic_destroy(&R.vmic); rdx_pw_graph_sync(&R.graph); rdx_pw_graph_stop(&R.graph); }
    if (R.ns.proxy) pw_proxy_destroy(R.ns.proxy);
    pw_thread_loop_unlock(R.loop);
    if (atomic_load(&R.ended) == 2) return 1;
    pw_thread_loop_stop(R.loop);
    printf("\n[arx] fill target at the end %u frames (%.1f ms), raised %u, lowered %u times\n",
           atomic_load(&R.target_now), atomic_load(&R.target_now) * 1000.0 / RDX_AUDIO_RATE,
           atomic_load(&R.raises), atomic_load(&R.lowers));
    printf("\n[arx] done: quanta %llu, gaps %llu, content errors %llu, underruns %u, overruns %u, resyncs %u, "
           "reports %u, last ppm %.3f\n", (unsigned long long)R.quanta, (unsigned long long)R.gaps,
           (unsigned long long)R.bad, atomic_load(&R.underruns), atomic_load(&R.overruns), atomic_load(&R.resyncs),
           atomic_load(&R.reports),
           atomic_load(&R.ppm_milli) / 1000.0);
    printf("[arx] cpu: %.2f %% of one core over %.1f s (user+sys, all threads, getrusage)\n", cpu_pct, el);
    printf("[arx] own PipeWire cycles: max gap %.3f ms, %u gaps > 4 ms\n", atomic_load(&R.gap_max_ns) / 1e6,
           atomic_load(&R.gaps4ms));
    if (R.mic) printf("[arx] mic: capture opened %u time(s)%s\n", atomic_load(&R.mic_opens),
                      atomic_load(&R.ended) == 3 ? ", ended by signal" : "");
    if (logf) fclose(logf);
    if (qos >= 0) close(qos);
    close(sock);
    return 0;
}
