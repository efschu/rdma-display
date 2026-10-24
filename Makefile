CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
.DEFAULT_GOAL := all     # included sub-makefiles (p5/p5.mk) define rules before "all"

# DRM structures come from the kernel UAPI headers (linux-libc-dev); libdrm is not needed
BIN := p0b/rdx_src p0b/rdx_sink_win p0b/rdx_sink_kms p4/rdx_input_tx p4/rdx_input_rx p4/rdx_input_win
COMMON := common/rdx_common.c common/rdx_common.h
include common/ctl.mk     # control plane: civetweb, cJSON, rdx_proc, rdx_ctl
include host/host.mk
# P0-A gate and the P1 mirror source need libgbm headers (libgbm-dev)
# a game monitor by DRM lease from the X server (RandR 1.6, xcb-randr)
ifeq ($(shell pkg-config --exists xcb-randr && echo 1),1)
BIN += p0b/rdx_lease
endif
ifeq ($(shell pkg-config --exists gbm && echo 1),1)
BIN += p0a/rdx_gate_gbm p1/rdx_mirror_src
endif
# test app of the end-to-end latency measurement (tools/rdx_perf.py --latency): X11 + GL
ifeq ($(shell pkg-config --exists gl x11 && echo 1),1)
BIN += tools/rdx_flash
endif

include p5/p5.mk
include clip/clip.mk     # clipboard between the client's desktop and the app in the stream
include usb/usb.mk
include client/client.mk     # after common/ctl.mk (shared control-plane objects)
-include gs/fg.mk     # frame generation on a helper GPU (before gs.mk: FG_GS_OBJ)
-include gs/gs.mk     # the gamescope path: gamescope into CUDA VMM RDMA buffers (needs CUDA + Vulkan)

all: $(BIN)

p0b/rdx_src: p0b/rdx_src.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ p0b/rdx_src.c common/rdx_common.c -libverbs

# the host's cursor as the window's cursor needs libXcursor; without it the window sink does not offer the
# cursor channel (the source draws the cursor into the frames)
WIN_XCURSOR := $(if $(shell pkg-config --exists xcursor && echo 1),-DRDX_HAVE_XCURSOR -lXcursor)
p0b/rdx_sink_win: p0b/rdx_sink_win.c p0b/rdx_xwin.h p0b/rdx_winmenu.h p0b/rdx_xmon.h common/rdx_menu_ipc.h \
		common/rdx_kms.c common/rdx_kms.h common/rdx_cursor.c common/rdx_cursor.h $(COMMON)
	$(CC) $(CFLAGS) -o $@ p0b/rdx_sink_win.c common/rdx_common.c common/rdx_kms.c common/rdx_cursor.c \
		-libverbs -lEGL -lGLESv2 -lX11 -lXrandr $(WIN_XCURSOR)

p0b/rdx_lease: p0b/rdx_lease.c p0b/rdx_xwin.h
	$(CC) $(CFLAGS) -o $@ p0b/rdx_lease.c -lxcb -lxcb-randr

TESTS += tests/test_hotkey
CLEAN += tests/test_hotkey
tests/test_hotkey: tests/test_hotkey.c common/rdx_hotkey.h
	$(CC) $(CFLAGS) -o $@ tests/test_hotkey.c

p0b/rdx_sink_kms: p0b/rdx_sink_kms.c p0b/rdx_overlay.c p0b/rdx_overlay.h common/rdx_menu_ipc.h \
		common/rdx_kms.c common/rdx_kms.h common/rdx_cursor.c common/rdx_cursor.h $(COMMON)
	$(CC) $(CFLAGS) -o $@ p0b/rdx_sink_kms.c p0b/rdx_overlay.c common/rdx_kms.c common/rdx_cursor.c common/rdx_common.c \
		-libverbs

p0a/rdx_gate_gbm: p0a/rdx_gate_gbm.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ p0a/rdx_gate_gbm.c common/rdx_common.c -libverbs -lgbm

