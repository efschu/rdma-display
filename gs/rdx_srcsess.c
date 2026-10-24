/* Source side of the sink protocol for self-rendering producers, see rdx_srcsess.h. */
#include "rdx_srcsess.h"
#include "rdx_local_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WR_SRC_BASE 0x100u     /* same ids as p1/rdx_mirror_src.c */
#define WR_RECV     0x200u
#define WR_END      0x300u
#define NRECV       32
/* the cursor channel (common/rdx_cursor.h): wr_id = RDX_CUR_WR_TAG | buffer + 1 << 32 | post number */
#define CUR_SQ      128        /* send queue of the cursor QP */
#define CUR_NBUF    40         /* picture chunks in flight: two whole 256x256 pictures (16 chunks each) and more */
#define CUR_SIGNAL  16         /* positions: one signaled WR in 16 frees the send queue */

static int poll_detail(struct rdx_cq *rc, struct rdx_wc *out, uint32_t *vendor, int max);

int rdx_srcsess_init(struct rdx_srcsess *s, const char *dev, const char *local_ip, int mtu, int comp_vector)
{
    memset(s, 0, sizeof(*s));
    s->sock = -1;
    s->ctx = rdx_open_dev_ip(dev, local_ip);
    if (!s->ctx) return -1;
    s->pd = ibv_alloc_pd(s->ctx);
    if (!s->pd) { perror("ibv_alloc_pd"); return -1; }
    s->hdrs = calloc(RDX_MAX_SLOTS, sizeof(*s->hdrs));
    s->hmr = ibv_reg_mr(s->pd, s->hdrs, RDX_MAX_SLOTS * sizeof(*s->hdrs), IBV_ACCESS_LOCAL_WRITE);
    if (!s->hmr || rdx_cq_create_ts(s->ctx, 256, comp_vector, &s->rcq)) return -1;
    s->gidx = rdx_find_gid_index_ip(s->ctx, local_ip, s->gid);
    if (s->gidx < 0) return -1;
    s->mtu = rdx_parse_mtu(mtu);
    s->mtu_bytes = 256 << (s->mtu - IBV_MTU_256);
    /* the cursor channel is offered to every sink that can take it; picture chunks go from these buffers */
    s->want_cursor = 1;
    s->cbuf = calloc(CUR_NBUF, RDX_CUR_RECV_BYTES);
    s->cmr = s->cbuf ? ibv_reg_mr(s->pd, s->cbuf, (size_t)CUR_NBUF * RDX_CUR_RECV_BYTES, IBV_ACCESS_LOCAL_WRITE) : NULL;
    if (!s->cmr) {
        fprintf(stderr, "[sess] no buffers for the cursor channel: the cursor stays in the picture\n");
        s->want_cursor = 0;
    }
    return 0;
}

/* the source's half of the cursor channel's bootstrap (common/rdx_cursor.h); 1 = this sink takes it */
static int cursor_offer(struct rdx_srcsess *s)
{
    s->cqp = NULL;
    s->cur_failed = 0;
    s->cbuf_busy = 0;
    s->cur_posted = s->cur_done = 0;
    if (!s->want_cursor || !s->cmr || !(s->hello.caps & RDX_CAP_CURSOR)) return 0;
    s->cqp = rdx_create_qp_inline(s->pd, s->rcq.cq, CUR_SQ, sizeof(struct rdx_cur_msg));
    if (s->cqp && !rdx_qp_to_init(s->cqp)) return 1;
    if (s->cqp) ibv_destroy_qp(s->cqp);
    s->cqp = NULL;
    fprintf(stderr, "[sess] no cursor QP: the cursor stays in the picture\n");
    return 0;
}

static int cursor_connect(struct rdx_srcsess *s)
{
    struct rdx_conn mine, peer;
    memset(&mine, 0, sizeof(mine));
    memcpy(mine.gid, s->gid, 16);
    mine.qpn = s->cqp->qp_num;
    mine.psn = 0x1357;
    if (rdx_io_all(s->sock, &mine, sizeof(mine), 1) || rdx_io_all(s->sock, &peer, sizeof(peer), 0)) return -1;
    if (!peer.qpn || rdx_qp_to_rtr_rts(s->cqp, &peer, s->gidx, mine.psn, s->mtu)) {
        fprintf(stderr, "[sess] the sink's cursor QP did not come up: the cursor stays in the picture\n");
        ibv_destroy_qp(s->cqp);
        s->cqp = NULL;
    }
    return 0;
}

