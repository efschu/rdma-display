/* SPDX-License-Identifier: MIT
 *
 * rdma-display: supervised child processes (see rdx_proc.h).
 */
#define _GNU_SOURCE
#include "rdx_proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RING_BYTES (64 * 1024)
#define LINE_BYTES 1024
#define STDBUF     "/usr/bin/stdbuf"

enum { EP_PIPE, EP_PIDFD };

struct ep_tag {
    struct rdx_proc *p;
    int kind;
};

struct rdx_proc {
    char name[32];
    pid_t pid;
    int pidfd, outfd;
    int status;                 /* waitpid status, -1 while running */
    double t0, t_end;           /* CLOCK_MONOTONIC seconds */
    double cpu_last;            /* utime+stime seconds, kept after exit */
    rdx_proc_exit_cb cb;
    void *user;
    struct ep_tag tag_pipe, tag_pidfd;
    char ring[RING_BYTES];
    size_t ring_head, ring_len; /* head = next write position */
    char line[LINE_BYTES];
    size_t line_len;
    char keep_prefix[32];       /* rdx_proc_keep_last */
    char kept[LINE_BYTES];
};

static int g_ep = -1;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int rdx_proc_poll_fd(void)
{
    if (g_ep < 0) g_ep = epoll_create1(EPOLL_CLOEXEC);
    return g_ep;
}

static void flush_line(struct rdx_proc *p)
{
    if (!p->line_len) return;
    fprintf(stderr, "[%s] %.*s\n", p->name, (int)p->line_len, p->line);
    size_t kl = strlen(p->keep_prefix);
    if (kl && p->line_len >= kl && !memcmp(p->line, p->keep_prefix, kl))
        snprintf(p->kept, sizeof(p->kept), "%.*s", (int)(p->line_len - kl), p->line + kl);
    p->line_len = 0;
}

static void take_output(struct rdx_proc *p, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        p->ring[p->ring_head] = b[i];
        p->ring_head = (p->ring_head + 1) % RING_BYTES;
        if (p->ring_len < RING_BYTES) p->ring_len++;
        if (b[i] == '\n') flush_line(p);
        else {
            if (p->line_len == LINE_BYTES) flush_line(p);
            p->line[p->line_len++] = b[i];
        }
    }
}

static void drain_pipe(struct rdx_proc *p)
{
    char buf[4096];
    while (p->outfd >= 0) {
        ssize_t r = read(p->outfd, buf, sizeof(buf));
        if (r > 0) { take_output(p, buf, (size_t)r); continue; }
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            if (errno == EINTR) continue;
            return;
        }
        /* EOF or error: the child closed its side */
        flush_line(p);
        epoll_ctl(g_ep, EPOLL_CTL_DEL, p->outfd, NULL);
        close(p->outfd);
        p->outfd = -1;
    }
}

static double read_cpu_s(pid_t pid)
{
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    const char *s = strrchr(buf, ')');      /* comm may contain spaces */
    if (!s) return -1;
    unsigned long ut = 0, st = 0;
    /* after ')': state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15) */
    if (sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) return -1;
    return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}

/* the child is gone (pidfd readable or we killed it): reap, keep the last CPU sample, fire cb */
static int reap(struct rdx_proc *p, int block)
{
    if (p->status >= 0) return 1;
    double cpu = read_cpu_s(p->pid);        /* zombie still has its stat */
    int st;
    pid_t r = waitpid(p->pid, &st, block ? 0 : WNOHANG);
    if (r != p->pid) return 0;
    if (cpu >= 0) p->cpu_last = cpu;
    p->status = st;
    p->t_end = now_s();
    drain_pipe(p);
    flush_line(p);
    if (p->pidfd >= 0) {
        epoll_ctl(g_ep, EPOLL_CTL_DEL, p->pidfd, NULL);
        close(p->pidfd);
        p->pidfd = -1;
    }
    if (p->cb) p->cb(p, st, p->user);
    return 1;
}

struct rdx_proc *rdx_proc_spawn(const char *name, char *const argv[], rdx_proc_exit_cb cb, void *user)
{
    if (rdx_proc_poll_fd() < 0) return NULL;
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) return NULL;
    struct rdx_proc *p = calloc(1, sizeof(*p));
    if (!p) { close(pfd[0]); close(pfd[1]); return NULL; }
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->status = -1;
    p->cb = cb;
    p->user = user;
    p->t0 = now_s();
    pid_t parent = getpid();

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); free(p); return NULL; }
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(127);
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, NULL);
        for (int s = 1; s < NSIG; s++) signal(s, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        int argc = 0;
        while (argv[argc]) argc++;
        if (!access(STDBUF, X_OK)) {
            char **a = calloc((size_t)argc + 4, sizeof(char *));
            a[0] = STDBUF; a[1] = "-oL"; a[2] = "-eL";
            for (int i = 0; i < argc; i++) a[3 + i] = argv[i];
            execv(STDBUF, a);
        }
        execv(argv[0], argv);
        fprintf(stderr, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    p->pid = pid;
    p->outfd = pfd[0];
    fcntl(p->outfd, F_SETFL, fcntl(p->outfd, F_GETFL) | O_NONBLOCK);
    p->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    p->tag_pipe = (struct ep_tag){p, EP_PIPE};
    p->tag_pidfd = (struct ep_tag){p, EP_PIDFD};
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &p->tag_pipe};
    epoll_ctl(g_ep, EPOLL_CTL_ADD, p->outfd, &ev);
    if (p->pidfd >= 0) {
        ev.data.ptr = &p->tag_pidfd;
        epoll_ctl(g_ep, EPOLL_CTL_ADD, p->pidfd, &ev);
    }
    fprintf(stderr, "[proc] %s started, pid %d\n", p->name, (int)pid);
    return p;
}

