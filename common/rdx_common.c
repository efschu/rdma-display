/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "rdx_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

uint64_t rdx_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int rdx_parse_res(const char *s, uint32_t *w, uint32_t *h)
{
    if (!strcmp(s, "1080p")) { *w = 1920; *h = 1080; return 0; }
    if (!strcmp(s, "1440p")) { *w = 2560; *h = 1440; return 0; }
    if (!strcmp(s, "4k") || !strcmp(s, "2160p")) { *w = 3840; *h = 2160; return 0; }
    if (sscanf(s, "%ux%u", w, h) == 2 && *w > RDX_SEQ_BLK + RDX_BAR_W && *h > RDX_SEQ_BLK) return 0;
    return -1;
}

enum ibv_mtu rdx_parse_mtu(int mtu)
{
    switch (mtu) {
    case 256:  return IBV_MTU_256;
    case 512:  return IBV_MTU_512;
    case 2048: return IBV_MTU_2048;
    case 4096: return IBV_MTU_4096;
    default:   return IBV_MTU_1024;
    }
}

static struct ibv_context *open_by_name(const char *name)
{
    int n = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    struct ibv_context *ctx = NULL;
    for (int i = 0; list && i < n; i++)
        if (!strcmp(ibv_get_device_name(list[i]), name)) { ctx = ibv_open_device(list[i]); break; }
    if (list) ibv_free_device_list(list);
    return ctx;
}

struct ibv_context *rdx_open_dev_ip(const char *name, const char *local_ip)
{
    char buf[64];
    const char *dev = rdx_resolve_dev(name, local_ip, buf, sizeof(buf));
    if (!dev) return NULL;
    struct ibv_context *ctx = open_by_name(dev);
    if (!ctx) fprintf(stderr, "RDMA device %s not found or not openable\n", dev);
    return ctx;
}

struct ibv_context *rdx_open_dev(const char *name) { return rdx_open_dev_ip(name, NULL); }

const struct rdx_rdma_seam *rdx_rdma_seam;

/* a usable local IPv4 in network byte order, 0 = unknown */
static uint32_t local_ip4(const char *ip)
{
    struct in_addr a;
    if (!ip || !*ip || inet_pton(AF_INET, ip, &a) != 1) return 0;
    uint32_t h = ntohl(a.s_addr);
    if (!h || (h >> 24) == 127) return 0;                 /* 0.0.0.0, loopback: no GID carries it */
    return a.s_addr;
}

/* IPv4-mapped GID: bytes 0..9 ALL zero (a link-local fe80:: GID with ff:ff from the EUI-64
 * would otherwise match), 10..11 ff, a non-zero address */
static int gid_ip4(const uint8_t g[16], uint32_t *ip)
{
    for (int k = 0; k < 10; k++)
        if (g[k]) return 0;
    if (g[10] != 0xff || g[11] != 0xff || !(g[12] | g[13] | g[14] | g[15])) return 0;
    memcpy(ip, g + 12, 4);
    return 1;
}

int rdx_pick_gid(const struct rdx_gid_rec *t, int n, const char *local_ip)
{
    uint32_t want = local_ip4(local_ip), ip;
    int first_v2 = -1, first = -1, match_v2 = -1, match = -1;
    for (int i = 0; i < n; i++) {
        if (!gid_ip4(t[i].gid, &ip)) continue;
        if (first < 0) first = i;
        if (t[i].roce_v2 && first_v2 < 0) first_v2 = i;
        if (want && ip == want) {
            if (match < 0) match = i;
            if (t[i].roce_v2 && match_v2 < 0) match_v2 = i;
        }
    }
    if (want) return match_v2 >= 0 ? match_v2 : match >= 0 ? match : -2;
    return first_v2 >= 0 ? first_v2 : first;
}

/* port 1's GID table: ibv_query_gid_table (rdma-core >= 33, carries the type), else
 * ibv_query_gid per index + sysfs gid_attrs/types */
