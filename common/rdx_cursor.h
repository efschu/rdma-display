/* SPDX-License-Identifier: MIT
 *
 * The pointer next to the picture: the host's cursor (image, hotspot, position, visible) travels on a
 * channel of its own and the client shows it itself (rdx_sink_kms: the CRTC's cursor plane;
 * rdx_sink_win: the window's X cursor). A pointer that only moves costs the source no composite and
 * no frame: before, gamescope drew the cursor into every frame, and with variable refresh each mouse
 * motion over a still app sent a new frame (the stream ran at the cap, ~140 Hz, for a launcher).
 * The idea is the one of SPICE's cursor channel, Parsec's cursor events and Looking Glass's pointer
 * queue; gamescope's nested backends (SDL, Wayland) do the same on one machine (INestedHints).
 *
 * Transport: a second RC QP between source and sink, on the PD and CQ the frames use. Its send queue
 * is not behind the frames' (a 1440p frame is ~15 MB, a few ms on the wire): a position goes out as
 * one inline SEND of 48 bytes and the NIC interleaves it with a frame in flight. A failure of this QP
 * never touches the frames' QP (its recovery, rdx_srcsess_recover, keeps counting only frames).
 *
 * Negotiation, in the TCP bootstrap, compatible both ways:
 *   sink -> src  hello with caps |= RDX_CAP_CURSOR (the field was padding: older sinks send 0)
 *   src -> sink  its conn, psn |= RDX_CONN_F_CURSOR when it takes the channel (PSNs are 24 bits: older
 *                sources never set the bit), then the rdx_conn of its cursor QP
 *   sink -> src  the rdx_conn of the sink's cursor QP
 *   then the usual sync. Without the flag nothing more is exchanged and the source draws the cursor
 *   into the picture as before (an older sink, a mirrored desktop, gamescope without the patch).
 * The channel being up does not hide anything yet: the sink shows a cursor of its own only once the
 * first message came (a gamescope that cannot report its cursor keeps drawing it).
 *
 * Messages, source -> sink, SEND into receive buffers of RDX_CUR_RECV_BYTES:
 *   POS    a struct rdx_cur_msg: where the hotspot is (frame pixels, may lie outside), visible or not
 *   IMAGE  a struct rdx_cur_msg + up to RDX_CUR_CHUNK bytes of the picture (ARGB8888, premultiplied
 *          alpha like X cursors and the KMS cursor plane, rows of w pixels); complete when
 *          off + len == w * h * 4. A new serial starts a new picture; the last complete one stays.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <infiniband/verbs.h>
#include "rdx_common.h"

#define RDX_CUR_MAGIC     0x31435852u   /* "RXC1" */
#define RDX_CUR_MAX       256           /* largest picture; a bigger one is clipped (top left, hotspot kept) */
#define RDX_CUR_CHUNK     16384         /* picture bytes per IMAGE message (64x64 in one) */
#define RDX_CUR_F_VISIBLE 1u

enum rdx_cur_type { RDX_CUR_POS = 1, RDX_CUR_IMAGE = 2 };

struct rdx_cur_msg {
    uint32_t magic;
    uint16_t type, flags;        /* RDX_CUR_*, RDX_CUR_F_VISIBLE (POS) */
    uint32_t serial;             /* the picture's number (POS: the picture current at the source) */
    int32_t  x, y;               /* POS: the hotspot in frame pixels */
    uint16_t w, h, hot_x, hot_y; /* IMAGE */
    uint32_t off, len;           /* IMAGE: this chunk's byte range of the w*h*4 picture */
    uint32_t pad;
    uint64_t t_ns;               /* source CLOCK_MONOTONIC of the change (gamescope's), for telemetry */
};
#define RDX_CUR_RECV_BYTES ((uint32_t)(sizeof(struct rdx_cur_msg) + RDX_CUR_CHUNK))

/* what a message changed (rdx_cur_apply, rdx_cur_rx_complete) */
#define RDX_CUR_CH_POS   1          /* position or visibility */
#define RDX_CUR_CH_IMAGE 2          /* a picture completed */

/* ---------------- pure parts (no verbs): both ends, tests ---------------- */

