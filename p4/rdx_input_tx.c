/* SPDX-License-Identifier: MIT
 *
 * P4 input sender: runs on the host with keyboard/mouse (B). Reads evdev
 * devices and forwards every report (events up to SYN_REPORT) immediately as
 * one inline RDMA SEND to rdx_input_rx on the host running the program (A).
 * No batching across reports, no TCP on the data path.
 *
 * Real devices (--input, repeatable) are grabbed (EVIOCGRAB) by default so the
 * local desktop no longer sees them; --no-grab shares them. LeftCtrl+LeftAlt+Esc
 * releases the grab and exits; every key/button still held is released on A
 * first, so nothing sticks.
 *
 * System keys (power, sleep, suspend, wake-up, ...: rdx_input_system_key) never go to A: there
 * every key device is a power switch of its logind, a power key would switch A off. They stay
 * here; with the devices grabbed this computer's logind / desktop no longer sees them, so they
 * come out of a local uinput device "rdma-display system keys (local)" instead.
 *
 * Waiting (default): the process blocks in ppoll on the evdev fds and the CQ
 * completion-channel fd; it wakes only for an input event, a completion or the
 * next probe deadline. --busy-poll spins instead (one core at 100 %).
 * --spin-us N keeps spinning for N us after each event before blocking again
 * (hybrid). PM QoS (--pm-qos-us, default 50, 0 = off) holds
 * /dev/cpu_dma_latency so a blocked CPU wakes up quickly. --rt-prio (default 10,
 * 0 = off) runs the loop SCHED_FIFO so a wake-up is not queued behind desktop load
 * (the probe writer child resets itself to SCHED_OTHER).
 *
 * Probe mode (--probe-count N): creates a local "rdx-probe" uinput device that
 * only emits MSC_SCAN (desktops ignore it on both hosts). A forked writer
 * process writes N probes at --probe-rate Hz; this process reads them through
 * evdev like a real device, so its CPU and wake-ups are those of the real path.
 * Measured per probe: capture (writer's uinput write -> our evdev read, i.e. the
 * wake-up latency, B clock), round trip (send -> ack, B clock) and inject on A
 * (uinput write -> mirror evdev, A clock, carried in the ack immediate).
 * --probe-silent: rx neither reads back nor acks (plain-path CPU, no latency).
 *
 * In-stream menu (--menu-sock PATH, set by rdx-clientd): Ctrl+Alt+Shift+M opens the
 * menu. Everything held is released on A first, then key presses go to rdx-clientd
 * (common/rdx_menu_ipc.h) and nothing is forwarded until clientd reports the menu
 * closed. Mouse events are swallowed while it is open. The release combo still works.
 *
 * --desktop-hotkey SPEC (repeatable, e.g. "ctrl+alt+h", common/rdx_hotkey.h; rdx-clientd sets it for
 * a game monitor taken by lease, where the desktop runs on the other monitors): the keyboard and
 * mouse toggle between the stream (grabbed, everything forwarded, the combination itself swallowed)
 * and the desktop (grab let go; still read passively, nothing forwarded, so the combination is seen
 * also while the desktop or a desktop game holds the keyboard). Going to the desktop first lets go
 * of everything held on the host; coming back grabs only once every key is up (EVIOCGKEY), so the
 * desktop has seen the combination's releases too: nothing stays pressed on either side. The
 * change goes to rdx-clientd (RDX_MENU_DESKTOP) for the "Desktop" card on the stream.
 *
 *   rdx_input_tx --peer 10.0.0.2 [--dev auto] [--port 18660] [--mtu 4096]
 *                [--input /dev/input/eventN ...] [--no-grab] [--desktop-hotkey ctrl+alt+h ...]
 *                [--probe-count 5000] [--probe-rate 1000] [--probe-silent] [--duration-s 0]
 *                [--busy-poll] [--spin-us 0] [--pm-qos-us 50] [--rt-prio 10]
 *                [--comp-vector -1]
 */
#define _GNU_SOURCE
#include "../common/rdx_input.h"
#include "../common/rdx_menu_ipc.h"
#include "../common/rdx_hotkey.h"

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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sched.h>
#include <dirent.h>
#include <pwd.h>
#include <spawn.h>

extern char **environ;

#define WR_SEND  0x100u
#define WR_END   0x101u
#define SIGNAL_EVERY 64   /* signal one send in 64: fewer completions, fewer wake-ups */
#define WR_RECV  0x200u
#define NRECV    64
#define RING     65536u            /* per-seq bookkeeping, power of two */

