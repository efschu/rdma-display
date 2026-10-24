# rdx-hostd (sender control daemon + web UI); included from the top-level Makefile
BIN += host/rdx_hostd
CLEAN += host/rdx_hostd

host/rdx_hostd: host/rdx_hostd.c host/rdx_apps.c host/rdx_apps.h host/rdx_mgpu.c host/rdx_mgpu.h \
		host/rdx_seatguard.c host/rdx_seatguard.h host/rdx_vrr.c host/rdx_vrr.h $(CTL_OBJ) $(CTL_HDR)
	$(CC) $(CFLAGS) -o $@ host/rdx_hostd.c host/rdx_apps.c host/rdx_mgpu.c host/rdx_seatguard.c host/rdx_vrr.c \
		$(CTL_OBJ) $(CTL_LD)

TESTS += tests/test_desktop_user
CLEAN += tests/test_desktop_user
tests/test_desktop_user: tests/test_desktop_user.c host/rdx_apps.c host/rdx_apps.h third_party/cjson/cJSON.o
	$(CC) $(CFLAGS) -o $@ tests/test_desktop_user.c host/rdx_apps.c third_party/cjson/cJSON.o

TESTS += tests/test_seatguard
CLEAN += tests/test_seatguard
tests/test_seatguard: tests/test_seatguard.c host/rdx_seatguard.c host/rdx_seatguard.h
	$(CC) $(CFLAGS) -o $@ tests/test_seatguard.c host/rdx_seatguard.c

# variable refresh of a session (the client's "display" -> rdx_gs_src --vrr), make test runs it
TESTS += tests/test_hostd_vrr
CLEAN += tests/test_hostd_vrr
tests/test_hostd_vrr: tests/test_hostd_vrr.c host/rdx_vrr.c host/rdx_vrr.h third_party/cjson/cJSON.o
	$(CC) $(CFLAGS) -o $@ tests/test_hostd_vrr.c host/rdx_vrr.c third_party/cjson/cJSON.o
