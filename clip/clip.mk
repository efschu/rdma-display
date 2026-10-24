# Clipboard between the client's desktop and the app in the stream (clip/rdx_clip.c, X11 + XFixes);
# included from the top-level Makefile. rdx_clip links xclip's xclib.c (third_party/xclip, GPL-2.0+).
CLIP_PROTO := clip/rdx_clip_proto.c clip/rdx_clip_proto.h

ifeq ($(shell pkg-config --exists x11 xfixes && echo 1),1)
BIN += clip/rdx_clip
CLEAN += clip/rdx_clip third_party/xclip/xclib.o

third_party/xclip/xclib.o: third_party/xclip/xclib.c third_party/xclip/xclib.h
	$(CC) -O2 -g -std=gnu11 -w -c -o $@ $<

clip/rdx_clip: clip/rdx_clip.c $(CLIP_PROTO) p0b/rdx_xwin.h third_party/xclip/xclib.o third_party/xclip/xclib.h
	$(CC) $(CFLAGS) -o $@ clip/rdx_clip.c clip/rdx_clip_proto.c third_party/xclip/xclib.o \
		$(shell pkg-config --libs x11 xfixes)

# end to end on two Xvfb displays (skips without Xvfb / xclip)
TESTS += tests/test_clip_e2e.sh
tests/test_clip_e2e.sh: clip/rdx_clip
# --x-of-pid with the X server double-forked away from gamescope (as wlroots does)
TESTS += tests/test_clip_find.sh
tests/test_clip_find.sh: clip/rdx_clip
endif

# wire format, UTF-8, limit, echo guard (no X needed)
TESTS += tests/test_clip_proto
CLEAN += tests/test_clip_proto
tests/test_clip_proto: tests/test_clip_proto.c $(CLIP_PROTO)
	$(CC) $(CFLAGS) -o $@ tests/test_clip_proto.c clip/rdx_clip_proto.c
