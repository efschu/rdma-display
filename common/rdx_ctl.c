/* SPDX-License-Identifier: MIT
 *
 * rdma-display: newline-delimited JSON control channel (see rdx_ctl.h).
 */
#define _GNU_SOURCE
#include "rdx_ctl.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SEND_TIMEOUT_MS 2000

struct rdx_ctl {
    int fd;
    pthread_mutex_t send_mu;
    char *buf;                  /* received bytes not yet consumed */
    size_t len, cap;
    char peer[INET_ADDRSTRLEN];
};

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static struct rdx_ctl *wrap(int fd)
{
    struct rdx_ctl *c = calloc(1, sizeof(*c));
    if (!c) { close(fd); return NULL; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_nonblock(fd);
    c->fd = fd;
    pthread_mutex_init(&c->send_mu, NULL);
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    if (!getpeername(fd, (struct sockaddr *)&a, &al)) inet_ntop(AF_INET, &a.sin_addr, c->peer, sizeof(c->peer));
    return c;
}

int rdx_ctl_listen(const char *ip, int port)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { fprintf(stderr, "[ctl] bad address %s\n", ip); return -1; }
    int one = 1, s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) {
        fprintf(stderr, "[ctl] listen %s:%d: %s\n", ip, port, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

struct rdx_ctl *rdx_ctl_accept(int lfd)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    return fd < 0 ? NULL : wrap(fd);
}

struct rdx_ctl *rdx_ctl_connect(const char *ip, int port, int timeout_ms)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { errno = EINVAL; return NULL; }
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) return NULL;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) && errno != EINPROGRESS) { close(s); return NULL; }
    struct pollfd p = {.fd = s, .events = POLLOUT};
    int err = 0;
    socklen_t el = sizeof(err);
    if (poll(&p, 1, timeout_ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err) {
        close(s);
        errno = err ? err : ETIMEDOUT;
        return NULL;
    }
    return wrap(s);
}

int rdx_ctl_fd(const struct rdx_ctl *c) { return c ? c->fd : -1; }
const char *rdx_ctl_peer(const struct rdx_ctl *c) { return c ? c->peer : ""; }

int rdx_ctl_send(struct rdx_ctl *c, const cJSON *msg)
{
    if (!c || c->fd < 0) return -1;
    char *s = cJSON_PrintUnformatted(msg);
    if (!s) return -1;
    size_t n = strlen(s);
    s[n] = '\n';                /* replaces the terminator; length is tracked */
    int rc = 0;
    pthread_mutex_lock(&c->send_mu);
    for (size_t off = 0; off < n + 1;) {
        ssize_t w = send(c->fd, s + off, n + 1 - off, MSG_NOSIGNAL);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            struct pollfd p = {.fd = c->fd, .events = POLLOUT};
            if (poll(&p, 1, SEND_TIMEOUT_MS) == 1) continue;
        }
        rc = -1;
        break;
    }
    pthread_mutex_unlock(&c->send_mu);
    free(s);
    return rc;
}

/* parse one complete line from the buffer: 1 = msg, 0 = none, -1 = garbage */
static int take_line(struct rdx_ctl *c, cJSON **msg)
{
    char *nl = memchr(c->buf, '\n', c->len);
    if (!nl) return c->len > RDX_CTL_MAX_LINE ? -1 : 0;
    *nl = 0;
    cJSON *j = cJSON_Parse(c->buf);
    size_t used = (size_t)(nl - c->buf) + 1;
    memmove(c->buf, nl + 1, c->len - used);
    c->len -= used;
    if (!j || !cJSON_IsObject(j)) {
        fprintf(stderr, "[ctl] dropped a line that is not a JSON object from %s\n", c->peer);
        cJSON_Delete(j);
        return 0;
    }
    *msg = j;
    return 1;
}

int rdx_ctl_recv(struct rdx_ctl *c, cJSON **msg)
{
    *msg = NULL;
    if (!c || c->fd < 0) return -1;
    int r = take_line(c, msg);
    if (r) return r;
    if (c->cap - c->len < 4096) {
        size_t nc = c->cap ? c->cap * 2 : 8192;
        char *nb = realloc(c->buf, nc);
        if (!nb) return -1;
        c->buf = nb;
        c->cap = nc;
    }
    ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len - 1, 0);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    c->len += (size_t)n;
    return take_line(c, msg);
}

void rdx_ctl_close(struct rdx_ctl *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    pthread_mutex_destroy(&c->send_mu);
    free(c->buf);
    free(c);
}
