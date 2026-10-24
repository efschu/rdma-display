/* SPDX-License-Identifier: MIT
 *
 * USB passthrough, client side (the machine the devices are plugged into).
 *
 *   rdx_usb_client --list-json
 *       Devices this machine could pass through, as JSON for the client UI:
 *       {"devices":[{"id","busid","vid","pid","name","bus","speed","kind",
 *        "kinds":[..],"hidraw":[..],"in_use_by"?}]}
 *       id = USB busid (stable per port), or hid-<uniq|phys> for HID devices
 *       that are not on USB (Bluetooth gamepads, ...). kind is the default path:
 *       "hid" when every interface is HID, otherwise "usbip". in_use_by marks
 *       devices that are busy: "input" (an evdev node of it is held by
 *       rdx_input_tx, i.e. it is the keyboard/mouse of the running session) or
 *       "usbip" (bound to usbip-host, e.g. left over after a crash: --release).
 *
 *   rdx_usb_client --peer HOSTIP [--port 18680] [--dev auto] [--mtu 4096]
 *                  --use ID[:hid|:usbip] [--use ...] [--force] [--no-grab]
 *                  [--comp-vector -1]
 *       Runs for the session. hid: forwards every top-level hidraw node of the
 *       device to the host (uhid there) over RDMA and grabs the local evdev
 *       nodes so the input does not act here as well. usbip: binds the device to
 *       usbip-host and hands the kernel an AF_SMC socket to the host's vhci.
 *       Devices marked in_use_by are refused unless --force. SIGTERM/SIGINT or
 *       the end of the host session releases everything: grabs dropped, usbip
 *       devices unbound from usbip-host and re-probed by their own driver.
 *
 *   rdx_usb_client --release BUSID     give a device stuck on usbip-host back
 *
 *   Test only: --use vudc exports the gadget bound to usbip-vudc (g_zero, as in
 *   the USB gate) through the usbip path, without unbinding a real device.
 */
#define _GNU_SOURCE
#include "rdx_usb.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <linux/usb/ch9.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define SYS_USB  "/sys/bus/usb/devices"
#define STUB_DRV "/sys/bus/usb/drivers/usbip-host"
#define MAX_HID_PER_DEV 8
#define MAX_EV_PER_HID  16

/* ---------------- enumeration ---------------- */

struct cand {
    char id[64], busid[32], name[128], vid[8], pid[8], bus[16], speed[16];
    char hidraw[MAX_HID_PER_DEV][32];
    int nhid, all_hid, is_usb;
    const char *in_use_by;
};

static int is_busid(const char *n) { return n[0] >= '0' && n[0] <= '9' && !strchr(n, ':'); }

/* usb busid owning a sysfs device path: the element in front of the first "<busid>:<cfg>.<if>" */
static int usb_busid_of(const char *real, char *out, size_t n)
{
    const char *p = real;
    char prev[64] = "";
    while ((p = strchr(p, '/'))) {
        p++;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char el[128];
        snprintf(el, sizeof(el), "%.*s", (int)len, p);
        if (strchr(el, ':') && prev[0] && !strncmp(el, prev, strlen(prev)) && el[strlen(prev)] == ':') {
            snprintf(out, n, "%s", prev);
            return 0;
        }
        snprintf(prev, sizeof(prev), "%s", el);
    }
    return -1;
}

/* top-level hid device of a hidraw node: its parent is not another hid device (skips
   e.g. the per-device children of a Logitech receiver, whose reports pass the receiver) */
static int hidraw_toplevel(const char *hidraw_name)
{
    char p[256], real[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/..", hidraw_name);
    if (!realpath(p, real)) return 1;
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    /* hid device names look like BBBB:VVVV:PPPP.NNNN */
    unsigned a, b, c, d;
    return sscanf(base, "%4x:%4x:%4x.%4x", &a, &b, &c, &d) != 4;
}

static void uevent_field(const char *path, const char *key, char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, n, "%s", line + kl + 1);
        }
    fclose(f);
}

/* /dev/input/eventN nodes held open by rdx_input_tx (the running input session) */
static int input_session_nodes(char nodes[][32], int max)
{
    int n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300], comm[64];
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        if (rdx_sysfs_read(p, comm, sizeof(comm)) <= 0 || strcmp(comm, "rdx_input_tx")) continue;
        snprintf(p, sizeof(p), "/proc/%s/fd", e->d_name);
        DIR *fd = opendir(p);
        struct dirent *f;
        while (fd && (f = readdir(fd))) {
            char lp[600], tgt[64];
            snprintf(lp, sizeof(lp), "%s/%s", p, f->d_name);
            ssize_t l = readlink(lp, tgt, sizeof(tgt) - 1);
            if (l <= 0) continue;
            tgt[l] = 0;
            if (!strncmp(tgt, "/dev/input/event", 16) && n < max) snprintf(nodes[n++], 32, "%s", tgt);
        }
        if (fd) closedir(fd);
    }
    if (d) closedir(d);
    return n;
}

