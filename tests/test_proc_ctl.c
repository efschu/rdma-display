/* SPDX-License-Identifier: MIT
 *
 * Unit test for rdx_proc (spawn, log tail, exit callback, stop with SIGTERM
 * and SIGKILL escalation, CPU sample) and rdx_ctl (JSON lines over loopback,
 * several messages in one segment, a split message, peer close).
 *   make tests/test_proc_ctl && tests/test_proc_ctl
 */
#define _GNU_SOURCE
#include "../common/rdx_ctl.h"
#include "../common/rdx_proc.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int exits, last_status;
static void on_exit_cb(struct rdx_proc *p, int status, void *user)
{
    (void)p;
    exits++;
    last_status = status;
    *(int *)user = 1;
}

/* run the loop until flag is set or timeout */
static void loop_until(int *flag, int timeout_ms)
{
    for (int t = 0; !*flag && t < timeout_ms; t += 10) {
        struct pollfd p = {.fd = rdx_proc_poll_fd(), .events = POLLIN};
        if (poll(&p, 1, 10) > 0) rdx_proc_dispatch();
    }
}

static void test_proc(void)
{
    int done = 0;
    char *a1[] = {"/bin/sh", "-c", "echo hello-out; echo hello-err >&2; printf partial; exit 3", NULL};
    struct rdx_proc *p = rdx_proc_spawn("t1", a1, on_exit_cb, &done);
    CHECK(p && rdx_proc_pid(p) > 0);
    loop_until(&done, 3000);
    CHECK(done);
    CHECK(!rdx_proc_running(p));
    CHECK(WIFEXITED(last_status) && WEXITSTATUS(last_status) == 3);
    char buf[256];
    rdx_proc_log_tail(p, buf, sizeof(buf));
    CHECK(strstr(buf, "hello-out\n") && strstr(buf, "hello-err\n") && strstr(buf, "partial"));
    CHECK(rdx_proc_output_has(p, "hello-err"));
    CHECK(!rdx_proc_output_has(p, "never-printed"));
    rdx_proc_log_tail(p, buf, 5);                       /* last 4 bytes + NUL */
    CHECK(!strcmp(buf, "tial"));
    rdx_proc_free(p);

    /* a state line survives 100 KB of other output (more than the 64 KB ring) */
    done = 0;
    char *a9[] = {"/bin/sh", "-c", "echo '[gs] vrr: off, early'; echo '[gs] vrr: on, up to 139.679 Hz'; "
                  "i=0; while [ $i -lt 1700 ]; do echo 'ATTENTION: default value of option vk_xwayland_wait_ready "
                  "overridden by environment.'; i=$((i+1)); done", NULL};
    p = rdx_proc_spawn("t9", a9, on_exit_cb, &done);
    rdx_proc_keep_last(p, "[gs] vrr: ");
    loop_until(&done, 200000);   /* counts poll rounds, not ms, while output flows */
    CHECK(done);
    CHECK(!rdx_proc_output_has(p, "[gs] vrr: on"));
    CHECK(rdx_proc_kept(p, buf, sizeof(buf)) && !strcmp(buf, "on, up to 139.679 Hz"));
    rdx_proc_free(p);

    /* SIGTERM is honoured */
    done = 0;
    char *a2[] = {"/bin/sleep", "30", NULL};
    p = rdx_proc_spawn("t2", a2, on_exit_cb, &done);
    CHECK(rdx_proc_running(p));
    rdx_proc_stop(p, 1000);
    CHECK(done && WIFSIGNALED(last_status) && WTERMSIG(last_status) == SIGTERM);
    rdx_proc_free(p);

    /* SIGTERM ignored -> SIGKILL after the grace period */
    done = 0;
    char *a3[] = {"/bin/sh", "-c", "trap '' TERM; echo armed; while :; do sleep 1; done", NULL};
    p = rdx_proc_spawn("t3", a3, on_exit_cb, &done);
    for (int t = 0; t < 300 && !rdx_proc_output_has(p, "armed"); t++) {
        struct pollfd pf = {.fd = rdx_proc_poll_fd(), .events = POLLIN};
        if (poll(&pf, 1, 10) > 0) rdx_proc_dispatch();
    }
    CHECK(rdx_proc_output_has(p, "armed"));
    rdx_proc_stop(p, 300);
    CHECK(done && WIFSIGNALED(last_status) && WTERMSIG(last_status) == SIGKILL);
    rdx_proc_free(p);

    /* CPU accounting: a busy child shows up near 100 % */
    done = 0;
    char *a4[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 2000000 ]; do i=$((i+1)); done", NULL};
    p = rdx_proc_spawn("t4", a4, on_exit_cb, &done);
    loop_until(&done, 5000);
    double cpu = rdx_proc_cpu_pct(p);
    fprintf(stderr, "busy child cpu %.1f %%\n", cpu);
    CHECK(cpu > 30 && cpu < 130);
    rdx_proc_free(p);

    /* exec failure surfaces as exit 127 */
    done = 0;
    char *a5[] = {"/nonexistent/binary", NULL};
    p = rdx_proc_spawn("t5", a5, on_exit_cb, &done);
    loop_until(&done, 3000);
    CHECK(done && WIFEXITED(last_status) && WEXITSTATUS(last_status) == 127);
    rdx_proc_free(p);
}

