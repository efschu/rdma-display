/* Output buffers of the gamescope path (VRAM or pinned host memory), see rdx_gsbuf.h. */
#define _GNU_SOURCE
#include "rdx_gsbuf.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <linux/memfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define HUGE_2M (2ull << 20)

/* Bridges between the root complex and a PCI function: /sys/devices/pci0000:00/0000:00:03.1/0000:04:00.0
 * is 1 (a root port), a device behind a switch or the chipset shows more. -1 if unknown. */
static int pci_hops(const char *sys_path)
{
    char real[PATH_MAX];
    if (!realpath(sys_path, real)) return -1;
    const char *p = strstr(real, "/pci");
    if (!p) return -1;
    p = strchr(p + 1, '/');              /* skip the root complex component "pci0000:00" */
    int n = 0;
    while (p && *p) {
        const char *q = strchr(p + 1, '/');
        size_t len = q ? (size_t)(q - p - 1) : strlen(p + 1);
        /* a BDF component looks like 0000:00:03.1 */
        if (len == 12 && p[5] == ':' && p[8] == ':' && p[11] == '.') n++;
        p = q;
    }
    return n - 1;                        /* bridges above the function */
}

/* may the NIC peer-read this GPU's VRAM? Prints why or why not. On the test host (RTX 2080 Ti
 * behind the chipset, stock driver) peer reads stalled for ~60 ms when an app started on the
 * GPU (local protection error, vendor_err 0x51), and repeated stalls took the GPU down
 * (Xid 62/154, "GPU requires reset") - docs/GAMESCOPE.md. */
static int vram_safe(int cuda_ord, const char *rdma_dev)
{
    char name[96] = "?", bdf[32] = "", gpu_sys[128], nic_sys[160];
    uint8_t uuid[16];
    int dmabuf = 0;
    if (rdx_vmm_probe(cuda_ord, name, sizeof(name), uuid, &dmabuf, bdf, sizeof(bdf))) {
        printf("[buf] CUDA device %d not usable\n", cuda_ord);
        return 0;
    }
    snprintf(gpu_sys, sizeof(gpu_sys), "/sys/bus/pci/devices/%s", bdf);
    snprintf(nic_sys, sizeof(nic_sys), "/sys/class/infiniband/%s/device", rdma_dev);
    int gpu_hops = pci_hops(gpu_sys), nic_hops = pci_hops(nic_sys);
    int holder = access("/dev/dmabuf_holder", F_OK) == 0;
    int vram = gpu_hops == 1 && nic_hops == 1 && holder;
    printf("[buf] %s (%s) %s, NIC %s %s, barlink stack (/dev/dmabuf_holder) %s, CUDA dma-buf %s: VRAM peer reads %s\n",
           name, bdf, gpu_hops == 1 ? "at a CPU root port" : gpu_hops > 1 ? "behind a switch/chipset" : "placement unknown",
           rdma_dev, nic_hops == 1 ? "at a CPU root port" : nic_hops > 1 ? "behind a switch/chipset" : "placement unknown",
           holder ? "present" : "absent", dmabuf ? "yes" : "no (RM export)", vram ? "safe" : "not safe");
    return vram;
}

int rdx_gsbuf_choose(const char *want, int cuda_ord, const char *rdma_dev)
{
    if (!strcmp(want, "host")) return RDX_GSBUF_HOST;
    if (!strcmp(want, "vram-force")) {
        printf("[buf] vram forced: the NIC peer-reads VRAM without the safety rule\n");
        return RDX_GSBUF_VRAM;
    }
    if (!strcmp(want, "vram")) {
        if (vram_safe(cuda_ord, rdma_dev)) return RDX_GSBUF_VRAM;
        fprintf(stderr, "[buf] --buffers vram refused on this machine (use host, or vram-force)\n");
        return -2;
    }
    if (strcmp(want, "auto")) return -1;
    int kind = vram_safe(cuda_ord, rdma_dev) ? RDX_GSBUF_VRAM : RDX_GSBUF_HOST;
    printf("[buf] auto -> %s\n", kind == RDX_GSBUF_VRAM ? "vram" : "host");
    return kind;
}

