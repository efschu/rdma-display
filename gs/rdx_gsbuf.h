/* Output buffers of the gamescope path, one per sink slot: where gamescope's frame lands and the NIC reads.
 *
 *   VRAM: CUDA VMM allocation, dma-buf MR (the NIC peer-reads the GPU through BAR1),
 *         opaque fd to gamescope.
 *   HOST: pinned host memory (memfd, 2 MiB huge pages if available, mlocked), plain MR,
 *         memfd to gamescope, which imports it with VK_EXT_external_memory_host. The GPU
 *         writes the frame over PCIe (posted writes), the NIC reads host RAM; nothing ever
 *         peer-reads VRAM.
 *
 * auto (rdx_gsbuf_choose): VRAM only where peer reads are known to be safe and fast (also on
 * GeForce with the RM export):
 * GPU and NIC each directly below a CPU root port (no PCIe switch / chipset in either
 * path) AND the barlink stack is present (/dev/dmabuf_holder, an out-of-tree BAR1 pinning
 * driver), which keeps peer reads stable with nearly full VRAM. Everything else: HOST.
 */
#pragma once
#include "../common/rdx_vmm.h"

#include <infiniband/verbs.h>
#include <stddef.h>
#include <stdint.h>

#define RDX_GSBUF_VRAM 0
#define RDX_GSBUF_HOST 1

struct rdx_gsbuf {
    int kind;
    size_t size;                 /* bytes handed to gamescope and registered with the NIC */
    struct rdx_vmm vm;           /* VRAM */
    int memfd;                   /* HOST */
    void *host;
    int huge;                    /* HOST: backed by 2 MiB huge pages */
    struct ibv_mr *mr;
};

/* kind for "host", "vram" or "auto" (prints the reason); -1 on an unknown word */
int  rdx_gsbuf_choose(const char *want, int cuda_ord, const char *rdma_dev);
int  rdx_gsbuf_alloc(struct rdx_gsbuf *b, int kind, int cuda_ord, size_t bytes, struct ibv_pd *pd);
/* the fd gamescope imports (stays owned by b) */
int  rdx_gsbuf_fd(const struct rdx_gsbuf *b);
/* HOST: a dma-buf of the same pages (/dev/udmabuf on the sealed memfd), for GPUs that cannot
 * import a host pointer of a shared mapping (AMD/Intel: amdgpu userptr takes anonymous memory
 * only). A new fd each call, the caller owns it; -1 without udmabuf (modprobe udmabuf). */
int  rdx_gsbuf_dmabuf(const struct rdx_gsbuf *b);
/* address of byte 0 in the MR's address space (dma-buf MR: iova 0; host MR: the pointer) */
uint64_t rdx_gsbuf_addr(const struct rdx_gsbuf *b);
/* recovery after a local error: new MR (VRAM: on a freshly exported dma-buf) */
int  rdx_gsbuf_reregister(struct rdx_gsbuf *b, struct ibv_pd *pd);
const char *rdx_gsbuf_describe(const struct rdx_gsbuf *b);
void rdx_gsbuf_free(struct rdx_gsbuf *b);
