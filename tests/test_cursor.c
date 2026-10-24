/* The cursor next to the picture (common/rdx_cursor.h) without a GPU, a NIC or gamescope:
 *  - the layouts: struct rdx_cur_msg (48 bytes on the wire), struct rdx_hello (caps in the former padding,
 *    the rest where every older peer reads it), struct rdx_gs_curimg
 *  - gamescope -> rdx_gs_src: CURPOS / CURIMG packets as the rdx backend builds them (CRdxBackend::CursorPosition,
 *    SendCursorImageLocked) over a SOCK_SEQPACKET socketpair into rdx_gs_cursor_take: positions, a picture in
 *    one chunk and in 16, a picture given up half way, clipping to RDX_GS_CUR_MAX, foreign packets left alone
 *  - rdx_gs_src -> sink: rdx_cur_pack_image / rdx_cur_pack_pos into rdx_cur_apply: chunks with positions
 *    between them, a restart, a chunk out of order, repeated positions, bounds
 *  - the bootstrap step of the sink (rdx_cur_rx_connect) over a socketpair: an older source (no flag: nothing
 *    more is read), a source that takes the channel (the flag is cleared, its conn read, ours sent)
 *  - the sink's drawing: rdx_cur_blend (premultiplied over XRGB, clipped at every edge), rdx_cur_scale
 * Build: make tests/test_cursor; run: tests/test_cursor (exit 0 = all passed). */
#include "../gs/rdx_srcsess.h"
#include "../common/rdx_cursor.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* gamescope's side, as CRdxBackend builds the packets */
static void gs_pos(int fd, int32_t x, int32_t y, int vis, uint32_t serial, uint64_t t)
{
    struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_CURPOS, .slot = vis ? RDX_GS_CUR_VISIBLE : 0u, .pad = serial,
                           .seq = (uint64_t)(uint32_t)x | (uint64_t)(uint32_t)y << 32, .t_ns = t};
    CHECK(send(fd, &m, sizeof(m), 0) == (ssize_t)sizeof(m), "send pos");
}

static int src_drains(int fd, struct rdx_gs_cursor *c);

/* the picture of src_w x src_h, clipped to RDX_GS_CUR_MAX, in chunks; stop_after >= 0: only that many chunks.
 * rdx_gs_src (rx, c) reads while gamescope sends, as the two processes do: *changed = what it saw */
static int g_seen;
static void gs_image(int fd, int rx, struct rdx_gs_cursor *cs, uint32_t serial, const uint32_t *px, uint32_t src_w, uint32_t src_h, uint32_t hx, uint32_t hy,
                     int stop_after)
{
    uint32_t w = src_w < RDX_GS_CUR_MAX ? src_w : RDX_GS_CUR_MAX, h = src_h < RDX_GS_CUR_MAX ? src_h : RDX_GS_CUR_MAX;
    uint32_t *clip = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) memcpy(clip + (size_t)y * w, px + (size_t)y * src_w, w * 4);
    static uint8_t pkt[sizeof(struct rdx_gs_curimg) + RDX_GS_CUR_CHUNK];
    uint32_t total = w * h * 4;
    int k = 0;
    for (uint32_t off = 0; off < total && (stop_after < 0 || k < stop_after); off += RDX_GS_CUR_CHUNK, k++) {
        uint32_t len = total - off < RDX_GS_CUR_CHUNK ? total - off : RDX_GS_CUR_CHUNK;
        struct rdx_gs_curimg c = {.magic = RDX_GS_MAGIC, .type = RDX_GS_CURIMG, .serial = serial, .w = (uint16_t)w,
                                  .h = (uint16_t)h, .hot_x = (uint16_t)(hx < w ? hx : w - 1), .hot_y = (uint16_t)(hy < h ? hy : h - 1),
                                  .off = off, .len = len, .t_ns = 77};
        memcpy(pkt, &c, sizeof(c));
        memcpy(pkt + sizeof(c), (uint8_t *)clip + off, len);
        CHECK(send(fd, pkt, sizeof(c) + len, 0) == (ssize_t)(sizeof(c) + len), "send chunk %d", k);
        int ch = src_drains(rx, cs);
        g_seen |= ch < 0 ? 0 : ch;
    }
    free(clip);
}

