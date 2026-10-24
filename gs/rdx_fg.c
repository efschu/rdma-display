/* SPDX-License-Identifier: MIT
 * Frame generation on a helper GPU between host-memory frames: see rdx_fg.h. The optical flow
 * and the interpolation are afmf-linux's (third_party/afmf-linux), driven here outside of a
 * Vulkan layer: we own the instance and the device, fill afmf's struct afmf_device with the
 * loader's entry points and record afmf_framegen_record_host() into our own command buffer. */
#define _GNU_SOURCE
#include "rdx_fg.h"

#include "../third_party/afmf-linux/src/framegen_host.h"
#include "../third_party/afmf-linux/src/framegen_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define FG_MAX_BUFS 32
#define FG_RING 8

struct fg_buf {
    void *ptr;
    size_t size;
    VkBuffer buf;
    VkDeviceMemory mem;
};

struct fg_job {
    int src, dst, staging;
    uint64_t tag;
    uint64_t t_submit;
#ifdef RDX_FG_CUDA
    CUcontext ctx;
    CUdeviceptr cu_src;
#endif
    int cuda;
};

struct rdx_fg {
    struct rdx_fg_cfg cfg;
    char desc[320];
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkDeviceSize host_align;
    uint32_t host_types;         /* memory types the device offers for host pointers (from the first import) */
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_props;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_props;
    int have_dmabuf;
    struct afmf_device ad;
    struct afmf_framegen *afmf;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkQueryPool qp;
    double ts_period_ns;
    struct fg_buf bufs[FG_MAX_BUFS];
    int nbufs;

    pthread_t th;
    int th_running;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int stop;
    struct fg_job jobs[FG_RING];
    int j_head, j_n;
    struct rdx_fg_done done[FG_RING];
    int d_head, d_n;
    int in_flight;                /* taken by the worker, not yet in done[] */
    int reset_next;
    int efd;
};

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* "0000:0e:00.0" -> domain/bus/dev/fn; 0 = ok */
static int parse_pci(const char *s, unsigned *dom, unsigned *bus, unsigned *dev, unsigned *fn)
{
    if (sscanf(s, "%x:%x:%x.%x", dom, bus, dev, fn) == 4) return 0;
    *dom = 0;
    return sscanf(s, "%x:%x.%x", bus, dev, fn) == 3 ? 0 : -1;
}

static int has_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

/* every function afmf's framegen calls, from the loader's own exports: the command buffers we
 * allocate through the loader carry its dispatch pointer, so its trampolines are the right entry */
