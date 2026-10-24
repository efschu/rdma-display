/* SPDX-License-Identifier: MIT
 *
 * USB passthrough, host side (the machine the devices should appear on).
 *
 *   rdx_usb_host [--bind 10.0.0.2] [--port 18680] [--dev auto] [--mtu 4096]
 *                [--comp-vector -1] [--private] [--wait-grab]
 *
 * Runs for one session: prints "[usb-host] listening on IP:PORT", accepts one
 * rdx_usb_client, then
 *   hid    creates one /dev/uhid device per forwarded hidraw node with the
 *          client's report descriptor, name, bus, vendor, product and version,
 *          injects input reports and sends output reports and GET/SET_REPORT
 *          back (phys is "rdx:<client phys>" so the mirror is recognisable);
 *   usbip  accepts the device's AF_SMC connection on PORT+1 and attaches it
 *          to a free vhci-hcd port (sysfs "attach"), no usbip userspace needed.
 * --private (rdx-hostd, gamescope apps): the devices are for one gamescope only, never the desktop
 * (common/rdx_input.h RDX_INPUT_PRIVATE_*): uhid phys "rdx:private:<client phys>"; for USB/IP the
 * flag file VHCI_PRIVATE_FLAG exists while this session runs, and desktop/72-rdma-display-input.rules
 * moves every input device behind vhci_hcd to the private seat meanwhile (vhci ports are host-wide:
 * two sessions with USB/IP on one host share the setting of the one that started last).
 * --wait-grab (rdx-hostd, gamescope apps): input reports of a HID device with a keyboard part are
 * held back until its consumer (gamescope) grabs its keyboard evdev nodes (common/rdx_input.h, grab
 * gate): the host's console keyboard handler must never see them. Devices without one (gamepads)
 * pass; a device that has no input node 2 s after its creation passes too.
 * The session ends when the client says BYE, its connection drops, or on
 * SIGTERM/SIGINT; then all uhid devices are destroyed, vhci ports detached and
 * the SMC settings restored.
 */
#define _GNU_SOURCE
#include "rdx_usb.h"
#include "../common/rdx_input.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <getopt.h>
#include <linux/uhid.h>
#include <linux/usb/ch9.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define VHCI "/sys/devices/platform/vhci_hcd.0"
#define VHCI_PRIVATE_FLAG "/run/rdx-vhci-private"     /* TEST in desktop/72-rdma-display-input.rules */
static int g_private, g_flag_made, g_wait_grab;

/* --wait-grab: the evdev nodes of each uhid device, found by its phys once the kernel made them */
#define UHID_MAX_NODES 8
static struct {
    char phys[72], name[48];
    int found, n;
    uint64_t t_create, t_try;
    struct rdx_grab_gate gate[UHID_MAX_NODES];
} G[RDX_USB_MAX_DEV];

static void uhid_find_nodes(int i)
{
    DIR *in = opendir("/sys/class/input");
    struct dirent *e;
    while (in && (e = readdir(in)) && G[i].n < UHID_MAX_NODES) {
        if (strncmp(e->d_name, "input", 5)) continue;
        char p[300], phys[80] = "";
        snprintf(p, sizeof(p), "/sys/class/input/%s/phys", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(phys, sizeof(phys), f)) phys[0] = 0;
        fclose(f);
        phys[strcspn(phys, "\n")] = 0;
        if (strcmp(phys, G[i].phys)) continue;
        G[i].found = 1;
        snprintf(p, sizeof(p), "/sys/class/input/%s", e->d_name);
        DIR *d = opendir(p);
        struct dirent *n;
        while (d && (n = readdir(d)) && G[i].n < UHID_MAX_NODES) {
            if (strncmp(n->d_name, "event", 5)) continue;
            char dev[64], what[96];
            snprintf(dev, sizeof(dev), "/dev/input/%s", n->d_name);
            snprintf(what, sizeof(what), "%.40s (%s)", G[i].name, n->d_name);
            rdx_grab_gate_init(&G[i].gate[G[i].n++], dev, what);
        }
        if (d) closedir(d);
    }
    if (in) closedir(in);
}