/* a window on this computer: unix socket, its hello, no QP */
static int connect_local(struct rdx_srcsess *s, const char *path)
{
    s->sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (s->sock < 0 || connect(s->sock, (struct sockaddr *)&sa, sizeof(sa))) {
        fprintf(stderr, "[sess] local window %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (recv(s->sock, &s->hello, sizeof(s->hello), 0) != (ssize_t)sizeof(s->hello) || s->hello.magic != RDX_MAGIC ||
        !s->hello.nslots || s->hello.nslots > RDX_MAX_SLOTS) {
        fprintf(stderr, "[sess] local window: invalid hello\n");
        return -1;
    }
    s->period_ns = (double)s->hello.vblank_ns;
    s->local = 1;
    s->local_bufs = 0;
    printf("[sess] local window %s: %ux%u stride %u, %u slots, vblank %.3f ms (no RDMA, the window imports our slots)\n",
           path, s->hello.width, s->hello.height, s->hello.stride, s->hello.nslots, s->period_ns / 1e6);
    return 0;
}

int rdx_srcsess_local_buffers(struct rdx_srcsess *s, const int *fds, uint32_t n)
{
    if (!s->local) return 0;
    struct rdx_local_msg m = {.magic = RDX_LOCAL_MAGIC, .type = RDX_LOCAL_BUFS, .n = n, .b = s->local_gen + 1,
                              .a = s->hello.width, .slot = s->hello.height};   /* the slots' frame size */
    struct iovec iov = {.iov_base = &m, .iov_len = sizeof(m)};
    char cbuf[CMSG_SPACE(sizeof(int) * RDX_MAX_SLOTS)];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = CMSG_SPACE(sizeof(int) * n)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    if (n > RDX_MAX_SLOTS || sendmsg(s->sock, &mh, MSG_NOSIGNAL) != (ssize_t)sizeof(m)) return -1;
    s->local_gen++;
    s->local_bufs = 1;
    return 0;
}

int rdx_srcsess_connect(struct rdx_srcsess *s, const char *peer_ip, int port)
{
    /* per-sink state starts fresh; counters of the whole run stay */
    s->inflight = s->f_head = s->f_n = s->need_recover = s->net_n = s->net_i = 0;
    s->recv_ok = 0;
    s->t_shown_last = 0;
    memset(s->slot_t_post, 0, sizeof(s->slot_t_post));
    memset(s->rec, 0, sizeof(s->rec));
    s->local = 0;
    if (rdx_srcsess_is_local_peer(peer_ip)) return connect_local(s, peer_ip + 6);
    /* the source GID is the one of the link IP towards this sink (a resume may go elsewhere) */
    char lip[64] = "";
    if (!rdx_local_ip_for(peer_ip, lip, sizeof(lip)) && (s->gidx = rdx_find_gid_index_ip(s->ctx, lip, s->gid)) < 0)
        return -1;
    s->sock = rdx_tcp_connect(peer_ip, port);
    if (s->sock < 0) return -1;
    if (rdx_io_all(s->sock, &s->hello, sizeof(s->hello), 0) || s->hello.magic != RDX_MAGIC) {
        fprintf(stderr, "bootstrap: invalid hello\n"); return -1;
    }
    if (!s->hello.nslots || s->hello.nslots > RDX_MAX_SLOTS) { fprintf(stderr, "invalid nslots=%u\n", s->hello.nslots); return -1; }
    s->period_ns = (double)s->hello.vblank_ns;
    s->qp = rdx_create_qp(s->pd, s->rcq.cq, 64);
    if (!s->qp || rdx_qp_to_init(s->qp)) return -1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(s->qp, WR_RECV)) { perror("post_recv"); return -1; }
    int cursor = cursor_offer(s);
    struct rdx_conn mine;
    memset(&mine, 0, sizeof(mine));
    memcpy(mine.gid, s->gid, 16);
    mine.qpn = s->qp->qp_num;
    mine.psn = 0x3456;
    struct rdx_conn wire = mine;              /* the flag only on the wire: our PSNs stay 24 bits */
    if (cursor) wire.psn |= RDX_CONN_F_CURSOR;
    if (rdx_io_all(s->sock, &wire, sizeof(wire), 1)) return -1;
    if (cursor && cursor_connect(s)) return -1;
    s->sq_psn_next = mine.psn;
    if (rdx_qp_to_rtr_rts(s->qp, &s->hello.conn, s->gidx, mine.psn, s->mtu)) return -1;
    if (rdx_sync(s->sock)) { fprintf(stderr, "sync failed\n"); return -1; }
    printf("[sess] sink %ux%u stride %u, %u slots, vblank %.3f ms, NIC timestamps %s, cursor %s\n", s->hello.width,
           s->hello.height, s->hello.stride, s->hello.nslots, s->period_ns / 1e6, s->rcq.cqx ? "on" : "off",
           s->cqp ? "next to the picture (the sink shows it)" : (s->hello.caps & RDX_CAP_CURSOR) ? "in the picture"
           : "in the picture (the sink cannot show one)");
    return 0;
}

