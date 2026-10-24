/* SPDX-License-Identifier: MIT
 *
 * P4 input sender of a WINDOW client (rdx-clientd "sink": "window"): the keyboard and mouse of the
 * window rdx_sink_win shows the stream in, and nothing else. It selects the window's input events
 * on the X server as a second client of that window (KeyPress, KeyRelease, ButtonPress,
 * ButtonRelease, PointerMotion, Focus, Enter, Structure; the sink itself selects none of the
 * exclusive ones), translates them like the --local window does (p0b/rdx_xwin.h: keys by keycode,
 * buttons incl. the side buttons, wheel steps, relative motion in the frame's pixels, F11 stays
 * local) and sends every report as one inline RDMA SEND to the host's rdx_input_rx, the same wire
 * format as rdx_input_tx (common/rdx_input.h). The two devices it announces are the local window's
 * (gs/rdx_localinput.h rdx_localinput_desc), named "window keyboard/mouse (<this host>)"; on the host
 * they become "rdx: window ..." uinput devices, which gamescope's rdx backend takes.
 * No device of this computer is opened or grabbed: other windows keep their keyboard and mouse.
 * Focus loss lets go of every key and button on the host; the window going away, SIGTERM or SIGINT
 * do the same and end the input session (END).
 *
 * The pointer, two modes (docs/WINDOW-CLIENT.md):
 *   absolute  a third device "window pointer" (ABS_X/ABS_Y over the frame, 0..w-1 / 0..h-1, buttons,
 *             wheel): the app's pointer is where the local one is over the frame (letterboxed like
 *             the sink draws it). For desktop apps and launchers. The local cursor is hidden over the
 *             window while the stream shows the host's cursor (the menu's "Cursor"); when the host's
 *             cursor travels next to the picture, the sink makes it the window's cursor and the
 *             window's cursor is left alone here (clientd: POINTER b = 2).
 *   relative  the relative mouse; a click into the window captures the pointer (X pointer grab
 *             confined to the window, invisible cursor, warped back to the centre after every
 *             motion: unbounded motion for mouse look); Ctrl+Alt+Shift+Z, focus loss and the menu
 *             let it go.
 * Which one: rdx-clientd decides (RDX_MENU_POINTER): the setting "pointer" auto | absolute |
 * relative; auto = relative when the app hides its cursor and locks the pointer (gamescope's own
 * rule, sent by the seat in the frame header) or for a mirrored desktop, absolute otherwise.
 * --menu-sock PATH (from rdx-clientd): Ctrl+Alt+Shift+M opens the in-stream menu as on the KMS
 * client (everything held is let go on the host first; keys then go to the menu, the mouse is
 * swallowed), the mic-mute key mutes the microphone sender; both stay local.
 *
 *   rdx_input_win --peer 10.0.1.12 --port 18860 --window 0x4a00002 [--frame 1920x1080]
 *                 [--dev auto] [--mtu 4096] [--menu-sock /run/rdx/menu-window.sock]
 *                 [--pointer absolute|relative]
 */
#define _GNU_SOURCE
#include "../common/rdx_input.h"
#include "../common/rdx_menu_ipc.h"
#include "../common/rdx_hotkey.h"
#include "../gs/rdx_localinput.h"
#include "../p0b/rdx_xwin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WR_SEND 0x100u
#define WR_END  0x101u
#define WR_RECV 0x200u
#define NRECV   64
#define SIGNAL_EVERY 64

enum { DEV_KBD = 0, DEV_MOUSE = 1, DEV_PTR = 2, NDEV = 3 };

static struct {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    uint32_t seq;
    long sent, acks, events;
    uint8_t down[NDEV][RDX_BITS(KEY_CNT)];
    Display *dpy;
    Window win;
    int win_w, win_h;
    unsigned fw, fh;
    int gone;
    /* in-stream menu (rdx-clientd), pointer mode */
    int menu, menu_open;               /* socket (-1 = none); the menu has the keyboard */
    int rel, stream_cursor;            /* relative + captured wanted; the stream shows the host's cursor (1),
                                        * the window's cursor is the host's, set by the sink (2) */
    int grabbed, focus, cx, cy, ax, ay;   /* pointer captured, window focused, its centre, last absolute */
    double fx, fy;                     /* sub-pixel remainder of relative motion */
    Cursor blank;
    struct rdx_xin *xi;
} S = {.menu = -1, .ax = -1, .ay = -1};

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static int x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadWindow) S.gone = 1;     /* the sink's window went away */
    return 0;
}