/* evdev nodes (/dev/input/eventN) that belong to the hid device behind a hidraw node */
static int hid_event_nodes(const char *hidraw_name, char out[][32], int max)
{
    char p[256], hid[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device", hidraw_name);
    if (!realpath(p, hid)) return 0;
    size_t hl = strlen(hid);
    int n = 0;
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char q[300], real[PATH_MAX];
        snprintf(q, sizeof(q), "/sys/class/input/%s", e->d_name);
        if (!realpath(q, real) || strncmp(real, hid, hl) || real[hl] != '/') continue;
        if (n < max) snprintf(out[n++], 32, "/dev/input/%s", e->d_name);
    }
    if (d) closedir(d);
    return n;
}

static int cand_cmp(const void *a, const void *b)
{
    return strcmp(((const struct cand *)a)->id, ((const struct cand *)b)->id);
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-' ||
              *s == '.' || *s == '_'))
            *s = '_';
}

static int enumerate(struct cand *c, int max)
{
    int n = 0;
    char busy[64][32];
    int nbusy = input_session_nodes(busy, 64);

    DIR *d = opendir(SYS_USB);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (!is_busid(e->d_name)) continue;                   /* skips usbN root hubs and interfaces */
        char p[512], v[256];
        snprintf(p, sizeof(p), SYS_USB "/%s/bDeviceClass", e->d_name);
        if (rdx_sysfs_read_long(p, 16, 0) == USB_CLASS_HUB) continue;
        struct cand *x = &c[n];
        memset(x, 0, sizeof(*x));
        x->is_usb = 1;
        snprintf(x->id, sizeof(x->id), "%s", e->d_name);
        snprintf(x->busid, sizeof(x->busid), "%s", e->d_name);
        snprintf(x->bus, sizeof(x->bus), "usb");
        snprintf(p, sizeof(p), SYS_USB "/%s/idVendor", e->d_name);
        rdx_sysfs_read(p, x->vid, sizeof(x->vid));
        snprintf(p, sizeof(p), SYS_USB "/%s/idProduct", e->d_name);
        rdx_sysfs_read(p, x->pid, sizeof(x->pid));
        snprintf(p, sizeof(p), SYS_USB "/%s/speed", e->d_name);
        rdx_sysfs_read(p, x->speed, sizeof(x->speed));
        char man[128] = "", prod[128] = "";
        snprintf(p, sizeof(p), SYS_USB "/%s/manufacturer", e->d_name);
        rdx_sysfs_read(p, man, sizeof(man));
        snprintf(p, sizeof(p), SYS_USB "/%s/product", e->d_name);
        rdx_sysfs_read(p, prod, sizeof(prod));
        snprintf(x->name, sizeof(x->name), "%s%s%s", man, man[0] && prod[0] ? " " : "", prod);
        if (!x->name[0]) snprintf(x->name, sizeof(x->name), "USB device %s:%s", x->vid, x->pid);
        /* interfaces: all HID? */
        x->all_hid = 1;
        int nif = 0;
        DIR *di = opendir(SYS_USB);
        struct dirent *f;
        size_t bl = strlen(e->d_name);
        while (di && (f = readdir(di))) {
            if (strncmp(f->d_name, e->d_name, bl) || f->d_name[bl] != ':') continue;
            snprintf(p, sizeof(p), SYS_USB "/%s/bInterfaceClass", f->d_name);
            if (rdx_sysfs_read_long(p, 16, -1) != USB_CLASS_HID) x->all_hid = 0;
            nif++;
        }
        if (di) closedir(di);
        if (!nif) x->all_hid = 0;
        snprintf(p, sizeof(p), SYS_USB "/%s/driver", e->d_name);
        ssize_t l = readlink(p, v, sizeof(v) - 1);
        if (l > 0) {
            v[l] = 0;
            if (!strcmp(strrchr(v, '/') ? strrchr(v, '/') + 1 : v, "usbip-host")) x->in_use_by = "usbip";
        }
        n++;
    }
    if (d) closedir(d);

    /* hidraw nodes: attach to their usb device, or list them on their own */
    d = opendir("/sys/class/hidraw");
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "hidraw", 6) || !hidraw_toplevel(e->d_name)) continue;
        char p[300], real[PATH_MAX], busid[64];
        snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device", e->d_name);
        if (!realpath(p, real)) continue;
        struct cand *x = NULL;
        if (!usb_busid_of(real, busid, sizeof(busid)))
            for (int i = 0; i < n; i++)
                if (c[i].is_usb && !strcmp(c[i].busid, busid)) x = &c[i];
        if (!x) {
            if (n >= max) continue;
            x = &c[n++];
            memset(x, 0, sizeof(*x));
            char ue[300], hid_id[64], uniq[64], phys[64];
            snprintf(ue, sizeof(ue), "/sys/class/hidraw/%s/device/uevent", e->d_name);
            uevent_field(ue, "HID_NAME", x->name, sizeof(x->name));
            uevent_field(ue, "HID_ID", hid_id, sizeof(hid_id));
            uevent_field(ue, "HID_UNIQ", uniq, sizeof(uniq));
            uevent_field(ue, "HID_PHYS", phys, sizeof(phys));
            unsigned bus = 0, vid = 0, pid = 0;
            sscanf(hid_id, "%x:%x:%x", &bus, &vid, &pid);
            snprintf(x->vid, sizeof(x->vid), "%04x", vid & 0xFFFF);
            snprintf(x->pid, sizeof(x->pid), "%04x", pid & 0xFFFF);
            snprintf(x->bus, sizeof(x->bus), "%s", bus == BUS_BLUETOOTH ? "bluetooth" : bus == BUS_I2C ? "i2c"
                                                   : bus == BUS_VIRTUAL ? "virtual" : "other");
            snprintf(x->id, sizeof(x->id), "hid-%s", uniq[0] ? uniq : phys[0] ? phys : e->d_name);
            sanitize(x->id + 4);
            x->all_hid = 1;
        }
        if (x->nhid < MAX_HID_PER_DEV) snprintf(x->hidraw[x->nhid++], 32, "/dev/%s", e->d_name);
        char ev[MAX_EV_PER_HID][32];
        int nev = hid_event_nodes(e->d_name, ev, MAX_EV_PER_HID);
        for (int i = 0; i < nev; i++)
            for (int k = 0; k < nbusy; k++)
                if (!strcmp(ev[i], busy[k])) x->in_use_by = "input";
    }
    if (d) closedir(d);
    /* usb devices whose input nodes are not hidraw-backed (e.g. xpad) can still be the session's */
    for (int i = 0; i < n; i++) {
        if (!c[i].is_usb || c[i].in_use_by) continue;
        char p[300];
        snprintf(p, sizeof(p), SYS_USB "/%s", c[i].busid);
        char dev[PATH_MAX];
        if (!realpath(p, dev)) continue;
        size_t dl = strlen(dev);
        for (int k = 0; k < nbusy; k++) {
            char q[300], real[PATH_MAX];
            snprintf(q, sizeof(q), "/sys/class/input/%s", busy[k] + 11);
            if (realpath(q, real) && !strncmp(real, dev, dl) && real[dl] == '/') c[i].in_use_by = "input";
        }
    }
    for (int i = 0; i < n; i++)                                /* hidraw0, hidraw1, ... in order */
        for (int a = 1; a < c[i].nhid; a++)
            for (int b = a; b > 0 && atoi(c[i].hidraw[b - 1] + 11) > atoi(c[i].hidraw[b] + 11); b--) {
                char t[32];
                memcpy(t, c[i].hidraw[b], 32);
                memcpy(c[i].hidraw[b], c[i].hidraw[b - 1], 32);
                memcpy(c[i].hidraw[b - 1], t, 32);
            }
    qsort(c, (size_t)n, sizeof(*c), cand_cmp);
    return n;
}

