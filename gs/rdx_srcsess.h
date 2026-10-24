/* Source side of the rdx_sink_kms / rdx_sink_win protocol, for producers that
 * render into their own NIC-registered buffers (the gamescope path: gamescope into VMM).
 * Same wire protocol as p1/rdx_mirror_src.c: TCP bootstrap (hello, conn, sync),
 * frames as RDMA WRITE (pixels) + RDMA WRITE WITH IMM (header), credits and
 * "shown" reports as SEND WITH IMM from the sink, NIC completion timestamps.
 * Event driven: rdx_srcsess_fd() is a pollable completion channel.
 * A peer "local:PATH" is a window on this computer (p0b/rdx_sink_win --local, gs/rdx_local_proto.h):
 * no RDMA, the window imports our host slots (rdx_srcsess_local_buffers) and reports the same
 * events over the unix socket, which is then also rdx_srcsess_fd(). */
#pragma once
#include "../common/rdx_common.h"
#include "../common/rdx_cursor.h"
#include "rdx_gs_proto.h"

#include <infiniband/verbs.h>

enum rdx_ev_type {
    RDX_EV_CREDIT = 1,   /* sink slot free again */
    RDX_EV_SHOWN  = 2,   /* sink slot on screen since its vblank (t_ns = report arrival, NIC clock) */
    RDX_EV_DROP   = 3,   /* sink dropped the frame of that slot (never shown) */
    RDX_EV_SENT   = 4,   /* our post of that slot completed (source buffer readable again) */
    RDX_EV_MODE   = 5,   /* local window resized: w x h (gs/rdx_local_proto.h) */
    RDX_EV_INPUT  = 6,   /* local window input: in_kind, in_code, in_value */
};

struct rdx_ev {
    int type;
    uint32_t slot;
    long seq;            /* frame number posted into that slot */
    uint64_t t_ns;
    double round_us;     /* post -> report (SHOWN / timed CREDIT), 0 if unknown */
    double sink_us;      /* sink part of round_us */
    uint32_t w, h;                          /* RDX_EV_MODE */
    int in_kind, in_code, in_value;         /* RDX_EV_INPUT */
};

struct rdx_srcsess {
    int sock;
    struct rdx_hello hello;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct rdx_cq rcq;
    struct ibv_qp *qp;
    struct rdx_frame_hdr *hdrs;
    struct ibv_mr *hmr;
    uint64_t slot_t_post[RDX_MAX_SLOTS];
    long slot_seq[RDX_MAX_SLOTS];
    int inflight;
    long posted, sent, shown, dropped;
    double net_ewma_ns;          /* network part of the round trip, from shown reports */
    double net_ring[128];        /* recent transfer times post -> write completed (ns), for the lead */
    int net_n, net_i;
    uint64_t t_shown_last;       /* NIC time of the last shown report: the sink's vblank grid anchor */
    double period_ns;            /* sink refresh period (hello.vblank_ns) */
    /* recovery after a local error (see rdx_srcsess_recover): PSN bookkeeping of both
     * directions and the frames still in flight, oldest first */
    int gidx, mtu_bytes;
    uint8_t gid[16];
    enum ibv_mtu mtu;
    uint32_t sq_psn_next;        /* PSN our next message starts with */
    long recv_ok;                /* SENDs from the sink that completed: its next PSN = conn.psn + this */
    struct { uint32_t slot, psn; } fifo[RDX_MAX_SLOTS * 2];
    int f_head, f_n;
    int need_recover;            /* a WR failed locally (not a flush); the QP is in the error state */
    enum ibv_wc_status err_status;
    long flushed, recoveries;
    /* what was posted into each slot last (for the error dump and the bounds check) */
    struct rdx_post_rec {
        long seq;
        uint64_t addr, mr_addr, mr_len, t_ns;
        uint32_t len, lkey, psn;
    } rec[RDX_MAX_SLOTS];
    long refused;                /* posts refused by the bounds check (frame dropped, never posted) */
    struct rdx_tlog *tl;         /* optional: a P line per post */
    int local;                   /* the sink is a local window ("local:PATH"): s->sock is its socket */
    int local_bufs;              /* local: the window has our slots (rdx_srcsess_local_buffers) */
    uint32_t local_gen;          /* local: generation of the slots it has (reports of older ones are stale) */
    int hint_rel_mouse;          /* pointer hint from the app (RDX_GS_HINT): goes into every frame header */
    /* the cursor channel (common/rdx_cursor.h): a QP of its own on the same PD and CQ */
    int want_cursor;             /* offer to take it (set before connecting; 0 = the cursor stays in the picture) */
    struct ibv_qp *cqp;          /* NULL = none with this sink (an older sink, a local window, failed) */
    int cur_failed;              /* a cursor WR failed: the channel is off until the next sink */
    uint8_t *cbuf;               /* RDX_SRC_CUR_NBUF buffers for picture chunks in flight */
    struct ibv_mr *cmr;
    uint64_t cbuf_busy;
    uint32_t cur_posted, cur_done;   /* cursor WRs posted, and completed (a signaled one covers all before it) */
    long cur_pos_sent, cur_img_sent, cur_dropped;
};