static int post(uint16_t dev, const struct rdx_input_ev *ev, int n)
{
    struct rdx_input_pkt p;
    memset(&p, 0, RDX_INPUT_PKT_LEN(0));
    p.seq = S.seq;
    p.dev = dev;
    p.n = (uint16_t)n;
    if (n) memcpy(p.ev, ev, (size_t)n * sizeof(ev[0]));
    struct ibv_sge sge = {.addr = (uintptr_t)&p, .length = RDX_INPUT_PKT_LEN(n), .lkey = 0};
    struct ibv_send_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    int last = dev == 0xFFFF;
    wr.wr_id = last ? WR_END : WR_SEND;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge; wr.num_sge = 1;
    wr.send_flags = IBV_SEND_INLINE | ((last || S.seq % SIGNAL_EVERY == SIGNAL_EVERY - 1) ? IBV_SEND_SIGNALED : 0);
    if (ibv_post_send(S.qp, &wr, &bad)) { perror("post_send"); return -1; }
    S.seq++;
    S.sent++;
    return 0;
}

/* one report: the event and its SYN_REPORT */
static void emit(int dev, int type, int code, int value)
{
    struct rdx_input_ev ev[2] = {{(uint16_t)type, (uint16_t)code, value}, {EV_SYN, SYN_REPORT, 0}};
    if (type == EV_KEY) {
        if (value) S.down[dev][code / 8] |= (uint8_t)(1u << (code % 8));
        else S.down[dev][code / 8] &= (uint8_t)~(1u << (code % 8));
    }
    S.events++;
    post((uint16_t)dev, ev, 2);
}

/* every key and button still down goes up on the host */
static void release_all(void)
{
    for (int d = 0; d < NDEV; d++)
        for (unsigned c = 0; c < KEY_CNT; c++)
            if (rdx_test_bit(S.down[d], c)) emit(d, EV_KEY, (int)c, 0);
}

/* the absolute pointer at window pixel (x, y): frame pixels over the letterboxed frame */
static void motion_abs(int x, int y)
{
    int vx, vy, vw, vh;
    rdx_letterbox(S.win_w, S.win_h, S.fw, S.fh, &vx, &vy, &vw, &vh);
    int fx = vw > 0 ? (int)((x - vx) * (double)S.fw / vw) : 0, fy = vh > 0 ? (int)((y - vy) * (double)S.fh / vh) : 0;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx > (int)S.fw - 1) fx = (int)S.fw - 1;
    if (fy > (int)S.fh - 1) fy = (int)S.fh - 1;
    if (fx == S.ax && fy == S.ay) return;
    S.ax = fx;
    S.ay = fy;
    struct rdx_input_ev ev[3] = {{EV_ABS, ABS_X, fx}, {EV_ABS, ABS_Y, fy}, {EV_SYN, SYN_REPORT, 0}};
    S.events++;
    post(DEV_PTR, ev, 3);
}

static void grab(void)
{
    if (S.grabbed || !S.rel || S.menu_open || !S.focus) return;
    unsigned mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    int r = XGrabPointer(S.dpy, S.win, False, mask, GrabModeAsync, GrabModeAsync, S.win, S.blank, CurrentTime);
    if (r != GrabSuccess) { fprintf(stderr, "[win-in] pointer grab refused (%d)\n", r); return; }
    S.grabbed = 1;
    S.cx = S.win_w / 2;
    S.cy = S.win_h / 2;
    S.fx = S.fy = 0;
    XWarpPointer(S.dpy, None, S.win, 0, 0, 0, 0, S.cx, S.cy);
    XFlush(S.dpy);
    printf("[win-in] mouse captured (relative; Ctrl+Alt+Shift+Z lets it go)\n");
    fflush(stdout);
}

static void ungrab(void)
{
    if (!S.grabbed) return;
    XUngrabPointer(S.dpy, CurrentTime);
    XFlush(S.dpy);
    S.grabbed = 0;
    printf("[win-in] mouse let go\n");
    fflush(stdout);
}

