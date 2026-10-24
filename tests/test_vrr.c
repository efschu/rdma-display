/* SPDX-License-Identifier: MIT
 * FreeSync / VRR range from EDID bytes (common/rdx_kms.c rdx_edid_range_bytes): the range-limits
 * descriptor (0xFD) of block 0, the EDID 1.4 +255 Hz offsets, no descriptor, a broken header;
 * when an LFC repeat is due (rdx_lfc_due_ns), whether the driver repeats itself (rdx_driver_does_lfc).
 *   tests/test_vrr      exit 0 = PASS */
#include "../common/rdx_kms.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static void edid(uint8_t *e, int slot, uint8_t flags, uint8_t vmin, uint8_t vmax, uint8_t clk10)
{
    static const uint8_t magic[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memset(e, 0, 128);
    memcpy(e, magic, 8);
    uint8_t *d = e + 54 + 18 * slot;           /* a display descriptor: 0 0 0 tag */
    d[3] = 0xFD;
    d[4] = flags;
    d[5] = vmin;
    d[6] = vmax;
    d[7] = 30;                                 /* horizontal kHz, not used */
    d[8] = 255;
    d[9] = clk10;
}

int main(void)
{
    uint8_t e[128];
    struct rdx_kms_range r;
    edid(e, 3, 0, 48, 144, 60);                /* a FreeSync monitor: 48-144 Hz, 600 MHz */
    check(!rdx_edid_range_bytes(e, sizeof(e), &r) && r.vmin_hz == 48 && r.vmax_hz == 144 && r.max_clock_khz == 600000,
          "0xFD descriptor: 48-144 Hz, 600 MHz");
    edid(e, 1, 0x02, 48, 5, 90);               /* EDID 1.4: max + 255 -> 260 Hz */
    check(!rdx_edid_range_bytes(e, sizeof(e), &r) && r.vmin_hz == 48 && r.vmax_hz == 260, "+255 Hz offset on the maximum (260 Hz)");
    edid(e, 2, 0x03, 5, 5, 90);                /* both offsets */
    check(!rdx_edid_range_bytes(e, sizeof(e), &r) && r.vmin_hz == 260 && r.vmax_hz == 260, "+255 Hz offset on both");
    edid(e, 0, 0, 48, 144, 60);
    e[54 + 3] = 0xFC;                          /* a name descriptor instead */
    check(rdx_edid_range_bytes(e, sizeof(e), &r) == -1 && !r.vmin_hz, "no range descriptor: -1, range 0");
    edid(e, 3, 0, 48, 144, 60);
    e[1] = 0;                                  /* not an EDID */
    check(rdx_edid_range_bytes(e, sizeof(e), &r) == -1, "broken header: -1");
    check(rdx_edid_range_bytes(e, 64, &r) == -1, "short: -1");

    /* LFC (rdx_lfc_due_ns): a measured run on a test host (amdgpu, 40-144 Hz) replayed. Frames
     * arrive every 7.17 ms, the flip event of each is read 1-14 ms after its request; at no
     * moment between two frames may a repeat be due. The old rule (event vblank timestamp, stale
     * under VRR) fired 15.8 times a second there. */
    {
        const double vmin = 40;
        uint64_t t_frame = 0, t_done = 0, ms = 1000000ull;
        int early = 0;
        for (int i = 1; i <= 2000; i++) {
            uint64_t arrive = (uint64_t)i * 7170000ull;
            uint64_t read = arrive + ms + (uint64_t)(i % 14) * ms;   /* 1..14 ms late */
            for (uint64_t now = (t_frame ? t_frame : arrive); now < arrive; now += 100000ull)
                if (t_frame && now > rdx_lfc_due_ns(t_done, t_frame, vmin)) early++;
            t_frame = arrive;
            if (read > t_done) t_done = read;
        }
        check(!early, "LFC: no repeat between frames at 139 fps with flip events read 1-14 ms late");
        uint64_t due = rdx_lfc_due_ns(100 * ms, 90 * ms, vmin);
        check(due == 100 * ms + 22500000ull, "LFC: 22.5 ms (90 % of 1/40 Hz) after the later of flip done and arrival");
        check(rdx_lfc_due_ns(90 * ms, 100 * ms, vmin) == 100 * ms + 22500000ull, "LFC: a later arrival counts");
        check(!rdx_lfc_due_ns(0, 100 * ms, vmin) && !rdx_lfc_due_ns(100 * ms, 0, vmin) && !rdx_lfc_due_ns(1, 1, 0),
              "LFC: none before the first flip, the first frame, or without a range");
    }
    check(rdx_driver_does_lfc("amdgpu", 40, 144) && rdx_driver_does_lfc("amdgpu", 48, 96),
          "driver LFC: amdgpu with a range of a factor 2 or more (an Acer 40-144 monitor)");
    check(!rdx_driver_does_lfc("amdgpu", 48, 75) && !rdx_driver_does_lfc("amdgpu", 0, 144),
          "driver LFC: not on amdgpu with a narrow or unknown range (the sink repeats)");
    check(!rdx_driver_does_lfc("i915", 40, 144) && !rdx_driver_does_lfc("nvidia-drm", 40, 144) && !rdx_driver_does_lfc(NULL, 40, 144),
          "driver LFC: not on i915 / nvidia-drm / unknown (the sink repeats)");
    /* the cursor kick (rdx_sink_kms, i915 & co. under VRR): two fastest periods after the last arrival */
    {
        const uint64_t ms = 1000000ull;
        check(rdx_cursor_kick_due_ns(1, 100 * ms, 101 * ms, 144.0, 6944444) == 100 * ms + 13888888ull,
              "cursor kick: 2 / 144 Hz after the last frame");
        check(rdx_cursor_kick_due_ns(1, 100 * ms, 101 * ms, 0, 6944444) == 100 * ms + 2 * 6944444ull,
              "cursor kick: two mode periods without a VRR range");
        check(rdx_cursor_kick_due_ns(0, 100 * ms, 101 * ms, 144.0, 6944444) == UINT64_MAX &&
              rdx_cursor_kick_due_ns(1, 0, 101 * ms, 144.0, 6944444) == UINT64_MAX &&
              rdx_cursor_kick_due_ns(1, 100 * ms, 0, 144.0, 6944444) == UINT64_MAX,
              "cursor kick: none unwanted, before the first frame or the first flip");
        check(!rdx_due_reached(UINT64_MAX, UINT64_MAX - 1) && !rdx_due_reached(UINT64_MAX, UINT64_MAX),
              "due: UINT64_MAX is never reached");
        check(rdx_due_reached(100 * ms, 100 * ms) && rdx_due_reached(100 * ms, 101 * ms) && !rdx_due_reached(100 * ms + 1, 100 * ms),
              "due: reached at and after it, not before (the clock is read after the due time)");
        /* the sink's loop: 30 fps frames, a pass every 1 ms, the pointer moving: one kick per frame interval */
        int kicks = 0;
        for (uint64_t t = 0; t < 990 * ms; t += ms) {
            uint64_t last = t / (33 * ms) * (33 * ms) + 1;          /* the arrival of the newest frame */
            uint64_t d = rdx_cursor_kick_due_ns(1, last, last + 2 * ms, 144.0, 6944444);
            if (rdx_due_reached(d, t) && t < d + ms) kicks++;       /* the first pass at or after the due time */
        }
        check(kicks == 30, "cursor kick: due once per frame interval of a 30 fps app");
    }
    printf("test_vrr: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
