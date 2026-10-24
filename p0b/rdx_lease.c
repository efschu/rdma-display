/* SPDX-License-Identifier: MIT
 *
 * Game monitor by DRM lease (X11, RandR 1.6): one monitor of this desktop is taken from the X
 * server and driven directly by p0b/rdx_sink_kms (page flips, VRR, LFC), the desktop stays on the
 * other monitors. The way VR runtimes take a headset's display (Monado, SteamVR:
 * VK_EXT_acquire_xlib_display = RRCreateLease; keithp's kmscube --lease / xrandr --lease).
 *
 *   rdx_lease --output NAME --state FILE -- /path/rdx_sink_kms --drm-fd @LEASEFD@ [sink options]
 *   rdx_lease --restore FILE        put the monitor back as FILE says (after a crash)
 *   rdx_lease --probe               can this X server lease? prints one JSON line, no change
 *
 * 1. The output's CRTC configuration (mode, position, rotation, primary) goes to FILE, then the
 *    output is switched off with RandR, so the desktop moves its windows to the other monitors.
 * 2. RRCreateLease for that CRTC and output: a DRM fd that is master of just those two (the kernel
 *    adds the CRTC's primary plane; overlay planes are not part of it, so the in-stream menu has no
 *    overlay there). The sink runs with that fd ("@LEASEFD@" in its arguments).
 * 3. When the sink ends (or on SIGTERM / SIGINT, passed on to it): RRFreeLease, the old
 *    configuration back (screen size grown again if the desktop shrank it, primary), FILE removed.
 *    The sink dies with us (PR_SET_PDEATHSIG); a crash of the sink ends the lease in the kernel (its
 *    fd closes). If this program itself is killed, FILE stays and rdx-clientd runs --restore.
 * Exit codes: the sink's; 3 = no lease (RandR < 1.6, the driver refused, output unknown), with the
 * monitor left as it was.
 */
#define _GNU_SOURCE
#include "rdx_xwin.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

struct saved {
    char output_name[64];
    uint32_t output, crtc, mode;
    int16_t x, y;
    uint16_t rotation;
    int primary;
};

static xcb_connection_t *C;
static xcb_window_t ROOT;
static volatile sig_atomic_t g_sig;
static pid_t g_child = -1;

static void on_sig(int s)
{
    g_sig = s;
    if (g_child > 0) kill(g_child, s);
}

static int connect_x(void)
{
    rdx_x_desktop_env();
    int screen = 0;
    C = xcb_connect(NULL, &screen);
    if (!C || xcb_connection_has_error(C)) { fprintf(stderr, "[lease] no X connection (DISPLAY %s)\n", getenv("DISPLAY")); return -1; }
    const xcb_setup_t *setup = xcb_get_setup(C);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen; i++) xcb_screen_next(&it);
    ROOT = it.data->root;
    return 0;
}

/* RandR >= 1.6 and not Xwayland under a compositor that leases nothing; 0 = can try */
static int randr_lease_version(int *major, int *minor)
{
    xcb_randr_query_version_reply_t *v = xcb_randr_query_version_reply(C, xcb_randr_query_version(C, 1, 6), NULL);
    if (!v) return -1;
    *major = (int)v->major_version;
    *minor = (int)v->minor_version;
    free(v);
    return *major > 1 || (*major == 1 && *minor >= 6) ? 0 : -1;
}

static int find_output(const char *name, struct saved *s, xcb_timestamp_t *cfg_ts)
{
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(C, xcb_randr_get_screen_resources_current(C, ROOT), NULL);
    if (!res) return -1;
    *cfg_ts = res->config_timestamp;
    xcb_randr_output_t *outs = xcb_randr_get_screen_resources_current_outputs(res);
    int n = xcb_randr_get_screen_resources_current_outputs_length(res), found = -1;
    for (int i = 0; i < n && found < 0; i++) {
        xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(C, xcb_randr_get_output_info(C, outs[i], *cfg_ts), NULL);
        if (!oi) continue;
        int len = xcb_randr_get_output_info_name_length(oi);
        if (len == (int)strlen(name) && !memcmp(xcb_randr_get_output_info_name(oi), name, (size_t)len) && oi->crtc) {
            s->output = outs[i];
            s->crtc = oi->crtc;
            snprintf(s->output_name, sizeof(s->output_name), "%s", name);
            found = 0;
        }
        free(oi);
    }
    free(res);
    if (found) return -1;
    xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(C, xcb_randr_get_crtc_info(C, s->crtc, *cfg_ts), NULL);
    if (!ci) return -1;
    s->mode = ci->mode;
    s->x = ci->x;
    s->y = ci->y;
    s->rotation = ci->rotation;
    free(ci);
    xcb_randr_get_output_primary_reply_t *pr = xcb_randr_get_output_primary_reply(C, xcb_randr_get_output_primary(C, ROOT), NULL);
    s->primary = pr && pr->output == s->output;
    free(pr);
    return 0;
}

