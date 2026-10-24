/* Simulation test for rdx_lock_due: a sink with an exact vblank grid, a source
 * that posts at the returned time and whose frames arrive lead later. Checks the
 * achieved frame rate, that no two frames aim at the same vblank, and the gap
 * pattern in vblanks.
 *   cc -O2 -Icommon tests/test_lock_due.c common/rdx_common.c -libverbs -o tests/test_lock_due */
#include "rdx_common.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t rng = 88172645463325252ull;
static double jitter(double us)               /* uniform in [-us, +us] microseconds, in ns */
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((double)(rng % 2000001) / 1e6 - 1.0) * us * 1e3;
}

/* jitter_us: noise on the anchor (the "shown" report arrives with jitter);
 * the source's wanted interval is (uint64)(1e9 / fps) like rdx_mirror_src, the sink
 * reports its period rounded to whole ns, the true grid is exact */
static int run(double hz, double fps, double lead_ms, double jitter_us)
{
    const double true_period = 1e9 / hz, lead = lead_ms * 1e6, margin = 1.5e6;
    const double period = (double)(uint64_t)true_period, interval = (double)(uint64_t)(1e9 / fps);
    const double secs = 10;
    uint64_t t0 = 1000000000ull;                 /* grid: t0 + k * period */
    uint64_t anchor = t0, now = t0, shown_vbl = t0;
    struct rdx_lock lock = {0, 0};
    uint64_t targets[16]; int nt = 0;            /* posted, not yet shown */
    long frames = 0, collisions = 0, gaps[4] = {0};
    uint64_t prev_shown = 0;
    while (now < t0 + (uint64_t)(secs * 1e9)) {
        /* retire frames whose vblank has passed: they become the anchor */
        int j = 0;
        for (int i = 0; i < nt; i++) {
            if (targets[i] <= now) {
                /* it arrives margin before its target (lead includes the margin) and shows at the
                 * first true vblank after its arrival */
                double k = ((double)(targets[i] - t0) - margin) / true_period;
                long ki = (long)k; if ((double)ki < k - 1e-9) ki++;
                shown_vbl = t0 + (uint64_t)((double)ki * true_period);
                anchor = (uint64_t)((double)shown_vbl + jitter(jitter_us));
                if (prev_shown) {
                    long g = (long)((double)(shown_vbl - prev_shown) / true_period + 0.5);
                    if (g <= 0) collisions++; else gaps[g < 3 ? g : 3]++;
                }
                prev_shown = shown_vbl;
                frames++;
            } else targets[j++] = targets[i];
        }
        nt = j;
        uint64_t due = rdx_lock_due(anchor, period, interval, nt, &lock, lead, now);
        uint64_t target = due + (uint64_t)lead;
        for (int i = 0; i < nt; i++) if (targets[i] == target) collisions++;
        if (nt < 16) targets[nt++] = target;
        now = due > now ? due : now + 1000;      /* the source sleeps until due, then posts */
    }
    double got = frames / secs;
    /* an even cadence (fps divides hz) must keep every gap equal; only uneven ones mix */
    int even = (long)(hz + 0.5) % (long)(fps + 0.5) == 0;
    long want = even ? (long)(hz / fps + 0.5) : 0;
    long off = 0;
    for (int g = 1; g <= 3; g++) if (even && g != want) off += gaps[g];
    int ok = collisions == 0 && got > fps * 0.98 && got < fps * 1.02 && off <= frames / 1000;
    printf("%-4s %5.0f Hz %5.0f fps lead %4.1f ms jitter %3.0f us: %7.2f fps, gaps 1v=%ld 2v=%ld >=3v=%ld, collisions %ld\n",
           ok ? "ok" : "FAIL", hz, fps, lead_ms, jitter_us, got, gaps[1], gaps[2], gaps[3], collisions);
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= run(240, 144, 5.0, 0);
    ok &= run(240, 144, 9.0, 0);  /* lead > 2 periods: frames in flight */
    ok &= run(240, 120, 5.0, 0);
    ok &= run(240, 240, 5.0, 0);
    ok &= run(120, 60, 5.0, 0);
    ok &= run(120, 120, 3.0, 0);
    ok &= run(60, 60, 5.0, 0);
    ok &= run(240, 200, 5.0, 0);
    /* real reports jitter: an even cadence must stay even */
    ok &= run(240, 120, 4.6, 100);
    ok &= run(240, 240, 4.6, 100);
    ok &= run(120, 60, 4.6, 100);
    ok &= run(240, 144, 4.6, 100);
    printf(ok ? "ALL OK\n" : "SOME FAILED\n");
    return ok ? 0 : 1;
}