static int recv_one(struct rdx_ctl *c, cJSON **m, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 10) {
        int r = rdx_ctl_recv(c, m);
        if (r) return r;
        struct pollfd p = {.fd = rdx_ctl_fd(c), .events = POLLIN};
        poll(&p, 1, 10);
    }
    return 0;
}

static void test_ctl(void)
{
    int lfd = rdx_ctl_listen("127.0.0.1", 47199);
    CHECK(lfd >= 0);
    struct rdx_ctl *cl = rdx_ctl_connect("127.0.0.1", 47199, 1000);
    CHECK(cl);
    struct pollfd p = {.fd = lfd, .events = POLLIN};
    poll(&p, 1, 1000);
    struct rdx_ctl *sv = rdx_ctl_accept(lfd);
    CHECK(sv && !strcmp(rdx_ctl_peer(sv), "127.0.0.1"));

    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", 1);
    cJSON_AddStringToObject(m, "cmd", "hello");
    CHECK(!rdx_ctl_send(cl, m));
    cJSON_ReplaceItemInObject(m, "id", cJSON_CreateNumber(2));
    CHECK(!rdx_ctl_send(cl, m));
    cJSON_Delete(m);

    cJSON *r = NULL;
    CHECK(recv_one(sv, &r, 1000) == 1);
    CHECK(r && cJSON_GetObjectItem(r, "id")->valueint == 1);
    cJSON_Delete(r);
    CHECK(recv_one(sv, &r, 1000) == 1);
    CHECK(r && cJSON_GetObjectItem(r, "id")->valueint == 2);
    cJSON_Delete(r);

    /* a message split over two writes, and a non-object line that must be skipped */
    const char *part1 = "[1,2]\n{\"id\":3,\"cmd\":\"st", *part2 = "atus\"}\n";
    send(rdx_ctl_fd(cl), part1, strlen(part1), 0);
    CHECK(recv_one(sv, &r, 200) == 0);
    send(rdx_ctl_fd(cl), part2, strlen(part2), 0);
    CHECK(recv_one(sv, &r, 1000) == 1);
    CHECK(r && !strcmp(cJSON_GetObjectItem(r, "cmd")->valuestring, "status"));
    cJSON_Delete(r);

    /* a large message (300 KiB string) crosses many segments */
    char *big = malloc(300 * 1024 + 1);
    memset(big, 'x', 300 * 1024);
    big[300 * 1024] = 0;
    m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "blob", big);
    CHECK(!rdx_ctl_send(sv, m));
    cJSON_Delete(m);
    CHECK(recv_one(cl, &r, 2000) == 1);
    CHECK(r && strlen(cJSON_GetObjectItem(r, "blob")->valuestring) == 300 * 1024);
    cJSON_Delete(r);
    free(big);

    rdx_ctl_close(cl);
    CHECK(recv_one(sv, &r, 1000) == -1);
    rdx_ctl_close(sv);
    close(lfd);

    CHECK(rdx_ctl_connect("127.0.0.1", 47198, 500) == NULL);      /* nobody listens */
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_proc();
    test_ctl();
    printf("test_proc_ctl: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
