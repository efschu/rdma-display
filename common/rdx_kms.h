/* SPDX-License-Identifier: MIT
 *
 * Minimal KMS helpers on top of the kernel DRM UAPI (<drm/drm.h>,
 * <drm/drm_mode.h> from linux-libc-dev), so the game-mode sink builds without
 * libdrm-dev. Covers exactly what the sink needs: master, resources,
 * connector/encoder lookup, PRIME import, ADDFB2, SETCRTC, PAGE_FLIP and
 * flip-complete events.
 */
#pragma once
#include <stdint.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <stddef.h>

#define RDX_CONN_CONNECTED 1   /* drm_mode_get_connector.connection */
#ifndef DRM_PLANE_TYPE_CURSOR  /* plane "type" property values (kernel enum, also in libdrm) */
#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2
#endif

struct rdx_kms_out {
    uint32_t conn_id, crtc_id;
    struct drm_mode_modeinfo mode;
};

int rdx_kms_set_master(int fd);
int rdx_kms_monotonic_ts(int fd);
/* first connected connector, its preferred mode and a usable CRTC */
/* print every mode of every connected connector (size, refresh, pixel clock) */
int rdx_kms_list_modes(int fd);
/* the same as one JSON line on stdout for the first connected connector, plus the derived modes
 * rdx_kms_pick_output can set: {"card","connector","modes":[{"w","h","hz","derived","preferred"}]} */
int rdx_kms_list_modes_json(int fd, const char *card);
/* refresh rate from the timings: clock * 1000 / (htotal * vtotal), e.g. 143.998 */
double rdx_kms_mode_hz(const struct drm_mode_modeinfo *m);
/* monitor limits from the EDID range-limits descriptor (0xFD); 0 = not stated */
struct rdx_kms_range { uint32_t vmin_hz, vmax_hz, max_clock_khz; };
/* the range limits of a connector's EDID; -1 if it has no EDID or no range descriptor */
int rdx_kms_edid_range(int fd, uint32_t conn_id, struct rdx_kms_range *r);
/* the same from EDID bytes (a connector's blob, /sys/class/drm/<card>-<conn>/edid) */
int rdx_edid_range_bytes(const uint8_t *e, size_t n, struct rdx_kms_range *r);
/* variable refresh (VRR, FreeSync, G-Sync Compatible) on crtc_id when the connector says
 * vrr_capable = 1: sets the CRTC's VRR_ENABLED (also from a legacy client: it is no atomic-only
 * property) and reads it back. 1 = on, 0 = the connector cannot, -1 = the driver refused */
int rdx_kms_vrr_enable(int fd, uint32_t conn_id, uint32_t crtc_id, int on);
/* low framerate compensation under VRR: the frame on screen is shown again at 90 % of the longest
 * refresh (1 / vrr_min) after the later of the last flip completion (when its event was READ) and
 * the last frame's arrival, both CLOCK_MONOTONIC of the sink. Never from the event's vblank
 * timestamp: amdgpu quantizes that to the nominal grid under VRR. 0 = no LFC (no range) */
/* the driver repeats frames below the range itself, so the sink must not: amdgpu's freesync module
 * (amd/display/modules/freesync/freesync.c, "below the range" BTR) does it whenever the range spans
 * a factor of two (max >= 2 x min; amdgpu_dm sets config.btr = true for every VRR CRTC). A second
 * LFC in the sink only adds latency there: a frame arriving during its repeat flip waits for it
 * (measured on the test host, amdgpu, launcher at 25 fps: arrival -> flip p99 6.1 ms instead of 1.8). */
static inline int rdx_driver_does_lfc(const char *driver, double vrr_min, double vrr_max)
{
    return driver && !__builtin_strcmp(driver, "amdgpu") && vrr_min > 0 && vrr_max >= 2 * vrr_min;
}
int rdx_kms_driver_name(int fd, char *name, size_t n);   /* DRM_IOCTL_VERSION; 0 = ok */
static inline uint64_t rdx_lfc_due_ns(uint64_t t_flip_done, uint64_t t_last_frame, double vrr_min)
{
    if (vrr_min <= 0 || !t_flip_done || !t_last_frame) return 0;
    return (t_flip_done > t_last_frame ? t_flip_done : t_last_frame) + (uint64_t)(0.9e9 / vrr_min);
}
/* the cursor kick under VRR (rdx_sink_kms, where the driver does not repeat frames itself): when the frame on
 * screen may be flipped once more for a cursor change, two periods of the fastest refresh after the last
 * frame's arrival (a game's own frames carry the cursor); UINT64_MAX = not now (wanted 0: nothing changed, no
 * VRR, a flip outstanding or a frame waiting; no frame or flip yet) */
static inline uint64_t rdx_cursor_kick_due_ns(int wanted, uint64_t t_last_frame, uint64_t t_flip_done, double vrr_max,
                                              uint64_t period_ns)
{
    if (!wanted || !t_last_frame || !t_flip_done) return UINT64_MAX;
    return t_last_frame + (vrr_max > 0 ? (uint64_t)(2e9 / vrr_max) : 2 * period_ns);
}
/* a due time against the clock. Read the clock in its own statement AFTER the due time: in
 * "rdx_now_ns() >= due_fn()" or "f(due_fn(), rdx_now_ns())" the order of the two calls is unspecified, and a
 * due time of "now" computed after the clock was read is in its future, so the deadline is never reached */
