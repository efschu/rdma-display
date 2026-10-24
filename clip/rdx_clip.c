/* SPDX-License-Identifier: GPL-2.0-or-later
 * (this program links xclip's selection library, third_party/xclip/xclib.c, GPL-2.0-or-later;
 *  the protocol part clip/rdx_clip_proto.c is MIT like the rest of rdma-display)
 *
 * rdx_clip: the clipboard between the client's desktop and the app in the stream, both ways.
 * One instance per side and session, the same program on both:
 *
 *   client desktop (Xorg / Xwayland)            seat (gamescope's Xwayland)
 *   rdx_clip --connect HOST --desktop  --TCP-->  rdx_clip --listen LINK_IP --peer CLIENT --x-of-pid GS_SRC
 *   (rdx-clientd, after the session.start reply)  (rdx-hostd, with the senders of a session)
 *
 * Each side is an ordinary X client of its display: it watches the owner of CLIPBOARD (and PRIMARY)
 * with XFixes (like clipnotify / gamescope itself), reads a new owner's text (UTF8_STRING, else
 * STRING) with xclip's xcout() and sends it; text from the peer it takes over as the owner of that
 * selection and serves it with xclip's xcin() (TARGETS, INCR for large texts). gamescope copies a
 * selection set on one of its Xwaylands to all of them (its own bridge, gamescope_set_selection), so
 * one Xwayland is enough for every app in it. Only text: UTF-8, at most --max-bytes (1 MiB); a
 * larger text is not sent (one log line). The text itself is never logged.
 * Echo loops end in the guard of clip/rdx_clip_proto.h (the last text that crossed, per selection).
 *
 * PRIMARY (the selected text, middle click) travels when both sides were started with --primary
 * (HELLO flag); CLIPBOARD always. At each connection the client sends its desktop's current
 * clipboard (copied before the stream started: paste it in the app); the host never pushes what
 * its app had before. Losing the peer keeps what each side owns; the client reconnects every second.
 *
 * X display:
 *   --display :N          that one (XAUTHORITY from the environment, or --xauthority FILE)
 *   --x-of-pid PID        the display of the gamescope below process PID (rdx_gs_src): an Xwayland
 *                         below it, else the X socket a process below it listens on (gamescope holds
 *                         them; its Xwayland is double-forked to init), else DISPLAY of the app
 *                         (display_below); -auth from that Xwayland's command line; looked for every
 *                         second until it is there (gamescope starts it after us)
 *   --desktop             the desktop session's (p0b/rdx_xwin.h rdx_x_desktop_env, as the window sink)
 * Started as root it drops to the owner of the display's socket once connected. The X server going
 * away ends the program (exit 2): the session goes on without a clipboard.
 *
 *   rdx_clip --listen IP --port P [--peer IP] | --connect IP --port P
 *            --display :N | --x-of-pid PID | --desktop  [--xauthority FILE] [--primary] [--max-bytes N]
 *   rdx_clip --get [--primary] --display :N | --x-of-pid PID | --desktop     print the selection, exit
 *   rdx_clip --set [--primary] [--seconds N] ...same displays...      own it with the text on stdin and
 *            serve it until another owner takes it (gamescope at once: it then serves it itself), or N s
 * Port: rdx-hostd's "clip_port", 18700 + 100 * seat.
 */
#define _GNU_SOURCE
#include "rdx_clip_proto.h"
#include "../p0b/rdx_xwin.h"          /* rdx_x_desktop_env */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../third_party/xclip/xclib.h"
#pragma GCC diagnostic pop

#define READ_TIMEOUT_S  3.0         /* an owner that does not answer a conversion */
#define INCR_TIMEOUT_S  10.0        /* a requestor that stops taking our INCR chunks */
#define SEND_TIMEOUT_S  3

static struct {
    /* options */
    const char *listen_ip, *connect_ip, *peer_ip, *display, *xauth;
    int port, x_of_pid, desktop, want_primary, get_mode, set_mode;
    uint8_t *set_txt;                   /* --set: the text from stdin, served until another owner takes it */
    size_t set_len;
    double set_end;                     /* --seconds limit of --set, 0 = none */
    uint32_t max;
    /* network */
    int lfd, cfd, hello_rx, peer_primary;
    uint8_t hdr[RDX_CLIP_HDR];
    size_t hdr_got, pay_got;
    struct rdx_clip_hdr rh;
    uint8_t *pay;
    double next_connect;
    int connect_logged;
    struct rdx_clip_sync sync;
    /* X */
    Display *dpy;
    Window win;
    Atom sel[RDX_CLIP_NSEL], utf8, targets, text, tplain, tplain8;
    int xfixes_ev;
    double next_xtry;
    int x_logged, ready_logged;
    int own[RDX_CLIP_NSEL];
    uint8_t *own_txt[RDX_CLIP_NSEL];
    size_t own_len[RDX_CLIP_NSEL];
    int pending_apply[RDX_CLIP_NSEL];   /* text from the peer before the display was there */
    int pending_read[RDX_CLIP_NSEL];
    int reading;                        /* selection being read, -1 none */
    Atom rd_target, rd_type;
    unsigned int rd_ctx;
    unsigned char *rd_txt;
    unsigned long rd_len;
    double rd_deadline;
    struct {                            /* one INCR transfer we serve at a time */
        int active, sel;
        Window w;
        Atom pty, target;
        unsigned long pos;
        unsigned int ctx;
        long chunk;
        uint8_t *txt;
        size_t len;
        double deadline;
    } incr;
} G = {.lfd = -1, .cfd = -1, .reading = -1, .max = RDX_CLIP_MAX_DEFAULT};

