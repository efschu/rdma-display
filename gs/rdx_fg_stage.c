/* SPDX-License-Identifier: MIT
 * Frame generation on a helper GPU inside rdx_gs_src: see rdx_fg_stage.h. */
#define _GNU_SOURCE
#include "rdx_fg_stage.h"
#include "rdx_fg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define FGS_GEN_BUFS 3
#define FGS_Q 16
#define FGS_SAMPLES 4096
#define FGS_LEAD_RING 64
#define OWN_FREE (-1)
#define OWN_GEN 1000              /* sink slot carries generated buffer (owner - OWN_GEN) */

enum { GB_FREE, GB_HELPER, GB_QUEUED, GB_POSTED };

struct fgs_pend {                 /* a new picture of gamescope, in order */
    uint32_t slot;                /* its gamescope slot */
    int job;                      /* went to the helper */
    int gb;                       /* generated buffer the job writes, -1 none */
    int done, generated, dropped;
};

struct fgs_out {                  /* what goes out next, in order */
    int gen;                      /* 1 = generated buffer idx, 0 = gamescope slot idx */
    uint32_t idx;
    int hold;                     /* a picture behind its generated frame: waits hold_ns after it */
};

struct rdx_fgs {
    struct rdx_fgs_cfg c;
    struct rdx_srcsess *S;
    struct rdx_gsbuf *slots;      /* rdx_gs_src's */
    struct rdx_fg *fg;
    int slot_id[RDX_MAX_SLOTS];   /* rdx_fg buffer id of each host slot (HOST kind) */
    int staging_id;               /* VRAM kind: the host buffer the CUDA copy lands in */
    struct rdx_gsbuf staging;
    uint8_t *sig_rows;            /* VRAM kind: sampled rows for the picture signature */
    struct rdx_gsbuf gen[FGS_GEN_BUFS];
    int gen_id[FGS_GEN_BUFS], gen_state[FGS_GEN_BUFS];
    int sink_owner[RDX_MAX_SLOTS];
    struct fgs_pend pend[FGS_Q];
    int p_head, p_n;
    struct fgs_out outq[FGS_Q];
    int o_head, o_n;
    uint64_t t_last_post;
    uint64_t last_sig, sig_seq;
    int have_sig;
    uint64_t t_last_new;
    double ival_ns;               /* EWMA of the time between new pictures */
    int fg_on;                    /* the app is slower than the sink: frames are generated */
    int warm;                     /* new pictures since the start / a detach */
    long lead_seq[FGS_LEAD_RING];
    unsigned char lead_flag[FGS_LEAD_RING];
    int lead_i;
    long n_dup, n_new, n_pass, n_real, n_gen, n_held, n_history, n_no_buffer, n_failed, n_switch;
    double *total_ms, *gpu_ms;
    int n_samples;
    int ep, tfd;                  /* epoll over rdx_fg's eventfd and the hold timer: rdx_fgs_fd */
};

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* a host buffer into the helper's device: as a host pointer, else (AMD/Intel helper) as a
 * dma-buf of the same memfd pages */
static int import_host(struct rdx_fg *fg, struct rdx_gsbuf *b)
{
    int id = rdx_fg_import(fg, b->host, b->size);
    if (id >= 0) return id;
    int fd = rdx_gsbuf_dmabuf(b);
    if (fd < 0) return -1;
    id = rdx_fg_import_dmabuf(fg, fd, b->host, b->size);
    if (id == -1) close(fd);
    return id < 0 ? -1 : id;
}

static void arm(struct rdx_fgs *f, uint64_t ns)
{
    struct itimerspec its = {.it_value = {.tv_sec = (time_t)(ns / 1000000000ull), .tv_nsec = (long)(ns % 1000000000ull)}};
    timerfd_settime(f->tfd, 0, &its, NULL);
}