static int list_json(void)
{
    static struct cand c[64];
    int n = enumerate(c, 64);
    printf("{\"devices\":[");
    for (int i = 0; i < n; i++) {
        struct cand *x = &c[i];
        const char *kind = x->nhid && x->all_hid ? "hid" : x->is_usb ? "usbip" : "hid";
        printf("%s\n {\"id\":", i ? "," : "");
        rdx_json_str(stdout, x->id);
        printf(",\"busid\":");
        if (x->is_usb) rdx_json_str(stdout, x->busid); else printf("null");
        printf(",\"vid\":\"%s\",\"pid\":\"%s\",\"name\":", x->vid, x->pid);
        rdx_json_str(stdout, x->name);
        printf(",\"bus\":\"%s\",\"speed\":", x->bus);
        if (x->speed[0]) rdx_json_str(stdout, x->speed); else printf("null");
        printf(",\"kind\":\"%s\",\"kinds\":[%s%s%s]", kind, x->nhid ? "\"hid\"" : "",
               x->nhid && x->is_usb ? "," : "", x->is_usb ? "\"usbip\"" : "");
        printf(",\"hidraw\":[");
        for (int k = 0; k < x->nhid; k++) printf("%s\"%s\"", k ? "," : "", x->hidraw[k]);
        printf("]");
        if (x->in_use_by) printf(",\"in_use_by\":\"%s\"", x->in_use_by);
        printf("}");
    }
    printf("\n]}\n");
    return 0;
}

/* ---------------- usbip-host binding ---------------- */

/* test mode: --use vudc exports the gadget on usbip-vudc (e.g. g_zero) instead of a real device */
#define VUDC_DIR "/sys/devices/platform/usbip-vudc.0"
static void usbip_attr(const char *busid, const char *attr, char *out, size_t n)
{
    if (!strcmp(busid, "vudc")) snprintf(out, n, VUDC_DIR "/%s", attr);
    else snprintf(out, n, SYS_USB "/%s/%s", busid, attr);
}