static inline int rdx_due_reached(uint64_t due, uint64_t now) { return due != UINT64_MAX && now >= due; }
/* a mode with base's timings (porches, sync, totals) and only the pixel clock scaled so the
 * refresh is hz (fractional allowed; the clock is rounded to whole kHz, so the result is
 * within ~0.0005 Hz). Refused (-1, reason in why) if the clock exceeds the range's max pixel
 * clock or hz is outside its vertical range; a zero limit is not checked. */
int rdx_kms_synth_mode(const struct drm_mode_modeinfo *base, double hz, const struct rdx_kms_range *range,
                       struct drm_mode_modeinfo *out, char *why, size_t whyn);
/* want_w/want_h = 0: preferred mode; otherwise the highest-refresh mode of that size, or
 * the refresh want_hz if given (fractional allowed, e.g. 143.998 to match a host output):
 *   1. a listed mode of that size within 0.01 Hz (or with exactly that integer vrefresh),
 *   2. derived: a listed mode whose refresh x k (k <= 8) is want_hz (pixel clock x k, e.g.
 *      1080p120 -> 1080p240 at 594 MHz like CTA-861),
 *   3. synthesized: the listed mode of that size with the closest refresh, pixel clock scaled
 *      to want_hz (rdx_kms_synth_mode), within the EDID range limits.
 * Modes 2 and 3 are not listed by the monitor, it may still refuse them. */
int rdx_kms_pick_output(int fd, struct rdx_kms_out *out, uint32_t want_w, uint32_t want_h, double want_hz);
int rdx_kms_prime_import(int fd, int dmabuf_fd, uint32_t *handle);
int rdx_kms_add_fb_linear(int fd, uint32_t handle, uint32_t w, uint32_t h, uint32_t stride,
                          uint32_t fourcc, uint32_t *fb_id);
/* a scanout buffer of the driver's own (DRM dumb buffer, w x h, 32 bpp), mapped for the CPU: for a
 * driver that refuses framebuffers from imported dma-bufs (amdgpu); NULL on failure */
void *rdx_kms_dumb_create(int fd, uint32_t w, uint32_t h, uint32_t *handle, uint32_t *pitch, uint64_t *size);
int rdx_kms_set_crtc(int fd, const struct rdx_kms_out *out, uint32_t fb_id);
int rdx_kms_page_flip(int fd, uint32_t crtc_id, uint32_t fb_id, int async, uint64_t user_data);
/* reads pending events; returns the number of completed flips, last vblank time in *tv_ns */
int rdx_kms_read_flips(int fd, uint64_t *tv_ns);

/* ---- capture side (source host): find what an output scans out right now ---- */

struct rdx_kms_cap_out {
    uint32_t conn_id, crtc_id, plane_id;
    char name[32];                  /* e.g. "DP-1" */
    uint32_t x, y, w, h, vrefresh;  /* CRTC position in the framebuffer and mode size */
    double hz_exact;                /* refresh from the timings: clock * 1000 / (htotal * vtotal) */
};

/* enable universal planes so primary planes are listed */
int rdx_kms_universal_planes(int fd);
/* connected outputs with an active CRTC and their primary plane; returns count */
int rdx_kms_list_cap_outputs(int fd, struct rdx_kms_cap_out *out, int max);
/* tell the kernel a framebuffer's content changed without a flip (whole buffer); needed when
 * another device writes into the buffer being scanned out. 0 = ok */
int rdx_kms_dirty_fb(int fd, uint32_t fb_id);
/* index of a CRTC in GETRESOURCES order (for vblank ioctls), -1 if unknown */
int rdx_kms_crtc_index(int fd, uint32_t crtc_id);
/* vblank of a CRTC without DRM master: time of the last one (returns at once) or of the next
 * one (sleeps until it). CLOCK_MONOTONIC ns. 0 = ok. */
int rdx_kms_last_vblank(int fd, int crtc_index, uint64_t *t_ns);
int rdx_kms_next_vblank(int fd, int crtc_index, uint64_t *t_ns);
/* current framebuffer of a plane (0 if none) */
uint32_t rdx_kms_plane_fb(int fd, uint32_t plane_id);
/* GETFB2; fails if the kernel returned no handle (needs root or DRM master) */
int rdx_kms_getfb2(int fd, uint32_t fb_id, struct drm_mode_fb_cmd2 *fb);
/* cursor plane of a CRTC (plane "type" property == cursor and usable on that CRTC);
 * needs universal planes + atomic client caps (both set here). 0 if none.
 * Also resolves the plane property ids used by rdx_kms_cursor_state. */
struct rdx_kms_cursor {
    uint32_t plane_id;
    uint32_t prop_crtc_x, prop_crtc_y, prop_crtc_w, prop_crtc_h;
};
int rdx_kms_find_cursor_plane(int fd, uint32_t crtc_id, struct rdx_kms_cursor *cur);
/* current cursor: fb (0 = hidden or on another CRTC) and its CRTC-relative rectangle */
int rdx_kms_cursor_state(int fd, const struct rdx_kms_cursor *cur, uint32_t crtc_id, uint32_t *fb_id,
                         int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);
/* export a GEM handle as dma-buf fd */
int rdx_kms_handle_to_fd(int fd, uint32_t handle, int *dmabuf_fd);
void rdx_kms_gem_close(int fd, uint32_t handle);