static const char *sel_name[RDX_CLIP_NSEL] = {"CLIPBOARD", "PRIMARY"};

void errmalloc(void)                    /* xclib.c: out of memory */
{
    fprintf(stderr, "[clip] out of memory\n");
    exit(1);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int primary_on(void) { return G.want_primary && G.peer_primary && G.hello_rx; }
static int sel_live(int s) { return G.cfd >= 0 && G.hello_rx && (s == RDX_CLIP_CLIPBOARD || primary_on()); }

/* ---------------- network ---------------- */

static void net_close(const char *why)
{
    if (G.cfd < 0) return;
    fprintf(stderr, "[clip] peer gone (%s)\n", why);
    close(G.cfd);
    G.cfd = -1;
    G.hello_rx = G.peer_primary = 0;
    G.hdr_got = G.pay_got = 0;
    free(G.pay);
    G.pay = NULL;
    rdx_clip_sync_reset(&G.sync);
    G.next_connect = now_s() + 1.0;
    for (int s = 0; s < RDX_CLIP_NSEL; s++) G.pending_read[s] = 0;
}

static int send_all(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = send(G.cfd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_frame(uint8_t type, uint8_t sel, uint8_t flags, const uint8_t *pay, uint32_t len)
{
    uint8_t h[RDX_CLIP_HDR];
    rdx_clip_hdr_pack(h, type, sel, flags, len);
    if (G.cfd < 0) return -1;
    if (send_all(h, sizeof(h)) || (len && send_all(pay, len))) { net_close(strerror(errno)); return -1; }
    return 0;
}

static void net_up(int fd)
{
    int one = 1;
    struct timeval tv = {.tv_sec = SEND_TIMEOUT_S};
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    G.cfd = fd;
    G.hello_rx = 0;
    G.connect_logged = 0;
    rdx_clip_sync_reset(&G.sync);
    send_frame(RDX_CLIP_HELLO, 0, G.want_primary ? RDX_CLIP_F_PRIMARY : 0, NULL, 0);
}

static int make_addr(const char *ip, int port, struct sockaddr_in *a)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &a->sin_addr) == 1 ? 0 : -1;
}

static int net_listen(void)
{
    struct sockaddr_in a;
    if (make_addr(G.listen_ip, G.port, &a)) { fprintf(stderr, "[clip] bad address %s\n", G.listen_ip); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 2)) {
        fprintf(stderr, "[clip] cannot listen on %s:%d: %s\n", G.listen_ip, G.port, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    G.lfd = fd;
    fprintf(stderr, "[clip] listening on %s:%d%s%s\n", G.listen_ip, G.port, G.peer_ip ? " for " : "", G.peer_ip ? G.peer_ip : "");
    return 0;
}

static void net_accept(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int fd = accept4(G.lfd, (struct sockaddr *)&a, &al, SOCK_CLOEXEC);
    if (fd < 0) return;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    if (G.peer_ip && strcmp(ip, G.peer_ip)) {
        fprintf(stderr, "[clip] refused a connection from %s (the session's client is %s)\n", ip, G.peer_ip);
        close(fd);
        return;
    }
    if (G.cfd >= 0) net_close("replaced by a new connection");
    fprintf(stderr, "[clip] connection from %s\n", ip);
    net_up(fd);
}

static void net_connect(void)
{
    struct sockaddr_in a;
    G.next_connect = now_s() + 1.0;
    if (make_addr(G.connect_ip, G.port, &a)) { fprintf(stderr, "[clip] bad address %s\n", G.connect_ip); exit(1); }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));       /* bounds connect() too */
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        if (!G.connect_logged++)
            fprintf(stderr, "[clip] connecting to %s:%d: %s, retrying every second\n", G.connect_ip, G.port, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    fprintf(stderr, "[clip] connected to %s:%d\n", G.connect_ip, G.port);
    net_up(fd);
}

/* ---------------- X: owning a selection ---------------- */

static int sel_index(Atom a)
{
    for (int s = 0; s < RDX_CLIP_NSEL; s++) if (G.sel[s] == a) return s;
    return -1;
}

/* become the owner of selection s with the peer's text (the echo guard holds it) */
static void take_over(int s)
{
    if (!G.dpy) { G.pending_apply[s] = 1; return; }
    G.pending_apply[s] = 0;
    free(G.own_txt[s]);
    G.own_len[s] = G.sync.len[s];
    G.own_txt[s] = malloc(G.own_len[s] ? G.own_len[s] : 1);
    if (!G.own_txt[s]) errmalloc();
    memcpy(G.own_txt[s], G.sync.last[s], G.own_len[s]);
    XSetSelectionOwner(G.dpy, G.sel[s], G.win, CurrentTime);
    G.own[s] = XGetSelectionOwner(G.dpy, G.sel[s]) == G.win;
    fprintf(stderr, "[clip] <- %s %zu bytes%s\n", sel_name[s], G.own_len[s], G.own[s] ? "" : " (could not own it)");
    XFlush(G.dpy);
}

static void refuse(XSelectionRequestEvent *rq)
{
    XEvent r = {0};
    r.xselection.type = SelectionNotify;
    r.xselection.display = rq->display;
    r.xselection.requestor = rq->requestor;
    r.xselection.selection = rq->selection;
    r.xselection.target = rq->target;
    r.xselection.property = None;
    r.xselection.time = rq->time;
    XSendEvent(G.dpy, rq->requestor, False, NoEventMask, &r);
    XFlush(G.dpy);
}

static int text_target(Atom t)
{
    return t == G.utf8 || t == XA_STRING || t == G.text || t == G.tplain || t == G.tplain8;
}

static void serve(XEvent *ev)
{
    XSelectionRequestEvent *rq = &ev->xselectionrequest;
    int s = sel_index(rq->selection);
    if (s < 0 || !G.own[s] || rq->property == None || (rq->target != G.targets && !text_target(rq->target)) ||
        (G.incr.active && rq->target != G.targets)) {
        refuse(rq);
        return;
    }
    Window w;
    Atom pty;
    unsigned long pos = 0;
    unsigned int ctx = XCLIB_XCIN_NONE;
    long chunk = 0;
    /* TARGETS lists TARGETS + UTF8_STRING; a text target is answered in the type it asked for */
    Atom tgt = rq->target == G.targets ? G.utf8 : rq->target;
    int done = xcin(G.dpy, &w, *ev, &pty, tgt, G.own_txt[s], G.own_len[s], &pos, NULL, &ctx, &chunk);
    if (!done && ctx == XCLIB_XCIN_INCR) {      /* larger than a quarter of the request size */
        G.incr.active = 1;
        G.incr.sel = s;
        G.incr.w = w;
        G.incr.pty = pty;
        G.incr.target = tgt;
        G.incr.pos = pos;
        G.incr.ctx = ctx;
        G.incr.chunk = chunk;
        G.incr.len = G.own_len[s];
        G.incr.txt = malloc(G.incr.len ? G.incr.len : 1);
        if (!G.incr.txt) errmalloc();
        memcpy(G.incr.txt, G.own_txt[s], G.incr.len);
        G.incr.deadline = now_s() + INCR_TIMEOUT_S;
    }
}

static void incr_end(void)
{
    if (!G.incr.active) return;
    XSelectInput(G.dpy, G.incr.w, NoEventMask);
    free(G.incr.txt);
    memset(&G.incr, 0, sizeof(G.incr));
}

static void incr_event(XEvent *ev)
{
    int done = xcin(G.dpy, &G.incr.w, *ev, &G.incr.pty, G.incr.target, G.incr.txt, G.incr.len, &G.incr.pos, NULL,
                    &G.incr.ctx, &G.incr.chunk);
    if (done) incr_end();
    else G.incr.deadline = now_s() + INCR_TIMEOUT_S;
}

/* ---------------- X: reading a new owner's text ---------------- */

static void start_next_read(void);

static void read_end(void)
{
    free(G.rd_txt);
    G.rd_txt = NULL;
    G.rd_len = 0;
    G.rd_ctx = XCLIB_XCOUT_NONE;
    G.reading = -1;
}

static void read_done(int ok)
{
    int s = G.reading;
    if (ok && G.rd_len) {
        size_t n = 0;
        uint8_t *t = rdx_clip_to_utf8(G.rd_txt, G.rd_len, G.rd_target == XA_STRING, &n);
        if (!t) errmalloc();
        if (G.get_mode) {
            fwrite(t, 1, n, stdout);
            fflush(stdout);
            exit(0);
        }
        if (n > G.max)
            fprintf(stderr, "[clip] %s has %zu bytes, over the limit of %u: not sent\n", sel_name[s], n, G.max);
        else if (n && sel_live(s) && rdx_clip_sync_local(&G.sync, s, t, n) &&
                 !send_frame(RDX_CLIP_TEXT, (uint8_t)s, 0, t, (uint32_t)n))
            fprintf(stderr, "[clip] -> %s %zu bytes\n", sel_name[s], n);
        free(t);
    } else if (G.get_mode) {
        fprintf(stderr, "[clip] %s holds no text\n", sel_name[s]);
        exit(1);
    }
    read_end();
    start_next_read();
}

static void read_feed(XEvent *ev)
{
    XEvent none = {0};
    if (xcout(G.dpy, G.win, ev ? *ev : none, G.sel[G.reading], G.rd_target, &G.rd_type, &G.rd_txt, &G.rd_len, &G.rd_ctx)) {
        read_done(1);
        return;
    }
    if (G.rd_ctx == XCLIB_XCOUT_BAD_TARGET) {
        if (G.rd_target == G.utf8) {           /* an old program: STRING (ISO 8859-1) */
            G.rd_target = XA_STRING;
            G.rd_ctx = XCLIB_XCOUT_NONE;
            xcout(G.dpy, G.win, none, G.sel[G.reading], G.rd_target, &G.rd_type, &G.rd_txt, &G.rd_len, &G.rd_ctx);
            G.rd_deadline = now_s() + READ_TIMEOUT_S;
        } else {
            read_done(0);                       /* no text there (an image, files) */
        }
        return;
    }
    if (G.rd_len > 2ul * G.max + 16) {          /* an INCR transfer far over the limit: stop taking it */
        fprintf(stderr, "[clip] %s is over the limit of %u bytes: not sent\n", sel_name[G.reading], G.max);
        XDeleteProperty(G.dpy, G.win, XInternAtom(G.dpy, "XCLIP_OUT", False));
        read_end();
        start_next_read();
    }
    G.rd_deadline = now_s() + READ_TIMEOUT_S;
}

static void start_next_read(void)
{
    if (G.reading >= 0 || !G.dpy) return;
    for (int s = 0; s < RDX_CLIP_NSEL; s++) {
        if (!G.pending_read[s]) continue;
        G.pending_read[s] = 0;
        if (!G.get_mode && !sel_live(s)) continue;
        G.reading = s;
        G.rd_target = G.utf8;
        G.rd_ctx = XCLIB_XCOUT_NONE;
        G.rd_len = 0;
        G.rd_txt = NULL;
        read_feed(NULL);                        /* XConvertSelection */
        XFlush(G.dpy);
        G.rd_deadline = now_s() + READ_TIMEOUT_S;
        return;
    }
}

/* ---------------- X: the display ---------------- */

/* ---- which display belongs to the gamescope below a process ----
 * gamescope (wlroots) starts its Xwayland with a double fork: Xwayland's parent is init, not
 * gamescope (seen in a seat container), so the process tree alone does not lead to it. gamescope
 * itself opens the display's listening sockets (/tmp/.X11-unix/XN and its abstract twin) and hands
 * them to Xwayland (-listenfd); it starts the app with DISPLAY=:N. In that order: an Xwayland that
 * is still below pid, the X socket a process below pid listens on, DISPLAY of a process below pid. */

#define MAXP 32768
static int g_np, g_pid[MAXP], g_ppid[MAXP];

static void proc_table(void)
{
    g_np = 0;
    DIR *d = opendir("/proc");
    struct dirent *de;
    while (d && (de = readdir(d)) && g_np < MAXP) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], st[512];
        snprintf(path, sizeof(path), "/proc/%.200s/stat", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(st, 1, sizeof(st) - 1, f);
        fclose(f);
        st[n] = 0;
        char *rp = strrchr(st, ')');
        int pp = 0;
        if (!rp || sscanf(rp + 1, " %*c %d", &pp) != 1) continue;
        g_pid[g_np] = atoi(de->d_name);
        g_ppid[g_np++] = pp;
    }
    if (d) closedir(d);
}

