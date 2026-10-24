/* SPDX-License-Identifier: MIT
 *
 * Variable refresh (FreeSync / VRR) on the source side of a gamescope app (rdx_gs_src).
 *
 * The client's screen has VRR and shows every frame the moment it arrives (rdx-hostd passes
 * --vrr <max Hz> from the client's session.start "display"). Then the client's vblank does not
 * pace the source any more: gamescope runs at the app's pace with its own adaptive sync logic
 * (the rdx connector reports VRR active, RDX_GS_FLAG_VRR), capped at the client's maximum, and
 * every composited frame is posted at once. Below the client's minimum the client repeats
 * frames itself (low framerate compensation); the source does nothing special.
 *
 * Fixed refresh instead (cap 0): no VRR asked for, tearing (the client has one front buffer
 * and never flips, a refresh rate does not exist there), frame generation (it fills the
 * vblanks of the client's fixed grid).
 *
 * The cap is a hard one, like the kernel's hold of an early VRR flip: a frame is never posted
 * sooner than one cap period after the previous one (rdx_gs_vrr_hold_until). gamescope marks
 * its vblank at the same moment, so its flip-ready test lets the app run at the cap.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* why a session runs at fixed refresh, for the log; NULL = VRR */
static inline const char *rdx_gs_vrr_off_reason(double vrr_max_hz, int tearing, int fg)
{
    if (vrr_max_hz <= 0) return "the client's screen has no variable refresh (or it is off there)";
    if (tearing) return "tearing mode (the client never flips, frames show on arrival)";
    if (fg) return "frame generation fills the client's fixed vblank grid";
    if (vrr_max_hz < 1) return "the client's maximum refresh is below 1 Hz";
    return NULL;
}

/* the cap in mHz for gamescope: the client's VRR maximum, never above the sink mode's refresh
 * (KMS stretches the vblank of the mode, it never refreshes faster than the mode); 0 = fixed */
static inline uint32_t rdx_gs_vrr_cap_mhz(double vrr_max_hz, uint32_t sink_mhz, int tearing, int fg)
{
    if (rdx_gs_vrr_off_reason(vrr_max_hz, tearing, fg)) return 0;
    double cap = vrr_max_hz * 1000.0;
    if (sink_mhz && cap > (double)sink_mhz) cap = (double)sink_mhz;
    return (uint32_t)(cap + 0.5);
}

static inline uint64_t rdx_gs_vrr_period_ns(uint32_t cap_mhz)
{
    return cap_mhz ? (uint64_t)(1e12 / (double)cap_mhz + 0.5) : 0;
}

/* When a frame that is ready at `now` may go out: 0 = now, else the time it is due, one cap period
 * after the previous post (last_post, 0 = none yet). Within 2 % of the period it goes now: the
 * client's kernel holds a flip that early for a few microseconds, a timer here would cost more. */
static inline uint64_t rdx_gs_vrr_hold_until(uint64_t last_post, uint64_t now, uint64_t period)
{
    if (!period || !last_post) return 0;
    const uint64_t due = last_post + period;
    if (now + period / 50 >= due) return 0;
    return due;
}