/* gamescope's cursor as rdx_gs_src keeps it (RDX_GS_CURPOS / RDX_GS_CURIMG, rdx_gs_proto.h): the last
 * complete picture and position, so a resumed client gets them at once, and what the sink lacks */
struct rdx_gs_cursor {
    int told;                          /* RDX_GS_CURSOR as gamescope got it last: 1 = it draws none */
    uint32_t serial, w, h, hx, hy;     /* the complete picture, serial 0 = none yet */
    uint32_t px[RDX_GS_CUR_MAX * RDX_GS_CUR_MAX];
    uint32_t in_serial, in_got;        /* a picture coming in */
    uint32_t in_px[RDX_GS_CUR_MAX * RDX_GS_CUR_MAX];
    int have_pos, visible;
    int32_t x, y;                      /* the hotspot in output pixels */
    uint32_t pos_serial;
    uint64_t t_pos, t_img;             /* gamescope's time of the change */
    int img_dirty, pos_dirty;          /* not at the sink yet */
    long pos_in, img_in;
};
/* one packet of gamescope (n bytes): 0 = no cursor packet; 1 = taken, *changed = RDX_CUR_CH_POS (a position)
 * | RDX_CUR_CH_IMAGE (a picture completed), 0 for a chunk on the way or an invalid one */
int  rdx_gs_cursor_take(struct rdx_gs_cursor *c, const void *pkt, size_t n, int *changed);
/* what the sink has not got yet, the picture first, then the position (only while c->told and the channel
 * is up); a picture held back for chunk buffers in flight goes on a later call */
void rdx_gs_cursor_push(struct rdx_gs_cursor *c, struct rdx_srcsess *s);

/* the cursor channel to the current sink is up (negotiated, no failure) */
static inline int rdx_srcsess_cursor_up(const struct rdx_srcsess *s) { return s->cqp && !s->cur_failed; }
/* the hotspot at x, y (frame pixels), shown or not, the picture's serial; one inline SEND. -1 = not sent
 * (no channel, or the send queue full: the next position follows anyway) */
int  rdx_srcsess_cursor_pos(struct rdx_srcsess *s, uint32_t serial, int visible, int32_t x, int32_t y, uint64_t t_ns);
/* the picture (premultiplied ARGB, w x h <= RDX_CUR_MAX^2) in chunks. 0 = posted, -EAGAIN = buffers still in
 * flight (try again after the next completions), -1 = no channel */
int  rdx_srcsess_cursor_image(struct rdx_srcsess *s, uint32_t serial, uint32_t w, uint32_t h, uint32_t hot_x,
                              uint32_t hot_y, const uint32_t *px, uint64_t t_ns);

/* gamescope's pointer hint (RDX_GS_HINT message m): kept in the session, so it goes into the header of every
 * frame posted from now on (RDX_HDR_F_REL_MOUSE), also after a resume at another sink. A local window peer
 * ignores it. 1 = the state changed, 0 = same state or not a HINT. */
int  rdx_srcsess_gs_hint(struct rdx_srcsess *s, const struct rdx_gs_msg *m);
/* the header of a frame: magic, number, post time and the hints of the source (RDX_HDR_F_HINTS always, REL_MOUSE
 * from the pointer hint); every field is written, so memory that was not zeroed does not leak into the flags */