static int real_gid_table(struct ibv_context *ctx, const char *devname, struct rdx_gid_rec *out, int max)
{
    struct ibv_device_attr da;
    struct ibv_port_attr pa;
    int n = 0;
    if (!ibv_query_device(ctx, &da) && !ibv_query_port(ctx, 1, &pa) && pa.gid_tbl_len > 0) {
        size_t cap = (size_t)da.phys_port_cnt * (size_t)pa.gid_tbl_len;
        struct ibv_gid_entry *e = calloc(cap ? cap : 1, sizeof(*e));
        ssize_t got = e ? ibv_query_gid_table(ctx, e, cap, 0) : -1;
        for (ssize_t i = 0; i < got && n < max; i++) {
            if (e[i].port_num != 1) continue;
            out[n].index = (int)e[i].gid_index;
            memcpy(out[n].gid, e[i].gid.raw, 16);
            out[n].roce_v2 = e[i].gid_type == IBV_GID_TYPE_ROCE_V2;
            n++;
        }
        free(e);
        if (got >= 0) return n;
    }
    for (int i = 0; i < 256 && n < max; i++) {
        union ibv_gid g;
        if (ibv_query_gid(ctx, 1, i, &g)) continue;
        out[n].index = i;
        memcpy(out[n].gid, g.raw, 16);
        out[n].roce_v2 = rdx_gid_is_v2(devname, i);
        n++;
    }
    return n;
}

static int gid_table(struct ibv_context *ctx, const char *devname, struct rdx_gid_rec *out, int max)
{
    if (rdx_rdma_seam && rdx_rdma_seam->gid_table) return rdx_rdma_seam->gid_table(devname, out, max);
    if (ctx) return real_gid_table(ctx, devname, out, max);
    struct ibv_context *c = open_by_name(devname);
    if (!c) return 0;
    int n = real_gid_table(c, devname, out, max);
    ibv_close_device(c);
    return n;
}

#define RDX_GID_MAX 512

int rdx_find_gid_index_ip(struct ibv_context *ctx, const char *local_ip, uint8_t out[16])
{
    const char *dev = ctx ? ibv_get_device_name(ctx->device) : "";
    struct rdx_gid_rec *t = calloc(RDX_GID_MAX, sizeof(*t));
    if (!t) return -1;
    int n = gid_table(ctx, dev, t, RDX_GID_MAX);
    int k = rdx_pick_gid(t, n, local_ip);
    int idx = -1;
    if (k >= 0) {
        memcpy(out, t[k].gid, 16);
        idx = t[k].index;
        fprintf(stderr, "[gid] %s index %d = %u.%u.%u.%u (%s)%s%s\n", dev, idx, out[12], out[13], out[14], out[15],
                t[k].roce_v2 ? "RoCE v2" : "RoCE v1", local_ip4(local_ip) ? ", link IP " : ", no link IP known",
                local_ip4(local_ip) ? local_ip : "");
    } else if (k == -2) {
        fprintf(stderr, "no GID on %s carries %s (is the link IP on this device's netdev or a macvlan of it?)\n",
                dev, local_ip);
    } else {
        fprintf(stderr, "no IPv4 GID on %s\n", dev);
    }
    free(t);
    return idx;
}

int rdx_find_gid_index(struct ibv_context *ctx, const char *devname, uint8_t out[16])
{
    (void)devname;
    return rdx_find_gid_index_ip(ctx, NULL, out);
}

int rdx_local_ip_for(const char *peer_ip, char *out, size_t n)
{
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(9)};
    if (!peer_ip || inet_pton(AF_INET, peer_ip, &sa.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in me;
    socklen_t ml = sizeof(me);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) || getsockname(fd, (struct sockaddr *)&me, &ml) ||
             !inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)n) ? -1 : 0;
    close(fd);
    return rc;
}

static int real_dev_names(char names[][64], int max)
{
    int n = 0, k = 0;
    struct ibv_device **list = ibv_get_device_list(&n);
    for (int i = 0; list && i < n && k < max; i++) snprintf(names[k++], 64, "%s", ibv_get_device_name(list[i]));
    if (list) ibv_free_device_list(list);
    return k;
}

