/* SPDX-License-Identifier: MIT
 *
 * X11 helpers shared by the window sink (p0b/rdx_sink_win.c, --local and RDMA paths) and the
 * window's input sender (p4/rdx_input_win.c):
 *   rdx_x_desktop_env()  DISPLAY / XAUTHORITY of the desktop session for a root process without
 *                        them (from the environment of gnome-shell & co.; a Wayland session has
 *                        none there: then Xwayland's - its command line, else the display most
 *                        of the session's processes with the newest .mutter-Xwaylandauth.* use;
 *                        Xwayland starts on demand when we connect)
 *   rdx_letterbox()      where a frame of fw x fh sits in a window of win_w x win_h (aspect kept)
 *   rdx_xin_event()      one X event of the window -> evdev events of a keyboard / a mouse
 *                        (RDX_LOCAL_IN_KEY / _BUTTON / _REL of gs/rdx_local_proto.h, kind 0 = let
 *                        go of everything): keys by X keycode (= evdev code + 8), buttons 1-3 and
 *                        the side buttons 8/9, wheel 4-7 as one REL_WHEEL / REL_HWHEEL step per
 *                        press, pointer motion as relative motion in the frame's pixels (the window
 *                        may be scaled), F11 press stays local (fullscreen), focus loss -> kind 0.
 * The window only gets keys while it has the focus, so nothing of the rest of the desktop reaches
 * the app; no device is grabbed.
 */
#pragma once
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <linux/input-event-codes.h>
#include "../gs/rdx_local_proto.h"

/* Xwayland's display of user uid's Wayland session: Xwayland's own command line (":1 ... -auth FILE")
 * when it runs, else the DISPLAY most of the user's processes with the newest .mutter-Xwaylandauth
 * carry (mutter-x11-frames and ibus-x11 use a second display, :2; processes of an older X11 session
 * still have :0 with gdm's Xauthority). 0 = found */
static inline int rdx_xwayland_env(uid_t uid, char *disp, size_t dn, char *auth, size_t an)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    char cand[16][64], cauth[256] = "";
    int cnt[16], nc = 0, xw = 0;
    time_t best_t = 0;
    while (!xw && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], comm[64] = "";
        struct stat pst, ast;
        snprintf(path, sizeof(path), "/proc/%.200s", de->d_name);
        if (stat(path, &pst) || pst.st_uid != uid) continue;
        snprintf(path, sizeof(path), "/proc/%.200s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(comm, sizeof(comm), f)) comm[0] = 0; fclose(f); }
        comm[strcspn(comm, "\n")] = 0;
        static char buf[65536];
        if (!strcmp(comm, "Xwayland")) {          /* argv: Xwayland :1 -rootless ... -auth FILE ... */
            snprintf(path, sizeof(path), "/proc/%.200s/cmdline", de->d_name);
            if (!(f = fopen(path, "r"))) continue;
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            const char *dsp = NULL, *au = NULL, *prev = "";
            for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
                if (buf[i] == ':' && !dsp) dsp = buf + i;
                if (!strcmp(prev, "-auth")) au = buf + i;
                prev = buf + i;
            }
            if (dsp && au && !stat(au, &ast)) {
                snprintf(disp, dn, "%s", dsp);
                snprintf(auth, an, "%s", au);
                xw = 1;
            }
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%.200s/environ", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *dp = NULL, *xa = NULL;
        for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
            if (!strncmp(buf + i, "DISPLAY=", 8)) dp = buf + i + 8;
            else if (!strncmp(buf + i, "XAUTHORITY=", 11)) xa = buf + i + 11;
        }
        if (!dp || !xa || !strstr(xa, ".mutter-Xwaylandauth") || stat(xa, &ast) || ast.st_mtime < best_t) continue;
        if (ast.st_mtime > best_t) { best_t = ast.st_mtime; nc = 0; snprintf(cauth, sizeof(cauth), "%s", xa); }
        int k = 0;
        while (k < nc && strcmp(cand[k], dp)) k++;
        if (k == nc && nc < 16) { snprintf(cand[nc], sizeof(cand[0]), "%s", dp); cnt[nc++] = 0; }
        if (k < nc) cnt[k]++;
    }
    closedir(d);
    if (xw) return 0;
    int b = -1;
    for (int k = 0; k < nc; k++) if (b < 0 || cnt[k] > cnt[b]) b = k;
    if (b < 0) return -1;
    snprintf(disp, dn, "%s", cand[b]);
    snprintf(auth, an, "%s", cauth);
    return 0;
}

