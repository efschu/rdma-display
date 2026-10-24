/* SPDX-License-Identifier: MIT
 * Variable refresh on the source side (gs/rdx_gs_vrr.h): the cap gamescope gets, when fixed
 * refresh stays (the regression guard: no --vrr, tearing, frame generation), and the hold that
 * keeps the posts at or below the cap. No GPU, NIC or gamescope. */
#include "../gs/rdx_gs_vrr.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

int main(void)
{
    /* fixed refresh: exactly as before the feature */
    CHECK(rdx_gs_vrr_cap_mhz(0, 240000, 0, 0) == 0, "no --vrr: fixed refresh (cap 0)");
    CHECK(rdx_gs_vrr_cap_mhz(-1, 240000, 0, 0) == 0, "a negative maximum: fixed");
    CHECK(rdx_gs_vrr_cap_mhz(240, 240000, 1, 0) == 0, "tearing: fixed (the client never flips)");
    CHECK(rdx_gs_vrr_cap_mhz(240, 240000, 0, 1) == 0, "frame generation: fixed (it fills the client's grid)");
    CHECK(rdx_gs_vrr_cap_mhz(0.5, 240000, 0, 0) == 0, "a maximum below 1 Hz: fixed");
    CHECK(rdx_gs_vrr_off_reason(0, 0, 0) && strstr(rdx_gs_vrr_off_reason(0, 0, 0), "no variable refresh"),
          "the reason without VRR: %s", rdx_gs_vrr_off_reason(0, 0, 0));
    CHECK(rdx_gs_vrr_off_reason(240, 1, 1) && strstr(rdx_gs_vrr_off_reason(240, 1, 1), "tearing"),
          "tearing named first: %s", rdx_gs_vrr_off_reason(240, 1, 1));
    CHECK(rdx_gs_vrr_off_reason(240, 0, 0) == NULL, "240 Hz VRR: no reason (on)");

    /* the cap: the client's maximum, never above the sink's mode */
    CHECK(rdx_gs_vrr_cap_mhz(240, 240000, 0, 0) == 240000, "240 Hz VRR at a 240 Hz mode: cap 240 Hz");
    CHECK(rdx_gs_vrr_cap_mhz(165, 240000, 0, 0) == 165000, "a range up to 165 Hz at a 240 Hz mode: cap 165 Hz");
    CHECK(rdx_gs_vrr_cap_mhz(240, 143999, 0, 0) == 143999, "a range up to 240 Hz at a 143.999 Hz mode: cap = the mode (%u)",
          rdx_gs_vrr_cap_mhz(240, 143999, 0, 0));
    CHECK(rdx_gs_vrr_cap_mhz(143.9992, 0, 0, 0) == 143999, "sink refresh unknown: the client's maximum, rounded to mHz (%u)",
          rdx_gs_vrr_cap_mhz(143.9992, 0, 0, 0));

    /* the period of the cap */
    CHECK(rdx_gs_vrr_period_ns(240000) == 4166667, "240 Hz: 4166667 ns (%llu)", (unsigned long long)rdx_gs_vrr_period_ns(240000));
    CHECK(rdx_gs_vrr_period_ns(0) == 0, "no cap: period 0 (never holds)");

    /* the hold: never sooner than one period after the previous post */
    const uint64_t P = 4166667, t = 1000000000ull;
    CHECK(rdx_gs_vrr_hold_until(0, t, P) == 0, "first frame: goes at once");
    CHECK(rdx_gs_vrr_hold_until(t, t + P, P) == 0, "one period later: at once");
    CHECK(rdx_gs_vrr_hold_until(t, t + 3 * P, P) == 0, "an app below the cap (3 periods): at once");
    CHECK(rdx_gs_vrr_hold_until(t, t + 1000000, P) == t + P, "1 ms after the previous post: held to the cap period");
    CHECK(rdx_gs_vrr_hold_until(t, t + P - P / 100, P) == 0, "1 %% early: within the tolerance, at once");
    CHECK(rdx_gs_vrr_hold_until(t, t + P - P / 20, P) == t + P, "5 %% early: held");
    CHECK(rdx_gs_vrr_hold_until(t, t + 1000000, 0) == 0, "fixed refresh (period 0): never held");
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