/* rdx_gs_src's side: its receive buffer and loop; returns the OR of what changed, -1 if a packet was no cursor */
static int src_drains(int fd, struct rdx_gs_cursor *c)
{
    static uint8_t buf[sizeof(struct rdx_gs_curimg) + RDX_GS_CUR_CHUNK];
    int all = 0;
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        int ch = 0;
        if (!rdx_gs_cursor_take(c, buf, (size_t)n, &ch)) return -1;
        all |= ch;
    }
    return all;
}

static uint32_t pix(uint32_t x, uint32_t y, uint32_t salt) { return 0xFF000000u | ((x * 7 + y * 13 + salt) & 0xFFFFFFu); }

static void test_gs_to_src(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");
    /* gamescope's side sends up to 16 KiB + 40 per packet: the default socket buffer takes it */
    struct rdx_gs_cursor *c = calloc(1, sizeof(*c));
    gs_pos(sv[0], 100, -3, 1, 1, 5);
    CHECK(src_drains(sv[1], c) == RDX_CUR_CH_POS, "a position");
    CHECK(c->have_pos && c->visible && c->x == 100 && c->y == -3 && c->pos_serial == 1 && c->t_pos == 5 && c->pos_dirty,
          "position %d,%d vis %d", c->x, c->y, c->visible);
    gs_pos(sv[0], -20, 1079, 0, 1, 6);
    src_drains(sv[1], c);
    CHECK(!c->visible && c->x == -20 && c->y == 1079, "negative x, hidden");

    /* a 24x24 picture: one chunk */
    uint32_t *px = malloc(300 * 300 * 4);
    for (uint32_t y = 0; y < 300; y++) for (uint32_t x = 0; x < 300; x++) px[y * 300 + x] = pix(x, y, 1);
    uint32_t small[24 * 24];
    for (uint32_t y = 0; y < 24; y++) for (uint32_t x = 0; x < 24; x++) small[y * 24 + x] = pix(x, y, 2);
    g_seen = 0;
    gs_image(sv[0], sv[1], c, 2, small, 24, 24, 3, 4, -1);
    CHECK(g_seen == RDX_CUR_CH_IMAGE, "picture complete");
    CHECK(c->serial == 2 && c->w == 24 && c->h == 24 && c->hx == 3 && c->hy == 4 && c->img_dirty && c->t_img == 77, "picture fields");
    CHECK(!memcmp(c->px, small, sizeof(small)), "24x24 pixels");

    /* 300x300 is clipped to 256x256 = 16 chunks, with a position between two of them */
    g_seen = 0;
    gs_image(sv[0], sv[1], c, 3, px, 300, 300, 299, 10, 5);
    gs_pos(sv[0], 7, 8, 1, 3, 9);
    CHECK((src_drains(sv[1], c) | g_seen) == RDX_CUR_CH_POS && c->serial == 2, "half a picture: the old one stays, the position counts");
    g_seen = 0;
    gs_image(sv[0], sv[1], c, 3, px, 300, 300, 299, 10, -1);   /* sent again from the start */
    CHECK(g_seen == RDX_CUR_CH_IMAGE, "the whole picture after a restart");
    CHECK(c->serial == 3 && c->w == 256 && c->h == 256 && c->hx == 255 && c->hy == 10, "clipped %ux%u hot %u,%u", c->w, c->h, c->hx, c->hy);
    int same = 1;
    for (uint32_t y = 0; y < 256 && same; y++)
        for (uint32_t x = 0; x < 256; x++) if (c->px[y * 256 + x] != pix(x, y, 1)) { same = 0; break; }
    CHECK(same, "the top left 256x256 of the picture");

    /* a chunk that does not continue the picture coming in changes nothing */
    {
        struct rdx_gs_curimg ci = {.magic = RDX_GS_MAGIC, .type = RDX_GS_CURIMG, .serial = 4, .w = 64, .h = 128,
                                   .off = RDX_GS_CUR_CHUNK, .len = RDX_GS_CUR_CHUNK};
        static uint8_t pkt[sizeof(ci) + RDX_GS_CUR_CHUNK];
        memcpy(pkt, &ci, sizeof(ci));
        send(sv[0], pkt, sizeof(pkt), 0);
        CHECK(src_drains(sv[1], c) == 0 && c->serial == 3, "a second chunk without the first is dropped");
        ci.w = 300;                                           /* too big */
        ci.off = 0;
        memcpy(pkt, &ci, sizeof(ci));
        send(sv[0], pkt, sizeof(pkt), 0);
        CHECK(src_drains(sv[1], c) == 0 && c->serial == 3, "a picture over RDX_GS_CUR_MAX is refused");
    }
    /* not the cursor's: READY, HINT and a short packet are left to the caller */
    {
        struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_READY, .slot = 1};
        int ch = 0;
        CHECK(!rdx_gs_cursor_take(c, &m, sizeof(m), &ch), "READY is no cursor packet");
        m.type = RDX_GS_HINT;
        CHECK(!rdx_gs_cursor_take(c, &m, sizeof(m), &ch), "HINT is no cursor packet");
        m.type = RDX_GS_CURPOS;
        CHECK(!rdx_gs_cursor_take(c, &m, 16, &ch), "short packet");
    }
    free(px);
    free(c);
    close(sv[0]);
    close(sv[1]);
}

