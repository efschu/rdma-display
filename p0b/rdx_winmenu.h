/* SPDX-License-Identifier: MIT
 *
 * In-stream menu, sink side of a WINDOW client (p0b/rdx_sink_win as rdx-clientd's sink): the same
 * menu socket and messages as the KMS sink's overlay plane (common/rdx_menu_ipc.h, p0b/rdx_overlay.c),
 * but the two ARGB8888 buffers clientd renders (menu, start placeholder, link card) are imported into
 * the window's GL context (memfd -> udmabuf -> EGLImage, no copy) and blended over the frame by the
 * sink at its next draw. Semantics as on KMS:
 *   SHOW b = 0  the menu / link card, until HIDE;
 *   SHOW b = 1  the start placeholder, until the first stream frame (ignored once frames flow);
 *   SHOW b = 2  a placeholder the host asked for, over the last frame after RDX_MENU_STALL_GRACE_MS,
 *               off with the next frame.
 * The sink reports OVERLAY (buffers usable), FRAME a = 1 (the first frame after none / a placeholder),
 * STATS once per second and POINTER (the source's pointer hint from the frame header, and b = 1 while the
 * sink shows the host's cursor itself as the window's X cursor, common/rdx_cursor.h; on change).
 * Header-only: included by rdx_sink_win.c only; the GL import is the sink's (callback).
 */
#pragma once
#include "../common/rdx_menu_ipc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct rdx_winmenu {
    int sock;                       /* clientd's menu socket, -1 = no menu */
    int usable;                     /* buffers imported */
    uint32_t w, h;
    int32_t x, y;                   /* on the frame (the sink's mode) */
    int shown, want;                /* buffer drawn / wanted, -1 = none */
    int until_frame, waiting;
    uint64_t show_at;               /* SHOW b = 2: not before (CLOCK_MONOTONIC ns), 0 = now */
    int hint;                       /* last pointer hint reported, -2 = none yet */
    int own_cursor;                 /* reported: the window's X cursor is the host's (the cursor channel) */
};

/* import buffer i (memfd, ARGB8888 premultiplied, w x h, stride) into the sink's GL; 0 = ok */
typedef int (*rdx_winmenu_import)(void *user, int i, int memfd, uint32_t w, uint32_t h, uint32_t stride);