const char *rdx_resolve_dev(const char *want, const char *local_ip, char *buf, size_t n)
{
    if (want && *want && strcmp(want, "auto")) return want;
    char names[16][64];
    int nd = rdx_rdma_seam && rdx_rdma_seam->dev_names ? rdx_rdma_seam->dev_names(names, 16) : real_dev_names(names, 16);
    struct rdx_gid_rec *t = calloc(RDX_GID_MAX, sizeof(*t));
    int with_ip4 = 0, pick = -1, first_ip4 = -1;
    int known = local_ip4(local_ip) != 0;
    for (int d = 0; t && d < nd && pick < 0; d++) {
        int ng = gid_table(NULL, names[d], t, RDX_GID_MAX);
        int k = rdx_pick_gid(t, ng, known ? local_ip : NULL);
        if (known && k >= 0) pick = d;                 /* the device carrying the link IP */
        if (!known && k >= 0 && with_ip4++ == 0) first_ip4 = d;
    }
    if (!known && with_ip4 == 1) pick = first_ip4;     /* no link IP: only if unambiguous */
    free(t);
    if (pick < 0) {
        if (known) fprintf(stderr, "no RDMA device carries %s (rdma_dev auto)\n", local_ip);
        else fprintf(stderr, "rdma_dev auto: no link IP known and not exactly one RDMA device with an IPv4 GID\n");
        return NULL;
    }
    snprintf(buf, n, "%s", names[pick]);
    return buf;
}

int rdx_gid_is_v2(const char *devname, int idx)
{
    char p[256], buf[64] = {0};
    snprintf(p, sizeof(p), "/sys/class/infiniband/%s/ports/1/gid_attrs/types/%d", devname, idx);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
    fclose(f);
    return strstr(buf, "RoCE v2") != NULL;
}

struct ibv_qp *rdx_create_qp_inline(struct ibv_pd *pd, struct ibv_cq *cq, int depth, uint32_t max_inline)
{
    struct ibv_qp_init_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = cq; ia.recv_cq = cq; ia.qp_type = IBV_QPT_RC;
    ia.cap.max_send_wr = depth; ia.cap.max_recv_wr = depth;
    ia.cap.max_send_sge = 1; ia.cap.max_recv_sge = 1;
    ia.cap.max_inline_data = max_inline;
    struct ibv_qp *qp = ibv_create_qp(pd, &ia);
    if (!qp) perror("ibv_create_qp");
    return qp;
}

struct ibv_qp *rdx_create_qp(struct ibv_pd *pd, struct ibv_cq *cq, int depth)
{
    return rdx_create_qp_inline(pd, cq, depth, 0);
}

int rdx_qp_to_init(struct ibv_qp *qp)
{
    struct ibv_qp_attr at;
    memset(&at, 0, sizeof(at));
    at.qp_state        = IBV_QPS_INIT;
    at.pkey_index      = 0;
    at.port_num        = 1;
    at.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(qp, &at, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                               IBV_QP_ACCESS_FLAGS)) {
        perror("modify->INIT"); return -1;
    }
    return 0;
}