p1/rdx_mirror_src: p1/rdx_mirror_src.c common/rdx_kms.c common/rdx_kms.h $(COMMON)
	$(CC) $(CFLAGS) -o $@ p1/rdx_mirror_src.c common/rdx_kms.c common/rdx_common.c \
		-libverbs -lgbm -lEGL -lGLESv2

INPUT := common/rdx_input.c common/rdx_input.h

tools/rdx_flash: tools/rdx_flash.c
	$(CC) $(CFLAGS) -o $@ tools/rdx_flash.c -lGL -lX11

p4/rdx_input_tx: p4/rdx_input_tx.c common/rdx_hotkey.h $(INPUT) $(COMMON)
	$(CC) $(CFLAGS) -o $@ p4/rdx_input_tx.c common/rdx_input.c common/rdx_common.c -libverbs

p4/rdx_input_rx: p4/rdx_input_rx.c $(INPUT) $(COMMON)
	$(CC) $(CFLAGS) -o $@ p4/rdx_input_rx.c common/rdx_input.c common/rdx_common.c -libverbs

# the window client's input: the keyboard / mouse of rdx_sink_win's window (X11), nothing grabbed
p4/rdx_input_win: p4/rdx_input_win.c p0b/rdx_xwin.h common/rdx_menu_ipc.h common/rdx_hotkey.h gs/rdx_localinput.c gs/rdx_localinput.h $(INPUT) $(COMMON)
	$(CC) $(CFLAGS) -o $@ p4/rdx_input_win.c gs/rdx_localinput.c common/rdx_input.c common/rdx_common.c -libverbs -lX11

TESTS += tests/test_lock_due
TESTS += tests/test_mode_synth
TESTS += tests/test_vrr
CLEAN += tests/test_vrr
tests/test_vrr: tests/test_vrr.c common/rdx_kms.c common/rdx_kms.h
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_vrr.c common/rdx_kms.c
TESTS += tests/test_syskeys
CLEAN += tests/test_syskeys
tests/test_syskeys: tests/test_syskeys.c common/rdx_input.c common/rdx_input.h $(COMMON)
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_syskeys.c common/rdx_input.c common/rdx_common.c -libverbs
TESTS += tests/test_pace_slots
CLEAN += tests/test_pace_slots
tests/test_pace_slots: tests/test_pace_slots.c common/rdx_ctl.h
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_pace_slots.c
TESTS += tests/test_default_mtu
CLEAN += tests/test_default_mtu
tests/test_default_mtu: tests/test_default_mtu.c p0b/rdx_src.c p0b/rdx_sink_win.c p0b/rdx_sink_kms.c $(COMMON)
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_default_mtu.c common/rdx_common.c -libverbs
TESTS += tests/test_gid_pick
CLEAN += tests/test_gid_pick
tests/test_gid_pick: tests/test_gid_pick.c $(COMMON)
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_gid_pick.c common/rdx_common.c -libverbs
CLEAN += tests/test_mode_synth
tests/test_mode_synth: tests/test_mode_synth.c common/rdx_kms.c common/rdx_kms.h
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_mode_synth.c common/rdx_kms.c
CLEAN += tests/test_lock_due
tests/test_lock_due: tests/test_lock_due.c $(COMMON)
	$(CC) $(CFLAGS) -Icommon -o $@ tests/test_lock_due.c common/rdx_common.c -libverbs

test: $(TESTS)
	for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f p0b/rdx_src p0b/rdx_sink_win p0b/rdx_sink_kms p0a/rdx_gate_gbm p1/rdx_mirror_src \
		p4/rdx_input_tx p4/rdx_input_rx p4/rdx_input_win p5/rdx_audio_tx p5/rdx_audio_rx \
		usb/rdx_usb_client usb/rdx_usb_host usb/rdx_usb_probe client/rdx_clientd tools/rdx_flash $(CLEAN)
	rm -rf client/obj

.PHONY: all clean test