static int ppid_of(int p)
{
    for (int i = 0; i < g_np; i++) if (g_pid[i] == p) return g_ppid[i];
    return 0;
}

static int is_below(int p, int root)
{
    for (int hop = 0; hop < 64 && p > 1; hop++) {
        p = ppid_of(p);
        if (p == root) return 1;
    }
    return 0;
}

static size_t read_small(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return r;
}

/* an Xwayland's ":N" and "-auth FILE" from its command line; want ":N" or NULL = any. 1 = it is one */
static int xwayland_args(int pid, const char *want, char *disp, size_t dn, char *auth, size_t an)
{
    static char buf[65536];
    char path[64], comm[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    if (!read_small(path, comm, sizeof(comm))) return 0;
    comm[strcspn(comm, "\n")] = 0;
    if (strcmp(comm, "Xwayland")) return 0;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    size_t n = read_small(path, buf, sizeof(buf));
    const char *dsp = NULL, *au = "", *prev = "";
    for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
        if (buf[i] == ':' && !dsp) dsp = buf + i;
        if (!strcmp(prev, "-auth")) au = buf + i;
        prev = buf + i;
    }
    if (!dsp || (want && strcmp(dsp, want))) return 0;
    snprintf(disp, dn, "%.32s", dsp);
    snprintf(auth, an, "%.*s", (int)(an > 1 ? an - 1 : 0), au);
    return 1;
}

