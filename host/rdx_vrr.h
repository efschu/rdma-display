/* SPDX-License-Identifier: MIT
 *
 * rdma-display: variable refresh (FreeSync / VRR) of a session, decided in rdx-hostd.
 *
 * The client reports its screen in session.start:
 *   "display": {"vrr": bool, "min_hz": float, "max_hz": float, "hz": float}
 * vrr = the screen has variable refresh and it is on there; min_hz..max_hz its range, hz the mode
 * it runs. With vrr the client shows every frame when it arrives and repeats frames itself below
 * min_hz. A gamescope app then runs at the app's pace instead of the client's vblank:
 * rdx_gs_src --vrr <cap Hz> (gs/rdx_gs_vrr.h), cap = max_hz, never above the mode (KMS stretches
 * the mode's vblank, it never refreshes faster). Everything else, and vrr false or no "display"
 * at all (an older client), stays at fixed refresh exactly as before.
 * On by default; "vrr": false in the host's "defaults" turns it off on this host.
 * A stream that joins the app on the seat's own monitor (picture monitor+stream) gets it only when
 * that monitor reports vrr_capable (RDX_VRR_JOIN_VRR): gamescope runs --adaptive-sync there ("monitor_vrr",
 * default on), the monitor flips at the app's pace and the client shows every frame on arrival.
 * The cap is a little below the screen's maximum ("vrr_cap_frac" in the defaults, 0.97: the usual
 * advice, so the rate stays inside the VRR range and the screen never has to hold a frame).
 */
#pragma once
#include "../third_party/cjson/cJSON.h"

#define RDX_VRR_CAP_FRAC_DEFAULT 0.97

enum rdx_vrr_kind {
    RDX_VRR_GAMESCOPE,        /* a gamescope app streamed to the client */
    RDX_VRR_MIRROR,           /* a whole screen: the host's monitor sets the pace */
    RDX_VRR_JOIN,             /* the stream joins the app on the seat's own monitor (its DRM clock) */
    RDX_VRR_JOIN_VRR,         /* the same, and that monitor has variable refresh (vrr_capable): its
                               * adaptive sync flips at the app's pace, the client shows every frame */
    RDX_VRR_LOCAL_WINDOW,     /* "Start here on this PC": a window on this desktop */
};

struct rdx_vrr {
    int reported;             /* the client sent "display" */
    int client_vrr;           /* display.vrr */
    double min_hz, max_hz, hz;   /* as reported (0 = not given) */
    int active;               /* the source runs at the app's pace, capped at cap_hz */
    double cap_hz;
    char why[192];            /* active: what happens; else why the refresh stays fixed */
};

/* display: the client's "display" object (NULL / not an object: not reported); cap_frac: the cap as a
 * fraction of the client's maximum (0.5 .. 1, else RDX_VRR_CAP_FRAC_DEFAULT), never below min_hz */
void rdx_vrr_decide(const cJSON *display, int host_allows, enum rdx_vrr_kind kind, int tearing, double cap_frac,
                    struct rdx_vrr *v);

/* the connector's DRM property vrr_capable on card ("/dev/dri/card1", conn "HDMI-A-1", as gamescope -O
 * names it): 1 / 0, -1 = cannot tell (no such card or connector). Opens the card node: call it only
 * while a DRM master holds the card (the seat's gamescope runs), else the open would make us master
 * for a moment (host/rdx_hostd.c, local_connector). */
int rdx_vrr_monitor_capable(const char *card, const char *conn);

/* {"active", "cap_hz", "why", "client": {vrr, min_hz, max_hz, hz} | null} */
cJSON *rdx_vrr_json(const struct rdx_vrr *v);
