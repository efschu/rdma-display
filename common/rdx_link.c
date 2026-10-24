/* SPDX-License-Identifier: MIT
 * Link health: RDMA port state and network module temperature, see rdx_link.h.
 */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* sysfs names and log lines: cut is fine */
#include "rdx_link.h"
#include "../third_party/cjson/cJSON.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define KMSG_KEEP_S   30.0      /* a module event explains a link-down this long after it */
#define HYST_C        1.0       /* a warning ends this far below its threshold */
#define EEPROM_EVERY_S 300.0

static struct {
    pthread_mutex_t mu;
    pthread_t th;
    int running, evfd[2], wake[2];
    char root[256];
    char want_ip[64];
    int retarget;
    double next_resolve;
    struct rdx_link_info li;
    /* the last module event of the kernel log for our PCI function */
    char kmsg_text[200];
    double kmsg_at;
    double eeprom_at;
    struct rdx_link_module ee;      /* from the EEPROM: thresholds, vendor, part (+ temperature) */
    int ee_ok;
    double last_up_temp;            /* module temperature at the last sample with the link up */
    /* the card (not the module): from the kernel log, for our PCI function */
    double fatal_at;                /* the driver reported a fatal error / lost PCI slot; latched until recovery */
    char fatal_text[200];
    double chip_event_at;           /* the firmware reported heat or a health error */
    char chip_event_text[120];
    double heat_at;                 /* last evidence of heat on the module or the chip */
    int chip_lvl, mod_lvl;          /* enum rdx_link_level of the chip / the module alone (hysteresis) */
    double nodev_since;             /* the RDMA device is missing since (card on the bus), 0 = no */
    char pci_seen[32];              /* the PCI function whose /sys/bus/pci/devices entry we have seen */
} L ={.mu = PTHREAD_MUTEX_INITIALIZER, .evfd = {-1, -1}, .wake = {-1, -1}};

