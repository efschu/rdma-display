/* SPDX-License-Identifier: MIT
 *
 * P0-A gate G0b: can the NIC register a dma-buf that a compositor would use as
 * its output buffer? Allocates GBM buffer objects on a render node (e.g. the
 * NVIDIA one), exports them as dma-buf and calls ibv_reg_dmabuf_mr on them.
 * Prints one line per buffer kind with success or the errno.
 *
 *   rdx_gate_gbm [--render /dev/dri/renderD129] [--dev auto] [--res 4k]
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>

struct kind {
    const char *name;
    uint32_t flags;
    int linear_modifier;     /* allocate with an explicit LINEAR modifier list */
};

static void try_kind(struct gbm_device *gbm, struct ibv_pd *pd, const struct kind *k,
                     uint32_t w, uint32_t h)
{
    struct gbm_bo *bo;
    if (k->linear_modifier) {
        uint64_t mod = DRM_FORMAT_MOD_LINEAR;
        bo = gbm_bo_create_with_modifiers2(gbm, w, h, GBM_FORMAT_XRGB8888, &mod, 1, k->flags);
    } else {
        bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888, k->flags);
    }
    if (!bo) {
        printf("  %-34s gbm_bo_create failed: %s\n", k->name, strerror(errno));
        return;
    }
    int fd = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    size_t size = (size_t)stride * h;
    off_t end = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
    if (end > 0) size = (size_t)end;          /* dma-buf size includes any padding */
    if (fd < 0) {
        printf("  %-34s gbm_bo_get_fd failed\n", k->name);
        gbm_bo_destroy(bo);
        return;
    }
    struct ibv_mr *mr = ibv_reg_dmabuf_mr(pd, 0, size, 0, fd,
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                                          IBV_ACCESS_REMOTE_WRITE);
    int err = errno;
    printf("  %-34s stride=%u modifier=0x%016llx size=%zu -> ibv_reg_dmabuf_mr %s",
           k->name, stride, (unsigned long long)mod, size, mr ? "OK" : "FAILED");
    if (mr) printf(" (lkey 0x%x)\n", mr->lkey);
    else printf(" (%s)\n", strerror(err));
    if (mr) ibv_dereg_mr(mr);
    close(fd);
    gbm_bo_destroy(bo);
}

int main(int argc, char **argv)
{
    const char *render = "/dev/dri/renderD129", *dev = "auto", *res = "4k";
    static const struct option opts[] = {
        {"render", 1, 0, 'r'}, {"dev", 1, 0, 'd'}, {"res", 1, 0, 's'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'r': render = optarg; break;
        case 'd': dev = optarg; break;
        case 's': res = optarg; break;
        default:  fprintf(stderr, "see the header of rdx_gate_gbm.c for options\n"); return 2;
        }
    }
    uint32_t w, h;
    if (rdx_parse_res(res, &w, &h)) { fprintf(stderr, "invalid --res\n"); return 2; }

    int rfd = open(render, O_RDWR | O_CLOEXEC);
    if (rfd < 0) { perror(render); return 1; }
    struct gbm_device *gbm = gbm_create_device(rfd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 1; }
    printf("[gate] %s, GBM backend %s, %ux%u XRGB8888, NIC %s\n", render,
           gbm_device_get_backend_name(gbm), w, h, dev);

    struct ibv_context *ctx = rdx_open_dev(dev);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd"); return 1; }

    static const struct kind kinds[] = {
        {"RENDERING (driver layout)",          GBM_BO_USE_RENDERING, 0},
        {"RENDERING|SCANOUT (driver layout)",  GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT, 0},
        {"RENDERING|LINEAR",                   GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR, 0},
        {"RENDERING, modifier LINEAR",         GBM_BO_USE_RENDERING, 1},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) try_kind(gbm, pd, &kinds[i], w, h);

    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    gbm_device_destroy(gbm);
    close(rfd);
    return 0;
}
