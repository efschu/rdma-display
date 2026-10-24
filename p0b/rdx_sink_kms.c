/* SPDX-License-Identifier: MIT
 *
 * P0-B sink, game mode: own DRM master, no compositor, no drawing.
 * The NIC writes via RDMA into udmabuf buffers in host RAM; the very same
 * buffers are iGPU framebuffers (PRIME import + ADDFB2, plain kernel UAPI, no
 * libdrm needed) and are scanned out directly by page flips. A driver that takes no framebuffer
 * from an imported dma-buf (amdgpu) gets copy mode: a scanout buffer of its own (dumb buffer) per
 * slot, the frame copied into it before the flip (~1-2 ms per 2560x1440 frame).
 *
 * Slot life cycle: free -> written (CQE) -> ready -> flip requested ->
 * on screen -> free once the next flip has completed. If a new frame arrives
 * while an older one is still waiting, the older one is dropped.
 * On every completed flip the sink reports "shown" with CQE -> vblank.
 *
 * Late flip (default on, --no-late-flip): a legacy page flip cannot be
 * replaced once queued, so the flip is requested only `--flip-lead-us` before
 * the next vblank, always with the NEWEST ready frame. Without it, a flip is
 * requested as soon as the previous one completes, i.e. almost a full refresh
 * early, with whatever frame happens to be ready.
 *
 * Switch to a free VT first (chvt) so that the desktop releases DRM master.
 *
 *   rdx_sink_kms [--dev auto] [--bind 10.0.0.1] [--port 18650]
 *                [--card /dev/dri/card1] [--slots 3] [--mtu 4096]
 *                [--async] [--no-late-flip] [--flip-lead-us 2000] [--mode WxH[@Hz]] [--mode-fallback WxH[@Hz]]
 *                [--busy-poll] [--pm-qos-us 50] [--log-dir /var/log/rdx | -]
 *
 * Per-frame telemetry (arrival, drops, flips, hitches) goes to --log-dir, see
 * tools/rdx_hitch.py; the journal only gets a line for seconds with anomalies.
 *
 * Waiting is event driven by default: the loop sleeps on the CQ completion
 * channel and the DRM fd, with a timeout for the late-flip deadline, and keeps
 * CPUs out of deep idle states via PM QoS. --busy-poll spins instead.
 *                [--frames-max 100000]
 *
 * --list-modes prints every mode of the connected monitors; --list-modes-json prints one
 * JSON line for the monitor the sink would drive, including the derived modes (e.g.
 * 1080p240 from a listed 1080p120) for rdx-clientd. Once the bootstrap socket listens the
 * sink prints "[sink] listening on IP:PORT"; a supervisor starts the source only after it.
 *
 * --tearing: no vsync. One scanout buffer, set once with SETCRTC and never flipped, is the
 *   only slot (hello nslots = 1): the NIC writes every frame straight into the buffer being
 *   scanned out, so a frame shows from the next scanline on and may tear on motion. The credit
 *   and a "shown" report (time = arrival) go back right after the frame's CQE. Telemetry T
 *   lines carry the arrival and the CRTC's last vblank, i.e. where the beam was; the summary
 *   has arrival -> screen centre. Without --tearing the flip paths are unchanged.
 *
 * --mode WxH@Hz takes a fractional refresh (e.g. @143.998, a host output's exact rate) and
 * synthesizes an unlisted mode from the listed one of that size with the closest refresh by
 * scaling only the pixel clock, within the EDID range limits (rdx_kms_pick_output). A refused
 * --mode falls back to --mode-fallback when given ("auto" = the preferred mode).
 *
 * --vrr (rdx-clientd: default on, config "vrr"): variable refresh (FreeSync / VRR) when the monitor
 *   can (connector vrr_capable): the CRTC's VRR_ENABLED is set after the mode, every frame is flipped
 *   the moment it is ready (no late flip, the refresh follows the frames) and, below the monitor's
 *   minimum (EDID range limits), the frame on screen is flipped again (low framerate compensation)
 *   so the panel never runs out of its range. The sink prints, before it listens,
 *   "[sink] display vrr=1 min=48.000 max=144.000 hz=144.000" (vrr=0 without VRR), which rdx-clientd
 *   sends to the host as session.start "display".
 * --drm-fd N: drive the CRTC and connector of a DRM lease (p0b/rdx_lease: one monitor of a desktop
 *   taken from the X server) instead of opening --card; the fd is master of just those. The kernel
 *   adds the CRTC's primary plane only, so the in-stream menu finds no overlay plane there: the menu,
 *   the placeholder and the cards are then blended by the CPU into each frame before its flip (about
 *   1-2 ms for the 30x29-line panel, only while one shows), and over a still picture into a copy of
 *   it in the spare start buffer.
 * --menu-probe checks read-only (no DRM master) which overlay plane the menu would use.
 * --menu-sock PATH (set by rdx-clientd): in-stream menu on a DRM overlay plane, see
 * rdx_overlay.h. The sink reports its stats there once per second; a menu change rides on
 * the next stream flip as one atomic commit. Without the socket the sink runs as before.
 * Before the first frame (and once no frame came for RDX_MENU_STALL_MS) there is no flip to
 * ride on: a change is then its own commit over the buffer on screen, which is how clientd's
 * start placeholder appears over the black start buffer, and a placeholder the host asked for
 * (SHOW b = 2) over a still last frame; the commit with the next frame takes it off
 * (client/rdx_placeholder.h). A still picture alone shows nothing.
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <linux/udmabuf.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/rdx_kms.h"
#include "../common/rdx_cursor.h"
#include "rdx_overlay.h"
#include "../common/rdx_menu_ipc.h"

#define WR_RECV   0x200u
#define WR_CREDIT 0x400u
#define NRECV     32
#define NPROBE    4

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

struct kms {
    int fd;
    struct rdx_kms_out out;
    uint32_t fb[RDX_MAX_SLOTS + 1];
    int async;
    /* state: slot index, ns = black start buffer, -1 = none */
    int on_screen, pending, ready;
    uint64_t t_cqe[RDX_MAX_SLOTS];
    uint64_t t_flip_req[RDX_MAX_SLOTS];
    uint64_t last_vblank;
    uint64_t period_ns;            /* refresh period from the mode timings */
    int late_flip;
    uint64_t flip_lead_ns;
    uint32_t want_w, want_h;            /* --mode WxH[@Hz]; 0 = monitor's preferred mode */
    double want_hz;                     /* fractional allowed (e.g. 143.998: match a host output) */
    uint32_t fb_w, fb_h;                /* --mode-fallback WxH[@Hz], tried when --mode is refused */
    double fb_hz;
    int fb_set;                         /* --mode-fallback given ("auto" = preferred mode) */
    int tearing;                        /* --tearing: slot 0 is scanned out, never flipped */
    int crtc_idx;                       /* for WAIT_VBLANK (tearing telemetry) */
    int ov_busy;                        /* a menu commit without a stream flip is outstanding */
    uint64_t t_last_frame;              /* CQE of the last stream frame, 0 = none yet */
    int want_vrr, vrr;                  /* --vrr; VRR_ENABLED is on */
    double vrr_min, vrr_max;            /* the monitor's range (EDID), 0 = unknown */
    int lfc;                            /* the outstanding flip repeats the frame on screen */
    long lfc_flips, lfc_sec;
    int lfc_driver;                     /* the driver repeats frames below the range itself (amdgpu BTR) */
    /* CLOCK_MONOTONIC when the last flip completion (stream, LFC or menu) was read. LFC times from
     * here and from the last frame's arrival, never from the event's vblank timestamp: under VRR
     * amdgpu quantizes that to the nominal refresh grid and queues the event to the end of the
     * stretched front porch, so a repeat timed from it fired between frames that were flowing at
     * 139 fps (15.8 repeats/s measured) and each blocked the next real frame for a refresh */
    uint64_t t_flip_done;
    int drm_fd;                         /* --drm-fd: a lease's fd, -1 = open --card */
    int disp;                           /* the buffer actually scanned out (slot, or the start buffer) */
    /* copy mode: the driver refuses framebuffers from imported dma-bufs (amdgpu: "Cannot create
     * framebuffer from imported dma_buf", amdgpu_display_user_framebuffer_create since Linux 5.12),
     * so each buffer has a scanout twin of the driver's own (dumb buffer) and a frame is copied
     * into it before its flip */
    int copy;
    uint8_t *scan[RDX_MAX_SLOTS + 1];
    uint32_t scan_pitch;
    uint64_t copies, copy_ns, copy_max_ns;
};

