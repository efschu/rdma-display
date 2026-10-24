/* CUDA VMM buffers for the NIC and for Vulkan/GL rendering, see rdx_vmm.h. */
#include "rdx_vmm.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef RDX_WITH_OGKM
/* headers of NVIDIA/open-gpu-kernel-modules, not shipped here (make OGKM=<their checkout>, gs/gs.mk) */
#include <nvtypes.h>
#include <nvos.h>
#include <nv-ioctl.h>
#include <nv_escape.h>
#include <class/cl0000.h>
#include <class/cl0080.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0000/ctrl0000unix.h>
#endif

#define CU_OK(x) cu_ok((x), #x)
static int cu_ok(CUresult r, const char *what)
{
    if (r == CUDA_SUCCESS) return 1;
    const char *s = NULL;
    cuGetErrorName(r, &s);
    fprintf(stderr, "[vmm] %s -> %d (%s)\n", what, r, s ? s : "?");
    return 0;
}

const char *rdx_vmm_via_name(enum rdx_vmm_via via)
{
    return via == RDX_VMM_VIA_CUDA ? "cuMemGetHandleForAddressRange"
         : via == RDX_VMM_VIA_RM   ? "RM export (NV_ESC_EXPORT_TO_DMABUF_FD)" : "none";
}

#ifdef RDX_WITH_OGKM
static int nv_ioctl(int fd, int nr, void *p, size_t size)
{
    return ioctl(fd, _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, nr, (unsigned)size), p);
}

static int rm_control(int ctl, NvHandle client, NvHandle obj, NvU32 cmd, void *p, NvU32 size)
{
    NVOS54_PARAMETERS c;
    memset(&c, 0, sizeof(c));
    c.hClient = client; c.hObject = obj; c.cmd = cmd;
    c.params = (NvP64)(uintptr_t)p; c.paramsSize = size;
    if (nv_ioctl(ctl, NV_ESC_RM_CONTROL, &c, sizeof(c)) < 0) return -1;
    return c.status ? -(int)c.status : 0;
}

