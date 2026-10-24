/* SPDX-License-Identifier: MIT
 * Sink slots for a paced stream (rdx_pace_slots, common/rdx_ctl.h): busy lead + period per slot,
 * half a period to spare. Cases from measurements on a seat (RTX 3080, NIC at PCIe Gen3 x4,
 * 1080p@240, lead 7.8-8.5 ms: 3 slots starved, 4 did not). */
#include "rdx_ctl.h"

#include <stdio.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

int main(void)
{
    CHECK(rdx_pace_slots(240, 8.134) == 4, "seat 2 at 240 Hz, lead 8.13 ms: 4 slots (3 starved)");
    CHECK(rdx_pace_slots(240, 7.8) == 4, "240 Hz, lead 7.8 ms: 4");
    CHECK(rdx_pace_slots(240, 0) == 4, "240 Hz, no measurement yet (default 8 ms): 4");
    CHECK(rdx_pace_slots(200, 0) == 4, "200 Hz, default lead: 4");
    CHECK(rdx_pace_slots(165, 0) == 3, "165 Hz, default lead: 3");
    CHECK(rdx_pace_slots(144, 8.13) == 3, "144 Hz, lead 8.13 ms: 3");
    CHECK(rdx_pace_slots(60, 8.13) == 3, "60 Hz: 3");
    CHECK(rdx_pace_slots(240, 5.0) == 3, "240 Hz, a fast link (lead 5 ms): 3");
    CHECK(rdx_pace_slots(240, 12.0) == 5, "240 Hz, lead 12 ms: 5");
    CHECK(rdx_pace_slots(500, 40.0) == 8, "lead far beyond the slots: capped at 8");
    CHECK(rdx_pace_slots(0, 8.0) == 3, "refresh unknown: 3 (the sink's default)");
    /* the boundary: lead + period == (n - 0.5) * period stays at n */
    CHECK(rdx_pace_slots(250, 6.0) == 3, "250 Hz, lead exactly 1.5 periods: 3");
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