/* where the beam is at t: time since the start of the active area of the current refresh
 * (DRM vblank timestamps mark the end of vblank = first active line) and the wait until it
 * next passes line `row` (both ns); 0 = ok */
static int beam_at(const struct kms *k, uint64_t t, uint64_t tv, uint32_t row, double *pos, double *wait)
{
    const struct drm_mode_modeinfo *m = &k->out.mode;
    if (!m->clock || !k->period_ns) return -1;
    double line = (double)m->htotal * 1e6 / m->clock, per = (double)k->period_ns;
    double d = (double)t - (double)tv;
    d -= per * (double)(long)(d / per);
    if (d < 0) d += per;
    double w = row * line - d;
    if (w < 0) w += per;
    *pos = d;
    *wait = w;
    return 0;
}

static struct kms K;
static struct rdx_overlay OV = {.sock = -1, .shown = -1, .want = -1};

/* late flip: the flip is due `flip_lead_ns` before the next vblank (0 = right away) */
static uint64_t kms_flip_deadline(const struct kms *k, uint64_t now)
{
    if (!k->late_flip || !k->last_vblank || !k->period_ns) return 0;
    uint64_t next = k->last_vblank + k->period_ns;
    while (next <= now) next += k->period_ns;
    return next > k->flip_lead_ns ? next - k->flip_lead_ns : 0;
}

static int kms_flip_due(const struct kms *k, uint64_t now)
{
    return now >= kms_flip_deadline(k, now);
}

/* when an LFC repeat is due (common/rdx_kms.h rdx_lfc_due_ns) */
static uint64_t lfc_due(const struct kms *k) { return rdx_lfc_due_ns(k->t_flip_done, k->t_last_frame, k->vrr_min); }

static int kms_init(struct kms *k, const char *card)
{
    if (k->drm_fd >= 0) {                /* a lease: already master of its CRTC and connector */
        k->fd = k->drm_fd;
        fcntl(k->fd, F_SETFD, FD_CLOEXEC);
        printf("[kms] DRM lease fd %d (one monitor of the desktop)\n", k->fd);
    } else if ((k->fd = open(card, O_RDWR | O_CLOEXEC)) < 0) {
        perror(card);
        return -1;
    }
    if (k->drm_fd < 0 && rdx_kms_set_master(k->fd)) {
        fprintf(stderr, "DRM SET_MASTER: %s (desktop still on this VT? chvt first)\n", strerror(errno));
        return -1;
    }
    if (!rdx_kms_monotonic_ts(k->fd))
        fprintf(stderr, "[kms] warning: flip timestamps are not CLOCK_MONOTONIC\n");
    if (rdx_kms_pick_output(k->fd, &k->out, k->want_w, k->want_h, k->want_hz)) {
        if (!k->fb_set) return -1;
        fprintf(stderr, "[kms] --mode refused, using --mode-fallback\n");
        if (rdx_kms_pick_output(k->fd, &k->out, k->fb_w, k->fb_h, k->fb_hz)) return -1;
    }
    const struct drm_mode_modeinfo *m = &k->out.mode;
    k->period_ns = m->clock ? (uint64_t)m->htotal * m->vtotal * 1000000ull / m->clock : 0;
    printf("[kms] connector %u, CRTC %u, mode %s @ %.4f Hz (period %.4f ms, vblank %.3f ms)\n",
           k->out.conn_id, k->out.crtc_id, m->name, rdx_kms_mode_hz(m), k->period_ns / 1e6,
           m->clock ? (double)m->htotal * (m->vtotal - m->vdisplay) / m->clock : 0.0);
    return 0;
}

static int kms_add_fb(struct kms *k, int idx, int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride)
{
    uint32_t handle;
    if (!k->copy) {
        if (rdx_kms_prime_import(k->fd, dmabuf_fd, &handle)) { perror("PRIME_FD_TO_HANDLE"); return -1; }
        if (!rdx_kms_add_fb_linear(k->fd, handle, w, h, stride, DRM_FORMAT_XRGB8888, &k->fb[idx])) return 0;
        if (idx || errno != EINVAL) { perror("ADDFB2"); return -1; }
        struct drm_gem_close gc = {.handle = handle};
        ioctl(k->fd, DRM_IOCTL_GEM_CLOSE, &gc);
        k->copy = 1;
        printf("[kms] the driver takes no framebuffer from an imported dma-buf (amdgpu): copy mode, each frame "
               "is copied into a scanout buffer of the driver's own before its flip\n");
    }
    uint64_t size;
    uint32_t pitch;
    if (!(k->scan[idx] = rdx_kms_dumb_create(k->fd, w, h, &handle, &pitch, &size))) { perror("CREATE_DUMB"); return -1; }
    memset(k->scan[idx], 0, size);
    k->scan_pitch = pitch;
    if (rdx_kms_add_fb_linear(k->fd, handle, w, h, pitch, DRM_FORMAT_XRGB8888, &k->fb[idx])) {
        perror("ADDFB2 (dumb buffer)");
        return -1;
    }
    close(dmabuf_fd);                    /* the NIC writes into the memfd itself, not through this */
    return 0;
}

/* ---------------- soft overlay: the menu blended into the frames (no overlay plane) ---------------- */

static struct { uint8_t *map; uint64_t slot_bytes, hdr_off; uint32_t stride, w, h; int ns; } SB;

static uint8_t *slot_ptr(int i) { return SB.map + (size_t)i * SB.slot_bytes; }

static void soft_after_copy(struct kms *k, int dst);

/* copy mode: the picture of buffer src into the scanout buffer dst (no-op otherwise); a software cursor
 * goes on top of the copy */
static void scan_copy(struct kms *k, int src, int dst)
{
    if (!k->copy) return;
    uint64_t t0 = rdx_now_ns();
    const uint8_t *s = slot_ptr(src);
    uint8_t *d = k->scan[dst];
    if (k->scan_pitch == SB.stride) memcpy(d, s, (size_t)SB.stride * SB.h);
    else
        for (uint32_t y = 0; y < SB.h; y++) memcpy(d + (size_t)y * k->scan_pitch, s + (size_t)y * SB.stride, (size_t)SB.w * 4);
    uint64_t dt = rdx_now_ns() - t0;
    k->copies++;
    k->copy_ns += dt;
    if (dt > k->copy_max_ns) k->copy_max_ns = dt;
    soft_after_copy(k, dst);
}

/* ---------------- the host's cursor, next to the picture (common/rdx_cursor.h) ----------------
 * Hardware: the CRTC's cursor plane through the legacy cursor ioctls (CURSOR2: picture + hotspot,
 * CURSOR MOVE), the way X servers and gamescope's DRM backend without atomic do it: a pointer that only
 * moves costs no flip, the plane is latched at the next refresh. A lease has the cursor plane only when
 * its lessor runs without universal planes (the kernel adds the CRTC's primary and cursor plane then,
 * drm_lease.c fill_object_idr) or lists the cursor plane, else the ioctl answers EACCES
 * (drm_mode_cursor_common checks the plane's lease); the probe at the start says which ("[kms] cursor:").
 * Under variable refresh a cursor update starts no refresh of its own: measured on the test host (amdgpu,
 * lease, 40-144 Hz) the panel kept its BTR rate, 62.6 Hz without frames and 60 Hz (2 x 30) under a 30 fps
 * app, with the cursor moving 30 or 180 times a second; the cursor shows at the next of those refreshes.
 * Where the driver repeats frames below the range itself (rdx_driver_does_lfc: amdgpu BTR) that is the
 * rate the cursor moves at, and the refresh does not follow the mouse. Elsewhere the panel may idle at
 * its minimum (the sink's own LFC at 90 % of it), so the frame on screen is flipped once more after a
 * cursor change (a "kick", like LFC), at most one flip at a time and not while frames flow;
 * RDX_CURSOR_KICK=1 / 0 forces it on / off (amdgpu with kicks: refreshes at 82.6 Hz mean, following the mouse).
 * Software (copy mode without a cursor plane, e.g. a lease of a lessor with universal planes): the cursor
 * is blended into the scanout copy of the buffer, never into the slot itself, so moving it means putting
 * the slot's pixels back under the old place and blending it at the new one, in the buffer on screen
 * (no copy of the frame), plus a kick under VRR. Without copy mode and without a cursor plane the sink
 * does not offer the channel: the source then draws the cursor into the frames as before. */
