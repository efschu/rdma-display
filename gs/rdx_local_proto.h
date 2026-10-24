/* Local sink: rdx_gs_src shows its frames in a window on this computer's own desktop
 * (p0b/rdx_sink_win --local) instead of sending them to a client over RDMA. No copy: the
 * window imports rdx_gs_src's host slots (dma-bufs of the same pages, udmabuf) into its GPU
 * (the iGPU shares the RAM) and draws from them. Same events as an RDMA sink (credits, "shown"
 * reports, drops), so pacing, suspend/resume and RECONFIG work unchanged.
 * "Send to the client" / "Back to this PC" are a detach + resume to the other kind of sink.
 *
 * Unix SOCK_SEQPACKET socket, the window listens (rdx_gs_src --peer local:PATH):
 *   sink -> src  struct rdx_hello (rdx_common.h): mode, stride, slots, vblank_ns; RDMA fields 0
 *   src  -> sink BUFS   + SCM_RIGHTS: one dma-buf per slot (slot order), n = slots,
 *                       a = width, slot = height of their frames (stride: width * 4 rounded to 256)
 *   src  -> sink FRAME  slot holds frame seq, posted at t_ns
 *   sink -> src  CREDIT slot free again (the window copied it into its back buffer)
 *   sink -> src  SHOWN  frame of slot visible since t_ns (after its vsync'ed swap);
 *                       a = arrival -> visible in ns
 *   sink -> src  DROP   slot's frame was replaced by a newer one before it was drawn
 *   sink -> src  MODE   the window was resized (debounced): a = width, b = height; rdx_gs_src
 *                       RECONFIGs gamescope to it and sends new BUFS
 *   sink -> src  INPUT  the window's own keyboard / mouse while it has the focus: slot = kind
 *                       (RDX_LOCAL_IN_*), n = evdev code, seq = value; rdx_gs_src injects it into a
 *                       uinput device named "rdx: local window ..." (gamescope's rdx backend takes
 *                       only "rdx: " devices, so the desktop's own keyboard never reaches the app)
 *   either       END
 * Generations: BUFS carries b = generation (+1 per BUFS); FRAME, CREDIT, SHOWN, DROP carry it in b,
 * and a report of an older generation (sent before the window had the new slots) is ignored.
 * Times are CLOCK_MONOTONIC of this computer. */
#pragma once
#include <stdint.h>

#define RDX_LOCAL_MAGIC 0x4c435852u     /* "RXCL" */
#define RDX_LOCAL_PREFIX "local:"

enum rdx_local_type {
    RDX_LOCAL_BUFS = 1,
    RDX_LOCAL_FRAME = 2,
    RDX_LOCAL_CREDIT = 3,
    RDX_LOCAL_SHOWN = 4,
    RDX_LOCAL_DROP = 5,
    RDX_LOCAL_END = 6,
    RDX_LOCAL_MODE = 7,
    RDX_LOCAL_INPUT = 8,
};

enum rdx_local_in { RDX_LOCAL_IN_KEY = 1, RDX_LOCAL_IN_REL = 2, RDX_LOCAL_IN_BUTTON = 3 };

struct rdx_local_msg {
    uint32_t magic, type;
    uint32_t slot, n;
    int64_t seq;
    uint64_t t_ns;
    uint64_t a, b;
};
