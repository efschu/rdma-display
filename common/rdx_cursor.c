/* SPDX-License-Identifier: MIT
 * The pointer next to the picture, see rdx_cursor.h.
 */
#include "rdx_cursor.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct rdx_cur_msg) == 48, "rdx_cur_msg is part of the wire protocol");
/* caps took the hello's padding: the layout every older peer reads is unchanged */
_Static_assert(offsetof(struct rdx_hello, caps) == 76 && offsetof(struct rdx_hello, vblank_ns) == 80 &&
               sizeof(struct rdx_hello) == 88, "rdx_hello layout");

/* ---------------- pure parts ---------------- */

int rdx_cur_apply(struct rdx_cur_state *s, const void *msg, size_t len)
{
    struct rdx_cur_msg m;
    if (len < sizeof(m)) { s->n_bad++; return -1; }
    memcpy(&m, msg, sizeof(m));
    if (m.magic != RDX_CUR_MAGIC) { s->n_bad++; return -1; }
    s->t_src = m.t_ns;
    if (m.type == RDX_CUR_POS) {
        int vis = (m.flags & RDX_CUR_F_VISIBLE) != 0;
        s->n_pos++;
        if (s->have_pos && vis == s->visible && m.x == s->x && m.y == s->y) return 0;
        s->have_pos = 1;
        s->visible = vis;
        s->x = m.x;
        s->y = m.y;
        return RDX_CUR_CH_POS;
    }
    if (m.type != RDX_CUR_IMAGE) { s->n_bad++; return -1; }
    uint32_t total = (uint32_t)m.w * m.h * 4;
    if (!m.w || !m.h || m.w > RDX_CUR_MAX || m.h > RDX_CUR_MAX || m.len > RDX_CUR_CHUNK ||
        len < sizeof(m) + m.len || m.off > total || m.len > total - m.off) {
        s->n_bad++;
        return -1;
    }
    if (m.off == 0) {                     /* a picture starts (a new one, or the same sent again) */
        s->in_serial = m.serial;
        s->in_got = 0;
        s->in_w = m.w; s->in_h = m.h; s->in_hx = m.hot_x; s->in_hy = m.hot_y;
    }
    /* chunks arrive in order (RC); one that does not continue the picture coming in is dropped */
    if (m.serial != s->in_serial || m.off != s->in_got || m.w != s->in_w || m.h != s->in_h) { s->n_bad++; return -1; }
    memcpy((uint8_t *)s->in_px + m.off, (const uint8_t *)msg + sizeof(m), m.len);
    s->in_got += m.len;
    if (s->in_got < total) return 0;
    s->serial = s->in_serial;
    s->w = s->in_w; s->h = s->in_h; s->hot_x = s->in_hx; s->hot_y = s->in_hy;
    memcpy(s->px, s->in_px, total);
    s->in_got = 0;
    s->in_serial = 0;
    s->n_img++;
    return RDX_CUR_CH_IMAGE;
}

int rdx_cur_nchunks(uint32_t w, uint32_t h)
{
    uint32_t total = w * h * 4;
    return total ? (int)((total + RDX_CUR_CHUNK - 1) / RDX_CUR_CHUNK) : 0;
}

size_t rdx_cur_pack_image(void *out, uint32_t serial, uint32_t w, uint32_t h, uint32_t hot_x, uint32_t hot_y,
                          const uint32_t *px, int k, uint64_t t_ns)
{
    uint32_t total = w * h * 4, off = (uint32_t)k * RDX_CUR_CHUNK;
    if (off >= total) return 0;
    uint32_t len = total - off < RDX_CUR_CHUNK ? total - off : RDX_CUR_CHUNK;
    struct rdx_cur_msg m = {.magic = RDX_CUR_MAGIC, .type = RDX_CUR_IMAGE, .serial = serial, .w = (uint16_t)w,
                            .h = (uint16_t)h, .hot_x = (uint16_t)hot_x, .hot_y = (uint16_t)hot_y, .off = off, .len = len,
                            .t_ns = t_ns};
    memcpy(out, &m, sizeof(m));
    memcpy((uint8_t *)out + sizeof(m), (const uint8_t *)px + off, len);
    return sizeof(m) + len;
}

