/* SPDX-License-Identifier: MIT
 * Frame generation on a helper GPU inside rdx_gs_src's event loop (docs/MULTI-GPU.md).
 *
 * gamescope runs at the sink's full refresh and keeps all its slots. It composites at every
 * vblank, also when the app delivered nothing new (then the picture is the previous one). The
 * stage owns every post:
 *   - a composite equal to the previous one (sampled signature) is not sent: its slot goes back
 *     to gamescope at once; that vblank gets a generated frame instead,
 *   - new pictures come further apart than ~1.5 sink periods (the app runs slower than the
 *     sink): each one goes to the helper GPU, which makes the frame between it and the previous
 *     one; the generated frame is posted as soon as it is ready, the new picture is held behind
 *     it for half the app's frame time (whole sink periods), so they land in successive vblanks,
 *   - new pictures come about every vblank (the app keeps up with the sink): posted at once,
 *     nothing generated, as without frame generation (hysteresis between the two).
 * Sink slots are a pool: a frame (gamescope's or generated) goes into any free one; the slot's
 * credit returns whatever it carried (rdx_fgs_credit).
 * Host slots: the helper reads them directly (host pointer, or udmabuf on AMD/Intel). VRAM slots:
 * staged to host RAM by CUDA on the seat's GPU. */
#pragma once
#include "rdx_gsbuf.h"
#include "rdx_srcsess.h"

#include <stdint.h>

struct rdx_fgs;

struct rdx_fgs_cfg {
    const char *helper_pci;      /* the helper GPU ("0000:0e:00.0"); "" = the seat's own GPU */
    const char *quality;         /* "auto" | "quality" | "performance" */
    uint32_t width, height, stride;
    uint32_t sink_slots;         /* the sink's slot count (all of them stay gamescope's); >= 3 */
    int buf_kind;                /* RDX_GSBUF_HOST or RDX_GSBUF_VRAM (slots are CUDA VMM) */
    int cuda_ord;                /* the seat GPU's CUDA ordinal (VRAM slots) */
};

/* NULL = frame generation stays off (the reason is printed), the caller runs as before.
 * `buf` = rdx_gs_src's slots (one per sink slot). */
struct rdx_fgs *rdx_fgs_open(const struct rdx_fgs_cfg *c, struct rdx_srcsess *S, struct rdx_gsbuf *buf);
/* poll it for POLLIN; then call rdx_fgs_service */
int  rdx_fgs_fd(const struct rdx_fgs *f);
/* gamescope composited slot `slot`. 1 = the same picture as before: give the slot back to
 * gamescope now (not sent); 0 = the stage owns it until rdx_fgs_credit returns it; -1 = post
 * error. *seq: rdx_gs_src's frame counter, one per post. */
int  rdx_fgs_ready(struct rdx_fgs *f, uint32_t slot, long *seq);
/* helper results and due posts. 0 = ok, -1 = post error */
int  rdx_fgs_service(struct rdx_fgs *f, long *seq);
/* the sink's credit for `sink_slot`: >= 0 = the gamescope slot it carried (give it back),
 * -1 = it carried a generated frame or nothing; frames waiting for a slot go out (-2 = post error) */
int  rdx_fgs_credit(struct rdx_fgs *f, uint32_t sink_slot, long *seq);
/* 1 if frame `seq` went out as soon as it was ready (a generated frame, or a picture that is not
 * held): only these steer rdx_gs_src's lead loop */
int  rdx_fgs_lead_frame(const struct rdx_fgs *f, long seq);
/* the sink went away (or a QP recovery lost frames): queued posts are dropped, every sink slot
 * counts as free; the gamescope slots the stage held are the caller's again */
void rdx_fgs_detach(struct rdx_fgs *f);
/* counters and helper timings, one block */
void rdx_fgs_report(const struct rdx_fgs *f);
void rdx_fgs_close(struct rdx_fgs *f);
