/* SPDX-License-Identifier: MIT
 *
 * System helpers for the USB passthrough: sysfs, modules, SMC-R settings and
 * diagnosis (smc_diag), JSON strings, signals as an fd.
 */
#define _GNU_SOURCE
#include "rdx_usb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/smc_diag.h>
#include <linux/sock_diag.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AF_SMC
#define AF_SMC 43
#endif

int rdx_sysfs_read(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    return (int)r;
}

long rdx_sysfs_read_long(const char *path, int base, long dflt)
{
    char b[64];
    if (rdx_sysfs_read(path, b, sizeof(b)) <= 0) return dflt;
    return strtol(b, NULL, base);
}

int rdx_sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t r = write(fd, val, strlen(val));
    int e = errno;
    close(fd);
    return r < 0 ? -e : 0;
}

int rdx_modprobe(const char *mod)
{
    pid_t p = fork();
    if (p == 0) {
        execl("/sbin/modprobe", "modprobe", "-q", mod, (char *)NULL);
        execlp("modprobe", "modprobe", "-q", mod, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

/* ---------------- SMC-R ---------------- */
#define SMC_BUF_MIN   1048576     /* > 64 KiB URB + header; gate G3: 64 KiB buffers break usbip */
static const char *const smc_knob[3] = {"/proc/sys/net/smc/wmem", "/proc/sys/net/smc/rmem",
                                        "/proc/sys/net/smc/autocorking_size"};
static long smc_old[3] = {-1, -1, -1};

int rdx_smc_prepare(void)
{
    rdx_modprobe("smc");
    rdx_modprobe("smc_diag");
    long want[3] = {SMC_BUF_MIN, SMC_BUF_MIN, 0};
    for (int i = 0; i < 3; i++) {
        long cur = rdx_sysfs_read_long(smc_knob[i], 10, -1);
        if (cur < 0) { fprintf(stderr, "[usb] %s missing (smc module?)\n", smc_knob[i]); return -1; }
        int raise = i < 2 ? cur < want[i] : cur != want[i];
        if (!raise) continue;
        char v[32];
        snprintf(v, sizeof(v), "%ld", want[i]);
        if (rdx_sysfs_write(smc_knob[i], v)) { fprintf(stderr, "[usb] cannot set %s\n", smc_knob[i]); return -1; }
        smc_old[i] = cur;
        printf("[usb] %s: %ld -> %ld (restored at exit)\n", smc_knob[i], cur, want[i]);
    }
    return 0;
}

void rdx_smc_restore(void)
{
    for (int i = 0; i < 3; i++) {
        if (smc_old[i] < 0) continue;
        char v[32];
        snprintf(v, sizeof(v), "%ld", smc_old[i]);
        rdx_sysfs_write(smc_knob[i], v);
        smc_old[i] = -1;
    }
}

int rdx_smc_socket(void)
{
    int fd = socket(AF_SMC, SOCK_STREAM | SOCK_CLOEXEC, 0 /* SMCPROTO_SMC: IPv4 */);
    if (fd < 0) perror("socket(AF_SMC)");
    return fd;
}

int rdx_smc_mode(int fd, char *out, size_t n)
{
    struct sockaddr_in me, peer;
    socklen_t l1 = sizeof(me), l2 = sizeof(peer);
    snprintf(out, n, "unknown");
    if (getsockname(fd, (struct sockaddr *)&me, &l1) || getpeername(fd, (struct sockaddr *)&peer, &l2)) return -1;
    int nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (nl < 0) return -1;
    struct {
        struct nlmsghdr nh;
        struct smc_diag_req req;
    } msg;
    memset(&msg, 0, sizeof(msg));
    msg.nh.nlmsg_len = sizeof(msg);
    msg.nh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.req.diag_family = AF_SMC;
    msg.req.diag_ext = 1 << (SMC_DIAG_CONNINFO - 1);
    if (send(nl, &msg, sizeof(msg), 0) < 0) { close(nl); return -1; }
    char buf[16384];
    int found = -1;
    for (int done = 0; !done;) {
        ssize_t r = recv(nl, buf, sizeof(buf), 0);
        if (r <= 0) break;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)r); h = NLMSG_NEXT(h, r)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
            struct smc_diag_msg *m = NLMSG_DATA(h);
            if (m->id.idiag_sport != me.sin_port || m->id.idiag_dport != peer.sin_port) continue;
            uint32_t sb = 0, rb = 0;
            struct rtattr *a = (struct rtattr *)(m + 1);
            int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));
            for (; RTA_OK(a, alen); a = RTA_NEXT(a, alen))
                if (a->rta_type == SMC_DIAG_CONNINFO) {
                    struct smc_diag_conninfo *c = RTA_DATA(a);
                    sb = c->sndbuf_size;
                    rb = c->rmbe_size;
                }
            if (m->diag_mode == SMC_DIAG_MODE_SMCR) snprintf(out, n, "SMC-R sndbuf %u rmb %u", sb, rb);
            else if (m->diag_mode == SMC_DIAG_MODE_FALLBACK_TCP) snprintf(out, n, "TCP fallback");
            else snprintf(out, n, "mode %u", m->diag_mode);
            found = m->diag_mode == SMC_DIAG_MODE_SMCR ? 0 : 1;
        }
    }
    close(nl);
    return found;
}

/* ---------------- misc ---------------- */
void rdx_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

int rdx_signalfd(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGHUP);
    sigprocmask(SIG_BLOCK, &s, NULL);
    signal(SIGPIPE, SIG_IGN);
    return signalfd(-1, &s, SFD_CLOEXEC | SFD_NONBLOCK);
}