int rdx_srcsess_cursor_pos(struct rdx_srcsess *s, uint32_t serial, int visible, int32_t x, int32_t y, uint64_t t_ns)
{
    if (!rdx_srcsess_cursor_up(s)) return -1;
    if (s->cur_posted - s->cur_done >= CUR_SQ - 2) { s->cur_dropped++; return -1; }   /* send queue full */
    struct rdx_cur_msg m;
    rdx_cur_pack_pos(&m, serial, visible, x, y, t_ns);
    struct ibv_sge sge = {.addr = (uintptr_t)&m, .length = sizeof(m), .lkey = 0};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    uint32_t no = ++s->cur_posted;
    wr.wr_id = RDX_CUR_WR_TAG | no;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_INLINE | (no % CUR_SIGNAL == 0 ? IBV_SEND_SIGNALED : 0);
    if (ibv_post_send(s->cqp, &wr, &bad)) {
        s->cur_posted--;
        s->cur_dropped++;
        return -1;
    }
    s->cur_pos_sent++;
    return 0;
}

int rdx_srcsess_cursor_image(struct rdx_srcsess *s, uint32_t serial, uint32_t w, uint32_t h, uint32_t hot_x,
                             uint32_t hot_y, const uint32_t *px, uint64_t t_ns)
{
    if (!rdx_srcsess_cursor_up(s)) return -1;
    if (!w || !h || w > RDX_CUR_MAX || h > RDX_CUR_MAX) return -1;
    int n = rdx_cur_nchunks(w, h), nfree = 0;
    for (int b = 0; b < CUR_NBUF; b++) nfree += !(s->cbuf_busy & (1ull << b));
    if (nfree < n || s->cur_posted - s->cur_done + (uint32_t)n >= CUR_SQ - 2) return -EAGAIN;
    for (int k = 0, b = 0; k < n; k++, b++) {
        while (s->cbuf_busy & (1ull << b)) b++;
        uint8_t *p = s->cbuf + (size_t)b * RDX_CUR_RECV_BYTES;
        size_t len = rdx_cur_pack_image(p, serial, w, h, hot_x, hot_y, px, k, t_ns);
        struct ibv_sge sge = {.addr = (uintptr_t)p, .length = (uint32_t)len, .lkey = s->cmr->lkey};
        struct ibv_send_wr wr, *bad;
        memset(&wr, 0, sizeof(wr));
        uint32_t no = ++s->cur_posted;
        wr.wr_id = RDX_CUR_WR_TAG | (uint64_t)(b + 1) << 32 | no;
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;           /* the buffer is ours again at its completion */
        if (ibv_post_send(s->cqp, &wr, &bad)) {
            s->cur_posted--;
            perror("[sess] cursor post_send");
            s->cur_failed = 1;
            return -1;
        }
        s->cbuf_busy |= 1ull << b;
    }
    s->cur_img_sent++;
    return 0;
}

/* a completion of the cursor QP: never a frame's business (no recovery of the frames' QP for it) */
static void cursor_completion(struct rdx_srcsess *s, const struct rdx_wc *w)
{
    if (w->status != IBV_WC_SUCCESS) {
        if (!s->cur_failed)
            fprintf(stderr, "[sess] cursor channel: %s, the cursor goes back into the picture\n", ibv_wc_status_str(w->status));
        s->cur_failed = 1;
        s->cbuf_busy = 0;
        return;
    }
    uint32_t b = (uint32_t)(w->wr_id >> 32) & 0xFFFFu, no = (uint32_t)w->wr_id;
    if (b && b <= CUR_NBUF) s->cbuf_busy &= ~(1ull << (b - 1));
    if ((int32_t)(no - s->cur_done) > 0) s->cur_done = no;    /* RC completes in order: all before it are done */
}

int rdx_srcsess_open(struct rdx_srcsess *s, const char *dev, const char *peer_ip, int port, int mtu,
                     int comp_vector)
{
    char lip[64] = "";
    if (!rdx_srcsess_is_local_peer(peer_ip)) rdx_local_ip_for(peer_ip, lip, sizeof(lip));
    if (rdx_srcsess_init(s, dev, lip[0] ? lip : NULL, mtu, comp_vector)) return -1;
    return rdx_srcsess_connect(s, peer_ip, port);
}

