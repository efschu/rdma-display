# Control plane shared by rdx-hostd and rdx-clientd: vendored civetweb (MIT)
# and cJSON (MIT), process supervision (rdx_proc), the JSON-lines control channel (rdx_ctl) and
# the link health of the RDMA port (rdx_link). Included from the top-level Makefile; link with
#   $(CTL_OBJ) $(CTL_LD)
CW_CFLAGS := -O2 -g -std=gnu11 -DNO_SSL -DNO_CGI -DUSE_WEBSOCKET -DNO_CACHING -Ithird_party/civetweb
CTL_OBJ   := third_party/civetweb/civetweb.o third_party/cjson/cJSON.o common/rdx_proc.o common/rdx_ctl.o common/rdx_link.o
CTL_HDR   := common/rdx_proc.h common/rdx_ctl.h common/rdx_link.h third_party/cjson/cJSON.h third_party/civetweb/civetweb.h
CTL_LD    := -lpthread -ldl -lm

third_party/civetweb/civetweb.o: third_party/civetweb/civetweb.c third_party/civetweb/civetweb.h
	$(CC) $(CW_CFLAGS) -c -o $@ $<

third_party/cjson/cJSON.o: third_party/cjson/cJSON.c third_party/cjson/cJSON.h
	$(CC) -O2 -g -std=gnu11 -c -o $@ $<

common/rdx_proc.o: common/rdx_proc.c common/rdx_proc.h
	$(CC) $(CFLAGS) -c -o $@ $<

common/rdx_ctl.o: common/rdx_ctl.c common/rdx_ctl.h third_party/cjson/cJSON.h
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_proc_ctl: tests/test_proc_ctl.c $(CTL_OBJ) $(CTL_HDR)
	$(CC) $(CFLAGS) -o $@ tests/test_proc_ctl.c $(CTL_OBJ) $(CTL_LD)

TESTS += tests/test_proc_ctl
CLEAN += $(CTL_OBJ) tests/test_proc_ctl

common/rdx_link.o: common/rdx_link.c common/rdx_link.h third_party/cjson/cJSON.h
	$(CC) $(CFLAGS) -c -o $@ $<

# link health on a fake sysfs tree; `tests/test_link --live <link IP>` reads a real port (read-only)
tests/test_link: tests/test_link.c common/rdx_link.o third_party/cjson/cJSON.o
	$(CC) $(CFLAGS) -o $@ tests/test_link.c common/rdx_link.o third_party/cjson/cJSON.o -lpthread -lm

TESTS += tests/test_link
CLEAN += tests/test_link