struct src_dev {
    int fd;
    int probe, skip_report;
    struct rdx_input_pkt pkt;      /* report being assembled */
    uint8_t down[RDX_BITS(KEY_CNT)];
};

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static struct ibv_qp *g_qp;
static uint32_t g_seq;
static uint64_t *g_t_send;
static int32_t *g_probe_of;
static long g_sent;

static int send_pkt(struct rdx_input_pkt *p, uint16_t dev, int32_t probe_val)
{
    p->seq = g_seq;
    p->dev = dev;
    struct ibv_sge sge = {.addr = (uintptr_t)p, .length = RDX_INPUT_PKT_LEN(p->n), .lkey = 0};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    int last = dev == 0xFFFF;
    wr.wr_id = last ? WR_END : WR_SEND;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge; wr.num_sge = 1;
    wr.send_flags = IBV_SEND_INLINE |
                    ((last || g_seq % SIGNAL_EVERY == SIGNAL_EVERY - 1) ? IBV_SEND_SIGNALED : 0);
    g_t_send[g_seq & (RING - 1)] = rdx_now_ns();
    g_probe_of[g_seq & (RING - 1)] = probe_val;
    if (ibv_post_send(g_qp, &wr, &bad)) { perror("post_send"); return -1; }
    g_seq++;
    g_sent++;
    p->n = 0;
    return 0;
}

/* in-stream menu socket of rdx-clientd: -1 = no menu */
static int g_menu = -1, g_menu_open;
static int g_syskeys = -1;                /* the local uinput device for the system keys */

/* the system keys' own local device (header): this computer's logind and desktop act on it as on
 * any keyboard's power / sleep key. Not "rdx: ...": it is no stream device. */
static int syskeys_create(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror("[tx] system keys: /dev/uinput"); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (unsigned k = 0; k < KEY_CNT; k++)
        if (rdx_input_system_key(k)) ioctl(fd, UI_SET_KEYBIT, k);
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    snprintf(us.name, sizeof(us.name), "rdma-display system keys (local)");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("[tx] system keys: uinput");
        close(fd);
        return -1;
    }
    return fd;
}

static void syskey_local(uint16_t code, int32_t value)
{
    if (g_syskeys < 0) return;
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY;
    ev[0].code = code;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    if (write(g_syskeys, ev, sizeof(ev)) < 0) perror("[tx] system key");
}

static int menu_connect(const char *path)
{
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "[tx] no in-stream menu: %s: %s\n", path, strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    struct rdx_menu_msg m = {RDX_MENU_HELLO, RDX_MENU_ROLE_INPUT, 0, 0, 0, 0};
    send(s, &m, sizeof(m), MSG_NOSIGNAL);
    return s;
}

static void menu_send(uint32_t type, int a, int b)
{
    struct rdx_menu_msg m = {type, a, b, 0, 0, 0};
    if (g_menu >= 0 && send(g_menu, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
        close(g_menu);
        g_menu = -1;
        g_menu_open = 0;
    }
}

static void menu_read(void)
{
    struct rdx_menu_msg m;
    ssize_t n;
    while (g_menu >= 0 && (n = recv(g_menu, &m, sizeof(m), MSG_DONTWAIT)) != 0) {
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            break;
        }
        if (n == (ssize_t)sizeof(m) && m.type == RDX_MENU_STATE) g_menu_open = m.a != 0;
    }
    if (g_menu >= 0) { close(g_menu); g_menu = -1; }     /* clientd is gone: forward again */
    g_menu_open = 0;
}

static int menu_hotkey(const struct src_dev *d, const struct input_event *e)
{
    return e->type == EV_KEY && e->value == 1 && e->code == KEY_M &&
           (rdx_test_bit(d->down, KEY_LEFTCTRL) || rdx_test_bit(d->down, KEY_RIGHTCTRL)) &&
           (rdx_test_bit(d->down, KEY_LEFTALT) || rdx_test_bit(d->down, KEY_RIGHTALT)) &&
           (rdx_test_bit(d->down, KEY_LEFTSHIFT) || rdx_test_bit(d->down, KEY_RIGHTSHIFT));
}

/* Volume keys act here, on B, where the stream's sound plays: during a stream the desktop
 * session is in the background (another VT) and ignores them, and forwarding them to A would
 * turn the host's speakers instead. wpctl runs as the owner of the desktop PipeWire. */
static int is_volume_key(uint16_t code)
{
    return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN || code == KEY_MUTE;
}