void rdx_srcsess_disconnect(struct rdx_srcsess *s)
{
    if (s->qp) ibv_destroy_qp(s->qp);
    s->qp = NULL;
    if (s->cqp) ibv_destroy_qp(s->cqp);
    s->cqp = NULL;
    s->cbuf_busy = 0;
    /* completions of the destroyed QP: drop them, nothing of them is ours any more */
    struct rdx_wc wc[16];
    uint32_t vendor[16];
    while (poll_detail(&s->rcq, wc, vendor, 16) > 0) ;
    if (s->sock >= 0) close(s->sock);
    s->sock = -1;
    s->inflight = s->f_head = s->f_n = s->need_recover = 0;
    s->t_shown_last = 0;
}

int rdx_srcsess_fd(const struct rdx_srcsess *s) { return s->local ? s->sock : s->rcq.ch->fd; }

int rdx_srcsess_arm(struct rdx_srcsess *s)
{
    if (s->local) return 0;                    /* the socket itself is pollable */
    return ibv_req_notify_cq(s->rcq.cq, 0) ? -1 : 0;
}

void rdx_srcsess_ack(struct rdx_srcsess *s)
{
    if (s->local) return;
    struct ibv_cq *cq;
    void *ctx;
    while (ibv_get_cq_event(s->rcq.ch, &cq, &ctx) == 0) ibv_ack_cq_events(cq, 1);
}

/* rdx_cq_poll plus the vendor error of failed completions (mlx5: the CQE syndrome byte) */
static int poll_detail(struct rdx_cq *rc, struct rdx_wc *out, uint32_t *vendor, int max)
{
    if (!rc->cqx) {
        struct ibv_wc wc[16];
        int n = ibv_poll_cq(rc->cq, max < 16 ? max : 16, wc);
        for (int i = 0; i < n; i++) {
            out[i] = (struct rdx_wc){wc[i].status, wc[i].opcode, wc[i].wr_id, wc[i].imm_data, 0};
            vendor[i] = wc[i].vendor_err;
        }
        return n;
    }
    struct ibv_poll_cq_attr pa = {0};
    int r = ibv_start_poll(rc->cqx, &pa);
    if (r == ENOENT) return 0;
    if (r) return -1;
    int n = 0;
    double ns_per_tick = rc->raw1 > rc->raw0 ? (double)(rc->mono1 - rc->mono0) / (double)(rc->raw1 - rc->raw0)
                                             : 1e6 / rc->khz;
    do {
        struct ibv_cq_ex *c = rc->cqx;
        out[n].status = c->status;
        out[n].wr_id = c->wr_id;
        out[n].opcode = ibv_wc_read_opcode(c);
        out[n].imm_data = (out[n].opcode == IBV_WC_RECV || out[n].opcode == IBV_WC_RECV_RDMA_WITH_IMM)
                              ? ibv_wc_read_imm_data(c) : 0;
        out[n].hw_ns = (uint64_t)((double)rc->mono1 + ((double)ibv_wc_read_completion_ts(c) - (double)rc->raw1) * ns_per_tick);
        vendor[n] = c->status != IBV_WC_SUCCESS ? ibv_wc_read_vendor_err(c) : 0;
        n++;
    } while (n < max && ibv_next_poll(rc->cqx) == 0);
    ibv_end_poll(rc->cqx);
    return n;
}

static void dump_error(const struct rdx_srcsess *s, const struct rdx_wc *w, uint32_t vendor)
{
    fprintf(stderr, "[sess] FIRST ERROR: %s (status %d, vendor_err 0x%x, opcode %d, wr_id 0x%llx), "
            "posted %ld, sent %ld, in flight %d, recv ok %ld, next PSN 0x%06x\n", ibv_wc_status_str(w->status),
            (int)w->status, vendor, (int)w->opcode, (unsigned long long)w->wr_id, s->posted, s->sent, s->f_n,
            s->recv_ok, s->sq_psn_next);
    for (int i = 0; i < s->f_n; i++) {
        uint32_t sl = s->fifo[(s->f_head + i) % (RDX_MAX_SLOTS * 2)].slot;
        const struct rdx_post_rec *r = &s->rec[sl];
        fprintf(stderr, "[sess]   in flight #%d: slot %u seq %ld psn 0x%06x sge addr 0x%llx len %u lkey 0x%x | "
                "MR [0x%llx, +%llu) %s, posted %.3f ms ago\n", i, sl, r->seq, r->psn, (unsigned long long)r->addr,
                r->len, r->lkey, (unsigned long long)r->mr_addr, (unsigned long long)r->mr_len,
                r->addr >= r->mr_addr && r->addr + r->len <= r->mr_addr + r->mr_len ? "inside" : "OUTSIDE",
                (rdx_now_ns() - r->t_ns) / 1e6);
    }
}