int rdx_qp_to_rtr_rts(struct ibv_qp *qp, const struct rdx_conn *peer, int sgid_idx,
                      uint32_t my_psn, enum ibv_mtu mtu)
{
    struct ibv_qp_attr at;
    memset(&at, 0, sizeof(at));
    at.qp_state           = IBV_QPS_RTR;
    at.path_mtu           = mtu;
    at.dest_qp_num        = peer->qpn;
    at.rq_psn             = peer->psn;
    at.max_dest_rd_atomic = 1;
    at.min_rnr_timer      = 12;
    at.ah_attr.is_global  = 1;
    at.ah_attr.port_num   = 1;
    at.ah_attr.grh.hop_limit  = 16;
    at.ah_attr.grh.sgid_index = sgid_idx;
    memcpy(at.ah_attr.grh.dgid.raw, peer->gid, 16);
    if (ibv_modify_qp(qp, &at, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                               IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                               IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        perror("modify->RTR"); return -1;
    }
    memset(&at, 0, sizeof(at));
    at.qp_state      = IBV_QPS_RTS;
    at.timeout       = 14;
    at.retry_cnt     = 7;
    at.rnr_retry     = 7;
    at.sq_psn        = my_psn;
    at.max_rd_atomic = 1;
    if (ibv_modify_qp(qp, &at, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("modify->RTS"); return -1;
    }
    return 0;
}

int rdx_post_zero_recv(struct ibv_qp *qp, uint64_t wr_id)
{
    struct ibv_recv_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.num_sge = 0;
    return ibv_post_recv(qp, &wr, &bad);
}

int rdx_post_imm_send(struct ibv_qp *qp, uint32_t imm, uint64_t wr_id)
{
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = wr_id;
    wr.opcode = IBV_WR_SEND_WITH_IMM;
    wr.num_sge = 0;
    wr.imm_data = htonl(imm);
    wr.send_flags = IBV_SEND_SIGNALED;
    return ibv_post_send(qp, &wr, &bad);
}

void rdx_stop_signals(void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                     /* no SA_RESTART */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int rdx_tcp_listen_accept(const char *bind_ip, int port)
{
    return rdx_tcp_listen_accept_as(bind_ip, port, NULL);
}

int rdx_tcp_listen_accept_as(const char *bind_ip, int port, const char *who)
{
    struct sockaddr_in a;
    int one = 1, lst = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(bind_ip);
    setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lst, (struct sockaddr *)&a, sizeof(a))) { perror("bind"); return -1; }
    if (listen(lst, 1)) { perror("listen"); return -1; }
    /* supervisors (rdx-clientd, rdx-hostd) start the peer only after this line */
    if (who) printf("[%s] listening on %s:%d\n", who, bind_ip, port);
    printf("[oob] waiting for source on %s:%d ...\n", bind_ip, port); fflush(stdout);
    int s = accept(lst, NULL, NULL);
    close(lst);
    if (s < 0) { perror("accept"); return -1; }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

int rdx_tcp_connect(const char *ip, int port)
{
    struct sockaddr_in a;
    int one = 1, s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(ip);
    for (int i = 0; i < 50; i++) {             /* the sink may start up to 5 s later */
        if (!connect(s, (struct sockaddr *)&a, sizeof(a))) {
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return s;
        }
        if (errno != ECONNREFUSED) break;
        usleep(100000);
    }
    perror("connect");
    close(s);
    return -1;
}

int rdx_io_all(int fd, void *p, size_t n, int do_write)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = do_write ? write(fd, b, n) : read(fd, b, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        b += r; n -= (size_t)r;
    }
    return 0;
}

/* both sides have reached RTS; nothing may be sent before this */
int rdx_sync(int fd)
{
    char c = 'R';
    if (rdx_io_all(fd, &c, 1, 1) || rdx_io_all(fd, &c, 1, 0) || c != 'R') return -1;
    return 0;
}

/* ---------------- test pattern ---------------- */

uint32_t rdx_bar_x(uint32_t seq, uint32_t w)
{
    return (seq * 8u) % (w - RDX_BAR_W);
}

static inline uint32_t rdx_gradient(uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    return ((x * 255u / (w - 1)) << 16) | ((y * 255u / (h - 1)) << 8) | 0x40u;
}

/* XRGB8888, pixel value 0x00RRGGBB; the X byte is ignored */
uint32_t rdx_expected_pixel(uint32_t seq, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    if (x < RDX_SEQ_BLK && y < RDX_SEQ_BLK) return seq & 0x00FFFFFFu;
    uint32_t bx = rdx_bar_x(seq, w);
    if (x >= bx && x < bx + RDX_BAR_W) return 0x00FFFFFFu;
    return rdx_gradient(w, h, x, y);
}

void rdx_fill_base(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride)
{
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)y * stride);
        for (uint32_t x = 0; x < w; x++) row[x] = rdx_gradient(w, h, x, y);
    }
}

/* same layering as rdx_expected_pixel: gradient, bar on top, counter on top of that */
void rdx_paint_frame(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride,
                     uint32_t seq, int64_t *prev_bar_x)
{
    uint32_t bx = rdx_bar_x(seq, w);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)y * stride);
        if (*prev_bar_x >= 0)
            for (uint32_t x = (uint32_t)*prev_bar_x; x < (uint32_t)*prev_bar_x + RDX_BAR_W; x++)
                row[x] = rdx_gradient(w, h, x, y);
        for (uint32_t x = bx; x < bx + RDX_BAR_W; x++) row[x] = 0x00FFFFFFu;
        if (y < RDX_SEQ_BLK)
            for (uint32_t x = 0; x < RDX_SEQ_BLK; x++) row[x] = seq & 0x00FFFFFFu;
    }
    *prev_bar_x = bx;
}

/* ---------------- vblank lock ---------------- */

