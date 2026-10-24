/* The start placeholder's two state machines, without a display or a host:
 *   clientd side: the real client/rdx_menu.c on its socket, with a fake sink and a fake input sender;
 *   sink side:    the real p0b/rdx_overlay.c message handling over a socketpair (no DRM involved).
 *   tests/test_placeholder_ipc [font.ttf]     exit 0 = PASS */
#define _GNU_SOURCE
#include "../client/rdx_menu.h"
#include "../common/rdx_menu_ipc.h"
#include "../p0b/rdx_overlay.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fails, restarted;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static void act_quit(void) {}
static void act_update(int c, const char *o) { (void)c; (void)o; }
static void act_monitor(int on) { (void)on; }
static void act_restart(void) { restarted++; }

static void sendm(int fd, uint32_t t, int a, int b, int c, int d, int e)
{
    struct rdx_menu_msg m = {t, a, b, c, d, e};
    send(fd, &m, sizeof(m), 0);
}

/* run the menu's loop for ms and collect what the sink gets; fds of BUFFERS go to bufs */
static int pump(int sink, int ms, struct rdx_menu_msg *out, int max, int *bufs)
{
    int n = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) break;
        struct pollfd p[2] = {{rdx_menu_poll_fd(), POLLIN, 0}, {sink, POLLIN, 0}};
        poll(p, 2, (int)(ms - el));
        if (p[0].revents) rdx_menu_dispatch();
        for (;;) {
            struct rdx_menu_msg m;
            char cb[CMSG_SPACE(sizeof(int) * RDX_MENU_NBUF)];
            struct iovec iov = {&m, sizeof(m)};
            struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof(cb)};
            if (recvmsg(sink, &mh, MSG_DONTWAIT) != (ssize_t)sizeof(m)) break;
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            if (c && bufs) memcpy(bufs, CMSG_DATA(c), sizeof(int) * RDX_MENU_NBUF);
            if (n < max) out[n++] = m;
        }
    }
    return n;
}

static int count(const struct rdx_menu_msg *m, int n, uint32_t type, int b)
{
    int k = 0;
    for (int i = 0; i < n; i++) if (m[i].type == type && (b < 0 || m[i].b == b)) k++;
    return k;
}

static int peer(const char *path, int role)
{
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(s, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); exit(2); }
    if (role == RDX_MENU_ROLE_SINK) sendm(s, RDX_MENU_HELLO, role, 1920, 1080, 60000, 0);
    else sendm(s, RDX_MENU_HELLO, role, 0, 0, 0, 0);
    return s;
}