/* own RM client: import the CUDA export object, re-export the range as a dma-buf */
static int rm_export_dmabuf(struct rdx_vmm *v)
{
    int ctl = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (ctl < 0) { perror("[vmm] /dev/nvidiactl"); return -1; }
    int ret = -1, dev_fd = -1;

    nv_ioctl_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    ver.cmd = NV_RM_API_VERSION_CMD_RELAXED;
    FILE *f = fopen("/proc/driver/nvidia/version", "r");
    char buf[256] = {0};
    if (f) { if (!fgets(buf, sizeof(buf), f)) buf[0] = 0; fclose(f); }
    char *p = strstr(buf, "for x86_64");
    if (p) sscanf(p, "for x86_64 %63s", ver.versionString);
    if (nv_ioctl(ctl, NV_ESC_CHECK_VERSION_STR, &ver, sizeof(ver)) < 0) { perror("[vmm] CHECK_VERSION_STR"); goto out; }

    nv_ioctl_card_info_t ci[32];
    memset(ci, 0, sizeof(ci));
    if (nv_ioctl(ctl, NV_ESC_CARD_INFO, ci, sizeof(ci)) < 0) { perror("[vmm] CARD_INFO"); goto out; }
    NvU32 gpu_id = 0, minor = 0;
    int found = 0;
    for (int i = 0; i < 32; i++)
        if (ci[i].valid && (int)ci[i].pci_info.bus == v->bus) { gpu_id = ci[i].gpu_id; minor = ci[i].minor_number; found = 1; }
    if (!found) { fprintf(stderr, "[vmm] PCI bus 0x%02x not in CARD_INFO\n", v->bus); goto out; }

    NVOS21_PARAMETERS a;
    memset(&a, 0, sizeof(a));
    a.hClass = NV01_ROOT;
    if (nv_ioctl(ctl, NV_ESC_RM_ALLOC, &a, sizeof(a)) < 0 || a.status) { fprintf(stderr, "[vmm] RM root alloc failed\n"); goto out; }
    NvHandle client = a.hObjectNew, device = 0xbee00001, memory = 0xbee00010;

    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS idp;
    memset(&idp, 0, sizeof(idp));
    idp.gpuId = gpu_id;
    if (rm_control(ctl, client, client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &idp, sizeof(idp))) {
        fprintf(stderr, "[vmm] GET_ID_INFO_V2 failed\n"); goto out;
    }
    NV0080_ALLOC_PARAMETERS dp;
    memset(&dp, 0, sizeof(dp));
    memset(&a, 0, sizeof(a));
    dp.deviceId = idp.deviceInstance; dp.hClientShare = client;
    a.hRoot = client; a.hObjectParent = client; a.hObjectNew = device; a.hClass = NV01_DEVICE_0;
    a.pAllocParms = (NvP64)(uintptr_t)&dp;
    if (nv_ioctl(ctl, NV_ESC_RM_ALLOC, &a, sizeof(a)) < 0 || a.status) { fprintf(stderr, "[vmm] RM device alloc failed\n"); goto out; }

    NV0000_CTRL_OS_UNIX_IMPORT_OBJECT_FROM_FD_PARAMS ip;
    memset(&ip, 0, sizeof(ip));
    ip.fd = v->opaque_fd;
    ip.object.type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM;
    ip.object.data.rmObject.hDevice = device;
    ip.object.data.rmObject.hParent = device;
    ip.object.data.rmObject.hObject = memory;
    if (rm_control(ctl, client, client, NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD, &ip, sizeof(ip))) {
        fprintf(stderr, "[vmm] IMPORT_OBJECT_FROM_FD failed\n"); goto out;
    }

    char path[32];
    snprintf(path, sizeof(path), "/dev/nvidia%u", minor);
    dev_fd = open(path, O_RDWR | O_CLOEXEC);
    if (dev_fd < 0) { perror(path); goto out; }
    nv_ioctl_export_to_dma_buf_fd_t ep;
    memset(&ep, 0, sizeof(ep));
    ep.fd = -1;
    ep.hClient = client;
    ep.totalObjects = 1; ep.numObjects = 1; ep.index = 0;
    ep.totalSize = v->size;
    ep.mappingType = NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT;
    ep.bAllowMmap = NV_FALSE;
    ep.handles[0] = memory; ep.offsets[0] = 0; ep.sizes[0] = v->size;
    if (nv_ioctl(dev_fd, NV_ESC_EXPORT_TO_DMABUF_FD, &ep, sizeof(ep)) < 0 || ep.status) {
        fprintf(stderr, "[vmm] EXPORT_TO_DMABUF_FD failed (status 0x%x)\n", ep.status); goto out;
    }
    v->dmabuf_fd = ep.fd;
    v->rm_ctl = ctl;            /* closing the RM client would free the imported object */
    ctl = -1;
    ret = 0;
out:
    if (dev_fd >= 0) close(dev_fd);
    if (ctl >= 0) close(ctl);
    return ret;
}
#endif

int rdx_vmm_probe(int cuda_ord, char *name, size_t name_len, uint8_t uuid[16], int *dmabuf_supported,
                  char *pci_bdf, size_t bdf_len)
{
    CUdevice dev;
    CUuuid uu;
    if (!CU_OK(cuInit(0)) || !CU_OK(cuDeviceGet(&dev, cuda_ord)) || !CU_OK(cuDeviceGetUuid(&uu, dev))) return -1;
    memcpy(uuid, uu.bytes, 16);
    if (name) cuDeviceGetName(name, (int)name_len, dev);
    *dmabuf_supported = 0;
    cuDeviceGetAttribute(dmabuf_supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, dev);
    if (pci_bdf && bdf_len) {
        pci_bdf[0] = 0;
        if (cuDeviceGetPCIBusId(pci_bdf, (int)bdf_len, dev) == CUDA_SUCCESS)
            for (char *c = pci_bdf; *c; c++) *c = (char)tolower((unsigned char)*c);
    }
    return 0;
}

int rdx_vmm_ordinal_for_pci(const char *pci)
{
    CUdevice dev;
    if (!CU_OK(cuInit(0)) || cuDeviceGetByPCIBusId(&dev, pci) != CUDA_SUCCESS) {
        fprintf(stderr, "[vmm] no CUDA device at PCI %s\n", pci);
        return -1;
    }
    return (int)dev;           /* a CUdevice is its ordinal */
}