struct hwcur {
    int ok;                              /* the cursor ioctls work on this fd and CRTC */
    uint32_t w, h, pitch;                /* cursor buffer size (DRM_CAP_CURSOR_WIDTH/HEIGHT) */
    uint32_t handle[2];
    uint8_t *map[2];
    int cur;                             /* the buffer the plane shows */
    uint32_t serial;                     /* picture in map[cur], 0 = none */
    int shown;                           /* the plane is on */
    int32_t x, y;                        /* its top left as programmed */
    long sets, moves, fails;
};
static struct hwcur HC;
static struct rdx_cur_rx CR;             /* the channel; CR.st = the host's cursor */
static int g_cur_soft;                   /* the cursor is blended into the copies (no cursor plane) */
static int g_cur_kick = -1;              /* -1 auto, 0 never, 1 always (RDX_CURSOR_KICK) */
static int g_cur_dirty;                  /* changed since the last refresh: a kick may be due */
static long g_cur_kicks, g_cur_soft_draws, g_cur_msgs;
static struct { int on; int32_t x, y; int w, h; } g_soft_rect[RDX_MAX_SLOTS + 1];   /* drawn into scan[i] */

static int cursor_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && errno == EINTR);
    return r;
}

/* cursor buffers of the size the driver asks for, then a "plane off" as the test: 0 = usable */
static int hwcur_probe(struct kms *k)
{
    struct drm_get_cap cw = {.capability = DRM_CAP_CURSOR_WIDTH}, ch = {.capability = DRM_CAP_CURSOR_HEIGHT};
    HC.w = !cursor_ioctl(k->fd, DRM_IOCTL_GET_CAP, &cw) && cw.value ? (uint32_t)cw.value : 64;
    HC.h = !cursor_ioctl(k->fd, DRM_IOCTL_GET_CAP, &ch) && ch.value ? (uint32_t)ch.value : 64;
    if (HC.w > RDX_CUR_MAX) HC.w = RDX_CUR_MAX;
    if (HC.h > RDX_CUR_MAX) HC.h = RDX_CUR_MAX;
    for (int i = 0; i < 2; i++) {
        uint64_t size;
        if (!(HC.map[i] = rdx_kms_dumb_create(k->fd, HC.w, HC.h, &HC.handle[i], &HC.pitch, &size))) {
            fprintf(stderr, "[cursor] no cursor buffer: %s\n", strerror(errno));
            return -1;
        }
        memset(HC.map[i], 0, size);
    }
    struct drm_mode_cursor off = {.flags = DRM_MODE_CURSOR_BO, .crtc_id = k->out.crtc_id, .handle = 0};
    if (cursor_ioctl(k->fd, DRM_IOCTL_MODE_CURSOR, &off)) {
        fprintf(stderr, "[cursor] the CRTC's cursor plane is not ours (%s%s)\n", strerror(errno),
                errno == EACCES ? ": not part of the lease" : "");
        return -1;
    }
    HC.ok = 1;
    return 0;
}

/* the host's cursor onto the plane: a new picture into the other buffer, then picture and place in one
 * ioctl; a move alone is CURSOR MOVE */
static void hwcur_apply(struct kms *k, int changed)
{
    const struct rdx_cur_state *s = CR.st;
    int vis = s->have_pos && s->visible && s->serial;
    if (!vis) {
        if (HC.shown) {
            struct drm_mode_cursor off = {.flags = DRM_MODE_CURSOR_BO, .crtc_id = k->out.crtc_id, .handle = 0};
            if (cursor_ioctl(k->fd, DRM_IOCTL_MODE_CURSOR, &off)) HC.fails++;
            HC.shown = 0;
        }
        return;
    }
    int32_t x = s->x - s->hot_x, y = s->y - s->hot_y;
    if ((changed & RDX_CUR_CH_IMAGE) || !HC.shown || HC.serial != s->serial) {
        int b = HC.serial == s->serial && HC.serial ? HC.cur : !HC.cur;
        if (HC.serial != s->serial || !HC.serial) {
            uint32_t cw = s->w < HC.w ? s->w : HC.w, chh = s->h < HC.h ? s->h : HC.h;
            for (uint32_t r = 0; r < HC.h; r++) {
                uint32_t *d = (uint32_t *)(HC.map[b] + (size_t)r * HC.pitch);
                memset(d, 0, (size_t)HC.w * 4);
                if (r < chh) memcpy(d, s->px + (size_t)r * s->w, (size_t)cw * 4);
            }
        }
        struct drm_mode_cursor2 c2 = {.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE, .crtc_id = k->out.crtc_id,
                                      .x = x, .y = y, .width = HC.w, .height = HC.h, .handle = HC.handle[b],
                                      .hot_x = s->hot_x, .hot_y = s->hot_y};
        if (cursor_ioctl(k->fd, DRM_IOCTL_MODE_CURSOR2, &c2)) { HC.fails++; return; }
        HC.cur = b;
        HC.serial = s->serial;
        HC.shown = 1;
        HC.x = x;
        HC.y = y;
        HC.sets++;
        return;
    }
    if (x == HC.x && y == HC.y) return;
    struct drm_mode_cursor mv = {.flags = DRM_MODE_CURSOR_MOVE, .crtc_id = k->out.crtc_id, .x = x, .y = y};
    if (cursor_ioctl(k->fd, DRM_IOCTL_MODE_CURSOR, &mv)) { HC.fails++; return; }
    HC.x = x;
    HC.y = y;
    HC.moves++;
}

/* software cursor: the slot's pixels back under what was drawn into scan[i] */
static void soft_undraw(struct kms *k, int i)
{
    if (!g_soft_rect[i].on) return;
    const uint8_t *src = SB.map + (size_t)i * SB.slot_bytes;
    for (int r = 0; r < g_soft_rect[i].h; r++) {
        int y = g_soft_rect[i].y + r;
        if (y < 0 || y >= (int)SB.h) continue;
        int x0 = g_soft_rect[i].x < 0 ? 0 : g_soft_rect[i].x, x1 = g_soft_rect[i].x + g_soft_rect[i].w;
        if (x1 > (int)SB.w) x1 = (int)SB.w;
        if (x1 <= x0) continue;
        memcpy(k->scan[i] + (size_t)y * k->scan_pitch + (size_t)x0 * 4, src + (size_t)y * SB.stride + (size_t)x0 * 4,
               (size_t)(x1 - x0) * 4);
    }
    g_soft_rect[i].on = 0;
}

/* software cursor: the host's cursor blended into scan[i] (its copy of slot i is clean or undrawn) */
static void soft_draw(struct kms *k, int i)
{
    const struct rdx_cur_state *s = CR.st;
    if (!g_cur_soft || !s->have_pos || !s->visible || !s->serial) return;
    int32_t x = s->x - s->hot_x, y = s->y - s->hot_y;
    rdx_cur_blend(k->scan[i], k->scan_pitch, SB.w, SB.h, s->px, s->w, s->h, x, y);
    g_soft_rect[i].on = 1;
    g_soft_rect[i].x = x;
    g_soft_rect[i].y = y;
    g_soft_rect[i].w = s->w;
    g_soft_rect[i].h = s->h;
    g_cur_soft_draws++;
}

static void soft_after_copy(struct kms *k, int dst)
{
    g_soft_rect[dst].on = 0;             /* a fresh copy of the slot */
    soft_draw(k, dst);
}

/* a cursor message changed something (RDX_CUR_CH_* bits) */
static void cursor_changed(struct kms *k, int changed)
{
    if (HC.ok) hwcur_apply(k, changed);
    else if (g_cur_soft && k->copy && k->disp >= 0) {
        soft_undraw(k, k->disp);             /* in the buffer on screen: a tear at most inside the cursor's box */
        soft_draw(k, k->disp);
        rdx_kms_dirty_fb(k->fd, k->fb[k->disp]);
    }
    g_cur_dirty = 1;
}

/* under VRR a cursor change needs a refresh (kick, see above): when one is due, 0 = now, UINT64_MAX = none.
 * Not while frames flow: a kick holds the display for one refresh, a frame arriving meanwhile waits for it,
 * so it waits two periods of the fastest refresh after the last frame (a game's own frames carry the cursor) */