static int usbip_status(const char *busid)
{
    char p[256];
    usbip_attr(busid, "usbip_status", p, sizeof(p));
    return (int)rdx_sysfs_read_long(p, 10, -1);       /* 1 available, 2 used, 3 error */
}

static void current_driver(const char *busid, char *out, size_t n)
{
    char p[256], v[256];
    snprintf(p, sizeof(p), SYS_USB "/%s/driver", busid);
    ssize_t l = readlink(p, v, sizeof(v) - 1);
    if (l <= 0) { snprintf(out, n, "(none)"); return; }
    v[l] = 0;
    snprintf(out, n, "%s", strrchr(v, '/') ? strrchr(v, '/') + 1 : v);
}

static int usbip_bind(const char *busid)
{
    char p[256], m[64], drv[64];
    if (rdx_modprobe("usbip-host")) { fprintf(stderr, "[usb-client] modprobe usbip-host failed\n"); return -1; }
    current_driver(busid, drv, sizeof(drv));
    if (strcmp(drv, "usbip-host")) {
        if (strcmp(drv, "(none)")) {                          /* the usb device driver, usually "usb" */
            snprintf(p, sizeof(p), SYS_USB "/%s/driver/unbind", busid);
            if (rdx_sysfs_write(p, busid)) { fprintf(stderr, "[usb-client] unbind %s from %s failed\n", busid, drv); return -1; }
        }
        snprintf(m, sizeof(m), "add %s", busid);
        if (rdx_sysfs_write(STUB_DRV "/match_busid", m)) { fprintf(stderr, "[usb-client] match_busid add failed\n"); return -1; }
        if (rdx_sysfs_write(STUB_DRV "/bind", busid)) { fprintf(stderr, "[usb-client] bind to usbip-host failed\n"); return -1; }
    }
    int st = usbip_status(busid);
    if (st != 1) { fprintf(stderr, "[usb-client] %s: usbip_status %d, not available\n", busid, st); return -1; }
    printf("[usb-client] %s bound to usbip-host (was %s)\n", busid, drv);
    return 0;
}

/* the reverse of usbip_bind, following usbip's own unbind: stop the stub, unbind, rebind */
static int usbip_release(const char *busid)
{
    char p[256], m[64], drv[64];
    if (usbip_status(busid) == 2) {
        usbip_attr(busid, "usbip_sockfd", p, sizeof(p));
        rdx_sysfs_write(p, "-1");
        for (int i = 0; i < 100 && usbip_status(busid) == 2; i++) usleep(10000);
    }
    if (!strcmp(busid, "vudc")) { printf("[usb-client] vudc released, status %d\n", usbip_status(busid)); return 0; }
    current_driver(busid, drv, sizeof(drv));
    if (!strcmp(drv, "usbip-host")) rdx_sysfs_write(STUB_DRV "/unbind", busid);
    rdx_sysfs_write(STUB_DRV "/rebind", busid);               /* re-probes and drops the match entry */
    snprintf(m, sizeof(m), "del %s", busid);
    rdx_sysfs_write(STUB_DRV "/match_busid", m);              /* in case rebind did not */
    current_driver(busid, drv, sizeof(drv));
    if (!strcmp(drv, "(none)")) {
        rdx_sysfs_write("/sys/bus/usb/drivers_probe", busid);
        current_driver(busid, drv, sizeof(drv));
    }
    printf("[usb-client] %s released, driver now %s\n", busid, drv);
    return strcmp(drv, "usbip-host") ? 0 : -1;
}

static uint32_t speed_enum(const char *s)
{
    if (!strcmp(s, "1.5")) return USB_SPEED_LOW;
    if (!strcmp(s, "12")) return USB_SPEED_FULL;
    if (!strcmp(s, "480")) return USB_SPEED_HIGH;
    if (!strcmp(s, "53.3")) return USB_SPEED_WIRELESS;
    return USB_SPEED_SUPER;          /* 5000 and above; vhci has no SuperSpeedPlus ports */
}

/* ---------------- session ---------------- */

struct hdev {                        /* one forwarded hid device (= one hidraw node) */
    int fd, nev, evfd[MAX_EV_PER_HID];
    char path[32];
};
struct udev {                        /* one usbip device */
    char busid[32];
    int sock, bound, up;
};

static struct {
    struct rdx_usb_chan chan;
    struct rdx_usb_dev desc[RDX_USB_MAX_DEV];
    int kind[RDX_USB_MAX_DEV];
    struct hdev hid[RDX_USB_MAX_DEV];
    struct udev usb[RDX_USB_MAX_DEV];
    int ndev, tcp;
    long nin, nout, nget, nset;
} S;

