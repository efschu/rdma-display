/* Test load for the gamescope path: put a game's kind of memory pressure on one GPU.
 *
 *   rdx_gpu_pressure [--bar1-mib N] [--vram-mib N] [--hold-s S] [--chunk-mib 16] [--device NAME-substring]
 *
 * --bar1-mib: DEVICE_LOCAL|HOST_VISIBLE memory, mapped and touched (without resizable BAR
 *             this lives in the 256 MiB BAR1 aperture, like a game's upload heaps)
 * --vram-mib: plain DEVICE_LOCAL memory (not mapped)
 * Allocates in chunks until the amount or the first failure, holds, frees. Reports what
 * it got, so a run shows whether the driver had to make room.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define MAXC 4096

static int find_type(const VkPhysicalDeviceMemoryProperties *mp, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((mp->memoryTypes[i].propertyFlags & want) == want && !(mp->memoryTypes[i].propertyFlags & avoid)) return (int)i;
    return -1;
}

int main(int argc, char **argv)
{
    long bar1_mib = 0, vram_mib = 0, chunk_mib = 16;
    double hold_s = 10;
    const char *want_dev = "NVIDIA";
    static const struct option opts[] = {{"bar1-mib", 1, 0, 'b'}, {"vram-mib", 1, 0, 'v'}, {"hold-s", 1, 0, 'h'},
                                         {"chunk-mib", 1, 0, 'c'}, {"device", 1, 0, 'd'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'b': bar1_mib = atol(optarg); break;
        case 'v': vram_mib = atol(optarg); break;
        case 'h': hold_s = atof(optarg); break;
        case 'c': chunk_mib = atol(optarg); break;
        case 'd': want_dev = optarg; break;
        default: fprintf(stderr, "see the header of rdx_gpu_pressure.c\n"); return 2;
        }
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no Vulkan\n"); return 1; }
    uint32_t n = 8;
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < n && !pd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (strstr(props.deviceName, want_dev)) pd = pds[i];
    }
    if (!pd) { fprintf(stderr, "no device matching %s\n", want_dev); return 1; }
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int t_bar = find_type(&mp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    int t_vram = find_type(&mp, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        printf("[gpu] %s heap %u: %llu MiB%s\n", props.deviceName, h, (unsigned long long)(mp.memoryHeaps[h].size >> 20),
               (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 1; }
    static VkDeviceMemory mem[MAXC];
    int nm = 0;
    long got_bar = 0, got_vram = 0;
    VkDeviceSize chunk = (VkDeviceSize)chunk_mib << 20;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long done = 0; t_bar >= 0 && done < bar1_mib && nm < MAXC; done += chunk_mib) {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = chunk, .memoryTypeIndex = (uint32_t)t_bar};
        VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem[nm]);
        if (r != VK_SUCCESS) { printf("[gpu] BAR1-type allocation stopped at %ld MiB: VkResult %d\n", got_bar, r); break; }
        void *p;
        r = vkMapMemory(dev, mem[nm], 0, chunk, 0, &p);
        nm++;
        if (r != VK_SUCCESS) { printf("[gpu] map stopped at %ld MiB: VkResult %d\n", got_bar, r); break; }
        memset(p, 0x5a, (size_t)chunk);
        got_bar += chunk_mib;
    }
    for (long done = 0; t_vram >= 0 && done < vram_mib && nm < MAXC; done += chunk_mib) {
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = chunk, .memoryTypeIndex = (uint32_t)t_vram};
        VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem[nm]);
        if (r != VK_SUCCESS) { printf("[gpu] VRAM allocation stopped at %ld MiB: VkResult %d\n", got_vram, r); break; }
        nm++;
        got_vram += chunk_mib;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("[gpu] holding %ld MiB mapped DEVICE_LOCAL|HOST_VISIBLE + %ld MiB VRAM (%.0f ms to allocate) for %.0f s\n",
           got_bar, got_vram, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, hold_s);
    fflush(stdout);
    usleep((useconds_t)(hold_s * 1e6));
    for (int i = 0; i < nm; i++) vkFreeMemory(dev, mem[i], NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("[gpu] released\n");
    return 0;
}
