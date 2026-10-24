/* SPDX-License-Identifier: MIT
 *
 * rdma-display: parts shared by source and sinks.
 * QP setup, GID lookup and TCP bootstrap follow the author's earlier GPUDirect RDMA
 * transfer experiments (own code, MIT).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <infiniband/verbs.h>

#define RDX_MAGIC     0x31584452u   /* "RDX1": hello, and frame header of the test pattern */
#define RDX_MAGIC_LIVE 0x314c5852u  /* "RXL1": frame header of live content (no pattern checks) */
#define RDX_IMM_END   0xFFFFFFFFu   /* source -> sink: no more frames */
#define RDX_MAX_SLOTS 8

/* Test pattern: frame counter block top left, moving bar, gradient */
#define RDX_SEQ_BLK 64
#define RDX_BAR_W   32

struct rdx_conn {
    uint32_t qpn, psn;
    uint8_t  gid[16];
};

/* sink -> source during bootstrap */
struct rdx_hello {
    uint32_t magic;
    uint32_t width, height, stride, fourcc;
    uint32_t nslots;
    uint64_t slot_bytes;     /* slot pitch, page aligned */
    uint64_t hdr_off;        /* offset of the frame header inside a slot */
    uint64_t base_addr;
    uint32_t rkey;
    struct rdx_conn conn;
    uint32_t caps;           /* RDX_CAP_*: what else the sink can do; was padding, so older sinks send 0 */
    uint64_t vblank_ns;      /* sink refresh period (KMS sink); 0 = unknown (window sink) */
};
#define RDX_CAP_CURSOR    0x1u         /* the sink shows the host's cursor itself (common/rdx_cursor.h) */
/* in the source's conn.psn (PSNs are 24 bits): the source takes the cursor channel, its cursor QP's
 * rdx_conn follows (common/rdx_cursor.h); never set towards a sink without RDX_CAP_CURSOR */
#define RDX_CONN_F_CURSOR 0x80000000u