static double lock_floor(double x)   /* floor() without libm */
{
    long i = (long)x;
    return (double)(i - ((double)i > x));
}

uint64_t rdx_lock_due(uint64_t anchor, double period, double interval, int ahead,
                      struct rdx_lock *L, double lead, uint64_t now)
{
    /* Count whole vblank steps from the previous target instead of rounding absolute times.
     * The previous target sits (near) exactly on a vblank, so finding its index on the grid
     * of the current anchor rounds a value close to an integer: stable even though the anchor
     * jitters by ~100 us. Rounding the ideal time of an accumulated interval instead hits the
     * .5 boundary as that time drifts, and then flips between 1 and 3 vblanks.
     * frac carries the fractional steps: 144 fps on 240 Hz advances 1,2,2,1,2,2... vblanks. */
    double step = interval / period;
    double sr = lock_floor(step + 0.5);
    if (sr >= 1 && step - sr < 1e-3 && sr - step < 1e-3) step = sr;   /* even cadence: exact */
    if (step < 1) step = 1;
    /* hard limits, rounded up: one vblank per frame in flight, and post time not in the past;
     * the ceil has a 1/1000-period tolerance so an exact multiple does not skip a vblank */
    double floor_t = (double)anchor + period * (1 + ahead);
    if ((double)now + lead > floor_t) floor_t = (double)now + lead;
    double kf = -lock_floor(-((floor_t - (double)anchor) / period - 1e-3));
    double k = kf;
    if (L->last_target) {
        double klast = lock_floor(((double)L->last_target - (double)anchor) / period + 0.5);
        double adv = L->frac + step;
        double n = lock_floor(adv + 1e-6);
        L->frac = adv - n > 0 ? adv - n : 0;
        k = klast + n;
        if (k < kf) { k = kf; L->frac = 0; }   /* pushed late: restart the phase, no catch-up burst */
    } else {
        L->frac = 0;
    }
    double target = (double)anchor + k * period;
    L->last_target = (uint64_t)target;
    return (uint64_t)(target - lead);
}

/* ---------------- event-driven waiting ---------------- */

struct ibv_cq *rdx_create_cq_ev(struct ibv_context *ctx, int n, struct ibv_comp_channel **ch)
{
    *ch = ibv_create_comp_channel(ctx);
    if (!*ch) { perror("ibv_create_comp_channel"); return NULL; }
    int fl = fcntl((*ch)->fd, F_GETFL);
    fcntl((*ch)->fd, F_SETFL, fl | O_NONBLOCK);
    struct ibv_cq *cq = ibv_create_cq(ctx, n, NULL, *ch, 0);
    if (!cq) perror("ibv_create_cq");
    return cq;
}

static int hca_raw(struct ibv_context *ctx, uint64_t *raw, uint64_t *mono)
{
    struct ibv_values_ex v;
    memset(&v, 0, sizeof(v));
    v.comp_mask = IBV_VALUES_MASK_RAW_CLOCK;
    uint64_t a = rdx_now_ns();
    if (ibv_query_rt_values_ex(ctx, &v)) return -1;
    uint64_t b = rdx_now_ns();
    *raw = (uint64_t)v.raw_clock.tv_sec * 1000000000ull + (uint64_t)v.raw_clock.tv_nsec;
    *mono = a + (b - a) / 2;
    return 0;
}