static int host_alloc(struct rdx_gsbuf *b, size_t bytes)
{
    /* 2 MiB huge pages if the pool has them (fewer NIC translation entries), else 4 KiB pages */
    b->size = (bytes + HUGE_2M - 1) & ~(HUGE_2M - 1);
    /* sealable, size sealed: /dev/udmabuf turns such a memfd into a dma-bufs of the same pages
     * (rdx_gsbuf_dmabuf), for GPUs that cannot import a host pointer of a shared mapping */
    b->memfd = memfd_create("rdx-gs-slot", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB | MFD_HUGE_2MB);
    if (b->memfd >= 0 && ftruncate(b->memfd, (off_t)b->size) == 0) {
        b->host = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, b->memfd, 0);
        if (b->host != MAP_FAILED) b->huge = 1;
    }
    if (!b->huge) {
        if (b->memfd >= 0) close(b->memfd);
        b->memfd = memfd_create("rdx-gs-slot", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (b->memfd < 0 || ftruncate(b->memfd, (off_t)b->size)) { perror("memfd"); return -1; }
        b->host = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, b->memfd, 0);
        if (b->host == MAP_FAILED) { b->host = NULL; perror("mmap host slot"); return -1; }
    }
    fcntl(b->memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);   /* udmabuf needs F_SEAL_SHRINK */
    if (mlock(b->host, b->size)) perror("mlock host slot (continuing, the MR pins it)");
    memset(b->host, 0, b->size);
    return 0;
}

static int reg(struct rdx_gsbuf *b, struct ibv_pd *pd)
{
    int acc = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
    b->mr = b->kind == RDX_GSBUF_VRAM ? ibv_reg_dmabuf_mr(pd, 0, b->vm.size, 0, b->vm.dmabuf_fd, acc)
                                      : ibv_reg_mr(pd, b->host, b->size, acc);
    if (!b->mr) perror(b->kind == RDX_GSBUF_VRAM ? "ibv_reg_dmabuf_mr" : "ibv_reg_mr");
    return b->mr ? 0 : -1;
}

int rdx_gsbuf_alloc(struct rdx_gsbuf *b, int kind, int cuda_ord, size_t bytes, struct ibv_pd *pd)
{
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->memfd = -1;
    b->vm.opaque_fd = b->vm.dmabuf_fd = b->vm.rm_ctl = -1;
    if (kind == RDX_GSBUF_VRAM) {
        if (rdx_vmm_alloc(&b->vm, cuda_ord, bytes)) return -1;
        b->size = b->vm.size;
    } else if (host_alloc(b, bytes)) {
        return -1;
    }
    return reg(b, pd);
}

int rdx_gsbuf_fd(const struct rdx_gsbuf *b)
{
    return b->kind == RDX_GSBUF_VRAM ? b->vm.opaque_fd : b->memfd;
}

uint64_t rdx_gsbuf_addr(const struct rdx_gsbuf *b)
{
    return b->kind == RDX_GSBUF_VRAM ? 0 : (uint64_t)(uintptr_t)b->host;
}

int rdx_gsbuf_reregister(struct rdx_gsbuf *b, struct ibv_pd *pd)
{
    if (b->mr) ibv_dereg_mr(b->mr);
    b->mr = NULL;
    if (b->kind == RDX_GSBUF_VRAM && rdx_vmm_reexport(&b->vm)) return -1;
    return reg(b, pd);
}

const char *rdx_gsbuf_describe(const struct rdx_gsbuf *b)
{
    static char s[160];
    if (b->kind == RDX_GSBUF_VRAM)
        snprintf(s, sizeof(s), "VRAM on %s, dma-buf via %s", b->vm.name, rdx_vmm_via_name(b->vm.via));
    else
        snprintf(s, sizeof(s), "pinned host memory, %s pages", b->huge ? "2 MiB huge" : "4 KiB");
    return s;
}

int rdx_gsbuf_dmabuf(const struct rdx_gsbuf *b)
{
    if (b->kind != RDX_GSBUF_HOST || b->memfd < 0) return -1;
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) { perror("/dev/udmabuf (modprobe udmabuf)"); return -1; }
    struct udmabuf_create c = {.memfd = (uint32_t)b->memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = b->size};
    int fd = ioctl(dev, UDMABUF_CREATE, &c);
    if (fd < 0) perror("UDMABUF_CREATE");
    close(dev);
    return fd;
}

void rdx_gsbuf_free(struct rdx_gsbuf *b)
{
    if (b->mr) ibv_dereg_mr(b->mr);
    if (b->kind == RDX_GSBUF_VRAM) rdx_vmm_free(&b->vm);
    if (b->host) munmap(b->host, b->size);
    if (b->memfd >= 0) close(b->memfd);
    memset(b, 0, sizeof(*b));
    b->memfd = -1;
}
