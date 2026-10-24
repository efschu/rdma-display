/* SPDX-License-Identifier: MIT
 * Test helper: LD_PRELOAD shim that raises a Vulkan 1.0 application's apiVersion to 1.2, so
 * frame-generation layers that need 1.1 subgroups (afmf-linux, lsfg-vk) can be exercised with
 * vkcube (vulkan-tools asks for 1.0; games under DXVK / vkd3d-proton ask for 1.3 anyway).
 * vkcube dlopen()s libvulkan and looks vkGetInstanceProcAddr up with dlsym(), so the shim wraps
 * dlsym: the application gets our vkGetInstanceProcAddr, which hands out our vkCreateInstance.
 * Build: cc -shared -fPIC -O2 -o rdx-mgpu-api12.so tools/rdx-mgpu-api12.c -ldl
 * Use:   LD_PRELOAD=/path/rdx-mgpu-api12.so vkcube */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkCreateInstance real_create;

static VKAPI_ATTR VkResult VKAPI_CALL api12_create(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                                                   VkInstance *out)
{
    VkInstanceCreateInfo c = *ci;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (ci->pApplicationInfo) app = *ci->pApplicationInfo;
    if (app.apiVersion < VK_API_VERSION_1_2) app.apiVersion = VK_API_VERSION_1_2;
    c.pApplicationInfo = &app;
    return real_create(&c, a, out);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL api12_gipa(VkInstance inst, const char *name)
{
    if (!inst && name && !strcmp(name, "vkCreateInstance")) {
        if (!real_create) real_create = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
        return real_create ? (PFN_vkVoidFunction)api12_create : NULL;
    }
    if (name && !strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)api12_gipa;
    return real_gipa(inst, name);
}

void *dlsym(void *handle, const char *name)
{
    static void *(*next)(void *, const char *);
    if (!next) next = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!next) next = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    void *p = next(handle, name);
    if (p && !strcmp(name, "vkGetInstanceProcAddr")) {
        real_gipa = (PFN_vkGetInstanceProcAddr)p;
        return (void *)api12_gipa;
    }
    if (p && !strcmp(name, "vkCreateInstance")) {
        real_create = (PFN_vkCreateInstance)p;
        return (void *)api12_create;
    }
    return p;
}