int rdx_vmm_alloc(struct rdx_vmm *v, int cuda_ord, size_t want)
{
    memset(v, 0, sizeof(*v));
    v->opaque_fd = v->dmabuf_fd = v->rm_ctl = -1;
    if (!CU_OK(cuInit(0)) || !CU_OK(cuDeviceGet(&v->dev, cuda_ord))) return -1;
    cuDeviceGetName(v->name, sizeof(v->name), v->dev);
    cuDeviceGetAttribute(&v->bus, CU_DEVICE_ATTRIBUTE_PCI_BUS_ID, v->dev);
    CUuuid uu;
    if (CU_OK(cuDeviceGetUuid(&uu, v->dev))) memcpy(v->uuid, uu.bytes, 16);
    if (!CU_OK(cuDevicePrimaryCtxRetain(&v->ctx, v->dev)) || !CU_OK(cuCtxSetCurrent(v->ctx))) return -1;

    CUmemAllocationProp prop;
    memset(&prop, 0, sizeof(prop));
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = v->dev;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    int rdma_ok = 0;
    cuDeviceGetAttribute(&rdma_ok, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED, v->dev);
    if (rdma_ok) prop.allocFlags.gpuDirectRDMACapable = 1;
    size_t gran = 0;
    if (!CU_OK(cuMemGetAllocationGranularity(&gran, &prop, CU_MEM_ALLOC_GRANULARITY_RECOMMENDED))) return -1;
    v->size = (want + gran - 1) / gran * gran;
    if (!CU_OK(cuMemCreate(&v->mh, v->size, &prop, 0))) return -1;
    if (!CU_OK(cuMemAddressReserve(&v->ptr, v->size, gran, 0, 0))) return -1;
    if (!CU_OK(cuMemMap(v->ptr, v->size, 0, v->mh, 0))) return -1;
    CUmemAccessDesc acc = {.location = prop.location, .flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE};
    if (!CU_OK(cuMemSetAccess(v->ptr, v->size, &acc, 1))) return -1;
    if (!CU_OK(cuMemExportToShareableHandle(&v->opaque_fd, v->mh, CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR, 0)))
        return -1;

    return rdx_vmm_reexport(v);
}

int rdx_vmm_reexport(struct rdx_vmm *v)
{
    if (v->dmabuf_fd >= 0) close(v->dmabuf_fd);
    if (v->rm_ctl >= 0) close(v->rm_ctl);
    v->dmabuf_fd = v->rm_ctl = -1;
    v->via = RDX_VMM_VIA_NONE;
    int dmabuf_ok = 0;
    cuDeviceGetAttribute(&dmabuf_ok, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, v->dev);
    if (dmabuf_ok) {
        CUresult r = cuMemGetHandleForAddressRange(&v->dmabuf_fd, v->ptr, v->size,
                                                   CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
        if (r == CUDA_SUCCESS) v->via = RDX_VMM_VIA_CUDA;
        else { cu_ok(r, "cuMemGetHandleForAddressRange"); v->dmabuf_fd = -1; }
    } else {
        static int told;
        if (!told++) fprintf(stderr, "[vmm] CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED = 0 on %s\n", v->name);
    }
#ifdef RDX_WITH_OGKM
    if (v->dmabuf_fd < 0 && rm_export_dmabuf(v) == 0) v->via = RDX_VMM_VIA_RM;
#endif
    if (v->dmabuf_fd < 0) { fprintf(stderr, "[vmm] no dma-buf for the NIC\n"); return -1; }
    return 0;
}

int rdx_vmm_dup_opaque(const struct rdx_vmm *v)
{
    return v->opaque_fd >= 0 ? fcntl(v->opaque_fd, F_DUPFD_CLOEXEC, 0) : -1;
}

void rdx_vmm_free(struct rdx_vmm *v)
{
    if (v->dmabuf_fd >= 0) close(v->dmabuf_fd);
    if (v->rm_ctl >= 0) close(v->rm_ctl);
    if (v->opaque_fd >= 0) close(v->opaque_fd);
    if (v->ptr) { cuMemUnmap(v->ptr, v->size); cuMemAddressFree(v->ptr, v->size); }
    if (v->mh) cuMemRelease(v->mh);
    if (v->ctx) cuDevicePrimaryCtxRelease(v->dev);
    memset(v, 0, sizeof(*v));
    v->opaque_fd = v->dmabuf_fd = v->rm_ctl = -1;
}