/* 1: inject the device's input report, 0: hold it back (a keyboard part not grabbed yet) */
static int uhid_gate_pass(int i)
{
    if (!g_wait_grab) return 1;
    uint64_t now = rdx_now_ns();
    if (!G[i].found) {
        if (now - G[i].t_try >= 100000000ull) {
            G[i].t_try = now;
            uhid_find_nodes(i);
        }
        if (!G[i].found) return now - G[i].t_create > 2000000000ull;
    }
    int ok = 1;
    for (int k = 0; k < G[i].n; k++)
        if (!rdx_grab_gate_pass(&G[i].gate[k])) ok = 0;
    return ok;
}

static struct {
    struct rdx_usb_chan chan;
    struct rdx_usb_dev desc[RDX_USB_MAX_DEV];
    int uhid[RDX_USB_MAX_DEV];               /* hid: uhid fd, -1 when gone */
    int port[RDX_USB_MAX_DEV];               /* usbip: vhci port, -1 when not attached */
    int sock[RDX_USB_MAX_DEV];               /* usbip: our reference to the AF_SMC socket */
    int ndev, tcp;
    long nin, nout, nget, nset;
} S;

static int tcp_listen(const char *ip, int port, int smc)
{
    int one = 1, l = smc ? rdx_smc_socket() : socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (l < 0) return -1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = inet_addr(ip);
    if (bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, RDX_USB_MAX_DEV)) {
        fprintf(stderr, "[usb-host] bind/listen %s:%d: %s\n", ip, port, strerror(errno));
        close(l);
        return -1;
    }
    return l;
}

/* accept with the signal fd watched; -1 on timeout, error or signal */
static int accept_wait(int l, int sfd, int timeout_ms)
{
    struct pollfd p[2] = {{.fd = l, .events = POLLIN}, {.fd = sfd, .events = POLLIN}};
    for (;;) {
        int r = poll(p, 2, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || (p[1].revents & POLLIN)) return -1;
        return accept4(l, NULL, NULL, SOCK_CLOEXEC);
    }
}

static int uhid_send(int fd, struct uhid_event *ev)
{
    return write(fd, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev) ? 0 : -1;
}