static void local_volume(uint16_t code)
{
    static char user[64], rt[64];
    if (!user[0]) {                       /* first /run/user/<uid>/pipewire-0 with uid >= 1000 */
        DIR *dir = opendir("/run/user");
        struct dirent *de;
        while (dir && (de = readdir(dir))) {
            char sock[300];
            long uid = strtol(de->d_name, NULL, 10);
            snprintf(sock, sizeof(sock), "/run/user/%s/pipewire-0", de->d_name);
            struct passwd *pw = uid >= 1000 ? getpwuid((uid_t)uid) : NULL;
            if (pw && !access(sock, F_OK)) {
                snprintf(user, sizeof(user), "%s", pw->pw_name);
                snprintf(rt, sizeof(rt), "XDG_RUNTIME_DIR=/run/user/%ld", uid);
                break;
            }
        }
        if (dir) closedir(dir);
        if (!user[0]) { fprintf(stderr, "[tx] volume key: no desktop PipeWire found\n"); return; }
    }
    char *const up[] = {"runuser", "-u", user, "--", "env", rt, "wpctl", "set-volume", "-l", "1.0",
                        "@DEFAULT_AUDIO_SINK@", "5%+", NULL};
    char *const down[] = {"runuser", "-u", user, "--", "env", rt, "wpctl", "set-volume",
                          "@DEFAULT_AUDIO_SINK@", "5%-", NULL};
    char *const mute[] = {"runuser", "-u", user, "--", "env", rt, "wpctl", "set-mute",
                          "@DEFAULT_AUDIO_SINK@", "toggle", NULL};
    pid_t pid;
    char *const *argv = code == KEY_VOLUMEUP ? up : code == KEY_VOLUMEDOWN ? down : mute;
    if (posix_spawnp(&pid, "runuser", NULL, NULL, argv, environ)) perror("[tx] volume key: runuser");
}

/* the desktop hotkey: keyboard and mouse to the desktop and back (header) */
static struct rdx_hotkey g_hk[4];
static int g_nhk, g_free, g_pending_grab;

static int held_bit(void *user, uint16_t code) { return rdx_test_bit((const uint8_t *)user, code); }

/* no key of any device is physically down (the kernel's state, not ours) */
static int all_keys_up(const struct src_dev *devs, int ndev)
{
    for (int i = 0; i < ndev; i++) {
        uint8_t k[RDX_BITS(KEY_CNT)];
        memset(k, 0, sizeof(k));
        if (devs[i].probe || ioctl(devs[i].fd, EVIOCGKEY(sizeof(k)), k) < 0) continue;
        for (size_t j = 0; j < sizeof(k); j++) if (k[j]) return 0;
    }
    return 1;
}