struct rdx_fgs *rdx_fgs_open(const struct rdx_fgs_cfg *c, struct rdx_srcsess *S, struct rdx_gsbuf *buf)
{
    if (c->sink_slots < 3 || c->sink_slots > RDX_MAX_SLOTS) {
        fprintf(stderr, "[fg] frame generation needs a sink with 3 slots or more (it has %u): off\n", c->sink_slots);
        return NULL;
    }
    struct rdx_fgs *f = calloc(1, sizeof(*f));
    f->c = *c;
    f->S = S;
    f->slots = buf;
    f->staging_id = -1;
    f->ep = f->tfd = -1;
    for (int k = 0; k < RDX_MAX_SLOTS; k++) f->sink_owner[k] = OWN_FREE;
    char err[256] = "";
    struct rdx_fg_cfg fc = {.pci = c->helper_pci, .width = c->width, .height = c->height, .stride = c->stride,
                            .quality = c->quality, .high_priority = 1};
    f->fg = rdx_fg_open(&fc, err, sizeof(err));
    if (!f->fg) {
        fprintf(stderr, "[fg] frame generation off: %s\n", err);
        free(f);
        return NULL;
    }
    size_t bytes = (size_t)c->stride * c->height;
    int ok = 1;
    for (int i = 0; i < FGS_GEN_BUFS && ok; i++) {
        ok = !rdx_gsbuf_alloc(&f->gen[i], RDX_GSBUF_HOST, c->cuda_ord, bytes, S->pd) &&
             (f->gen_id[i] = import_host(f->fg, &f->gen[i])) >= 0;
        f->gen_state[i] = GB_FREE;
    }
    if (ok && c->buf_kind == RDX_GSBUF_HOST) {
        for (uint32_t s = 0; s < c->sink_slots && ok; s++) ok = (f->slot_id[s] = import_host(f->fg, &buf[s])) >= 0;
    } else if (ok) {
#ifdef RDX_FG_CUDA
        ok = !rdx_gsbuf_alloc(&f->staging, RDX_GSBUF_HOST, c->cuda_ord, bytes, S->pd) &&
             cuMemHostRegister(f->staging.host, f->staging.size, CU_MEMHOSTREGISTER_PORTABLE) == CUDA_SUCCESS &&
             (f->staging_id = import_host(f->fg, &f->staging)) >= 0 &&
             (f->sig_rows = malloc((size_t)c->stride * 16)) != NULL;
#else
        ok = 0;
#endif
    }
    if (ok) {
        f->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        f->ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event e1 = {.events = EPOLLIN, .data.u32 = 1}, e2 = {.events = EPOLLIN, .data.u32 = 2};
        ok = f->tfd >= 0 && f->ep >= 0 && !epoll_ctl(f->ep, EPOLL_CTL_ADD, rdx_fg_fd(f->fg), &e1) &&
             !epoll_ctl(f->ep, EPOLL_CTL_ADD, f->tfd, &e2);
    }
    if (!ok) {
        fprintf(stderr, "[fg] frame generation off: buffers for the helper GPU could not be set up\n");
        rdx_fgs_close(f);
        return NULL;
    }
    f->total_ms = calloc(FGS_SAMPLES, sizeof(double));
    f->gpu_ms = calloc(FGS_SAMPLES, sizeof(double));
    printf("[fg] frame generation on %s: %u sink slots shared by gamescope and the generated frames, %s\n",
           rdx_fg_describe(f->fg), c->sink_slots,
           c->buf_kind == RDX_GSBUF_HOST ? "the helper reads the host slots directly"
                                         : "VRAM slots staged to host RAM by CUDA");
    fflush(stdout);
    return f;
}

int rdx_fgs_fd(const struct rdx_fgs *f) { return f ? f->ep : -1; }

/* a signature of the picture in gamescope slot `slot`: 64 rows x 64 pixels (VRAM: 16 rows,
 * copied by CUDA). Equal signatures = gamescope composited the same picture again. */