static void test_wire(void)
{
    struct rdx_cur_state *st = calloc(1, sizeof(*st));
    static uint8_t msg[RDX_CUR_RECV_BYTES];
    uint32_t *px = malloc(RDX_CUR_MAX * RDX_CUR_MAX * 4);
    for (uint32_t i = 0; i < RDX_CUR_MAX * RDX_CUR_MAX; i++) px[i] = 0x80000000u | i;
    CHECK(rdx_cur_nchunks(64, 64) == 1 && rdx_cur_nchunks(65, 64) == 2 && rdx_cur_nchunks(256, 256) == 16 && rdx_cur_nchunks(0, 5) == 0,
          "chunk counts");
    size_t n = rdx_cur_pack_pos(msg, 1, 1, 10, 20, 99);
    CHECK(n == sizeof(struct rdx_cur_msg) && rdx_cur_apply(st, msg, n) == RDX_CUR_CH_POS, "position");
    CHECK(st->x == 10 && st->y == 20 && st->visible && st->t_src == 99, "position fields");
    CHECK(rdx_cur_apply(st, msg, n) == 0, "the same position again changes nothing");
    rdx_cur_pack_pos(msg, 1, 0, 10, 20, 100);
    CHECK(rdx_cur_apply(st, msg, n) == RDX_CUR_CH_POS && !st->visible, "hidden is a change");

    /* 200x100: 5 chunks, a position after the second */
    int nc = rdx_cur_nchunks(200, 100), got = 0;
    for (int k = 0; k < nc; k++) {
        size_t m = rdx_cur_pack_image(msg, 7, 200, 100, 5, 6, px, k, 1);
        CHECK(m > sizeof(struct rdx_cur_msg) && m <= RDX_CUR_RECV_BYTES, "chunk %d length %zu", k, m);
        got |= rdx_cur_apply(st, msg, RDX_CUR_RECV_BYTES);      /* the sink knows only its buffer size */
        if (k == 1) {
            size_t p = rdx_cur_pack_pos(msg, 7, 1, 1, 2, 2);
            CHECK(rdx_cur_apply(st, msg, p) == RDX_CUR_CH_POS && st->serial == 0, "a position between chunks");
        }
        if (k < nc - 1) CHECK(!(got & RDX_CUR_CH_IMAGE), "not complete before the last chunk");
    }
    CHECK(got & RDX_CUR_CH_IMAGE, "complete");
    CHECK(st->serial == 7 && st->w == 200 && st->h == 100 && st->hot_x == 5 && st->hot_y == 6, "picture fields");
    CHECK(!memcmp(st->px, px, 200 * 100 * 4), "pixels");
    CHECK(rdx_cur_pack_image(msg, 7, 200, 100, 5, 6, px, nc, 1) == 0, "no chunk past the end");

    /* a picture given up half way: the complete one stays; a chunk out of order is refused */
    rdx_cur_pack_image(msg, 8, 256, 256, 0, 0, px, 0, 1);
    rdx_cur_apply(st, msg, RDX_CUR_RECV_BYTES);
    rdx_cur_pack_image(msg, 8, 256, 256, 0, 0, px, 2, 1);
    CHECK(rdx_cur_apply(st, msg, RDX_CUR_RECV_BYTES) < 0 && st->serial == 7, "chunk 2 after 0 is refused");
    for (int k = 0; k < 16; k++) {
        rdx_cur_pack_image(msg, 9, 256, 256, 255, 255, px, k, 1);
        rdx_cur_apply(st, msg, RDX_CUR_RECV_BYTES);
    }
    CHECK(st->serial == 9 && st->w == 256 && !memcmp(st->px, px, 256 * 256 * 4), "a restarted serial completes");

    /* bounds */
    struct rdx_cur_msg bad = {.magic = RDX_CUR_MAGIC, .type = RDX_CUR_IMAGE, .serial = 10, .w = 300, .h = 4, .len = 16};
    CHECK(rdx_cur_apply(st, &bad, sizeof(bad) + 16) < 0, "w > RDX_CUR_MAX");
    bad.w = 4; bad.len = RDX_CUR_CHUNK + 4;
    CHECK(rdx_cur_apply(st, &bad, sizeof(bad) + 16) < 0, "len > RDX_CUR_CHUNK");
    bad.len = 64; bad.off = 16;
    CHECK(rdx_cur_apply(st, &bad, sizeof(bad) + 64) < 0, "off + len past the picture");
    bad.magic = 0;
    CHECK(rdx_cur_apply(st, &bad, sizeof(bad)) < 0, "magic");
    CHECK(rdx_cur_apply(st, &bad, 10) < 0, "short");
    CHECK(st->serial == 9, "the picture survived");
    free(px);
    free(st);
}

