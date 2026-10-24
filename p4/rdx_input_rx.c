/* SPDX-License-Identifier: MIT
 *
 * P4 input receiver: runs on the host that runs the program (A). Accepts one
 * rdx_input_tx session, creates one uinput device per remote device that
 * mirrors its capabilities (name prefixed "rdx: "), and injects every received
 * report with a single write(). Each report is acknowledged with
 * SEND_WITH_IMM so the sender can measure the round trip with its own clock.
 *
 * Waiting (default): arm the CQ, poll it once more (closes the race), then
 * block on the completion channel; no spinning, no fixed tick. --busy-poll
 * spins instead. --spin-us N keeps spinning N us after each completion before
 * blocking (hybrid). PM QoS (--pm-qos-us, default 50, 0 = off) holds
 * /dev/cpu_dma_latency while the program runs. --rt-prio (default 10, 0 = off)
 * runs the loop SCHED_FIFO so a wake-up is not queued behind desktop load.
 *
 * --private: the mirrors are for one gamescope only (rdx-hostd passes it for gamescope apps): phys
 * RDX_INPUT_PRIVATE_PHYS, i.e. on the seat RDX_INPUT_PRIVATE_SEAT, which no desktop reads
 * (desktop/72-rdma-display-input.rules). Without it (a mirrored desktop) they are seat0 devices.
 *
 * --wait-grab: the consumer grabs the devices (gamescope, rdx-hostd passes it for gamescope apps);
 * key events of a keyboard mirror are held back until it has (common/rdx_input.h, grab gate):
 * otherwise the host's console keyboard handler would see them meanwhile.
 *
 * For the probe device ("rdx-probe", MSC_SCAN only, ignored by desktops) the
 * receiver reads the injected event back from the mirror's evdev node and puts
 * the time uinput write -> event readable (A clock, us) into the ack.
 *
 *   rdx_input_rx [--dev auto] [--bind 10.0.0.2] [--port 18660]
 *                [--mtu 4096] [--busy-poll] [--spin-us 0] [--pm-qos-us 50] [--rt-prio 10]
 *                [--comp-vector -1] [--private] [--wait-grab]
 */
#define _GNU_SOURCE
#include "../common/rdx_input.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/uinput.h>

#define WR_RECV_BASE 0x1000u        /* wr_id of receives: base + ring index */
#define WR_ACK       0x200u
#define NRECV        64
#define READBACK_TIMEOUT_NS 5000000ull

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int post_recv(struct ibv_qp *qp, struct ibv_mr *mr, struct rdx_input_pkt *ring, uint32_t i)
{
    struct ibv_sge sge = {.addr = (uintptr_t)&ring[i], .length = sizeof(ring[i]), .lkey = mr->lkey};
    struct ibv_recv_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = WR_RECV_BASE + i;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    return ibv_post_recv(qp, &wr, &bad);
}