static void load_fns(struct afmf_device *d)
{
    struct afmf_device_fns *f = &d->fns;
    f->destroy_device = vkDestroyDevice;
    f->get_device_queue = vkGetDeviceQueue;
    f->get_device_queue2 = vkGetDeviceQueue2;
    f->create_image = vkCreateImage;
    f->destroy_image = vkDestroyImage;
    f->get_image_memory_requirements = vkGetImageMemoryRequirements;
    f->allocate_memory = vkAllocateMemory;
    f->free_memory = vkFreeMemory;
    f->bind_image_memory = vkBindImageMemory;
    f->create_semaphore = vkCreateSemaphore;
    f->destroy_semaphore = vkDestroySemaphore;
    f->create_fence = vkCreateFence;
    f->destroy_fence = vkDestroyFence;
    f->wait_for_fences = vkWaitForFences;
    f->reset_fences = vkResetFences;
    f->create_command_pool = vkCreateCommandPool;
    f->destroy_command_pool = vkDestroyCommandPool;
    f->allocate_command_buffers = vkAllocateCommandBuffers;
    f->begin_command_buffer = vkBeginCommandBuffer;
    f->end_command_buffer = vkEndCommandBuffer;
    f->cmd_pipeline_barrier = vkCmdPipelineBarrier;
    f->cmd_copy_image = vkCmdCopyImage;
    f->queue_submit = vkQueueSubmit;
    f->queue_wait_idle = vkQueueWaitIdle;
    f->device_wait_idle = vkDeviceWaitIdle;
    f->create_shader_module = vkCreateShaderModule;
    f->destroy_shader_module = vkDestroyShaderModule;
    f->create_descriptor_set_layout = vkCreateDescriptorSetLayout;
    f->destroy_descriptor_set_layout = vkDestroyDescriptorSetLayout;
    f->create_pipeline_layout = vkCreatePipelineLayout;
    f->destroy_pipeline_layout = vkDestroyPipelineLayout;
    f->create_compute_pipelines = vkCreateComputePipelines;
    f->destroy_pipeline = vkDestroyPipeline;
    f->create_descriptor_pool = vkCreateDescriptorPool;
    f->destroy_descriptor_pool = vkDestroyDescriptorPool;
    f->allocate_descriptor_sets = vkAllocateDescriptorSets;
    f->update_descriptor_sets = vkUpdateDescriptorSets;
    f->create_image_view = vkCreateImageView;
    f->destroy_image_view = vkDestroyImageView;
    f->create_sampler = vkCreateSampler;
    f->destroy_sampler = vkDestroySampler;
    f->create_buffer = vkCreateBuffer;
    f->destroy_buffer = vkDestroyBuffer;
    f->get_buffer_memory_requirements = vkGetBufferMemoryRequirements;
    f->bind_buffer_memory = vkBindBufferMemory;
    f->map_memory = vkMapMemory;
    f->unmap_memory = vkUnmapMemory;
    f->cmd_bind_pipeline = vkCmdBindPipeline;
    f->cmd_bind_descriptor_sets = vkCmdBindDescriptorSets;
    f->cmd_dispatch = vkCmdDispatch;
    f->cmd_push_constants = vkCmdPushConstants;
    f->cmd_clear_color_image = vkCmdClearColorImage;
    f->cmd_copy_image_to_buffer = vkCmdCopyImageToBuffer;
    f->cmd_execute_commands = vkCmdExecuteCommands;
    f->create_query_pool = vkCreateQueryPool;
    f->destroy_query_pool = vkDestroyQueryPool;
    f->cmd_reset_query_pool = vkCmdResetQueryPool;
    f->cmd_write_timestamp = vkCmdWriteTimestamp;
    f->get_query_pool_results = vkGetQueryPoolResults;
    d->ifns.get_format_properties = vkGetPhysicalDeviceFormatProperties;
}

