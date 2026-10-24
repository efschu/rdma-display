/* Display-less stand-in for rdx_sink_kms, for testing sources without taking a screen.
 *
 * Same wire protocol and the same credit/shown semantics as p0b/rdx_sink_kms.c, but the
 * "display" is a CLOCK_MONOTONIC timer at the requested refresh: a frame that arrived
 * becomes ready, the newest ready frame is latched at the flip deadline (vblank - lead),
 * shows at the vblank (SHOWN report for it, credit for the previous one), and a ready
 * frame overtaken by a newer one is reported dropped. Pixels land in host RAM; the last
 * shown frame can be written to a PPM file as visual proof.
 *
 *   rdx_sink_null [--dev auto] [--bind IP] [--port 18650] [--mtu 4096] [--slots 3]
 *                 [--mode 1920x1080@60] [--flip-lead-us 2000] [--seconds 0] [--dump FILE.ppm]
 *                 [--hash]   (checksum of every received frame, see gs/rdx_hash.h)
 *                 [--tearing] (like rdx_sink_kms --tearing: frames count as shown on arrival, credit at once;
 *                             use with --slots 1)
 *                 [--watch X,Y] (end-to-end latency probe, tools/rdx_perf.py: the mean luma of a 16x16
 *                             box at pixel X,Y of every frame; each time it crosses dark <-> bright one
 *                             "[watch] arrive" line at the frame's arrival (software and NIC CQE clock,
 *                             CLOCK_MONOTONIC ns) and one "[watch] shown" line at the vblank that shows
 *                             the first frame of the new state)
 *                 [--watch-cursor] (the pointer latency probe, tools/rdx_perf.py --latency with the cursor load:
 *                             every frame whose sampled picture differs from the frame before is one "[watch]
 *                             arrive" line (bright alternates), and every position of the host's cursor next
 *                             to the picture (common/rdx_cursor.h) one "[watch] cursor" line at its arrival
 *                             plus "[watch] cursor-shown" at the vblank that shows it: a cursor drawn into
 *                             the frames and one beside them, measured the same way)
 *                 [--cursor auto|picture] (auto: offer the cursor channel like the real sinks do; picture: the
 *                             source draws the cursor into the frames, as for an older sink)
 *
 * At the end: frames received / shown / dropped, arrival -> vblank (software clock, and NIC clock
 * when the CQ has completion timestamps) and the payload rate (frames x stride x height).
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"
#include "../common/rdx_cursor.h"
#include "rdx_hash.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define WR_RECV   0x200u
#define WR_CREDIT 0x400u
#define NRECV     32

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static void arm_at(int tfd, uint64_t t_ns)
{
    struct itimerspec its = {.it_value = {.tv_sec = (time_t)(t_ns / 1000000000ull), .tv_nsec = (long)(t_ns % 1000000000ull)}};
    timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* a hash of every 4th pixel of every 2nd row: changes when a cursor of 8x8 pixels or more moves */
static uint64_t sample_hash(const uint8_t *p, uint32_t w, uint32_t h, uint32_t stride)
{
    uint64_t x = 1469598103934665603ull;
    for (uint32_t yy = 0; yy < h; yy += 2) {
        const uint32_t *r = (const uint32_t *)(p + (size_t)yy * stride);
        for (uint32_t xx = (yy >> 1) & 3; xx < w; xx += 4) x = (x ^ (r[xx] & 0x00FFFFFFu)) * 1099511628211ull;
    }
    return x;
}

/* mean luma (0..255) of a 16x16 box (every 2nd pixel) around x,y of an XRGB8888 frame */
static int box_luma(const uint8_t *p, uint32_t w, uint32_t h, uint32_t stride, uint32_t x, uint32_t y)
{
    uint32_t x0 = x >= 8 ? x - 8 : 0, y0 = y >= 8 ? y - 8 : 0;
    if (x0 + 16 > w) x0 = w > 16 ? w - 16 : 0;
    if (y0 + 16 > h) y0 = h > 16 ? h - 16 : 0;
    uint32_t sum = 0, n = 0;
    for (uint32_t yy = y0; yy < y0 + 16 && yy < h; yy += 2)
        for (uint32_t xx = x0; xx < x0 + 16 && xx < w; xx += 2) {
            const uint8_t *q = p + (size_t)yy * stride + (size_t)xx * 4;     /* B G R X */
            sum += (77u * q[2] + 150u * q[1] + 29u * q[0]) >> 8;
            n++;
        }
    return n ? (int)(sum / n) : 0;
}