/* the sink's view of the host's cursor */
struct rdx_cur_state {
    uint32_t serial;                 /* of the complete picture in px, 0 = none yet */
    uint16_t w, h, hot_x, hot_y;
    uint32_t px[RDX_CUR_MAX * RDX_CUR_MAX];
    uint32_t in_serial, in_got;      /* the picture coming in: its bytes so far */
    uint16_t in_w, in_h, in_hx, in_hy;
    uint32_t in_px[RDX_CUR_MAX * RDX_CUR_MAX];
    int have_pos, visible;
    int32_t x, y;
    uint64_t t_src;                  /* t_ns of the last message (source clock) */
    long n_pos, n_img, n_bad;
};

/* one received message of len bytes: RDX_CUR_CH_* bits of what changed, 0 = nothing, -1 = invalid */
int    rdx_cur_apply(struct rdx_cur_state *s, const void *msg, size_t len);
/* IMAGE messages a picture of w x h takes */
int    rdx_cur_nchunks(uint32_t w, uint32_t h);
/* message k of that picture into out (RDX_CUR_RECV_BYTES room); returns its length */
size_t rdx_cur_pack_image(void *out, uint32_t serial, uint32_t w, uint32_t h, uint32_t hot_x, uint32_t hot_y,
                          const uint32_t *px, int k, uint64_t t_ns);
size_t rdx_cur_pack_pos(void *out, uint32_t serial, int visible, int32_t x, int32_t y, uint64_t t_ns);
/* the picture (premultiplied ARGB, w x h) with its top left at x, y over an XRGB8888 frame fw x fh at dst
 * (pitch bytes per row), clipped to the frame */
void   rdx_cur_blend(uint8_t *dst, uint32_t pitch, uint32_t fw, uint32_t fh, const uint32_t *px, int w, int h,
                     int x, int y);
/* the picture scaled by s (0 < s <= 4) into out (room for RDX_CUR_MAX^2), premultiplied stays premultiplied:
 * area average when shrinking, nearest when growing; *ow, *oh = the new size (at least 1x1) */
void   rdx_cur_scale(const uint32_t *px, int w, int h, double s, uint32_t *out, int *ow, int *oh);

/* ---------------- the sink's end of the channel ---------------- */

#define RDX_CUR_WR_TAG  0x4352000000000000ull   /* "CR" in the top bits of a cursor QP's wr_id */
#define RDX_CUR_WR_MASK 0xFFFF000000000000ull
#define RDX_CUR_NRECV   64
static inline int rdx_cur_wr(uint64_t wr_id) { return (wr_id & RDX_CUR_WR_MASK) == RDX_CUR_WR_TAG; }

struct rdx_cur_rx {
    struct ibv_qp *qp;               /* NULL = no channel */
    struct ibv_mr *mr;
    uint8_t *buf;                    /* RDX_CUR_NRECV receive buffers */
    int up;                          /* negotiated: the source sends on it */
    int failed;                      /* an error completion: the channel is gone for this session */
    struct rdx_cur_state *st;
};

/* the sink's cursor QP on pd/cq, in INIT with its receives posted (before the hello offers it). Never
 * fatal: -1 = no channel (the hello then does not offer it) */
int  rdx_cur_rx_init(struct rdx_cur_rx *c, struct ibv_pd *pd, struct ibv_cq *cq);
/* the bootstrap step after the source's conn was read (peer: its RDX_CONN_F_CURSOR bit is cleared here):
 * with the bit, read the source's cursor conn, send ours, bring the QP up. 1 = channel up, 0 = the source
 * takes none, -1 = the socket failed (the session cannot go on) */
int  rdx_cur_rx_connect(struct rdx_cur_rx *c, int sock, struct rdx_conn *peer, int sgid_idx, const uint8_t gid[16],
                        enum ibv_mtu mtu);
/* a completion of the cursor QP (rdx_cur_wr(wr_id)): applied and the buffer posted again. RDX_CUR_CH_* bits,
 * 0 = nothing changed, -1 = the channel failed (status not ok; the sink then shows no cursor of its own) */
int  rdx_cur_rx_complete(struct rdx_cur_rx *c, uint64_t wr_id, int ok);
void rdx_cur_rx_fini(struct rdx_cur_rx *c);