size_t rdx_cur_pack_pos(void *out, uint32_t serial, int visible, int32_t x, int32_t y, uint64_t t_ns)
{
    struct rdx_cur_msg m = {.magic = RDX_CUR_MAGIC, .type = RDX_CUR_POS, .flags = visible ? RDX_CUR_F_VISIBLE : 0,
                            .serial = serial, .x = x, .y = y, .t_ns = t_ns};
    memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

void rdx_cur_blend(uint8_t *dst, uint32_t pitch, uint32_t fw, uint32_t fh, const uint32_t *px, int w, int h,
                   int x, int y)
{
    for (int r = 0; r < h; r++) {
        int fy = y + r;
        if (fy < 0 || fy >= (int)fh) continue;
        uint32_t *d = (uint32_t *)(dst + (size_t)fy * pitch);
        const uint32_t *s = px + (size_t)r * w;
        for (int c = 0; c < w; c++) {
            int fx = x + c;
            if (fx < 0 || fx >= (int)fw) continue;
            uint32_t p = s[c], a = p >> 24;
            if (!a) continue;
            if (a == 255) { d[fx] = p & 0x00FFFFFFu; continue; }
            uint32_t q = d[fx], ia = 255 - a;
            uint32_t rr = ((p >> 16) & 255) + ((q >> 16) & 255) * ia / 255, gg = ((p >> 8) & 255) + ((q >> 8) & 255) * ia / 255,
                     bb = (p & 255) + (q & 255) * ia / 255;
            d[fx] = (rr > 255 ? 255 : rr) << 16 | (gg > 255 ? 255 : gg) << 8 | (bb > 255 ? 255 : bb);
        }
    }
}

void rdx_cur_scale(const uint32_t *px, int w, int h, double s, uint32_t *out, int *ow, int *oh)
{
    if (!(s > 0)) s = 1;
    if (s > 4) s = 4;
    int nw = (int)(w * s + 0.5), nh = (int)(h * s + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > RDX_CUR_MAX) nw = RDX_CUR_MAX;
    if (nh > RDX_CUR_MAX) nh = RDX_CUR_MAX;
    for (int oy = 0; oy < nh; oy++)
        for (int ox = 0; ox < nw; ox++) {
            /* the source rectangle of this pixel; growing: one source pixel (nearest) */
            double x0 = ox / s, x1 = (ox + 1) / s, y0 = oy / s, y1 = (oy + 1) / s;
            int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)(x1 - 1e-9), iy1 = (int)(y1 - 1e-9);
            if (s >= 1) { ix1 = ix0; iy1 = iy0; }
            if (ix1 >= w) ix1 = w - 1;
            if (iy1 >= h) iy1 = h - 1;
            if (ix0 >= w) ix0 = w - 1;
            if (iy0 >= h) iy0 = h - 1;
            uint32_t sum[4] = {0}, n = 0;
            for (int yy = iy0; yy <= iy1; yy++)
                for (int xx = ix0; xx <= ix1; xx++) {
                    uint32_t p = px[(size_t)yy * w + xx];
                    sum[0] += p >> 24; sum[1] += (p >> 16) & 255; sum[2] += (p >> 8) & 255; sum[3] += p & 255;
                    n++;
                }
            out[(size_t)oy * nw + ox] = n ? (sum[0] / n) << 24 | (sum[1] / n) << 16 | (sum[2] / n) << 8 | (sum[3] / n) : 0;
        }
    *ow = nw;
    *oh = nh;
}

/* ---------------- the sink's end ---------------- */