/* relative motion while captured: from the centre, then back there (unbounded, for mouse look) */
static void motion_rel(int x, int y)
{
    if (!S.grabbed || (x == S.cx && y == S.cy)) return;   /* our own warp */
    int vx, vy, vw, vh;
    rdx_letterbox(S.win_w, S.win_h, S.fw, S.fh, &vx, &vy, &vw, &vh);
    S.fx += (x - S.cx) * (vw > 0 ? (double)S.fw / vw : 1);
    S.fy += (y - S.cy) * (vh > 0 ? (double)S.fh / vh : 1);
    int dx = (int)S.fx, dy = (int)S.fy;
    S.fx -= dx;
    S.fy -= dy;
    struct rdx_input_ev ev[3];
    int n = 0;
    if (dx) ev[n++] = (struct rdx_input_ev){EV_REL, REL_X, dx};
    if (dy) ev[n++] = (struct rdx_input_ev){EV_REL, REL_Y, dy};
    if (n) {
        ev[n++] = (struct rdx_input_ev){EV_SYN, SYN_REPORT, 0};
        S.events++;
        post(DEV_MOUSE, ev, n);
    }
    XWarpPointer(S.dpy, None, S.win, 0, 0, 0, 0, S.cx, S.cy);
}

/* the local cursor over the window: hidden while the host's is in the stream (absolute); 2 = the sink shows
 * the host's cursor as the window's cursor (common/rdx_cursor.h): not ours to touch */
static void apply_cursor(void)
{
    if (S.stream_cursor == 2) return;
    if (!S.rel && S.stream_cursor) XDefineCursor(S.dpy, S.win, S.blank);
    else XUndefineCursor(S.dpy, S.win);
    XFlush(S.dpy);
}

/* the pointer mode clientd chose */
static void set_mode(int rel, int stream_cursor)
{
    if (rel != S.rel) {
        release_all();
        if (!rel) ungrab();
        S.rel = rel;
        S.ax = S.ay = -1;
        printf("[win-in] pointer %s\n", rel ? "relative (a click into the window captures it)" : "absolute");
        fflush(stdout);
        if (rel) {                          /* the pointer is in the focused window already: take it */
            Window r, c;
            int rx, ry, wx, wy;
            unsigned m;
            if (XQueryPointer(S.dpy, S.win, &r, &c, &rx, &ry, &wx, &wy, &m) && wx >= 0 && wy >= 0 && wx < S.win_w &&
                wy < S.win_h)
                grab();
        }
    }
    S.stream_cursor = stream_cursor;
    apply_cursor();
}

/* ---------------- in-stream menu (rdx-clientd) ---------------- */

static void menu_send(uint32_t type, int a, int b)
{
    struct rdx_menu_msg m = {type, a, b, 0, 0, 0};
    if (S.menu >= 0 && send(S.menu, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
        close(S.menu);
        S.menu = -1;
        S.menu_open = 0;
    }
}

static int menu_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "[win-in] no in-stream menu: %s: %s\n", path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    S.menu = fd;
    menu_send(RDX_MENU_HELLO, RDX_MENU_ROLE_INPUT, RDX_MENU_CAP_POINTER);
    return 0;
}

static void menu_read(void)
{
    struct rdx_menu_msg m;
    ssize_t n;
    while (S.menu >= 0 && (n = recv(S.menu, &m, sizeof(m), MSG_DONTWAIT)) != 0) {
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) return;
            break;
        }
        if (n != (ssize_t)sizeof(m)) continue;
        if (m.type == RDX_MENU_STATE) {
            S.menu_open = m.a != 0;
            if (S.menu_open) ungrab();
        } else if (m.type == RDX_MENU_POINTER) {
            set_mode(m.a != 0, m.b == 2 ? 2 : m.b != 0);     /* b = 2: the sink owns the window's cursor */
        }
    }
    if (S.menu >= 0) { close(S.menu); S.menu = -1; }      /* clientd is gone: forward again */
    S.menu_open = 0;
}