static int hid_open(int i, const char *path)
{
    struct hdev *h = &S.hid[i];
    struct rdx_usb_dev *d = &S.desc[i];
    memset(h, 0, sizeof(*h));
    h->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (h->fd < 0) { perror(path); return -1; }
    snprintf(h->path, sizeof(h->path), "%s", path);
    int sz = 0;
    struct hidraw_report_descriptor rd;
    struct hidraw_devinfo info;
    if (ioctl(h->fd, HIDIOCGRDESCSIZE, &sz) < 0 || sz <= 0 || sz > HID_MAX_DESCRIPTOR_SIZE) { perror("HIDIOCGRDESCSIZE"); return -1; }
    rd.size = (uint32_t)sz;
    if (ioctl(h->fd, HIDIOCGRDESC, &rd) < 0 || ioctl(h->fd, HIDIOCGRAWINFO, &info) < 0) { perror("HIDIOCGRDESC"); return -1; }
    d->kind = RDX_USB_KIND_HID;
    d->rd_size = rd.size;
    memcpy(d->rd, rd.value, rd.size);
    d->bus = (uint32_t)info.bustype;
    d->vendor = (uint16_t)info.vendor;
    d->product = (uint16_t)info.product;
    ioctl(h->fd, HIDIOCGRAWNAME(sizeof(d->name) - 1), d->name);
    ioctl(h->fd, HIDIOCGRAWPHYS(sizeof(d->phys) - 1), d->phys);
    ioctl(h->fd, HIDIOCGRAWUNIQ(sizeof(d->uniq) - 1), d->uniq);
    /* hid-core takes the version from bcdDevice on USB */
    char p[256], real[PATH_MAX], busid[64];
    const char *base = strrchr(path, '/') + 1;
    snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device", base);
    if (realpath(p, real) && !usb_busid_of(real, busid, sizeof(busid))) {
        snprintf(p, sizeof(p), SYS_USB "/%s/bcdDevice", busid);
        d->version = (uint32_t)rdx_sysfs_read_long(p, 16, 0);
    }
    char ev[MAX_EV_PER_HID][32];
    h->nev = hid_event_nodes(base, ev, MAX_EV_PER_HID);
    for (int k = 0; k < h->nev; k++) h->evfd[k] = open(ev[k], O_RDONLY | O_CLOEXEC);   /* for EVIOCGRAB */
    printf("[usb-client] dev %d: hid %s \"%s\" %04x:%04x, descriptor %u bytes, %d evdev node(s)\n", i, path, d->name,
           d->vendor, d->product, d->rd_size, h->nev);
    return 0;
}

static void hid_grab(int i, int on)
{
    struct hdev *h = &S.hid[i];
    for (int k = 0; k < h->nev; k++)
        if (h->evfd[k] >= 0 && ioctl(h->evfd[k], EVIOCGRAB, on ? 1 : 0) && on)
            fprintf(stderr, "[usb-client] dev %d: EVIOCGRAB failed: %s\n", i, strerror(errno));
}

static void hid_close(int i)
{
    struct hdev *h = &S.hid[i];
    for (int k = 0; k < h->nev; k++)
        if (h->evfd[k] >= 0) { ioctl(h->evfd[k], EVIOCGRAB, 0); close(h->evfd[k]); h->evfd[k] = -1; }
    h->nev = 0;
    if (h->fd >= 0) close(h->fd);
    h->fd = -1;
}

static void on_msg(const struct rdx_usb_msg *m, const uint8_t *data, void *user)
{
    (void)user;
    if (m->dev >= S.ndev || S.kind[m->dev] != RDX_USB_KIND_HID) return;
    struct hdev *h = &S.hid[m->dev];
    if (m->type == RDX_USB_M_OUT) {
        if (h->fd >= 0 && write(h->fd, data, m->len) < 0) perror("hidraw write (output report)");
        S.nout++;
    } else if (m->type == RDX_USB_M_GETREQ) {
        uint8_t r[RDX_USB_MAX_REPORT];
        r[0] = m->rnum;
        int n = h->fd < 0 ? -ENODEV
              : ioctl(h->fd, m->rtype == UHID_FEATURE_REPORT ? HIDIOCGFEATURE(sizeof(r))
                           : m->rtype == UHID_OUTPUT_REPORT  ? HIDIOCGOUTPUT(sizeof(r)) : HIDIOCGINPUT(sizeof(r)), r);
        struct rdx_usb_msg rep = {.type = RDX_USB_M_GETREP, .dev = m->dev, .id = m->id,
                                  .len = n > 0 ? (uint16_t)n : 0, .err = n < 0 ? (uint16_t)(h->fd < 0 ? ENODEV : errno) : 0};
        rdx_usb_chan_send(&S.chan, &rep, r, on_msg, NULL);
        S.nget++;
    } else if (m->type == RDX_USB_M_SETREQ) {
        int n = h->fd < 0 ? -1
              : ioctl(h->fd, m->rtype == UHID_FEATURE_REPORT ? HIDIOCSFEATURE(m->len) : HIDIOCSOUTPUT(m->len), data);
        struct rdx_usb_msg rep = {.type = RDX_USB_M_SETREP, .dev = m->dev, .id = m->id,
                                  .err = n < 0 ? (uint16_t)(h->fd < 0 ? ENODEV : errno) : 0};
        rdx_usb_chan_send(&S.chan, &rep, NULL, on_msg, NULL);
        S.nset++;
    }
}

