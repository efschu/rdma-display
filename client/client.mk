# rdx-clientd (receiver control daemon + web UI); included from the top-level Makefile
# AFTER common/ctl.mk, whose shared control-plane objects it links:
#   $(CTL_OBJ) = civetweb, cJSON, rdx_proc, rdx_ctl;  $(CTL_LD) = -lpthread -ldl
# Without ctl.mk (a tree that has the sources but not the shared rules) it builds
# them itself under client/obj.
ifdef CTL_OBJ
CLIENTD_OBJ := $(CTL_OBJ)
CLIENTD_HDR := $(CTL_HDR)
CLIENTD_LD  := $(CTL_LD)
else ifeq ($(words $(wildcard common/rdx_proc.c common/rdx_ctl.c third_party/cjson/cJSON.c third_party/civetweb/civetweb.c)),4)
CLIENTD_OBJ := client/obj/civetweb.o client/obj/cJSON.o client/obj/rdx_proc.o client/obj/rdx_ctl.o
CLIENTD_HDR := common/rdx_proc.h common/rdx_ctl.h third_party/cjson/cJSON.h third_party/civetweb/civetweb.h
CLIENTD_LD  := -lpthread -ldl

client/obj/civetweb.o: third_party/civetweb/civetweb.c third_party/civetweb/civetweb.h
	@mkdir -p client/obj
	$(CC) -O2 -g -std=gnu11 -DNO_SSL -DNO_CGI -DUSE_WEBSOCKET -DNO_CACHING -c -o $@ $<

client/obj/cJSON.o: third_party/cjson/cJSON.c third_party/cjson/cJSON.h
	@mkdir -p client/obj
	$(CC) -O2 -g -std=gnu11 -c -o $@ $<

client/obj/%.o: common/%.c common/%.h
	@mkdir -p client/obj
	$(CC) $(CFLAGS) -c -o $@ $<
endif

ifdef CLIENTD_OBJ
BIN += client/rdx_clientd
CLEAN += client/rdx_clientd

# in-stream menu: model + socket (rdx_menu.c), CPU renderer with stb_truetype (rdx_menu_draw.c),
# the start placeholder on the same overlay (rdx_placeholder.c, icons decoded with stb_image)
MENU_SRC := client/rdx_menu.c client/rdx_menu_draw.c client/rdx_placeholder.c
MENU_HDR := client/rdx_menu.h client/rdx_menu_draw.h client/rdx_placeholder.h common/rdx_menu_ipc.h \
            third_party/stb/stb_truetype.h third_party/stb/stb_image.h

client/rdx_clientd: client/rdx_clientd.c $(MENU_SRC) $(MENU_HDR) $(CLIENTD_OBJ) $(CLIENTD_HDR)
	$(CC) $(CFLAGS) -o $@ client/rdx_clientd.c $(MENU_SRC) $(CLIENTD_OBJ) $(CLIENTD_LD) -lm

# review aid: renders the menu panel into a PNG (no display needed)
TESTS += tests/test_menu_render
CLEAN += tests/test_menu_render
tests/test_menu_render: tests/test_menu_render.c client/rdx_menu_draw.c client/rdx_menu_draw.h
	$(CC) $(CFLAGS) -o $@ tests/test_menu_render.c client/rdx_menu_draw.c -lm

# review aid: renders the start placeholder in four states into a PNG
TESTS += tests/test_placeholder_render
CLEAN += tests/test_placeholder_render
tests/test_placeholder_render: tests/test_placeholder_render.c client/rdx_menu_draw.c client/rdx_placeholder.c \
                               client/rdx_menu_draw.h client/rdx_placeholder.h third_party/stb/stb_image.h
	$(CC) $(CFLAGS) -o $@ tests/test_placeholder_render.c client/rdx_menu_draw.c client/rdx_placeholder.c -lm

# the window client's menu: real rdx_menu.c, real p0b/rdx_winmenu.h as the sink, a fake window input
TESTS += tests/test_window_menu
CLEAN += tests/test_window_menu
tests/test_window_menu: tests/test_window_menu.c $(MENU_SRC) $(MENU_HDR) p0b/rdx_winmenu.h
	$(CC) $(CFLAGS) -o $@ tests/test_window_menu.c $(MENU_SRC) -lm

# the placeholder's state machines: real rdx_menu.c with a fake sink, real rdx_overlay.c on a socketpair
TESTS += tests/test_placeholder_ipc
CLEAN += tests/test_placeholder_ipc
tests/test_placeholder_ipc: tests/test_placeholder_ipc.c $(MENU_SRC) $(MENU_HDR) p0b/rdx_overlay.c p0b/rdx_overlay.h common/rdx_kms.c
	$(CC) $(CFLAGS) -o $@ tests/test_placeholder_ipc.c $(MENU_SRC) p0b/rdx_overlay.c common/rdx_kms.c -lm
endif