/* release everything still held on device d (on A), then SYN */
static int release_all(struct src_dev *d, uint16_t id)
{
    d->pkt.n = 0;
    for (unsigned k = 0; k < KEY_CNT; k++) {
        if (!rdx_test_bit(d->down, k)) continue;
        d->pkt.ev[d->pkt.n++] = (struct rdx_input_ev){EV_KEY, (uint16_t)k, 0};
        if (d->pkt.n == RDX_INPUT_MAX_EV - 1) {
            d->pkt.ev[d->pkt.n++] = (struct rdx_input_ev){EV_SYN, SYN_REPORT, 0};
            if (send_pkt(&d->pkt, id, -1)) return -1;
        }
    }
    if (d->pkt.n) {
        d->pkt.ev[d->pkt.n++] = (struct rdx_input_ev){EV_SYN, SYN_REPORT, 0};
        if (send_pkt(&d->pkt, id, -1)) return -1;
    }
    memset(d->down, 0, sizeof(d->down));
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer_ip = NULL, *inputs[RDX_INPUT_MAX_DEV], *menu_sock = NULL;
    int port = 18660, mtu = 4096, grab = 1, busy = 0, ninputs = 0, spin_us = 0, pm_qos_us = 50;
    double duration_s = 0;
    int silent = 0, rt_prio = 10, comp_vector = -1;
    long probe_count = 0;
    double probe_rate = 1000;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"input", 1, 0, 'i'}, {"no-grab", 0, 0, 'G'}, {"probe-count", 1, 0, 'c'},
        {"probe-rate", 1, 0, 'r'}, {"busy-poll", 0, 0, 'B'}, {"spin-us", 1, 0, 's'},
        {"pm-qos-us", 1, 0, 'q'}, {"duration-s", 1, 0, 'D'}, {"probe-silent", 0, 0, 'S'},
        {"rt-prio", 1, 0, 'R'}, {"comp-vector", 1, 0, 'V'}, {"menu-sock", 1, 0, 'U'},
        {"desktop-hotkey", 1, 0, 'H'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'p': peer_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'i': if (ninputs < RDX_INPUT_MAX_DEV - 1) inputs[ninputs++] = optarg; break;
        case 'G': grab = 0; break;
        case 'c': probe_count = atol(optarg); break;
        case 'r': probe_rate = atof(optarg); break;
        case 'B': busy = 1; break;
        case 's': spin_us = atoi(optarg); break;
        case 'q': pm_qos_us = atoi(optarg); break;
        case 'D': duration_s = atof(optarg); break;
        case 'S': silent = 1; break;
        case 'R': rt_prio = atoi(optarg); break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'U': menu_sock = optarg; break;
        case 'H':
            if (g_nhk < 4 && !rdx_hotkey_parse(optarg, &g_hk[g_nhk])) g_nhk++;
            else fprintf(stderr, "[tx] desktop hotkey \"%s\" not understood (e.g. ctrl+alt+h)\n", optarg);
            break;
        default:  fprintf(stderr, "see the header of rdx_input_tx.c for options\n"); return 2;
        }
    }
    if (!peer_ip || (!ninputs && !probe_count && duration_s <= 0)) {
        fprintf(stderr, "need --peer and --input, --probe-count or --duration-s\n"); return 2;
    }
    rdx_stop_signals(on_signal);
    signal(SIGCHLD, SIG_IGN);             /* volume-key helpers reap themselves */

    /* devices: real inputs first, then the probe */
    struct src_dev devs[RDX_INPUT_MAX_DEV];
    struct rdx_input_dev desc[RDX_INPUT_MAX_DEV];
    int ndev = 0, probe_ufd = -1;
    memset(devs, 0, sizeof(devs));
    for (int i = 0; i < ninputs; i++) {
        int fd = open(inputs[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { perror(inputs[i]); return 1; }
        if (rdx_evdev_describe(fd, (uint32_t)ndev, &desc[ndev])) { perror("describe"); return 1; }
        devs[ndev].fd = fd;
        printf("[tx] input %d: %s (%s)\n", ndev, inputs[i], desc[ndev].name);
        ndev++;
    }
    if (probe_count || !ninputs) {               /* idle run (--duration-s only) still needs one device */
        struct rdx_input_dev pd;
        memset(&pd, 0, sizeof(pd));
        strcpy(pd.name, silent ? RDX_PROBE_SILENT : RDX_PROBE_NAME);
        pd.iid.bustype = BUS_VIRTUAL;
        pd.ev[EV_SYN / 8] |= 1 << (EV_SYN % 8);
        pd.ev[EV_MSC / 8] |= 1 << (EV_MSC % 8);
        pd.msc[MSC_SCAN / 8] |= 1 << (MSC_SCAN % 8);
        probe_ufd = rdx_uinput_create(&pd, "");
        char path[64];
        if (probe_ufd < 0 || rdx_uinput_event_path(probe_ufd, path, sizeof(path))) return 1;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { perror(path); return 1; }
        if (rdx_evdev_describe(fd, (uint32_t)ndev, &desc[ndev])) { perror("describe"); return 1; }
        devs[ndev].fd = fd;
        devs[ndev].probe = 1;
        printf("[tx] probe device %s (MSC_SCAN only), %ld probes at %.0f Hz\n", path, probe_count, probe_rate);
        ndev++;
    }
    for (int i = 0; i < ndev; i++) {
        int clk = CLOCK_MONOTONIC;
        ioctl(devs[i].fd, EVIOCSCLOCKID, &clk);
    }

    char lip[64] = "";
    rdx_local_ip_for(peer_ip, lip, sizeof(lip));         /* the seat's link IP towards the peer */
    struct ibv_context *ctx = rdx_open_dev_ip(dev, lip);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    /* completion channel: without busy poll the CQ is one more fd in the poll set */
    struct ibv_comp_channel *ch = pd ? ibv_create_comp_channel(ctx) : NULL;
    if (ch) fcntl(ch->fd, F_SETFL, fcntl(ch->fd, F_GETFL) | O_NONBLOCK);
    comp_vector = rdx_pick_comp_vector(ctx, comp_vector);
    struct ibv_cq *cq = ch ? ibv_create_cq(ctx, 512, NULL, ch, comp_vector) : NULL;
    g_qp = cq ? rdx_create_qp_inline(pd, cq, 256, RDX_INPUT_INLINE) : NULL;
    if (!g_qp || rdx_qp_to_init(g_qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(g_qp, WR_RECV)) { perror("post_recv"); return 1; }

    struct rdx_input_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_INPUT_MAGIC;
    hello.ndev = (uint32_t)ndev;
    int gidx = rdx_find_gid_index_ip(ctx, lip, hello.conn.gid);
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    hello.conn.qpn = g_qp->qp_num;
    hello.conn.psn = 0x4567;
    int sock = rdx_tcp_connect(peer_ip, port);
    if (sock < 0) return 1;
    struct rdx_conn peer;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) ||
        rdx_io_all(sock, desc, sizeof(desc[0]) * (size_t)ndev, 1) ||
        rdx_io_all(sock, &peer, sizeof(peer), 0)) { fprintf(stderr, "bootstrap failed\n"); return 1; }
    if (rdx_qp_to_rtr_rts(g_qp, &peer, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }

    /* grab only once the path to A stands, so a failed start never steals the keyboard */
    if (grab)
        for (int i = 0; i < ndev; i++)
            if (ioctl(devs[i].fd, EVIOCGRAB, (void *)1)) perror("EVIOCGRAB");
    if (grab) g_syskeys = syskeys_create();           /* shared devices: the desktop sees them anyway */
    int qos_fd = rdx_pm_qos(pm_qos_us);
    int rt_ok = !busy && !rdx_set_rt_prio(rt_prio);   /* a spinning SCHED_FIFO loop would starve the CPU */
    printf("[tx] connected to %s, %d device(s), %s, %s, spin %d us, PM QoS %s, SCHED_FIFO %s\n", peer_ip,
           ndev, grab ? "grabbed" : "shared", busy ? "busy poll" : "event driven", spin_us,
           qos_fd >= 0 ? "on" : "off", rt_ok ? "on" : "off");
    printf("[tx] completion vector %d of %d\n", comp_vector, ctx->num_comp_vectors);
    if (ninputs) printf("[tx] release: LeftCtrl+LeftAlt+Esc\n");
    if (ninputs && grab && g_nhk) printf("[tx] desktop hotkey: %d combination(s), keyboard and mouse to the desktop and back\n", g_nhk);
    if (ninputs && menu_sock && (g_menu = menu_connect(menu_sock)) >= 0)
        printf("[tx] in-stream menu: Ctrl+Alt+Shift+M (%s)\n", menu_sock);
    fflush(stdout);

    g_t_send = calloc(RING, sizeof(*g_t_send));
    g_probe_of = calloc(RING, sizeof(*g_probe_of));
    /* probe write times live in shared memory: the writer is a child process */
    uint64_t *t_write = mmap(NULL, sizeof(uint64_t) * (size_t)(probe_count ? probe_count : 1),
                             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (t_write == MAP_FAILED) { perror("mmap"); return 1; }
    double *cap = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    double *cap_k = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    double *rtt = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    double *inj = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    double *tot = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    double *cap_of = calloc((size_t)(probe_count ? probe_count : 1), sizeof(double));
    long probes_acked = 0, nprobe = 0, acks = 0, dropped_reports = 0;
    uint64_t period = probe_rate > 0 ? (uint64_t)(1e9 / probe_rate) : 1000000;
    int probes_done = 0;
    uint64_t t_probes_done = 0;
    struct pollfd pfd[RDX_INPUT_MAX_DEV + 2];   /* + completion channel + menu socket */
    for (int i = 0; i < ndev; i++) pfd[i] = (struct pollfd){.fd = devs[i].fd, .events = POLLIN};
    pfd[ndev] = (struct pollfd){.fd = busy ? -1 : ch->fd, .events = POLLIN};
    pfd[ndev + 1] = (struct pollfd){.fd = g_menu, .events = POLLIN};
    if (!busy) ibv_req_notify_cq(cq, 0);
    struct ibv_wc wc[32];
    int combo = 0;
    /* The probe writer runs as a child process with its own CPU account, so the CPU of
     * this process is the plain path a real device produces: evdev wake, read, send.
     * It ends with an MSC_SCAN -1 marker that is not forwarded. */
    pid_t writer = -1;
    if (probe_count) {
        writer = fork();
        if (writer < 0) { perror("fork"); return 1; }
        if (writer == 0) {
            struct sched_param sp0 = {.sched_priority = 0};
            sched_setscheduler(0, SCHED_OTHER, &sp0);   /* harness: the writer stands for the kernel */
            struct timespec next;
            clock_gettime(CLOCK_MONOTONIC, &next);
            next.tv_sec += 1;                           /* let the parent reach ppoll first */
            for (long i = 0; i <= probe_count; i++) {
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
                struct input_event pe[2];
                memset(pe, 0, sizeof(pe));
                pe[0].type = EV_MSC; pe[0].code = MSC_SCAN; pe[0].value = i < probe_count ? (int32_t)i : -1;
                pe[1].type = EV_SYN; pe[1].code = SYN_REPORT;
                if (i < probe_count) t_write[i] = rdx_now_ns();
                if (write(probe_ufd, pe, sizeof(pe)) != (ssize_t)sizeof(pe)) _exit(1);
                next.tv_nsec += (long)period;
                while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
            }
            _exit(0);
        }
    }
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns(), t_act = 0;
    long slow = 0, slow_first = -1;
    uint64_t t_end = duration_s > 0 ? t0_wall + (uint64_t)(duration_s * 1e9) : 0;

    while (!g_stop && !combo) {
        uint64_t now = rdx_now_ns();
        if (t_end && now >= t_end && (!probe_count || probes_done)) break;
        if (probe_count && probes_done && !ninputs &&
            (silent || probes_acked == probe_count || now - t_probes_done > 2000000000ull)) break;

        /* busy or inside the spin window: never sleep. Otherwise block until an input
         * event, a completion, or the next deadline (probe, probe drain, duration). */
        struct timespec ts = {0, 0}, *tsp = &ts;
        int spinning = busy || (spin_us > 0 && t_act && now - t_act < (uint64_t)spin_us * 1000ull);
        if (!spinning) {
            uint64_t deadline = 0;
            if (probe_count && probes_done && !ninputs) deadline = t_probes_done + 2000000000ull;
            if (t_end && (!deadline || t_end < deadline)) deadline = t_end;
            if (deadline) {
                uint64_t wait = deadline > now ? deadline - now : 0;
                ts.tv_sec = (time_t)(wait / 1000000000ull);
                ts.tv_nsec = (long)(wait % 1000000000ull);
            } else {
                tsp = NULL;                       /* nothing scheduled: wait for an event */
            }
        }
        pfd[ndev + 1].fd = g_menu;
        int pr = ppoll(pfd, (nfds_t)ndev + 2, tsp, NULL);
        if (pr > 0 && pfd[ndev + 1].revents) menu_read();
        if (pr > 0) t_act = rdx_now_ns();
        if (!busy && pr > 0 && (pfd[ndev].revents & POLLIN)) {
            struct ibv_cq *ecq;
            void *ectx;
            if (!ibv_get_cq_event(ch, &ecq, &ectx)) ibv_ack_cq_events(ecq, 1);
            ibv_req_notify_cq(cq, 0);       /* re-arm before draining below */
        }
        if (pr > 0) {
            for (int i = 0; i < ndev && !combo; i++) {
                if (!(pfd[i].revents & POLLIN)) continue;
                struct input_event ev[64];
                ssize_t r = read(devs[i].fd, ev, sizeof(ev));
                if (r <= 0) continue;
                uint64_t t_read = rdx_now_ns();
                struct src_dev *d = &devs[i];
                for (size_t k = 0; k < (size_t)r / sizeof(ev[0]) && !combo; k++) {
                    struct input_event *e = &ev[k];
                    if (e->type == EV_SYN && e->code == SYN_DROPPED) { dropped_reports++; d->pkt.n = 0; continue; }
                    if (e->type == EV_KEY && e->code < KEY_CNT) {
                        if (e->value) d->down[e->code / 8] |= (uint8_t)(1 << (e->code % 8));
                        else d->down[e->code / 8] &= (uint8_t)~(1 << (e->code % 8));
                        if (e->value == 1 && e->code == KEY_ESC && !g_free && rdx_test_bit(d->down, KEY_LEFTCTRL) &&
                            rdx_test_bit(d->down, KEY_LEFTALT)) {
                            d->down[KEY_ESC / 8] &= (uint8_t)~(1 << (KEY_ESC % 8));
                            combo = 1;
                            break;
                        }
                    }
                    if (grab && g_nhk && !d->probe && e->type == EV_KEY && e->value == 1 &&
                        rdx_hotkey_hit(g_hk, g_nhk, e->code, rdx_hotkey_mods(held_bit, d->down))) {
                        if (!g_free) {                /* to the desktop: nothing stays held on A */
                            for (int j = 0; j < ndev; j++)
                                if (!devs[j].probe && release_all(&devs[j], (uint16_t)j)) return 1;
                            for (int j = 0; j < ndev; j++) ioctl(devs[j].fd, EVIOCGRAB, (void *)0);
                            g_free = 1;
                            g_pending_grab = 0;
                            menu_send(RDX_MENU_DESKTOP, 1, 0);
                            printf("[tx] keyboard and mouse: desktop\n");
                            fflush(stdout);
                        } else {
                            g_pending_grab = 1;       /* back once every key is up */
                        }
                        d->pkt.n = 0;
                        d->skip_report = 1;
                        continue;
                    }
                    if (g_free && !d->probe) {        /* the desktop has them: read, never forward */
                        d->pkt.n = 0;
                        continue;
                    }
                    if (e->type == EV_KEY && rdx_input_system_key(e->code) && !d->probe) {
                        syskey_local(e->code, e->value);   /* never to A (header), this computer's own */
                        continue;
                    }
                    if (e->type == EV_KEY && e->code == KEY_MICMUTE && !d->probe) {
                        /* the mic-mute key mutes this computer's microphone sender (rdx-clientd,
                         * like the volume keys it stays here), never forwarded */
                        if (e->value == 1) menu_send(RDX_MENU_MICMUTE, 0, 0);
                        continue;
                    }
                    if (g_menu >= 0 && menu_hotkey(d, e)) {
                        if (!g_menu_open) {           /* nothing stays pressed on A while the menu is up */
                            for (int j = 0; j < ndev; j++)
                                if (!devs[j].probe && release_all(&devs[j], (uint16_t)j)) return 1;
                            g_menu_open = 1;
                        }
                        menu_send(RDX_MENU_TOGGLE, 0, 0);
                        d->pkt.n = 0;
                        d->skip_report = 1;            /* the rest of this report stays local */
                        continue;
                    }
                    if (g_menu_open && !d->probe) {    /* the menu has the keyboard; mouse is swallowed */
                        if (e->type == EV_KEY && e->value) menu_send(RDX_MENU_KEY, e->code, e->value);
                        if (e->type == EV_SYN && e->code == SYN_REPORT) d->skip_report = 0;
                        d->pkt.n = 0;
                        continue;
                    }
                    if (d->probe && e->type == EV_MSC && e->code == MSC_SCAN && e->value == -1) {
                        probes_done = 1;                /* end marker: drop this report */
                        t_probes_done = t_read;
                        d->pkt.n = 0;
                        d->skip_report = 1;
                        continue;
                    }
                    if (d->skip_report) {
                        if (e->type == EV_SYN && e->code == SYN_REPORT) d->skip_report = 0;
                        continue;
                    }
                    if (e->type == EV_KEY && is_volume_key(e->code) && !d->probe) {
                        if (e->value) local_volume(e->code);    /* press and autorepeat */
                        continue;                               /* never forwarded */
                    }
                    int32_t pv = -1;
                    if (d->probe && e->type == EV_MSC && e->code == MSC_SCAN) {
                        pv = e->value;
                        if (pv >= 0 && pv < probe_count && nprobe < probe_count) {
                            cap_of[pv] = (t_read - t_write[pv]) / 1e6;
                            cap_k[nprobe] = (t_read - ((uint64_t)e->input_event_sec * 1000000000ull +
                                                       (uint64_t)e->input_event_usec * 1000ull)) / 1e6;
                            nprobe++;
                        }
                    }
                    d->pkt.ev[d->pkt.n++] = (struct rdx_input_ev){e->type, e->code, e->value};
                    if ((e->type == EV_SYN && e->code == SYN_REPORT) || d->pkt.n == RDX_INPUT_MAX_EV) {
                        int32_t probe_val = -1;
                        for (int q = 0; q < d->pkt.n; q++)
                            if (d->probe && d->pkt.ev[q].type == EV_MSC && d->pkt.ev[q].code == MSC_SCAN)
                                probe_val = d->pkt.ev[q].value;
                        if (send_pkt(&d->pkt, (uint16_t)i, probe_val)) return 1;
                    }
                }
            }
        }

        if (g_pending_grab && all_keys_up(devs, ndev)) {      /* back to the stream */
            for (int j = 0; j < ndev; j++) {
                if (!devs[j].probe && ioctl(devs[j].fd, EVIOCGRAB, (void *)1)) perror("EVIOCGRAB");
                memset(devs[j].down, 0, sizeof(devs[j].down));
                devs[j].pkt.n = 0;
                devs[j].skip_report = 1;              /* start with the next whole report */
            }
            g_free = g_pending_grab = 0;
            menu_send(RDX_MENU_DESKTOP, 0, 0);
            printf("[tx] keyboard and mouse: stream\n");
            fflush(stdout);
        }
        int nwc = ibv_poll_cq(cq, 32, wc);
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }
        if (nwc > 0) t_act = rdx_now_ns();
        for (int k = 0; k < nwc; k++) {
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[k].status),
                        (unsigned long long)wc[k].wr_id);
                return 1;
            }
            if (wc[k].opcode != IBV_WC_RECV) continue;
            uint64_t t_ack = rdx_now_ns();
            uint32_t imm = ntohl(wc[k].imm_data);
            if (rdx_post_zero_recv(g_qp, WR_RECV)) { perror("repost"); return 1; }
            acks++;
            uint32_t s = rdx_ack_seq(imm);
            /* the 20-bit seq in the ack maps back to the ring slot of the full seq */
            uint32_t full = (g_seq & ~0xFFFFFu) | s;
            if (full >= g_seq) full -= 0x100000u;      /* acked seqs are always < g_seq */
            int32_t pv = g_probe_of[full & (RING - 1)];
            if (pv >= 0 && pv < probe_count && probes_acked < probe_count) {
                double r_ms = (t_ack - g_t_send[full & (RING - 1)]) / 1e6;
                rtt[probes_acked] = r_ms;
                if (r_ms > 1.0) { if (slow_first < 0) slow_first = probes_acked; slow++; }
                inj[probes_acked] = rdx_ack_inj_us(imm) == RDX_ACK_NO_INJ ? 0 : rdx_ack_inj_us(imm) / 1e3;
                cap[probes_acked] = cap_of[pv];
                tot[probes_acked] = cap_of[pv] + r_ms;
                probes_acked++;
            }
        }
    }

    /* release whatever is held, ungrab, then tell rx we are done */
    for (int i = 0; i < ndev; i++)
        if (!devs[i].probe && release_all(&devs[i], (uint16_t)i)) return 1;
    if (grab && !g_free)
        for (int i = 0; i < ndev; i++) ioctl(devs[i].fd, EVIOCGRAB, (void *)0);
    struct rdx_input_pkt endp = {.n = 0};
    if (send_pkt(&endp, 0xFFFF, -1)) return 1;
    /* wait (not spin) for the END completion, at most 200 ms */
    for (uint64_t t_fin = rdx_now_ns(), got = 0; !got && rdx_now_ns() - t_fin < 200000000ull;) {
        int nwc = ibv_poll_cq(cq, 32, wc);
        if (nwc < 0) break;
        for (int k = 0; k < nwc; k++) if (wc[k].wr_id == WR_END) got = 1;
        if (!nwc) usleep(200);
    }

    rdx_cpu_report("tx", t0_wall, t0_cpu);
    printf("\n[tx] %s, %ld reports sent, %ld acks, %ld SYN_DROPPED\n",
           combo ? "released by LeftCtrl+LeftAlt+Esc" : "done", g_sent, acks, dropped_reports);
    if (probe_count) {
        printf("[tx] probes: received %ld, acked %ld (rate %.0f Hz, writer = child process)\n", nprobe,
               probes_acked, probe_rate);
        rdx_report("B capture (write->read)", cap, (size_t)probes_acked);
        rdx_report("B capture (kernel ts)", cap_k, (size_t)nprobe);
        printf("[tx] round trips > 1 ms: %ld (first at probe %ld)\n", slow, slow_first);
        rdx_report("round send->ack", rtt, (size_t)probes_acked);
        rdx_report("A inject->evdev", inj, (size_t)probes_acked);
        rdx_report("B write -> ack (total)", tot, (size_t)probes_acked);
    }
    if (writer > 0) { kill(writer, SIGTERM); waitpid(writer, NULL, 0); }
    if (probe_ufd >= 0) { ioctl(probe_ufd, UI_DEV_DESTROY); close(probe_ufd); }
    if (qos_fd >= 0) close(qos_fd);
    close(sock);
    /* exit code RDX_EXIT_RELEASED tells a supervisor that the user asked to leave
     * (e.g. stop the KMS sink and return to the desktop VT) */
    return combo ? RDX_EXIT_RELEASED : 0;
}