static int uhid_create(int i)
{
    struct rdx_usb_dev *d = &S.desc[i];
    int u = open("/dev/uhid", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (u < 0) { int e = errno; perror("/dev/uhid"); return -e; }
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    d->name[sizeof(d->name) - 1] = d->phys[sizeof(d->phys) - 1] = d->uniq[sizeof(d->uniq) - 1] = 0;
    snprintf((char *)ev.u.create2.name, sizeof(ev.u.create2.name), "%s", d->name);
    snprintf((char *)ev.u.create2.phys, sizeof(ev.u.create2.phys), "%s%s", g_private ? "rdx:private:" : "rdx:", d->phys);
    snprintf((char *)ev.u.create2.uniq, sizeof(ev.u.create2.uniq), "%s", d->uniq);
    snprintf(G[i].phys, sizeof(G[i].phys), "%s", (char *)ev.u.create2.phys);
    snprintf(G[i].name, sizeof(G[i].name), "%s", d->name);
    G[i].found = G[i].n = 0;
    G[i].t_create = rdx_now_ns();
    G[i].t_try = 0;
    if (d->rd_size > sizeof(ev.u.create2.rd_data)) { close(u); return -EINVAL; }
    ev.u.create2.rd_size = (uint16_t)d->rd_size;
    memcpy(ev.u.create2.rd_data, d->rd, d->rd_size);
    ev.u.create2.bus = (uint16_t)d->bus;
    ev.u.create2.vendor = d->vendor;
    ev.u.create2.product = d->product;
    ev.u.create2.version = d->version;
    ev.u.create2.country = d->country;
    if (uhid_send(u, &ev)) { int e = errno; perror("UHID_CREATE2"); close(u); return -e; }
    S.uhid[i] = u;
    printf("[usb-host] dev %d: uhid \"%s\" %04x:%04x v%04x bus %u, descriptor %u bytes\n", i, d->name, d->vendor,
           d->product, d->version, d->bus, d->rd_size);
    return 0;
}

static void uhid_destroy(int i)
{
    if (S.uhid[i] < 0) return;
    for (int k = 0; k < G[i].n; k++) rdx_grab_gate_close(&G[i].gate[k]);
    G[i].n = G[i].found = 0;
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_send(S.uhid[i], &ev);
    close(S.uhid[i]);
    S.uhid[i] = -1;
}

/* free vhci port for this speed: status lines "hub port sta spd dev sockfd local_busid", sta 4 = free */
static int vhci_free_port(uint32_t speed)
{
    const char *hub = speed >= USB_SPEED_SUPER ? "ss" : "hs";
    char path[128];
    for (int f = 0; f < 32; f++) {
        if (f) snprintf(path, sizeof(path), VHCI "/status.%d", f);
        else snprintf(path, sizeof(path), VHCI "/status");
        FILE *fp = fopen(path, "r");
        if (!fp) break;
        char line[256], h[8];
        unsigned port, sta;
        while (fgets(line, sizeof(line), fp))
            if (sscanf(line, "%7s %u %u", h, &port, &sta) == 3 && !strcmp(h, hub) && sta == 4) {
                fclose(fp);
                return (int)port;
            }
        fclose(fp);
    }
    return -1;
}

static int vhci_attach(int i, int sock)
{
    struct rdx_usb_dev *d = &S.desc[i];
    int port = vhci_free_port(d->speed);
    if (port < 0) { fprintf(stderr, "[usb-host] no free vhci port\n"); return -ENOSPC; }
    char v[64];
    if (g_private && !g_flag_made) {           /* before the device appears: udev's rules see it */
        int f = open(VHCI_PRIVATE_FLAG, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (f >= 0) { close(f); g_flag_made = 1; }
        else perror(VHCI_PRIVATE_FLAG);
    }
    snprintf(v, sizeof(v), "%d %d %u %u", port, sock, d->devid, d->speed);
    int e = rdx_sysfs_write(VHCI "/attach", v);
    if (e) { fprintf(stderr, "[usb-host] vhci attach \"%s\": %s\n", v, strerror(-e)); return e; }
    S.port[i] = port;
    return port;
}

static void vhci_detach(int i)
{
    if (S.port[i] < 0) return;
    char v[16];
    snprintf(v, sizeof(v), "%d", S.port[i]);
    int e = rdx_sysfs_write(VHCI "/detach", v);
    printf("[usb-host] dev %d: vhci port %d detached%s\n", i, S.port[i], e ? " (already gone)" : "");
    S.port[i] = -1;
}

static void on_msg(const struct rdx_usb_msg *m, const uint8_t *data, void *user)
{
    (void)user;
    if (m->dev >= S.ndev || S.uhid[m->dev] < 0) return;
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    if (m->len > RDX_USB_MAX_REPORT) return;
    switch (m->type) {
    case RDX_USB_M_IN:
        if (!uhid_gate_pass(m->dev)) return;    /* --wait-grab: the keyboard part is not grabbed yet */
        ev.type = UHID_INPUT2;
        ev.u.input2.size = m->len;
        memcpy(ev.u.input2.data, data, m->len);
        S.nin++;
        break;
    case RDX_USB_M_GETREP:
        ev.type = UHID_GET_REPORT_REPLY;
        ev.u.get_report_reply.id = m->id;
        ev.u.get_report_reply.err = m->err;
        ev.u.get_report_reply.size = m->len;
        memcpy(ev.u.get_report_reply.data, data, m->len);
        break;
    case RDX_USB_M_SETREP:
        ev.type = UHID_SET_REPORT_REPLY;
        ev.u.set_report_reply.id = m->id;
        ev.u.set_report_reply.err = m->err;
        break;
    case RDX_USB_M_GONE:
        printf("[usb-host] dev %u: unplugged on the client\n", m->dev);
        uhid_destroy(m->dev);
        return;
    default:
        return;
    }
    if (uhid_send(S.uhid[m->dev], &ev)) perror("uhid write");
}

static int on_uhid(int i)
{
    struct uhid_event ev;
    ssize_t r = read(S.uhid[i], &ev, sizeof(ev));
    if (r <= 0) return errno == EAGAIN ? 0 : -1;
    struct rdx_usb_msg m = {.dev = (uint16_t)i};
    const void *payload = NULL;
    switch (ev.type) {
    case UHID_OUTPUT:
        m.type = RDX_USB_M_OUT;
        m.len = ev.u.output.size;
        payload = ev.u.output.data;
        S.nout++;
        break;
    case UHID_GET_REPORT:
        m.type = RDX_USB_M_GETREQ;
        m.id = ev.u.get_report.id;
        m.rnum = ev.u.get_report.rnum;
        m.rtype = ev.u.get_report.rtype;
        S.nget++;
        break;
    case UHID_SET_REPORT:
        m.type = RDX_USB_M_SETREQ;
        m.id = ev.u.set_report.id;
        m.rnum = ev.u.set_report.rnum;
        m.rtype = ev.u.set_report.rtype;
        m.len = ev.u.set_report.size;
        payload = ev.u.set_report.data;
        S.nset++;
        break;
    default:                                  /* START/STOP/OPEN/CLOSE: nothing to forward */
        return 0;
    }
    return rdx_usb_chan_send(&S.chan, &m, payload, on_msg, NULL);
}

static void cleanup(void)
{
    for (int i = 0; i < S.ndev; i++) {
        if (S.desc[i].kind == RDX_USB_KIND_HID) uhid_destroy(i);
        else vhci_detach(i);
        if (S.sock[i] >= 0) close(S.sock[i]);
        S.sock[i] = -1;
    }
    if (g_flag_made) unlink(VHCI_PRIVATE_FLAG);
    g_flag_made = 0;
    rdx_smc_restore();
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "10.0.0.2";
    int port = RDX_USB_PORT, mtu = 4096, comp_vector = -1, rc = 1, lsmc = -1;
    static const struct option opts[] = {
        {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"dev", 1, 0, 'd'}, {"mtu", 1, 0, 'm'},
        {"comp-vector", 1, 0, 'V'}, {"private", 0, 0, 'p'}, {"wait-grab", 0, 0, 'g'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'd': dev = optarg; break;
        case 'm': mtu = atoi(optarg); break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'p': g_private = 1; break;
        case 'g': g_wait_grab = 1; break;
        default:  fprintf(stderr, "see the header of rdx_usb_host.c for options\n"); return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sfd = rdx_signalfd();
    for (int i = 0; i < RDX_USB_MAX_DEV; i++) S.uhid[i] = S.port[i] = S.sock[i] = -1;
    if (rdx_usb_chan_init(&S.chan, dev, bind_ip, comp_vector)) return 1;
    int l = tcp_listen(bind_ip, port, 0);
    if (l < 0) return 1;
    printf("[usb-host] listening on %s:%d\n", bind_ip, port);
    S.tcp = accept_wait(l, sfd, -1);
    close(l);
    if (S.tcp < 0) { printf("[usb-host] stopped before a client came\n"); return 0; }
    int one = 1;
    setsockopt(S.tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct rdx_usb_hello hello;
    if (rdx_io_all(S.tcp, &hello, sizeof(hello), 0) || hello.magic != RDX_USB_MAGIC || hello.ndev > RDX_USB_MAX_DEV ||
        rdx_io_all(S.tcp, S.desc, sizeof(S.desc[0]) * hello.ndev, 0)) {
        fprintf(stderr, "[usb-host] bootstrap: invalid hello\n"); goto out;
    }
    S.ndev = (int)hello.ndev;
    struct rdx_usb_ready ready = {.magic = RDX_USB_MAGIC, .conn = S.chan.mine};
    int nusbip = 0;
    for (int i = 0; i < S.ndev; i++) {
        if (S.desc[i].kind == RDX_USB_KIND_HID) ready.status[i] = uhid_create(i);
        else if (S.desc[i].kind == RDX_USB_KIND_USBIP) nusbip++;
        else ready.status[i] = -EINVAL;
    }
    if (nusbip) {                                 /* listener must exist before the client connects */
        if (rdx_smc_prepare() || rdx_modprobe("vhci-hcd")) { fprintf(stderr, "[usb-host] usbip setup failed\n"); goto out; }
        lsmc = tcp_listen(bind_ip, port + 1, 1);
        if (lsmc < 0) goto out;
    }
    if (rdx_io_all(S.tcp, &ready, sizeof(ready), 1) || rdx_usb_chan_connect(&S.chan, &hello.conn, mtu) ||
        rdx_sync(S.tcp)) {
        fprintf(stderr, "[usb-host] RDMA connect failed\n"); goto out;
    }
    for (int k = 0; k < nusbip; k++) {
        int s = accept_wait(lsmc, sfd, 5000);
        struct rdx_usb_smc_hello sh;
        if (s < 0 || rdx_io_all(s, &sh, sizeof(sh), 0) || sh.magic != RDX_USB_MAGIC || sh.dev >= (uint32_t)S.ndev ||
            S.desc[sh.dev].kind != RDX_USB_KIND_USBIP) {
            fprintf(stderr, "[usb-host] usbip connection missing or invalid\n");
            if (s >= 0) close(s);
            goto out;
        }
        S.sock[sh.dev] = s;
        char mode[96];
        rdx_smc_mode(s, mode, sizeof(mode));
        int r = vhci_attach((int)sh.dev, s);
        struct rdx_usb_ctl up = {.type = RDX_USB_CTL_USBIP_UP, .dev = sh.dev, .status = r};
        if (rdx_io_all(S.tcp, &up, sizeof(up), 1)) goto out;
        if (r >= 0)
            printf("[usb-host] dev %u: \"%s\" %04x:%04x attached on vhci port %d over %s\n", sh.dev,
                   S.desc[sh.dev].name, S.desc[sh.dev].vendor, S.desc[sh.dev].product, r, mode);
    }
    if (lsmc >= 0) close(lsmc);
    lsmc = -1;
    printf("[usb-host] session up: %d device(s)\n", S.ndev);
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns();

    struct pollfd pf[3 + RDX_USB_MAX_DEV];
    int idx[3 + RDX_USB_MAX_DEV];
    for (int running = 1; running;) {
        int np = 0;
        pf[np] = (struct pollfd){.fd = sfd, .events = POLLIN}; idx[np++] = -1;
        pf[np] = (struct pollfd){.fd = S.tcp, .events = POLLIN}; idx[np++] = -2;
        pf[np] = (struct pollfd){.fd = rdx_usb_chan_fd(&S.chan), .events = POLLIN}; idx[np++] = -3;
        for (int i = 0; i < S.ndev; i++)
            if (S.uhid[i] >= 0) { pf[np] = (struct pollfd){.fd = S.uhid[i], .events = POLLIN}; idx[np++] = i; }
        if (poll(pf, (nfds_t)np, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int k = 0; k < np && running; k++) {
            if (!pf[k].revents) continue;
            if (idx[k] == -1) {
                struct signalfd_siginfo si;
                if (read(sfd, &si, sizeof(si)) > 0) printf("[usb-host] signal %u: ending session\n", si.ssi_signo);
                cleanup();                        /* detach first, then tell the client to release */
                struct rdx_usb_ctl bye = {.type = RDX_USB_CTL_BYE};
                rdx_io_all(S.tcp, &bye, sizeof(bye), 1);
                struct pollfd p = {.fd = S.tcp, .events = POLLIN};
                struct rdx_usb_ctl ack;
                if (poll(&p, 1, 2000) > 0) rdx_io_all(S.tcp, &ack, sizeof(ack), 0);
                running = 0;
                rc = 0;
            } else if (idx[k] == -2) {
                struct rdx_usb_ctl m;
                if (rdx_io_all(S.tcp, &m, sizeof(m), 0)) { printf("[usb-host] client gone\n"); running = 0; rc = 0; }
                else if (m.type == RDX_USB_CTL_BYE) {
                    cleanup();
                    struct rdx_usb_ctl ack = {.type = RDX_USB_CTL_BYE_ACK};
                    rdx_io_all(S.tcp, &ack, sizeof(ack), 1);
                    printf("[usb-host] client ended the session\n");
                    running = 0;
                    rc = 0;
                }
            } else if (idx[k] == -3) {
                if (rdx_usb_chan_dispatch(&S.chan, on_msg, NULL)) { running = 0; break; }
            } else if (on_uhid(idx[k])) {
                running = 0;
            }
        }
    }
    rdx_cpu_report("usb-host", t0_wall, t0_cpu);
    printf("[usb-host] %ld input reports in, %ld output reports out, %ld GET, %ld SET\n", S.nin, S.nout, S.nget, S.nset);
out:
    cleanup();
    if (lsmc >= 0) close(lsmc);
    if (S.tcp > 0) close(S.tcp);
    rdx_usb_chan_destroy(&S.chan);
    return rc;
}
