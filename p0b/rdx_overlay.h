/* SPDX-License-Identifier: MIT
 *
 * In-stream menu, sink side: shows buffers rendered by rdx-clientd on a DRM overlay
 * plane above the stream (i915: a sprite plane of the sink's pipe).
 *
 * The stream path stays as it is: the menu is never composited into stream frames
 * (no copy). A change of the overlay (show buffer k / hide) is applied together with
 * the next stream flip as ONE atomic commit (primary FB + overlay plane, nonblocking,
 * page-flip event), so it costs no extra vblank and the late flip / vblank lock keep
 * their timing. Every other flip stays a legacy page flip.
 *
 * Buffers: RDX_MENU_NBUF memfds from clientd (SCM_RIGHTS) -> udmabuf -> PRIME import
 * -> ADDFB2 ARGB8888 linear; the KMS default blend mode is premultiplied alpha.
 */
#pragma once
#include <stdint.h>
#include <time.h>

struct rdx_overlay {
    int drm_fd, sock;               /* sock: the menu socket of rdx-clientd, -1 = no menu */
    int usable;                     /* overlay plane found and the test commit passed */
    uint32_t crtc_id, plane_id, primary_id;
    /* property ids: overlay plane, primary plane */
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, pp_fb;
    uint32_t fb[2];                 /* RDX_MENU_NBUF framebuffers, 0 = none */
    uint32_t w, h;
    int32_t x, y;
    int shown;                      /* buffer on screen, -1 = hidden */
    int want;                       /* wanted buffer, -1 = hidden */
    int until_frame;                /* want is a placeholder: off with the next stream frame */
    int waiting;                    /* no stream frame yet, or none for RDX_MENU_STALL_MS, or a host event */
    uint64_t show_at;               /* CLOCK_MONOTONIC ns: a host-asked placeholder (SHOW b = 2) waits until
                                     * then; a frame before it cancels it unseen (a quick switch shows nothing) */
    long commits, failed;
    /* no overlay plane (a DRM lease has the primary plane only): the sink blends the buffer into the
     * frames itself (rdx_sink_kms soft_*); clientd's memfds mapped read-only */
    int soft;
    const uint8_t *smap[2];
    size_t slen;
    uint32_t sstride;
};

/* connect to the menu socket and find an overlay plane for crtc_id; never fatal:
 * returns 0 when the menu is usable, -1 otherwise (the sink runs without it) */
int  rdx_overlay_init(struct rdx_overlay *o, int drm_fd, uint32_t crtc_id, const char *sock_path,
                      uint32_t mode_w, uint32_t mode_h, uint32_t refresh_mhz);
/* the buffer to show now (want, or what is shown while a SHOW b = 2 waits its grace), -1 = none */
static inline int rdx_overlay_current(const struct rdx_overlay *o)
{
    if (!o->usable || o->want < 0) return -1;
    if (o->show_at) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec < o->show_at) return o->shown;
    }
    return o->want;
}
/* read-only check without DRM master (works while another sink drives the screen):
 * prints the overlay and primary plane the menu would use on crtc_id; 0 = usable */
int  rdx_overlay_probe(int drm_fd, uint32_t crtc_id);
/* read pending messages from clientd (call when o->sock is readable); -1 = socket gone */
int  rdx_overlay_handle(struct rdx_overlay *o);
/* an overlay change waits for the next flip */
static inline int rdx_overlay_pending(const struct rdx_overlay *o)
{
    if (!o->usable || o->want == o->shown) return 0;
    if (o->show_at) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec < o->show_at) return 0;
    }
    return 1;
}
/* the next stream flip with the pending overlay change: atomic, nonblocking, with a
 * DRM_EVENT_FLIP_COMPLETE carrying user_data like a legacy page flip; -1 = failed (the
 * overlay is disabled and the caller does a legacy flip instead) */
int  rdx_overlay_flip(struct rdx_overlay *o, uint32_t primary_fb, uint64_t user_data);
/* a stream frame goes on screen with the next flip (or, tearing, was just written): the first one
 * after none or a stall takes the start placeholder off in that same commit and tells clientd */
void rdx_overlay_frame(struct rdx_overlay *o);
/* no stream frame for RDX_MENU_STALL_MS: menu changes become their own commits; clientd is NOT
 * told (a still picture is no event; the placeholder comes only with the host's SHOW b = 2) */
void rdx_overlay_stalled(struct rdx_overlay *o);
/* the sink is leaving: take the menu off the screen (blocking commit) */
void rdx_overlay_fini(struct rdx_overlay *o);
/* once per second: stats for the menu's statistics line */
void rdx_overlay_stats(struct rdx_overlay *o, long shown_s, long hitches_s, long dropped_s,
                       double p50_us, uint32_t refresh_mhz);