/* local window: its reports over the socket, as the same events */
static int poll_local(struct rdx_srcsess *s, struct rdx_ev *ev, int max)
{
    int n = 0;
    struct rdx_local_msg m;
    while (n < max) {
        ssize_t r = recv(s->sock, &m, sizeof(m), MSG_DONTWAIT);
        if (r != (ssize_t)sizeof(m)) break;      /* nothing more (EOF: the caller's peek sees it) */
        if (m.magic != RDX_LOCAL_MAGIC) continue;
        struct rdx_ev *e = &ev[n];
        if (m.type == RDX_LOCAL_MODE || m.type == RDX_LOCAL_INPUT) {     /* not about a slot */
            memset(e, 0, sizeof(*e));
            if (m.type == RDX_LOCAL_MODE) {
                e->type = RDX_EV_MODE;
                e->w = (uint32_t)m.a;
                e->h = (uint32_t)m.b;
            } else {
                e->type = RDX_EV_INPUT;
                e->in_kind = (int)m.slot;
                e->in_code = (int)m.n;
                e->in_value = (int)m.seq;
            }
            n++;
            continue;
        }
        /* a report about slots of an older generation (sent before the window had the new ones) */
        if (m.slot >= s->hello.nslots || m.b != s->local_gen) continue;
        memset(e, 0, sizeof(*e));
        e->slot = m.slot;
        e->seq = s->slot_seq[m.slot];
        e->t_ns = m.t_ns;
        uint64_t tp = s->slot_t_post[m.slot];
        if (m.type == RDX_LOCAL_CREDIT) {
            e->type = RDX_EV_CREDIT;
            if (s->inflight > 0) s->inflight--;
        } else if (m.type == RDX_LOCAL_SHOWN) {
            e->type = RDX_EV_SHOWN;
            e->round_us = tp && m.t_ns > tp ? (double)(m.t_ns - tp) / 1e3 : 0;
            e->sink_us = (double)m.a / 1e3;
            s->t_shown_last = m.t_ns;
            s->shown++;
        } else if (m.type == RDX_LOCAL_DROP) {
            e->type = RDX_EV_DROP;
            s->dropped++;
        } else continue;
        n++;
    }
    return n;
}

int rdx_srcsess_poll(struct rdx_srcsess *s, struct rdx_ev *ev, int max)
{
    struct rdx_wc wc[16];
    uint32_t vendor[16];
    int n = 0;
    if (max > 16) max = 16;
    if (s->local) return poll_local(s, ev, max);
    int nwc = poll_detail(&s->rcq, wc, vendor, max);
    if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return -1; }
    for (int k = 0; k < nwc; k++) {
        if (rdx_cur_wr(wc[k].wr_id)) { cursor_completion(s, &wc[k]); continue; }
        if (wc[k].status != IBV_WC_SUCCESS) {
            if (wc[k].status == IBV_WC_WR_FLUSH_ERR) { s->flushed++; continue; }
            fprintf(stderr, "WC error: %s (wr_id=0x%llx, vendor_err 0x%x)\n", ibv_wc_status_str(wc[k].status),
                    (unsigned long long)wc[k].wr_id, vendor[k]);
            if (!s->need_recover && !s->recoveries) dump_error(s, &wc[k], vendor[k]);
            if (!s->need_recover) s->err_status = wc[k].status;
            s->need_recover = 1;
            continue;
        }
        struct rdx_ev *e = &ev[n];
        memset(e, 0, sizeof(*e));
        if (wc[k].opcode == IBV_WC_RECV) {
            uint64_t t = wc[k].hw_ns ? wc[k].hw_ns : rdx_now_ns();
            uint32_t imm = ntohl(wc[k].imm_data), slot = rdx_imm_slot(imm), us = rdx_credit_us(imm);
            int shown = (imm & RDX_IMM_SHOWN) != 0;
            s->recv_ok++;
            /* in the error state the repost fails; recovery posts a fresh set */
            if (!s->need_recover && rdx_post_zero_recv(s->qp, WR_RECV)) { perror("repost"); return -1; }
            if (slot >= s->hello.nslots) continue;
            e->slot = slot;
            e->seq = s->slot_seq[slot];
            e->t_ns = t;
            if (us < RDX_US_DROP && s->slot_t_post[slot]) {
                e->round_us = (double)(t - s->slot_t_post[slot]) / 1e3;
                e->sink_us = us;
            }
            if (shown) {
                e->type = RDX_EV_SHOWN;
                s->shown++;
                s->t_shown_last = t;
                if (e->round_us > 0) {
                    double net = (e->round_us - e->sink_us) * 1e3;
                    s->net_ewma_ns = s->net_ewma_ns > 0 ? 0.9 * s->net_ewma_ns + 0.1 * net : net;
                }
            } else {
                e->type = us == RDX_US_DROP ? RDX_EV_DROP : RDX_EV_CREDIT;
                if (us == RDX_US_DROP) {
                    s->dropped++;
                    /* a drop also frees the slot: report it, then the credit */
                    ev[n++] = *e;
                    if (n >= max) return n;
                    e = &ev[n];
                    *e = ev[n - 1];
                    e->type = RDX_EV_CREDIT;
                }
            }
            n++;
        } else if (wc[k].wr_id >= WR_SRC_BASE && wc[k].wr_id < WR_SRC_BASE + RDX_MAX_SLOTS) {
            e->type = RDX_EV_SENT;
            e->slot = (uint32_t)(wc[k].wr_id - WR_SRC_BASE);
            e->seq = s->slot_seq[e->slot];
            e->t_ns = wc[k].hw_ns ? wc[k].hw_ns : rdx_now_ns();
            /* transfer time: post -> completion of the signaled header write (the remote NIC has
             * the whole frame, RC acks in order). Measured here, with one clock, per frame. */
            if (s->slot_t_post[e->slot] && e->t_ns > s->slot_t_post[e->slot]) {
                s->net_ring[s->net_i] = (double)(e->t_ns - s->slot_t_post[e->slot]);
                s->net_i = (s->net_i + 1) % 128;
                if (s->net_n < 128) s->net_n++;
            }
            s->inflight--;
            s->sent++;
            if (s->f_n) { s->f_head = (s->f_head + 1) % (RDX_MAX_SLOTS * 2); s->f_n--; }
            n++;
        }
        if (n >= max) break;
    }
    return n;
}