static uint64_t frame_sig(struct rdx_fgs *f, uint32_t slot)
{
    uint32_t w = f->c.width, h = f->c.height, rows = 64;
    const uint8_t *base = f->slots[slot].host;
    size_t stride = f->c.stride;
#ifdef RDX_FG_CUDA
    if (f->c.buf_kind != RDX_GSBUF_HOST) {
        struct rdx_gsbuf *b = &f->slots[slot];
        rows = 16;
        int ok = cuCtxPushCurrent(b->vm.ctx) == CUDA_SUCCESS;
        for (uint32_t r = 0; ok && r < rows; r++)
            ok = cuMemcpyDtoH(f->sig_rows + r * stride, b->vm.ptr + (CUdeviceptr)((h - 1) * r / (rows - 1)) * stride,
                              stride) == CUDA_SUCCESS;
        CUcontext dummy;
        cuCtxPopCurrent(&dummy);
        if (!ok) return ++f->sig_seq;                 /* unknown: never "the same" */
        base = f->sig_rows;
    }
#endif
    if (!base) return ++f->sig_seq;
    uint64_t sig = 1469598103934665603ull;
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t y = rows == 64 ? (h - 1) * r / (rows - 1) : r;
        const uint32_t *row = (const uint32_t *)(base + (size_t)y * stride);
        for (uint32_t i = 0; i < 64; i++) sig = (sig ^ (row[(w - 1) * i / 63] & 0x00ffffffu)) * 1099511628211ull;
    }
    return sig;
}

static void note_lead(struct rdx_fgs *f, long seq, int lead)
{
    f->lead_seq[f->lead_i % FGS_LEAD_RING] = seq;
    f->lead_flag[f->lead_i % FGS_LEAD_RING] = (unsigned char)lead;
    f->lead_i++;
}

int rdx_fgs_lead_frame(const struct rdx_fgs *f, long seq)
{
    if (!f) return 1;
    for (int i = 0; i < FGS_LEAD_RING; i++)
        if (f->lead_seq[i] == seq) return f->lead_flag[i];
    return 1;
}

/* how long a picture waits behind its generated frame: half the app's frame time, in whole sink
 * periods (60 fps on 120 Hz: 1 period, on 240 Hz: 2) */
static uint64_t hold_ns(const struct rdx_fgs *f)
{
    double p = f->S->period_ns > 0 ? f->S->period_ns : 4166667.0;
    double k = f->ival_ns / 2 / p;
    long n = (long)(k + 0.5);
    if (n < 1) n = 1;
    return (uint64_t)(n * p);
}

/* post what is due into free sink slots */
static int pump(struct rdx_fgs *f, long *seq)
{
    while (f->o_n) {
        struct fgs_out *o = &f->outq[f->o_head];
        uint64_t t = now_ns();
        if (o->hold) {
            uint64_t due = f->t_last_post + hold_ns(f);
            if (t < due) { arm(f, due - t); return 0; }
        }
        int k = -1;
        for (uint32_t i = 0; i < f->c.sink_slots && k < 0; i++)
            if (f->sink_owner[i] == OWN_FREE) k = (int)i;
        if (k < 0) return 0;                          /* the next credit pumps again */
        struct rdx_gsbuf *b = o->gen ? &f->gen[o->idx] : &f->slots[o->idx];
        long s0 = *seq;
        if (rdx_srcsess_post(f->S, (uint32_t)k, s0, b->mr, rdx_gsbuf_addr(b), f->c.stride * f->c.height)) return -1;
        (*seq)++;
        note_lead(f, s0, !o->hold);
        f->sink_owner[k] = o->gen ? OWN_GEN + (int)o->idx : (int)o->idx;
        if (o->gen) { f->gen_state[o->idx] = GB_POSTED; f->n_gen++; }
        else { f->n_real++; if (o->hold) f->n_held++; }
        f->t_last_post = t;
        f->o_head = (f->o_head + 1) % FGS_Q;
        f->o_n--;
    }
    return 0;
}

static void push_out(struct rdx_fgs *f, int gen, uint32_t idx, int hold)
{
    struct fgs_out *o = &f->outq[(f->o_head + f->o_n) % FGS_Q];   /* at most one per gamescope slot + gen buffer */
    *o = (struct fgs_out){.gen = gen, .idx = idx, .hold = hold};
    f->o_n++;
}

