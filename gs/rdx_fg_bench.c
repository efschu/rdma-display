/* SPDX-License-Identifier: MIT
 * rdx_fg_bench: frame generation on a (helper) GPU between host-memory frames, without a NIC,
 * a sink or gamescope - the data path of docs/MULTI-GPU.md on its own.
 *
 *   rdx_fg_bench [--gpu 0000:0e:00.0] [--mode 1920x1080] [--frames 600] [--quality auto|quality|performance]
 *                [--fps 0] [--dump out.ppm] [--step 8] [--pan] [--anon]
 *
 * Synthetic frames in pinned memfd buffers (like rdx_gs_src's host slots): a white 64x64 square
 * that moves --step px per frame on black (afmf-linux's own test case), with --pan on a textured
 * background that scrolls --step/2 px per frame (harder: two motions, a repetitive texture). Each
 * frame enters the helper GPU's history and the frame between it and the previous one is written
 * into a third host buffer. Checked: the square in every generated frame sits halfway between its
 * two real positions (within 2 px). Measured: submit -> done (wall clock) and helper GPU time,
 * which includes both PCIe copies (host -> helper, helper -> host). --fps paces the submits like
 * a game at that rate (0 = back to back). Exit 0 = every check passed. */
#define _GNU_SOURCE
#include "rdx_fg.h"

#include <fcntl.h>
#include <getopt.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SQ 64

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int g_anon;  /* --anon: anonymous memory instead of a shared memfd (amdgpu imports only that) */

/* a sealed memfd like rdx_gsbuf's slots (*memfd), or anonymous memory with --anon (*memfd = -1) */
static void *host_buf(size_t *size, int *memfd)
{
    size_t sz = (*size + (2u << 20) - 1) & ~((size_t)(2u << 20) - 1);
    void *p;
    *memfd = -1;
    if (g_anon) {
        p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    } else {
        int fd = memfd_create("rdx-fg-bench", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0 || ftruncate(fd, (off_t)sz)) return NULL;
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
        p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        *memfd = fd;
    }
    if (p == MAP_FAILED) return NULL;
    mlock(p, sz);
    *size = sz;
    return p;
}

static int g_pan;   /* --pan: a textured background that scrolls under the square, else black */

static void draw(uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, long n, int step)
{
    int pan = (int)(n * step / 2), sx = 64 + (int)((n * step) % (long)(w - 2 * SQ - 64)), sy = (int)h / 2 - SQ / 2;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
        if (!g_pan) { memset(row, 0, (size_t)w * 4); continue; }
        for (uint32_t x = 0; x < w; x++) {
            uint32_t u = x + (uint32_t)pan;
            uint8_t v = (uint8_t)(40 + ((u / 16 + y / 16) & 1) * 60 + ((u * 7 + y * 13) & 31));
            row[x] = 0xff000000u | (uint32_t)v << 16 | (uint32_t)(v / 2 + 30) << 8 | (uint32_t)(200 - v);
        }
    }
    for (int y = sy; y < sy + SQ; y++) {
        uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
        /* two bright levels in a pattern that travels with the square (afmf-linux's test: a flat
         * interior looks the same in both frames, its blocks come out static and stay put) */
        for (int x = sx; x < sx + SQ; x++) row[x] = ((unsigned)(x - sx) * 7u + (unsigned)y * 13u) % 5u < 2u ? 0xffa0a0a0u : 0xffffffffu;
    }
}

static int square_x(long n, uint32_t w, int step) { return 64 + (int)((n * step) % (long)(w - 2 * SQ - 64)); }