static double now_unix(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void path(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void path(char *out, size_t n, const char *fmt, ...)
{
    char rel[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof(rel), fmt, ap);
    va_end(ap);
    snprintf(out, n, "%s%s", L.root, rel);
}

/* first line of a file, newline cut; -1 = unreadable */
static int read_line(const char *p, char *out, size_t n)
{
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static double read_milli(const char *p)
{
    char b[32];
    if (read_line(p, b, sizeof(b))) return NAN;
    char *e;
    long v = strtol(b, &e, 10);
    return e == b ? NAN : v / 1000.0;
}

const char *rdx_link_level_name(int level)
{
    switch (level) {
    case RDX_LINK_OK: return "ok";
    case RDX_LINK_WARN: return "warn";
    case RDX_LINK_ALARM: return "alarm";
    case RDX_LINK_DOWN: return "down";
    case RDX_LINK_FAILED: return "failed";
    default: return "unknown";
    }
}

static int level_of(const char *s)
{
    if (!s) return RDX_LINK_UNKNOWN;
    if (!strcmp(s, "ok")) return RDX_LINK_OK;
    if (!strcmp(s, "warn")) return RDX_LINK_WARN;
    if (!strcmp(s, "alarm")) return RDX_LINK_ALARM;
    if (!strcmp(s, "down")) return RDX_LINK_DOWN;
    if (!strcmp(s, "failed")) return RDX_LINK_FAILED;
    return RDX_LINK_UNKNOWN;
}

/* ---------------- which port ---------------- */

/* the RDMA device and port whose GID table holds ip (IPv4-mapped GID), with that GID's netdev */
static int resolve(const char *ip, struct rdx_link_info *li)
{
    struct in_addr a;
    if (!ip[0] || inet_pton(AF_INET, ip, &a) != 1) return -1;
    const unsigned char *b = (const unsigned char *)&a.s_addr;
    char want[64];
    snprintf(want, sizeof(want), "0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x", b[0], b[1], b[2], b[3]);
    char p[600];
    path(p, sizeof(p), "/sys/class/infiniband");
    DIR *d = opendir(p);
    struct dirent *de;
    int found = -1;
    while (d && found < 0 && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        for (int port = 1; port <= 2 && found < 0; port++) {
            char gd[700];
            snprintf(gd, sizeof(gd), "%s/%s/ports/%d/gids", p, de->d_name, port);
            if (access(gd, R_OK)) continue;                   /* no such port */
            for (int g = 0; g < 256 && found < 0; g++) {
                char gp[760], gid[80];
                snprintf(gp, sizeof(gp), "%s/%d", gd, g);
                if (read_line(gp, gid, sizeof(gid))) continue;   /* an empty entry (EINVAL) */
                if (strcmp(gid, want)) continue;
                snprintf(li->rdma_dev, sizeof(li->rdma_dev), "%s", de->d_name);
                li->port = port;
                snprintf(gp, sizeof(gp), "%s/%s/ports/%d/gid_attrs/ndevs/%d", p, de->d_name, port, g);
                char nd[32] = "";
                if (!read_line(gp, nd, sizeof(nd))) snprintf(li->netdev, sizeof(li->netdev), "%s", nd);
                found = 0;
            }
        }
    }
    if (d) closedir(d);
    if (found) return -1;
    /* the PCI function, and its own netdev when it is in this network namespace (not a macvlan's) */
    char lnk[600], tgt[600];
    snprintf(lnk, sizeof(lnk), "%s/%s/device", p, li->rdma_dev);
    ssize_t n = readlink(lnk, tgt, sizeof(tgt) - 1);
    li->pci[0] = 0;
    if (n > 0) {
        tgt[n] = 0;
        const char *base = strrchr(tgt, '/');
        snprintf(li->pci, sizeof(li->pci), "%s", base ? base + 1 : tgt);
    }
    char np[700];
    snprintf(np, sizeof(np), "%s/%s/device/net", p, li->rdma_dev);
    if ((d = opendir(np))) {
        while ((de = readdir(d)))
            if (de->d_name[0] != '.') { snprintf(li->netdev, sizeof(li->netdev), "%s", de->d_name); break; }
        closedir(d);
    }
    return 0;
}

/* ---------------- the module ---------------- */

/* hwmon of the port's PCI function: the "Module*" temperature (mlx5) */
static void hwmon_module(const struct rdx_link_info *li, struct rdx_link_module *m)
{
    char p[600];
    path(p, sizeof(p), "/sys/class/infiniband/%s/device/hwmon", li->rdma_dev);
    DIR *d = opendir(p);
    struct dirent *de;
    while (d && !m->present && (de = readdir(d))) {
        if (strncmp(de->d_name, "hwmon", 5)) continue;
        for (int t = 1; t <= 16 && !m->present; t++) {
            char lp[800], lab[32];
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_label", p, de->d_name, t);
            if (read_line(lp, lab, sizeof(lab)) || strncmp(lab, "Module", 6)) continue;
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_input", p, de->d_name, t);
            double v = read_milli(lp);
            if (isnan(v)) continue;
            m->present = 1;
            snprintf(m->label, sizeof(m->label), "%s", lab);
            m->temp_c = v;
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_crit", p, de->d_name, t);
            m->warn_c = read_milli(lp);               /* mlx5: the module's high warning threshold */
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_emergency", p, de->d_name, t);
            m->alarm_c = read_milli(lp);
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_highest", p, de->d_name, t);
            m->highest_c = read_milli(lp);
        }
    }
    if (d) closedir(d);
}

/* hwmon of the port's PCI function: the card's chip sensor (mlx5 on a ConnectX-5 and newer: label
 * "sensor0", crit = the limit the firmware warns at). A ConnectX-4 has no hwmon: chip stays absent. */
static void hwmon_chip(const struct rdx_link_info *li, struct rdx_link_chip *c)
{
    char p[600];
    path(p, sizeof(p), "/sys/class/infiniband/%s/device/hwmon", li->rdma_dev);
    DIR *d = opendir(p);
    struct dirent *de;
    while (d && !c->present && (de = readdir(d))) {
        if (strncmp(de->d_name, "hwmon", 5)) continue;
        for (int t = 1; t <= 16 && !c->present; t++) {
            char lp[800], lab[32];
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_label", p, de->d_name, t);
            if (read_line(lp, lab, sizeof(lab)) || strncmp(lab, "sensor", 6)) continue;
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_input", p, de->d_name, t);
            double v = read_milli(lp);
            if (isnan(v)) continue;
            c->present = 1;
            snprintf(c->label, sizeof(c->label), "%s", lab);
            c->temp_c = v;
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_crit", p, de->d_name, t);
            c->limit_c = read_milli(lp);
            snprintf(lp, sizeof(lp), "%s/%s/temp%d_highest", p, de->d_name, t);
            c->highest_c = read_milli(lp);
        }
    }
    if (d) closedir(d);
}

/* ---------------- is the card still there ---------------- */

enum { PCI_ALIVE = 0, PCI_GONE, PCI_DEAD };

/* the PCI function on the bus and answering config reads. The vendor attribute is cached by the
 * kernel and keeps its value on a dead card; the config file does a real read (all ones / EIO when
 * the card fell off the bus). A missing config file (a fake tree) is no verdict. */
static int pci_probe(const char *pci, char *why, size_t n)
{
    char p[600];
    path(p, sizeof(p), "/sys/bus/pci/devices/%s", pci);
    if (access(p, F_OK)) {
        /* gone only if it was ever visible here: a container may not show /sys/bus/pci at all */
        if (strcmp(L.pci_seen, pci)) return PCI_ALIVE;
        snprintf(why, n, "PCI device %s is gone from the bus", pci);
        return PCI_GONE;
    }
    snprintf(L.pci_seen, sizeof(L.pci_seen), "%s", pci);
    path(p, sizeof(p), "/sys/bus/pci/devices/%s/config", pci);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return PCI_ALIVE;
        snprintf(why, n, "PCI config space of %s cannot be opened (%s)", pci, strerror(errno));
        return PCI_DEAD;
    }
    unsigned char b[4] = {0};
    ssize_t r = read(fd, b, sizeof(b));
    int err = errno;
    close(fd);
    if (r < 2) {
        snprintf(why, n, "PCI config space of %s cannot be read (%s)", pci, r < 0 ? strerror(err) : "empty");
        return PCI_DEAD;
    }
    if (b[0] == 0xff && b[1] == 0xff) {
        snprintf(why, n, "PCI config space of %s reads all ones (\"PCI slot is unavailable\")", pci);
        return PCI_DEAD;
    }
    return PCI_ALIVE;
}

/* a Mellanox function on this machine that does not answer: when the link IP resolves to no port at
 * all (the daemon started after the card died, or its netdev vanished with it) this is the reason */
static int mellanox_dead(char *pci_out, size_t pn, char *why, size_t n)
{
    char p[600];
    path(p, sizeof(p), "/sys/bus/pci/devices");
    DIR *d = opendir(p);
    struct dirent *de;
    int dead = 0;
    char seen[sizeof(L.pci_seen)];
    snprintf(seen, sizeof(seen), "%s", L.pci_seen);           /* the scan is no sighting of our card */
    while (d && !dead && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char vp[800], v[16];
        snprintf(vp, sizeof(vp), "%s/%s/vendor", p, de->d_name);
        if (read_line(vp, v, sizeof(v)) || strtol(v, NULL, 16) != 0x15b3) continue;
        if (pci_probe(de->d_name, why, n) == PCI_DEAD) {
            snprintf(pci_out, pn, "%s", de->d_name);
            dead = 1;
        }
    }
    if (d) closedir(d);
    snprintf(L.pci_seen, sizeof(L.pci_seen), "%s", seen);
    return dead;
}

static double s16be_c(const unsigned char *p) { return (int16_t)(p[0] << 8 | p[1]) / 256.0; }

static void ascii(char *out, size_t n, const unsigned char *p, size_t len)
{
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < n; i++) out[k++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : ' ';
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
}

int rdx_link_parse_eeprom(unsigned type, const unsigned char *ee, size_t len, struct rdx_link_module *m, int temp_too)
{
    if (type == ETH_MODULE_SFF_8636 || type == ETH_MODULE_SFF_8436) {
        if (len < 256) return -1;
        ascii(m->vendor, sizeof(m->vendor), ee + 148, 16);
        ascii(m->part, sizeof(m->part), ee + 168, 16);
        /* a copper cable (transmitter technology >= 0xA) has no monitor: nothing but its name */
        if ((ee[147] >> 4) >= 0xA) return 0;
        /* page 03h (thresholds) only on paged memory; ethtool's dump puts it at 512 (lower, 00h, 01h, 02h, 03h) */
        if (len >= 640 && !(ee[2] & 0x04) && (ee[512] || ee[513])) {
            m->alarm_c = s16be_c(ee + 512 + 0);
            m->warn_c = s16be_c(ee + 512 + 4);
        }
        if (temp_too && (ee[22] || ee[23])) { m->temp_c = s16be_c(ee + 22); m->present = 1; }
        return 0;
    }
    if (type == ETH_MODULE_SFF_8472 || type == ETH_MODULE_SFF_8079) {
        if (len < 128) return -1;
        ascii(m->vendor, sizeof(m->vendor), ee + 20, 16);
        ascii(m->part, sizeof(m->part), ee + 40, 16);
        if (type == ETH_MODULE_SFF_8472 && len >= 512 && (ee[92] & 0x40)) {   /* A2h: diagnostics implemented */
            m->alarm_c = s16be_c(ee + 256 + 0);
            m->warn_c = s16be_c(ee + 256 + 4);
            if (temp_too) { m->temp_c = s16be_c(ee + 256 + 96); m->present = 1; }
        }
        return 0;
    }
    return -1;
}

/* the module EEPROM of netdev, as `ethtool -m` reads it (needs CAP_NET_ADMIN) */
static int eeprom_module(const char *netdev, struct rdx_link_module *m)
{
    if (!netdev[0] || L.root[0]) return -1;            /* tests: a fake tree has no ioctl */
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", netdev);
    struct ethtool_modinfo mi = {.cmd = ETHTOOL_GMODULEINFO};
    ifr.ifr_data = (void *)&mi;
    int r = -1;
    if (!ioctl(s, SIOCETHTOOL, &ifr) && mi.eeprom_len > 0 && mi.eeprom_len <= 1024) {
        struct ethtool_eeprom *ee = calloc(1, sizeof(*ee) + mi.eeprom_len);
        if (ee) {
            ee->cmd = ETHTOOL_GMODULEEEPROM;
            ee->len = mi.eeprom_len;
            ifr.ifr_data = (void *)ee;
            if (!ioctl(s, SIOCETHTOOL, &ifr)) r = rdx_link_parse_eeprom(mi.type, ee->data, ee->len, m, 1);
            free(ee);
        }
    }
    close(s);
    return r;
}

/* ---------------- why it went down ---------------- */

/* the module event text after "module N, " -> a reason in plain words */
static void reason_of_event(const char *ev, char *out, size_t n)
{
    if (strcasestr(ev, "High Temperature")) snprintf(out, n, "the network module overheated");
    else if (strcasestr(ev, "unplugged")) snprintf(out, n, "the network module or cable was unplugged");
    else if (strcasestr(ev, "Power budget")) snprintf(out, n, "the network module needs more power than the port gives");
    else if (strcasestr(ev, "shorted") || strcasestr(ev, "Bad or")) snprintf(out, n, "the network module or cable is faulty");
    else {
        const char *c = strcasestr(ev, "Cable error, ");
        snprintf(out, n, "network module error: %.80s", c ? c + 13 : ev);
    }
}

void rdx_link_kmsg_record(const char *rec)
{
    /* "6,1234,5678,-;mlx5_core 0000:01:00.1: port_module:252:(pid 0): Port module event[error]: module 1, ..." */
    const char *msg = strchr(rec, ';');
    msg = msg ? msg + 1 : rec;
    pthread_mutex_lock(&L.mu);
    if (!L.li.pci[0] || !strstr(msg, L.li.pci)) { pthread_mutex_unlock(&L.mu); return; }
    double now = now_unix();
    char line[200];
    snprintf(line, sizeof(line), "%.*s", (int)strcspn(msg, "\n"), msg);
    const char *ev = strstr(msg, "Port module event");
    if (ev && strstr(ev, "[error]")) {
        snprintf(L.kmsg_text, sizeof(L.kmsg_text), "%.*s", (int)strcspn(ev, "\n"), ev);
        L.kmsg_at = now;
        if (strcasestr(ev, "High Temperature")) L.heat_at = now;
        fprintf(stderr, "[link] kernel: %s\n", L.kmsg_text);
    } else if (strstr(msg, "High temperature on sensors")) {
        /* "temp_warn: High temperature on sensors with bit set <hi> <lo>": bit 0 of lo is the chip, the
         * hi bits are modules (the module's own hwmon level covers those) */
        unsigned long long hi = 0, lo = 0;
        const char *b = strstr(msg, "bit set ");
        if (b) sscanf(b + 8, "%llx %llx", &hi, &lo);
        L.heat_at = now;
        if (!b || (lo & 1)) {
            L.chip_event_at = now;
            snprintf(L.chip_event_text, sizeof(L.chip_event_text), "the kernel log reports a high chip temperature");
        }
        fprintf(stderr, "[link] kernel: %s\n", line);
    } else if ((ev = strstr(msg, "Health issue observed, "))) {
        char what[80];
        snprintf(what, sizeof(what), "%.*s", (int)strcspn(ev + 23, ",\n"), ev + 23);
        if (strcasestr(what, "temperature")) L.heat_at = now;
        L.chip_event_at = now;
        snprintf(L.chip_event_text, sizeof(L.chip_event_text), "the card's firmware reports: %s", what);
        fprintf(stderr, "[link] kernel: %s\n", line);
    } else if (strstr(msg, "health compromised")) {
        L.chip_event_at = now;
        snprintf(L.chip_event_text, sizeof(L.chip_event_text), "the card's firmware stopped answering");
        fprintf(stderr, "[link] kernel: %s\n", line);
    } else if ((strstr(msg, "Fatal error") && strstr(msg, "detected")) || strstr(msg, "PCI slot is unavailable") ||
               strstr(msg, "recovery flow aborted") || strstr(msg, "PCI reads still not working")) {
        if (L.fatal_at <= 0) L.fatal_at = now;       /* the first words stay: they come first, the rest follows */
        if (!L.fatal_text[0] || strstr(msg, "Fatal error"))
            snprintf(L.fatal_text, sizeof(L.fatal_text), "%s", line);
        fprintf(stderr, "[link] kernel: %s\n", line);
    } else if (strstr(msg, "recovery succeeded") || strstr(msg, ": Link up")) {
        L.fatal_at = 0;                              /* the card is back */
        L.fatal_text[0] = 0;
    }
    pthread_mutex_unlock(&L.mu);
}

/* ---------------- one sample ---------------- */

static void notify(void)
{
    if (L.evfd[1] >= 0) {
        char c = 1;
        if (write(L.evfd[1], &c, 1) < 0) { /* full: a change is pending anyway */ }
    }
}

static void fmt_temp(char *out, size_t n, const struct rdx_link_module *m)
{
    if (!m->present) { out[0] = 0; return; }
    if (!isnan(m->warn_c)) snprintf(out, n, " (%.1f \xc2\xb0""C, warning at %.0f \xc2\xb0""C)", m->temp_c, m->warn_c);
    else snprintf(out, n, " (%.1f \xc2\xb0""C)", m->temp_c);
}

/* the chip's level from its temperature and the kernel log's heat reports; fills c->event */
static int chip_level(struct rdx_link_chip *c, int old_lvl, double now)
{
    c->event = L.chip_event_at > 0 && now - L.chip_event_at < RDX_LINK_CHIP_EVENT_S;
    snprintf(c->event_text, sizeof(c->event_text), "%s", c->event ? L.chip_event_text : "");
    int lvl = RDX_LINK_OK;
    if (c->present && !isnan(c->limit_c)) {
        double hw = old_lvl == RDX_LINK_WARN || old_lvl == RDX_LINK_ALARM ? HYST_C : 0;
        double ha = old_lvl == RDX_LINK_ALARM ? HYST_C : 0;
        if (c->temp_c >= c->limit_c - ha) lvl = RDX_LINK_ALARM;
        else if (c->temp_c >= c->limit_c - RDX_LINK_CHIP_MARGIN_C - hw) lvl = RDX_LINK_WARN;
    }
    if (c->event) lvl = RDX_LINK_ALARM;
    return lvl;
}

static void chip_message(char *out, size_t n, const struct rdx_link_chip *c, int lvl)
{
    if (lvl == RDX_LINK_ALARM && c->present && !isnan(c->limit_c) && c->temp_c >= c->limit_c - HYST_C)
        snprintf(out, n, "Network card chip %.1f \xc2\xb0""C \xe2\x80\x93 at its limit of %.0f \xc2\xb0""C, the card may fail",
                 c->temp_c, c->limit_c);
    else if (lvl == RDX_LINK_ALARM && c->present)
        snprintf(out, n, "Network card chip %.1f \xc2\xb0""C \xe2\x80\x93 %s, the card may fail", c->temp_c, c->event_text);
    else if (lvl == RDX_LINK_ALARM)
        snprintf(out, n, "Network card too hot \xe2\x80\x93 %s, the card may fail: improve its cooling", c->event_text);
    else
        snprintf(out, n, "Network card chip %.1f \xc2\xb0""C \xe2\x80\x93 close to its limit of %.0f \xc2\xb0""C", c->temp_c, c->limit_c);
}

enum { NIC_OK = 0, NIC_GONE, NIC_DEAD, NIC_FATAL, NIC_NODEV };

/* is the card of li still there? why = what was seen. Writes li->pci when it finds a dead Mellanox
 * function while no port carries the link IP */
static int nic_state(struct rdx_link_info *li, double now, char *why, size_t n)
{
    why[0] = 0;
    if (!li->known && li->link_ip[0] && mellanox_dead(li->pci, sizeof(li->pci), why, n))
        return NIC_DEAD;                             /* no port carries link_ip and a Mellanox card is dead */
    if (!li->pci[0]) return NIC_OK;
    int pr = pci_probe(li->pci, why, n);
    if (pr == PCI_GONE) return NIC_GONE;
    if (pr == PCI_DEAD) return NIC_DEAD;
    char p[600];
    if (li->rdma_dev[0]) {
        path(p, sizeof(p), "/sys/class/infiniband/%s", li->rdma_dev);
        if (access(p, F_OK)) {                       /* on the bus, but its RDMA device is gone */
            if (L.nodev_since <= 0) L.nodev_since = now;
            const char *e = getenv("RDX_LINK_NODEV_S");     /* tests: no 30 s wait */
            if (L.fatal_at <= 0 && now - L.nodev_since >= (e ? atof(e) : RDX_LINK_NODEV_S)) {
                snprintf(why, n, "RDMA device %s has been missing for %.0f s although %s is on the bus", li->rdma_dev,
                         now - L.nodev_since, li->pci);
                return NIC_NODEV;
            }
        } else {
            L.nodev_since = 0;
        }
    }
    if (L.fatal_at > 0) {
        char st[48] = "";
        path(p, sizeof(p), "/sys/class/infiniband/%s/ports/%d/state", li->rdma_dev, li->port);
        if (li->rdma_dev[0] && now - L.fatal_at > 60 && !read_line(p, st, sizeof(st)) && strstr(st, "ACTIVE")) {
            L.fatal_at = 0;                          /* answering again for a minute and the port is active */
            L.fatal_text[0] = 0;
        } else {
            snprintf(why, n, "%s", L.fatal_text[0] ? L.fatal_text : "the driver reported a fatal error");
            return NIC_FATAL;
        }
    }
    return NIC_OK;
}

/* the card is gone: level failed, with the words and the bookkeeping of a link down */
static void set_failed(struct rdx_link_info *li, double now, int kind, const char *seen)
{
    int heat = L.heat_at > 0 && now - L.heat_at < RDX_LINK_HEAT_EVIDENCE_S;
    const char *why = kind == NIC_GONE ? "PCI device gone from the bus" : kind == NIC_DEAD ? "PCI device not answering"
                    : kind == NIC_FATAL ? "fatal error reported by the driver" : "RDMA device vanished";
    int cycle = kind != NIC_NODEV;
    if (li->level != RDX_LINK_FAILED) {
        if (li->up || !li->have_down || li->back_at > 0) li->downs++;
        if (!(li->have_down && li->back_at == 0)) li->down_at = now;       /* an outage already running keeps its start */
        li->have_down = 1;
        li->back_at = 0;
        li->down_temp_c = NAN;
        li->down_detail[0] = 0;
        fprintf(stderr, "[link] %s: the network card failed (%s): %s\n", li->pci, why, seen);
    }
    snprintf(li->down_reason, sizeof(li->down_reason), "%s",
             !cycle ? "the network card disappeared" : heat ? "the network card overheated" : "the network card failed");
    snprintf(li->down_detail, sizeof(li->down_detail), "%s", seen);
    li->nic_failed = 1;
    li->nic_heat = heat && cycle;
    li->nic_power_cycle = cycle;
    snprintf(li->nic_why, sizeof(li->nic_why), "%s", why);
    snprintf(li->nic_detail, sizeof(li->nic_detail), "%s", seen);
    li->up = 0;
    li->known = 1;                                   /* "known" gates the tray and the pages: this is news */
    snprintf(li->state, sizeof(li->state), "card failed");
    li->phys_state[0] = 0;
    if (cycle)
        snprintf(li->message, sizeof(li->message), "Network card failed (%s): power-cycle needed",
                 li->nic_heat ? "likely overheated" : why);
    else
        snprintf(li->message, sizeof(li->message), "Network card not available (%s): reload the driver or power-cycle", why);
    li->level = RDX_LINK_FAILED;
    li->hot = RDX_HOT_NONE;
}

void rdx_link_sample_once(void)
{
    pthread_mutex_lock(&L.mu);
    struct rdx_link_info *li = &L.li;
    double now = now_unix();
    if (L.retarget) {
        L.retarget = 0;
        snprintf(li->link_ip, sizeof(li->link_ip), "%s", L.want_ip);
        li->known = 0;
        li->pci[0] = li->rdma_dev[0] = 0;
        L.next_resolve = 0;
        L.ee_ok = 0;
        L.eeprom_at = 0;
        L.fatal_at = L.nodev_since = 0;
    }
    int old_level = li->level, old_present = li->mod.present;
    long old_downs = li->downs;
    char seen[200];
    int nic = nic_state(li, now, seen, sizeof(seen));
    if (nic != NIC_OK) {                             /* the card itself is gone: no reconnect will help */
        set_failed(li, now, nic, seen);
        goto done;
    }
    if (li->nic_failed) {                            /* answering again */
        li->nic_failed = li->nic_heat = li->nic_power_cycle = 0;
        li->nic_why[0] = li->nic_detail[0] = 0;
        if (!li->rdma_dev[0]) { li->known = 0; L.next_resolve = 0; }
        fprintf(stderr, "[link] %s: the network card answers again\n", li->pci);
    }
    if (!li->known && now >= L.next_resolve) {
        struct rdx_link_info t;
        memset(&t, 0, sizeof(t));
        if (!resolve(li->link_ip, &t)) {
            snprintf(li->rdma_dev, sizeof(li->rdma_dev), "%s", t.rdma_dev);
            snprintf(li->pci, sizeof(li->pci), "%s", t.pci);
            snprintf(li->netdev, sizeof(li->netdev), "%s", t.netdev);
            li->port = t.port;
            li->known = 1;
            fprintf(stderr, "[link] %s is %s port %d (%s, %s)\n", li->link_ip, li->rdma_dev, li->port,
                    li->pci[0] ? li->pci : "no PCI", li->netdev[0] ? li->netdev : "no netdev");
        }
        L.next_resolve = now + 10;
    }
    if (!li->known) {
        li->level = RDX_LINK_UNKNOWN;
        li->up = 0;
        if (li->link_ip[0]) snprintf(li->message, sizeof(li->message), "No RDMA port carries %s", li->link_ip);
        else li->message[0] = 0;
        goto done;
    }
    char p[600], st[48] = "", ps[48] = "";
    path(p, sizeof(p), "/sys/class/infiniband/%s/ports/%d/state", li->rdma_dev, li->port);
    if (read_line(p, st, sizeof(st))) {              /* the device went away (driver reload, unplug) */
        li->known = 0;
        L.next_resolve = 0;
        li->level = RDX_LINK_UNKNOWN;
        li->up = 0;
        snprintf(li->message, sizeof(li->message), "RDMA device %s does not answer (%s)", li->rdma_dev, strerror(errno));
        goto done;
    }
    path(p, sizeof(p), "/sys/class/infiniband/%s/ports/%d/phys_state", li->rdma_dev, li->port);
    read_line(p, ps, sizeof(ps));
    path(p, sizeof(p), "/sys/class/infiniband/%s/ports/%d/rate", li->rdma_dev, li->port);
    if (read_line(p, li->rate, sizeof(li->rate))) li->rate[0] = 0;
    const char *sv = strchr(st, ':'), *pv = strchr(ps, ':');
    snprintf(li->state, sizeof(li->state), "%s", sv ? sv + 2 : st);
    snprintf(li->phys_state, sizeof(li->phys_state), "%s", pv ? pv + 2 : ps);
    int up = !strcmp(li->state, "ACTIVE");

    /* the module: hwmon (cheap) every sample, the EEPROM now and then and after a link change */
    struct rdx_link_module m;
    memset(&m, 0, sizeof(m));
    m.temp_c = m.warn_c = m.alarm_c = m.highest_c = NAN;
    hwmon_module(li, &m);
    if (now - L.eeprom_at > EEPROM_EVERY_S || up != li->up) {
        struct rdx_link_module e;
        memset(&e, 0, sizeof(e));
        e.temp_c = e.warn_c = e.alarm_c = e.highest_c = NAN;
        L.ee_ok = !eeprom_module(li->netdev, &e);
        if (L.ee_ok) L.ee = e;
        L.eeprom_at = now;
    }
    if (L.ee_ok) {
        if (!m.present && L.ee.present && up) { m.present = 1; m.temp_c = L.ee.temp_c; }   /* no hwmon */
        if (isnan(m.warn_c)) m.warn_c = L.ee.warn_c;
        if (isnan(m.alarm_c)) m.alarm_c = L.ee.alarm_c;
        snprintf(m.vendor, sizeof(m.vendor), "%s", L.ee.vendor);
        snprintf(m.part, sizeof(m.part), "%s", L.ee.part);
    }
    if (!up && !m.present) m = li->mod;                /* keep the last known values while down */
    li->mod = m;

    /* the card's chip: hwmon where the driver has it, the kernel log's heat reports everywhere */
    struct rdx_link_chip c;
    memset(&c, 0, sizeof(c));
    c.temp_c = c.limit_c = c.highest_c = NAN;
    hwmon_chip(li, &c);
    if (!up && !c.present) {
        c.present = li->chip.present;
        c.temp_c = li->chip.temp_c;
        c.limit_c = li->chip.limit_c;
        c.highest_c = li->chip.highest_c;
        snprintf(c.label, sizeof(c.label), "%s", li->chip.label);
    }
    int clvl = chip_level(&c, L.chip_lvl, now);
    L.chip_lvl = clvl;
    li->chip = c;
    if (clvl >= RDX_LINK_WARN) L.heat_at = now;

    if (!up && li->level == RDX_LINK_DOWN && li->back_at == 0 && !li->down_detail[0] && L.kmsg_text[0] &&
        L.kmsg_at >= li->down_at - KMSG_KEEP_S) {
        /* the module event was read only after the sample that saw the port down */
        reason_of_event(L.kmsg_text, li->down_reason, sizeof(li->down_reason));
        snprintf(li->down_detail, sizeof(li->down_detail), "%s", L.kmsg_text);
        old_level = RDX_LINK_UNKNOWN;                /* tell the daemons: the reason changed */
    }
    if (!up) {
        if (li->up || !li->have_down || li->back_at > 0 || (li->level != RDX_LINK_DOWN && li->level != RDX_LINK_FAILED)) {
            /* went down now (or was down at the start): why. (From "failed" the outage goes on.) */
            li->downs += li->up ? 1 : 0;
            li->have_down = 1;
            li->down_at = now;
            li->back_at = 0;
            li->down_temp_c = !isnan(L.last_up_temp) ? L.last_up_temp : (m.present ? m.temp_c : NAN);
            li->down_detail[0] = 0;
            if (L.kmsg_text[0] && now - L.kmsg_at < KMSG_KEEP_S) {
                reason_of_event(L.kmsg_text, li->down_reason, sizeof(li->down_reason));
                snprintf(li->down_detail, sizeof(li->down_detail), "%s", L.kmsg_text);
            } else if (!isnan(li->down_temp_c) && !isnan(m.warn_c) && li->down_temp_c >= m.warn_c) {
                snprintf(li->down_reason, sizeof(li->down_reason), "the network module overheated");
            } else if (clvl == RDX_LINK_ALARM) {
                snprintf(li->down_reason, sizeof(li->down_reason), "the network card overheated");
                if (li->chip.event) snprintf(li->down_detail, sizeof(li->down_detail), "%s", li->chip.event_text);
            } else {
                snprintf(li->down_reason, sizeof(li->down_reason), "no signal from the other side (its computer, module or the cable)");
            }
            fprintf(stderr, "[link] %s port %d DOWN (%s): %s%s%s\n", li->rdma_dev, li->port, li->phys_state,
                    li->down_reason, li->down_detail[0] ? " - " : "", li->down_detail);
        }
        li->level = RDX_LINK_DOWN;
        li->hot = RDX_HOT_NONE;
        L.mod_lvl = RDX_LINK_OK;
        char t[64] = "";
        if (!isnan(li->down_temp_c) && strstr(li->down_reason, "network module overheat")) {
            if (!isnan(m.warn_c)) snprintf(t, sizeof(t), " (%.1f \xc2\xb0""C, warning at %.0f \xc2\xb0""C)", li->down_temp_c, m.warn_c);
            else snprintf(t, sizeof(t), " (%.1f \xc2\xb0""C)", li->down_temp_c);
        }
        snprintf(li->message, sizeof(li->message), "Link down: %s%s", li->down_reason, t);
    } else {
        if (!li->up && li->have_down && li->back_at == 0 && (li->level == RDX_LINK_DOWN || li->level == RDX_LINK_FAILED)) {
            li->back_at = now;
            fprintf(stderr, "[link] %s port %d back after %.0f s\n", li->rdma_dev, li->port, now - li->down_at);
        }
        L.last_up_temp = m.present ? m.temp_c : NAN;
        int lvl = RDX_LINK_OK;
        if (m.present) {
            double hw = L.mod_lvl == RDX_LINK_WARN || L.mod_lvl == RDX_LINK_ALARM ? HYST_C : 0;
            double ha = L.mod_lvl == RDX_LINK_ALARM ? HYST_C : 0;
            if (!isnan(m.alarm_c) && m.temp_c >= m.alarm_c - ha) lvl = RDX_LINK_ALARM;
            else if (!isnan(m.warn_c) && m.temp_c >= m.warn_c - hw) lvl = RDX_LINK_WARN;
        }
        L.mod_lvl = lvl;
        if (lvl >= RDX_LINK_WARN) L.heat_at = now;
        li->hot = lvl > RDX_LINK_OK ? RDX_HOT_MODULE : RDX_HOT_NONE;
        if (clvl > lvl) {                            /* the card's chip is the worse news */
            chip_message(li->message, sizeof(li->message), &li->chip, clvl);
            li->hot = RDX_HOT_CHIP;
            li->level = clvl;
        } else {
            li->level = lvl;
        }
        if (li->hot == RDX_HOT_CHIP) {
            /* message already written */
        } else if (lvl == RDX_LINK_ALARM)
            snprintf(li->message, sizeof(li->message),
                     "Network module %.1f \xc2\xb0""C \xe2\x80\x93 at its alarm threshold of %.0f \xc2\xb0""C, the link may go down any moment",
                     m.temp_c, m.alarm_c);
        else if (lvl == RDX_LINK_WARN && !isnan(m.alarm_c))
            snprintf(li->message, sizeof(li->message),
                     "Network module %.1f \xc2\xb0""C \xe2\x80\x93 warning at %.0f \xc2\xb0""C, the link goes down at %.0f \xc2\xb0""C",
                     m.temp_c, m.warn_c, m.alarm_c);
        else if (lvl == RDX_LINK_WARN)
            snprintf(li->message, sizeof(li->message), "Network module %.1f \xc2\xb0""C \xe2\x80\x93 above its warning threshold of %.0f \xc2\xb0""C",
                     m.temp_c, m.warn_c);
        else li->message[0] = 0;
    }
    li->up = up;
done:
    if (li->level != old_level || li->mod.present != old_present || li->downs != old_downs) {
        li->since = now;
        if (li->level != old_level && li->level != RDX_LINK_DOWN)
            fprintf(stderr, "[link] %s: %s%s%s\n", li->link_ip, rdx_link_level_name(li->level), li->message[0] ? " - " : "",
                    li->message);
        notify();
    }
    pthread_mutex_unlock(&L.mu);
}

/* ---------------- the thread ---------------- */

static void *thread_main(void *arg)
{
    (void)arg;
    char kp[300];
    snprintf(kp, sizeof(kp), "%s/dev/kmsg", L.root);
    int kfd = L.root[0] ? -1 : open(kp, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kfd >= 0) lseek(kfd, 0, SEEK_END);               /* only what happens from now on */
    else if (!L.root[0]) fprintf(stderr, "[link] kernel log not readable (%s): link-down reasons from the temperature only\n", strerror(errno));
    struct timespec t0;
    for (;;) {
        rdx_link_sample_once();
        clock_gettime(CLOCK_MONOTONIC, &t0);
        double end = t0.tv_sec + t0.tv_nsec / 1e9 + RDX_LINK_SAMPLE_MS / 1000.0;
        for (;;) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            double left = end - (t.tv_sec + t.tv_nsec / 1e9);
            if (left <= 0) break;
            struct pollfd pf[2] = {{kfd, POLLIN, 0}, {L.wake[0], POLLIN, 0}};
            int r = poll(pf, 2, (int)(left * 1000) + 1);
            if (r < 0 && errno != EINTR) break;
            if (pf[1].revents) {
                char b[16];
                while (read(L.wake[0], b, sizeof(b)) > 0) {}
                break;                                   /* retarget: sample now */
            }
            if (kfd >= 0 && (pf[0].revents & POLLIN)) {
                char rec[2048];
                ssize_t n;
                while ((n = read(kfd, rec, sizeof(rec) - 1)) > 0 || (n < 0 && errno == EPIPE)) {
                    if (n <= 0) continue;                /* EPIPE: records were overwritten, go on */
                    rec[n] = 0;
                    rdx_link_kmsg_record(rec);
                }
            }
        }
    }
    return NULL;
}

int rdx_link_start(const char *link_ip, const char *sysroot)
{
    if (L.running) { rdx_link_target(link_ip); return 0; }
    snprintf(L.root, sizeof(L.root), "%s", sysroot && strcmp(sysroot, "/") ? sysroot : "");
    if (pipe2(L.evfd, O_CLOEXEC | O_NONBLOCK) || pipe2(L.wake, O_CLOEXEC | O_NONBLOCK)) return -1;
    memset(&L.li, 0, sizeof(L.li));
    L.li.mod.temp_c = L.li.mod.warn_c = L.li.mod.alarm_c = L.li.mod.highest_c = NAN;
    L.li.down_temp_c = NAN;
    L.last_up_temp = NAN;
    snprintf(L.want_ip, sizeof(L.want_ip), "%s", link_ip ? link_ip : "");
    L.retarget = 1;
    L.running = 1;
    if (sysroot && strcmp(sysroot, "/")) return 0;      /* tests drive rdx_link_sample_once() themselves */
    if (pthread_create(&L.th, NULL, thread_main, NULL)) { L.running = 0; return -1; }
    pthread_detach(L.th);
    return 0;
}

void rdx_link_target(const char *link_ip)
{
    pthread_mutex_lock(&L.mu);
    if (strcmp(L.want_ip, link_ip ? link_ip : "")) {
        snprintf(L.want_ip, sizeof(L.want_ip), "%s", link_ip ? link_ip : "");
        L.retarget = 1;
        if (L.wake[1] >= 0 && write(L.wake[1], "w", 1) < 0) { /* the next sample takes it */ }
    }
    pthread_mutex_unlock(&L.mu);
}

int rdx_link_event_fd(void) { return L.evfd[0]; }

int rdx_link_take(void)
{
    char b[64];
    int any = 0;
    while (L.evfd[0] >= 0 && read(L.evfd[0], b, sizeof(b)) > 0) any = 1;
    return any;
}

void rdx_link_get(struct rdx_link_info *out)
{
    pthread_mutex_lock(&L.mu);
    *out = L.li;
    pthread_mutex_unlock(&L.mu);
}

/* ---------------- JSON ---------------- */

static void num_or_null(cJSON *o, const char *k, double v)
{
    if (isnan(v)) cJSON_AddNullToObject(o, k);
    else cJSON_AddNumberToObject(o, k, round(v * 10) / 10);
}

cJSON *rdx_link_json(const struct rdx_link_info *li)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "known", li->known);
    cJSON_AddStringToObject(o, "link_ip", li->link_ip);
    cJSON_AddStringToObject(o, "rdma_dev", li->rdma_dev);
    cJSON_AddNumberToObject(o, "port", li->port);
    cJSON_AddStringToObject(o, "pci", li->pci);
    cJSON_AddStringToObject(o, "netdev", li->netdev);
    cJSON_AddBoolToObject(o, "up", li->up);
    cJSON_AddStringToObject(o, "state", li->state);
    cJSON_AddStringToObject(o, "phys_state", li->phys_state);
    cJSON_AddStringToObject(o, "rate", li->rate);
    if (li->mod.present || li->mod.vendor[0]) {
        cJSON *m = cJSON_AddObjectToObject(o, "module");
        cJSON_AddStringToObject(m, "label", li->mod.label);
        num_or_null(m, "temp_c", li->mod.present ? li->mod.temp_c : NAN);
        num_or_null(m, "warn_c", li->mod.warn_c);
        num_or_null(m, "alarm_c", li->mod.alarm_c);
        num_or_null(m, "highest_c", li->mod.highest_c);
        cJSON_AddStringToObject(m, "vendor", li->mod.vendor);
        cJSON_AddStringToObject(m, "part", li->mod.part);
    } else {
        cJSON_AddNullToObject(o, "module");
    }
    if (li->chip.present || li->chip.event) {
        cJSON *c = cJSON_AddObjectToObject(o, "chip");
        cJSON_AddStringToObject(c, "label", li->chip.label);
        num_or_null(c, "temp_c", li->chip.present ? li->chip.temp_c : NAN);
        num_or_null(c, "limit_c", li->chip.limit_c);
        num_or_null(c, "highest_c", li->chip.highest_c);
        cJSON_AddBoolToObject(c, "event", li->chip.event);
        cJSON_AddStringToObject(c, "event_text", li->chip.event_text);
    } else {
        cJSON_AddNullToObject(o, "chip");
    }
    if (li->nic_failed) {
        cJSON *f = cJSON_AddObjectToObject(o, "nic");
        cJSON_AddBoolToObject(f, "failed", 1);
        cJSON_AddBoolToObject(f, "likely_overheated", li->nic_heat);
        cJSON_AddBoolToObject(f, "power_cycle", li->nic_power_cycle);
        cJSON_AddStringToObject(f, "why", li->nic_why);
        cJSON_AddStringToObject(f, "detail", li->nic_detail);
    } else {
        cJSON_AddNullToObject(o, "nic");
    }
    if (li->hot) cJSON_AddStringToObject(o, "hot", li->hot == RDX_HOT_CHIP ? "chip" : "module");
    else cJSON_AddNullToObject(o, "hot");
    cJSON_AddStringToObject(o, "level", rdx_link_level_name(li->level));
    cJSON_AddStringToObject(o, "message", li->message);
    cJSON_AddNumberToObject(o, "downs", (double)li->downs);
    if (li->have_down) {
        cJSON *d = cJSON_AddObjectToObject(o, "last_down");
        cJSON_AddNumberToObject(d, "at", floor(li->down_at));
        cJSON_AddStringToObject(d, "reason", li->down_reason);
        cJSON_AddStringToObject(d, "detail", li->down_detail);
        num_or_null(d, "temp_c", li->down_temp_c);
        if (li->back_at > 0) {
            cJSON_AddNumberToObject(d, "back_at", floor(li->back_at));
            cJSON_AddNumberToObject(d, "down_s", floor(li->back_at - li->down_at + 0.5));
        } else {
            cJSON_AddNullToObject(d, "back_at");
            cJSON_AddNullToObject(d, "down_s");
        }
    } else {
        cJSON_AddNullToObject(o, "last_down");
    }
    cJSON_AddNumberToObject(o, "since", floor(li->since));
    return o;
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static void jstr(const cJSON *o, const char *k, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    snprintf(out, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

int rdx_link_from_json(const cJSON *j, struct rdx_link_info *out)
{
    memset(out, 0, sizeof(*out));
    out->mod.temp_c = out->mod.warn_c = out->mod.alarm_c = out->mod.highest_c = NAN;
    out->down_temp_c = NAN;
    if (!cJSON_IsObject(j)) return -1;
    out->known = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "known"));
    out->up = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "up"));
    jstr(j, "link_ip", out->link_ip, sizeof(out->link_ip));
    jstr(j, "rdma_dev", out->rdma_dev, sizeof(out->rdma_dev));
    jstr(j, "state", out->state, sizeof(out->state));
    jstr(j, "message", out->message, sizeof(out->message));
    char lv[16];
    jstr(j, "level", lv, sizeof(lv));
    out->level = level_of(lv);
    double d = jnum(j, "downs");
    out->downs = isnan(d) ? 0 : (long)d;
    out->since = jnum(j, "since");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "module");
    if (cJSON_IsObject(m)) {
        out->mod.temp_c = jnum(m, "temp_c");
        out->mod.present = !isnan(out->mod.temp_c);
        out->mod.warn_c = jnum(m, "warn_c");
        out->mod.alarm_c = jnum(m, "alarm_c");
        out->mod.highest_c = jnum(m, "highest_c");
        jstr(m, "label", out->mod.label, sizeof(out->mod.label));
        jstr(m, "vendor", out->mod.vendor, sizeof(out->mod.vendor));
        jstr(m, "part", out->mod.part, sizeof(out->mod.part));
    }
    out->chip.temp_c = out->chip.limit_c = out->chip.highest_c = NAN;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "chip");
    if (cJSON_IsObject(c)) {
        out->chip.temp_c = jnum(c, "temp_c");
        out->chip.present = !isnan(out->chip.temp_c);
        out->chip.limit_c = jnum(c, "limit_c");
        out->chip.highest_c = jnum(c, "highest_c");
        out->chip.event = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "event"));
        jstr(c, "label", out->chip.label, sizeof(out->chip.label));
        jstr(c, "event_text", out->chip.event_text, sizeof(out->chip.event_text));
    }
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(j, "nic");
    if (cJSON_IsObject(f)) {
        out->nic_failed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "failed"));
        out->nic_heat = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "likely_overheated"));
        out->nic_power_cycle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "power_cycle"));
        jstr(f, "why", out->nic_why, sizeof(out->nic_why));
        jstr(f, "detail", out->nic_detail, sizeof(out->nic_detail));
    }
    char hot[16];
    jstr(j, "hot", hot, sizeof(hot));
    out->hot = !strcmp(hot, "chip") ? RDX_HOT_CHIP : !strcmp(hot, "module") ? RDX_HOT_MODULE : RDX_HOT_NONE;
    const cJSON *ld = cJSON_GetObjectItemCaseSensitive(j, "last_down");
    if (cJSON_IsObject(ld)) {
        out->have_down = 1;
        out->down_at = jnum(ld, "at");
        double b = jnum(ld, "back_at");
        out->back_at = isnan(b) ? 0 : b;
        out->down_temp_c = jnum(ld, "temp_c");
        jstr(ld, "reason", out->down_reason, sizeof(out->down_reason));
        jstr(ld, "detail", out->down_detail, sizeof(out->down_detail));
    }
    return 0;
}