static int held(int code) { return code + 8 < 256 && (S.xi->kdown[(code + 8) / 8] >> ((code + 8) % 8)) & 1; }
static int held_cb(void *user, uint16_t code) { (void)user; return held(code); }
/* the desktop hotkey (--desktop-hotkey, common/rdx_hotkey.h): here it lets the captured mouse go / takes it */
static struct rdx_hotkey g_hk[2];
static int g_nhk;
static int ctrl_alt_shift(void)
{
    return (held(KEY_LEFTCTRL) || held(KEY_RIGHTCTRL)) && (held(KEY_LEFTALT) || held(KEY_RIGHTALT)) &&
           (held(KEY_LEFTSHIFT) || held(KEY_RIGHTSHIFT));
}

static void out(void *user, int kind, int code, int value)
{
    (void)user;
    int pdev = S.rel ? DEV_MOUSE : DEV_PTR;
    if (kind == RDX_LOCAL_IN_KEY) {
        if (code == KEY_MICMUTE) {          /* this computer's microphone, never forwarded */
            if (value) menu_send(RDX_MENU_MICMUTE, 0, 0);
            return;
        }
        if (value && code == KEY_M && S.menu >= 0 && ctrl_alt_shift()) {
            if (!S.menu_open) {             /* nothing stays pressed on the host while the menu is up */
                release_all();
                S.menu_open = 1;
                ungrab();
            }
            menu_send(RDX_MENU_TOGGLE, 0, 0);
            return;
        }
        if (value && S.rel && ((code == KEY_Z && ctrl_alt_shift()) ||
                               rdx_hotkey_hit(g_hk, g_nhk, (uint16_t)code, rdx_hotkey_mods(held_cb, NULL)))) {   /* let the captured mouse go / take it */
            if (S.grabbed) ungrab(); else grab();
            return;
        }
        if (S.menu_open) {                  /* the menu has the keyboard */
            if (value) menu_send(RDX_MENU_KEY, code, value);
            return;
        }
        emit(DEV_KBD, EV_KEY, code, value);
    } else if (kind == RDX_LOCAL_IN_BUTTON) {
        if (!S.menu_open) emit(pdev, EV_KEY, code, value);
    } else if (kind == RDX_LOCAL_IN_REL) {
        if (!S.menu_open && (code == REL_WHEEL || code == REL_HWHEEL)) emit(pdev, EV_REL, code, value);
    } else {
        release_all();
    }
}

