/* SPDX-License-Identifier: MIT
 *
 * Bidirectional RDMA message channel for the USB/HID path: one RC QP, a ring
 * of posted receive buffers on each side, two-sided SEND. Small messages (all
 * ordinary HID reports) go inline. Event driven: the caller polls the
 * completion channel fd together with its other fds and calls dispatch().
 */
#define _GNU_SOURCE
#include "rdx_usb.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int post_recv(struct rdx_usb_chan *c, uint32_t i)
{
    struct ibv_sge sge = {.addr = (uintptr_t)(c->rbuf + (size_t)i * RDX_USB_MSG_MAX),
                          .length = (uint32_t)RDX_USB_MSG_MAX, .lkey = c->rmr->lkey};
    struct ibv_recv_wr wr = {.wr_id = i, .sg_list = &sge, .num_sge = 1}, *bad;
    return ibv_post_recv(c->qp, &wr, &bad);
}

int rdx_usb_chan_init(struct rdx_usb_chan *c, const char *dev, const char *local_ip, int comp_vector)
{
    memset(c, 0, sizeof(*c));
    c->ctx = rdx_open_dev_ip(dev, local_ip);
    if (!c->ctx) return -1;
    c->pd = ibv_alloc_pd(c->ctx);
    c->ch = c->pd ? ibv_create_comp_channel(c->ctx) : NULL;
    if (!c->ch) { perror("pd/comp channel"); return -1; }
    fcntl(c->ch->fd, F_SETFL, fcntl(c->ch->fd, F_GETFL) | O_NONBLOCK);
    comp_vector = rdx_pick_comp_vector(c->ctx, comp_vector);
    c->cq = ibv_create_cq(c->ctx, RDX_USB_NRECV + RDX_USB_NSEND + 16, NULL, c->ch, comp_vector);
    if (!c->cq) { perror("ibv_create_cq"); return -1; }
    c->max_inline = 256;
    c->qp = rdx_create_qp_inline(c->pd, c->cq, RDX_USB_NRECV > RDX_USB_NSEND ? RDX_USB_NRECV : RDX_USB_NSEND,
                                 c->max_inline);
    if (!c->qp) {
        c->max_inline = 0;
        c->qp = rdx_create_qp(c->pd, c->cq, RDX_USB_NRECV);
    }
    if (!c->qp || rdx_qp_to_init(c->qp)) return -1;
    size_t rb = (size_t)RDX_USB_NRECV * RDX_USB_MSG_MAX, sb = (size_t)RDX_USB_NSEND * RDX_USB_MSG_MAX;
    if (posix_memalign((void **)&c->rbuf, 4096, rb) || posix_memalign((void **)&c->sbuf, 4096, sb)) return -1;
    c->rmr = ibv_reg_mr(c->pd, c->rbuf, rb, IBV_ACCESS_LOCAL_WRITE);
    c->smr = ibv_reg_mr(c->pd, c->sbuf, sb, 0);
    if (!c->rmr || !c->smr) { perror("ibv_reg_mr"); return -1; }
    for (uint32_t i = 0; i < RDX_USB_NRECV; i++)
        if (post_recv(c, i)) { perror("post_recv"); return -1; }
    c->gidx = rdx_find_gid_index_ip(c->ctx, local_ip, c->mine.gid);
    if (c->gidx < 0) return -1;
    c->mine.qpn = c->qp->qp_num;
    c->mine.psn = (uint32_t)(rdx_now_ns() & 0xFFFFFF);
    return 0;
}

int rdx_usb_chan_connect(struct rdx_usb_chan *c, const struct rdx_conn *peer, int mtu)
{
    if (rdx_qp_to_rtr_rts(c->qp, peer, c->gidx, c->mine.psn, rdx_parse_mtu(mtu))) return -1;
    return ibv_req_notify_cq(c->cq, 0);
}

int rdx_usb_chan_fd(const struct rdx_usb_chan *c) { return c->ch->fd; }