static int pick_device(struct rdx_fg *fg, char *err, size_t errn)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(fg->inst, &n, NULL);
    if (!n) { set_err(err, errn, "no Vulkan GPU"); return -1; }
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(fg->inst, &n, pds);
    unsigned dom = 0, bus = 0, dv = 0, fn = 0;
    int want = fg->cfg.pci && *fg->cfg.pci;
    if (want && parse_pci(fg->cfg.pci, &dom, &bus, &dv, &fn)) {
        set_err(err, errn, "bad PCI address \"%s\"", fg->cfg.pci);
        free(pds);
        return -1;
    }
    for (uint32_t i = 0; i < n && !fg->pd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;    /* llvmpipe */
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        int have_pci = has_ext(pds[i], VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
        if (have_pci) p2.pNext = &pci;
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (want && (!have_pci || pci.pciDomain != dom || pci.pciBus != bus || pci.pciDevice != dv ||
                     pci.pciFunction != fn))
            continue;
        if (!has_ext(pds[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
            if (want) { set_err(err, errn, "%s lacks VK_EXT_external_memory_host", p.deviceName); free(pds); return -1; }
            continue;
        }
        fg->pd = pds[i];
        snprintf(fg->desc, sizeof(fg->desc), "%s (%04x:%02x:%02x.%x)", p.deviceName, pci.pciDomain, pci.pciBus,
                 pci.pciDevice, pci.pciFunction);
    }
    free(pds);
    if (!fg->pd) { set_err(err, errn, want ? "no Vulkan GPU at %s" : "no usable Vulkan GPU", fg->cfg.pci); return -1; }
    return 0;
}

static int create_device(struct rdx_fg *fg, char *err, size_t errn)
{
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(fg->pd, &nf, NULL);
    VkQueueFamilyProperties *qf = calloc(nf ? nf : 1, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(fg->pd, &nf, qf);
    /* a compute family without graphics first: it runs beside whatever else uses the card */
    int fam = -1;
    for (uint32_t i = 0; i < nf && fam < 0; i++)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            qf[i].timestampValidBits)
            fam = (int)i;
    for (uint32_t i = 0; i < nf && fam < 0; i++)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qf[i].timestampValidBits) fam = (int)i;
    free(qf);
    if (fam < 0) { set_err(err, errn, "no compute queue with timestamps"); return -1; }
    fg->family = (uint32_t)fam;

    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(fg->pd, &have);
    VkPhysicalDeviceFeatures2 use = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    use.features.shaderInt16 = have.features.shaderInt16;
    use.features.shaderStorageImageExtendedFormats = have.features.shaderStorageImageExtendedFormats;

    /* host memory: a host pointer (NVIDIA) or, where the driver imports host pointers of
     * anonymous memory only (amdgpu), a dma-buf of the same pages (udmabuf) */
    const char *exts[5] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
    uint32_t next = 1;
    fg->have_dmabuf = has_ext(fg->pd, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
                      has_ext(fg->pd, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (fg->have_dmabuf) {
        exts[next++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        exts[next++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    const uint32_t base_exts = next;
    const char *prio_ext = has_ext(fg->pd, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME
                         : has_ext(fg->pd, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) ? VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME
                                                                                  : NULL;
    float qprio = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoKHR gp = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR,
                                                   .globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR};
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = fg->family,
                                   .queueCount = 1, .pQueuePriorities = &qprio};
    int try_prio = fg->cfg.high_priority && prio_ext;
    if (try_prio) { exts[next++] = prio_ext; qci.pNext = &gp; }
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &use, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    VkResult r = vkCreateDevice(fg->pd, &dci, NULL, &fg->dev);
    if (r != VK_SUCCESS && try_prio) {     /* high priority needs CAP_SYS_NICE: VK_ERROR_NOT_PERMITTED */
        qci.pNext = NULL;
        dci.enabledExtensionCount = base_exts;
        r = vkCreateDevice(fg->pd, &dci, NULL, &fg->dev);
        try_prio = 0;
    }
    if (r != VK_SUCCESS) { set_err(err, errn, "vkCreateDevice: %d", (int)r); return -1; }
    vkGetDeviceQueue(fg->dev, fg->family, 0, &fg->queue);
    fg->get_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(fg->dev, "vkGetMemoryHostPointerPropertiesEXT");
    if (!fg->get_host_props) { set_err(err, errn, "vkGetMemoryHostPointerPropertiesEXT missing"); return -1; }
    if (fg->have_dmabuf)
        fg->get_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(fg->dev, "vkGetMemoryFdPropertiesKHR");

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &hp};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
    vkGetPhysicalDeviceProperties2(fg->pd, &p2);
    fg->host_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    fg->ts_period_ns = p2.properties.limits.timestampPeriod;

    struct afmf_device *d = &fg->ad;
    d->handle = fg->dev;
    d->physical_device = fg->pd;
    d->api_version = p2.properties.apiVersion < VK_API_VERSION_1_2 ? p2.properties.apiVersion : VK_API_VERSION_1_2;
    d->gdpa = vkGetDeviceProcAddr;
    d->set_loader_data = NULL;            /* no pre-recorded secondaries: recorded inline per frame */
    load_fns(d);
    vkGetPhysicalDeviceMemoryProperties(fg->pd, &d->memory_properties);
    d->limits = p2.properties.limits;
    d->subgroup = sg;
    d->subgroup.pNext = NULL;
    d->shader_int16 = use.features.shaderInt16 == VK_TRUE;
    d->storage_write_without_format = false;
    d->async_queue = fg->queue;
    d->async_family = fg->family;
    d->async_high_priority = try_prio;
    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->async_lock, NULL);
    return 0;
}

static void *worker(void *arg);

struct rdx_fg *rdx_fg_open(const struct rdx_fg_cfg *c, char *err, size_t errn)
{
    if (!c || c->width < 256 || c->height < 256 || c->stride < c->width * 4 || c->stride % 4) {
        set_err(err, errn, "bad frame geometry");
        return NULL;
    }
    struct rdx_fg *fg = calloc(1, sizeof(*fg));
    fg->cfg = *c;
    fg->efd = -1;
    /* afmf reads its configuration from the environment once (first use below) */
    const char *q = c->quality && *c->quality ? c->quality : "auto";
    setenv("AFMF_PERFORMANCE_MODE", q, 0);
    setenv("AFMF_LOG", "1", 0);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "rdx_fg",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ici, NULL, &fg->inst) != VK_SUCCESS) { set_err(err, errn, "vkCreateInstance failed"); goto fail; }
    if (pick_device(fg, err, errn) || create_device(fg, err, errn)) goto fail;

    afmf_framegen_pipelines_prepare(&fg->ad);
    fg->afmf = afmf_framegen_create(&fg->ad, VK_FORMAT_B8G8R8A8_UNORM, (VkExtent2D){c->width, c->height}, 2, NULL, 0,
                                    false, false);
    if (!fg->afmf) { set_err(err, errn, "afmf frame generation unavailable on %s (AFMF_LOG=2 tells why)", fg->desc); goto fail; }
    size_t dl = strlen(fg->desc);
    snprintf(fg->desc + dl, sizeof(fg->desc) - dl, ", flow at 1/%u, %u levels", afmf_framegen_flow_scale(fg->afmf),
             afmf_framegen_levels(fg->afmf));

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = fg->family};
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                 .queryCount = 2};
    if (vkCreateCommandPool(fg->dev, &pci, NULL, &fg->pool) != VK_SUCCESS) { set_err(err, errn, "command pool"); goto fail; }
    cai.commandPool = fg->pool;
    if (vkAllocateCommandBuffers(fg->dev, &cai, &fg->cmd) != VK_SUCCESS ||
        vkCreateFence(fg->dev, &fci, NULL, &fg->fence) != VK_SUCCESS ||
        vkCreateQueryPool(fg->dev, &qpi, NULL, &fg->qp) != VK_SUCCESS) {
        set_err(err, errn, "command buffer / fence / query pool");
        goto fail;
    }
    fg->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pthread_mutex_init(&fg->mu, NULL);
    pthread_cond_init(&fg->cv, NULL);
    if (fg->efd < 0 || pthread_create(&fg->th, NULL, worker, fg)) { set_err(err, errn, "worker thread"); goto fail; }
    fg->th_running = 1;
    return fg;