/* the number N of a listening X socket ("/tmp/.X11-unix/XN", abstract or not) with this inode, -1 none */
static int x_socket_display(unsigned long ino)
{
    static char buf[1 << 20];
    read_small("/proc/net/unix", buf, sizeof(buf));
    for (char *l = strchr(buf, '\n'); l && *++l; l = strchr(l, '\n')) {
        unsigned long in, flags;
        unsigned st;
        char path[256] = "";
        /* Num RefCount Protocol Flags Type St Inode Path */
        if (sscanf(l, "%*s %*s %*s %lx %*s %x %lu %255s", &flags, &st, &in, path) < 3 || in != ino) continue;
        const char *x = strstr(path, "/tmp/.X11-unix/X");
        if (x && (flags & 0x10000)) return atoi(x + 16);      /* __SO_ACCEPTCON: listening */
    }
    return -1;
}

/* the display of the gamescope below pid: 1 = found, how = the way it was found */
static int display_below(int pid, char *disp, size_t dn, char *auth, size_t an, const char **how)
{
    proc_table();
    for (int i = 0; i < g_np; i++)
        if (is_below(g_pid[i], pid) && xwayland_args(g_pid[i], NULL, disp, dn, auth, an)) { *how = "its Xwayland"; return 1; }
    int best = -1;
    for (int i = 0; i < g_np; i++) {
        if (!is_below(g_pid[i], pid)) continue;
        char dir[64], lnk[64], path[340];
        snprintf(dir, sizeof(dir), "/proc/%d/fd", g_pid[i]);
        DIR *d = opendir(dir);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            snprintf(path, sizeof(path), "%s/%.255s", dir, de->d_name);
            ssize_t l = readlink(path, lnk, sizeof(lnk) - 1);
            unsigned long ino;
            if (l <= 0) continue;
            lnk[l] = 0;
            if (sscanf(lnk, "socket:[%lu]", &ino) != 1) continue;
            int nr = x_socket_display(ino);
            if (nr >= 0 && (best < 0 || nr < best)) best = nr;
        }
        if (d) closedir(d);
    }
    if (best >= 0) {
        char want[16];
        snprintf(want, sizeof(want), ":%d", best);
        snprintf(disp, dn, "%s", want);
        auth[0] = 0;
        for (int i = 0; i < g_np; i++)          /* its Xwayland, wherever it hangs: for -auth */
            if (xwayland_args(g_pid[i], want, disp, dn, auth, an)) break;
        *how = "the X socket gamescope listens on";
        return 1;
    }
    static char env[65536];
    for (int i = 0; i < g_np; i++) {
        if (!is_below(g_pid[i], pid)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/environ", g_pid[i]);
        size_t n = read_small(path, env, sizeof(env));
        for (size_t k = 0; k < n; k += strlen(env + k) + 1)
            if (!strncmp(env + k, "DISPLAY=:", 9)) {
                snprintf(disp, dn, "%.32s", env + k + 8);
                auth[0] = 0;
                *how = "DISPLAY of the app";
                return 1;
            }
    }
    return 0;
}