int rdx_cq_create_ts(struct ibv_context *ctx, int n, int comp_vector, struct rdx_cq *rc)
{
    int vec = rdx_pick_comp_vector(ctx, comp_vector);
    memset(rc, 0, sizeof(*rc));
    rc->ctx = ctx;
    struct ibv_device_attr_ex da;
    memset(&da, 0, sizeof(da));
    rc->ch = ibv_create_comp_channel(ctx);
    if (!rc->ch) { perror("ibv_create_comp_channel"); return -1; }
    int fl = fcntl(rc->ch->fd, F_GETFL);
    fcntl(rc->ch->fd, F_SETFL, fl | O_NONBLOCK);
    if (!ibv_query_device_ex(ctx, NULL, &da) && da.hca_core_clock && !hca_raw(ctx, &rc->raw0, &rc->mono0)) {
        struct ibv_cq_init_attr_ex ca;
        memset(&ca, 0, sizeof(ca));
        ca.cqe = (uint32_t)n;
        ca.channel = rc->ch;
        ca.comp_vector = (uint32_t)vec;
        ca.wc_flags = IBV_WC_EX_WITH_COMPLETION_TIMESTAMP | IBV_WC_EX_WITH_IMM;
        rc->cqx = ibv_create_cq_ex(ctx, &ca);
        if (rc->cqx) {
            rc->khz = (double)da.hca_core_clock;
            rc->raw1 = rc->raw0;
            rc->mono1 = rc->mono0;
            rc->cq = ibv_cq_ex_to_cq(rc->cqx);
            return 0;
        }
    }
    fprintf(stderr, "[cq] no NIC completion timestamps, plain CQ\n");
    rc->cq = ibv_create_cq(ctx, n, NULL, rc->ch, vec);
    if (!rc->cq) { perror("ibv_create_cq"); return -1; }
    return 0;
}

void rdx_cq_clock_sync(struct rdx_cq *rc)
{
    uint64_t raw, mono;
    if (!rc->cqx || hca_raw(rc->ctx, &raw, &mono)) return;
    if (mono - rc->mono1 >= 500000000ull) {          /* keep a >= 0.5 s baseline for the slope */
        rc->raw0 = rc->raw1; rc->mono0 = rc->mono1;
        rc->raw1 = raw;      rc->mono1 = mono;
    }
}

static uint64_t hw_to_mono(const struct rdx_cq *rc, uint64_t raw)
{
    double ns_per_tick = rc->raw1 > rc->raw0 ? (double)(rc->mono1 - rc->mono0) / (double)(rc->raw1 - rc->raw0)
                                             : 1e6 / rc->khz;
    return (uint64_t)((double)rc->mono1 + ((double)raw - (double)rc->raw1) * ns_per_tick);
}

int rdx_cq_poll(struct rdx_cq *rc, struct rdx_wc *out, int max)
{
    if (!rc->cqx) {
        struct ibv_wc wc[32];
        int n = ibv_poll_cq(rc->cq, max < 32 ? max : 32, wc);
        for (int i = 0; i < n; i++)
            out[i] = (struct rdx_wc){wc[i].status, wc[i].opcode, wc[i].wr_id, wc[i].imm_data, 0};
        return n;
    }
    struct ibv_poll_cq_attr pa = {0};
    int r = ibv_start_poll(rc->cqx, &pa);
    if (r == ENOENT) return 0;
    if (r) return -1;
    int n = 0;
    do {
        struct ibv_cq_ex *c = rc->cqx;
        out[n].status = c->status;
        out[n].wr_id = c->wr_id;
        out[n].opcode = ibv_wc_read_opcode(c);
        out[n].imm_data = (out[n].opcode == IBV_WC_RECV || out[n].opcode == IBV_WC_RECV_RDMA_WITH_IMM)
                              ? ibv_wc_read_imm_data(c) : 0;
        out[n].hw_ns = hw_to_mono(rc, ibv_wc_read_completion_ts(c));
        n++;
    } while (n < max && ibv_next_poll(rc->cqx) == 0);
    ibv_end_poll(rc->cqx);
    return n;
}

int rdx_sleep_on(struct ibv_comp_channel *ch, int extra_fd, uint64_t timeout_ns)
{
    struct pollfd pfd[2] = {{.fd = ch->fd, .events = POLLIN}, {.fd = extra_fd, .events = POLLIN}};
    struct timespec ts = {.tv_sec = (time_t)(timeout_ns / 1000000000ull), .tv_nsec = (long)(timeout_ns % 1000000000ull)};
    int r = ppoll(pfd, extra_fd >= 0 ? 2 : 1, timeout_ns == UINT64_MAX ? NULL : &ts, NULL);
    if (r <= 0) return 0;
    if (pfd[0].revents & POLLIN) {
        struct ibv_cq *ev_cq;
        void *ev_ctx;
        while (ibv_get_cq_event(ch, &ev_cq, &ev_ctx) == 0) ibv_ack_cq_events(ev_cq, 1);
    }
    return extra_fd >= 0 && (pfd[1].revents & POLLIN);
}

