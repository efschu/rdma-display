/* SPDX-License-Identifier: MIT
 *
 * P0-B source: writes a test pattern from host RAM into the sink's slots with
 * RDMA_WRITE_WITH_IMM. Slots are handed out by credits that the sink returns
 * with SEND_WITH_IMM. Latency is measured with the source clock only:
 * post -> credit/"shown" report, minus the sink time carried in the immediate.
 *
 * vblank lock (default on, --no-lock): a KMS sink reports "shown" at every
 * flip, so these reports arrive at the sink's refresh rhythm, seen through our
 * own clock. From them the source estimates period and phase of the remote
 * vblank and posts each frame `lead` before the next one, with
 * lead = network time (EWMA) + margin. Without the lock the source runs on its
 * own clock and lands at a fixed, arbitrary phase (up to a full refresh late).
 *
 *   rdx_src [--dev rocep1s0f0] [--peer 127.0.0.1] [--port 18650]
 *           [--frames 600] [--fps 0] [--mtu 4096]
 *           [--no-lock] [--lock-margin-ms 1.5] [--busy-poll] [--pm-qos-us 50]
 *
 * Waiting is event driven (CQ completion channel, PM QoS against deep idle
 * states); only the last 50 us before a post deadline spin. --busy-poll spins.
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"

#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WR_SRC_BASE  0x100u          /* wr_id of frame writes: base + source buffer */
#define WR_RECV      0x200u
#define WR_END       0x300u
#define NRECV        32
#define CRING        (RDX_MAX_SLOTS * 4)