/* gamescope's display disp (":N") may be used: an Xwayland for it runs (anywhere: it is double-forked)
 * and a process below pid has DISPLAY=:N (gamescope starts the app only once Xwayland is ready) */
static int gamescope_ready(int pid, const char *disp)
{
    char d2[64], a2[PATH_MAX], want[48];
    int xw = 0, app = 0;
    proc_table();
    for (int i = 0; i < g_np && !xw; i++) xw = xwayland_args(g_pid[i], disp, d2, sizeof(d2), a2, sizeof(a2));
    snprintf(want, sizeof(want), "DISPLAY=%s", disp);
    static char env[65536];
    for (int i = 0; i < g_np && xw && !app; i++) {
        if (!is_below(g_pid[i], pid)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/environ", g_pid[i]);
        size_t n = read_small(path, env, sizeof(env));
        for (size_t k = 0; k < n && !app; k += strlen(env + k) + 1) app = !strcmp(env + k, want);
    }
    return xw && app;
}

static int x_io_error(Display *d)
{
    (void)d;
    fprintf(stderr, "[clip] the X display went away: no clipboard from now on\n");
    exit(2);
}

/* started as root: become the owner of the display's socket (the desktop user) */
static void drop_root(const char *disp)
{
    if (geteuid() != 0) return;
    const char *c = strrchr(disp, ':');
    char sock[64];
    struct stat st;
    snprintf(sock, sizeof(sock), "/tmp/.X11-unix/X%d", c ? atoi(c + 1) : 0);
    if (stat(sock, &st) || st.st_uid == 0) return;
    if (setgroups(0, NULL) || setgid(st.st_gid) || setuid(st.st_uid)) {
        fprintf(stderr, "[clip] cannot drop to uid %u: %s\n", (unsigned)st.st_uid, strerror(errno));
        exit(1);
    }
    fprintf(stderr, "[clip] runs as uid %u (the display's owner)\n", (unsigned)st.st_uid);
}

static void x_try(void)
{
    char disp[64] = "", auth[PATH_MAX] = "";
    G.next_xtry = now_s() + 1.0;
    if (G.display) snprintf(disp, sizeof(disp), "%s", G.display);
    else if (G.x_of_pid) {
        const char *how = "";
        if (!display_below(G.x_of_pid, disp, sizeof(disp), auth, sizeof(auth), &how)) {
            if (!G.x_logged++) fprintf(stderr, "[clip] waiting for the display of the gamescope below process %d\n", G.x_of_pid);
            return;
        }
        if (!gamescope_ready(G.x_of_pid, disp)) {
            /* never the first client: wlroots starts Xwayland lazily on the first connection, and
             * one that fails and leaves made gamescope wait for an Xwayland that had quit
             * (-terminate), and Steam never started */
            if (G.ready_logged++ % 30 == 0)
                fprintf(stderr, "[clip] display %s below process %d (%s): waiting until its Xwayland runs and the "
                        "app is started on it\n", disp, G.x_of_pid, how);
            return;
        }
        fprintf(stderr, "[clip] display %s below process %d: %s; Xwayland and the app run\n", disp, G.x_of_pid, how);
        if (auth[0]) setenv("XAUTHORITY", auth, 1);
    } else if (G.desktop) {
        unsetenv("DISPLAY");
        rdx_x_desktop_env();
        if (!getenv("DISPLAY")) { G.next_xtry = now_s() + 5.0; return; }
        snprintf(disp, sizeof(disp), "%s", getenv("DISPLAY"));
    }
    if (G.xauth) setenv("XAUTHORITY", G.xauth, 1);
    /* never as root: gamescope's Xwayland has no cookie and admits only its own user
     * ("Authorization required"); the owner of the display's socket connects */
    drop_root(disp);
    Display *d = XOpenDisplay(disp);
    if (!d) {
        if (!G.x_logged++) fprintf(stderr, "[clip] cannot open display %s yet, retrying\n", disp);
        return;
    }
    int ev_base, err_base;
    if (!XFixesQueryExtension(d, &ev_base, &err_base)) {
        fprintf(stderr, "[clip] display %s has no XFixes: no clipboard\n", disp);
        exit(1);
    }
    G.dpy = d;
    G.xfixes_ev = ev_base;
    XSetErrorHandler(xchandler);
    XSetIOErrorHandler(x_io_error);
    Window root = DefaultRootWindow(d);
    G.win = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(d, G.win, PropertyChangeMask);
    G.sel[RDX_CLIP_CLIPBOARD] = XInternAtom(d, "CLIPBOARD", False);
    G.sel[RDX_CLIP_PRIMARY] = XA_PRIMARY;
    G.utf8 = XInternAtom(d, "UTF8_STRING", False);
    G.targets = XInternAtom(d, "TARGETS", False);
    G.text = XInternAtom(d, "TEXT", False);
    G.tplain = XInternAtom(d, "text/plain", False);
    G.tplain8 = XInternAtom(d, "text/plain;charset=utf-8", False);
    for (int s = 0; s < RDX_CLIP_NSEL; s++)
        XFixesSelectSelectionInput(d, root, G.sel[s], XFixesSetSelectionOwnerNotifyMask);
    XFlush(d);
    fprintf(stderr, "[clip] on display %s\n", disp);
    drop_root(disp);
    for (int s = 0; s < RDX_CLIP_NSEL; s++) if (G.pending_apply[s]) take_over(s);
    if (G.get_mode) {
        G.pending_read[G.want_primary ? RDX_CLIP_PRIMARY : RDX_CLIP_CLIPBOARD] = 1;
        start_next_read();
    }
    if (G.set_mode) {
        int s = G.want_primary ? RDX_CLIP_PRIMARY : RDX_CLIP_CLIPBOARD;
        rdx_clip_sync_remote(&G.sync, s, G.set_txt, G.set_len);
        take_over(s);
        if (!G.own[s]) exit(1);
    }
}

static void x_event(XEvent *ev)
{
    if (ev->type == G.xfixes_ev + XFixesSelectionNotify) {
        XFixesSelectionNotifyEvent *fe = (XFixesSelectionNotifyEvent *)ev;
        int s = sel_index(fe->selection);
        if (s < 0 || fe->owner == G.win || fe->owner == None || !sel_live(s)) return;
        G.pending_read[s] = 1;                  /* a new owner: read its text */
        start_next_read();
        return;
    }
    switch (ev->type) {
    case SelectionRequest:
        serve(ev);
        break;
    case SelectionClear: {
        int s = sel_index(ev->xselectionclear.selection);
        if (s >= 0) G.own[s] = 0;
        if (G.set_mode && s == (G.want_primary ? RDX_CLIP_PRIMARY : RDX_CLIP_CLIPBOARD)) {
            fprintf(stderr, "[clip] another owner took %s: done\n", sel_name[s]);
            exit(0);                            /* gamescope: it serves our text now on all its Xwaylands */
        }
        break;
    }
    case SelectionNotify:
        if (G.reading >= 0 && ev->xselection.requestor == G.win && ev->xselection.selection == G.sel[G.reading])
            read_feed(ev);
        break;
    case PropertyNotify:
        if (ev->xproperty.window == G.win) {
            if (G.reading >= 0 && G.rd_ctx == XCLIB_XCOUT_INCR) read_feed(ev);
        } else if (G.incr.active && ev->xproperty.window == G.incr.w && ev->xproperty.atom == G.incr.pty) {
            incr_event(ev);
        }
        break;
    default:
        break;
    }
}

/* ---------------- frames from the peer ---------------- */

static void frame(void)
{
    if (G.rh.type == RDX_CLIP_HELLO) {
        G.hello_rx = 1;
        G.peer_primary = !!(G.rh.flags & RDX_CLIP_F_PRIMARY);
        fprintf(stderr, "[clip] peer ready: CLIPBOARD%s\n", primary_on() ? " and PRIMARY" : "");
        if (G.connect_ip) {                     /* the client: what the desktop has now, for the app */
            G.pending_read[RDX_CLIP_CLIPBOARD] = 1;
            if (primary_on()) G.pending_read[RDX_CLIP_PRIMARY] = 1;
            start_next_read();
        }
        return;
    }
    int s = G.rh.sel;
    if (!G.hello_rx) { net_close("text before HELLO"); return; }
    if (!rdx_clip_utf8_ok(G.pay, G.rh.len)) { net_close("text that is not UTF-8"); return; }
    if (s == RDX_CLIP_PRIMARY && !primary_on()) return;
    if (rdx_clip_sync_remote(&G.sync, s, G.pay, G.rh.len)) take_over(s);
}

static void net_read(void)
{
    for (;;) {
        ssize_t r;
        if (G.hdr_got < RDX_CLIP_HDR) {
            r = recv(G.cfd, G.hdr + G.hdr_got, RDX_CLIP_HDR - G.hdr_got, MSG_DONTWAIT);
            if (r > 0) {
                G.hdr_got += (size_t)r;
                if (G.hdr_got < RDX_CLIP_HDR) continue;
                char why[96];
                if (rdx_clip_hdr_parse(G.hdr, G.max, &G.rh, why, sizeof(why))) { net_close(why); return; }
                G.pay_got = 0;
                G.pay = malloc(G.rh.len + 1);
                if (!G.pay) errmalloc();
            }
        } else {
            r = G.pay_got < G.rh.len ? recv(G.cfd, G.pay + G.pay_got, G.rh.len - G.pay_got, MSG_DONTWAIT) : 0;
            if (r > 0) G.pay_got += (size_t)r;
        }
        if (G.hdr_got == RDX_CLIP_HDR && G.pay_got == G.rh.len) {
            frame();
            if (G.cfd < 0) return;
            free(G.pay);
            G.pay = NULL;
            G.hdr_got = G.pay_got = 0;
            continue;
        }
        if (r == 0) { net_close("closed"); return; }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            net_close(strerror(errno));
            return;
        }
    }
}

