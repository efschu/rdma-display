/* SPDX-License-Identifier: MIT
 *
 * The monitors of the desktop a window client runs on (XRandR 1.5 monitors, the outputs' current
 * modes), for p0b/rdx_sink_win: "match this monitor" modes in its mode list, the monitor a window
 * opens on (the remembered one, else the primary, else the one under the pointer), fullscreen there
 * when the frame has that monitor's size, and the monitor the window is on now.
 */
#pragma once
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#define RDX_MAX_MONS 8

struct rdx_mon {
    char name[32];                  /* connector, e.g. "DisplayPort-0" */
    int x, y, w, h;                 /* on the root window */
    double hz;                      /* refresh of its current mode, 0 = unknown */
    int primary;
};

/* the active monitors; returns how many */
static inline int rdx_x_monitors(Display *d, struct rdx_mon *m, int max)
{
    int n = 0, nm = 0;
    Window root = DefaultRootWindow(d);
    XRRMonitorInfo *mi = XRRGetMonitors(d, root, True, &nm);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(d, root);
    for (int i = 0; mi && i < nm && n < max; i++) {
        struct rdx_mon *o = &m[n++];
        memset(o, 0, sizeof(*o));
        char *nmz = XGetAtomName(d, mi[i].name);
        snprintf(o->name, sizeof(o->name), "%s", nmz ? nmz : "?");
        if (nmz) XFree(nmz);
        o->x = mi[i].x; o->y = mi[i].y; o->w = mi[i].width; o->h = mi[i].height;
        o->primary = mi[i].primary;
        if (res && mi[i].noutput > 0) {
            XRROutputInfo *oi = XRRGetOutputInfo(d, res, mi[i].outputs[0]);
            XRRCrtcInfo *ci = oi && oi->crtc ? XRRGetCrtcInfo(d, res, oi->crtc) : NULL;
            for (int k = 0; ci && k < res->nmode; k++) {
                const XRRModeInfo *mo = &res->modes[k];
                if (mo->id != ci->mode || !mo->hTotal || !mo->vTotal) continue;
                double v = (double)mo->dotClock / ((double)mo->hTotal * mo->vTotal);
                if (mo->modeFlags & RR_Interlace) v *= 2;
                if (mo->modeFlags & RR_DoubleScan) v /= 2;
                o->hz = v;
            }
            if (ci) XRRFreeCrtcInfo(ci);
            if (oi) XRRFreeOutputInfo(oi);
        }
    }
    if (res) XRRFreeScreenResources(res);
    if (mi) XRRFreeMonitors(mi);
    return n;
}

/* the monitor containing root point (x, y), -1 = none */
static inline int rdx_mon_at(const struct rdx_mon *m, int n, int x, int y)
{
    for (int i = 0; i < n; i++)
        if (x >= m[i].x && x < m[i].x + m[i].w && y >= m[i].y && y < m[i].y + m[i].h) return i;
    return -1;
}

/* where a new window goes: the monitor named want (the remembered one), else the one with the highest
 * refresh (the gaming monitor; a tie goes to the primary), else the primary, else the one under the
 * pointer: the same answer for the mode list (clientd's "preferred") and the window */
static inline int rdx_mon_pick(Display *d, const struct rdx_mon *m, int n, const char *want)
{
    for (int i = 0; want && want[0] && i < n; i++)
        if (!strcmp(m[i].name, want)) return i;
    int best = -1;
    for (int i = 0; i < n; i++)
        if (m[i].hz > 0 && (best < 0 || m[i].hz > m[best].hz + 0.5 || (m[i].hz > m[best].hz - 0.5 && m[i].primary))) best = i;
    if (best >= 0) return best;
    for (int i = 0; i < n; i++) if (m[i].primary) return i;
    Window r, c;
    int rx, ry, wx, wy;
    unsigned mask;
    if (d && XQueryPointer(d, DefaultRootWindow(d), &r, &c, &rx, &ry, &wx, &wy, &mask)) {
        int k = rdx_mon_at(m, n, rx, ry);
        if (k >= 0) return k;
    }
    return n ? 0 : -1;
}
