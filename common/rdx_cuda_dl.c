/* SPDX-License-Identifier: MIT
 * libcuda loaded on first use (dlopen) instead of linked: the CUDA driver API functions that
 * rdma-display calls, each forwarding to libcuda.so.1. A host without the NVIDIA driver then
 * still runs rdx_gs_src with an AMD or Intel GPU (host buffers, no CUDA call is ever made);
 * a call without the driver returns CUDA_ERROR_NOT_INITIALIZED.
 * Link this file instead of -lcuda (plus -ldl). cuda.h maps some names to versioned symbols
 * (cuMemcpyDtoH -> cuMemcpyDtoH_v2, ...): the definitions below take the same macros, and the
 * looked-up symbol name is the expanded one, so each forwards to the symbol cuda.h would link. */
#define _GNU_SOURCE
#include <cuda.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

static void *g_lib;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void load(void)
{
    g_lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) fprintf(stderr, "[cuda] libcuda.so.1 not loadable (%s): no NVIDIA driver, CUDA unavailable\n", dlerror());
}

static void *sym(const char *name)
{
    pthread_once(&g_once, load);
    return g_lib ? dlsym(g_lib, name) : NULL;
}

#define RDX_STR(x) #x
#define RDX_XSTR(x) RDX_STR(x)
#define FWD(name, params, args)                                                     \
    CUresult CUDAAPI name params                                                   \
    {                                                                              \
        static CUresult (CUDAAPI *f) params;                                       \
        if (!f) f = (CUresult (CUDAAPI *) params)sym(RDX_XSTR(name));              \
        return f ? f args : CUDA_ERROR_NOT_INITIALIZED;                            \
    }

FWD(cuInit, (unsigned int Flags), (Flags))
FWD(cuGetErrorName, (CUresult error, const char **pStr), (error, pStr))
FWD(cuDeviceGet, (CUdevice *device, int ordinal), (device, ordinal))
FWD(cuDeviceGetAttribute, (int *pi, CUdevice_attribute attrib, CUdevice dev), (pi, attrib, dev))
FWD(cuDeviceGetByPCIBusId, (CUdevice *dev, const char *pciBusId), (dev, pciBusId))
FWD(cuDeviceGetName, (char *name, int len, CUdevice dev), (name, len, dev))
FWD(cuDeviceGetPCIBusId, (char *pciBusId, int len, CUdevice dev), (pciBusId, len, dev))
FWD(cuDeviceGetUuid, (CUuuid *uuid, CUdevice dev), (uuid, dev))
FWD(cuDevicePrimaryCtxRetain, (CUcontext *pctx, CUdevice dev), (pctx, dev))
FWD(cuDevicePrimaryCtxRelease, (CUdevice dev), (dev))
FWD(cuCtxSetCurrent, (CUcontext ctx), (ctx))
FWD(cuCtxPushCurrent, (CUcontext ctx), (ctx))
FWD(cuCtxPopCurrent, (CUcontext *pctx), (pctx))
FWD(cuMemAddressReserve, (CUdeviceptr *ptr, size_t size, size_t alignment, CUdeviceptr addr, unsigned long long flags),
    (ptr, size, alignment, addr, flags))
FWD(cuMemAddressFree, (CUdeviceptr ptr, size_t size), (ptr, size))
FWD(cuMemCreate, (CUmemGenericAllocationHandle *handle, size_t size, const CUmemAllocationProp *prop, unsigned long long flags),
    (handle, size, prop, flags))
FWD(cuMemRelease, (CUmemGenericAllocationHandle handle), (handle))
FWD(cuMemMap, (CUdeviceptr ptr, size_t size, size_t offset, CUmemGenericAllocationHandle handle, unsigned long long flags),
    (ptr, size, offset, handle, flags))
FWD(cuMemUnmap, (CUdeviceptr ptr, size_t size), (ptr, size))
FWD(cuMemSetAccess, (CUdeviceptr ptr, size_t size, const CUmemAccessDesc *desc, size_t count), (ptr, size, desc, count))
FWD(cuMemGetAllocationGranularity, (size_t *granularity, const CUmemAllocationProp *prop, CUmemAllocationGranularity_flags option),
    (granularity, prop, option))
FWD(cuMemExportToShareableHandle, (void *shareableHandle, CUmemGenericAllocationHandle handle, CUmemAllocationHandleType handleType,
                                   unsigned long long flags),
    (shareableHandle, handle, handleType, flags))
FWD(cuMemGetHandleForAddressRange, (void *handle, CUdeviceptr dptr, size_t size, CUmemRangeHandleType handleType,
                                    unsigned long long flags),
    (handle, dptr, size, handleType, flags))
FWD(cuMemHostRegister, (void *p, size_t bytesize, unsigned int Flags), (p, bytesize, Flags))
FWD(cuMemHostUnregister, (void *p), (p))
FWD(cuMemcpyDtoH, (void *dstHost, CUdeviceptr srcDevice, size_t ByteCount), (dstHost, srcDevice, ByteCount))
