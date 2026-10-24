# Frame generation on a helper GPU (docs/MULTI-GPU.md): afmf-linux's frame generation core
# (third_party/afmf-linux, MIT) driven from host-memory frames. Included from the top-level
# Makefile after gs/gs.mk. Needs Vulkan and glslangValidator (Ubuntu: glslang-tools).
#   gs/rdx_fg_bench   standalone test (no NIC, no gamescope)
#   FG_GS_OBJ         objects rdx_gs_src links for --fg helper (gs/gs.mk adds them; include this
#                     file before gs/gs.mk so the variable exists when its rule is read)
CUDA_INC ?= /usr/local/cuda/include
ifeq ($(shell pkg-config --exists vulkan && echo 1),1)
GLSLANG ?= $(shell command -v glslangValidator 2>/dev/null)
ifneq ($(GLSLANG),)
AFMF := third_party/afmf-linux
FGB := gs/.fg
AFMF_FFX := $(AFMF)/shaders/fidelityfx
AFMF_FFX_DEF := -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0
AFMF_GLSL = $(GLSLANG) -V --target-env vulkan1.1 -S comp -I$(AFMF_FFX) $(2) -o $@ $(1) >/dev/null
AFMF_PASSES := ffx_opticalflow_prepare_luma_pass ffx_opticalflow_compute_luminance_pyramid_pass \
               ffx_opticalflow_generate_scd_histogram_pass ffx_opticalflow_compute_scd_divergence_pass \
               ffx_opticalflow_compute_optical_flow_advanced_pass_v5 ffx_opticalflow_filter_optical_flow_pass_v5 \
               ffx_opticalflow_scale_optical_flow_advanced_pass_v5
AFMF_SPV := $(AFMF_PASSES:%=$(FGB)/%.spv) $(FGB)/ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16.spv \
            $(FGB)/afmf_interpolate_rgba8.spv $(FGB)/afmf_interpolate_rgba8_bgra.spv \
            $(FGB)/afmf_interpolate_rgb10a2.spv $(FGB)/afmf_interpolate_rgba16f.spv $(FGB)/afmf_interpolate_noformat.spv \
            $(FGB)/afmf_ingest_rgba8.spv $(FGB)/afmf_ingest_rgb10a2.spv $(FGB)/afmf_ingest_rgba16f.spv \
            $(FGB)/afmf_ingest_noformat.spv $(FGB)/afmf_downsample_rgba8.spv $(FGB)/afmf_downsample_rgba16f.spv
AFMF_HDRS := $(wildcard $(AFMF_FFX)/*.h $(AFMF_FFX)/*/*.h)

$(FGB)/%.spv: $(AFMF_FFX)/passes/%.glsl $(AFMF_HDRS)
	@mkdir -p $(FGB)
	$(call AFMF_GLSL,$<,$(AFMF_FFX_DEF))
$(FGB)/ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16.spv: $(AFMF_FFX)/passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl $(AFMF_HDRS)
	@mkdir -p $(FGB)
	$(call AFMF_GLSL,$<,$(AFMF_FFX_DEF) -DAFMF_SAD_INT16=1)
