/* rdx_gs_src <-> gamescope "rdx" backend, over a unix SOCK_SEQPACKET socket.
 *
 * rdx_gs_src owns the output buffers, one per sink slot, linear, stride = sink stride:
 *  - RDX_GS_BUF_VRAM: CUDA VMM allocations, handed over as opaque fds; gamescope imports
 *    them as Vulkan images and composites into them; the NIC reads the same VRAM.
 *  - RDX_GS_BUF_HOST: pinned host memory (memfd, mlocked, registered with the NIC);
 *    gamescope maps the memfd and imports the pointer (VK_EXT_external_memory_host).
 *    The GPU writes the frame over PCIe (posted writes), the NIC reads host RAM: no
 *    peer read of VRAM at all.
 *
 *   src -> gs  HELLO  (+ SCM_RIGHTS: nslots opaque fds, slot order)
 *   gs  -> src ACK    row pitch of the imported images (must equal stride)
 *   src -> gs  FREE   slot may be composited into (initially all slots)
 *   src -> gs  VBLANK virtual vblank on CLOCK_MONOTONIC: the sink's vblank grid
 *                     minus network time and margin, i.e. "composite done by then"
 *   gs  -> src READY  slot composited and finished on the GPU (gamescope waited
 *                     for its own timeline), may be sent now
 *                     VRR (RDX_GS_FLAG_VRR): no VBLANK at all; gamescope paints when the app presents
 *                     (at most once per cap period, refresh_mhz) and rdx_gs_src posts each READY at
 *                     once, but never sooner than one cap period after the previous post (what the
 *                     kernel does with an early VRR flip); gamescope marks its vblank at that time
 *   gs  -> src HINT   what the app wants from the client's pointer, sent whenever it changes (slot =
 *                     RDX_GS_HINT_* bits); rdx_gs_src puts it into the header of every frame it
 *                     sends to an RDMA sink (RDX_HDR_F_REL_MOUSE). A gamescope without it never sends
 *                     one: the pointer stays absolute.
 *   src -> gs  CURSOR the client shows the cursor itself (slot 1: its sink took the cursor channel,
 *                     common/rdx_cursor.h) or not (slot 0, the start): gamescope draws no cursor into the
 *                     frames, a pointer that only moves paints nothing (gamescope's nested-backend rule,
 *                     INestedHints::ShouldPaintCursor), and it reports the cursor instead:
 *   gs  -> src CURPOS where the hotspot is in output pixels and whether the cursor shows, on every motion
 *   gs  -> src CURIMG the cursor's picture, in chunks (struct rdx_gs_curimg + pixels), on every change
 *                     A gamescope without it ignores CURSOR and keeps drawing the cursor.
 *   either     BYE    end of session
 * The same header is copied into the gamescope patch (src/Backends/rdx_gs_proto.h).
 */
#pragma once
#include <stdint.h>

#define RDX_GS_MAGIC     0x31534752u    /* "RGS1" */
#define RDX_GS_MAX_SLOTS 8

#define RDX_GS_BUF_VRAM  0
#define RDX_GS_BUF_HOST  1

/* hello flags */
#define RDX_GS_FLAG_TEARING 1u   /* the client shows frames on arrival (rdx_sink_kms --tearing): no vblank
                                 * pacing, gamescope composites and READYs as soon as the app presents */
#define RDX_GS_FLAG_DMABUF  2u   /* host buffers handed over as dma-bufs (udmabuf of the memfd slots, same
                                 * pages): for drivers that cannot import a host pointer of a shared
                                 * mapping (amdgpu userptr takes anonymous memory only); gamescope
                                 * imports them with VK_EXT_external_memory_dma_buf */