static void clientd_side(const char *font)
{
    const char *path = "/tmp/rdx-ph-test.sock";
    setenv("RDX_PLACEHOLDER_WAIT_S", "2", 1);
    struct rdx_menu_actions act = {act_quit, act_quit, act_update, act_monitor, act_restart, NULL};
    if (rdx_menu_start(path, font, &act)) { printf("SKIP (no font %s)\n", font); exit(0); }
    struct rdx_menu_msg m[64];
    int bufs[RDX_MENU_NBUF] = {-1, -1};
    int sink = peer(path, RDX_MENU_ROLE_SINK), input = peer(path, RDX_MENU_ROLE_INPUT);
    int n = pump(sink, 200, m, 64, bufs);
    check(count(m, n, RDX_MENU_BUFFERS, -1) == 1 && bufs[0] >= 0, "the sink gets the overlay buffers");

    rdx_menu_placeholder(RDX_PH_START, "Steam");
    n = pump(sink, 200, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, -1) == 0, "no placeholder before the overlay is confirmed");
    sendm(sink, RDX_MENU_OVERLAY, 1, 0, 0, 0, 0);
    n = pump(sink, 300, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 1) == 1, "placeholder shown, marked until the first frame (SHOW b=1)");
    int k = n ? m[n - 1].a : 0;
    uint8_t *px = mmap(NULL, 4096 * 64, PROT_READ, MAP_SHARED, bufs[k], 0);
    long lit = 0;
    for (int i = 0; px != MAP_FAILED && i < 4096 * 64 / 4; i++) lit += ((uint32_t *)px)[i] >> 24 != 0;
    check(lit > 1000, "the shown buffer holds the drawn card");
    rdx_menu_placeholder_icon("Other app", (const unsigned char *)"x", 1);   /* late icon of another app: ignored */
    n = pump(sink, 1200, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 1) >= 1, "redrawn every second (the seconds count)");

    sendm(input, RDX_MENU_TOGGLE, 0, 0, 0, 0, 0);
    n = pump(sink, 150, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 0) == 1, "the menu opens over it (SHOW b=0, not taken off by a frame)");
    sendm(input, RDX_MENU_KEY, KEY_ESC, 1, 0, 0, 0);
    n = pump(sink, 150, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 1) >= 1 && count(m, n, RDX_MENU_SHOW, 0) == 0 && count(m, n, RDX_MENU_HIDE, -1) == 0,
          "menu closed before the first frame: back to the placeholder, no HIDE");

    pump(sink, 1200, m, 64, NULL);                     /* past RDX_PLACEHOLDER_WAIT_S = 2 */
    sendm(input, RDX_MENU_TOGGLE, 0, 0, 0, 0, 0);
    /* from "Back to the stream" down past the two quit entries onto the extra last one (down, not
     * up around the top: entries added above, like "Microphone mute", do not move it) */
    sendm(input, RDX_MENU_KEY, KEY_DOWN, 1, 0, 0, 0);
    sendm(input, RDX_MENU_KEY, KEY_DOWN, 1, 0, 0, 0);
    sendm(input, RDX_MENU_KEY, KEY_DOWN, 1, 0, 0, 0);
    sendm(input, RDX_MENU_KEY, KEY_ENTER, 1, 0, 0, 0);
    pump(sink, 200, m, 64, NULL);
    check(restarted == 1, "after the wait the menu offers \"Restart the host program\" (last entry, Enter)");

    rdx_menu_placeholder(RDX_PH_RESUME, "Steam");     /* a new start (the restart ended the old one) */
    n = pump(sink, 200, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 1) == 1, "resume: placeholder again");
    sendm(sink, RDX_MENU_FRAME, 1, 0, 0, 0, 0);
    n = pump(sink, 1300, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, -1) == 0, "first frame: no more placeholder draws (the sink took it off)");
    /* a still picture (a loading screen sends no frames for seconds; an older sink even reports
     * it with FRAME a=0): nothing is drawn over it, however long (drawing over it was a visible flicker) */
    sendm(sink, RDX_MENU_FRAME, 0, 0, 0, 0, 0);
    n = pump(sink, 10000, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, -1) == 0, "still picture, no frames for 10 s: no placeholder at all");
    /* the host says the picture pauses (session.stall: it switches the output, the source reconfigures) */
    rdx_menu_stall("switching to another output");
    n = pump(sink, 300, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 2) == 1, "host stall: placeholder over the last frame until the next one (SHOW b=2)");
    sendm(sink, RDX_MENU_FRAME, 1, 0, 0, 0, 0);
    n = pump(sink, 1300, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, -1) == 0, "the next frame: placeholder off, nothing drawn any more");
    rdx_menu_stall("switching again");
    n = pump(sink, 300, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, 2) == 1, "another host stall: placeholder again");
    sendm(sink, RDX_MENU_FRAME, 1, 0, 0, 0, 0);
    rdx_menu_end();
    n = pump(sink, 1200, m, 64, NULL);
    check(count(m, n, RDX_MENU_SHOW, -1) == 0, "session end: nothing drawn any more");
    close(sink);
    close(input);
    unlink(path);
}

static int sink_recv(int fd, struct rdx_menu_msg *m)
{
    return recv(fd, m, sizeof(*m), MSG_DONTWAIT) == (ssize_t)sizeof(*m);
}

