/* CUDA VMM buffers that the NIC can read and Vulkan/GL can render into.
 *
 * One physical VRAM allocation, three views:
 *   - CUDA device pointer (verification, CUDA kernels),
 *   - opaque fd (cuMemExportToShareableHandle POSIX_FD) for Vulkan
 *     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT / GL_EXT_memory_object_fd,
 *   - dma-buf fd for ibv_reg_dmabuf_mr.
 * The dma-buf comes from cuMemGetHandleForAddressRange when the driver offers
 * it, otherwise from an RM export (own RM
 * client imports the opaque fd, NV_ESC_EXPORT_TO_DMABUF_FD), which works on
 * GeForce cards with the unmodified open kernel modules (build with
 * -DRDX_WITH_OGKM and the open-gpu-kernel-modules include dirs).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <cuda.h>

enum rdx_vmm_via { RDX_VMM_VIA_NONE = 0, RDX_VMM_VIA_CUDA = 1, RDX_VMM_VIA_RM = 2 };

struct rdx_vmm {
    CUdevice dev;
    CUcontext ctx;
    CUmemGenericAllocationHandle mh;
    CUdeviceptr ptr;
    size_t size;               /* rounded up to the allocation granularity */
    int opaque_fd;             /* for Vulkan/GL import */
    int dmabuf_fd;             /* for the NIC */
    int rm_ctl;                /* RM client of the RM export path, kept open while the dma-buf lives */
    enum rdx_vmm_via via;
    uint8_t uuid[16];          /* matches VkPhysicalDeviceIDProperties.deviceUUID */
    int bus;                   /* PCI bus of the GPU */
    char name[96];
};

/* identify a GPU without allocating: name, PCI bus, Vulkan-compatible UUID, and whether
 * CUDA can export dma-bufs itself (CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED; 0 on GeForce,
 * where only the RM export path exists). 0 = ok */
int  rdx_vmm_probe(int cuda_ord, char *name, size_t name_len, uint8_t uuid[16], int *dmabuf_supported,
                   char *pci_bdf, size_t bdf_len);    /* pci_bdf: "0000:04:00.0", lower case */
/* CUDA ordinal of the GPU at a PCI address ("0000:0d:00.0"), -1 if none */
int  rdx_vmm_ordinal_for_pci(const char *pci);
/* cuda_ord: CUDA device ordinal (not NVML). want: bytes. 0 = ok. */
int  rdx_vmm_alloc(struct rdx_vmm *v, int cuda_ord, size_t want);
void rdx_vmm_free(struct rdx_vmm *v);
/* drop the dma-buf (and the RM client behind it) and export the same VRAM again; the
 * CUDA allocation, its opaque fd and every Vulkan import of it stay as they are.
 * Deregister NIC MRs on the old dma-buf first. 0 = ok */
int  rdx_vmm_reexport(struct rdx_vmm *v);
/* a fresh duplicate of the opaque fd (Vulkan takes ownership of what it imports) */
int  rdx_vmm_dup_opaque(const struct rdx_vmm *v);
const char *rdx_vmm_via_name(enum rdx_vmm_via via);