int main(int argc, char **argv)
{
    const char *dev = "rocep1s0f0", *peer_ip = "127.0.0.1";
    int port = 18650, mtu = RDX_DEFAULT_MTU;
    long frames = 600;
    double fps = 0, lock_margin_ms = 1.5;
    int busy = 0, pm_qos_us = 50;
    int lock = 1;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'},
        {"frames", 1, 0, 'n'}, {"fps", 1, 0, 'f'}, {"mtu", 1, 0, 'm'},
        {"no-lock", 0, 0, 'L'}, {"lock-margin-ms", 1, 0, 'g'}, {"busy-poll", 0, 0, 'B'},
        {"pm-qos-us", 1, 0, 'Q'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'p': peer_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'n': frames = atol(optarg); break;
        case 'f': fps = atof(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'L': lock = 0; break;
        case 'g': lock_margin_ms = atof(optarg); break;
        case 'B': busy = 1; break;
        case 'Q': pm_qos_us = atoi(optarg); break;
        default:  fprintf(stderr, "see the header of rdx_src.c for options\n"); return 2;
        }
    }

    int sock = rdx_tcp_connect(peer_ip, port);
    if (sock < 0) return 1;
    struct rdx_hello hello;
    if (rdx_io_all(sock, &hello, sizeof(hello), 0) || hello.magic != RDX_MAGIC) {
        fprintf(stderr, "bootstrap: invalid hello\n"); return 1;
    }
    uint32_t w = hello.width, h = hello.height, stride = hello.stride, ns = hello.nslots;
    if (!ns || ns > RDX_MAX_SLOTS) { fprintf(stderr, "invalid nslots=%u\n", ns); return 1; }
    uint64_t wr_len = hello.hdr_off + sizeof(struct rdx_frame_hdr);
    printf("[i] sink: %ux%u stride=%u slots=%u slot_bytes=%llu write=%llu B/frame\n",
           w, h, stride, ns, (unsigned long long)hello.slot_bytes, (unsigned long long)wr_len);

    /* one source buffer per slot, so no buffer is repainted while the NIC reads it */
    uint8_t *src = NULL;
    if (posix_memalign((void **)&src, 4096, (size_t)hello.slot_bytes * ns)) { perror("alloc"); return 1; }
    int64_t prev_bar[RDX_MAX_SLOTS];
    for (uint32_t i = 0; i < ns; i++) {
        rdx_fill_base(src + (size_t)i * hello.slot_bytes, w, h, stride);
        prev_bar[i] = -1;
    }

    char lip[64] = "";
    rdx_local_ip_for(peer_ip, lip, sizeof(lip));
    struct ibv_context *ctx = rdx_open_dev_ip(dev, lip);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_mr *mr = ibv_reg_mr(pd, src, (size_t)hello.slot_bytes * ns, IBV_ACCESS_LOCAL_WRITE);
    if (!pd || !mr) { perror("pd/mr"); return 1; }
    struct ibv_comp_channel *ch = NULL;
    struct ibv_cq *cq = rdx_create_cq_ev(ctx, 256, &ch);
    struct ibv_qp *qp = cq ? rdx_create_qp(pd, cq, 64) : NULL;
    if (!qp || rdx_qp_to_init(qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(qp, WR_RECV)) { perror("post_recv"); return 1; }

    struct rdx_conn mine;
    memset(&mine, 0, sizeof(mine));
    int gidx = rdx_find_gid_index_ip(ctx, lip, mine.gid);
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    mine.qpn = qp->qp_num;
    mine.psn = 0x2345;
    printf("[i] %s sgid_index=%d (%s) ::ffff:%u.%u.%u.%u -> sink ::ffff:%u.%u.%u.%u\n",
           dev, gidx, rdx_gid_is_v2(dev, gidx) ? "RoCE v2" : "RoCE v1",
           mine.gid[12], mine.gid[13], mine.gid[14], mine.gid[15],
           hello.conn.gid[12], hello.conn.gid[13], hello.conn.gid[14], hello.conn.gid[15]);
    if (rdx_io_all(sock, &mine, sizeof(mine), 1)) return 1;
    if (rdx_qp_to_rtr_rts(qp, &hello.conn, gidx, mine.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }

    /* free sink slots (credits) as a ring */
    uint32_t credit[CRING], c_head = 0, c_tail = 0;
    for (uint32_t i = 0; i < ns; i++) credit[c_tail++ % CRING] = i;
    int src_busy[RDX_MAX_SLOTS] = {0};
    long inflight = 0, credits_back = 0;
    /* per-frame round trip with our own clock: post -> credit / "shown" */
    uint64_t slot_t_post[RDX_MAX_SLOTS] = {0};
    double *rtt = calloc((size_t)frames, sizeof(double));
    double *proc = calloc((size_t)frames, sizeof(double));
    double *xfer = calloc((size_t)frames, sizeof(double));
    long nrtt = 0, nshown = 0;
    /* vblank lock state, all in our own clock */
    uint64_t t_shown_last = 0;
    struct rdx_lock vlock = {0, 0};
    /* the KMS sink reports its exact refresh period; estimate only without it */
    int fixed_period = hello.vblank_ns != 0;
    double period_est = fixed_period ? (double)hello.vblank_ns : (fps > 0 ? 1e9 / fps : 0), net_ewma = 0;
    long nlocked = 0;
    /* posted frames whose "shown"/"dropped" report is still outstanding; each will
     * occupy one remote vblank, so the next frame must aim behind them */
    int unshown[RDX_MAX_SLOTS] = {0};

    uint64_t period_ns = fps > 0 ? (uint64_t)(1e9 / fps) : 0;
    uint64_t t0 = rdx_now_ns(), t_last_done = t0, t_print = t0;
    long done = 0, done_print = 0;
    struct ibv_wc wc[16];

#define POLL_ONCE() do {                                                          \
        int nwc = ibv_poll_cq(cq, 16, wc);                                        \
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }           \
        for (int k = 0; k < nwc; k++) {                                           \
            if (wc[k].status != IBV_WC_SUCCESS) {                                 \
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n",                  \
                        ibv_wc_status_str(wc[k].status),                          \
                        (unsigned long long)wc[k].wr_id);                         \
                return 1;                                                         \
            }                                                                     \
            if (wc[k].opcode == IBV_WC_RECV) {                                    \
                uint64_t t_cr = rdx_now_ns();                                     \
                uint32_t cimm = ntohl(wc[k].imm_data);                            \
                uint32_t cslot = rdx_imm_slot(cimm);                              \
                int shown = (cimm & RDX_IMM_SHOWN) != 0;                          \
                if (!shown) {                                                     \
                    credit[c_tail++ % CRING] = cslot;                             \
                    credits_back++;                                               \
                }                                                                 \
                if (cslot < ns && (shown || rdx_credit_us(cimm) == RDX_US_DROP))  \
                    unshown[cslot] = 0;                                           \
                if (rdx_credit_us(cimm) < RDX_US_DROP && nrtt < frames &&         \
                    cslot < ns && slot_t_post[cslot]) {                           \
                    if (shown) nshown++;                                          \
                    rtt[nrtt]  = (t_cr - slot_t_post[cslot]) / 1e6;               \
                    proc[nrtt] = rdx_credit_us(cimm) / 1e3;                       \
                    xfer[nrtt] = rtt[nrtt] - proc[nrtt];                          \
                    if (shown) net_ewma = net_ewma > 0 ?                          \
                        0.9 * net_ewma + 0.1 * xfer[nrtt] * 1e6 : xfer[nrtt] * 1e6; \
                    nrtt++;                                                       \
                }                                                                 \
                if (shown) {                                                      \
                    if (t_shown_last && !fixed_period) {                          \
                        double gap = (double)(t_cr - t_shown_last);               \
                        if (period_est <= 0 && gap > 4e6 && gap < 50e6)           \
                            period_est = gap;                                     \
                        else if (gap > 0.5 * period_est && gap < 1.5 * period_est) \
                            period_est = 0.95 * period_est + 0.05 * gap;          \
                    }                                                             \
                    t_shown_last = t_cr;                                          \
                }                                                                 \
                if (rdx_post_zero_recv(qp, WR_RECV)) { perror("repost"); return 1; } \
            } else if (wc[k].wr_id >= WR_SRC_BASE && wc[k].wr_id < WR_RECV) {     \
                src_busy[wc[k].wr_id - WR_SRC_BASE] = 0;                          \
                inflight--; done++; t_last_done = rdx_now_ns();                   \
            }                                                                     \
        }                                                                         \
    } while (0)
    int no_stop = 0;
#define WAIT_UNTIL(cond, deadline) RDX_WAIT_UNTIL(cond, deadline, POLL_ONCE(), cq, ch, busy, no_stop)
    int qos_fd = rdx_pm_qos(pm_qos_us);
    uint64_t t0_cpu = rdx_cpu_ns();

    for (long seq = 0; seq < frames; seq++) {
        if (lock && t_shown_last && period_est > 0 && net_ewma > 0) {
            /* aim at the next free remote vblank, arrive `margin` early */
            int ahead = 0;
            for (uint32_t i = 0; i < ns; i++) ahead += unshown[i];
            double interval = (double)period_ns > period_est ? (double)period_ns : period_est;
            uint64_t due = rdx_lock_due(t_shown_last, period_est, interval, ahead, &vlock,
                                        net_ewma + lock_margin_ms * 1e6, rdx_now_ns());
            WAIT_UNTIL(rdx_now_ns() >= due, due);
            nlocked++;
        } else if (period_ns) {
            uint64_t due = t0 + (uint64_t)seq * period_ns;
            WAIT_UNTIL(rdx_now_ns() >= due, due);
        }
        uint32_t sb = (uint32_t)(seq % ns);
        WAIT_UNTIL(!(c_head == c_tail || src_busy[sb]), 0);
        uint32_t slot = credit[c_head++ % CRING];

        uint8_t *buf = src + (size_t)sb * hello.slot_bytes;
        rdx_paint_frame(buf, w, h, stride, (uint32_t)seq, &prev_bar[sb]);
        struct rdx_frame_hdr *hdr = (struct rdx_frame_hdr *)(buf + hello.hdr_off);
        hdr->magic = RDX_MAGIC;
        hdr->seq = (uint32_t)seq;
        hdr->flags = 0;                  /* no hints from a test pattern */
        hdr->pad = 0;
        hdr->t_post_ns = rdx_now_ns();
        slot_t_post[slot] = hdr->t_post_ns;

        struct ibv_sge sge = {.addr = (uintptr_t)buf, .length = (uint32_t)wr_len, .lkey = mr->lkey};
        struct ibv_send_wr wr, *bad;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = WR_SRC_BASE + sb;
        wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        wr.sg_list = &sge; wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.imm_data = htonl(rdx_imm((uint32_t)seq, slot));
        wr.wr.rdma.remote_addr = hello.base_addr + (uint64_t)slot * hello.slot_bytes;
        wr.wr.rdma.rkey = hello.rkey;
        if (ibv_post_send(qp, &wr, &bad)) { perror("post_send"); return 1; }
        src_busy[sb] = 1;
        unshown[slot] = 1;
        inflight++;

        uint64_t now = rdx_now_ns();
        if (now - t_print >= 1000000000ull) {
            double dt = (now - t_print) / 1e9;
            printf("[src] %6ld frames  %7.1f fps  %6.2f GB/s\n", done,
                   (done - done_print) / dt, (done - done_print) * (double)wr_len / dt / 1e9);
            fflush(stdout);
            t_print = now; done_print = done;
        }
    }
    WAIT_UNTIL(inflight == 0, 0);
    double el = (t_last_done - t0) / 1e9;
    /* wait until the sink has returned every slot, then send END.
     * A KMS sink keeps the slot that is on screen: stop after 300 ms. */
    uint64_t t_end = rdx_now_ns() + 300000000ull;
    WAIT_UNTIL(credits_back >= frames, t_end);
    if (rdx_post_imm_send(qp, RDX_IMM_END, WR_END)) { perror("send END"); return 1; }
    for (int got = 0; !got;) {
        int nwc = ibv_poll_cq(cq, 16, wc);
        for (int k = 0; k < nwc; k++) if (wc[k].wr_id == WR_END) got = 1;
    }

    printf("[src] done: %ld frames of %llu B in %.3f s = %.1f fps, %.3f GB/s (MTU %d, fps target %.1f, 0=free)\n",
           done, (unsigned long long)wr_len, el, done / el, done * (double)wr_len / el / 1e9, mtu, fps);
    printf("[src] latency with own clock (~1 us return path not subtracted), %s:\n",
           nshown ? "sink reports SHOWN (vblank)" : "sink reports slot free");
    rdx_report("post -> report (round)", rtt, (size_t)nrtt);
    rdx_report(nshown ? "sink CQE -> vblank" : "sink CQE -> free", proc, (size_t)nrtt);
    rdx_report("round - sink = network", xfer, (size_t)nrtt);
    if (nlocked)
        printf("[src] vblank lock: %ld of %ld frames locked, period %.3f ms, network EWMA %.3f ms, "
               "margin %.2f ms\n", nlocked, done, period_est / 1e6, net_ewma / 1e6, lock_margin_ms);
    else
        printf("[src] vblank lock: %s\n", lock ? "never engaged (sink sends no SHOWN reports)" : "off");
    rdx_cpu_report("src", t0, t0_cpu);
    if (qos_fd >= 0) close(qos_fd);
    close(sock);
    return 0;
}
