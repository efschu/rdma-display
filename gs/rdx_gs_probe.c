/* Test stand-in for rdx_gs_src without a sink and without an RDMA NIC: hands host-memory slots
 * to a gamescope (rdx backend, or the stream add-on next to the DRM backend, see
 * docs/SEAT-MONITOR.md) over the rdx_gs_proto.h socket and checks what comes back.
 *
 *   rdx_gs_probe --socket PATH --uuid GPU-xxxxxxxx-... [--mode 1280x720@240] [--slots 3]
 *                [--seconds 20] [--hold-ms 4.2] [--vblank] [--vrr] [--cursor] [--input-cycle] [--dump FILE.ppm]
 *                [--user NAME]
 *
 * - listens on PATH (owned by --user, so gamescope running as that user can connect), sends
 *   HELLO with memfd host slots of the mode (stride = width * 4 rounded to 256, like the sink),
 *   takes the ACK, frees every slot;
 * - on READY: samples the slot (64x36 luma points: black / white / uniform / content) and
 *   gives it back --hold-ms later (a client that shows each frame for that long);
 * - --vblank: sends a VBLANK after every READY (the latest point of a grid at the mode's refresh, 2 ms
 *   early, at or before now), as rdx_gs_src does after each "shown" report (rdx_srcsess_vblank_before):
 *   one per frame, never a moving target, never a vblank in the future;
 * - --vrr: variable refresh (RDX_GS_FLAG_VRR, the mode's refresh is the cap): no VBLANK, gamescope paints at
 *   the app's pace; READY intervals then follow the app (not multiples of the period) and the count of
 *   intervals shorter than 98 % of the period shows how often rdx_gs_src would hold a frame to the cap;
 * - --cursor: RDX_GS_CURSOR 1 after the ACK (a client that shows the cursor itself, common/rdx_cursor.h):
 *   gamescope draws none into the frames and sends CURIMG / CURPOS instead; every picture and position is
 *   printed ("[probe] cursor ..."), and the frames it painted only for the pointer are counted;
 * - --input-cycle: RDX_GS_INPUT 0 after a third of --seconds (a detach: gamescope lets go of every input
 *   device, its log says "every device let go"), RDX_GS_INPUT 1 after two thirds (attached again);
 * - at the end: frames, rate, READY intervals, the classes, and the last frame as PPM.
 * --uuid: the GPU's UUID as nvidia-smi --query-gpu=uuid prints it (the stream add-on refuses
 * slots of another GPU). Exit 0 if at least one frame arrived and none was black or white
 * while content frames exist (a flicker check), 1 otherwise. */