static int add_device(struct cand *c, int nc, const char *spec, int force)
{
    char id[64], want[16] = "";
    snprintf(id, sizeof(id), "%s", spec);
    char *colon = strrchr(id, ':');
    if (colon && (!strcmp(colon + 1, "hid") || !strcmp(colon + 1, "usbip"))) {
        snprintf(want, sizeof(want), "%s", colon + 1);
        *colon = 0;
    }
    struct cand *x = NULL, vudc;
    for (int i = 0; i < nc; i++) if (!strcmp(c[i].id, id)) x = &c[i];
    if (!x && !strcmp(id, "vudc")) {
        if (usbip_status("vudc") != 1) { fprintf(stderr, "[usb-client] usbip-vudc not available\n"); return -1; }
        memset(&vudc, 0, sizeof(vudc));
        snprintf(vudc.id, sizeof(vudc.id), "vudc");
        snprintf(vudc.busid, sizeof(vudc.busid), "vudc");
        snprintf(vudc.name, sizeof(vudc.name), "usbip-vudc test gadget");
        snprintf(vudc.speed, sizeof(vudc.speed), "480");
        snprintf(vudc.vid, sizeof(vudc.vid), "0000");
        snprintf(vudc.pid, sizeof(vudc.pid), "0000");
        vudc.is_usb = 1;
        x = &vudc;
    }
    if (!x) { fprintf(stderr, "[usb-client] no device %s (see --list-json)\n", id); return -1; }
    if (x->in_use_by && !force) {
        fprintf(stderr, "[usb-client] %s is in use by %s; refusing without --force\n", id, x->in_use_by);
        return -1;
    }
    if (!want[0]) snprintf(want, sizeof(want), "%s", x->nhid && x->all_hid ? "hid" : x->is_usb ? "usbip" : "hid");
    if (!strcmp(want, "hid")) {
        if (!x->nhid) { fprintf(stderr, "[usb-client] %s has no HID interface\n", id); return -1; }
        for (int k = 0; k < x->nhid; k++) {
            if (S.ndev >= RDX_USB_MAX_DEV) { fprintf(stderr, "[usb-client] too many devices\n"); return -1; }
            int i = S.ndev;
            S.kind[i] = RDX_USB_KIND_HID;
            if (hid_open(i, x->hidraw[k])) return -1;
            snprintf(S.desc[i].id, sizeof(S.desc[i].id), "%s", x->id);
            S.ndev++;
        }
        return 0;
    }
    if (!x->is_usb) { fprintf(stderr, "[usb-client] %s is not a USB device\n", id); return -1; }
    if (S.ndev >= RDX_USB_MAX_DEV) { fprintf(stderr, "[usb-client] too many devices\n"); return -1; }
    int i = S.ndev;
    struct rdx_usb_dev *d = &S.desc[i];
    S.kind[i] = RDX_USB_KIND_USBIP;
    S.usb[i].sock = -1;
    snprintf(S.usb[i].busid, sizeof(S.usb[i].busid), "%s", x->busid);
    d->kind = RDX_USB_KIND_USBIP;
    snprintf(d->id, sizeof(d->id), "%s", x->busid);
    snprintf(d->name, sizeof(d->name), "%s", x->name);
    char p[256];
    snprintf(p, sizeof(p), SYS_USB "/%s/busnum", x->busid);
    long busnum = rdx_sysfs_read_long(p, 10, 0);
    snprintf(p, sizeof(p), SYS_USB "/%s/devnum", x->busid);
    long devnum = rdx_sysfs_read_long(p, 10, 0);
    d->devid = (uint32_t)((busnum << 16) | devnum);         /* vudc: 0, it does not check */
    d->speed = speed_enum(x->speed);
    d->vendor = (uint32_t)strtoul(x->vid, NULL, 16);
    d->product = (uint32_t)strtoul(x->pid, NULL, 16);
    S.ndev++;
    if (strcmp(x->busid, "vudc") && usbip_bind(x->busid)) return -1;
    S.usb[i].bound = 1;
    printf("[usb-client] dev %d: usbip %s \"%s\" %s:%s speed %s devid 0x%x\n", i, x->busid, x->name, x->vid, x->pid,
           x->speed, d->devid);
    return 0;
}

static void cleanup(void)
{
    for (int i = 0; i < S.ndev; i++) {
        if (S.kind[i] == RDX_USB_KIND_HID) hid_close(i);
        else if (S.usb[i].bound) {
            usbip_release(S.usb[i].busid);
            S.usb[i].bound = 0;
            if (S.usb[i].sock >= 0) close(S.usb[i].sock);
            S.usb[i].sock = -1;
        }
    }
    rdx_smc_restore();
}