$(FGB)/afmf_interpolate_rgba8.spv: $(AFMF)/shaders/afmf_interpolate.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=0)
$(FGB)/afmf_interpolate_rgba8_bgra.spv: $(AFMF)/shaders/afmf_interpolate.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=1)
$(FGB)/afmf_interpolate_rgb10a2.spv: $(AFMF)/shaders/afmf_interpolate.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgb10_a2 -DAFMF_SWAP_RB=0)
$(FGB)/afmf_interpolate_rgba16f.spv: $(AFMF)/shaders/afmf_interpolate.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba16f -DAFMF_SWAP_RB=0)
$(FGB)/afmf_interpolate_noformat.spv: $(AFMF)/shaders/afmf_interpolate.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT_NONE=1 -DAFMF_SWAP_RB=0)
$(FGB)/afmf_ingest_rgba8.spv: $(AFMF)/shaders/afmf_ingest.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba8)
$(FGB)/afmf_ingest_rgb10a2.spv: $(AFMF)/shaders/afmf_ingest.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgb10_a2)
$(FGB)/afmf_ingest_rgba16f.spv: $(AFMF)/shaders/afmf_ingest.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba16f)
$(FGB)/afmf_ingest_noformat.spv: $(AFMF)/shaders/afmf_ingest.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT_NONE=1)
$(FGB)/afmf_downsample_rgba8.spv: $(AFMF)/shaders/afmf_downsample.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba8)
$(FGB)/afmf_downsample_rgba16f.spv: $(AFMF)/shaders/afmf_downsample.comp $(AFMF_HDRS)
	@mkdir -p $(FGB); $(call AFMF_GLSL,$<,-DAFMF_OUT_FORMAT=rgba16f)

$(FGB)/afmf_spirv.h: $(AFMF_SPV) tools/rdx-mgpu-embed-spirv.py
	python3 tools/rdx-mgpu-embed-spirv.py $@ $(AFMF_SPV)

AFMF_C := config log framegen framegen_descriptors framegen_dump framegen_format framegen_passes \
          framegen_pipelines framegen_profiler framegen_record framegen_record_frame framegen_resources framegen_host
AFMF_OBJ := $(AFMF_C:%=$(FGB)/afmf_%.o)
AFMF_CF := -O2 -g -std=gnu11 -D_POSIX_C_SOURCE=200809L -I$(AFMF)/src -I$(FGB) $(shell pkg-config --cflags vulkan)
$(FGB)/afmf_%.o: $(AFMF)/src/%.c $(wildcard $(AFMF)/src/*.h) $(FGB)/afmf_spirv.h
	$(CC) $(AFMF_CF) -c -o $@ $<

FG_LD := $(shell pkg-config --libs vulkan) -lpthread -lm
$(FGB)/rdx_fg.o: gs/rdx_fg.c gs/rdx_fg.h $(FGB)/afmf_spirv.h
	$(CC) $(CFLAGS) -I$(FGB) $(shell pkg-config --cflags vulkan) -c -o $@ gs/rdx_fg.c

BIN += gs/rdx_fg_bench
gs/rdx_fg_bench: gs/rdx_fg_bench.c $(FGB)/rdx_fg.o $(AFMF_OBJ)
	$(CC) $(CFLAGS) -o $@ gs/rdx_fg_bench.c $(FGB)/rdx_fg.o $(AFMF_OBJ) $(FG_LD)

# for rdx_gs_src (CUDA builds): the stage + rdx_fg with the CUDA staging path
ifneq ($(wildcard $(CUDA_INC)/cuda.h),)
$(FGB)/rdx_fg_cuda.o: gs/rdx_fg.c gs/rdx_fg.h $(FGB)/afmf_spirv.h
	$(CC) $(CFLAGS) -DRDX_FG_CUDA -I$(CUDA_INC) -I$(FGB) $(shell pkg-config --cflags vulkan) -c -o $@ gs/rdx_fg.c
$(FGB)/rdx_fg_stage.o: gs/rdx_fg_stage.c gs/rdx_fg_stage.h gs/rdx_fg.h gs/rdx_gsbuf.h gs/rdx_srcsess.h
	@mkdir -p $(FGB)
	$(CC) $(CFLAGS) -DRDX_FG_CUDA -I$(CUDA_INC) $(shell pkg-config --cflags vulkan) -c -o $@ gs/rdx_fg_stage.c
FG_GS_OBJ := $(FGB)/rdx_fg_stage.o $(FGB)/rdx_fg_cuda.o $(AFMF_OBJ)
FG_GS_CF := -DRDX_WITH_FG
endif
CLEAN += $(FGB) gs/rdx_fg_bench
endif
endif