static int dump_ppm(const char *path, const uint8_t *p, uint32_t w, uint32_t h, uint32_t stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = p + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {            /* XRGB8888 little endian: B G R X */
            row[x * 3 + 0] = s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + 0];
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "10.0.0.1", *dump = NULL;
    int with_hash = 0, tearing = 0, watch = 0, watch_cursor = 0, want_cursor = 1;
    uint32_t watch_x = 0, watch_y = 0;
    int port = 18650, mtu = 4096;
    uint32_t ns = 3, w = 1920, h = 1080, hz = 60;
    double seconds = 0, lead_us = 2000;
    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'}, {"slots", 1, 0, 's'},
        {"mode", 1, 0, 'M'}, {"flip-lead-us", 1, 0, 'l'}, {"seconds", 1, 0, 'S'}, {"dump", 1, 0, 'D'},
        {"hash", 0, 0, 'H'}, {"tearing", 0, 0, 'T'}, {"watch", 1, 0, 'W'}, {"watch-cursor", 0, 0, 'K'},
        {"cursor", 1, 0, 'C'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 's': ns = (uint32_t)atoi(optarg); break;
        case 'M': {
            char res[32] = {0};
            const char *at = strchr(optarg, '@');
            snprintf(res, sizeof(res), "%.*s", at ? (int)(at - optarg) : (int)strlen(optarg), optarg);
            if (at) hz = (uint32_t)atoi(at + 1);
            if (rdx_parse_res(res, &w, &h) || !hz) { fprintf(stderr, "invalid --mode\n"); return 2; }
        } break;
        case 'l': lead_us = atof(optarg); break;
        case 'S': seconds = atof(optarg); break;
        case 'D': dump = optarg; break;
        case 'H': with_hash = 1; break;
        case 'T': tearing = 1; break;
        case 'W':
            if (sscanf(optarg, "%u,%u", &watch_x, &watch_y) != 2) { fprintf(stderr, "--watch X,Y\n"); return 2; }
            watch = 1;
            break;
        case 'K': watch_cursor = 1; break;
        case 'C':
            if (!strcmp(optarg, "picture")) want_cursor = 0;
            else if (strcmp(optarg, "auto")) { fprintf(stderr, "--cursor auto|picture\n"); return 2; }
            break;
        default:  fprintf(stderr, "see the header of rdx_sink_null.c for options\n"); return 2;
        }
    }
    if (!ns || ns > RDX_MAX_SLOTS) { fprintf(stderr, "--slots 1..%d\n", RDX_MAX_SLOTS); return 2; }
    if (watch && (watch_x >= w || watch_y >= h)) { fprintf(stderr, "--watch outside the frame\n"); return 2; }
    if (watch || watch_cursor) setvbuf(stdout, NULL, _IOLBF, 0);   /* the [watch] lines are read while the sink runs */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    uint32_t stride = (w * 4 + 255) & ~255u;
    uint64_t hdr_off = ((uint64_t)stride * h + 4095) & ~4095ull;
    uint64_t slot_bytes = (hdr_off + sizeof(struct rdx_frame_hdr) + 4095) & ~4095ull;
    uint64_t period = (uint64_t)(1e9 / hz), lead = (uint64_t)(lead_us * 1e3);
    uint8_t *map = mmap(NULL, slot_bytes * ns, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    memset(map, 0, slot_bytes * ns);

    struct ibv_context *ctx = rdx_open_dev_ip(dev, bind_ip);   /* dev "auto": the device carrying bind_ip */
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_mr *mr = pd ? ibv_reg_mr(pd, map, slot_bytes * ns, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE) : NULL;
    struct rdx_cq rcq;
    if (!mr || rdx_cq_create_ts(ctx, 256, -1, &rcq)) { perror("verbs setup"); return 1; }
    struct ibv_qp *qp = rdx_create_qp(pd, rcq.cq, 64);
    if (!qp || rdx_qp_to_init(qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(qp, WR_RECV)) { perror("post_recv"); return 1; }
    struct rdx_cur_rx cr;                        /* the host's cursor next to the picture */
    memset(&cr, 0, sizeof(cr));
    int cur_offer = want_cursor && !rdx_cur_rx_init(&cr, pd, rcq.cq);
    struct rdx_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_MAGIC;
    hello.width = w; hello.height = h; hello.stride = stride; hello.fourcc = 0x34325258; /* DRM_FORMAT_XRGB8888 */
    hello.nslots = ns; hello.slot_bytes = slot_bytes; hello.hdr_off = hdr_off;
    hello.base_addr = (uintptr_t)map; hello.rkey = mr->rkey;
    int gidx = rdx_find_gid_index_ip(ctx, bind_ip, hello.conn.gid);   /* the GID of the seat's link IP */
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    hello.conn.qpn = qp->qp_num;
    hello.conn.psn = 0x1234;
    hello.vblank_ns = period;
    if (cur_offer) hello.caps |= RDX_CAP_CURSOR;
    printf("[null] %ux%u@%u, %u slots, waiting on %s:%d\n", w, h, hz, ns, bind_ip, port);
    fflush(stdout);
    int sock = rdx_tcp_listen_accept(bind_ip, port);
    if (sock < 0) return 1;
    struct rdx_conn peer;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) || rdx_io_all(sock, &peer, sizeof(peer), 0)) return 1;
    int cur_up = rdx_cur_rx_connect(&cr, sock, &peer, gidx, hello.conn.gid, rdx_parse_mtu(mtu));
    if (cur_up < 0) return 1;
    if (rdx_qp_to_rtr_rts(qp, &peer, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    printf("[null] connected, cursor %s\n", cur_up ? "next to the picture" : "in the picture");
    fflush(stdout);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    uint64_t t0 = rdx_now_ns(), next_vb = t0 + period;
    int at_deadline = 1;                         /* next timer event: flip deadline, then the vblank */
    arm_at(tfd, next_vb - lead);
    int on_screen = -1, pending = -1, ready = -1;
    uint64_t t_cqe[RDX_MAX_SLOTS] = {0}, t_hw[RDX_MAX_SLOTS] = {0};
    int slot_bright[RDX_MAX_SLOTS] = {0}, last_bright = -1, shown_bright = -1;   /* --watch states */
    uint64_t last_hash = 0;                      /* --watch-cursor: the sampled picture of the last frame */
    int cur_pending_shown = 0;                   /* --watch-cursor: a cursor position waits for its vblank */
    long cur_pos = 0, cur_img = 0;
    uint32_t slot_seq[RDX_MAX_SLOTS] = {0}, last_seq = 0xFFFFFFFFu;
    long nf = 0, nshown = 0, dropped = 0, bad_hdr = 0, bad_order = 0, vblanks = 0;
    double *lat = calloc(1000000, sizeof(double)), *lat_hw = calloc(1000000, sizeof(double));
    long nlat_hw = 0;
    uint64_t t0_cpu = rdx_cpu_ns();
    int running = 1;
    while (running && !g_stop && (seconds <= 0 || rdx_now_ns() - t0 < (uint64_t)(seconds * 1e9))) {
        struct rdx_wc wc[16];
        int nwc = rdx_cq_poll(&rcq, wc, 16);
        if (nwc == 0) {
            ibv_req_notify_cq(rcq.cq, 0);
            nwc = rdx_cq_poll(&rcq, wc, 16);
        }
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }
        for (int k = 0; k < nwc; k++) {
            if (rdx_cur_wr(wc[k].wr_id)) {           /* the cursor channel: never fatal for the frames */
                int ch = rdx_cur_rx_complete(&cr, wc[k].wr_id, wc[k].status == IBV_WC_SUCCESS);
                if (ch & RDX_CUR_CH_IMAGE) cur_img++;
                if (ch > 0 && (ch & RDX_CUR_CH_POS)) {
                    cur_pos++;
                    if (watch_cursor) {
                        printf("[watch] cursor x=%d y=%d visible=%d sw_ns=%llu hw_ns=%llu\n", cr.st->x, cr.st->y,
                               cr.st->visible, (unsigned long long)rdx_now_ns(), (unsigned long long)wc[k].hw_ns);
                        cur_pending_shown = 1;
                    }
                }
                continue;
            }
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s\n", ibv_wc_status_str(wc[k].status)); return 1;
            }
            if (wc[k].opcode == IBV_WC_RECV) {
                if (ntohl(wc[k].imm_data) == RDX_IMM_END) running = 0;
                continue;
            }
            if (wc[k].opcode != IBV_WC_RECV_RDMA_WITH_IMM) continue;
            uint32_t imm = ntohl(wc[k].imm_data), slot = rdx_imm_slot(imm);
            if (rdx_post_zero_recv(qp, WR_RECV)) { perror("repost"); return 1; }
            if (slot >= ns) { fprintf(stderr, "slot %u out of range\n", slot); return 1; }
            struct rdx_frame_hdr hdr;
            static uint32_t last_flags = 0;
            memcpy(&hdr, map + (size_t)slot * slot_bytes + hdr_off, sizeof(hdr));
            if ((hdr.magic != RDX_MAGIC_LIVE && hdr.magic != RDX_MAGIC) || rdx_seq28(hdr.seq) != rdx_imm_seq(imm)) bad_hdr++;
            if (last_seq != 0xFFFFFFFFu && hdr.seq != last_seq + 1) bad_order++;
            last_seq = hdr.seq;
            if (hdr.flags != last_flags) {      /* the source's hints (frame header, RDX_HDR_F_*) */
                printf("[null] frame %u: source hints 0x%x%s\n", hdr.seq, hdr.flags,
                       !(hdr.flags & RDX_HDR_F_HINTS) ? " (none)" : (hdr.flags & RDX_HDR_F_REL_MOUSE)
                       ? " (relative mouse: the app locks the pointer)" : " (absolute mouse)");
                fflush(stdout);
                last_flags = hdr.flags;
            }
            slot_seq[slot] = hdr.seq;
            t_cqe[slot] = rdx_now_ns();         /* software time like rdx_sink_kms: the vblank is software time too */
            t_hw[slot] = wc[k].hw_ns;           /* NIC completion time on CLOCK_MONOTONIC, 0 without timestamps */
            nf++;
            if (watch) {
                int luma = box_luma(map + (size_t)slot * slot_bytes, w, h, stride, watch_x, watch_y);
                int bright = luma >= 128;
                slot_bright[slot] = bright;
                if (bright != last_bright)
                    printf("[watch] arrive seq=%u bright=%d luma=%d sw_ns=%llu hw_ns=%llu\n", hdr.seq, bright, luma,
                           (unsigned long long)t_cqe[slot], (unsigned long long)t_hw[slot]);
                last_bright = bright;
            }
            if (watch_cursor) {                  /* a changed picture: the cursor drawn into it moved */
                uint64_t hs = sample_hash(map + (size_t)slot * slot_bytes, w, h, stride);
                if (!last_hash || hs != last_hash) {     /* the first frame, then every change */
                    slot_bright[slot] = !last_bright;
                    printf("[watch] arrive seq=%u bright=%d luma=0 sw_ns=%llu hw_ns=%llu\n", hdr.seq, slot_bright[slot],
                           (unsigned long long)t_cqe[slot], (unsigned long long)t_hw[slot]);
                    last_bright = slot_bright[slot];
                } else slot_bright[slot] = last_bright;
                last_hash = hs;
            }
            if (with_hash)   /* what the NIC delivered, to compare with rdx_gs_src --verify */
                printf("[hash] %u %016llx\n", hdr.seq,
                       (unsigned long long)rdx_frame_hash(map + (size_t)slot * slot_bytes, w, h, stride));
            if (tearing) {          /* shown on arrival (scan-out of the buffer the NIC writes): credit now */
                if (rdx_post_imm_send(qp, rdx_credit_imm(slot, 0), WR_CREDIT)) return 1;
                on_screen = (int)slot;
                nshown++;
                if ((watch || watch_cursor) && slot_bright[slot] != shown_bright) {
                    printf("[watch] shown seq=%u bright=%d vblank_ns=%llu\n", hdr.seq, slot_bright[slot],
                           (unsigned long long)t_cqe[slot]);
                    shown_bright = slot_bright[slot];
                }
                continue;
            }
            if (ready >= 0) {
                if (rdx_post_imm_send(qp, rdx_credit_untimed((uint32_t)ready, RDX_US_DROP), WR_CREDIT)) return 1;
                dropped++;
            }
            ready = (int)slot;
        }
        if (nwc > 0) continue;

        struct pollfd p[2] = {{.fd = rcq.ch->fd, .events = POLLIN}, {.fd = tfd, .events = POLLIN}};
        if (poll(p, 2, 500) < 0) continue;
        if (p[0].revents & POLLIN) {
            struct ibv_cq *ecq; void *ectx;
            while (ibv_get_cq_event(rcq.ch, &ecq, &ectx) == 0) ibv_ack_cq_events(ecq, 1);
        }
        if (!(p[1].revents & POLLIN)) continue;
        uint64_t exp;
        if (read(tfd, &exp, sizeof(exp)) < 0) continue;
        if (at_deadline) {                       /* latch the newest ready frame for this vblank */
            if (pending < 0 && ready >= 0) { pending = ready; ready = -1; }
            at_deadline = 0;
            arm_at(tfd, next_vb);
        } else {                                 /* the vblank */
            vblanks++;
            if ((vblanks & 63) == 0) rdx_cq_clock_sync(&rcq);     /* NIC clock -> CLOCK_MONOTONIC slope */
            if (cur_pending_shown) {                 /* a cursor plane is latched at the vblank */
                printf("[watch] cursor-shown vblank_ns=%llu\n", (unsigned long long)next_vb);
                cur_pending_shown = 0;
            }
            if (pending >= 0) {
                int prev = on_screen, cur = pending;
                on_screen = cur; pending = -1;
                uint64_t c2v = next_vb > t_cqe[cur] ? next_vb - t_cqe[cur] : 0;
                if (nshown < 1000000) lat[nshown] = c2v / 1e6;
                if (t_hw[cur] && next_vb > t_hw[cur] && nlat_hw < 1000000) lat_hw[nlat_hw++] = (next_vb - t_hw[cur]) / 1e6;
                nshown++;
                if ((watch || watch_cursor) && slot_bright[cur] != shown_bright) {
                    printf("[watch] shown seq=%u bright=%d vblank_ns=%llu\n", slot_seq[cur], slot_bright[cur],
                           (unsigned long long)next_vb);
                    shown_bright = slot_bright[cur];
                }
                if (rdx_post_imm_send(qp, rdx_shown_imm((uint32_t)cur, c2v), WR_CREDIT)) return 1;
                if (prev >= 0 && rdx_post_imm_send(qp, rdx_credit_untimed((uint32_t)prev, RDX_US_NONE), WR_CREDIT)) return 1;
            }
            next_vb += period;
            uint64_t now = rdx_now_ns();
            while (next_vb - lead <= now) next_vb += period;      /* a late wake-up skips vblanks, like a display */
            at_deadline = 1;
            arm_at(tfd, next_vb - lead);
        }
    }
    double el = (rdx_now_ns() - t0) / 1e9;
    printf("[null] %.1f s: received %ld (%.1f/s), shown %ld (%.1f/s), dropped %ld, vblanks %ld\n", el, nf, nf / el,
           nshown, nshown / el, dropped, vblanks);
    printf("[null] errors: header=%ld order=%ld\n", bad_hdr, bad_order);
    if (cur_up)
        printf("[null] cursor next to the picture: %ld position changes, %ld pictures (%ld invalid messages)%s\n", cur_pos,
               cur_img, cr.st->n_bad, cr.failed ? ", the channel failed" : "");
    double mbps = el > 0 ? (double)nf * stride * h / el / 1e6 : 0;
    printf("[null] payload: %.1f MB/s = %.2f Gbit/s (%ld frames of %u x %u B)\n", mbps, mbps * 8 / 1e3, nf, stride, h);
    rdx_report("CQE -> vblank (ms)", lat, (size_t)(nshown < 1000000 ? nshown : 1000000));
    if (nlat_hw) rdx_report("NIC CQE -> vblank (ms)", lat_hw, (size_t)nlat_hw);
    rdx_cpu_report("null-sink", t0, t0_cpu);
    if (dump && on_screen >= 0) {
        const uint8_t *s = map + (size_t)on_screen * slot_bytes;
        long nonblack = 0;
        for (uint32_t y = 0; y < h; y += 4)
            for (uint32_t x = 0; x < w; x += 4)
                if (*(const uint32_t *)(s + (size_t)y * stride + (size_t)x * 4) & 0x00FFFFFFu) nonblack++;
        printf("[null] last shown frame seq %u: %.1f %% non-black (sampled), written to %s\n", slot_seq[on_screen],
               100.0 * nonblack / ((w / 4) * (h / 4)), dump);
        dump_ppm(dump, s, w, h, stride);
    }
    close(sock);
    return bad_hdr ? 3 : 0;
}