/* centroid x of bright pixels (all channels > 128, as afmf-linux's check), -1 if none */
static double white_cx(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride)
{
    double sum = 0;
    long cnt = 0;
    for (uint32_t y = h / 2 - SQ / 2; y < h / 2 + SQ / 2; y++) {
        const uint32_t *row = (const uint32_t *)(px + (size_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = row[x];
            if ((c & 0xff) > 128 && ((c >> 8) & 0xff) > 128 && ((c >> 16) & 0xff) > 128) { sum += x; cnt++; }
        }
    }
    return cnt > SQ * SQ / 4 ? sum / (double)cnt : -1;
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, double *v, int n)
{
    if (!n) { printf("  %-28s no samples\n", what); return; }
    qsort(v, (size_t)n, sizeof(double), cmpd);
    printf("  %-28s n=%d  p50=%7.3f  p90=%7.3f  p99=%7.3f  max=%7.3f ms\n", what, n, v[n / 2], v[(int)(0.9 * (n - 1))],
           v[(int)(0.99 * (n - 1))], v[n - 1]);
}

int main(int argc, char **argv)
{
    const char *gpu = "", *quality = "auto", *dump = NULL;
    uint32_t w = 1920, h = 1080;
    long frames = 600;
    double fps = 0;
    int step = 8;
    static const struct option lo[] = {{"gpu", 1, 0, 'g'},     {"mode", 1, 0, 'm'}, {"frames", 1, 0, 'n'},
                                       {"quality", 1, 0, 'q'}, {"fps", 1, 0, 'f'},  {"dump", 1, 0, 'd'},
                                       {"step", 1, 0, 's'},    {"pan", 0, 0, 'p'}, {"anon", 0, 0, 'A'}, {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (o) {
        case 'g': gpu = optarg; break;
        case 'm': if (sscanf(optarg, "%ux%u", &w, &h) != 2) return 2; break;
        case 'n': frames = atol(optarg); break;
        case 'q': quality = optarg; break;
        case 'f': fps = atof(optarg); break;
        case 'd': dump = optarg; break;
        case 's': step = atoi(optarg); break;
        case 'p': g_pan = 1; break;
        case 'A': g_anon = 1; break;
        default:
            fprintf(stderr, "rdx_fg_bench [--gpu PCI] [--mode WxH] [--frames N] [--quality auto|quality|performance] "
                            "[--fps F] [--dump out.ppm] [--step PX] [--pan] [--anon]\n");
            return 2;
        }
    }
    uint32_t stride = w * 4;
    size_t bytes = (size_t)stride * h, sz[3];
    uint8_t *buf[3];
    int mfd[3];
    for (int i = 0; i < 3; i++) {
        sz[i] = bytes;
        if (!(buf[i] = host_buf(&sz[i], &mfd[i]))) { perror("host buffer"); return 1; }
    }
    char err[256];
    struct rdx_fg_cfg c = {.pci = gpu, .width = w, .height = h, .stride = stride, .quality = quality, .high_priority = 1};
    uint64_t t_open = now_ns();
    struct rdx_fg *fg = rdx_fg_open(&c, err, sizeof(err));
    if (!fg) { fprintf(stderr, "rdx_fg_open: %s\n", err); return 1; }
    printf("[bench] %s, %ux%u, opened in %.0f ms\n", rdx_fg_describe(fg), w, h, (now_ns() - t_open) / 1e6);
    int id[3];
    int via_dmabuf = 0;
    for (int i = 0; i < 3; i++) {
        id[i] = rdx_fg_import(fg, buf[i], sz[i]);
        if (id[i] < 0 && mfd[i] >= 0) {          /* amdgpu: the same pages as a dma-buf (udmabuf) */
            int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            struct udmabuf_create uc = {.memfd = (uint32_t)mfd[i], .flags = UDMABUF_FLAGS_CLOEXEC, .size = sz[i]};
            int dfd = dev >= 0 ? ioctl(dev, UDMABUF_CREATE, &uc) : -1;
            if (dev >= 0) close(dev);
            if (dfd >= 0 && (id[i] = rdx_fg_import_dmabuf(fg, dfd, buf[i], sz[i])) == -1) close(dfd);
            via_dmabuf += id[i] >= 0;
        }
        if (id[i] < 0) { fprintf(stderr, "import failed\n"); return 1; }
    }
    if (via_dmabuf) printf("[bench] %d buffers imported as dma-bufs (udmabuf)\n", via_dmabuf);

    double *tot = calloc((size_t)frames, sizeof(double)), *gms = calloc((size_t)frames, sizeof(double));
    double max_err = 0;
    int nt = 0, bad = 0, checked = 0;
    uint64_t t0 = now_ns(), period = fps > 0 ? (uint64_t)(1e9 / fps) : 0;
    for (long n = 0; n < frames; n++) {
        if (period) {
            uint64_t due = t0 + (uint64_t)n * period, t = now_ns();
            if (due > t) usleep((useconds_t)((due - t) / 1000));
        }
        int s = (int)(n & 1);
        draw(buf[s], w, h, stride, n, step);
        if (rdx_fg_submit(fg, id[s], n ? id[2] : -1, (uint64_t)n)) { fprintf(stderr, "submit refused\n"); return 1; }
        struct pollfd p = {.fd = rdx_fg_fd(fg), .events = POLLIN};
        struct rdx_fg_done d;
        while (!rdx_fg_reap(fg, &d)) poll(&p, 1, 1000);
        if (d.failed) { fprintf(stderr, "job %ld failed\n", n); return 1; }
        if (!d.generated) continue;
        tot[nt] = d.total_ms;
        gms[nt] = d.gpu_ms;
        nt++;
        /* the square's expected centre halfway between frame n-1 and n (no wrap in between) */
        int x0 = square_x(n - 1, w, step), x1 = square_x(n, w, step);
        if (x1 < x0 || n < 8) continue;   /* wrap; and the flow's scene-change warm-up */
        double want = (x0 + x1) / 2.0 + SQ / 2.0 - 0.5, got = white_cx(buf[2], w, h, stride);
        double e = got < 0 ? 1e9 : fabs(got - want);
        checked++;
        if (e > max_err) max_err = e;
        if (e > 2.0) {
            if (bad < 5) printf("[bench] frame %ld: square at x %.1f, expected %.1f\n", n, got, want);
            bad++;
        }
        if (dump && n == 20) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t px = ((uint32_t *)(buf[2] + (size_t)y * stride))[x];
                        uint8_t rgb[3] = {(uint8_t)(px >> 16), (uint8_t)(px >> 8), (uint8_t)px};
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
            }
        }
    }
    double el = (now_ns() - t0) / 1e9;
    printf("[bench] %ld frames in %.2f s: %d generated, %d checked, %d off by more than 2 px (max %.2f px)\n", frames,
           el, nt, checked, bad, max_err > 1e8 ? -1.0 : max_err);
    report("submit -> done (wall)", tot, nt);
    report("helper GPU (incl. copies)", gms, nt);
    rdx_fg_close(fg);
    return bad || !checked ? 1 : 0;
}