static uint64_t cursor_kick_due(const struct kms *k)
{
    int wanted = g_cur_dirty && g_cur_kick == 1 && k->vrr && !k->tearing && !k->ov_busy && k->pending < 0 && k->ready < 0;
    return rdx_cursor_kick_due_ns(wanted, k->t_last_frame, k->t_flip_done, k->vrr_max, k->period_ns);
}

/* menu buffer b (premultiplied ARGB) over the XRGB frame at dst, the lines written back for scanout */
static void soft_blend(uint8_t *dst, int b)
{
    const uint8_t *src = OV.smap[b];
    if (!src) return;
    for (uint32_t y = 0; y < OV.h && OV.y + (int)y < (int)SB.h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * OV.sstride);
        uint32_t *d = (uint32_t *)(dst + (size_t)(OV.y + (int)y) * SB.stride) + OV.x;
        uint32_t n = OV.w;
        if (OV.x + (int)n > (int)SB.w) n = SB.w - (uint32_t)OV.x;
        for (uint32_t x = 0; x < n; x++) {
            uint32_t p = s[x], a = p >> 24;
            if (!a) continue;
            if (a == 255) { d[x] = p; continue; }
            uint32_t q = d[x], ia = 255 - a;
            uint32_t r = ((p >> 16) & 255) + ((q >> 16) & 255) * ia / 255, g = ((p >> 8) & 255) + ((q >> 8) & 255) * ia / 255,
                     bl = (p & 255) + (q & 255) * ia / 255;
            d[x] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
        }
        for (uint32_t off = 0; off < n * 4; off += 64) __builtin_ia32_clflush((uint8_t *)d + off);
    }
    __builtin_ia32_mfence();
}

/* no stream flip to ride on: the frame on screen (or black) with the menu, in the start buffer */
static int soft_still_commit(struct kms *k)
{
    int b = rdx_overlay_current(&OV), target = k->on_screen;
    if (b >= 0 || k->on_screen == SB.ns) {
        uint8_t *start = slot_ptr(SB.ns);
        if (k->on_screen != SB.ns) memcpy(start, slot_ptr(k->on_screen), SB.hdr_off);
        else memset(start, 0, SB.hdr_off);           /* before the first frame: black under it */
        if (b >= 0) soft_blend(start, b);
        if (k->copy) scan_copy(k, SB.ns, SB.ns);
        else {
            for (uint64_t off = 0; off < SB.hdr_off; off += 64) __builtin_ia32_clflush(start + off);
            __builtin_ia32_mfence();
        }
        target = SB.ns;
    } else if (k->copy && g_cur_soft) {
        soft_undraw(k, target);              /* the copy on screen again: the cursor where it is now */
        soft_draw(k, target);
    }
    if (rdx_kms_page_flip(k->fd, k->out.crtc_id, k->fb[target], 0, (uint64_t)target)) return -1;
    k->disp = target;
    OV.shown = b;
    return 0;
}

static int kms_flip(struct kms *k, int slot)
{
    rdx_overlay_frame(&OV);             /* the first frame takes the start placeholder off with it */
    g_cur_dirty = 0;                    /* the next refresh shows the cursor as it is now */
    k->disp = slot;
    if (OV.soft) {                      /* the menu into this frame, then a plain flip */
        int b = rdx_overlay_current(&OV);
        if (b >= 0) soft_blend(slot_ptr(slot), b);
        OV.shown = b;
    }
    scan_copy(k, slot, slot);
    /* a pending menu change goes with this flip (atomic); on failure the menu is off */
    if (!OV.soft && rdx_overlay_pending(&OV) && !rdx_overlay_flip(&OV, k->fb[slot], (uint64_t)slot)) {
        k->pending = slot;
        k->t_flip_req[slot] = rdx_now_ns();
        return 0;
    }
    if (rdx_kms_page_flip(k->fd, k->out.crtc_id, k->fb[slot], k->async, (uint64_t)slot)) {
        if (k->async && errno == EINVAL) {
            fprintf(stderr, "[kms] async flip rejected, falling back to vblank flips\n");
            k->async = 0;
            return kms_flip(k, slot);
        }
        perror("PAGE_FLIP");
        return -1;
    }
    k->pending = slot;
    k->t_flip_req[slot] = rdx_now_ns();
    return 0;
}

/* "WxH" or "WxH@Hz", Hz may be fractional; "@Hz" alone is not a mode */
static int parse_mode(const char *arg, uint32_t *w, uint32_t *h, double *hz)
{
    char res[32] = {0};
    const char *at = strchr(arg, '@');
    snprintf(res, sizeof(res), "%.*s", at ? (int)(at - arg) : (int)strlen(arg), arg);
    *hz = 0;
    if (at) {
        char *end;
        *hz = strtod(at + 1, &end);
        if (end == at + 1 || *end || *hz <= 0) return -1;
    }
    return rdx_parse_res(res, w, h);
}

/* supervisors (rdx-clientd) stop the sink with SIGTERM: leave the loop and print the summary
 * (frames, errors, latencies) instead of dying silently; no SA_RESTART, the wait wakes up */
static volatile sig_atomic_t g_stop;
static void on_stop(int s) { (void)s; g_stop = 1; }

/* bootstrap: wait for the source like rdx_tcp_listen_accept_as(), but serve the menu socket
 * meanwhile, so clientd's start placeholder shows over the black start buffer even while the
 * source has not connected yet (the host is still starting the app) */