static void test_bootstrap(void)
{
    struct rdx_cur_rx c;
    memset(&c, 0, sizeof(c));        /* no QP here: the socket half of the step */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    uint8_t gid[16] = {0};
    struct rdx_conn peer = {.qpn = 11, .psn = 0x3456};
    CHECK(rdx_cur_rx_connect(&c, sv[1], &peer, 0, gid, IBV_MTU_4096) == 0 && peer.psn == 0x3456 && !c.up,
          "an older source: nothing more on the socket");
    char b;
    CHECK(recv(sv[0], &b, 1, MSG_DONTWAIT) < 0, "nothing was sent");
    peer.psn = 0x3456 | RDX_CONN_F_CURSOR;
    struct rdx_conn src = {.qpn = 12, .psn = 0x1357};
    CHECK(send(sv[0], &src, sizeof(src), 0) == (ssize_t)sizeof(src), "the source's cursor conn");
    CHECK(rdx_cur_rx_connect(&c, sv[1], &peer, 0, gid, IBV_MTU_4096) == 0 && peer.psn == 0x3456,
          "the flag is cleared, the frames' PSN stays 24 bits");
    struct rdx_conn back;
    CHECK(recv(sv[0], &back, sizeof(back), MSG_DONTWAIT) == (ssize_t)sizeof(back) && back.qpn == 0,
          "our conn goes back (qpn 0 without a QP: the source then keeps the cursor in the picture)");
    CHECK(RDX_CONN_F_CURSOR & 0xFF000000u && !(RDX_CONN_F_CURSOR & 0xFFFFFFu), "the flag is outside the 24-bit PSN");
    close(sv[0]);
    close(sv[1]);
}