static inline uint64_t rdx_winmenu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline void rdx_winmenu_send(struct rdx_winmenu *o, uint32_t type, int a, int b, int c, int d, int e)
{
    struct rdx_menu_msg m = {type, a, b, c, d, e};
    if (o->sock >= 0) send(o->sock, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* connect and say hello (role sink, the frame size, refresh, e = 1: a window); never fatal */
static inline int rdx_winmenu_init(struct rdx_winmenu *o, const char *path, uint32_t mode_w, uint32_t mode_h,
                                   uint32_t refresh_mhz)
{
    memset(o, 0, sizeof(*o));
    o->sock = -1;
    o->shown = o->want = -1;
    o->waiting = 1;                 /* until the first stream frame */
    o->hint = -2;
    if (!path || !path[0]) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "[menu] no in-stream menu: %s: %s\n", path, strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    o->sock = s;
    rdx_winmenu_send(o, RDX_MENU_HELLO, RDX_MENU_ROLE_SINK, (int)mode_w, (int)mode_h, (int)refresh_mhz, 1);
    return 0;
}

/* messages from clientd (call when o->sock is readable); 1 = the overlay changed, -1 = socket gone */
static inline int rdx_winmenu_handle(struct rdx_winmenu *o, rdx_winmenu_import imp, void *user)
{
    int changed = 0;
    while (o->sock >= 0) {
        struct rdx_menu_msg m;
        int fds[RDX_MENU_NBUF] = {-1, -1};
        char cbuf[CMSG_SPACE(sizeof(fds))];
        struct iovec iov = {.iov_base = &m, .iov_len = sizeof(m)};
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
        ssize_t n = recvmsg(o->sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return changed;
        if (n <= 0) {                   /* clientd went away: no menu any more */
            close(o->sock);
            o->sock = -1;
            o->want = -1;
            return -1;
        }
        int nfd = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                if (nfd > RDX_MENU_NBUF) nfd = RDX_MENU_NBUF;
                memcpy(fds, CMSG_DATA(c), (size_t)nfd * sizeof(int));
            }
        if (n == (ssize_t)sizeof(m)) {
            if (m.type == RDX_MENU_BUFFERS && !o->usable) {
                o->w = (uint32_t)m.a; o->h = (uint32_t)m.b; o->x = m.d; o->y = m.e;
                int ok = nfd == RDX_MENU_NBUF;
                for (int i = 0; ok && i < RDX_MENU_NBUF; i++) ok = !imp(user, i, fds[i], o->w, o->h, (uint32_t)m.c);
                o->usable = ok;
                printf("[menu] window overlay %ux%u at %d,%d %s\n", o->w, o->h, o->x, o->y, ok ? "imported" : "NOT usable");
                fflush(stdout);
                rdx_winmenu_send(o, RDX_MENU_OVERLAY, ok, ok ? 0 : EINVAL, 0, 0, 0);
            } else if (m.type == RDX_MENU_SHOW && m.a >= 0 && m.a < RDX_MENU_NBUF) {
                int fresh = o->shown < 0 && !o->show_at;
                o->want = m.a;
                o->until_frame = m.b == 1 && o->waiting;
                if (m.b == 1 && !o->waiting) o->want = -1;
                if (m.b != 2) o->show_at = 0;
                if (m.b == 2) {             /* the host said the picture goes: after a grace period */
                    if (fresh) o->show_at = rdx_winmenu_now() + RDX_MENU_STALL_GRACE_MS * 1000000ull;
                    o->until_frame = 1;
                    o->waiting = 1;
                }
                changed = 1;
            } else if (m.type == RDX_MENU_HIDE) {
                o->want = -1;
                o->until_frame = 0;
                o->show_at = 0;
                changed = 1;
            }
        }
        for (int i = 0; i < nfd; i++) close(fds[i]);
    }
    return changed;
}

/* the buffer to blend over the frame now, -1 = none */
static inline int rdx_winmenu_current(struct rdx_winmenu *o)
{
    if (!o->usable || o->want < 0) return -1;
    if (o->show_at && rdx_winmenu_now() < o->show_at) return o->shown;
    return o->want;
}

/* a change waits to be drawn (a SHOW b = 2 only once its grace period is over) */
static inline int rdx_winmenu_pending(struct rdx_winmenu *o) { return o->usable && rdx_winmenu_current(o) != o->shown; }

/* a stream frame is drawn now: the first one after none / a placeholder takes the placeholder off */
static inline void rdx_winmenu_frame(struct rdx_winmenu *o)
{
    if (!o->waiting) return;
    o->waiting = 0;
    if (o->until_frame) {
        o->want = -1;
        o->until_frame = 0;
        o->show_at = 0;
    }
    rdx_winmenu_send(o, RDX_MENU_FRAME, 1, 0, 0, 0, 0);
}

/* no frame for RDX_MENU_STALL_MS: no event (a still picture), only the next frame is "first" again
 * for a start placeholder clientd may send */
static inline void rdx_winmenu_stalled(struct rdx_winmenu *o) { o->waiting = 1; }

/* the source's pointer hint (frame header flags): reported on change */
static inline void rdx_winmenu_hint(struct rdx_winmenu *o, int hint)
{
    if (hint == o->hint) return;
    o->hint = hint;
    printf("[sink] pointer hint of the source: %s\n", hint == 1 ? "relative (the app locks the pointer)" : "absolute");
    fflush(stdout);
    rdx_winmenu_send(o, RDX_MENU_POINTER, hint, o->own_cursor, 0, 0, 0);
}

/* the window shows the host's cursor itself (1) or the stream does (0): clientd tells the window's input to
 * leave the window's cursor alone (RDX_MENU_POINTER b = 2) instead of hiding it */
static inline void rdx_winmenu_cursor(struct rdx_winmenu *o, int own)
{
    if (own == o->own_cursor) return;
    o->own_cursor = own;
    rdx_winmenu_send(o, RDX_MENU_POINTER, o->hint == -2 ? -1 : o->hint, own, 0, 0, 0);
}

static inline void rdx_winmenu_stats(struct rdx_winmenu *o, long shown_s, long dropped_s, double p50_us, uint32_t refresh_mhz)
{
    rdx_winmenu_send(o, RDX_MENU_STATS, (int)shown_s, 0, (int)dropped_s, (int)(p50_us + 0.5), (int)refresh_mhz);
}