static int accept_source(struct kms *k, const char *bind_ip, int port)
{
    struct sockaddr_in a;
    int one = 1, lst = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(bind_ip);
    setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lst, (struct sockaddr *)&a, sizeof(a))) { perror("bind"); return -1; }
    if (listen(lst, 1)) { perror("listen"); return -1; }
    /* supervisors (rdx-clientd) start the peer only after this line */
    printf("[sink] listening on %s:%d\n", bind_ip, port);
    printf("[oob] waiting for source on %s:%d ...\n", bind_ip, port);
    fflush(stdout);
    for (;;) {
        struct pollfd p[3] = {{lst, POLLIN, 0}, {OV.sock, POLLIN, 0}, {k->fd, POLLIN, 0}};
        if (poll(p, 3, 1000) < 0 && errno != EINTR) { perror("poll"); close(lst); return -1; }
        if (p[0].revents) break;
        if (p[1].revents) rdx_overlay_handle(&OV);
        if (p[2].revents) {
            uint64_t vb = 0;
            rdx_kms_read_flips(k->fd, &vb);          /* a menu commit is done */
            k->ov_busy = 0;
        }
        if (!k->ov_busy && rdx_overlay_pending(&OV) &&
            !(OV.soft ? soft_still_commit(k) : rdx_overlay_flip(&OV, k->fb[k->on_screen], (uint64_t)k->on_screen)))
            k->ov_busy = 1;
    }
    int s = accept(lst, NULL, NULL);
    close(lst);
    if (s < 0) { perror("accept"); return -1; }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "10.0.0.1", *card = "/dev/dri/card1";
    int port = 18650, mtu = RDX_DEFAULT_MTU;
    uint32_t ns = 3;
    long frames_max = 100000;
    int busy = 0, pm_qos_us = 50, comp_vector = -1, list_modes = 0;
    const char *log_dir = RDX_TLOG_DIR, *menu_sock = NULL;
    int want_cursor = 1;                 /* --cursor auto: the host's cursor next to the picture when we can show it */
    K.late_flip = 1;
    K.flip_lead_ns = 2000000;
    K.drm_fd = -1;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"card", 1, 0, 'c'},
        {"slots", 1, 0, 's'}, {"mtu", 1, 0, 'm'}, {"async", 0, 0, 'a'},
        {"no-late-flip", 0, 0, 'N'}, {"flip-lead-us", 1, 0, 'l'},
        {"mode", 1, 0, 'M'}, {"frames-max", 1, 0, 'n'}, {"busy-poll", 0, 0, 'B'},
        {"pm-qos-us", 1, 0, 'Q'}, {"log-dir", 1, 0, 'T'}, {"comp-vector", 1, 0, 'V'},
        {"list-modes", 0, 0, 'L'}, {"mode-fallback", 1, 0, 'F'}, {"list-modes-json", 0, 0, 'J'},
        {"menu-sock", 1, 0, 'U'}, {"menu-probe", 0, 0, 'O'}, {"tearing", 0, 0, 'X'}, {"vrr", 0, 0, 'R'}, {"drm-fd", 1, 0, 'D'},
        {"cursor", 1, 0, 'C'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'c': card = optarg; break;
        case 's': ns = (uint32_t)atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'a': K.async = 1; break;
        case 'N': K.late_flip = 0; break;
        case 'l': K.flip_lead_ns = (uint64_t)atol(optarg) * 1000ull; break;
        case 'M':
            if (parse_mode(optarg, &K.want_w, &K.want_h, &K.want_hz)) { fprintf(stderr, "invalid --mode\n"); return 2; }
            break;
        case 'F':
            K.fb_set = 1;
            if (strcmp(optarg, "auto") && parse_mode(optarg, &K.fb_w, &K.fb_h, &K.fb_hz)) {
                fprintf(stderr, "invalid --mode-fallback\n"); return 2;
            }
            break;
        case 'n': frames_max = atol(optarg); break;
        case 'B': busy = 1; break;
        case 'Q': pm_qos_us = atoi(optarg); break;
        case 'T': log_dir = optarg; break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'L': list_modes = 1; break;
        case 'J': list_modes = 2; break;
        case 'U': menu_sock = optarg; break;
        case 'O': list_modes = 3; break;
        case 'X': K.tearing = 1; break;
        case 'R': K.want_vrr = 1; break;
        case 'D': K.drm_fd = atoi(optarg); break;
        case 'C':
            if (!strcmp(optarg, "picture")) want_cursor = 0;
            else if (strcmp(optarg, "auto")) { fprintf(stderr, "--cursor auto|picture\n"); return 2; }
            break;
        default:  fprintf(stderr, "see the header of rdx_sink_kms.c for options\n"); return 2;
        }
    }
    if (!ns || ns > RDX_MAX_SLOTS) { fprintf(stderr, "--slots 1..%d\n", RDX_MAX_SLOTS); return 2; }
    if (K.tearing) ns = 1;                /* the scanned-out buffer is the only slot */
    if (list_modes) {                     /* read-only, no DRM master needed */
        int fd = open(card, O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror(card); return 1; }
        if (list_modes == 3) {            /* --menu-probe: overlay plane for the in-stream menu */
            struct rdx_kms_out out;
            if (rdx_kms_pick_output(fd, &out, K.want_w, K.want_h, K.want_hz)) return 1;
            return rdx_overlay_probe(fd, out.crtc_id);
        }
        return (list_modes == 2 ? rdx_kms_list_modes_json(fd, card) : rdx_kms_list_modes(fd)) ? 1 : 0;
    }
    if (kms_init(&K, card)) return 1;
    uint32_t w = K.out.mode.hdisplay, h = K.out.mode.vdisplay;
    uint32_t stride = (uint32_t)align_up((uint64_t)w * 4, 256);
    uint64_t hdr_off = align_up((uint64_t)stride * h, 4096);
    uint64_t slot_bytes = align_up(hdr_off + sizeof(struct rdx_frame_hdr), 4096);
    uint64_t total = slot_bytes * (ns + 1);          /* + black start buffer, not exposed to the NIC */

    int mfd = memfd_create("rdx-kms", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, (off_t)total) || fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
        perror("memfd"); return 1;
    }
    uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    memset(map, 0, total);
    int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0) { perror("/dev/udmabuf"); return 1; }
    for (uint32_t i = 0; i <= ns; i++) {
        struct udmabuf_create uc = {.memfd = (uint32_t)mfd, .flags = UDMABUF_FLAGS_CLOEXEC,
                                    .offset = i * slot_bytes, .size = slot_bytes};
        int db = ioctl(ufd, UDMABUF_CREATE, &uc);
        if (db < 0) { perror("UDMABUF_CREATE"); return 1; }
        if (kms_add_fb(&K, (int)i, db, w, h, stride)) return 1;
    }
    if (K.copy)
        printf("[kms] %u+1 buffers in host RAM (memfd, stride %u) + %u+1 scanout buffers of the driver (pitch %u)\n", ns,
               stride, ns, K.scan_pitch);
    else
        printf("[kms] %u+1 framebuffers from udmabuf (host RAM, linear XRGB8888, stride %u)\n", ns, stride);
    /* tearing: slot 0 itself is scanned out from now on (starts black) */
    if (rdx_kms_set_crtc(K.fd, &K.out, K.fb[K.tearing ? 0 : ns])) { perror("SETCRTC"); return 1; }
    {                                     /* variable refresh: the refresh follows the frames */
        double hz = rdx_kms_mode_hz(&K.out.mode);
        struct rdx_kms_range rg;
        int has_range = !rdx_kms_edid_range(K.fd, K.out.conn_id, &rg);
        int v = K.want_vrr && !K.tearing ? rdx_kms_vrr_enable(K.fd, K.out.conn_id, K.out.crtc_id, 1) : 0;
        if (v == 1) {
            K.vrr = 1;
            K.late_flip = 0;              /* each frame at once: there is no fixed vblank to aim at */
            K.vrr_min = has_range && rg.vmin_hz ? rg.vmin_hz : 0;
            K.vrr_max = has_range && rg.vmax_hz && rg.vmax_hz < hz ? rg.vmax_hz : hz;
            char drv[32] = "";
            rdx_kms_driver_name(K.fd, drv, sizeof(drv));
            K.lfc_driver = rdx_driver_does_lfc(drv, K.vrr_min, K.vrr_max);
            printf("[kms] VRR on: %.0f-%.0f Hz (EDID range %s), frames flip on arrival, LFC below %.0f Hz %s\n", K.vrr_min,
                   K.vrr_max, has_range ? "read" : "missing", K.vrr_min,
                   K.lfc_driver ? "by the driver (amdgpu BTR), none by the sink" : "by the sink");
        } else if (K.want_vrr) {
            printf("[kms] VRR %s\n", v < 0 ? "refused by the driver" : K.tearing ? "off (tearing)" : "not supported by this monitor/driver");
        }
        printf("[sink] display vrr=%d min=%.3f max=%.3f hz=%.3f\n", K.vrr, K.vrr ? K.vrr_min : 0.0, K.vrr ? K.vrr_max : hz, hz);
        fflush(stdout);
    }
    K.on_screen = K.tearing ? 0 : (int)ns; K.pending = -1; K.ready = -1;
    K.disp = K.on_screen;
    SB.map = map; SB.slot_bytes = slot_bytes; SB.hdr_off = hdr_off; SB.stride = stride; SB.w = w; SB.h = h; SB.ns = (int)ns;
    K.crtc_idx = rdx_kms_crtc_index(K.fd, K.out.crtc_id);
    if (K.tearing) printf("[kms] TEARING mode: no vsync, the NIC writes into the scanned-out buffer (CRTC index %d)\n",
                          K.crtc_idx);
    uint32_t refresh_mhz = K.period_ns ? (uint32_t)(1e12 / (double)K.period_ns + 0.5) : 0;
    if (menu_sock) rdx_overlay_init(&OV, K.fd, K.out.crtc_id, menu_sock, w, h, refresh_mhz);

    struct ibv_context *ctx = rdx_open_dev_ip(dev, bind_ip);   /* dev "auto": the device carrying bind_ip */
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_mr *mr = ibv_reg_mr(pd, map, slot_bytes * ns, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!pd || !mr) { perror("pd/reg_mr"); return 1; }
    /* CQ with NIC completion timestamps: frame arrival per the NIC vs. when we looked */
    struct rdx_cq rcq;
    if (rdx_cq_create_ts(ctx, 256, comp_vector, &rcq)) return 1;
    struct ibv_comp_channel *ch = rcq.ch;
    struct ibv_cq *cq = rcq.cq;
    printf("[kms] NIC completion timestamps %s, completion vector %d of %d\n", rcq.cqx ? "on" : "unavailable",
           rdx_pick_comp_vector(ctx, comp_vector), ctx->num_comp_vectors);
    struct ibv_qp *qp = rdx_create_qp(pd, cq, 64);
    if (!qp || rdx_qp_to_init(qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(qp, WR_RECV)) { perror("post_recv"); return 1; }
    /* the cursor next to the picture: on the cursor plane, else blended into the copies (copy mode) */
    int cur_offer = 0;
    if (want_cursor) {
        int hw = !hwcur_probe(&K);
        g_cur_soft = !hw && K.copy;
        if ((hw || g_cur_soft) && !rdx_cur_rx_init(&CR, pd, cq)) cur_offer = 1;
        const char *ke = getenv("RDX_CURSOR_KICK");
        if (ke && *ke) g_cur_kick = atoi(ke) ? 1 : 0;
        else g_cur_kick = !K.lfc_driver;      /* the driver's own repeats (amdgpu BTR) already refresh often enough */
        printf("[kms] cursor: %s%s\n", !cur_offer ? "in the picture (no cursor plane, no copy mode)"
               : hw ? "offered, on the CRTC's cursor plane" : "offered, blended into the copies (no cursor plane)",
               cur_offer && K.vrr ? (g_cur_kick ? "; under VRR a cursor change flips the frame once more"
                                                 : "; under VRR it shows with the driver's own repeats (BTR)") : "");
    }

    struct rdx_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_MAGIC;
    hello.width = w; hello.height = h; hello.stride = stride; hello.fourcc = DRM_FORMAT_XRGB8888;
    hello.nslots = ns; hello.slot_bytes = slot_bytes; hello.hdr_off = hdr_off;
    hello.base_addr = (uintptr_t)map; hello.rkey = mr->rkey;
    int gidx = rdx_find_gid_index_ip(ctx, bind_ip, hello.conn.gid);   /* the GID of the seat's link IP */
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    hello.conn.qpn = qp->qp_num;
    hello.conn.psn = 0x1234;
    hello.vblank_ns = K.period_ns;
    if (cur_offer) hello.caps |= RDX_CAP_CURSOR;
    int sock = accept_source(&K, bind_ip, port);
    if (sock < 0) return 1;
    struct rdx_conn peer;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) || rdx_io_all(sock, &peer, sizeof(peer), 0)) return 1;
    int cur_up = rdx_cur_rx_connect(&CR, sock, &peer, gidx, hello.conn.gid, rdx_parse_mtu(mtu));
    if (cur_up < 0) return 1;
    if (rdx_qp_to_rtr_rts(qp, &peer, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    if (cur_offer)
        printf("[kms] cursor: %s\n", cur_up ? "next to the picture (the source sends it)"
               : "in the picture (the source takes no channel)");
    printf("[kms] connected (%s ::ffff:%u.%u.%u.%u), %s, late flip %s (lead %.2f ms)\n", dev,
           hello.conn.gid[12], hello.conn.gid[13], hello.conn.gid[14], hello.conn.gid[15],
           K.tearing ? "tearing (no flips)" : K.async ? "async flips" : "vblank flips", K.late_flip ? "on" : "off",
           K.flip_lead_ns / 1e6);
    fflush(stdout);

    double *lat_vbl = calloc((size_t)frames_max, sizeof(double));   /* CQE -> vblank */
    double *lat_req = calloc((size_t)frames_max, sizeof(double));   /* CQE -> flip requested */
    double *vbl_gap = calloc((size_t)frames_max, sizeof(double));   /* distance between flips */
    double *lat_ctr = calloc((size_t)frames_max, sizeof(double));   /* CQE -> beam passes the screen centre */
    double *beam_pos = calloc((size_t)frames_max, sizeof(double));  /* tearing: beam line at arrival */
    long nctr = 0;
    long nf = 0, nshown = 0, dropped = 0, bad_hdr = 0, bad_cpu = 0, bad_seq_order = 0, nlive = 0;
    uint32_t last_seq = 0xFFFFFFFFu;
    struct rdx_wc wc[16];
    int running = 1;
    rdx_stop_signals(on_stop);

    int qos_fd = rdx_pm_qos(pm_qos_us);
    printf("[kms] %s, PM QoS %s\n", busy ? "busy poll" : "event driven (CQ channel + DRM fd)",
           qos_fd >= 0 ? "on" : "off");
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns();
    /* telemetry: R = frame arrived, D = dropped (newer one arrived first), V = flip done,
     * H = hitch: a flip gap above the running mean gap (the frame interval) + half a vblank */
    struct rdx_tlog tl;
    rdx_tlog_open(&tl, log_dir, "kms-sink",
                  "R,seq,slot,t_cqe,t_nic\nD,seq,t_cqe_of_newer\n"
                  "V,seq,t_cqe,t_flip_req,t_vblank,gap_us,cqe_vbl_us\nH,seq,t_vblank,gap_us,mean_gap_us\n"
                  "T,seq,t_cqe,t_nic,t_last_vblank,beam_line,cqe_centre_us\n"
                  "C,t_cqe,t_nic,x,y,visible,changed");
    uint32_t slot_seq[RDX_MAX_SLOTS] = {0};
    double gap_mean = 0;
    int dirty_warned = 0;
    long hitches = 0, sec_hitch = 0, sec_drop = 0, sec_shown = 0;
    double sec_gap_max = 0;
    double sec_lat[1024];                /* CQE -> vblank of this second, for the menu's stats */
    int nsec_lat = 0;
    uint64_t t_sec = t0_wall;
    while (running && !g_stop) {
        uint64_t tnow = rdx_now_ns();
        rdx_tlog_tick(&tl, tnow);
        if (tnow - t_sec >= 1000000000ull) {
            rdx_cq_clock_sync(&rcq);
            if (sec_hitch || sec_drop)
                printf("[kms] ANOMALY %ld shown/s: hitches %ld (max gap %.2f ms, mean %.2f ms), dropped %ld, LFC repeats %ld\n",
                       sec_shown, sec_hitch, sec_gap_max, gap_mean / 1e3, sec_drop, K.lfc_sec), fflush(stdout);
            else if (K.lfc_sec && sec_shown >= K.vrr_min)   /* repeats although the frames came fast enough */
                printf("[kms] %ld shown/s, LFC repeats %ld (no frame for %.1f ms)\n", sec_shown, K.lfc_sec,
                       0.9e3 / K.vrr_min), fflush(stdout);
            K.lfc_sec = 0;
            if (OV.sock >= 0) {
                double p50 = 0;
                if (nsec_lat) {               /* small n: insertion sort is enough */
                    for (int i = 1; i < nsec_lat; i++)
                        for (int j = i; j > 0 && sec_lat[j - 1] > sec_lat[j]; j--) {
                            double t = sec_lat[j]; sec_lat[j] = sec_lat[j - 1]; sec_lat[j - 1] = t;
                        }
                    p50 = sec_lat[nsec_lat / 2];
                }
                rdx_overlay_stats(&OV, sec_shown, sec_hitch, sec_drop, p50, refresh_mhz);
            }
            if (K.t_last_frame && tnow - K.t_last_frame > RDX_MENU_STALL_MS * 1000000ull)
                rdx_overlay_stalled(&OV);        /* menu changes need their own commits now */
            nsec_lat = 0;
            t_sec = tnow;
            sec_hitch = sec_drop = sec_shown = 0;
            sec_gap_max = 0;
        }
        int activity = 0;
        /* a closed bootstrap socket means the source is gone (crash, restart): end the
         * session so a supervisor loop can accept the next one */
        struct pollfd psk[2] = {{.fd = sock, .events = POLLIN}, {.fd = OV.sock, .events = POLLIN}};
        if (poll(psk, 2, 0) > 0) {
            char b;
            if ((psk[0].revents & (POLLIN | POLLHUP)) && recv(sock, &b, 1, MSG_DONTWAIT) <= 0) {
                printf("[kms] source disconnected\n");
                break;
            }
            if (psk[1].revents) rdx_overlay_handle(&OV);      /* menu: buffers, show/hide */
        }
        /* flip events; only one flip is ever outstanding */
        struct pollfd pfd = {.fd = K.fd, .events = POLLIN};
        if (poll(&pfd, 1, 0) > 0) {
            uint64_t vb = 0;
            activity = 1;
            K.t_flip_done = rdx_now_ns();
            if (K.tearing) {
                rdx_kms_read_flips(K.fd, &vb);       /* menu commits only, nothing is flipped */
                K.ov_busy = 0;
            } else if (K.ov_busy) {
                rdx_kms_read_flips(K.fd, &vb);       /* the menu commit without a stream flip is done */
                K.ov_busy = 0;
            } else if (K.lfc) {
                rdx_kms_read_flips(K.fd, &vb);       /* the frame on screen once more (LFC): nothing to report */
                K.lfc = 0;
                K.pending = -1;
                K.last_vblank = vb ? vb : rdx_now_ns();
            } else if (rdx_kms_read_flips(K.fd, &vb) > 0 && K.pending >= 0) {
                int prev = K.on_screen, cur = K.pending;
                K.on_screen = cur; K.pending = -1;
                uint64_t cqe_to_vb = vb > K.t_cqe[cur] ? vb - K.t_cqe[cur] : 0;
                if (nshown < frames_max) {
                    lat_vbl[nshown] = cqe_to_vb / 1e6;
                    lat_req[nshown] = (K.t_flip_req[cur] - K.t_cqe[cur]) / 1e6;
                    vbl_gap[nshown] = K.last_vblank ? (vb - K.last_vblank) / 1e6 : 0;
                }
                double gap_us = K.last_vblank ? (vb - K.last_vblank) / 1e3 : 0;
                rdx_tlog_line(&tl, "V,%u,%llu,%llu,%llu,%.1f,%.1f", slot_seq[cur],
                              (unsigned long long)K.t_cqe[cur], (unsigned long long)K.t_flip_req[cur],
                              (unsigned long long)vb, gap_us, cqe_to_vb / 1e3);
                if (gap_us > 0) {
                    /* hitch = a gap more than half a vblank above the mean frame interval. The mean
                     * includes every gap: at 144 fps on 240 Hz the gaps alternate 1 and 2 vblanks
                     * by design, and a mean that skipped "long" gaps would lock onto 1 vblank and
                     * report every 2-vblank gap as a hitch */
                    double slack = K.period_ns ? K.period_ns / 2e3 : 0.5 * gap_mean;
                    if (gap_mean > 0 && gap_us > gap_mean + slack) {
                        hitches++;
                        sec_hitch++;
                        rdx_tlog_line(&tl, "H,%u,%llu,%.1f,%.1f", slot_seq[cur], (unsigned long long)vb,
                                      gap_us, gap_mean);
                    }
                    gap_mean = gap_mean > 0 ? 0.98 * gap_mean + 0.02 * gap_us : gap_us;
                    if (gap_us / 1e3 > sec_gap_max) sec_gap_max = gap_us / 1e3;
                }
                sec_shown++;
                if (nsec_lat < 1024) sec_lat[nsec_lat++] = cqe_to_vb / 1e3;
                if (nctr < frames_max && K.out.mode.clock)   /* + vblank end -> centre line */
                    lat_ctr[nctr++] = (cqe_to_vb + (double)K.out.mode.htotal * 1e6 / K.out.mode.clock * (h / 2)) / 1e6;
                K.last_vblank = vb;
                nshown++;
                if (rdx_post_imm_send(qp, rdx_shown_imm((uint32_t)cur, cqe_to_vb), WR_CREDIT)) {
                    perror("shown"); return 1;
                }
                if (prev < (int)ns &&
                    rdx_post_imm_send(qp, rdx_credit_untimed((uint32_t)prev, RDX_US_NONE), WR_CREDIT)) {
                    perror("credit"); return 1;
                }
            }
        }

        /* tearing: a menu change is its own atomic commit with the same primary buffer */
        if (K.tearing && !OV.soft && !K.ov_busy && rdx_overlay_pending(&OV) && !rdx_overlay_flip(&OV, K.fb[0], 0))
            K.ov_busy = 1;
        /* flip the newest ready frame once it is due */
        /* no stream flip to ride on (no frame yet, the stream stalled or paused): a menu or start
         * placeholder change is its own atomic commit with the buffer on screen */
        if (!K.tearing && !K.ov_busy && K.pending < 0 && K.ready < 0 && rdx_overlay_pending(&OV) &&
            (!K.t_last_frame || rdx_now_ns() - K.t_last_frame > 100000000ull) &&
            !(OV.soft ? soft_still_commit(&K) : rdx_overlay_flip(&OV, K.fb[K.on_screen], (uint64_t)K.on_screen)))
            K.ov_busy = 1;
        if (!K.tearing && !K.ov_busy && K.pending < 0 && K.ready >= 0 && kms_flip_due(&K, rdx_now_ns())) {
            int r = K.ready; K.ready = -1;
            if (kms_flip(&K, r)) return 1;
        }
        /* VRR below the monitor's minimum: show the frame on screen again before the panel has to
         * (low framerate compensation), at 90 % of the longest refresh it allows, once neither a
         * flip completed nor a frame arrived for that long (own clock, see t_flip_done); never
         * while a flip is outstanding or a frame waits */
        if (K.vrr && K.vrr_min > 0 && !K.lfc_driver && K.t_last_frame && K.t_flip_done && !K.ov_busy && K.pending < 0 &&
            K.ready < 0 && rdx_now_ns() > lfc_due(&K)) {
            if (rdx_kms_page_flip(K.fd, K.out.crtc_id, K.fb[K.disp], 0, (uint64_t)K.disp) == 0) {
                K.lfc = 1;
                K.pending = K.on_screen;
                K.lfc_flips++;
                K.lfc_sec++;
                g_cur_dirty = 0;
            }
        }
        /* the host's cursor changed and nothing else refreshes the screen soon (VRR): the frame once more */
        uint64_t kick_due = cursor_kick_due(&K);     /* first, then the clock (rdx_due_reached) */
        uint64_t kick_now = rdx_now_ns();
        if (rdx_due_reached(kick_due, kick_now) &&
            rdx_kms_page_flip(K.fd, K.out.crtc_id, K.fb[K.disp], 0, (uint64_t)K.disp) == 0) {
            K.lfc = 1;                    /* a repeat like LFC: nothing to report when it completes */
            K.pending = K.on_screen;
            g_cur_kicks++;
            g_cur_dirty = 0;
        }

        int nwc = rdx_cq_poll(&rcq, wc, 16);
        if (nwc == 0 && !busy && !activity) {
            ibv_req_notify_cq(cq, 0);
            nwc = rdx_cq_poll(&rcq, wc, 16);               /* closes the arm race */
            if (nwc == 0) {
                /* nothing to do: sleep until a CQE, a flip event or the late-flip deadline */
                uint64_t now = rdx_now_ns(), tmo = 500000000ull;   /* re-check the socket */
                if (OV.usable && OV.waiting) tmo = 100000000ull;    /* placeholder / menu without a stream */
                if (K.pending < 0 && K.ready >= 0) {
                    uint64_t d = kms_flip_deadline(&K, now);
                    tmo = d > now ? d - now : 0;
                }
                if (K.vrr && K.vrr_min > 0 && !K.lfc_driver && K.pending < 0 && K.t_last_frame && K.t_flip_done) {   /* the next LFC repeat */
                    uint64_t d = lfc_due(&K);
                    uint64_t t = d > now ? d - now : 0;
                    if (t < tmo) tmo = t;
                }
                uint64_t kd = cursor_kick_due(&K);          /* a cursor change waiting for its refresh */
                if (kd != UINT64_MAX && (kd > now ? kd - now : 0) < tmo) tmo = kd > now ? kd - now : 0;
                rdx_sleep_on(ch, K.fd, tmo);
                continue;
            }
        }
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }
        for (int k = 0; k < nwc && running; k++) {
            if (rdx_cur_wr(wc[k].wr_id)) {           /* the cursor channel: never fatal for the frames */
                int ch = rdx_cur_rx_complete(&CR, wc[k].wr_id, wc[k].status == IBV_WC_SUCCESS);
                g_cur_msgs++;
                if (ch < 0 && HC.shown) {         /* gone: no stale cursor of ours over the picture */
                    struct drm_mode_cursor off = {.flags = DRM_MODE_CURSOR_BO, .crtc_id = K.out.crtc_id, .handle = 0};
                    cursor_ioctl(K.fd, DRM_IOCTL_MODE_CURSOR, &off);
                    HC.shown = 0;
                }
                if (ch < 0) g_cur_soft = 0;
                if (ch > 0) {
                    cursor_changed(&K, ch);
                    rdx_tlog_line(&tl, "C,%llu,%llu,%d,%d,%d,%d", (unsigned long long)rdx_now_ns(),
                                  (unsigned long long)wc[k].hw_ns, CR.st->x, CR.st->y, CR.st->visible, ch);
                }
                continue;
            }
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[k].status),
                        (unsigned long long)wc[k].wr_id);
                return 1;
            }
            if (wc[k].opcode == IBV_WC_RECV) {
                if (ntohl(wc[k].imm_data) == RDX_IMM_END) running = 0;
                continue;
            }
            if (wc[k].opcode != IBV_WC_RECV_RDMA_WITH_IMM) continue;
            uint64_t t_cqe = rdx_now_ns();
            uint32_t imm = ntohl(wc[k].imm_data);
            uint32_t slot = rdx_imm_slot(imm), seq28 = rdx_imm_seq(imm);
            if (rdx_post_zero_recv(qp, WR_RECV)) { perror("repost"); return 1; }
            if (slot >= ns) { fprintf(stderr, "slot %u out of range\n", slot); return 1; }

            uint8_t *sp = map + (size_t)slot * slot_bytes;
            struct rdx_frame_hdr hdr;
            memcpy(&hdr, sp + hdr_off, sizeof(hdr));
            int live = hdr.magic == RDX_MAGIC_LIVE;
            if ((hdr.magic != RDX_MAGIC && !live) || rdx_seq28(hdr.seq) != seq28) bad_hdr++;
            uint32_t seq = hdr.seq;
            if (last_seq != 0xFFFFFFFFu && seq != last_seq + 1) bad_seq_order++;
            last_seq = seq;
            if (live) nlive++;
            uint32_t bx = rdx_bar_x(seq, w);
            uint32_t px[NPROBE] = {RDX_SEQ_BLK / 2, bx + RDX_BAR_W / 2, w / 2 + 7, w - 1};
            uint32_t py[NPROBE] = {RDX_SEQ_BLK / 2, h / 2, h / 3, h - 1};
            for (int i = 0; !live && i < NPROBE; i++) {      /* live content has no pattern */
                uint32_t v = *(uint32_t *)(sp + (size_t)py[i] * stride + (size_t)px[i] * 4) & 0x00FFFFFFu;
                if (v != rdx_expected_pixel(seq, w, h, px[i], py[i])) { bad_cpu++; break; }
            }
            nf++;
            K.t_cqe[slot] = t_cqe;
            K.t_last_frame = t_cqe;
            slot_seq[slot] = seq;
            rdx_tlog_line(&tl, "R,%u,%u,%llu,%llu", seq, slot, (unsigned long long)t_cqe,
                          (unsigned long long)wc[k].hw_ns);
            if (K.tearing) {
                rdx_overlay_frame(&OV);         /* on screen already: the placeholder goes with the next commit */
                scan_copy(&K, (int)slot, 0);    /* copy mode: into the scanned-out buffer */
                /* the NIC wrote into the buffer being scanned out, behind the kernel's back: i915
                 * scans a never-flipped buffer out of its compressed copy (FBC) and would keep
                 * showing the black start forever. DIRTYFB flushes it (frontbuffer tracking). */
                if (rdx_kms_dirty_fb(K.fd, K.fb[0]) && !dirty_warned) {
                    perror("[kms] DIRTYFB (picture may stay stale in tearing mode)");
                    dirty_warned = 1;
                }
                /* already on screen: give the slot back at once, "shown" at arrival */
                if (rdx_post_imm_send(qp, rdx_shown_imm(slot, 0), WR_CREDIT) ||
                    rdx_post_imm_send(qp, rdx_credit_untimed(slot, RDX_US_NONE), WR_CREDIT)) {
                    perror("credit"); return 1;
                }
                uint64_t tv = 0;
                double pos = 0, wait = 0;
                if (K.crtc_idx >= 0 && !rdx_kms_last_vblank(K.fd, K.crtc_idx, &tv) &&
                    !beam_at(&K, t_cqe, tv, h / 2, &pos, &wait)) {
                    double line = pos * K.out.mode.clock / ((double)K.out.mode.htotal * 1e6);
                    if (nctr < frames_max) { beam_pos[nctr] = line; lat_ctr[nctr++] = wait / 1e6; }
                    if (nsec_lat < 1024) sec_lat[nsec_lat++] = wait / 1e3;
                    rdx_tlog_line(&tl, "T,%u,%llu,%llu,%llu,%.0f,%.1f", seq, (unsigned long long)t_cqe,
                                  (unsigned long long)wc[k].hw_ns, (unsigned long long)tv, line, wait / 1e3);
                }
                nshown++;
                sec_shown++;
                continue;
            }
            if (K.ready >= 0) {          /* the older waiting frame will never be shown */
                if (rdx_post_imm_send(qp, rdx_credit_untimed((uint32_t)K.ready, RDX_US_DROP), WR_CREDIT)) {
                    perror("credit"); return 1;
                }
                rdx_tlog_line(&tl, "D,%u,%llu", slot_seq[K.ready], (unsigned long long)t_cqe);
                dropped++;
                sec_drop++;
            }
            K.ready = (int)slot;
        }
    }

    size_t n = K.tearing ? 0 : (size_t)(nshown < frames_max ? nshown : frames_max);   /* flip stats */
    printf("\n[kms] result %ux%u @ %u Hz, %u slots, %s, late flip %s (lead %.2f ms), MTU %d\n",
           w, h, K.out.mode.vrefresh, ns, K.tearing ? "TEARING (no flips)" : K.async ? "async flips" : "vblank flips",
           K.late_flip ? "on" : "off",
           K.flip_lead_ns / 1e6, mtu);
    printf("  frames received %ld (%ld live content, pattern checks skipped), shown %ld, dropped %ld\n",
           nf, nlive, nshown, dropped);
    printf("  errors: header=%ld  CPU probes=%ld  order=%ld\n", bad_hdr, bad_cpu, bad_seq_order);
    rdx_report("CQE -> flip requested", lat_req, n);
    rdx_report("CQE -> vblank (visible)", lat_vbl, n);
    if (n > 1) rdx_report("distance between flips", vbl_gap + 1, n - 1);
    rdx_report("CQE -> beam at screen centre", lat_ctr, (size_t)nctr);
    if (K.tearing && nctr) {      /* a frame that lands during the active area shows a tear line there */
        long active = 0;
        for (long i = 0; i < nctr; i++) active += beam_pos[i] < h;
        printf("  arrivals during the active area (tear line on screen if the content moved): %.1f %%\n",
               100.0 * active / nctr);
    }
    rdx_cpu_report("kms", t0_wall, t0_cpu);
    printf("[kms] hitches (flip gap > mean + half a vblank): %ld\n", hitches);
    if (K.vrr) {                         /* the desktop after us gets its CRTC without our VRR */
        if (K.lfc_driver) printf("[kms] VRR: below %.0f Hz the driver repeats frames (amdgpu BTR), the sink none\n", K.vrr_min);
        else printf("[kms] VRR: %ld repeated flips below %.0f Hz (LFC)\n", K.lfc_flips, K.vrr_min);
        rdx_kms_vrr_enable(K.fd, K.out.conn_id, K.out.crtc_id, 0);
    }
    if (HC.shown) {                      /* the desktop after us gets its CRTC without our cursor */
        struct drm_mode_cursor off = {.flags = DRM_MODE_CURSOR_BO, .crtc_id = K.out.crtc_id, .handle = 0};
        cursor_ioctl(K.fd, DRM_IOCTL_MODE_CURSOR, &off);
    }
    if (CR.up)
        printf("[kms] cursor next to the picture: %ld messages (%ld positions, %ld pictures, %ld invalid), %s: %ld pictures "
               "set, %ld moves, %ld failed ioctls, %ld drawn into copies; %ld refresh kicks\n", g_cur_msgs, CR.st->n_pos,
               CR.st->n_img, CR.st->n_bad, HC.ok ? "cursor plane" : "software", HC.sets, HC.moves, HC.fails,
               g_cur_soft_draws, g_cur_kicks);
    if (K.copy && K.copies)
        printf("[kms] copy mode: %llu frames copied into scanout buffers, mean %.2f ms, max %.2f ms\n",
               (unsigned long long)K.copies, K.copy_ns / 1e6 / K.copies, K.copy_max_ns / 1e6);
    rdx_tlog_close(&tl);
    rdx_overlay_fini(&OV);
    if (qos_fd >= 0) close(qos_fd);
    close(sock);
    return (bad_hdr || bad_cpu) ? 3 : 0;
}