static void sink_side(void)
{
    int sp[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp);
    struct rdx_overlay o;
    rdx_overlay_init(&o, -1, 0, NULL, 1920, 1080, 60000);      /* no socket path: just the defaults */
    o.sock = sp[1];
    o.usable = 1;
    check(o.waiting == 1, "sink: waiting for the first frame from the start");
    sendm(sp[0], RDX_MENU_SHOW, 1, 1, 0, 0, 0);
    rdx_overlay_handle(&o);
    check(o.want == 1 && o.until_frame && rdx_overlay_pending(&o), "sink: placeholder wanted, until the first frame");
    o.shown = o.want;                                          /* as if its own commit went out */
    rdx_overlay_frame(&o);
    struct rdx_menu_msg m;
    check(o.want == -1 && rdx_overlay_pending(&o), "sink: the first frame's commit takes it off");
    check(sink_recv(sp[0], &m) && m.type == RDX_MENU_FRAME && m.a == 1, "sink: tells clientd (FRAME a=1)");
    o.shown = -1;
    rdx_overlay_frame(&o);
    check(!sink_recv(sp[0], &m), "sink: later frames say nothing");
    sendm(sp[0], RDX_MENU_SHOW, 0, 1, 0, 0, 0);               /* a late placeholder while frames flow */
    rdx_overlay_handle(&o);
    check(o.want == -1, "sink: a placeholder that comes after the first frame is not shown");
    sendm(sp[0], RDX_MENU_SHOW, 0, 0, 0, 0, 0);               /* the menu */
    rdx_overlay_handle(&o);
    rdx_overlay_frame(&o);
    check(o.want == 0, "sink: the menu stays over frames");
    rdx_overlay_stalled(&o);
    check(!sink_recv(sp[0], &m) && o.want == 0, "sink: no frames for a while: nothing reported, nothing shown or hidden");
    sendm(sp[0], RDX_MENU_HIDE, 0, 0, 0, 0, 0);               /* the menu closes over the still picture */
    rdx_overlay_handle(&o);
    o.shown = -1;
    rdx_overlay_frame(&o);                                     /* frames again */
    while (sink_recv(sp[0], &m)) {}
    /* the host asks for the placeholder (SHOW b=2) while frames flow: a grace period first */
    sendm(sp[0], RDX_MENU_SHOW, 1, 2, 0, 0, 0);
    rdx_overlay_handle(&o);
    check(o.want == 1 && o.until_frame && o.waiting && !rdx_overlay_pending(&o),
          "sink: host-asked placeholder waits RDX_MENU_STALL_GRACE_MS before it shows");
    rdx_overlay_frame(&o);
    check(o.want == -1 && !rdx_overlay_pending(&o) && sink_recv(sp[0], &m) && m.type == RDX_MENU_FRAME && m.a == 1,
          "sink: a frame within the grace cancels it unseen (a quick switch shows nothing) and says so");
    sendm(sp[0], RDX_MENU_SHOW, 0, 2, 0, 0, 0);
    rdx_overlay_handle(&o);
    usleep((RDX_MENU_STALL_GRACE_MS + 100) * 1000);
    check(o.want == 0 && rdx_overlay_pending(&o), "sink: no frame within the grace: shown over the still last frame");
    o.shown = o.want;                                          /* its own commit went out */
    sendm(sp[0], RDX_MENU_SHOW, 1, 2, 0, 0, 0);               /* the card's redraw a second later */
    rdx_overlay_handle(&o);
    check(rdx_overlay_pending(&o), "sink: its redraws show at once (the grace is only before the first)");
    o.shown = o.want;
    rdx_overlay_frame(&o);
    check(o.want == -1 && rdx_overlay_pending(&o) && sink_recv(sp[0], &m) && m.a == 1,
          "sink: the next frame takes it off in its commit and tells clientd");
    close(sp[0]);
    close(sp[1]);
}

int main(int argc, char **argv)
{
    clientd_side(argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    sink_side();
    printf("test_placeholder_ipc: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