static int save_state(const char *path, const struct saved *s)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%s %u %u %u %d %d %u %d\n", s->output_name, s->output, s->crtc, s->mode, s->x, s->y, s->rotation, s->primary);
    return fclose(f);
}

static int load_state(const char *path, struct saved *s)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int x, y, r = fscanf(f, "%63s %u %u %u %d %d %hu %d", s->output_name, &s->output, &s->crtc, &s->mode, &x, &y, &s->rotation,
                         &s->primary);
    fclose(f);
    s->x = (int16_t)x;
    s->y = (int16_t)y;
    return r == 8 ? 0 : -1;
}

static int set_crtc(const struct saved *s, int on)
{
    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(C, xcb_randr_get_screen_resources_current(C, ROOT), NULL);
    if (!res) return -1;
    xcb_timestamp_t ts = res->config_timestamp;
    if (on) {
        /* the desktop may have shrunk the screen when the monitor went: make room for it again */
        int mw = 0, mh = 0;
        xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
        for (int i = 0; i < xcb_randr_get_screen_resources_current_modes_length(res); i++)
            if (modes[i].id == s->mode) { mw = modes[i].width; mh = modes[i].height; }
        if (s->rotation & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270)) { int t = mw; mw = mh; mh = t; }
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(C, xcb_get_geometry(C, ROOT), NULL);
        if (g && mw && (s->x + mw > g->width || s->y + mh > g->height)) {
            int W = s->x + mw > g->width ? s->x + mw : g->width, H = s->y + mh > g->height ? s->y + mh : g->height;
            xcb_randr_set_screen_size(C, ROOT, (uint16_t)W, (uint16_t)H, (uint32_t)(W * 25.4 / 96 + 0.5), (uint32_t)(H * 25.4 / 96 + 0.5));
        }
        free(g);
    }
    xcb_randr_output_t out = s->output;
    xcb_randr_set_crtc_config_reply_t *r = xcb_randr_set_crtc_config_reply(
        C, xcb_randr_set_crtc_config(C, s->crtc, XCB_CURRENT_TIME, ts, on ? s->x : 0, on ? s->y : 0, on ? s->mode : XCB_NONE,
                                     on ? s->rotation : XCB_RANDR_ROTATION_ROTATE_0, on ? 1 : 0, &out), NULL);
    free(res);
    int ok = r && r->status == XCB_RANDR_SET_CONFIG_SUCCESS;
    free(r);
    if (on && ok && s->primary) xcb_randr_set_output_primary(C, ROOT, s->output);
    xcb_flush(C);
    return ok ? 0 : -1;
}

