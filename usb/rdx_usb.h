/* SPDX-License-Identifier: MIT
 *
 * USB passthrough client -> host (rdx_usb_client / rdx_usb_host).
 *
 * Two paths, chosen per device:
 *   hid    report level: client hidraw (descriptor + reports) -> RDMA SEND ->
 *          host /dev/uhid with the very same descriptor. Output reports
 *          (rumble, LEDs) and GET/SET_REPORT go back the same way. The local
 *          evdev nodes of the device are grabbed on the client for the session.
 *   usbip  URB level: unmodified usbip kernel modules (usbip-host on the
 *          client, vhci-hcd on the host) riding on an AF_SMC socket (SMC-R =
 *          sockets over RoCE). We hand the connected sockets to the kernel via
 *          sysfs ourselves, no usbip userspace is needed.
 *
 * Bootstrap and teardown run over one TCP connection (port P); the usbip
 * sockets use AF_SMC on port P+1. The TCP connection stays open for the
 * session: its end (either side) ends the session and both sides clean up.
 */
#ifndef RDX_USB_H
#define RDX_USB_H

#include "../common/rdx_common.h"

#include <linux/hid.h>
#include <linux/hidraw.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RDX_USB_MAGIC      0x31425355u   /* "USB1" */
#define RDX_USB_PORT       18680
#define RDX_USB_MAX_DEV    16
#define RDX_USB_MAX_REPORT 4096          /* UHID_DATA_MAX */

enum { RDX_USB_KIND_HID = 1, RDX_USB_KIND_USBIP = 2 };

/* one device in the client's hello */
struct rdx_usb_dev {
    uint32_t kind;
    char     id[64];                     /* busid (usb) or hid-<uniq|phys> (non-USB HID) */
    char     name[128];
    /* hid */
    uint32_t bus, vendor, product, version, country;
    char     phys[64], uniq[64];
    uint32_t rd_size;
    uint8_t  rd[HID_MAX_DESCRIPTOR_SIZE];
    /* usbip */
    uint32_t devid;                      /* busnum << 16 | devnum, checked by usbip-host */
    uint32_t speed;                      /* enum usb_device_speed */
};

struct rdx_usb_hello {                   /* client -> host, followed by ndev x rdx_usb_dev */
    uint32_t magic, ndev;
    struct rdx_conn conn;
};

struct rdx_usb_ready {                   /* host -> client */
    uint32_t magic;
    struct rdx_conn conn;
    int32_t  status[RDX_USB_MAX_DEV];    /* hid: 0 or -errno of UHID_CREATE2 */
};

/* control messages on the TCP connection after the bootstrap */
enum { RDX_USB_CTL_BYE = 1, RDX_USB_CTL_BYE_ACK, RDX_USB_CTL_USBIP_UP };
struct rdx_usb_ctl {
    uint32_t type, dev;
    int32_t  status;                     /* USBIP_UP: host vhci port or -errno */
};

/* first bytes on each AF_SMC connection (usbip path) */
struct rdx_usb_smc_hello {
    uint32_t magic, dev;
};

/* RDMA messages (hid path) */
enum {
    RDX_USB_M_IN = 1,        /* c->h input report */
    RDX_USB_M_OUT,           /* h->c output report */
    RDX_USB_M_GETREQ,        /* h->c GET_REPORT */
    RDX_USB_M_GETREP,        /* c->h reply */
    RDX_USB_M_SETREQ,        /* h->c SET_REPORT */
    RDX_USB_M_SETREP,        /* c->h reply */
    RDX_USB_M_GONE,          /* c->h device unplugged */
};
struct rdx_usb_msg {
    uint16_t type, len;      /* payload length */
    uint16_t dev, err;
    uint32_t id;             /* GET/SET request id */
    uint8_t  rnum, rtype;
    uint8_t  pad[2];
};
#define RDX_USB_MSG_MAX (sizeof(struct rdx_usb_msg) + RDX_USB_MAX_REPORT)

/* ---------------- RDMA message channel (rdx_usb_chan.c) ---------------- */
#define RDX_USB_NRECV 128
#define RDX_USB_NSEND 64
struct rdx_usb_chan {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_comp_channel *ch;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *rmr, *smr;
    uint8_t *rbuf, *sbuf;
    uint32_t max_inline;
    uint32_t shead, sinflight;
    int gidx;
    struct rdx_conn mine;
};
/* payload follows the header in the same buffer */
typedef void (*rdx_usb_msg_cb)(const struct rdx_usb_msg *m, const uint8_t *data, void *user);

/* local_ip: the seat's link IP (device "auto" and the source GID), NULL = unknown */
int  rdx_usb_chan_init(struct rdx_usb_chan *c, const char *dev, const char *local_ip, int comp_vector);
int  rdx_usb_chan_connect(struct rdx_usb_chan *c, const struct rdx_conn *peer, int mtu);
int  rdx_usb_chan_fd(const struct rdx_usb_chan *c);
/* send one message; blocks on the completion channel only while all send slots are in flight
   (messages received meanwhile are dispatched to cb) */
int  rdx_usb_chan_send(struct rdx_usb_chan *c, const struct rdx_usb_msg *h, const void *payload,
                       rdx_usb_msg_cb cb, void *user);
/* drain completions, dispatch received messages, re-arm; call when the channel fd is readable */
int  rdx_usb_chan_dispatch(struct rdx_usb_chan *c, rdx_usb_msg_cb cb, void *user);
void rdx_usb_chan_destroy(struct rdx_usb_chan *c);

/* ---------------- system helpers (rdx_usb_sys.c) ---------------- */
int  rdx_sysfs_read(const char *path, char *buf, size_t n);          /* trims the newline */
long rdx_sysfs_read_long(const char *path, int base, long dflt);
int  rdx_sysfs_write(const char *path, const char *val);
int  rdx_modprobe(const char *mod);
/* usbip over SMC needs SMC buffers > the largest URB and no autocorking (gate G3/G4);
   both are per network namespace, we are the only SMC user there */
int  rdx_smc_prepare(void);
void rdx_smc_restore(void);
int  rdx_smc_socket(void);
int  rdx_smc_mode(int fd, char *out, size_t n);   /* "SMC-R sndbuf .. rmb .." / "TCP fallback" via smc_diag */
void rdx_json_str(FILE *f, const char *s);
int  rdx_signalfd(void);                          /* SIGINT/SIGTERM/SIGHUP as a readable fd */

#endif