static void test_draw(void)
{
    enum { FW = 40, FH = 30, P = FW * 4 + 64 };
    static uint8_t fb[P * FH];
    for (int y = 0; y < FH; y++) for (int x = 0; x < FW; x++) ((uint32_t *)(fb + y * P))[x] = 0x00102030u;
    uint32_t cur[4 * 4];
    for (int i = 0; i < 16; i++) cur[i] = 0xFFFFFFFFu;     /* opaque white */
    cur[5] = 0x00000000u;                                    /* transparent */
    cur[6] = 0x80808080u;                                    /* half: 0x80 + 0x10/0x20/0x30 * 127/255 */
    uint32_t want = (0x80u + 0x10u * 127 / 255) << 16 | (0x80u + 0x20u * 127 / 255) << 8 | (0x80u + 0x30u * 127 / 255);
    rdx_cur_blend(fb, P, FW, FH, cur, 4, 4, -2, -1);          /* clipped top left: frame (0,0) = cursor (2,1) */
    uint32_t *r0 = (uint32_t *)fb, *r1 = (uint32_t *)(fb + P), *r2 = (uint32_t *)(fb + 2 * P);
    CHECK(r0[0] == want && r0[1] == 0x00FFFFFFu && r0[2] == 0x00102030u, "row 0 (cursor row 1): %08x %08x %08x", r0[0], r0[1], r0[2]);
    CHECK(r1[0] == 0x00FFFFFFu && r2[1] == 0x00FFFFFFu && r2[2] == 0x00102030u, "rows below, right edge of the cursor");
    rdx_cur_blend(fb, P, FW, FH, cur, 4, 4, 10, 10);
    uint32_t *r11 = (uint32_t *)(fb + 11 * P);
    CHECK(r11[11] == 0x00102030u, "transparent pixel leaves the frame: %08x", r11[11]);
    CHECK(r11[12] == want, "half alpha premultiplied: %08x, want %08x", r11[12], want);
    rdx_cur_blend(fb, P, FW, FH, cur, 4, 4, FW - 1, FH - 1);  /* clipped bottom right */
    CHECK(((uint32_t *)(fb + (FH - 1) * P))[FW - 1] == 0x00FFFFFFu, "bottom right pixel");
    rdx_cur_blend(fb, P, FW, FH, cur, 4, 4, 100, 100);        /* outside: nothing, no crash */
    rdx_cur_blend(fb, P, FW, FH, cur, 4, 4, -10, -10);

    static uint32_t out[RDX_CUR_MAX * RDX_CUR_MAX];
    uint32_t src[8 * 8];
    for (int i = 0; i < 64; i++) src[i] = (i % 2) ? 0xFFFFFFFFu : 0xFF000000u;
    int ow, oh;
    rdx_cur_scale(src, 8, 8, 1.0, out, &ow, &oh);
    CHECK(ow == 8 && oh == 8 && !memcmp(out, src, sizeof(src)), "scale 1 is a copy");
    rdx_cur_scale(src, 8, 8, 0.5, out, &ow, &oh);
    CHECK(ow == 4 && oh == 4 && out[0] == 0xFF7F7F7Fu, "half: area average %08x", out[0]);
    rdx_cur_scale(src, 8, 8, 2.0, out, &ow, &oh);
    CHECK(ow == 16 && oh == 16 && out[0] == 0xFF000000u && out[1] == 0xFF000000u && out[2] == 0xFFFFFFFFu, "double: nearest");
    rdx_cur_scale(src, 8, 8, 0.01, out, &ow, &oh);
    CHECK(ow == 1 && oh == 1, "at least 1x1");
    static uint32_t wide[200];
    rdx_cur_scale(wide, 200, 1, 3.0, out, &ow, &oh);
    CHECK(ow == RDX_CUR_MAX && oh == 3, "clamped to RDX_CUR_MAX: %dx%d", ow, oh);
}

int main(void)
{
    CHECK(sizeof(struct rdx_cur_msg) == 48 && offsetof(struct rdx_cur_msg, t_ns) == 40, "rdx_cur_msg layout");
    CHECK(offsetof(struct rdx_hello, caps) == 76 && offsetof(struct rdx_hello, vblank_ns) == 80 && sizeof(struct rdx_hello) == 88,
          "rdx_hello: caps in the old padding, vblank_ns and the size unchanged");
    CHECK(sizeof(struct rdx_gs_curimg) == 40 && sizeof(struct rdx_gs_msg) == 40, "gs packet layouts");
    CHECK(RDX_GS_CURSOR == 10 && RDX_GS_CURPOS == 11 && RDX_GS_CURIMG == 12 && RDX_GS_CUR_MAX == RDX_CUR_MAX &&
          RDX_GS_CUR_CHUNK == RDX_CUR_CHUNK, "proto values");
    test_gs_to_src();
    test_wire();
    test_bootstrap();
    test_draw();
    printf("test_cursor: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