static int post_recv(struct rdx_cur_rx *c, uint32_t i)
{
    struct ibv_sge sge = {.addr = (uintptr_t)(c->buf + (size_t)i * RDX_CUR_RECV_BYTES), .length = RDX_CUR_RECV_BYTES,
                          .lkey = c->mr->lkey};
    struct ibv_recv_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = RDX_CUR_WR_TAG | i;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    return ibv_post_recv(c->qp, &wr, &bad);
}

int rdx_cur_rx_init(struct rdx_cur_rx *c, struct ibv_pd *pd, struct ibv_cq *cq)
{
    memset(c, 0, sizeof(*c));
    c->st = calloc(1, sizeof(*c->st));
    c->buf = calloc(RDX_CUR_NRECV, RDX_CUR_RECV_BYTES);
    if (!c->st || !c->buf) goto fail;
    c->mr = ibv_reg_mr(pd, c->buf, (size_t)RDX_CUR_NRECV * RDX_CUR_RECV_BYTES, IBV_ACCESS_LOCAL_WRITE);
    c->qp = c->mr ? rdx_create_qp(pd, cq, RDX_CUR_NRECV) : NULL;
    if (!c->qp || rdx_qp_to_init(c->qp)) goto fail;
    for (uint32_t i = 0; i < RDX_CUR_NRECV; i++)
        if (post_recv(c, i)) goto fail;
    return 0;
fail:
    fprintf(stderr, "[cursor] no cursor channel (%s): the source draws the pointer into the picture\n", strerror(errno));
    rdx_cur_rx_fini(c);
    return -1;
}

int rdx_cur_rx_connect(struct rdx_cur_rx *c, int sock, struct rdx_conn *peer, int sgid_idx, const uint8_t gid[16],
                       enum ibv_mtu mtu)
{
    int flagged = (peer->psn & RDX_CONN_F_CURSOR) != 0;
    peer->psn &= 0xFFFFFFu;
    if (!flagged) return 0;
    /* the source took the offer: its cursor conn follows, ours goes back, whatever happens here */
    struct rdx_conn src, mine;
    memset(&mine, 0, sizeof(mine));
    if (rdx_io_all(sock, &src, sizeof(src), 0)) return -1;
    if (c->qp) {
        mine.qpn = c->qp->qp_num;
        mine.psn = 0x2468;
        memcpy(mine.gid, gid, 16);
    }
    if (rdx_io_all(sock, &mine, sizeof(mine), 1)) return -1;
    if (!c->qp) return 0;                /* never offered without a QP; a source that ignored that gets qpn 0 */
    if (rdx_qp_to_rtr_rts(c->qp, &src, sgid_idx, mine.psn, mtu)) {
        c->failed = 1;
        return 0;
    }
    c->up = 1;
    return 1;
}

int rdx_cur_rx_complete(struct rdx_cur_rx *c, uint64_t wr_id, int ok)
{
    if (!ok) {
        if (!c->failed) fprintf(stderr, "[cursor] the cursor channel failed: no cursor of our own any more\n");
        c->failed = 1;
        return -1;
    }
    uint32_t i = (uint32_t)(wr_id & 0xFFFFu);
    if (i >= RDX_CUR_NRECV || !c->qp) return 0;
    const uint8_t *b = c->buf + (size_t)i * RDX_CUR_RECV_BYTES;
    int r = rdx_cur_apply(c->st, b, RDX_CUR_RECV_BYTES);
    if (!c->failed && post_recv(c, i)) {
        fprintf(stderr, "[cursor] repost failed: %s\n", strerror(errno));
        c->failed = 1;
    }
    return r < 0 ? 0 : r;
}

void rdx_cur_rx_fini(struct rdx_cur_rx *c)
{
    if (c->qp) ibv_destroy_qp(c->qp);
    if (c->mr) ibv_dereg_mr(c->mr);
    free(c->buf);
    free(c->st);
    memset(c, 0, sizeof(*c));
}