/* read the mirror's evdev until the MSC_SCAN probe value shows up; returns 0 when seen */
static int wait_probe(int efd, int32_t want, uint64_t deadline)
{
    struct input_event ev[32];
    while (rdx_now_ns() < deadline) {
        ssize_t r = read(efd, ev, sizeof(ev));
        if (r <= 0) continue;
        for (size_t k = 0; k < (size_t)r / sizeof(ev[0]); k++)
            if (ev[k].type == EV_MSC && ev[k].code == MSC_SCAN && ev[k].value == want) return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "10.0.0.2";
    int port = 18660, mtu = 4096, busy = 0, spin_us = 0, pm_qos_us = 50, rt_prio = 10, comp_vector = -1, priv = 0, wait_grab = 0;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"busy-poll", 0, 0, 'B'}, {"spin-us", 1, 0, 's'}, {"pm-qos-us", 1, 0, 'q'},
        {"rt-prio", 1, 0, 'R'}, {"comp-vector", 1, 0, 'V'}, {"private", 0, 0, 'p'}, {"wait-grab", 0, 0, 'g'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'B': busy = 1; break;
        case 's': spin_us = atoi(optarg); break;
        case 'q': pm_qos_us = atoi(optarg); break;
        case 'R': rt_prio = atoi(optarg); break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'p': priv = 1; break;
        case 'g': wait_grab = 1; break;
        default:  fprintf(stderr, "see the header of rdx_input_rx.c for options\n"); return 2;
        }
    }
    rdx_stop_signals(on_signal);

    struct ibv_context *ctx = rdx_open_dev_ip(dev, bind_ip);   /* dev "auto": the device carrying bind_ip */
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    /* completion channel: without busy poll the receiver sleeps in poll() on it */
    struct ibv_comp_channel *ch = pd ? ibv_create_comp_channel(ctx) : NULL;
    if (ch) fcntl(ch->fd, F_SETFL, fcntl(ch->fd, F_GETFL) | O_NONBLOCK);
    comp_vector = rdx_pick_comp_vector(ctx, comp_vector);
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 512, NULL, ch, comp_vector) : NULL;
    struct ibv_qp *qp = cq ? rdx_create_qp(pd, cq, 256) : NULL;
    if (!qp || rdx_qp_to_init(qp)) return 1;
    struct rdx_input_pkt *ring = calloc(NRECV, sizeof(*ring));
    struct ibv_mr *mr = ibv_reg_mr(pd, ring, NRECV * sizeof(*ring), IBV_ACCESS_LOCAL_WRITE);
    if (!mr) { perror("ibv_reg_mr"); return 1; }
    for (uint32_t i = 0; i < NRECV; i++)
        if (post_recv(qp, mr, ring, i)) { perror("post_recv"); return 1; }

    int sock = rdx_tcp_listen_accept_as(bind_ip, port, "rx");
    if (sock < 0) return 1;
    struct rdx_input_hello hello;
    if (rdx_io_all(sock, &hello, sizeof(hello), 0) || hello.magic != RDX_INPUT_MAGIC ||
        !hello.ndev || hello.ndev > RDX_INPUT_MAX_DEV) {
        fprintf(stderr, "bootstrap: invalid hello\n"); return 1;
    }
    struct rdx_input_dev desc[RDX_INPUT_MAX_DEV];
    if (rdx_io_all(sock, desc, sizeof(desc[0]) * hello.ndev, 0)) { fprintf(stderr, "bootstrap: devices\n"); return 1; }

    /* mirror devices; the probe mirror is also opened for read-back */
    int ufd[RDX_INPUT_MAX_DEV], efd[RDX_INPUT_MAX_DEV];
    struct rdx_grab_gate gate[RDX_INPUT_MAX_DEV];
    for (uint32_t i = 0; i < hello.ndev; i++) {
        desc[i].name[sizeof(desc[i].name) - 1] = 0;
        ufd[i] = rdx_uinput_create_phys(&desc[i], "rdx: ", priv ? RDX_INPUT_PRIVATE_PHYS : NULL);
        efd[i] = -1;
        gate[i].efd = -1;
        gate[i].held = 0;
        if (ufd[i] < 0) return 1;
        char path[64] = "";
        if (wait_grab && strcmp(desc[i].name, RDX_PROBE_NAME) && !rdx_uinput_event_path(ufd[i], path, sizeof(path))) {
            char what[96];
            snprintf(what, sizeof(what), "rdx: %.80s", desc[i].name);
            rdx_grab_gate_init(&gate[i], path, what);
        }
        if (!strcmp(desc[i].name, RDX_PROBE_NAME)) {
            if (rdx_uinput_event_path(ufd[i], path, sizeof(path))) return 1;
            efd[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (efd[i] < 0) { perror(path); return 1; }
            int clk = CLOCK_MONOTONIC;
            ioctl(efd[i], EVIOCSCLOCKID, &clk);
        }
        if (efd[i] >= 0) printf("[rx] mirror %u: \"rdx: %s\" (probe, read-back %s)\n", i, desc[i].name, path);
        else printf("[rx] mirror %u: \"rdx: %s\"%s\n", i, desc[i].name,
                    priv ? " (gamescope only: seat " RDX_INPUT_PRIVATE_SEAT ")" : "");
    }

    struct rdx_conn mine;
    memset(&mine, 0, sizeof(mine));
    int gidx = rdx_find_gid_index_ip(ctx, bind_ip, mine.gid);   /* the GID of the seat's link IP */
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    mine.qpn = qp->qp_num;
    mine.psn = 0x5678;
    if (rdx_io_all(sock, &mine, sizeof(mine), 1)) return 1;
    if (rdx_qp_to_rtr_rts(qp, &hello.conn, gidx, mine.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    int qos_fd = rdx_pm_qos(pm_qos_us);
    int rt_ok = !busy && !rdx_set_rt_prio(rt_prio);   /* a spinning SCHED_FIFO loop would starve the CPU */
    printf("[rx] connected, %u device(s), %s, spin %d us, PM QoS %s, SCHED_FIFO %s\n", hello.ndev,
           busy ? "busy poll" : "event driven", spin_us, qos_fd >= 0 ? "on" : "off", rt_ok ? "on" : "off");
    printf("[rx] completion vector %d of %d\n", comp_vector, ctx->num_comp_vectors);
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns(), t_act = 0;
    fflush(stdout);

    double *inj = calloc(1000000, sizeof(double));
    long ninj = 0, reports = 0, events = 0, readback_miss = 0, bad_pkt = 0;
    struct ibv_wc wc[32];
    int running = 1;

    while (running && !g_stop) {
        int nwc = ibv_poll_cq(cq, 32, wc);
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }
        if (nwc > 0) t_act = rdx_now_ns();
        if (!nwc && !busy && !(spin_us > 0 && t_act && rdx_now_ns() - t_act < (uint64_t)spin_us * 1000ull)) {
            /* arm, re-check (closes the race), then block on the channel */
            ibv_req_notify_cq(cq, 0);
            nwc = ibv_poll_cq(cq, 32, wc);
            if (!nwc) {
                struct pollfd cp = {.fd = ch->fd, .events = POLLIN};
                if (poll(&cp, 1, -1) > 0) {
                    struct ibv_cq *ecq;
                    void *ectx;
                    if (!ibv_get_cq_event(ch, &ecq, &ectx)) ibv_ack_cq_events(ecq, 1);
                }
                continue;
            }
        }
        for (int k = 0; k < nwc && running; k++) {
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[k].status),
                        (unsigned long long)wc[k].wr_id);
                return 1;
            }
            if (wc[k].opcode != IBV_WC_RECV) continue;          /* ack send completed */
            uint32_t idx = (uint32_t)(wc[k].wr_id - WR_RECV_BASE);
            if (idx >= NRECV) continue;
            struct rdx_input_pkt *p = &ring[idx];
            uint32_t seq = p->seq;
            if (p->dev == 0xFFFF) { running = 0; }
            else if (p->dev >= hello.ndev || p->n > RDX_INPUT_MAX_EV ||
                     wc[k].byte_len < RDX_INPUT_PKT_LEN(p->n)) {
                bad_pkt++;
            } else {
                struct input_event out[RDX_INPUT_MAX_EV];
                int32_t probe = -1, keys = 0;
                memset(out, 0, sizeof(out));
                for (int e = 0; e < p->n; e++) keys |= p->ev[e].type == EV_KEY;
                if (keys && !rdx_grab_gate_pass(&gate[p->dev])) {   /* not grabbed yet: hold back */
                    if (post_recv(qp, mr, ring, idx)) { perror("repost"); return 1; }
                    continue;
                }
                for (int e = 0; e < p->n; e++) {
                    out[e].type = p->ev[e].type;
                    out[e].code = p->ev[e].code;
                    out[e].value = p->ev[e].value;
                    if (p->ev[e].type == EV_MSC && p->ev[e].code == MSC_SCAN) probe = p->ev[e].value;
                }
                uint64_t t_inj = rdx_now_ns();
                ssize_t want = (ssize_t)(p->n * sizeof(out[0]));
                if (write(ufd[p->dev], out, (size_t)want) != want) perror("uinput write");
                uint32_t inj_us = RDX_ACK_NO_INJ;
                if (efd[p->dev] >= 0 && probe >= 0) {
                    if (!wait_probe(efd[p->dev], probe, t_inj + READBACK_TIMEOUT_NS)) {
                        uint64_t d = rdx_now_ns() - t_inj;
                        inj_us = (uint32_t)(d / 1000);
                        if (ninj < 1000000) inj[ninj++] = d / 1e6;
                    } else {
                        readback_miss++;
                    }
                }
                reports++;
                events += p->n;
                /* acks exist for measurement only: probe reports are acked, real reports are not */
                if (efd[p->dev] >= 0 && probe >= 0 &&
                    rdx_post_imm_send(qp, rdx_ack_imm(seq, inj_us), WR_ACK)) { perror("ack"); return 1; }
            }
            if (post_recv(qp, mr, ring, idx)) { perror("repost"); return 1; }
        }
    }

    rdx_cpu_report("rx", t0_wall, t0_cpu);
    if (qos_fd >= 0) close(qos_fd);
    for (uint32_t i = 0; i < hello.ndev; i++) {
        if (efd[i] >= 0) close(efd[i]);
        if (gate[i].held) printf("[grab] \"%s\": %ld report(s) held back in all\n", gate[i].what, gate[i].held);
        rdx_grab_gate_close(&gate[i]);
        ioctl(ufd[i], UI_DEV_DESTROY);
        close(ufd[i]);
    }
    printf("\n[rx] %s: %ld reports, %ld events injected, %ld bad packets, %ld read-back misses\n",
           g_stop ? "stopped" : "session ended", reports, events, bad_pkt, readback_miss);
    rdx_report("A uinput write -> evdev", inj, (size_t)ninj);
    close(sock);
    return 0;
}