int rdx_pm_qos(int us)
{
    if (us <= 0) return -1;
    int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    int32_t v = us;
    if (fd < 0 || write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
        perror("PM QoS /dev/cpu_dma_latency (wake-up tails may grow)");
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

/* ---------------- telemetry ---------------- */

static int tlog_new_file(struct rdx_tlog *t)
{
    char host[64] = "host", stamp[32], path[400];
    gethostname(host, sizeof(host) - 1);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    mkdir(t->dir, 0755);
    snprintf(path, sizeof(path), "%s/%s-%s-%s.csv", t->dir, t->role, host, stamp);
    t->f = fopen(path, "w");
    if (!t->f) { perror(path); return -1; }
    setvbuf(t->f, NULL, _IOFBF, 1 << 20);
    struct timespec mo, re;
    clock_gettime(CLOCK_MONOTONIC, &mo);
    clock_gettime(CLOCK_REALTIME, &re);
    t->bytes = (uint64_t)fprintf(t->f, "# rdx telemetry role=%s host=%s\n# clock mono_ns=%llu real_ns=%llu\n%s\n",
                                 t->role, host,
                                 (unsigned long long)mo.tv_sec * 1000000000ull + (unsigned long long)mo.tv_nsec,
                                 (unsigned long long)re.tv_sec * 1000000000ull + (unsigned long long)re.tv_nsec,
                                 t->header);
    printf("[tlog] writing %s\n", path);
    return 0;
}

int rdx_tlog_open(struct rdx_tlog *t, const char *dir, const char *role, const char *header)
{
    memset(t, 0, sizeof(*t));
    if (!dir || !strcmp(dir, "-")) return 0;
    snprintf(t->dir, sizeof(t->dir), "%s", dir);
    snprintf(t->role, sizeof(t->role), "%s", role);
    snprintf(t->header, sizeof(t->header), "%s", header);
    return tlog_new_file(t);
}

void rdx_tlog_line(struct rdx_tlog *t, const char *fmt, ...)
{
    if (!t->f) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(t->f, fmt, ap);
    va_end(ap);
    fputc('\n', t->f);
    t->bytes += (uint64_t)(n > 0 ? n : 0) + 1;
    if (t->bytes > (256ull << 20)) {               /* rotate */
        fclose(t->f);
        t->f = NULL;
        tlog_new_file(t);
    }
}

void rdx_tlog_tick(struct rdx_tlog *t, uint64_t now)
{
    if (!t->f || now - t->t_flush < 1000000000ull) return;
    fflush(t->f);
    t->t_flush = now;
}

void rdx_tlog_close(struct rdx_tlog *t)
{
    if (t->f) fclose(t->f);
    t->f = NULL;
}

int rdx_pick_comp_vector(struct ibv_context *ctx, int want)
{
    int n = ctx->num_comp_vectors > 0 ? ctx->num_comp_vectors : 1;
    return want >= 0 ? want % n : n - 1;
}

int rdx_set_rt_prio(int prio)
{
    if (prio <= 0) return -1;
    struct sched_param sp = {.sched_priority = prio};
    if (sched_setscheduler(0, SCHED_FIFO, &sp)) { perror("sched_setscheduler(SCHED_FIFO)"); return -1; }
    return 0;
}

uint64_t rdx_cpu_ns(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec) * 1000000000ull +
           ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec) * 1000ull;
}

void rdx_cpu_report(const char *who, uint64_t t0_wall, uint64_t t0_cpu)
{
    double wall = (rdx_now_ns() - t0_wall) / 1e9, cpu = (rdx_cpu_ns() - t0_cpu) / 1e9;
    printf("[%s] cpu: %.1f ms user+sys over %.2f s = %.2f %% of one core\n", who, cpu * 1e3, wall,
           wall > 0 ? 100.0 * cpu / wall : 0.0);
}

/* ---------------- statistics ---------------- */

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void rdx_report(const char *name, double *v, size_t n)
{
    if (!n) { printf("  %-24s no samples\n", name); return; }
    qsort(v, n, sizeof(*v), cmp_d);
    printf("  %-24s n=%zu  p50=%8.3f  p90=%8.3f  p99=%8.3f  max=%8.3f ms\n", name, n,
           v[n / 2], v[(size_t)(n * 0.90)], v[(size_t)(n * 0.99)], v[n - 1]);
}
