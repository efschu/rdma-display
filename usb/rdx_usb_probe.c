/* SPDX-License-Identifier: MIT
 *
 * Test tool for the USB/HID passthrough (not needed in a session).
 *
 *   rdx_usb_probe fake [count 5000] [rate 1000] [start-delay-s 4]
 *       client: creates the synthetic HID device "rdx-hid-probe" via uhid
 *       (vendor usage page 0xFF00, 1209:0001, uniq "rdx-hid-probe", so it lists
 *       as hid-rdx-hid-probe; no keyboard/mouse usage, desktops ignore it),
 *       sends input reports and measures the round trip via the echoed output
 *       report: client uhid -> hidraw -> rdx_usb_client -> RDMA -> host uhid ->
 *       host hidraw -> echo -> back the same way.
 *   rdx_usb_probe echo [name-substring rdx-hid-probe] [count 5000]
 *       host: opens the mirror (phys "rdx:...") and writes every input report
 *       back as output report; exercises GET_REPORT (feature) once.
 *   rdx_usb_probe rdesc <hidraw> | --name SUBSTRING [--mirror]
 *       prints name, ids and an FNV-1a hash of the report descriptor, to compare
 *       a client device with its mirror on the host.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/uhid.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int uhid_write(int fd, struct uhid_event *ev)
{
    return write(fd, ev, sizeof(*ev)) == (ssize_t)sizeof(*ev) ? 0 : -1;
}

static const uint8_t FAKE_RD[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x08,
    0x09, 0x02, 0x81, 0x02, 0x09, 0x03, 0x91, 0x02, 0xC0};

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int run_fake(long count, double rate, double delay_s)
{
    int u = open("/dev/uhid", O_RDWR | O_CLOEXEC);
    if (u < 0) { perror("/dev/uhid"); return 1; }
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    strcpy((char *)ev.u.create2.name, "rdx-hid-probe");
    strcpy((char *)ev.u.create2.uniq, "rdx-hid-probe");
    ev.u.create2.rd_size = sizeof(FAKE_RD);
    memcpy(ev.u.create2.rd_data, FAKE_RD, sizeof(FAKE_RD));
    ev.u.create2.bus = BUS_VIRTUAL;
    ev.u.create2.vendor = 0x1209;   /* pid.codes test VID */
    ev.u.create2.product = 0x0001;
    if (uhid_write(u, &ev)) { perror("UHID_CREATE2"); return 1; }
    printf("[fake] rdx-hid-probe created, %ld reports at %.0f Hz after %.1f s\n", count, rate, delay_s);
    fflush(stdout);
    uint64_t *t_sent = calloc((size_t)count, sizeof(uint64_t));
    double *rtt = calloc((size_t)count, sizeof(double));
    long sent = 0, got = 0, getreq = 0, setreq = 0;
    uint64_t period = (uint64_t)(1e9 / rate), next = now_ns() + (uint64_t)(delay_s * 1e9), t_last = 0;
    while (got < count) {
        uint64_t now = now_ns();
        if (sent < count && now >= next) {
            memset(&ev, 0, sizeof(ev));
            ev.type = UHID_INPUT2;
            ev.u.input2.size = 8;
            uint32_t s = (uint32_t)sent;
            memcpy(ev.u.input2.data, &s, 4);
            t_sent[sent] = now_ns();
            if (uhid_write(u, &ev)) { perror("UHID_INPUT2"); return 1; }
            sent++;
            next += period;
            t_last = now;
        }
        if (sent == count && now - t_last > 2000000000ull) break;
        int tmo = sent < count ? (int)((next > now ? next - now : 0) / 1000000) : 100;
        struct pollfd p = {.fd = u, .events = POLLIN};
        if (poll(&p, 1, tmo) <= 0) continue;
        if (read(u, &ev, sizeof(ev)) <= 0) continue;
        if (ev.type == UHID_OUTPUT && ev.u.output.size >= 4) {
            uint32_t s;
            memcpy(&s, ev.u.output.data, 4);
            if (s < (uint32_t)count && t_sent[s] && got < count) rtt[got++] = (now_ns() - t_sent[s]) / 1e3;
        } else if (ev.type == UHID_GET_REPORT) {
            uint32_t id = ev.u.get_report.id;
            memset(&ev, 0, sizeof(ev));
            ev.type = UHID_GET_REPORT_REPLY;
            ev.u.get_report_reply.id = id;
            ev.u.get_report_reply.size = 8;
            memset(ev.u.get_report_reply.data, 0x42, 8);
            uhid_write(u, &ev);
            getreq++;
        } else if (ev.type == UHID_SET_REPORT) {
            uint32_t id = ev.u.set_report.id;
            int ok = ev.u.set_report.size == 9 && ev.u.set_report.data[1] == 0x5A;
            memset(&ev, 0, sizeof(ev));
            ev.type = UHID_SET_REPORT_REPLY;
            ev.u.set_report_reply.id = id;
            ev.u.set_report_reply.err = ok ? 0 : EIO;
            uhid_write(u, &ev);
            setreq += ok;
        }
    }
    qsort(rtt, (size_t)got, sizeof(double), cmp_d);
    if (got)
        printf("[fake] sent %ld, echoed %ld; full round trip: p50 %.1f us, p99 %.1f us, p99.9 %.1f us, max %.1f us; "
               "GET_REPORT answered %ld, SET_REPORT ok %ld\n", sent, got, rtt[got / 2], rtt[(size_t)(got * 0.99)],
               rtt[(size_t)(got * 0.999)], rtt[got - 1], getreq, setreq);
    else
        printf("[fake] sent %ld, nothing echoed\n", sent);
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_write(u, &ev);
    close(u);
    return got == count ? 0 : 3;
}