#define RDX_GS_FLAG_VRR     4u   /* the client's screen has variable refresh (FreeSync / VRR) and shows each
                                 * frame when it arrives: gamescope runs at the app's pace (its own adaptive
                                 * sync logic, the connector reports VRR active), refresh_mhz is the cap
                                 * (the client's maximum), and rdx_gs_src sends no VBLANK. Never together
                                 * with RDX_GS_FLAG_TEARING. A gamescope without it ticks on its own timer
                                 * at refresh_mhz. */

/* HINT: slot = bits */
#define RDX_GS_HINT_REL_MOUSE 1u /* the app wants relative mouse motion: its cursor is hidden and the pointer is
                                 * locked (gamescope's own rule for its nested backends, SetRelativeMouseMode);
                                 * 0 = absolute is fine (desktop apps, visible cursor) */

enum rdx_gs_type {
    RDX_GS_HELLO  = 1,
    RDX_GS_ACK    = 2,
    RDX_GS_FREE   = 3,
    RDX_GS_VBLANK = 4,
    RDX_GS_READY  = 5,
    RDX_GS_BYE    = 6,
    RDX_GS_RECONFIG = 7,     /* src -> gs: a struct rdx_gs_hello with type RECONFIG + new fds (resume
                              * at another client mode); gs drops its slots, imports these, ACKs */
    RDX_GS_HINT   = 8,       /* gs -> src: a struct rdx_gs_msg, slot = RDX_GS_HINT_* bits (see above) */
    RDX_GS_VRR    = 9,       /* src -> gs: variable refresh on (slot 1) or off (slot 0) from now on, same
                              * buffers (the client's screen changed it during the session, or a resume at
                              * the same mode); t_ns = the refresh in mHz gamescope runs at now: the cap with
                              * VRR, the sink's fixed refresh without. A gamescope without it ignores it. */
    RDX_GS_CURSOR = 10,      /* src -> gs: a struct rdx_gs_msg, slot 1 = the client shows the cursor (gamescope
                              * draws none and sends CURPOS / CURIMG, the whole state at once), 0 = draw it */
    RDX_GS_CURPOS = 11,      /* gs -> src: a struct rdx_gs_msg, slot = RDX_GS_CUR_VISIBLE, pad = the serial of the
                              * picture, seq = x | (uint64_t)y << 32 (int32 each: the hotspot in output pixels),
                              * t_ns = when the pointer got there */
    RDX_GS_CURIMG = 12,      /* gs -> src: a struct rdx_gs_curimg + len bytes of the picture */
    RDX_GS_INPUT  = 13,      /* src -> gs: a struct rdx_gs_msg, slot 0 = the stream is detached (the app waits in
                              * the background): let go of every input device (libinput suspended, grabs
                              * released), so the gamescope of the app in the stream gets the client's devices
                              * alone; slot 1 = attached again: read them. A gamescope without it ignores it
                              * (its binary then lacks the marker "RDX_GS_INPUT v1", rdx-hostd allows one app
                              * in the background only). */
};

#define RDX_GS_CUR_VISIBLE 1u    /* CURPOS slot: the cursor shows (an image, not hidden) */
#define RDX_GS_CUR_MAX     256   /* largest picture sent; a bigger one is clipped */
#define RDX_GS_CUR_CHUNK   16384 /* picture bytes per CURIMG packet */

/* CURIMG: the picture (ARGB8888 premultiplied like an X cursor, rows of w pixels) of serial, bytes
 * [off, off + len) of w * h * 4; complete with the packet that reaches the end. A new serial (or off 0)
 * starts it over. */
struct rdx_gs_curimg {
    uint32_t magic, type;
    uint32_t serial;
    uint16_t w, h, hot_x, hot_y;
    uint32_t off, len;
    uint32_t pad;
    uint64_t t_ns;
};

struct rdx_gs_hello {
    uint32_t magic, type;
    uint32_t width, height, stride;
    uint32_t drm_fourcc;         /* DRM_FORMAT_XRGB8888 = Vulkan B8G8R8A8 */
    uint32_t nslots;
    uint32_t refresh_mhz;        /* sink refresh in mHz (exact, from its vblank period) */
    uint64_t slot_size;          /* bytes of each VMM allocation (import size) */
    uint8_t  gpu_uuid[16];       /* the GPU that composites (and, for VRAM, owns the buffers) */
    uint32_t buf_kind;           /* RDX_GS_BUF_VRAM | RDX_GS_BUF_HOST */
    uint32_t flags;              /* RDX_GS_FLAG_* */
};

struct rdx_gs_ack {
    uint32_t magic, type;
    uint32_t row_pitch;
    int32_t  error;              /* 0 = ok, else errno-style */
    uint32_t path;               /* host buffers: 0 = composite straight into them, 1 = composite
                                  * into VRAM + one GPU copy into them (driver cannot import a
                                  * host pointer as an image) */
    uint32_t pad;
};

struct rdx_gs_msg {              /* FREE, VBLANK, READY, HINT, BYE */
    uint32_t magic, type;
    uint32_t slot, pad;          /* HINT: slot = RDX_GS_HINT_* bits; READY: pad = slot generation (incremented by each RECONFIG) */
    uint64_t seq;                /* READY: composite counter of gamescope */
    uint64_t t_ns;               /* VBLANK: virtual vblank; READY: GPU done; VRR: refresh in mHz */
    uint64_t t_begin_ns;         /* READY: wake-up of the paint that produced it */
};