/* lives in the slot behind the pixels and arrives with the same RDMA write */
struct rdx_frame_hdr {
    uint32_t magic;
    uint32_t seq;
    uint64_t t_post_ns;      /* source CLOCK_MONOTONIC right before the post */
    uint32_t flags;          /* RDX_HDR_F_*: hints of the source for the client, 0 = none */
    uint32_t pad;
};
#define RDX_HDR_F_HINTS     0x1u   /* the source sends hints (the bits below are meaningful) */
#define RDX_HDR_F_REL_MOUSE 0x2u   /* the app wants relative mouse motion: its cursor is hidden and the
                                    * pointer is locked (gamescope's own rule for its nested backends) */

/* Frame immediate (source -> sink): bits 0..3 slot, bits 4..31 frame number (mod 2^28) */
static inline uint32_t rdx_imm(uint32_t seq, uint32_t slot) { return (seq << 4) | (slot & 0xFu); }
static inline uint32_t rdx_imm_slot(uint32_t imm) { return imm & 0xFu; }
static inline uint32_t rdx_imm_seq(uint32_t imm)  { return imm >> 4; }
static inline uint32_t rdx_seq28(uint32_t seq)    { return seq & 0x0FFFFFFFu; }

/* Credit immediate (sink -> source): bits 0..3 slot, bits 4..30 sink time in us,
 * bit 31 set = "shown" report instead of a credit. The sink reports its own share
 * so that the source can measure the round trip with ONE clock; two hosts have no
 * common CLOCK_MONOTONIC. */
#define RDX_IMM_SHOWN    0x80000000u   /* KMS sink: frame visible since vblank, not a credit */
#define RDX_US_NONE      0x07FFFFFFu   /* credit without timing (KMS: slot was on screen) */
#define RDX_US_DROP      0x07FFFFFEu   /* credit without timing: frame was dropped, never shown */
static inline uint32_t rdx_credit_imm(uint32_t slot, uint64_t proc_ns)
{
    uint64_t us = proc_ns / 1000;
    if (us >= RDX_US_DROP) us = RDX_US_DROP - 1;
    return ((uint32_t)us << 4) | (slot & 0xFu);
}
static inline uint32_t rdx_credit_untimed(uint32_t slot, uint32_t code)
{
    return (code << 4) | (slot & 0xFu);
}
static inline uint32_t rdx_credit_us(uint32_t imm) { return (imm >> 4) & RDX_US_NONE; }
/* us = CQE -> vblank */
static inline uint32_t rdx_shown_imm(uint32_t slot, uint64_t cqe_to_vblank_ns)
{
    return RDX_IMM_SHOWN | rdx_credit_imm(slot, cqe_to_vblank_ns);
}

uint64_t rdx_now_ns(void);
int  rdx_parse_res(const char *s, uint32_t *w, uint32_t *h);
/* the MTU the sinks use unless told otherwise (the stream runs at 4 KB): a QP pair with different
 * MTUs stops after the first frame (WC "Work Request Flushed"), so every sink defaults to the same value */
#define RDX_DEFAULT_MTU 4096
enum ibv_mtu rdx_parse_mtu(int mtu);

/* ---- RDMA device and GID by IP ----
 * Several seat containers share one RoCE NIC (a macvlan per container): every container sees
 * every GID of the device, and the GID of a macvlan's IPv4 address sits in the parent device's
 * table. The right source GID is the one carrying the seat's own link IP; a device given as
 * "" or "auto" is the one whose GID table holds that IP.
 * local_ip: the seat's link IP (a listener's --bind; a connector's route source towards its
 * peer, rdx_local_ip_for). NULL, "", 0.0.0.0 or 127.x = unknown: the first IPv4 RoCE v2 GID,
 * else the first IPv4 GID (the behaviour before). */
struct rdx_gid_rec {
    int index;                   /* GID table index (sgid_index) */
    uint8_t gid[16];
    int roce_v2;
};
/* test seam: a faked device list and GID tables; NULL = libibverbs (ibv_query_gid_table, else
 * ibv_query_gid + sysfs gid_attrs/types) */
struct rdx_rdma_seam {
    int (*dev_names)(char names[][64], int max);
    int (*gid_table)(const char *devname, struct rdx_gid_rec *out, int max);
};
extern const struct rdx_rdma_seam *rdx_rdma_seam;

/* index into t[] of the GID to use: matching local_ip (RoCE v2 first) or, with no usable
 * local_ip, the first IPv4 RoCE v2 / IPv4 GID. -1 = no IPv4 GID, -2 = none carries local_ip */
int  rdx_pick_gid(const struct rdx_gid_rec *t, int n, const char *local_ip);
/* the GID index for local_ip on ctx's port 1 (out = the GID); < 0 = none (printed) */
int  rdx_find_gid_index_ip(struct ibv_context *ctx, const char *local_ip, uint8_t out[16]);
/* the IPv4 address the kernel would send from towards peer_ip (connected UDP socket, nothing
 * sent); 0 = ok */
int  rdx_local_ip_for(const char *peer_ip, char *out, size_t n);
/* want if it names a device; "" / "auto": the device whose GID table holds local_ip, else (no
 * local_ip) the only device with an IPv4 GID. NULL = none found (printed) */
const char *rdx_resolve_dev(const char *want, const char *local_ip, char *buf, size_t n);
/* rdx_resolve_dev + open */
struct ibv_context *rdx_open_dev_ip(const char *name, const char *local_ip);

struct ibv_context *rdx_open_dev(const char *name);    /* = rdx_open_dev_ip(name, NULL) */
/* = rdx_find_gid_index_ip(ctx, NULL, out): only where no link IP exists (test tools) */
int  rdx_find_gid_index(struct ibv_context *ctx, const char *devname, uint8_t out[16]);
int  rdx_gid_is_v2(const char *devname, int idx);
struct ibv_qp *rdx_create_qp(struct ibv_pd *pd, struct ibv_cq *cq, int depth);
/* same, with room for inline sends of up to max_inline bytes (small messages, no lkey lookup) */
struct ibv_qp *rdx_create_qp_inline(struct ibv_pd *pd, struct ibv_cq *cq, int depth, uint32_t max_inline);
int  rdx_qp_to_init(struct ibv_qp *qp);
int  rdx_qp_to_rtr_rts(struct ibv_qp *qp, const struct rdx_conn *peer, int sgid_idx,
                       uint32_t my_psn, enum ibv_mtu mtu);
int  rdx_post_zero_recv(struct ibv_qp *qp, uint64_t wr_id);
int  rdx_post_imm_send(struct ibv_qp *qp, uint32_t imm, uint64_t wr_id);

/* SIGINT/SIGTERM -> h, WITHOUT SA_RESTART: a blocking accept()/poll() returns EINTR, so a
 * component waiting for its peer still stops on the supervisor's SIGTERM (signal() restarts it) */
void rdx_stop_signals(void (*h)(int));
int  rdx_tcp_listen_accept(const char *bind_ip, int port);
/* the same, printing "[who] listening on IP:PORT" once the socket listens (who NULL: no line) */
int  rdx_tcp_listen_accept_as(const char *bind_ip, int port, const char *who);
int  rdx_tcp_connect(const char *ip, int port);
int  rdx_io_all(int fd, void *p, size_t n, int do_write);
int  rdx_sync(int fd);

/* test pattern */
uint32_t rdx_bar_x(uint32_t seq, uint32_t w);
uint32_t rdx_expected_pixel(uint32_t seq, uint32_t w, uint32_t h, uint32_t x, uint32_t y);
void rdx_fill_base(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride);
void rdx_paint_frame(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                     uint32_t seq, int64_t *prev_bar_x);

/* statistics */
void rdx_report(const char *name, double *v, size_t n);

/* ---- event-driven waiting (default; busy polling only on request) ----
 * Pattern for "wait until cond or deadline":
 *   poll CQ; if !cond: ibv_req_notify_cq(cq, 0); poll CQ again (closes the arm race);
 *   if still !cond: rdx_sleep_on(ch, extra_fd, timeout) */
/* ---- CQ with NIC completion timestamps ----
 * The software poll time is when the process got around to look; the NIC timestamp
 * is when the completion actually happened. Their difference is wake-up latency.
 * hw_ns is in this host's CLOCK_MONOTONIC (HCA clock mapped via ibv_query_rt_values_ex,
 * drift corrected by rdx_cq_clock_sync once per second); 0 if the NIC has no timestamps. */
struct rdx_wc {
    enum ibv_wc_status status;
    enum ibv_wc_opcode opcode;
    uint64_t wr_id;
    uint32_t imm_data;           /* network byte order, as in struct ibv_wc */
    uint64_t hw_ns;
};
struct rdx_cq {
    struct ibv_cq *cq;           /* use for QPs and ibv_req_notify_cq */
    struct ibv_cq_ex *cqx;       /* NULL: plain CQ, no timestamps */
    struct ibv_comp_channel *ch;
    struct ibv_context *ctx;
    double khz;                  /* HCA core clock */
    uint64_t raw0, mono0, raw1, mono1;
};
/* comp_vector: -1 = auto (the last vector, see rdx_pick_comp_vector) */
int rdx_cq_create_ts(struct ibv_context *ctx, int n, int comp_vector, struct rdx_cq *rc);
int rdx_cq_poll(struct rdx_cq *rc, struct rdx_wc *out, int max);
void rdx_cq_clock_sync(struct rdx_cq *rc);

/* CQ bound to a fresh non-blocking completion channel */
struct ibv_cq *rdx_create_cq_ev(struct ibv_context *ctx, int n, struct ibv_comp_channel **ch);
/* sleep until the CQ channel or extra_fd (-1 = none) is readable or timeout_ns passed
 * (UINT64_MAX = no timeout); consumes and acks CQ events. Returns 1 if extra_fd is readable. */
int rdx_sleep_on(struct ibv_comp_channel *ch, int extra_fd, uint64_t timeout_ns);
/* keep CPUs out of deep idle states while the process runs (/dev/cpu_dma_latency);
 * us <= 0 disables. Returns the fd to keep open, or -1. */
int rdx_pm_qos(int us);
/* Wait until `cond` holds or `deadline` (0 = none) passes. `poll_stmt` drains the CQ
 * and updates the state `cond` looks at. Sleeps on the CQ channel; spins only during
 * the last RDX_SPIN_NS before a deadline (precision), or all the time if `busy`.
 * `stop` (an lvalue, e.g. a signal flag) aborts the wait. */
#define RDX_SPIN_NS 50000ull
#define RDX_WAIT_UNTIL(cond, deadline, poll_stmt, cq, ch, busy, stop) do {      \
        uint64_t dl_ = (deadline);                                                \
        while (!(cond)) {                                                         \
            poll_stmt;                                                            \
            if (cond) break;                                                      \
            uint64_t now_ = rdx_now_ns();                                         \
            if ((dl_ && now_ >= dl_) || (stop)) break;                            \
            uint64_t left_ = dl_ ? dl_ - now_ : UINT64_MAX;                       \
            if ((busy) || left_ <= RDX_SPIN_NS) continue;                         \
            ibv_req_notify_cq((cq), 0);                                           \
            poll_stmt;                                     /* closes the arm race */ \
            if (cond) break;                                                      \
            rdx_sleep_on((ch), -1, left_ == UINT64_MAX ? 200000000ull : left_ - RDX_SPIN_NS); \
        }                                                                         \
    } while (0)

