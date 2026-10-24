# The gamescope path: gamescope renders into CUDA VMM buffers that the NIC reads; included from the top-level Makefile.
# Builds only where cuda.h (CUDA toolkit) and Vulkan are present. OGKM = include root of
# NVIDIA/open-gpu-kernel-modules enables the RM dma-buf export fallback for GeForce cards.
CUDA_INC ?= /usr/local/cuda/include
OGKM ?=
ifneq ($(wildcard $(CUDA_INC)/cuda.h),)
ifeq ($(shell pkg-config --exists vulkan && echo 1),1)
GS_CF := -I$(CUDA_INC) $(shell pkg-config --cflags vulkan)
ifneq ($(OGKM),)
GS_CF += -DRDX_WITH_OGKM -I$(OGKM)/src/common/sdk/nvidia/inc -I$(OGKM)/src/nvidia/arch/nvalloc/unix/include \
         -I$(OGKM)/kernel-open/common/inc
endif
# libcuda is dlopen()ed on first use (common/rdx_cuda_dl.c): runs without the NVIDIA driver too
GS_LD := $(shell pkg-config --libs vulkan) -libverbs -ldl
BIN += p0a/rdx_gate_vmm

BIN += gs/rdx_gs_src

p0a/rdx_gate_vmm: p0a/rdx_gate_vmm.c common/rdx_vmm.c common/rdx_vmm.h $(COMMON)
	$(CC) $(CFLAGS) $(GS_CF) -o $@ p0a/rdx_gate_vmm.c common/rdx_vmm.c common/rdx_cuda_dl.c common/rdx_common.c $(GS_LD)

GS_SRC := gs/rdx_srcsess.c gs/rdx_gsbuf.c gs/rdx_localinput.c common/rdx_input.c common/rdx_vmm.c common/rdx_cuda_dl.c common/rdx_common.c \
          common/rdx_cursor.c
gs/rdx_gs_src: gs/rdx_gs_src.c gs/rdx_gs_proto.h gs/rdx_gs_vrr.h gs/rdx_srcsess.h gs/rdx_gsbuf.h gs/rdx_hash.h $(GS_SRC) common/rdx_vmm.h \
		common/rdx_cursor.h $(COMMON) $(FG_GS_OBJ)
	$(CC) $(CFLAGS) $(GS_CF) $(FG_GS_CF) -o $@ gs/rdx_gs_src.c $(GS_SRC) $(FG_GS_OBJ) $(GS_LD) -lm -lpthread
endif
endif

# display-less sink for tests (verbs only, builds everywhere)
BIN += gs/rdx_sink_null
gs/rdx_sink_null: gs/rdx_sink_null.c gs/rdx_hash.h common/rdx_cursor.c common/rdx_cursor.h $(COMMON)
	$(CC) $(CFLAGS) -o $@ gs/rdx_sink_null.c common/rdx_cursor.c common/rdx_common.c -libverbs

# the pointer hint (RDX_GS_HINT -> frame header flags) without GPU, NIC or gamescope; make test runs it
TESTS += tests/test_gs_hint
CLEAN += tests/test_gs_hint
tests/test_gs_hint: tests/test_gs_hint.c gs/rdx_srcsess.c gs/rdx_srcsess.h gs/rdx_gs_proto.h common/rdx_common.h common/rdx_cursor.c \
		common/rdx_cursor.h $(COMMON)
	$(CC) $(CFLAGS) -o $@ tests/test_gs_hint.c gs/rdx_srcsess.c common/rdx_cursor.c common/rdx_common.c -libverbs

# stand-in for rdx_gs_src without a sink or NIC: tests gamescope's rdx backend and its stream
# add-on next to the DRM backend (docs/SEAT-MONITOR.md)
BIN += gs/rdx_gs_probe
gs/rdx_gs_probe: gs/rdx_gs_probe.c gs/rdx_gs_proto.h
	$(CC) $(CFLAGS) -o $@ gs/rdx_gs_probe.c -lm

# GPU memory pressure test load (Vulkan only)
ifeq ($(shell pkg-config --exists vulkan && echo 1),1)
BIN += gs/rdx_gpu_pressure
gs/rdx_gpu_pressure: gs/rdx_gpu_pressure.c
	$(CC) $(CFLAGS) -o $@ gs/rdx_gpu_pressure.c $(shell pkg-config --libs vulkan)
endif

# variable refresh on the source side (gs/rdx_gs_vrr.h), header only; make test runs it
TESTS += tests/test_gs_vrr
CLEAN += tests/test_gs_vrr
tests/test_gs_vrr: tests/test_gs_vrr.c gs/rdx_gs_vrr.h
	$(CC) $(CFLAGS) -o $@ tests/test_gs_vrr.c

# the cursor next to the picture (common/rdx_cursor.h, the gamescope -> rdx_gs_src packets) without GPU,
# NIC or gamescope; make test runs it
TESTS += tests/test_cursor
CLEAN += tests/test_cursor
tests/test_cursor: tests/test_cursor.c gs/rdx_srcsess.c gs/rdx_srcsess.h gs/rdx_gs_proto.h common/rdx_cursor.c \
		common/rdx_cursor.h $(COMMON)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_cursor.c gs/rdx_srcsess.c common/rdx_cursor.c \
		common/rdx_common.c -libverbs