static int restore(const char *path)
{
    struct saved s;
    if (load_state(path, &s)) { fprintf(stderr, "[lease] no state in %s\n", path); return 1; }
    /* right after rdx_lease was killed the X server may still count the CRTC as leased (the DDX notices the
     * ended lease a moment later, drmmode_validate_leases): the first SetCrtcConfig then fails. Measured
     * on the test host: rdx-clientd's first --restore failed, its next one 10 s later worked. Retry for 3 s. */
    int rc = -1;
    for (int i = 0; i < 30 && (rc = set_crtc(&s, 1)); i++) usleep(100000);
    fprintf(stderr, "[lease] %s back on (mode 0x%x at %d,%d)%s\n", s.output_name, s.mode, s.x, s.y, rc ? ": FAILED" : "");
    if (!rc) unlink(path);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *output = NULL, *state = NULL;
    int k = 1;
    for (; k < argc; k++) {
        if (!strcmp(argv[k], "--")) { k++; break; }
        if (!strcmp(argv[k], "--output") && k + 1 < argc) output = argv[++k];
        else if (!strcmp(argv[k], "--state") && k + 1 < argc) state = argv[++k];
        else if (!strcmp(argv[k], "--restore") && k + 1 < argc) {
            if (connect_x()) return 1;
            return restore(argv[k + 1]);
        } else if (!strcmp(argv[k], "--probe")) {
            int ma = 0, mi = 0, ok = !connect_x() && !randr_lease_version(&ma, &mi);
            const char *vendor = C ? xcb_setup_vendor(xcb_get_setup(C)) : "";
            int vl = C ? xcb_setup_vendor_length(xcb_get_setup(C)) : 0;
            int xwayland = getenv("XAUTHORITY") && strstr(getenv("XAUTHORITY"), "Xwayland");
            printf("{\"randr\":\"%d.%d\",\"lease\":%s,\"xwayland\":%s,\"vendor\":\"%.*s\"}\n", ma, mi, ok ? "true" : "false",
                   xwayland ? "true" : "false", vl, vendor);
            return ok ? 0 : 3;
        } else { fprintf(stderr, "see the header of p0b/rdx_lease.c\n"); return 2; }
    }
    if (!output || !state || k >= argc) { fprintf(stderr, "see the header of p0b/rdx_lease.c\n"); return 2; }
    if (connect_x()) return 3;
    int ma = 0, mi = 0;
    if (randr_lease_version(&ma, &mi)) { fprintf(stderr, "[lease] RandR %d.%d: no leases (needs 1.6)\n", ma, mi); return 3; }
    struct saved s;
    memset(&s, 0, sizeof(s));
    xcb_timestamp_t ts;
    if (find_output(output, &s, &ts)) { fprintf(stderr, "[lease] no active output %s\n", output); return 3; }
    if (save_state(state, &s)) { perror(state); return 3; }
    if (set_crtc(&s, 0)) { fprintf(stderr, "[lease] cannot switch %s off\n", output); unlink(state); return 3; }
    usleep(300000);                                  /* the desktop moves its windows away */
    xcb_randr_lease_t lid = xcb_generate_id(C);
    xcb_randr_crtc_t crtc = s.crtc;
    xcb_randr_output_t out = s.output;
    xcb_generic_error_t *err = NULL;
    xcb_randr_create_lease_reply_t *lr = xcb_randr_create_lease_reply(C, xcb_randr_create_lease(C, ROOT, lid, 1, 1, &crtc, &out), &err);
    int *fds = lr ? xcb_randr_create_lease_reply_fds(C, lr) : NULL;
    if (!lr || !fds || lr->nfd < 1) {
        fprintf(stderr, "[lease] RRCreateLease for %s refused (X error %d): the monitor goes back\n", output, err ? err->error_code : -1);
        free(err);
        free(lr);
        restore(state);
        return 3;
    }
    int lfd = fds[0];
    free(lr);
    fcntl(lfd, F_SETFD, 0);                          /* the sink inherits it */
    char fdstr[16];
    snprintf(fdstr, sizeof(fdstr), "%d", lfd);
    char **av = calloc((size_t)(argc - k + 1), sizeof(char *));
    for (int i = k; i < argc; i++) av[i - k] = !strcmp(argv[i], "@LEASEFD@") ? fdstr : argv[i];
    printf("[lease] %s (CRTC 0x%x, output 0x%x) leased: DRM fd %d for %s\n", output, s.crtc, s.output, lfd, av[0]);
    fflush(stdout);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, on_sig);
    pid_t parent = getpid();
    g_child = fork();
    if (g_child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);            /* never outlive the one who gives the monitor back */
        if (getppid() != parent) _exit(1);
        execv(av[0], av);
        perror(av[0]);
        _exit(127);
    }
    close(lfd);                                      /* the sink holds the lease now */
    int st = 0;
    while (waitpid(g_child, &st, 0) < 0 && errno == EINTR) {}
    xcb_randr_free_lease(C, lid, 1);
    xcb_flush(C);
    usleep(200000);                                  /* the server sees the lease end */
    restore(state);
    xcb_disconnect(C);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}