int rdx_gs_cursor_take(struct rdx_gs_cursor *c, const void *pkt, size_t n, int *changed)
{
    *changed = 0;
    struct rdx_gs_msg m;
    if (n < sizeof(m)) return 0;
    memcpy(&m, pkt, sizeof(m));
    if (m.magic != RDX_GS_MAGIC) return 0;
    if (m.type == RDX_GS_CURPOS && n == sizeof(m)) {
        c->have_pos = 1;
        c->visible = (m.slot & RDX_GS_CUR_VISIBLE) != 0;
        c->x = (int32_t)(uint32_t)m.seq;
        c->y = (int32_t)(uint32_t)(m.seq >> 32);
        c->pos_serial = m.pad;
        c->t_pos = m.t_ns;
        c->pos_dirty = 1;
        c->pos_in++;
        *changed = RDX_CUR_CH_POS;
        return 1;
    }
    if (m.type != RDX_GS_CURIMG || n < sizeof(struct rdx_gs_curimg)) return 0;
    struct rdx_gs_curimg ci;
    memcpy(&ci, pkt, sizeof(ci));
    uint32_t total = (uint32_t)ci.w * ci.h * 4;
    if (!ci.w || !ci.h || ci.w > RDX_GS_CUR_MAX || ci.h > RDX_GS_CUR_MAX || ci.len > RDX_GS_CUR_CHUNK ||
        sizeof(ci) + ci.len > n || ci.off > total || ci.len > total - ci.off)
        return 1;
    if (ci.off == 0) { c->in_serial = ci.serial; c->in_got = 0; }
    if (ci.serial != c->in_serial || ci.off != c->in_got) return 1;   /* a chunk of a picture given up */
    memcpy((uint8_t *)c->in_px + ci.off, (const uint8_t *)pkt + sizeof(ci), ci.len);
    c->in_got += ci.len;
    if (c->in_got < total) return 1;
    c->serial = ci.serial;
    c->w = ci.w; c->h = ci.h; c->hx = ci.hot_x; c->hy = ci.hot_y;
    c->t_img = ci.t_ns;
    memcpy(c->px, c->in_px, total);
    c->in_got = 0;
    c->img_dirty = 1;
    c->img_in++;
    *changed = RDX_CUR_CH_IMAGE;
    return 1;
}