void rdx_srcsess_fill_hdr(const struct rdx_srcsess *s, struct rdx_frame_hdr *hdr, long seq, uint64_t t_post_ns);

/* a local sink ("local:PATH"): hand it a dma-buf of every slot, in slot order (it imports them,
 * the caller keeps its fds); again after every (re)connect and RECONFIG. 0 = ok, also when the
 * sink is not local (nothing to do) */
int  rdx_srcsess_local_buffers(struct rdx_srcsess *s, const int *fds, uint32_t n);
static inline int rdx_srcsess_is_local_peer(const char *peer) { return peer && !__builtin_strncmp(peer, "local:", 6); }

/* connect to the sink, read its hello, build the QP; pd for the caller's MRs is s->pd
 * (= rdx_srcsess_init + rdx_srcsess_connect) */
int  rdx_srcsess_open(struct rdx_srcsess *s, const char *dev, const char *peer_ip, int port, int mtu,
                      int comp_vector);
/* the parts that outlive a sink: device, PD (the caller's MRs), header MR, CQ */
/* local_ip: the seat's link IP (device "auto" and the source GID), NULL = unknown */
int  rdx_srcsess_init(struct rdx_srcsess *s, const char *dev, const char *local_ip, int mtu, int comp_vector);
/* one sink: TCP bootstrap (its hello in s->hello), a fresh QP on the same PD/CQ */
int  rdx_srcsess_connect(struct rdx_srcsess *s, const char *peer_ip, int port);
/* drop the sink (QP, socket, in-flight frames); PD, CQ and every MR stay valid */
void rdx_srcsess_disconnect(struct rdx_srcsess *s);
/* completion channel fd: readable when rdx_srcsess_poll has something; arm with rdx_srcsess_arm */
int  rdx_srcsess_fd(const struct rdx_srcsess *s);
/* request a completion event; returns 1 if completions are already pending (poll again first) */
int  rdx_srcsess_arm(struct rdx_srcsess *s);
/* drain channel events (after the fd was readable) */
void rdx_srcsess_ack(struct rdx_srcsess *s);
/* translate up to max completions into events; < 0 only if polling itself fails. A failed
 * work request sets need_recover (and err_status) and is skipped, flushed ones are counted
 * and skipped; successful completions after it are still delivered, so no credit is lost. */
int  rdx_srcsess_poll(struct rdx_srcsess *s, struct rdx_ev *ev, int max);
/* post pixels [mr_off, mr_off+len) of mr into sink slot `slot`, then the header with the frame immediate.
 * -2: refused, the range is not inside the MR (nothing posted; the caller drops the frame) */
int  rdx_srcsess_post(struct rdx_srcsess *s, uint32_t slot, long seq, struct ibv_mr *mr, uint64_t mr_off,
                      uint32_t len);
/* quantile q (0..1) of the recent network times (ns); a VRAM read through a slow PCIe path jitters,
 * and an EWMA (a mean) is pulled far up by single outliers */
double rdx_srcsess_net_quantile(const struct rdx_srcsess *s, double q);
/* the sink's next vblank grid point at or before `now`, shifted by -lead_ns; 0 if no anchor yet */
uint64_t rdx_srcsess_vblank_before(const struct rdx_srcsess *s, uint64_t now, double lead_ns);
/* After need_recover, once rdx_srcsess_poll returned 0 (CQ drained): bring the QP back without
 * a new handshake. RESET -> INIT -> RTR/RTS against the same sink QP, our send PSN restarting
 * at the first PSN of the oldest unfinished frame (to the sink a go-back-N retransmit; packets
 * it already had are duplicates), our receive PSN after the last SEND we completed (the sink
 * retransmits anything newer, within its retry window of ~0.5 s). The frames that were in
 * flight never reached the sink: their slots are returned in lost[] and are ours to reuse.
 * Returns the number of lost slots, < 0 if the QP could not be brought back. */
int  rdx_srcsess_recover(struct rdx_srcsess *s, uint32_t *lost, int max);
/* wait for outstanding posts, tell the sink the stream ended, close */
void rdx_srcsess_close(struct rdx_srcsess *s);