/* "the network module overheated" -> "the other computer's network module overheated"; the same for the card */
static void whose(char *out, size_t n, const char *reason, const char *owner)
{
    static const char *const what[] = {"the network module", "the network card"};
    for (size_t i = 0; i < sizeof(what) / sizeof(what[0]); i++)
        if (!strncmp(reason, what[i], strlen(what[i]))) {
            snprintf(out, n, "%s%s", owner, reason + 3);     /* "the " is replaced by the owner */
            return;
        }
    snprintf(out, n, "%s", reason);
}

/* "<owner> network card failed (likely overheated): power-cycle needed" */
static void failed_line(char *out, size_t n, const struct rdx_link_info *l, const char *owner)
{
    if (l->nic_power_cycle || !l->nic_why[0])
        snprintf(out, n, "%s network card failed (%s): power-cycle needed", owner,
                 l->nic_heat ? "likely overheated" : l->nic_why[0] ? l->nic_why : "not answering");
    else
        snprintf(out, n, "%s network card is not available (%s): reload the driver or power-cycle", owner, l->nic_why);
}

/* a warn/alarm level, whichever part is hot */
static void hot_line(char *out, size_t n, const struct rdx_link_info *h, const char *who)
{
    if (h->hot == RDX_HOT_CHIP) {
        int at_limit = h->chip.present && !isnan(h->chip.limit_c) && h->chip.temp_c >= h->chip.limit_c - 1.0;
        if (h->level != RDX_LINK_ALARM)
            snprintf(out, n, "%s network card chip: %.1f \xc2\xb0""C (limit %.0f \xc2\xb0""C)", who, h->chip.temp_c, h->chip.limit_c);
        else if (at_limit)
            snprintf(out, n, "%s network card chip: %.1f \xc2\xb0""C, at its limit \xe2\x80\x93 the card may fail", who, h->chip.temp_c);
        else if (h->chip.present)
            snprintf(out, n, "%s network card is too hot (%.1f \xc2\xb0""C) \xe2\x80\x93 it may fail: improve its cooling", who, h->chip.temp_c);
        else
            snprintf(out, n, "%s network card is too hot \xe2\x80\x93 it may fail: improve its cooling", who);
        return;
    }
    if (h->level == RDX_LINK_ALARM)
        snprintf(out, n, "%s network module: %.1f \xc2\xb0""C, at its alarm threshold \xe2\x80\x93 the link may go down",
                 who, h->mod.temp_c);
    else if (!isnan(h->mod.alarm_c))
        snprintf(out, n, "%s network module: %.1f \xc2\xb0""C (warning at %.0f, off at %.0f \xc2\xb0""C)",
                 who, h->mod.temp_c, h->mod.warn_c, h->mod.alarm_c);
    else
        snprintf(out, n, "%s network module: %.1f \xc2\xb0""C (warning at %.0f \xc2\xb0""C)", who, h->mod.temp_c,
                 h->mod.warn_c);
}

