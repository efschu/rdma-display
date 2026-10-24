/* SPDX-License-Identifier: MIT
 * Frame generation on a helper GPU, between frames that live in host memory.
 *
 * rdma-display's gamescope apps composite on the seat's GPU into host-RAM slots (or VRAM slots
 * that a CUDA copy stages to host RAM). A second GPU ("helper_gpu") imports those slots with
 * VK_EXT_external_memory_host, so it reads them over its own PCIe link, runs FidelityFX Optical
 * Flow + afmf-linux's interpolation shader (third_party/afmf-linux, MIT) between the previous and
 * the new frame, and writes the frame in between into another host buffer, which the NIC sends
 * like any other frame. The seat's GPU spends nothing on it; the helper's link carries one frame
 * in and one frame out per real frame. See docs/MULTI-GPU.md.
 *
 * One worker thread per instance runs the jobs in submission order (the history of two frames
 * is part of the state), one at a time; rdx_fg_fd() becomes readable when a job is done. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef RDX_FG_CUDA
#include <cuda.h>
#endif

struct rdx_fg;

struct rdx_fg_cfg {
    const char *pci;        /* helper GPU by PCI address ("0000:0e:00.0"); NULL / "" = first GPU */
    uint32_t width, height; /* frame size in pixels (B8G8R8A8 / XRGB8888) */
    uint32_t stride;        /* bytes per row of every host buffer, a multiple of 4 */
    const char *quality;    /* "auto" (half-resolution flow from 1440p up), "quality", "performance" */
    int high_priority;      /* ask for a high-priority compute queue (VK_KHR_global_priority) */
};

struct rdx_fg_done {
    uint64_t tag;
    int src, dst;           /* buffer ids of the job */
    int generated;          /* 1: the frame in between is in dst; 0: history only, or failed */
    int failed;
    double wait_ms;         /* submit -> the worker started it */
    double stage_ms;        /* CUDA staging copy (rdx_fg_submit_cuda), 0 otherwise */
    double gpu_ms;          /* helper GPU time of the job (timestamps), incl. both PCIe copies */
    double total_ms;        /* submit -> done, wall clock */
};

/* NULL on failure, with the reason in err */
struct rdx_fg *rdx_fg_open(const struct rdx_fg_cfg *c, char *err, size_t errn);
/* "NVIDIA GeForce RTX 3080 (0000:0e:00.0), flow 1920x1080, 7 levels" */
const char *rdx_fg_describe(const struct rdx_fg *fg);
/* make a host buffer (page aligned, size a multiple of the device's import alignment, e.g. a
 * 2 MiB rounded memfd slot) usable as source or destination; buffer id >= 0, -1 on failure */
int rdx_fg_import(struct rdx_fg *fg, void *ptr, size_t size);
/* the same host memory as a dma-buf (udmabuf of a memfd; amdgpu imports host pointers of
 * anonymous memory only): id >= 0 and the fd belongs to Vulkan; -1 = failed, fd still the
 * caller's; -2 = failed after Vulkan took the fd. `ptr` is only remembered. */
int rdx_fg_import_dmabuf(struct rdx_fg *fg, int fd, void *ptr, size_t size);
/* queue a job: the frame in buffer `src` enters the history; with dst >= 0 the frame between
 * the previous one and it is written into buffer `dst`. 0 = queued, -1 = queue full / bad id */
int rdx_fg_submit(struct rdx_fg *fg, int src, int dst, uint64_t tag);
#ifdef RDX_FG_CUDA
/* the same for a frame in VRAM of another GPU (a CUDA VMM slot): the worker first copies it
 * into host buffer `staging` (cuMemcpyDtoH in `ctx`), then runs the job from there */
int rdx_fg_submit_cuda(struct rdx_fg *fg, CUcontext ctx, CUdeviceptr src, int staging, int dst, uint64_t tag);
#endif
/* readable while results wait in rdx_fg_reap */
int rdx_fg_fd(const struct rdx_fg *fg);
/* 1 = one result in *d, 0 = none */
int rdx_fg_reap(struct rdx_fg *fg, struct rdx_fg_done *d);
/* jobs submitted and not yet reaped */
int rdx_fg_pending(const struct rdx_fg *fg);
/* forget the history (after a gap: a detach, a skipped frame), the next job starts fresh */
void rdx_fg_reset(struct rdx_fg *fg);
void rdx_fg_close(struct rdx_fg *fg);