/* finished pictures (in order) into the output queue */
static void drain(struct rdx_fgs *f)
{
    while (f->p_n) {
        struct fgs_pend *p = &f->pend[f->p_head];
        if (p->job && !p->done) break;
        f->p_head = (f->p_head + 1) % FGS_Q;
        f->p_n--;
        if (p->dropped) {
            if (p->gb >= 0) f->gen_state[p->gb] = GB_FREE;
            continue;
        }
        if (p->job && p->generated && p->gb >= 0) {
            f->gen_state[p->gb] = GB_QUEUED;
            push_out(f, 1, (uint32_t)p->gb, 0);
            push_out(f, 0, p->slot, 1);
        } else {
            if (p->gb >= 0) f->gen_state[p->gb] = GB_FREE;
            if (p->job && p->gb < 0) f->n_no_buffer++;
            else if (p->job) f->n_history++;
            push_out(f, 0, p->slot, 0);
        }
    }
}

int rdx_fgs_ready(struct rdx_fgs *f, uint32_t slot, long *seq)
{
    uint64_t sig = frame_sig(f, slot);
    if (f->have_sig && sig == f->last_sig) { f->n_dup++; return 1; }
    f->have_sig = 1;
    f->last_sig = sig;
    f->n_new++;
    f->warm++;
    uint64_t t = now_ns();
    if (f->t_last_new) {
        double d = (double)(t - f->t_last_new);
        f->ival_ns = f->ival_ns > 0 ? 0.8 * f->ival_ns + 0.2 * d : d;
    }
    f->t_last_new = t;
    double period = f->S->period_ns > 0 ? f->S->period_ns : 4166667.0;
    int was = f->fg_on;
    /* decided only after 30 pictures: gamescope's first frames come slowly while the app starts */
    if (!f->fg_on && f->warm > 30 && f->ival_ns > 1.5 * period) f->fg_on = 1;   /* room between the pictures */
    else if (f->fg_on && f->ival_ns < 1.25 * period) f->fg_on = 0;     /* the app keeps up with the sink */
    if (f->fg_on != was) {
        f->n_switch++;
        if (f->fg_on) rdx_fg_reset(f->fg);       /* the first picture after a pause only enters the history */
    }
    if (f->p_n >= FGS_Q) return -1;              /* each one holds a gamescope slot: cannot happen */
    struct fgs_pend *p = &f->pend[(f->p_head + f->p_n) % FGS_Q];
    *p = (struct fgs_pend){.slot = slot, .gb = -1};
    if (f->fg_on) {
        for (int i = 0; i < FGS_GEN_BUFS && p->gb < 0; i++)
            if (f->gen_state[i] == GB_FREE) p->gb = i;
        int dst = p->gb >= 0 ? f->gen_id[p->gb] : -1;
        int r;
#ifdef RDX_FG_CUDA
        if (f->c.buf_kind != RDX_GSBUF_HOST)
            r = rdx_fg_submit_cuda(f->fg, f->slots[slot].vm.ctx, f->slots[slot].vm.ptr, f->staging_id, dst, slot);
        else
#endif
            r = rdx_fg_submit(f->fg, f->slot_id[slot], dst, slot);
        if (r == 0) {
            p->job = 1;
            if (p->gb >= 0) f->gen_state[p->gb] = GB_HELPER;
        } else {
            p->gb = -1;
            f->n_failed++;
        }
    } else {
        f->n_pass++;
    }
    f->p_n++;
    drain(f);
    return pump(f, seq);
}

int rdx_fgs_service(struct rdx_fgs *f, long *seq)
{
    struct epoll_event ev[2];
    int ne = epoll_wait(f->ep, ev, 2, 0);
    for (int i = 0; i < ne; i++)
        if (ev[i].data.u32 == 2) {
            uint64_t exp;
            if (read(f->tfd, &exp, sizeof(exp)) < 0) {}
        }
    struct rdx_fg_done d;
    while (rdx_fg_reap(f->fg, &d)) {
        struct fgs_pend *p = NULL;
        for (int i = 0; i < f->p_n && !p; i++) {
            struct fgs_pend *q = &f->pend[(f->p_head + i) % FGS_Q];
            if (q->job && !q->done) p = q;
        }
        if (f->n_samples < FGS_SAMPLES) {
            f->total_ms[f->n_samples] = d.total_ms;
            f->gpu_ms[f->n_samples] = d.gpu_ms;
            f->n_samples++;
        }
        if (d.failed) f->n_failed++;
        if (!p) continue;                         /* a job of before a detach */
        p->done = 1;
        p->generated = d.generated && !d.failed;
    }
    drain(f);
    return pump(f, seq);
}