static inline void rdx_x_desktop_env(void)
{
    if (getenv("DISPLAY")) return;
    DIR *d = opendir("/proc");
    struct dirent *de;
    int found = 0;
    uid_t shell_uid = (uid_t)-1;
    while (d && !found && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], comm[64] = "";
        snprintf(path, sizeof(path), "/proc/%.200s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, "gnome-shell") && strcmp(comm, "plasmashell") && strcmp(comm, "xfce4-session") &&
            strcmp(comm, "cinnamon") && strcmp(comm, "mate-session")) continue;
        struct stat pst;
        snprintf(path, sizeof(path), "/proc/%.200s", de->d_name);
        if (!stat(path, &pst) && pst.st_uid) shell_uid = pst.st_uid;
        snprintf(path, sizeof(path), "/proc/%.200s/environ", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        static char env[65536];
        size_t n = fread(env, 1, sizeof(env) - 1, f);
        fclose(f);
        env[n] = 0;
        for (size_t i = 0; i < n; i += strlen(env + i) + 1) {
            if (!strncmp(env + i, "DISPLAY=", 8)) { setenv("DISPLAY", env + i + 8, 1); found = 1; }
            else if (!strncmp(env + i, "XAUTHORITY=", 11)) setenv("XAUTHORITY", env + i + 11, 1);
        }
    }
    if (d) closedir(d);
    if (!found && shell_uid != (uid_t)-1) {     /* Wayland: the shell has no DISPLAY; Xwayland's */
        static char xd[64], xa[256];
        if (!rdx_xwayland_env(shell_uid, xd, sizeof(xd), xa, sizeof(xa))) {
            setenv("DISPLAY", xd, 1);
            setenv("XAUTHORITY", xa, 1);
        }
    }
    fprintf(stderr, "[x] desktop session: DISPLAY=%s XAUTHORITY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(none)",
            getenv("XAUTHORITY") ? getenv("XAUTHORITY") : "(none)");
}

/* the frame fw x fh letterboxed into a window win_w x win_h: its rectangle in window pixels (top left) */
static inline void rdx_letterbox(int win_w, int win_h, unsigned fw, unsigned fh, int *x, int *y, int *w, int *h)
{
    double sx = fw ? (double)win_w / fw : 1, sy = fh ? (double)win_h / fh : 1, sc = sx < sy ? sx : sy;
    *w = (int)(fw * sc + 0.5);
    *h = (int)(fh * sc + 0.5);
    if (*w > win_w) *w = win_w;
    if (*h > win_h) *h = win_h;
    *x = (win_w - *w) / 2;
    *y = (win_h - *h) / 2;
}

/* the event mask the input translation needs on the window */
#define RDX_XIN_MASK (KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | \
                      FocusChangeMask | EnterWindowMask)

struct rdx_xin {
    unsigned char kdown[32];          /* X keycodes down (forwarded) */
    int have_ptr, px, py;
    double fx, fy;                    /* sub-pixel remainder of scaled pointer motion */
};

typedef void (*rdx_xin_out)(void *user, int kind, int code, int value);

/* translate ev (window win_w x win_h showing frames of fw x fh); 1 = it was an input event */
static inline int rdx_xin_event(struct rdx_xin *xi, XEvent *ev, int win_w, int win_h, unsigned fw, unsigned fh,
                                rdx_xin_out out, void *user)
{
    if (ev->type == KeyPress && XLookupKeysym(&ev->xkey, 0) == XK_F11) return 1;   /* fullscreen: ours */
    if (ev->type == KeyPress || ev->type == KeyRelease) {
        unsigned kc = ev->xkey.keycode;          /* X keycode = evdev code + 8 */
        int down = ev->type == KeyPress, was = kc < 256 && (xi->kdown[kc / 8] >> (kc % 8)) & 1;
        if (kc >= 8 && kc < 256 && down != was) {
            if (down) xi->kdown[kc / 8] |= (unsigned char)(1u << (kc % 8));
            else xi->kdown[kc / 8] &= (unsigned char)~(1u << (kc % 8));
            out(user, RDX_LOCAL_IN_KEY, (int)kc - 8, down);
        }
        return 1;
    }
    if (ev->type == ButtonPress || ev->type == ButtonRelease) {
        int down = ev->type == ButtonPress;
        static const int btn[10] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, 0, 0, 0, 0, BTN_SIDE, BTN_EXTRA};
        unsigned b = ev->xbutton.button;
        if (b >= 4 && b <= 7) {                 /* wheel: one step per press */
            if (down) out(user, RDX_LOCAL_IN_REL, b <= 5 ? REL_WHEEL : REL_HWHEEL, (b == 4 || b == 7) ? 1 : -1);
        } else if (b < 10 && btn[b]) out(user, RDX_LOCAL_IN_BUTTON, btn[b], down);
        return 1;
    }
    if (ev->type == MotionNotify) {
        if (xi->have_ptr) {                      /* relative, in the frame's pixels */
            xi->fx += (ev->xmotion.x - xi->px) * (double)fw / (win_w ? win_w : 1);
            xi->fy += (ev->xmotion.y - xi->py) * (double)fh / (win_h ? win_h : 1);
            int dx = (int)xi->fx, dy = (int)xi->fy;
            xi->fx -= dx; xi->fy -= dy;
            if (dx) out(user, RDX_LOCAL_IN_REL, REL_X, dx);
            if (dy) out(user, RDX_LOCAL_IN_REL, REL_Y, dy);
        }
        xi->px = ev->xmotion.x; xi->py = ev->xmotion.y; xi->have_ptr = 1;
        return 1;
    }
    if (ev->type == EnterNotify) { xi->have_ptr = 0; return 1; }
    if (ev->type == FocusOut) {                  /* nothing stays pressed in the app */
        memset(xi->kdown, 0, sizeof(xi->kdown));
        out(user, 0, 0, 0);
        return 1;
    }
    return 0;
}
