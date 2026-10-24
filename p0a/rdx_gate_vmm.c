/* Gate G-c1 (the gamescope path): can Vulkan render into memory the NIC reads, without a copy?
 *
 *   CUDA VMM allocation (one 1080p XRGB8888 frame)
 *     -> dma-buf -> ibv_reg_dmabuf_mr                       (the NIC view)
 *     -> opaque fd -> Vulkan LINEAR B8G8R8A8 image           (the renderer view)
 *   Vulkan writes an x/y pattern, CUDA reads it back (same memory?), then the NIC
 *   reads the VRAM with RDMA WRITE over a loopback QP pair into host RAM and the
 *   CPU verifies it. Reports rowPitch, NIC read rate from VRAM and a negative
 *   control (registering beyond the allocation must fail).
 *
 *   rdx_gate_vmm [--dev auto] [--cuda 0] [--size 1920x1080] [--iters 100]
 */
#include "../common/rdx_common.h"
#include "../common/rdx_vmm.h"

#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define VK_OK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[vk] %s -> %d\n", #x, _r); return -1; } } while (0)

struct vk {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkImage img;
    VkDeviceMemory mem;
    VkDeviceSize row_pitch, offset;
    VkCommandPool pool;
};

static uint32_t pattern(uint32_t x, uint32_t y, uint32_t seed)
{
    return 0xFF000000u | ((x * 7u + y * 13u + seed * 0x9E3779u) & 0x00FFFFFFu);
}

static int find_mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void report_usage_support(VkPhysicalDevice pd, VkFormat fmt)
{
    static const struct { VkImageUsageFlags u; const char *n; } us[] = {
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, "transfer_dst"},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "color_attachment"},
        {VK_IMAGE_USAGE_STORAGE_BIT, "storage"},
    };
    for (size_t i = 0; i < sizeof(us) / sizeof(us[0]); i++) {
        VkPhysicalDeviceExternalImageFormatInfo ext = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkPhysicalDeviceImageFormatInfo2 fi = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &ext,
            .format = fmt, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_LINEAR, .usage = us[i].u};
        VkExternalImageFormatProperties eprop = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 prop = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eprop};
        VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &prop);
        VkExternalMemoryFeatureFlags f = eprop.externalMemoryProperties.externalMemoryFeatures;
        printf("[vk] LINEAR fmt %d usage %-16s opaque-fd import: %s (max %ux%u)\n", fmt, us[i].n,
               r != VK_SUCCESS ? "NO (format)" : (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "yes" : "NO (not importable)",
               prop.imageFormatProperties.maxExtent.width, prop.imageFormatProperties.maxExtent.height);
    }
}

static int vk_init(struct vk *V, const struct rdx_vmm *vm, uint32_t w, uint32_t h)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rdx_gate_vmm",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VK_OK(vkCreateInstance(&ici, NULL, &V->inst));

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(V->inst, &n, NULL);
    VkPhysicalDevice pds[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(V->inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp};
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (!memcmp(idp.deviceUUID, vm->uuid, 16)) {
            V->pd = pds[i];
            printf("[vk] physical device by UUID: %s\n", p2.properties.deviceName);
        }
    }
    if (!V->pd) { fprintf(stderr, "[vk] no Vulkan device with the CUDA UUID\n"); return -1; }
    report_usage_support(V->pd, VK_FORMAT_B8G8R8A8_UNORM);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(V->pd, &nq, NULL);
    VkQueueFamilyProperties qfp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(V->pd, &nq, qfp);
    V->qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { V->qf = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V->qf,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = exts};
    VK_OK(vkCreateDevice(V->pd, &dci, NULL, &V->dev));
    vkGetDeviceQueue(V->dev, V->qf, 0, &V->q);

    VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi,
                            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_LINEAR,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_OK(vkCreateImage(V->dev, &ii, NULL, &V->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(V->dev, V->img, &mr);
    printf("[vk] image needs %llu B (align %llu), VMM allocation is %zu B, type bits 0x%x\n",
           (unsigned long long)mr.size, (unsigned long long)mr.alignment, vm->size, mr.memoryTypeBits);
    if (mr.size > vm->size) { fprintf(stderr, "[vk] image does not fit into the allocation\n"); return -1; }
    int mt = find_mem_type(V->pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { fprintf(stderr, "[vk] no device-local memory type\n"); return -1; }

    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                   .fd = rdx_vmm_dup_opaque(vm)};   /* Vulkan owns this fd on success */
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                .allocationSize = vm->size, .memoryTypeIndex = (uint32_t)mt};
    VK_OK(vkAllocateMemory(V->dev, &mai, NULL, &V->mem));
    VK_OK(vkBindImageMemory(V->dev, V->img, V->mem, 0));

    VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout lay;
    vkGetImageSubresourceLayout(V->dev, V->img, &sub, &lay);
    V->row_pitch = lay.rowPitch;
    V->offset = lay.offset;
    printf("[vk] imported as LINEAR image: offset %llu, rowPitch %llu (tight = %u), size %llu\n",
           (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch, w * 4, (unsigned long long)lay.size);

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V->qf};
    VK_OK(vkCreateCommandPool(V->dev, &pci, NULL, &V->pool));
    return 0;
}

