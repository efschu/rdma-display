# USB passthrough (kernel UAPI + libibverbs only); included from the top-level Makefile
USB_SRC := usb/rdx_usb_chan.c usb/rdx_usb_sys.c common/rdx_common.c
USB_DEP := usb/rdx_usb.h $(USB_SRC) $(COMMON)
# sysfs/dev paths are short by construction; gcc cannot see that
USB_CF := -Wno-format-truncation
BIN += usb/rdx_usb_client usb/rdx_usb_host usb/rdx_usb_probe

usb/rdx_usb_client: usb/rdx_usb_client.c $(USB_DEP)
	$(CC) $(CFLAGS) $(USB_CF) -o $@ usb/rdx_usb_client.c $(USB_SRC) -libverbs

usb/rdx_usb_host: usb/rdx_usb_host.c $(USB_DEP) common/rdx_input.c common/rdx_input.h
	$(CC) $(CFLAGS) $(USB_CF) -o $@ usb/rdx_usb_host.c $(USB_SRC) common/rdx_input.c -libverbs

usb/rdx_usb_probe: usb/rdx_usb_probe.c
	$(CC) $(CFLAGS) $(USB_CF) -o $@ usb/rdx_usb_probe.c