fail:
    rdx_fg_close(fg);
    return NULL;
}

const char *rdx_fg_describe(const struct rdx_fg *fg) { return fg ? fg->desc : ""; }
int rdx_fg_fd(const struct rdx_fg *fg) { return fg ? fg->efd : -1; }

int rdx_fg_import(struct rdx_fg *fg, void *ptr, size_t size)
{
    if (!fg || fg->nbufs >= FG_MAX_BUFS || !ptr || ((uintptr_t)ptr % fg->host_align) || (size % fg->host_align)) {
        if (fg) fprintf(stderr, "[fg] import refused: %p + %zu (alignment %llu)\n", ptr, size,
                        (unsigned long long)fg->host_align);
        return -1;
    }
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (fg->get_host_props(fg->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS ||
        !hp.memoryTypeBits) {
        fprintf(stderr, "[fg] the helper GPU cannot import host pointer %p\n", ptr);
        return -1;
    }
    struct fg_buf *b = &fg->bufs[fg->nbufs];
    VkExternalMemoryBufferCreateInfo ebi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(fg->dev, &bci, NULL, &b->buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(fg->dev, b->buf, &mr);
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits, type = 32;
    /* host-coherent first: the NIC and the CPU read what the helper wrote without a flush */
    for (uint32_t i = 0; i < fg->ad.memory_properties.memoryTypeCount && type == 32; i++)
        if ((bits & (1u << i)) &&
            (fg->ad.memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = i;
    for (uint32_t i = 0; i < 32 && type == 32; i++)
        if (bits & (1u << i)) type = i;
    if (type == 32) { vkDestroyBuffer(fg->dev, b->buf, NULL); return -1; }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = ptr};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                                .memoryTypeIndex = type};
    if (vkAllocateMemory(fg->dev, &mai, NULL, &b->mem) != VK_SUCCESS ||
        vkBindBufferMemory(fg->dev, b->buf, b->mem, 0) != VK_SUCCESS) {
        fprintf(stderr, "[fg] importing %zu B of host memory failed\n", size);
        if (b->mem) vkFreeMemory(fg->dev, b->mem, NULL);
        vkDestroyBuffer(fg->dev, b->buf, NULL);
        memset(b, 0, sizeof(*b));
        return -1;
    }
    b->ptr = ptr;
    b->size = size;
    return fg->nbufs++;
}

int rdx_fg_import_dmabuf(struct rdx_fg *fg, int fd, void *ptr, size_t size)
{
    if (!fg || fg->nbufs >= FG_MAX_BUFS || fd < 0 || !fg->get_fd_props) return -1;
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (fg->get_fd_props(fg->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS || !fp.memoryTypeBits)
        return -1;
    struct fg_buf *b = &fg->bufs[fg->nbufs];
    VkExternalMemoryBufferCreateInfo ebi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(fg->dev, &bci, NULL, &b->buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(fg->dev, b->buf, &mr);
    uint32_t bits = mr.memoryTypeBits & fp.memoryTypeBits, type = 32;
    for (uint32_t i = 0; i < 32 && type == 32; i++)
        if (bits & (1u << i)) type = i;
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                                .memoryTypeIndex = type};
    if (type == 32 || vkAllocateMemory(fg->dev, &mai, NULL, &b->mem) != VK_SUCCESS) {   /* fd stays the caller's */
        vkDestroyBuffer(fg->dev, b->buf, NULL);
        memset(b, 0, sizeof(*b));
        return -1;
    }
    if (vkBindBufferMemory(fg->dev, b->buf, b->mem, 0) != VK_SUCCESS) {                  /* fd is Vulkan's now */
        vkFreeMemory(fg->dev, b->mem, NULL);
        vkDestroyBuffer(fg->dev, b->buf, NULL);
        memset(b, 0, sizeof(*b));
        return -2;
    }
    b->ptr = ptr;
    b->size = size;
    return fg->nbufs++;
}

static int enqueue(struct rdx_fg *fg, struct fg_job *j)
{
    size_t need = (size_t)fg->cfg.stride * fg->cfg.height;
    if (j->src < 0 || j->src >= fg->nbufs || j->dst >= fg->nbufs || fg->bufs[j->src].size < need ||
        (j->dst >= 0 && fg->bufs[j->dst].size < need))
        return -1;
    pthread_mutex_lock(&fg->mu);
    if (fg->j_n + fg->d_n + fg->in_flight >= FG_RING) { pthread_mutex_unlock(&fg->mu); return -1; }
    if (fg->reset_next) { j->dst = -1; fg->reset_next = 0; }
    j->t_submit = now_ns();
    fg->jobs[(fg->j_head + fg->j_n) % FG_RING] = *j;
    fg->j_n++;
    pthread_cond_signal(&fg->cv);
    pthread_mutex_unlock(&fg->mu);
    return 0;
}

int rdx_fg_submit(struct rdx_fg *fg, int src, int dst, uint64_t tag)
{
    if (!fg) return -1;
    struct fg_job j = {.src = src, .dst = dst, .staging = -1, .tag = tag};
    return enqueue(fg, &j);
}

#ifdef RDX_FG_CUDA
int rdx_fg_submit_cuda(struct rdx_fg *fg, CUcontext ctx, CUdeviceptr src, int staging, int dst, uint64_t tag)
{
    if (!fg) return -1;
    struct fg_job j = {.src = staging, .dst = dst, .staging = staging, .tag = tag, .ctx = ctx, .cu_src = src, .cuda = 1};
    return enqueue(fg, &j);
}
#endif

int rdx_fg_reap(struct rdx_fg *fg, struct rdx_fg_done *d)
{
    if (!fg) return 0;
    uint64_t v;
    while (read(fg->efd, &v, sizeof(v)) == (ssize_t)sizeof(v)) {}
    pthread_mutex_lock(&fg->mu);
    int got = fg->d_n > 0;
    if (got) {
        *d = fg->done[fg->d_head];
        fg->d_head = (fg->d_head + 1) % FG_RING;
        fg->d_n--;
    }
    pthread_mutex_unlock(&fg->mu);
    return got;
}

int rdx_fg_pending(const struct rdx_fg *fg)
{
    if (!fg) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&fg->mu);
    int n = fg->j_n + fg->in_flight + fg->d_n;
    pthread_mutex_unlock((pthread_mutex_t *)&fg->mu);
    return n;
}

void rdx_fg_reset(struct rdx_fg *fg)
{
    if (!fg) return;
    pthread_mutex_lock(&fg->mu);
    fg->reset_next = 1;
    pthread_mutex_unlock(&fg->mu);
}

static int run_job(struct rdx_fg *fg, const struct fg_job *j, struct rdx_fg_done *d)
{
    const struct fg_buf *src = &fg->bufs[j->src], *dst = j->dst >= 0 ? &fg->bufs[j->dst] : NULL;
    uint32_t row_px = fg->cfg.stride / 4;
#ifdef RDX_FG_CUDA
    if (j->cuda) {
        uint64_t t = now_ns();
        CUresult cr = cuCtxPushCurrent(j->ctx);
        if (cr == CUDA_SUCCESS) {
            cr = cuMemcpyDtoH(src->ptr, j->cu_src, (size_t)fg->cfg.stride * fg->cfg.height);
            CUcontext dummy;
            cuCtxPopCurrent(&dummy);
        }
        d->stage_ms = (double)(now_ns() - t) / 1e6;
        if (cr != CUDA_SUCCESS) return -1;
    }
#endif
    vkResetCommandBuffer(fg->cmd, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(fg->cmd, &bi) != VK_SUCCESS) return -1;
    vkCmdResetQueryPool(fg->cmd, fg->qp, 0, 2);
    vkCmdWriteTimestamp(fg->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fg->qp, 0);
    if (!afmf_framegen_record_host(&fg->ad, fg->afmf, fg->cmd, 0, src->buf, 0, row_px,
                                   dst ? dst->buf : VK_NULL_HANDLE, 0, row_px))
        return -1;
    vkCmdWriteTimestamp(fg->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg->qp, 1);
    if (vkEndCommandBuffer(fg->cmd) != VK_SUCCESS) return -1;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &fg->cmd};
    if (vkQueueSubmit(fg->queue, 1, &si, fg->fence) != VK_SUCCESS) return -1;
    VkResult r = vkWaitForFences(fg->dev, 1, &fg->fence, VK_TRUE, 2000000000ull);
    vkResetFences(fg->dev, 1, &fg->fence);
    if (r != VK_SUCCESS) return -1;
    uint64_t ts[2] = {0, 0};
    if (vkGetQueryPoolResults(fg->dev, fg->qp, 0, 2, sizeof(ts), ts, sizeof(ts[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        ts[1] > ts[0])
        d->gpu_ms = (double)(ts[1] - ts[0]) * fg->ts_period_ns / 1e6;
    d->generated = dst != NULL;
    if (afmf_framegen_dump_recorded(fg->afmf))            /* AFMF_DUMP_DIR (debugging) */
        afmf_framegen_dump_write(&fg->ad, fg->afmf);
    return 0;
}

static void *worker(void *arg)
{
    struct rdx_fg *fg = arg;
    for (;;) {
        pthread_mutex_lock(&fg->mu);
        while (!fg->stop && !fg->j_n) pthread_cond_wait(&fg->cv, &fg->mu);
        if (fg->stop) { pthread_mutex_unlock(&fg->mu); break; }
        struct fg_job j = fg->jobs[fg->j_head];
        fg->j_head = (fg->j_head + 1) % FG_RING;
        fg->j_n--;
        fg->in_flight++;
        pthread_mutex_unlock(&fg->mu);

        struct rdx_fg_done d = {.tag = j.tag, .src = j.cuda ? -1 : j.src, .dst = j.dst};
        uint64_t t0 = now_ns();
        d.wait_ms = (double)(t0 - j.t_submit) / 1e6;
        if (run_job(fg, &j, &d)) { d.failed = 1; d.generated = 0; }
        d.total_ms = (double)(now_ns() - j.t_submit) / 1e6;

        pthread_mutex_lock(&fg->mu);
        fg->in_flight--;
        fg->done[(fg->d_head + fg->d_n) % FG_RING] = d;
        fg->d_n++;
        pthread_mutex_unlock(&fg->mu);
        uint64_t one = 1;
        if (write(fg->efd, &one, sizeof(one)) < 0) {}
    }
    return NULL;
}

void rdx_fg_close(struct rdx_fg *fg)
{
    if (!fg) return;
    if (fg->th_running) {
        pthread_mutex_lock(&fg->mu);
        fg->stop = 1;
        pthread_cond_signal(&fg->cv);
        pthread_mutex_unlock(&fg->mu);
        pthread_join(fg->th, NULL);
    }
    if (fg->dev) {
        vkDeviceWaitIdle(fg->dev);
        afmf_framegen_destroy(&fg->ad, fg->afmf);
        afmf_framegen_pipelines_destroy(&fg->ad);
        for (int i = 0; i < fg->nbufs; i++) {
            vkDestroyBuffer(fg->dev, fg->bufs[i].buf, NULL);
            vkFreeMemory(fg->dev, fg->bufs[i].mem, NULL);
        }
        if (fg->qp) vkDestroyQueryPool(fg->dev, fg->qp, NULL);
        if (fg->fence) vkDestroyFence(fg->dev, fg->fence, NULL);
        if (fg->pool) vkDestroyCommandPool(fg->dev, fg->pool, NULL);
        vkDestroyDevice(fg->dev, NULL);
    }
    if (fg->inst) vkDestroyInstance(fg->inst, NULL);
    if (fg->efd >= 0) close(fg->efd);
    free(fg);
}