#define _GNU_SOURCE
#include "rdx_gs_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int parse_uuid(const char *s, uint8_t out[16])
{
    if (!strncmp(s, "GPU-", 4)) s += 4;
    int n = 0;
    for (; *s && n < 32; s++) {
        if (*s == '-') continue;
        char c = *s;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return -1;
        if (n % 2 == 0) out[n / 2] = (uint8_t)(v << 4); else out[n / 2] |= (uint8_t)v;
        n++;
    }
    return n == 32 ? 0 : -1;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *sock_path = NULL, *uuid_s = NULL, *dump = NULL, *user = NULL;
    uint32_t w = 1280, h = 720, ns = 3;
    double hz = 240, seconds = 20, hold_ms = -1;
    int vblank = 0, vrr = 0, cursor = 0, input_cycle = 0;
    static const struct option opts[] = {
        {"socket", 1, 0, 's'}, {"uuid", 1, 0, 'u'}, {"mode", 1, 0, 'm'}, {"slots", 1, 0, 'n'},
        {"seconds", 1, 0, 'S'}, {"hold-ms", 1, 0, 'H'}, {"vblank", 0, 0, 'V'}, {"dump", 1, 0, 'D'},
        {"user", 1, 0, 'U'}, {"vrr", 0, 0, 'R'}, {"cursor", 0, 0, 'C'}, {"input-cycle", 0, 0, 'I'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 's': sock_path = optarg; break;
        case 'u': uuid_s = optarg; break;
        case 'm':
            if (sscanf(optarg, "%ux%u@%lf", &w, &h, &hz) < 2) { fprintf(stderr, "--mode WxH[@Hz]\n"); return 2; }
            break;
        case 'n': ns = (uint32_t)atoi(optarg); break;
        case 'S': seconds = atof(optarg); break;
        case 'H': hold_ms = atof(optarg); break;
        case 'V': vblank = 1; break;
        case 'R': vrr = 1; break;
        case 'C': cursor = 1; break;
        case 'I': input_cycle = 1; break;
        case 'D': dump = optarg; break;
        case 'U': user = optarg; break;
        default: fprintf(stderr, "see the header of gs/rdx_gs_probe.c\n"); return 2;
        }
    }
    struct rdx_gs_hello hello = {.magic = RDX_GS_MAGIC, .type = RDX_GS_HELLO, .width = w, .height = h,
                                 .stride = (w * 4 + 255) & ~255u, .drm_fourcc = 0x34325258 /* XR24 */,
                                 .nslots = ns, .refresh_mhz = (uint32_t)(hz * 1000 + 0.5),
                                 .buf_kind = RDX_GS_BUF_HOST, .flags = 0};
    if (!sock_path || !uuid_s || parse_uuid(uuid_s, hello.gpu_uuid) || !ns || ns > RDX_GS_MAX_SLOTS || !w || !h) {
        fprintf(stderr, "need --socket and --uuid (nvidia-smi --query-gpu=uuid), 1..%d slots\n", RDX_GS_MAX_SLOTS);
        return 2;
    }
    if (hold_ms < 0) hold_ms = 1000.0 / hz;
    if (vrr) {
        hello.flags |= RDX_GS_FLAG_VRR;
        vblank = 0;                         /* variable refresh: gamescope gets no VBLANK */
    }
    const uint64_t frame_bytes = (uint64_t)hello.stride * h;
    hello.slot_size = (frame_bytes + (2u << 20) - 1) & ~(uint64_t)((2u << 20) - 1);
    int fds[RDX_GS_MAX_SLOTS];
    uint8_t *map[RDX_GS_MAX_SLOTS];
    for (uint32_t i = 0; i < ns; i++) {
        fds[i] = memfd_create("rdx-gs-probe", MFD_CLOEXEC);
        if (fds[i] < 0 || ftruncate(fds[i], (off_t)hello.slot_size)) { perror("memfd"); return 1; }
        map[i] = mmap(NULL, hello.slot_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fds[i], 0);
        if (map[i] == MAP_FAILED) { perror("mmap"); return 1; }
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(lfd, 1)) { perror(sock_path); return 1; }
    if (user) {
        struct passwd *pw = getpwnam(user);
        if (!pw || chown(sock_path, pw->pw_uid, pw->pw_gid)) { fprintf(stderr, "cannot hand %s to %s\n", sock_path, user); return 1; }
    }
    printf("[probe] %ux%u@%.3f, %u host slots of %llu B, waiting for gamescope on %s\n", w, h, hz, ns,
           (unsigned long long)hello.slot_size, sock_path);
    fflush(stdout);
    struct pollfd ap = {.fd = lfd, .events = POLLIN};
    if (poll(&ap, 1, (int)(seconds * 1000)) <= 0) { fprintf(stderr, "[probe] nobody connected\n"); unlink(sock_path); return 1; }
    int gfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    struct iovec iov = {.iov_base = &hello, .iov_len = sizeof(hello)};
    char cbuf[CMSG_SPACE(sizeof(int) * RDX_GS_MAX_SLOTS)];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = CMSG_SPACE(sizeof(int) * ns)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * ns);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * ns);
    struct rdx_gs_ack ack;
    if (gfd < 0 || sendmsg(gfd, &mh, MSG_NOSIGNAL) != (ssize_t)sizeof(hello) ||
        recv(gfd, &ack, sizeof(ack), 0) != (ssize_t)sizeof(ack) || ack.magic != RDX_GS_MAGIC || ack.type != RDX_GS_ACK) {
        fprintf(stderr, "[probe] handshake failed\n");
        return 1;
    }
    printf("[probe] ACK: row pitch %u (stride %u), error %d, path %s\n", ack.row_pitch, hello.stride, ack.error,
           ack.path ? "composite + copy" : "direct");
    if (ack.error || ack.row_pitch != hello.stride) return 1;
    for (uint32_t i = 0; i < ns; i++) {
        struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_FREE, .slot = i};
        send(gfd, &m, sizeof(m), MSG_NOSIGNAL);
    }
    if (cursor) {                         /* the client shows the cursor itself */
        struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_CURSOR, .slot = 1};
        send(gfd, &m, sizeof(m), MSG_NOSIGNAL);
    }
    long cur_pos = 0, cur_chunks = 0, cur_pics = 0;

    int vfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    uint64_t period = (uint64_t)(1e9 / hz), grid0 = now_ns();
    uint64_t free_at[RDX_GS_MAX_SLOTS] = {0};
    const long cap = 200000;
    double *gaps = calloc((size_t)cap, sizeof(double)), *draw = calloc((size_t)cap, sizeof(double));
    long nframes = 0, ngap = 0, nblack = 0, nwhite = 0, nuniform = 0, ncontent = 0, switches = 0, vblanks = 0;
    char last_class = 0;
    int last_slot = -1;
    uint64_t t0 = now_ns(), t_last = 0;
    int input_step = 0;
    while (!g_stop && now_ns() - t0 < (uint64_t)(seconds * 1e9)) {
        /* due credits */
        uint64_t tn = now_ns(), next = tn + 100000000ull;
        if (input_cycle && input_step < 2 && tn - t0 >= (uint64_t)(seconds * 1e9 * (input_step + 1) / 3)) {
            struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_INPUT, .slot = input_step ? 1u : 0u};
            send(gfd, &m, sizeof(m), MSG_NOSIGNAL);
            printf("[probe] RDX_GS_INPUT %d sent (%s)\n", input_step ? 1 : 0, input_step ? "attached again" : "detached");
            fflush(stdout);
            input_step++;
        }
        for (uint32_t i = 0; i < ns; i++) {
            if (!free_at[i]) continue;
            if (free_at[i] <= tn) {
                struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_FREE, .slot = i};
                send(gfd, &m, sizeof(m), MSG_NOSIGNAL);
                free_at[i] = 0;
            } else if (free_at[i] < next) next = free_at[i];
        }
        struct pollfd p[2] = {{.fd = gfd, .events = POLLIN}, {.fd = vfd, .events = POLLIN}};
        int tmo = (int)((next - tn) / 1000000ull);
        if (poll(p, 2, tmo) < 0 && errno != EINTR) break;
        if (p[1].revents & POLLIN) {
            uint64_t exp;
            if (read(vfd, &exp, sizeof(exp)) > 0) {
                struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = RDX_GS_VBLANK, .t_ns = now_ns() + period - 2000000ull};
                send(gfd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
                vblanks++;
            }
        }
        if (p[0].revents & (POLLHUP | POLLERR)) { printf("[probe] gamescope closed the socket\n"); break; }
        if (!(p[0].revents & POLLIN)) continue;
        static union { struct rdx_gs_msg m; struct rdx_gs_curimg c; uint8_t b[sizeof(struct rdx_gs_curimg) + RDX_GS_CUR_CHUNK]; } u;
        ssize_t r = recv(gfd, &u, sizeof(u), 0);
        if (r <= 0) { printf("[probe] gamescope closed the socket\n"); break; }
        struct rdx_gs_msg m = u.m;
        if (r >= (ssize_t)sizeof(struct rdx_gs_curimg) && m.magic == RDX_GS_MAGIC && m.type == RDX_GS_CURIMG) {
            const struct rdx_gs_curimg *c = &u.c;
            long lit = 0;
            for (uint32_t i = 0; i + 4 <= c->len; i += 4) lit += u.b[sizeof(*c) + i + 3] != 0;    /* alpha */
            cur_chunks++;
            if (c->off + c->len == (uint32_t)c->w * c->h * 4) cur_pics++;
            printf("[probe] cursor picture %u: %ux%u hotspot %u,%u, chunk at %u of %u B, %ld opaque pixels in it\n", c->serial,
                   c->w, c->h, c->hot_x, c->hot_y, c->off, c->len, lit);
            continue;
        }
        if (r == (ssize_t)sizeof(m) && m.magic == RDX_GS_MAGIC && m.type == RDX_GS_CURPOS) {
            cur_pos++;
            printf("[probe] cursor at %d,%d %s (picture %u)\n", (int32_t)(uint32_t)m.seq, (int32_t)(uint32_t)(m.seq >> 32),
                   (m.slot & RDX_GS_CUR_VISIBLE) ? "visible" : "hidden", m.pad);
            continue;
        }
        if (r != (ssize_t)sizeof(m) || m.magic != RDX_GS_MAGIC || m.type != RDX_GS_READY || m.slot >= ns) continue;
        uint64_t t = now_ns();
        if (t_last && ngap < cap) gaps[ngap++] = (t - t_last) / 1e6;
        if (m.t_begin_ns && m.t_ns >= m.t_begin_ns && nframes < cap) draw[nframes] = (m.t_ns - m.t_begin_ns) / 1e6;
        t_last = t;
        /* 64x36 luma points */
        double sum = 0, sum2 = 0;
        const uint8_t *s = map[m.slot];
        for (uint32_t y = 0; y < 36; y++)
            for (uint32_t x = 0; x < 64; x++) {
                const uint8_t *px = s + (size_t)(y * h / 36 + h / 72) * hello.stride + (size_t)(x * w / 64 + w / 128) * 4;
                double l = (px[0] * 29 + px[1] * 150 + px[2] * 77) / 256.0;
                sum += l;
                sum2 += l * l;
            }
        double mean = sum / 2304, sd = sum2 / 2304 - mean * mean;
        sd = sd > 0 ? __builtin_sqrt(sd) : 0;
        char cl = sd < 1 && mean < 1 ? 'B' : sd < 1 && mean > 254 ? 'W' : sd < 1 ? 'U' : 'i';
        if (cl == 'B') nblack++; else if (cl == 'W') nwhite++; else if (cl == 'U') nuniform++; else ncontent++;
        if (last_class && cl != last_class) switches++;
        last_class = cl;
        last_slot = (int)m.slot;
        nframes++;
        free_at[m.slot] = t + (uint64_t)(hold_ms * 1e6);
        if (vblank) {                       /* the latest point of our grid (2 ms early) at or before now */
            uint64_t k = (t + 2000000ull - grid0) / period;
            struct rdx_gs_msg vm = {.magic = RDX_GS_MAGIC, .type = RDX_GS_VBLANK, .t_ns = grid0 + k * period - 2000000ull};
            send(gfd, &vm, sizeof(vm), MSG_NOSIGNAL | MSG_DONTWAIT);
            vblanks++;
        }
    }
    double el = (now_ns() - t0) / 1e9;
    if (cursor)
        printf("[probe] cursor next to the picture: %ld positions, %ld pictures in %ld chunks\n", cur_pos, cur_pics, cur_chunks);
    printf("[probe] %ld frames in %.1f s (%.1f fps), %ld VBLANK sent; content %ld, black %ld, white %ld, uniform %ld, "
           "class switches %ld\n", nframes, el, nframes / el, vblanks, ncontent, nblack, nwhite, nuniform, switches);
    if (ngap) {
        long nshort = 0;
        for (long i = 0; i < ngap; i++) nshort += gaps[i] < 0.98 * 1000.0 / hz;
        qsort(gaps, (size_t)ngap, sizeof(double), cmp_d);
        if (vrr)
            printf("[probe] variable refresh, cap %.3f Hz: %ld of %ld READY intervals below 98 %% of the period (%.2f ms)\n",
                   hz, nshort, ngap, 1000.0 / hz);
        printf("[probe] READY interval p50 %.2f p90 %.2f p99 %.2f max %.2f ms\n", gaps[ngap / 2], gaps[ngap * 9 / 10],
               gaps[ngap * 99 / 100], gaps[ngap - 1]);
    }
    long nd = nframes < cap ? nframes : cap;
    if (nd) {
        qsort(draw, (size_t)nd, sizeof(double), cmp_d);
        printf("[probe] composite (+ copy) begin -> READY p50 %.2f p99 %.2f ms\n", draw[nd / 2], draw[nd * 99 / 100]);
    }
    if (dump && last_slot >= 0) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t *px = map[last_slot] + (size_t)y * hello.stride + (size_t)x * 4;
                    uint8_t rgb[3] = {px[2], px[1], px[0]};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
            printf("[probe] last frame -> %s\n", dump);
        }
    }
    struct rdx_gs_msg bye = {.magic = RDX_GS_MAGIC, .type = RDX_GS_BYE};
    send(gfd, &bye, sizeof(bye), MSG_NOSIGNAL);
    close(gfd);
    unlink(sock_path);
    return nframes > 0 && !((nblack || nwhite) && ncontent) ? 0 : 1;
}