static void hid_uevent(const char *hidraw, char *name, char *phys, size_t n)
{
    char p[256], line[300];
    snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/uevent", hidraw);
    name[0] = phys[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "HID_NAME=", 9)) snprintf(name, n, "%s", line + 9);
        if (!strncmp(line, "HID_PHYS=", 9)) snprintf(phys, n, "%s", line + 9);
    }
    fclose(f);
}

/* hidraw node by name substring; mirror = 1: only rdx mirrors (phys "rdx:"), 0: only originals */
static int find_hidraw(const char *needle, int mirror, char *path, size_t len, int tries)
{
    for (int t = 0; t < tries; t++) {
        DIR *d = opendir("/sys/class/hidraw");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (strncmp(e->d_name, "hidraw", 6)) continue;
            char name[160], phys[160];
            hid_uevent(e->d_name, name, phys, sizeof(name));
            if (!strstr(name, needle) || (!strncmp(phys, "rdx:", 4)) != mirror) continue;
            snprintf(path, len, "/dev/%s", e->d_name);
            closedir(d);
            return 0;
        }
        if (d) closedir(d);
        usleep(10000);
    }
    return -1;
}

static int run_echo(const char *needle, long count)
{
    char path[64];
    if (find_hidraw(needle, 1, path, sizeof(path), 3000)) { fprintf(stderr, "no mirror named *%s*\n", needle); return 1; }
    int h = open(path, O_RDWR | O_CLOEXEC);
    if (h < 0) { perror(path); return 1; }
    printf("[echo] %s\n", path);
    fflush(stdout);
    uint8_t f[9] = {0};
    int g = ioctl(h, HIDIOCGFEATURE(sizeof(f)), f);
    printf("[echo] HIDIOCGFEATURE through the chain: %d%s\n", g, g < 0 ? " (failed)" : "");
    uint8_t sf[9] = {0, 0x5A, 1, 2, 3, 4, 5, 6, 7};
    int sr = ioctl(h, HIDIOCSFEATURE(sizeof(sf)), sf);
    printf("[echo] HIDIOCSFEATURE through the chain: %d%s\n", sr, sr < 0 ? " (failed)" : "");
    long n = 0;
    uint8_t buf[64];
    while (n < count) {
        struct pollfd p = {.fd = h, .events = POLLIN};
        if (poll(&p, 1, 15000) <= 0) break;
        ssize_t r = read(h, buf, sizeof(buf));
        if (r <= 0) break;
        if (write(h, buf, (size_t)r) < 0) { perror("hidraw write"); break; }
        n++;
    }
    printf("[echo] echoed %ld reports\n", n);
    return 0;
}

static int run_rdesc(const char *path)
{
    int h = open(path, O_RDONLY | O_CLOEXEC);
    if (h < 0) { perror(path); return 1; }
    int sz = 0;
    struct hidraw_report_descriptor rd;
    struct hidraw_devinfo info;
    char name[128] = "", phys[64] = "";
    if (ioctl(h, HIDIOCGRDESCSIZE, &sz) < 0) { perror("HIDIOCGRDESCSIZE"); return 1; }
    rd.size = (uint32_t)sz;
    ioctl(h, HIDIOCGRDESC, &rd);
    ioctl(h, HIDIOCGRAWINFO, &info);
    ioctl(h, HIDIOCGRAWNAME(sizeof(name) - 1), name);
    ioctl(h, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys);
    uint32_t fnv = 2166136261u;
    for (uint32_t i = 0; i < rd.size; i++) fnv = (fnv ^ rd.value[i]) * 16777619u;
    printf("[rdesc] %s \"%s\" phys \"%s\" bus %u %04x:%04x descriptor %u bytes fnv1a %08x\n", path, name, phys,
           info.bustype, (uint16_t)info.vendor, (uint16_t)info.product, rd.size, fnv);
    close(h);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "fake"))
        return run_fake(argc > 2 ? atol(argv[2]) : 5000, argc > 3 ? atof(argv[3]) : 1000, argc > 4 ? atof(argv[4]) : 4);
    if (argc >= 2 && !strcmp(argv[1], "echo"))
        return run_echo(argc > 2 ? argv[2] : "rdx-hid-probe", argc > 3 ? atol(argv[3]) : 5000);
    if (argc >= 3 && !strcmp(argv[1], "rdesc")) {
        if (strcmp(argv[2], "--name")) return run_rdesc(argv[2]);
        char path[64];
        int mirror = argc > 4 && !strcmp(argv[4], "--mirror");
        if (argc < 4 || find_hidraw(argv[3], mirror, path, sizeof(path), 300)) { fprintf(stderr, "not found\n"); return 1; }
        return run_rdesc(path);
    }
    fprintf(stderr, "usage: see header\n");
    return 2;
}