static int drain_cq(void)
{
    struct ibv_wc wc[32];
    int n;
    while ((n = ibv_poll_cq(S.cq, 32, wc)) > 0)
        for (int k = 0; k < n; k++) {
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[win-in] WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[k].status),
                        (unsigned long long)wc[k].wr_id);
                return -1;
            }
            if (wc[k].opcode != IBV_WC_RECV) continue;   /* our signaled sends */
            S.acks++;
            if (rdx_post_zero_recv(S.qp, WR_RECV)) { perror("repost"); return -1; }
        }
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer = NULL;
    int port = 18660, mtu = 4096;
    unsigned long xid = 0;
    S.fw = 1920; S.fh = 1080;
    static const struct option opts[] = {
        {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"dev", 1, 0, 'd'}, {"mtu", 1, 0, 'm'},
        {"window", 1, 0, 'w'}, {"frame", 1, 0, 'f'}, {"menu-sock", 1, 0, 'U'}, {"pointer", 1, 0, 'T'}, {"desktop-hotkey", 1, 0, 'H'}, {0, 0, 0, 0}};
    const char *menu_sock = NULL;
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'p': peer = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'd': dev = optarg; break;
        case 'm': mtu = atoi(optarg); break;
        case 'w': xid = strtoul(optarg, NULL, 0); break;
        case 'f': if (sscanf(optarg, "%ux%u", &S.fw, &S.fh) != 2) { fprintf(stderr, "--frame WxH\n"); return 2; } break;
        case 'U': menu_sock = optarg; break;
        case 'T': S.rel = !strcmp(optarg, "relative"); break;
        case 'H': if (g_nhk < 2 && !rdx_hotkey_parse(optarg, &g_hk[g_nhk])) g_nhk++; break;
        default:  fprintf(stderr, "see the header of rdx_input_win.c for options\n"); return 2;
        }
    }
    if (!peer || !xid) { fprintf(stderr, "need --peer and --window\n"); return 2; }

    /* the window first: without it there is nothing to forward */
    rdx_x_desktop_env();
    XSetErrorHandler(x_error);
    S.dpy = XOpenDisplay(NULL);
    if (!S.dpy) { fprintf(stderr, "[win-in] XOpenDisplay failed (DISPLAY/XAUTHORITY?)\n"); return 1; }
    S.win = (Window)xid;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(S.dpy, S.win, &wa) || S.gone) { fprintf(stderr, "[win-in] no window 0x%lx\n", xid); return 1; }
    S.win_w = wa.width; S.win_h = wa.height;
    XSelectInput(S.dpy, S.win, RDX_XIN_MASK | StructureNotifyMask);
    {                                       /* an invisible cursor: captured pointer, host cursor in the stream */
        Pixmap pm = XCreatePixmap(S.dpy, S.win, 1, 1, 1);
        XColor c;
        memset(&c, 0, sizeof(c));
        S.blank = XCreatePixmapCursor(S.dpy, pm, pm, &c, &c, 0, 0);
        XFreePixmap(S.dpy, pm);
    }
    XSync(S.dpy, False);
    if (S.gone) { fprintf(stderr, "[win-in] window 0x%lx went away\n", xid); return 1; }

    /* the input session with rdx_input_rx, as rdx_input_tx opens it */
    char lip[64] = "", host[64] = "this computer";
    gethostname(host, sizeof(host));
    rdx_local_ip_for(peer, lip, sizeof(lip));
    struct ibv_context *ctx = rdx_open_dev_ip(dev, lip);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_comp_channel *ch = pd ? ibv_create_comp_channel(ctx) : NULL;
    if (ch) fcntl(ch->fd, F_SETFL, fcntl(ch->fd, F_GETFL) | O_NONBLOCK);
    S.cq = ch ? ibv_create_cq(ctx, 512, NULL, ch, 0) : NULL;
    S.qp = S.cq ? rdx_create_qp_inline(pd, S.cq, 256, RDX_INPUT_INLINE) : NULL;
    if (!S.qp || rdx_qp_to_init(S.qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(S.qp, WR_RECV)) { perror("post_recv"); return 1; }
    struct rdx_input_dev desc[NDEV];
    for (int i = 0; i < NDEV; i++) {
        rdx_localinput_desc(i != DEV_KBD, &desc[i]);
        desc[i].id = (uint32_t)i;
        snprintf(desc[i].name, sizeof(desc[i].name), "window %s (%.40s)",
                 i == DEV_MOUSE ? "mouse" : i == DEV_PTR ? "pointer" : "keyboard", host);
    }
    /* the absolute pointer: the mouse's buttons and wheel, ABS_X / ABS_Y over the frame instead of REL_X / REL_Y */
    desc[DEV_PTR].rel[REL_X / 8] &= (uint8_t)~(1u << (REL_X % 8));
    desc[DEV_PTR].rel[REL_Y / 8] &= (uint8_t)~(1u << (REL_Y % 8));
    desc[DEV_PTR].ev[EV_ABS / 8] |= (uint8_t)(1u << (EV_ABS % 8));
    desc[DEV_PTR].abs[ABS_X / 8] |= (uint8_t)(1u << (ABS_X % 8));
    desc[DEV_PTR].abs[ABS_Y / 8] |= (uint8_t)(1u << (ABS_Y % 8));
    desc[DEV_PTR].absinfo[ABS_X] = (struct input_absinfo){.minimum = 0, .maximum = (int)S.fw - 1, .resolution = 1};
    desc[DEV_PTR].absinfo[ABS_Y] = (struct input_absinfo){.minimum = 0, .maximum = (int)S.fh - 1, .resolution = 1};
    desc[DEV_PTR].iid.product = 0x0103;
    struct rdx_input_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_INPUT_MAGIC;
    hello.ndev = NDEV;
    int gidx = rdx_find_gid_index_ip(ctx, lip, hello.conn.gid);
    if (gidx < 0) return 1;
    hello.conn.qpn = S.qp->qp_num;
    hello.conn.psn = 0x5678;
    int sock = rdx_tcp_connect(peer, port);
    if (sock < 0) return 1;
    struct rdx_conn pc;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) || rdx_io_all(sock, desc, sizeof(desc), 1) ||
        rdx_io_all(sock, &pc, sizeof(pc), 0)) { fprintf(stderr, "[win-in] bootstrap failed\n"); return 1; }
    if (rdx_qp_to_rtr_rts(S.qp, &pc, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "[win-in] sync failed\n"); return 1; }
    ibv_req_notify_cq(S.cq, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    printf("[win-in] connected to %s:%d, window 0x%lx %dx%d, frames %ux%u, devices \"%s\", \"%s\", \"%s\" (nothing grabbed)\n",
           peer, port, xid, S.win_w, S.win_h, S.fw, S.fh, desc[0].name, desc[1].name, desc[2].name);
    fflush(stdout);
    if (menu_sock && !menu_connect(menu_sock)) printf("[win-in] in-stream menu: Ctrl+Alt+Shift+M (%s)\n", menu_sock);
    printf("[win-in] pointer %s\n", S.rel ? "relative (a click into the window captures it)" : "absolute");
    fflush(stdout);

    struct rdx_xin xi;
    memset(&xi, 0, sizeof(xi));
    S.xi = &xi;
    {                                       /* the window may have the focus already */
        Window fw;
        int rv;
        XGetInputFocus(S.dpy, &fw, &rv);
        S.focus = fw == S.win;
    }
    struct pollfd pf[3] = {{.fd = ConnectionNumber(S.dpy), .events = POLLIN}, {.fd = ch->fd, .events = POLLIN},
                           {.fd = S.menu, .events = POLLIN}};
    uint64_t t0 = rdx_now_ns();
    while (!g_stop && !S.gone) {
        while (XPending(S.dpy)) {
            XEvent ev;
            XNextEvent(S.dpy, &ev);
            if (ev.type == ConfigureNotify) {
                S.win_w = ev.xconfigure.width; S.win_h = ev.xconfigure.height;
                if (S.grabbed) { S.cx = S.win_w / 2; S.cy = S.win_h / 2; }
            }
            else if (ev.type == DestroyNotify || ev.type == UnmapNotify) {
                if (ev.type == DestroyNotify) S.gone = 1;
                else { ungrab(); release_all(); }
            }
            else if (ev.type == MotionNotify) {      /* by mode, not as rdx_xin's relative motion */
                if (S.menu_open) continue;
                if (S.rel) motion_rel(ev.xmotion.x, ev.xmotion.y);
                else motion_abs(ev.xmotion.x, ev.xmotion.y);
            }
            else if (ev.type == FocusIn) S.focus = 1;
            else if (ev.type == FocusOut) {
                S.focus = 0;
                ungrab();
                rdx_xin_event(&xi, &ev, S.win_w, S.win_h, S.fw, S.fh, out, NULL);
            }
            else if (ev.type == ButtonPress && S.rel && !S.grabbed && !S.menu_open) {
                S.focus = 1;                         /* the click that captures stays here */
                grab();
            }
            else if (ev.type == ButtonPress && !S.rel && !S.menu_open) {
                motion_abs(ev.xbutton.x, ev.xbutton.y);   /* the click lands where the pointer is */
                rdx_xin_event(&xi, &ev, S.win_w, S.win_h, S.fw, S.fh, out, NULL);
            }
            else rdx_xin_event(&xi, &ev, S.win_w, S.win_h, S.fw, S.fh, out, NULL);
        }
        if (S.gone) break;
        pf[2].fd = S.menu;
        if (poll(pf, 3, 1000) < 0 && errno != EINTR) break;
        if (pf[2].revents) menu_read();
        if (pf[1].revents & POLLIN) {
            struct ibv_cq *ecq; void *ectx;
            while (ibv_get_cq_event(ch, &ecq, &ectx) == 0) ibv_ack_cq_events(ecq, 1);
            ibv_req_notify_cq(S.cq, 0);
        }
        if (drain_cq()) return 1;
    }
    ungrab();
    release_all();
    post(0xFFFF, NULL, 0);
    for (uint64_t te = rdx_now_ns(); rdx_now_ns() - te < 200000000ull;) { if (drain_cq()) break; usleep(10000); }
    /* rdx_input_rx acks only its probe reports (measurement), so no acks are expected here */
    printf("[win-in] done after %.1f s: %ld input events, %ld reports sent (incl. END)%s\n", (rdx_now_ns() - t0) / 1e9,
           S.events, S.sent, S.gone ? " (the window went away)" : "");
    close(sock);
    return 0;
}