static int drain(struct rdx_usb_chan *c, rdx_usb_msg_cb cb, void *user)
{
    struct ibv_wc wc[32];
    int n, total = 0;
    while ((n = ibv_poll_cq(c->cq, 32, wc)) > 0) {
        for (int k = 0; k < n; k++) {
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[usb] WC error: %s (opcode %d)\n", ibv_wc_status_str(wc[k].status), wc[k].opcode);
                return -1;
            }
            if (wc[k].opcode == IBV_WC_SEND) { c->sinflight--; continue; }
            if (wc[k].opcode != IBV_WC_RECV) continue;
            uint32_t i = (uint32_t)wc[k].wr_id;
            const uint8_t *b = c->rbuf + (size_t)i * RDX_USB_MSG_MAX;
            const struct rdx_usb_msg *m = (const struct rdx_usb_msg *)b;
            if (wc[k].byte_len >= sizeof(*m) && m->len <= wc[k].byte_len - sizeof(*m) && cb)
                cb(m, b + sizeof(*m), user);
            if (post_recv(c, i)) { perror("repost"); return -1; }
            total++;
        }
    }
    return n < 0 ? -1 : total;
}

int rdx_usb_chan_dispatch(struct rdx_usb_chan *c, rdx_usb_msg_cb cb, void *user)
{
    struct ibv_cq *ecq;
    void *ectx;
    while (!ibv_get_cq_event(c->ch, &ecq, &ectx)) ibv_ack_cq_events(ecq, 1);   /* fd is non-blocking */
    for (;;) {
        if (drain(c, cb, user) < 0) return -1;
        /* arm, then poll once more: closes the race with a completion that landed in between */
        if (ibv_req_notify_cq(c->cq, 0)) return -1;
        int n = drain(c, cb, user);
        if (n < 0) return -1;
        if (n == 0) return 0;
    }
}

int rdx_usb_chan_send(struct rdx_usb_chan *c, const struct rdx_usb_msg *h, const void *payload,
                      rdx_usb_msg_cb cb, void *user)
{
    while (c->sinflight >= RDX_USB_NSEND) {       /* peer slow: wait for send completions */
        struct pollfd p = {.fd = c->ch->fd, .events = POLLIN};
        if (poll(&p, 1, 1000) < 0 && errno != EINTR) return -1;
        if (rdx_usb_chan_dispatch(c, cb, user)) return -1;
    }
    uint8_t *slot = c->sbuf + (size_t)c->shead * RDX_USB_MSG_MAX;
    uint32_t len = (uint32_t)sizeof(*h) + h->len;
    memcpy(slot, h, sizeof(*h));
    if (h->len) memcpy(slot + sizeof(*h), payload, h->len);
    struct ibv_sge sge = {.addr = (uintptr_t)slot, .length = len, .lkey = c->smr->lkey};
    struct ibv_send_wr wr = {.wr_id = c->shead, .sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_SEND,
                             .send_flags = IBV_SEND_SIGNALED}, *bad;
    if (len <= c->max_inline) wr.send_flags |= IBV_SEND_INLINE;
    if (ibv_post_send(c->qp, &wr, &bad)) { perror("ibv_post_send"); return -1; }
    c->shead = (c->shead + 1) % RDX_USB_NSEND;
    c->sinflight++;
    return 0;
}

void rdx_usb_chan_destroy(struct rdx_usb_chan *c)
{
    if (c->qp) ibv_destroy_qp(c->qp);
    if (c->rmr) ibv_dereg_mr(c->rmr);
    if (c->smr) ibv_dereg_mr(c->smr);
    if (c->cq) ibv_destroy_cq(c->cq);
    if (c->ch) ibv_destroy_comp_channel(c->ch);
    if (c->pd) ibv_dealloc_pd(c->pd);
    if (c->ctx) ibv_close_device(c->ctx);
    free(c->rbuf);
    free(c->sbuf);
    memset(c, 0, sizeof(*c));
}