void rdx_link_line(const struct rdx_link_info *own, const struct rdx_link_info *peer, double now, char *out, size_t n)
{
    char r[160], t[64] = "";
    out[0] = 0;
    int peer_hot = peer && (peer->level == RDX_LINK_WARN || peer->level == RDX_LINK_ALARM);
    int own_hot = own && (own->level == RDX_LINK_WARN || own->level == RDX_LINK_ALARM);
    if (own && own->level == RDX_LINK_FAILED) {         /* no reconnect will help: say it first */
        failed_line(out, n, own, "This computer's");
        return;
    }
    if (peer && peer->level == RDX_LINK_FAILED) {
        failed_line(out, n, peer, "The other computer's");
        return;
    }
    if (own && own->level == RDX_LINK_DOWN) {
        /* our own port down without a reason of its own: the other side was hot before */
        if (!strncmp(own->down_reason, "no signal", 9) && peer_hot && peer->hot == RDX_HOT_CHIP) {
            if (peer->chip.present) snprintf(t, sizeof(t), " (%.1f \xc2\xb0""C)", peer->chip.temp_c);
            snprintf(out, n, "Link down: the other computer's network card overheated%s", t);
        } else if (!strncmp(own->down_reason, "no signal", 9) && peer_hot && peer->mod.present) {
            if (!isnan(peer->mod.warn_c))
                snprintf(t, sizeof(t), " (%.1f \xc2\xb0""C, warning at %.0f \xc2\xb0""C)", peer->mod.temp_c, peer->mod.warn_c);
            snprintf(out, n, "Link down: the other computer's network module overheated%s", t);
        } else {
            whose(r, sizeof(r), own->down_reason, "this computer's");
            fmt_temp(t, sizeof(t), &own->mod);
            snprintf(out, n, "Link down: %s%s", r, strstr(r, "network module overheat") ? t : "");
        }
        return;
    }
    if (peer && peer->level == RDX_LINK_DOWN) {        /* we still hear it (other path), its port is down */
        whose(r, sizeof(r), peer->down_reason, "the other computer's");
        snprintf(out, n, "The other computer's link is down: %s", r);
        return;
    }
    if (own_hot || peer_hot) {
        const struct rdx_link_info *h = own_hot && (!peer_hot || own->level >= peer->level) ? own : peer;
        hot_line(out, n, h, h == own ? "This computer's" : "The other computer's");
        return;
    }
    /* the link is back: say once what happened, for a while */
    const struct rdx_link_info *d = NULL;
    const char *owner = "";
    if (own && own->have_down && own->back_at > 0 && now - own->back_at < RDX_LINK_RECENT_S) { d = own; owner = "this computer's"; }
    if (peer && peer->have_down && peer->back_at > 0 && now - peer->back_at < RDX_LINK_RECENT_S &&
        (!d || !strncmp(d->down_reason, "no signal", 9))) { d = peer; owner = "the other computer's"; }
    if (d) {
        whose(r, sizeof(r), d->down_reason, owner);
        snprintf(out, n, "The link was down for %.0f s: %s", d->back_at - d->down_at, r);
    }
}