/* Vulkan writes pattern(seed) into the imported image: host staging buffer -> copy into the image */
static int vk_write_pattern(struct vk *V, uint32_t w, uint32_t h, uint32_t seed)
{
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer buf;
    VK_OK(vkCreateBuffer(V->dev, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(V->dev, buf, &mr);
    int mt = find_mem_type(V->pd, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)mt};
    VkDeviceMemory bm;
    VK_OK(vkAllocateMemory(V->dev, &mai, NULL, &bm));
    VK_OK(vkBindBufferMemory(V->dev, buf, bm, 0));
    uint32_t *px;
    VK_OK(vkMapMemory(V->dev, bm, 0, bytes, 0, (void **)&px));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) px[(size_t)y * w + x] = pattern(x, y, seed);
    vkUnmapMemory(V->dev, bm);

    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V->pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    VK_OK(vkAllocateCommandBuffers(V->dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = V->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, V->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
    /* release to "external": everything written must be visible to CUDA and the NIC */
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_OK(vkCreateFence(V->dev, &fci, NULL, &fence));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VK_OK(vkQueueSubmit(V->q, 1, &si, fence));
    VK_OK(vkWaitForFences(V->dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(V->dev, fence, NULL);
    vkFreeCommandBuffers(V->dev, V->pool, 1, &cb);
    vkDestroyBuffer(V->dev, buf, NULL);
    vkFreeMemory(V->dev, bm, NULL);
    return 0;
}

static long check_frame(const uint8_t *base, uint64_t off, uint64_t pitch, uint32_t w, uint32_t h, uint32_t seed)
{
    long bad = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(base + off + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++)
            if (row[x] != pattern(x, y, seed)) {
                if (!bad) printf("[chk] first mismatch at %u,%u: 0x%08x != 0x%08x\n", x, y, row[x], pattern(x, y, seed));
                bad++;
            }
    }
    return bad;
}

static int poll_one(struct ibv_cq *cq)
{
    struct ibv_wc wc;
    int n;
    while ((n = ibv_poll_cq(cq, 1, &wc)) == 0) ;   /* gate measurement: completion within microseconds */
    if (n < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[nic] completion %s\n", n < 0 ? "poll error" : ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *devname = "auto";
    int cuda_ord = 0, iters = 100;
    uint32_t w = 1920, h = 1080;
    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"cuda", 1, 0, 'c'}, {"size", 1, 0, 's'}, {"iters", 1, 0, 'n'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': devname = optarg; break;
        case 'c': cuda_ord = atoi(optarg); break;
        case 's': if (rdx_parse_res(optarg, &w, &h)) { fprintf(stderr, "invalid --size\n"); return 2; } break;
        case 'n': iters = atoi(optarg); break;
        default:  fprintf(stderr, "see the header of rdx_gate_vmm.c for options\n"); return 2;
        }
    }

    /* 1. VMM allocation with an NIC view and a Vulkan view */
    struct rdx_vmm vm;
    size_t want = (size_t)w * h * 4 + 64 * 1024;          /* room for a pitch above tight */
    if (rdx_vmm_alloc(&vm, cuda_ord, want)) { printf("=== G-c1: FAIL at VMM allocation / dma-buf ===\n"); return 1; }
    printf("[vmm] %s (PCI bus 0x%02x): %zu B, dma-buf via %s\n", vm.name, vm.bus, vm.size, rdx_vmm_via_name(vm.via));

    struct vk V;
    memset(&V, 0, sizeof(V));
    if (vk_init(&V, &vm, w, h)) { printf("=== G-c1: FAIL at Vulkan import ===\n"); return 1; }
    uint64_t frame_bytes = V.offset + V.row_pitch * h;

    /* 2. Vulkan writes, CUDA reads back the same memory */
    uint8_t *host = aligned_alloc(4096, vm.size);
    if (vk_write_pattern(&V, w, h, 1)) return 1;
    if (cuMemcpyDtoH(host, vm.ptr, frame_bytes) != CUDA_SUCCESS) { fprintf(stderr, "cuMemcpyDtoH failed\n"); return 1; }
    long bad_cuda = check_frame(host, V.offset, V.row_pitch, w, h, 1);
    printf("[chk] Vulkan wrote seed 1, CUDA reads it back: %ld bad pixels\n", bad_cuda);

    /* 3. the NIC reads the VRAM: loopback QP pair, RDMA WRITE VRAM -> host RAM */
    struct ibv_context *ctx = rdx_open_dev(devname);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    int acc = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    errno = 0;
    struct ibv_mr *bad_mr = ibv_reg_dmabuf_mr(pd, 0, vm.size * 4, 0, vm.dmabuf_fd, acc);
    printf("[neg] register 4x beyond the allocation: %s\n",
           bad_mr ? "ACCEPTED (control FAILED)" : strerror(errno));
    if (bad_mr) ibv_dereg_mr(bad_mr);
    struct ibv_mr *src = ibv_reg_dmabuf_mr(pd, 0, vm.size, 0, vm.dmabuf_fd, acc);
    if (!src) { printf("[nic] ibv_reg_dmabuf_mr: %s\n=== G-c1: FAIL at NIC registration ===\n", strerror(errno)); return 1; }
    memset(host, 0, vm.size);
    struct ibv_mr *dst = ibv_reg_mr(pd, host, vm.size, acc);
    struct ibv_cq *cq = ibv_create_cq(ctx, 64, NULL, NULL, 0);
    struct ibv_qp *qa = rdx_create_qp(pd, cq, 16), *qb = rdx_create_qp(pd, cq, 16);
    if (!src || !dst || !cq || !qa || !qb) { perror("verbs setup"); return 1; }
    uint8_t gid[16];
    int gidx = rdx_find_gid_index(ctx, devname, gid);
    if (gidx < 0) { fprintf(stderr, "no RoCE v2 GID on %s\n", devname); return 1; }
    struct rdx_conn ca = {.qpn = qa->qp_num, .psn = 0x1234}, cb = {.qpn = qb->qp_num, .psn = 0x4321};
    memcpy(ca.gid, gid, 16);
    memcpy(cb.gid, gid, 16);
    if (rdx_qp_to_init(qa) || rdx_qp_to_init(qb) ||
        rdx_qp_to_rtr_rts(qa, &cb, gidx, ca.psn, IBV_MTU_4096) || rdx_qp_to_rtr_rts(qb, &ca, gidx, cb.psn, IBV_MTU_4096))
        return 1;

    struct ibv_sge sge = {.addr = 0, .length = (uint32_t)frame_bytes, .lkey = src->lkey};   /* iova 0 = start of the VMM range */
    struct ibv_send_wr wr = {.sg_list = &sge, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED,
                             .wr.rdma = {.remote_addr = (uintptr_t)host, .rkey = dst->rkey}}, *badwr;
    if (ibv_post_send(qa, &wr, &badwr) || poll_one(cq)) return 1;
    long bad_nic = check_frame(host, V.offset, V.row_pitch, w, h, 1);
    printf("[chk] NIC read VRAM -> host RAM: %ld bad pixels\n", bad_nic);

    /* a second Vulkan write must show up in the next NIC read (no stale view) */
    if (vk_write_pattern(&V, w, h, 2)) return 1;
    memset(host, 0, frame_bytes);
    if (ibv_post_send(qa, &wr, &badwr) || poll_one(cq)) return 1;
    long bad_nic2 = check_frame(host, V.offset, V.row_pitch, w, h, 2);
    printf("[chk] after Vulkan rewrite (seed 2), NIC read: %ld bad pixels\n", bad_nic2);

    /* read rate: serial (one frame in flight) and pipelined (depth 4) */
    double *lat = calloc((size_t)iters, sizeof(double));
    for (int i = 0; i < iters; i++) {
        uint64_t t0 = rdx_now_ns();
        if (ibv_post_send(qa, &wr, &badwr) || poll_one(cq)) return 1;
        lat[i] = (double)(rdx_now_ns() - t0) / 1e3;
    }
    printf("[rate] frame = %llu B\n", (unsigned long long)frame_bytes);
    rdx_report("VRAM->NIC->RAM one frame (us)", lat, (size_t)iters);
    int depth = 4, posted = 0, done = 0;
    uint64_t t0 = rdx_now_ns();
    while (done < iters) {
        while (posted < iters && posted - done < depth) {
            if (ibv_post_send(qa, &wr, &badwr)) { perror("post"); return 1; }
            posted++;
        }
        if (poll_one(cq)) return 1;
        done++;
    }
    double s = (double)(rdx_now_ns() - t0) / 1e9;
    printf("[rate] pipelined depth %d: %.2f GB/s (%.1f us per frame)\n", depth,
           (double)frame_bytes * iters / s / 1e9, s * 1e6 / iters);

    int ok = bad_cuda == 0 && bad_nic == 0 && bad_nic2 == 0;
    printf("=== G-c1: %s (pitch %llu, dma-buf via %s) ===\n", ok ? "PASS" : "FAIL",
           (unsigned long long)V.row_pitch, rdx_vmm_via_name(vm.via));
    return ok ? 0 : 1;
}