int rdx_fgs_credit(struct rdx_fgs *f, uint32_t sink_slot, long *seq)
{
    if (sink_slot >= RDX_MAX_SLOTS) return -1;
    int owner = f->sink_owner[sink_slot], ret = -1;
    f->sink_owner[sink_slot] = OWN_FREE;
    if (owner >= OWN_GEN) f->gen_state[owner - OWN_GEN] = GB_FREE;
    else if (owner >= 0) ret = owner;
    if (pump(f, seq)) return -2;
    return ret;
}

void rdx_fgs_detach(struct rdx_fgs *f)
{
    if (!f) return;
    for (int i = 0; i < f->p_n; i++) f->pend[(f->p_head + i) % FGS_Q].dropped = 1;
    for (int i = 0; i < f->o_n; i++) {
        struct fgs_out *o = &f->outq[(f->o_head + i) % FGS_Q];
        if (o->gen) f->gen_state[o->idx] = GB_FREE;
    }
    f->o_n = 0;
    for (int k = 0; k < RDX_MAX_SLOTS; k++) {
        if (f->sink_owner[k] >= OWN_GEN) f->gen_state[f->sink_owner[k] - OWN_GEN] = GB_FREE;
        f->sink_owner[k] = OWN_FREE;              /* the new sink starts with every slot free */
    }
    arm(f, 0);
    f->have_sig = 0;
    f->t_last_new = 0;
    f->fg_on = 0;
    f->warm = 0;                          /* decide afresh for the new sink */
    rdx_fg_reset(f->fg);
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double q(double *v, int n, double p)
{
    if (!n) return 0;
    qsort(v, (size_t)n, sizeof(double), cmpd);
    return v[(int)(p * (n - 1))];
}

void rdx_fgs_report(const struct rdx_fgs *f)
{
    if (!f) return;
    int n = f->n_samples;
    double *t = malloc(sizeof(double) * (size_t)(n ? n : 1)), *g = malloc(sizeof(double) * (size_t)(n ? n : 1));
    memcpy(t, f->total_ms, sizeof(double) * (size_t)n);
    memcpy(g, f->gpu_ms, sizeof(double) * (size_t)n);
    printf("[fg] gamescope: %ld new pictures, %ld repeats not sent; posted %ld pictures (%ld held behind a generated "
           "frame) + %ld generated; %ld new pictures without generation (app as fast as the sink), %ld history only, "
           "%ld no free buffer, %ld failed, %ld mode switches\n",
           f->n_new, f->n_dup, f->n_real, f->n_held, f->n_gen, f->n_pass, f->n_history, f->n_no_buffer, f->n_failed,
           f->n_switch);
    printf("[fg] helper READY -> done p50/p90/p99 %.2f / %.2f / %.2f ms, helper GPU p50/p90 %.2f / %.2f ms (%d jobs), "
           "app frame time %.2f ms\n",
           q(t, n, 0.5), q(t, n, 0.9), q(t, n, 0.99), q(g, n, 0.5), q(g, n, 0.9), n, f->ival_ns / 1e6);
    free(t);
    free(g);
}

void rdx_fgs_close(struct rdx_fgs *f)
{
    if (!f) return;
    rdx_fg_close(f->fg);
    if (f->ep >= 0) close(f->ep);
    if (f->tfd >= 0) close(f->tfd);
    for (int i = 0; i < FGS_GEN_BUFS; i++)
        if (f->gen[i].host || f->gen[i].mr) rdx_gsbuf_free(&f->gen[i]);
#ifdef RDX_FG_CUDA
    if (f->staging.host) {
        cuMemHostUnregister(f->staging.host);
        rdx_gsbuf_free(&f->staging);
    }
#endif
    free(f->sig_rows);
    free(f->total_ms);
    free(f->gpu_ms);
    free(f);
}