/* ---------------- main ---------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: rdx_clip --listen IP --port P [--peer IP] | --connect IP --port P\n"
            "                --display :N | --x-of-pid PID | --desktop [--xauthority FILE] [--primary] [--max-bytes N]\n"
            "       rdx_clip --get [--primary] --display :N | --x-of-pid PID | --desktop\n"
            "       rdx_clip --set [--primary] [--seconds N] --display :N | --x-of-pid PID | --desktop  < text\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        {"listen", 1, 0, 'l'}, {"connect", 1, 0, 'c'}, {"peer", 1, 0, 'P'}, {"port", 1, 0, 'p'},
        {"display", 1, 0, 'd'}, {"x-of-pid", 1, 0, 'x'}, {"desktop", 0, 0, 'D'}, {"xauthority", 1, 0, 'a'},
        {"primary", 0, 0, 'r'}, {"max-bytes", 1, 0, 'm'}, {"get", 0, 0, 'g'}, {"set", 0, 0, 's'},
        {"seconds", 1, 0, 'S'}, {0, 0, 0, 0}};
    double seconds = 0;
    int o;
    while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (o) {
        case 'l': G.listen_ip = optarg; break;
        case 'c': G.connect_ip = optarg; break;
        case 'P': G.peer_ip = optarg; break;
        case 'p': G.port = atoi(optarg); break;
        case 'd': G.display = optarg; break;
        case 'x': G.x_of_pid = atoi(optarg); break;
        case 'D': G.desktop = 1; break;
        case 'a': G.xauth = optarg; break;
        case 'r': G.want_primary = 1; break;
        case 'm': G.max = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'g': G.get_mode = 1; break;
        case 's': G.set_mode = 1; break;
        case 'S': seconds = atof(optarg); break;
        default: usage();
        }
    }
    if (!!G.display + !!G.x_of_pid + G.desktop != 1) usage();
    if (G.get_mode && G.set_mode) usage();
    if (G.set_mode) {                           /* the text, from stdin */
        size_t cap = 65536, n = 0;
        uint8_t *b = malloc(cap);
        ssize_t r;
        while (b && (r = read(0, b + n, cap - n)) > 0)
            if ((n += (size_t)r) == cap && !(b = realloc(b, cap *= 2))) break;
        if (!b) errmalloc();
        G.set_txt = rdx_clip_to_utf8(b, n, 0, &G.set_len);
        free(b);
        if (!G.set_txt) errmalloc();
        if (G.set_len > (G.max ? G.max : RDX_CLIP_MAX_DEFAULT)) { fprintf(stderr, "[clip] over the limit\n"); return 1; }
        if (seconds > 0) G.set_end = now_s() + seconds;
        G.get_mode = 0;
    }
    if (!G.get_mode && !G.set_mode && (!!G.listen_ip + !!G.connect_ip != 1 || G.port <= 0 || G.port > 65535)) usage();
    if (!G.max) G.max = RDX_CLIP_MAX_DEFAULT;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (G.listen_ip && net_listen()) return 1;
    if (G.get_mode || G.set_mode) {
        double end = now_s() + 5.0;
        while (!G.dpy && now_s() < end) { x_try(); if (!G.dpy) usleep(200000); }
        if (!G.dpy) { fprintf(stderr, "[clip] no display\n"); return 1; }
    }
    for (;;) {
        double now = now_s();
        if (!G.dpy && now >= G.next_xtry) x_try();
        if (G.connect_ip && G.cfd < 0 && now >= G.next_connect) net_connect();
        if (G.dpy) {
            while (XPending(G.dpy)) {
                XEvent ev;
                XNextEvent(G.dpy, &ev);
                x_event(&ev);
            }
            now = now_s();
            if (G.reading >= 0 && now > G.rd_deadline) {
                fprintf(stderr, "[clip] the owner of %s did not answer\n", sel_name[G.reading]);
                if (G.get_mode) return 1;
                read_end();
                start_next_read();
            }
            if (G.incr.active && now > G.incr.deadline) incr_end();
            if (G.set_end > 0 && now > G.set_end) return 0;
            XFlush(G.dpy);
        }
        /* how long to sleep: the next retry or deadline */
        double next = now + 30.0;
        if (!G.dpy && G.next_xtry < next) next = G.next_xtry;
        if (G.connect_ip && G.cfd < 0 && G.next_connect < next) next = G.next_connect;
        if (G.reading >= 0 && G.rd_deadline < next) next = G.rd_deadline;
        if (G.incr.active && G.incr.deadline < next) next = G.incr.deadline;
        if (G.set_end > 0 && G.set_end < next) next = G.set_end;
        int ms = next > now ? (int)((next - now) * 1000) + 1 : 0;
        if (G.dpy && XQLength(G.dpy)) ms = 0;   /* events a round trip queued meanwhile */
        struct pollfd pf[3];
        int np = 0, il = -1, ic = -1, ix = -1;
        if (G.lfd >= 0) { il = np; pf[np++] = (struct pollfd){G.lfd, POLLIN, 0}; }
        if (G.cfd >= 0) { ic = np; pf[np++] = (struct pollfd){G.cfd, POLLIN, 0}; }
        if (G.dpy) { ix = np; pf[np++] = (struct pollfd){ConnectionNumber(G.dpy), POLLIN, 0}; }
        int r = poll(pf, (nfds_t)np, ms);
        if (r < 0 && errno != EINTR) { perror("poll"); return 1; }
        if (r <= 0) continue;
        if (il >= 0 && pf[il].revents) net_accept();
        if (ic >= 0 && pf[ic].revents && G.cfd >= 0) net_read();
        (void)ix;                               /* X events: XPending at the top of the loop */
    }
}
