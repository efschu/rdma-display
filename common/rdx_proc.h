/* SPDX-License-Identifier: MIT
 *
 * rdma-display: supervised child processes for the control daemons.
 * Each child's stdout+stderr goes into a 64 KiB ring (for the web UI) and is
 * also forwarded to our stderr prefixed "[name] " (lands in the journal).
 * Exit is noticed via pidfd; everything is reported through ONE epoll fd so a
 * daemon's main loop sleeps until a child prints or exits.
 * Not thread-safe: call all functions from the thread that runs the loop
 * (the daemons serialize web-server threads with their own mutex).
 * The exit callback also fires from rdx_proc_stop(); it may stop other
 * children, but must not rdx_proc_free() any child (free after dispatch).
 */
#pragma once
#include <stddef.h>
#include <sys/types.h>

struct rdx_proc;                                   /* one supervised child */
typedef void (*rdx_proc_exit_cb)(struct rdx_proc *p, int status, void *user); /* waitpid status */

/* argv[0] is the program path; started via stdbuf -oL -eL when available so
 * that line-wise progress output is not stuck in a stdio buffer of the pipe */
struct rdx_proc *rdx_proc_spawn(const char *name, char *const argv[], rdx_proc_exit_cb cb, void *user);
int   rdx_proc_stop(struct rdx_proc *p, int grace_ms);   /* SIGTERM, SIGKILL after grace; blocks <= grace */
int   rdx_proc_running(const struct rdx_proc *p);
pid_t rdx_proc_pid(const struct rdx_proc *p);
const char *rdx_proc_name(const struct rdx_proc *p);
size_t rdx_proc_log_tail(const struct rdx_proc *p, char *buf, size_t n); /* last n bytes of stdout+stderr */
double rdx_proc_cpu_pct(const struct rdx_proc *p);  /* /proc/<pid>/stat since spawn */
void  rdx_proc_free(struct rdx_proc *p);            /* after exit */
int   rdx_proc_poll_fd(void);                       /* epoll fd: readable when any child output/exit */
void  rdx_proc_dispatch(void);                      /* call when poll_fd is readable: read pipes, reap, fire cbs */

/* extras beyond the contract */
int    rdx_proc_exit_status(const struct rdx_proc *p);   /* waitpid status, -1 while running */
double rdx_proc_uptime_s(const struct rdx_proc *p);
int    rdx_proc_output_has(const struct rdx_proc *p, const char *needle); /* 1 if the ring holds needle */
/* keep the last output line that starts with prefix (state lines like "[gs] vrr: ..."), however much
 * other output follows; rdx_proc_kept copies it without the prefix (0 = none seen yet) */
void   rdx_proc_keep_last(struct rdx_proc *p, const char *prefix);
int    rdx_proc_kept(const struct rdx_proc *p, char *buf, size_t n);
