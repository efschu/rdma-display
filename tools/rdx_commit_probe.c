/* When does the compositor commit a new scanout buffer, relative to the vblank at which it
 * becomes visible? Polls the primary plane's FB_ID every --poll-us and records, for each
 * change, how long before the next vblank of that CRTC it happened. That time is what a
 * mirror could gain by reading the committed (pending) buffer instead of waiting for the
 * vblank. Read-only, no DRM master. Root (or video group) for the card.
 *
 *   rdx_commit_probe [--card /dev/dri/card1] [--output DP-1] [--seconds 10] [--poll-us 100]
 *   cc -O2 -Icommon tools/rdx_commit_probe.c common/rdx_kms.c common/rdx_common.c -libverbs
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"
#include "../common/rdx_kms.h"

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card1", *output = "DP-1";
    double seconds = 10, poll_us = 100;
    static const struct option opts[] = {
        {"card", 1, 0, 'c'}, {"output", 1, 0, 'o'}, {"seconds", 1, 0, 's'}, {"poll-us", 1, 0, 'p'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'c': card = optarg; break;
        case 'o': output = optarg; break;
        case 's': seconds = atof(optarg); break;
        case 'p': poll_us = atof(optarg); break;
        default: return 2;
        }
    }
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(card); return 1; }
    rdx_kms_universal_planes(fd);
    struct rdx_kms_cap_out outs[8];
    int n = rdx_kms_list_cap_outputs(fd, outs, 8), sel = -1;
    for (int i = 0; i < n; i++) if (!strcmp(outs[i].name, output)) sel = i;
    if (sel < 0) { fprintf(stderr, "output %s not found\n", output); return 1; }
    struct rdx_kms_cap_out *O = &outs[sel];
    int ci = rdx_kms_crtc_index(fd, O->crtc_id);
    double period = 1e9 / O->vrefresh;
    printf("[probe] %s %ux%u@%u, plane %u, CRTC index %d, polling every %.0f us for %.0f s\n",
           O->name, O->w, O->h, O->vrefresh, O->plane_id, ci, poll_us, seconds);

    size_t cap = 100000, nchg = 0, nlate = 0;
    double *lead = calloc(cap, sizeof(double));     /* commit -> next vblank, us */
    uint32_t last = rdx_kms_plane_fb(fd, O->plane_id);
    uint64_t t_end = rdx_now_ns() + (uint64_t)(seconds * 1e9), polls = 0;
    struct timespec ts = {0, (long)(poll_us * 1000)};
    while (rdx_now_ns() < t_end) {
        nanosleep(&ts, NULL);
        uint32_t fb = rdx_kms_plane_fb(fd, O->plane_id);
        polls++;
        if (fb == last) continue;
        uint64_t now = rdx_now_ns(), tv;
        last = fb;
        if (rdx_kms_last_vblank(fd, ci, &tv)) continue;
        double next = (double)tv + period;
        while (next <= (double)now) next += period;
        double l = (next - (double)now) / 1e3;
        if (nchg < cap) lead[nchg++] = l;
        if (l < poll_us) nlate++;
    }
    if (!nchg) { printf("[probe] no framebuffer change in %.0f s (static screen?)\n", seconds); return 0; }
    qsort(lead, nchg, sizeof(double), cmp_d);
    printf("[probe] %zu framebuffer changes (%.1f/s) in %llu polls\n", nchg, nchg / seconds,
           (unsigned long long)polls);
    printf("[probe] commit -> next vblank (= time a pending-buffer read gains): "
           "min %.0f p10 %.0f p50 %.0f p90 %.0f max %.0f us (period %.0f us)\n",
           lead[0], lead[nchg / 10], lead[nchg / 2], lead[nchg * 9 / 10], lead[nchg - 1], period / 1e3);
    printf("[probe] changes seen within one poll of the vblank: %zu\n", nlate);
    return 0;
}