void rdx_gs_cursor_push(struct rdx_gs_cursor *c, struct rdx_srcsess *s)
{
    if (!rdx_srcsess_cursor_up(s) || !c->told) return;
    if (c->img_dirty && c->serial &&
        rdx_srcsess_cursor_image(s, c->serial, c->w, c->h, c->hx, c->hy, c->px, c->t_img) != -EAGAIN)
        c->img_dirty = 0;
    if (c->pos_dirty && c->have_pos && !c->img_dirty && !rdx_srcsess_cursor_pos(s, c->pos_serial, c->visible, c->x, c->y, c->t_pos))
        c->pos_dirty = 0;
}

int rdx_srcsess_gs_hint(struct rdx_srcsess *s, const struct rdx_gs_msg *m)
{
    if (m->magic != RDX_GS_MAGIC || m->type != RDX_GS_HINT) return 0;
    int rel = (m->slot & RDX_GS_HINT_REL_MOUSE) != 0;
    if (rel == s->hint_rel_mouse) return 0;
    s->hint_rel_mouse = rel;
    return 1;
}

void rdx_srcsess_fill_hdr(const struct rdx_srcsess *s, struct rdx_frame_hdr *hdr, long seq, uint64_t t_post_ns)
{
    hdr->magic = RDX_MAGIC_LIVE;
    hdr->seq = (uint32_t)seq;
    hdr->t_post_ns = t_post_ns;
    hdr->flags = RDX_HDR_F_HINTS | (s->hint_rel_mouse ? RDX_HDR_F_REL_MOUSE : 0);
    hdr->pad = 0;
}