/* host has acknowledged our BYE or closed: wait for it (bounded) */
static void wait_bye_ack(int ms)
{
    uint64_t end = rdx_now_ns() + (uint64_t)ms * 1000000ull;
    while (rdx_now_ns() < end) {
        struct pollfd p = {.fd = S.tcp, .events = POLLIN};
        if (poll(&p, 1, (int)((end - rdx_now_ns()) / 1000000ull) + 1) <= 0) return;
        struct rdx_usb_ctl m;
        if (rdx_io_all(S.tcp, &m, sizeof(m), 0) || m.type == RDX_USB_CTL_BYE_ACK) return;
    }
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer = NULL, *uses[RDX_USB_MAX_DEV], *release = NULL;
    int port = RDX_USB_PORT, mtu = 4096, nuse = 0, force = 0, grab = 1, comp_vector = -1, list = 0;
    static const struct option opts[] = {
        {"list-json", 0, 0, 'l'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"dev", 1, 0, 'd'},
        {"mtu", 1, 0, 'm'}, {"use", 1, 0, 'u'}, {"force", 0, 0, 'f'}, {"no-grab", 0, 0, 'G'},
        {"comp-vector", 1, 0, 'V'}, {"release", 1, 0, 'r'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'l': list = 1; break;
        case 'p': peer = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'd': dev = optarg; break;
        case 'm': mtu = atoi(optarg); break;
        case 'u': if (nuse < RDX_USB_MAX_DEV) uses[nuse++] = optarg; break;
        case 'f': force = 1; break;
        case 'G': grab = 0; break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'r': release = optarg; break;
        default:  fprintf(stderr, "see the header of rdx_usb_client.c for options\n"); return 2;
        }
    }
    if (list) return list_json();
    if (release) return usbip_release(release) ? 1 : 0;
    if (!peer || !nuse) { fprintf(stderr, "need --peer and at least one --use (or --list-json)\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sfd = rdx_signalfd();

    static struct cand cands[64];
    int nc = enumerate(cands, 64);
    int nusbip = 0, rc = 1;
    for (int i = 0; i < nuse; i++)
        if (add_device(cands, nc, uses[i], force)) goto out;
    for (int i = 0; i < S.ndev; i++) nusbip += S.kind[i] == RDX_USB_KIND_USBIP;
    if (nusbip && rdx_smc_prepare()) goto out;
    char lip[64] = "";
    if (peer) rdx_local_ip_for(peer, lip, sizeof(lip));   /* the seat's link IP towards the host */
    if (rdx_usb_chan_init(&S.chan, dev, lip, comp_vector)) goto out;

    S.tcp = rdx_tcp_connect(peer, port);
    if (S.tcp < 0) goto out;
    struct rdx_usb_hello hello = {.magic = RDX_USB_MAGIC, .ndev = (uint32_t)S.ndev, .conn = S.chan.mine};
    struct rdx_usb_ready ready;
    if (rdx_io_all(S.tcp, &hello, sizeof(hello), 1) || rdx_io_all(S.tcp, S.desc, sizeof(S.desc[0]) * S.ndev, 1) ||
        rdx_io_all(S.tcp, &ready, sizeof(ready), 0) || ready.magic != RDX_USB_MAGIC) {
        fprintf(stderr, "[usb-client] bootstrap failed\n"); goto out;
    }
    if (rdx_usb_chan_connect(&S.chan, &ready.conn, mtu) || rdx_sync(S.tcp)) { fprintf(stderr, "[usb-client] RDMA connect failed\n"); goto out; }
    for (int i = 0; i < S.ndev; i++)
        if (ready.status[i]) fprintf(stderr, "[usb-client] dev %d: host refused: %s\n", i, strerror(-ready.status[i]));

    /* usbip: one AF_SMC connection per device, handed to usbip-host; the host attaches it to vhci */
    for (int i = 0; i < S.ndev; i++) {
        if (S.kind[i] != RDX_USB_KIND_USBIP) continue;
        int s = rdx_smc_socket();
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)(port + 1))};
        a.sin_addr.s_addr = inet_addr(peer);
        struct rdx_usb_smc_hello sh = {.magic = RDX_USB_MAGIC, .dev = (uint32_t)i};
        char p[256], fdv[16], mode[96];
        if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) || rdx_io_all(s, &sh, sizeof(sh), 1)) {
            perror("[usb-client] usbip socket"); goto bye;
        }
        S.usb[i].sock = s;
        rdx_smc_mode(s, mode, sizeof(mode));
        usbip_attr(S.usb[i].busid, "usbip_sockfd", p, sizeof(p));
        snprintf(fdv, sizeof(fdv), "%d", s);
        int e = rdx_sysfs_write(p, fdv);
        if (e) { fprintf(stderr, "[usb-client] %s: usbip_sockfd: %s\n", S.usb[i].busid, strerror(-e)); goto bye; }
        printf("[usb-client] dev %d: %s exported over %s\n", i, S.usb[i].busid, mode);
    }
    for (int k = 0; k < nusbip; k++) {                       /* attach results from the host */
        struct pollfd p = {.fd = S.tcp, .events = POLLIN};
        struct rdx_usb_ctl m;
        if (poll(&p, 1, 5000) <= 0 || rdx_io_all(S.tcp, &m, sizeof(m), 0) || m.type != RDX_USB_CTL_USBIP_UP ||
            m.dev >= (uint32_t)S.ndev) {
            fprintf(stderr, "[usb-client] no attach answer from the host\n"); goto bye;
        }
        if (m.status < 0) { fprintf(stderr, "[usb-client] dev %u: host attach failed: %s\n", m.dev, strerror(-m.status)); goto bye; }
        S.usb[m.dev].up = 1;
        printf("[usb-client] dev %u: attached on the host, vhci port %d\n", m.dev, m.status);
    }
    for (int i = 0; i < S.ndev; i++)
        if (S.kind[i] == RDX_USB_KIND_HID && grab) hid_grab(i, 1);
    printf("[usb-client] session up: %d device(s) on %s:%d, hid over RDMA (inline <= %u B), usbip over AF_SMC\n",
           S.ndev, peer, port, S.chan.max_inline);
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns();

    struct pollfd pf[3 + RDX_USB_MAX_DEV];
    int idx[3 + RDX_USB_MAX_DEV];
    for (int running = 1; running;) {
        int np = 0;
        pf[np] = (struct pollfd){.fd = sfd, .events = POLLIN}; idx[np++] = -1;
        pf[np] = (struct pollfd){.fd = S.tcp, .events = POLLIN}; idx[np++] = -2;
        pf[np] = (struct pollfd){.fd = rdx_usb_chan_fd(&S.chan), .events = POLLIN}; idx[np++] = -3;
        for (int i = 0; i < S.ndev; i++)
            if (S.kind[i] == RDX_USB_KIND_HID && S.hid[i].fd >= 0) {
                pf[np] = (struct pollfd){.fd = S.hid[i].fd, .events = POLLIN};
                idx[np++] = i;
            }
        if (poll(pf, (nfds_t)np, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int k = 0; k < np && running; k++) {
            if (!pf[k].revents) continue;
            if (idx[k] == -1) {
                struct signalfd_siginfo si;
                if (read(sfd, &si, sizeof(si)) > 0) printf("[usb-client] signal %u: ending session\n", si.ssi_signo);
                struct rdx_usb_ctl bye = {.type = RDX_USB_CTL_BYE};
                if (!rdx_io_all(S.tcp, &bye, sizeof(bye), 1)) wait_bye_ack(2000);
                running = 0;
                rc = 0;
            } else if (idx[k] == -2) {
                struct rdx_usb_ctl m;
                if (rdx_io_all(S.tcp, &m, sizeof(m), 0)) { printf("[usb-client] host closed the session\n"); running = 0; rc = 0; }
                else if (m.type == RDX_USB_CTL_BYE) {
                    struct rdx_usb_ctl ack = {.type = RDX_USB_CTL_BYE_ACK};
                    rdx_io_all(S.tcp, &ack, sizeof(ack), 1);
                    printf("[usb-client] host ended the session\n");
                    running = 0;
                    rc = 0;
                }
            } else if (idx[k] == -3) {
                if (rdx_usb_chan_dispatch(&S.chan, on_msg, NULL)) { running = 0; break; }
            } else {
                int i = idx[k];
                uint8_t buf[RDX_USB_MAX_REPORT];
                ssize_t n = read(S.hid[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    struct rdx_usb_msg h = {.type = RDX_USB_M_IN, .dev = (uint16_t)i, .len = (uint16_t)n};
                    if (rdx_usb_chan_send(&S.chan, &h, buf, on_msg, NULL)) { running = 0; break; }
                    S.nin++;
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR) || (pf[k].revents & (POLLHUP | POLLERR))) {
                    printf("[usb-client] dev %d: %s gone\n", i, S.hid[i].path);
                    hid_close(i);
                    struct rdx_usb_msg h = {.type = RDX_USB_M_GONE, .dev = (uint16_t)i};
                    rdx_usb_chan_send(&S.chan, &h, NULL, on_msg, NULL);
                }
            }
        }
    }
    rdx_cpu_report("usb-client", t0_wall, t0_cpu);
    printf("[usb-client] %ld input reports up, %ld output reports down, %ld GET, %ld SET\n", S.nin, S.nout, S.nget, S.nset);
    goto out;
bye:
    {
        struct rdx_usb_ctl bye = {.type = RDX_USB_CTL_BYE};
        if (!rdx_io_all(S.tcp, &bye, sizeof(bye), 1)) wait_bye_ack(2000);
    }
out:
    cleanup();
    if (S.tcp > 0) close(S.tcp);
    if (S.chan.ctx) rdx_usb_chan_destroy(&S.chan);
    return rc;
}