void rdx_proc_dispatch(void)
{
    struct epoll_event ev[16];
    int n;
    while ((n = epoll_wait(g_ep, ev, 16, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            struct ep_tag *t = ev[i].data.ptr;
            if (t->kind == EP_PIPE) drain_pipe(t->p);
            else reap(t->p, 0);
        }
        if (n < 16) break;
    }
}

int rdx_proc_stop(struct rdx_proc *p, int grace_ms)
{
    if (!p || p->status >= 0) return 0;
    kill(p->pid, SIGTERM);
    int fd = p->pidfd;
    if (fd >= 0) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, grace_ms) <= 0) {
            fprintf(stderr, "[proc] %s did not stop within %d ms, SIGKILL\n", p->name, grace_ms);
            kill(p->pid, SIGKILL);
        }
    } else {
        /* no pidfd: poll with WNOHANG in 10 ms steps */
        for (int t = 0; t < grace_ms; t += 10) {
            if (reap(p, 0)) return 0;
            usleep(10000);
        }
        kill(p->pid, SIGKILL);
    }
    reap(p, 1);
    return 0;
}

int rdx_proc_running(const struct rdx_proc *p) { return p && p->status < 0; }
pid_t rdx_proc_pid(const struct rdx_proc *p) { return p ? p->pid : -1; }
const char *rdx_proc_name(const struct rdx_proc *p) { return p ? p->name : ""; }
int rdx_proc_exit_status(const struct rdx_proc *p) { return p ? p->status : -1; }

double rdx_proc_uptime_s(const struct rdx_proc *p)
{
    if (!p) return 0;
    return (p->status >= 0 ? p->t_end : now_s()) - p->t0;
}

double rdx_proc_cpu_pct(const struct rdx_proc *p)
{
    if (!p) return 0;
    double cpu = p->cpu_last;
    if (p->status < 0) {
        double c = read_cpu_s(p->pid);
        if (c >= 0) cpu = ((struct rdx_proc *)p)->cpu_last = c;
    }
    double el = rdx_proc_uptime_s(p);
    return el > 0 ? 100.0 * cpu / el : 0;
}

void rdx_proc_keep_last(struct rdx_proc *p, const char *prefix)
{
    if (!p) return;
    snprintf(p->keep_prefix, sizeof(p->keep_prefix), "%s", prefix ? prefix : "");
    p->kept[0] = 0;
}

int rdx_proc_kept(const struct rdx_proc *p, char *buf, size_t n)
{
    if (!p || !p->kept[0] || !n) return 0;
    snprintf(buf, n, "%s", p->kept);
    return 1;
}

size_t rdx_proc_log_tail(const struct rdx_proc *p, char *buf, size_t n)
{
    if (!p || !n) return 0;
    size_t take = p->ring_len < n - 1 ? p->ring_len : n - 1;
    size_t start = (p->ring_head + RING_BYTES - take) % RING_BYTES;
    for (size_t i = 0; i < take; i++) buf[i] = p->ring[(start + i) % RING_BYTES];
    buf[take] = 0;
    return take;
}

int rdx_proc_output_has(const struct rdx_proc *p, const char *needle)
{
    if (!p) return 0;
    char *tmp = malloc(RING_BYTES + 1);
    if (!tmp) return 0;
    size_t n = rdx_proc_log_tail(p, tmp, RING_BYTES + 1);
    int hit = memmem(tmp, n, needle, strlen(needle)) != NULL;
    free(tmp);
    return hit;
}

void rdx_proc_free(struct rdx_proc *p)
{
    if (!p) return;
    if (p->status < 0) rdx_proc_stop(p, 2000);
    if (p->outfd >= 0) {
        epoll_ctl(g_ep, EPOLL_CTL_DEL, p->outfd, NULL);
        close(p->outfd);
    }
    if (p->pidfd >= 0) {
        epoll_ctl(g_ep, EPOLL_CTL_DEL, p->pidfd, NULL);
        close(p->pidfd);
    }
    free(p);
}