int rdx_srcsess_post(struct rdx_srcsess *s, uint32_t slot, long seq, struct ibv_mr *mr, uint64_t mr_off,
                     uint32_t len)
{
    if (slot >= s->hello.nslots) return -1;
    if (s->local) {                            /* the window reads our slot itself: just say which */
        if (!s->local_bufs) { s->refused++; return -2; }
        uint64_t t = rdx_now_ns();
        struct rdx_local_msg m = {.magic = RDX_LOCAL_MAGIC, .type = RDX_LOCAL_FRAME, .slot = slot, .seq = seq, .t_ns = t,
                                  .b = s->local_gen};
        s->slot_t_post[slot] = t;
        s->slot_seq[slot] = seq;
        if (send(s->sock, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)sizeof(m)) return -2;
        s->posted++;
        s->inflight++;
        (void)mr; (void)mr_off; (void)len;
        return 0;
    }
    uint64_t mr_addr = (uint64_t)(uintptr_t)mr->addr, mr_len = mr->length;
    if (mr_off < mr_addr || mr_off + len > mr_addr + mr_len ||
        (uint64_t)len > s->hello.hdr_off) {
        fprintf(stderr, "[sess] REFUSED post: slot %u seq %ld sge [0x%llx, +%u) not inside MR [0x%llx, +%llu) "
                "or longer than the sink slot (%llu)\n", slot, seq, (unsigned long long)mr_off, len,
                (unsigned long long)mr_addr, (unsigned long long)mr_len, (unsigned long long)s->hello.hdr_off);
        s->refused++;
        return -2;
    }
    struct rdx_frame_hdr *hdr = &s->hdrs[slot];
    rdx_srcsess_fill_hdr(s, hdr, seq, rdx_now_ns());
    s->slot_t_post[slot] = hdr->t_post_ns;
    s->slot_seq[slot] = seq;
    uint64_t raddr = s->hello.base_addr + (uint64_t)slot * s->hello.slot_bytes;
    struct ibv_sge sp = {.addr = mr_off, .length = len, .lkey = mr->lkey};      /* dma-buf MR: iova 0 */
    struct ibv_sge sh = {.addr = (uintptr_t)hdr, .length = sizeof(*hdr), .lkey = s->hmr->lkey};
    struct ibv_send_wr wp, wh, *bad;
    memset(&wp, 0, sizeof(wp));
    memset(&wh, 0, sizeof(wh));
    wp.opcode = IBV_WR_RDMA_WRITE;                 /* pixels, unsignaled */
    wp.sg_list = &sp; wp.num_sge = 1;
    wp.wr.rdma.remote_addr = raddr;
    wp.wr.rdma.rkey = s->hello.rkey;
    wp.next = &wh;
    wh.wr_id = WR_SRC_BASE + slot;                 /* header + immediate; RC keeps the order */
    wh.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wh.sg_list = &sh; wh.num_sge = 1;
    wh.send_flags = IBV_SEND_SIGNALED;
    wh.imm_data = htonl(rdx_imm((uint32_t)seq, slot));
    wh.wr.rdma.remote_addr = raddr + s->hello.hdr_off;
    wh.wr.rdma.rkey = s->hello.rkey;
    if (ibv_post_send(s->qp, &wp, &bad)) { perror("post_send"); return -1; }
    s->rec[slot] = (struct rdx_post_rec){.seq = seq, .addr = mr_off, .mr_addr = mr_addr, .mr_len = mr_len,
                                         .t_ns = hdr->t_post_ns, .len = len, .lkey = mr->lkey, .psn = s->sq_psn_next};
    if (s->tl)
        rdx_tlog_line(s->tl, "P,%ld,%u,0x%llx,%u,0x%x,0x%llx,%llu,0x%06x", seq, slot, (unsigned long long)mr_off, len,
                      mr->lkey, (unsigned long long)mr_addr, (unsigned long long)mr_len, s->sq_psn_next);
    /* PSNs this frame occupies: the pixel write in path-MTU packets, then the header */
    int q = (s->f_head + s->f_n) % (RDX_MAX_SLOTS * 2);
    s->fifo[q].slot = slot;
    s->fifo[q].psn = s->sq_psn_next;
    s->f_n++;
    uint32_t npkt = len ? (len + (uint32_t)s->mtu_bytes - 1) / (uint32_t)s->mtu_bytes : 1;
    s->sq_psn_next = (s->sq_psn_next + npkt + 1) & 0xFFFFFFu;
    s->inflight++;
    s->posted++;
    return 0;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

double rdx_srcsess_net_quantile(const struct rdx_srcsess *s, double q)
{
    if (!s->net_n) return 0;
    double v[128];
    memcpy(v, s->net_ring, sizeof(double) * (size_t)s->net_n);
    qsort(v, (size_t)s->net_n, sizeof(double), cmp_double);
    int i = (int)(q * (s->net_n - 1) + 0.5);
    return v[i];
}

uint64_t rdx_srcsess_vblank_before(const struct rdx_srcsess *s, uint64_t now, double lead_ns)
{
    if (!s->t_shown_last || s->period_ns <= 0) return 0;
    double v = (double)s->t_shown_last - lead_ns;
    double k = floor(((double)now - v) / s->period_ns);
    return (uint64_t)(v + k * s->period_ns);
}

int rdx_srcsess_recover(struct rdx_srcsess *s, uint32_t *lost, int max)
{
    if (s->local) { s->need_recover = 0; return 0; }
    int nlost = 0;
    uint32_t restart = s->f_n ? s->fifo[s->f_head].psn : s->sq_psn_next;
    for (int i = 0; i < s->f_n; i++) {
        uint32_t sl = s->fifo[(s->f_head + i) % (RDX_MAX_SLOTS * 2)].slot;
        if (nlost < max) lost[nlost++] = sl;
    }
    s->f_head = s->f_n = 0;
    s->inflight = 0;
    s->sq_psn_next = restart;
    struct ibv_qp_attr at = {.qp_state = IBV_QPS_RESET};
    if (ibv_modify_qp(s->qp, &at, IBV_QP_STATE)) { perror("modify->RESET"); return -1; }
    if (rdx_qp_to_init(s->qp)) return -1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(s->qp, WR_RECV)) { perror("post_recv"); return -1; }
    struct rdx_conn peer = s->hello.conn;
    peer.psn = (s->hello.conn.psn + (uint32_t)s->recv_ok) & 0xFFFFFFu;
    if (rdx_qp_to_rtr_rts(s->qp, &peer, s->gidx, restart, s->mtu)) return -1;
    s->need_recover = 0;
    s->recoveries++;
    return nlost;
}

void rdx_srcsess_close(struct rdx_srcsess *s)
{
    if (s->local) {
        struct rdx_local_msg m = {.magic = RDX_LOCAL_MAGIC, .type = RDX_LOCAL_END};
        if (s->sock >= 0) { send(s->sock, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT); close(s->sock); }
        s->sock = -1;
        return;
    }
    struct rdx_ev ev[16];
    uint64_t t_end = rdx_now_ns() + 500000000ull;
    while (s->inflight > 0 && rdx_now_ns() < t_end)
        if (rdx_srcsess_poll(s, ev, 16) < 0) break;
    if (s->qp && rdx_post_imm_send(s->qp, RDX_IMM_END, WR_END) == 0) {
        struct rdx_wc wc[16];
        for (int got = 0; !got && rdx_now_ns() < t_end + 500000000ull;) {
            int nwc = rdx_cq_poll(&s->rcq, wc, 16);
            for (int k = 0; k < nwc; k++) if (wc[k].wr_id == WR_END) got = 1;
        }
    }
    if (s->sock >= 0) close(s->sock);
    s->sock = -1;
}