/* completion vector for a CQ: want >= 0 is used as given (mod the device's count);
 * -1 = auto = the last vector. Vector 0 shares its interrupt CPU with every other
 * default user of the device (measured on the test host: 1.4 M interrupts on CPU 0 and
 * recurring ~23 ms completion-interrupt stalls there). */
int rdx_pick_comp_vector(struct ibv_context *ctx, int want);
/* SCHED_FIFO with priority prio for the calling thread (0 = leave as is); 0 on success */
int rdx_set_rt_prio(int prio);

/* ---- per-frame telemetry (default on) ----
 * CSV lines into <dir>/<role>-<host>-<YYYYmmdd-HHMMSS>.csv, a new file every 256 MB,
 * flushed once per second. The first lines carry the header and a clock anchor
 * (CLOCK_MONOTONIC and CLOCK_REALTIME at the same instant) so logs of two hosts can
 * be aligned; frames are matched across hosts by their sequence number.
 * dir "-" disables. Analysis: tools/rdx_hitch.py. */
#define RDX_TLOG_DIR "/var/log/rdx"
struct rdx_tlog {
    FILE *f;
    char dir[200], role[32], header[512];
    uint64_t bytes, t_flush;
};
int  rdx_tlog_open(struct rdx_tlog *t, const char *dir, const char *role, const char *header);
void rdx_tlog_line(struct rdx_tlog *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rdx_tlog_tick(struct rdx_tlog *t, uint64_t now);     /* flush at most once per second */
void rdx_tlog_close(struct rdx_tlog *t);

/* process CPU time (user+sys) in ns, and "cpu: X ms over Y s = Z % of one core" since t0 */
uint64_t rdx_cpu_ns(void);
void rdx_cpu_report(const char *who, uint64_t t0_wall, uint64_t t0_cpu);

/* vblank lock: choose the remote vblank to aim at, return the local post time.
 *   anchor   local arrival time of the last "shown" report (= a remote vblank)
 *   period   remote refresh period (sink-reported, or estimated)
 *   interval wanted frame interval (>= period; e.g. 60 fps on a 120 Hz sink)
 *   ahead    frames posted but not yet shown/dropped; each occupies a vblank
 *   lead     post -> arrival (network + capture) + margin
 * The target is snapped onto the remote vblank grid; *L keeps the phase between calls
 * (zero-initialise it). */
struct rdx_lock {
    uint64_t last_target;    /* previous target vblank (local clock), 0 = none yet */
    double frac;             /* fractional vblank steps carried to the next frame */
};
uint64_t rdx_lock_due(uint64_t anchor, double period, double interval, int ahead,
                      struct rdx_lock *L, double lead, uint64_t now);
