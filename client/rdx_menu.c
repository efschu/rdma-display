/* SPDX-License-Identifier: MIT
 * In-stream menu, clientd side, see rdx_menu.h.
 */
#define _GNU_SOURCE
#include "rdx_menu.h"
#include "rdx_menu_draw.h"
#include "rdx_placeholder.h"
#include "../common/rdx_menu_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS   6
#define MAX_OUTS    8
#define REDRAW_MS   25        /* coalesce changes: the sink shows a buffer only with its next flip */
#define ID_LISTEN   (1u << 30)
#define ID_TIMER    (1u << 29)

enum { IT_CURSOR, IT_OUTPUT, IT_MONITOR, IT_MIC, IT_CLIP, IT_MOUSE, IT_SWITCH, IT_QUITBG, IT_RESUME, IT_QUIT, IT_QUITAPP,
       NITEMS };
#define MAX_BG      4                      /* apps in the background on the host (rdx-hostd APPS_MAX) */
#define WARN_CARD_MS 12000                 /* a warning from the host (rdx_menu_warn) over the stream */
#define IT_RESTART NITEMS                  /* only while the start placeholder waits long */
#define LINK_CARD_MS 8000                  /* the link card over the stream (hot / back); down: until back */

static struct {
    int ep, lfd, tfd;
    struct rdx_menu_actions act;
    int peer[MAX_PEERS], role[MAX_PEERS];
    int input, sink;                       /* peer fds by role, -1 = none */
    /* sink and its buffers */
    uint32_t mode_w, mode_h, refresh_mhz;
    int overlay_ok;                        /* -1 unknown, 0 no, 1 yes */
    int bfd[RDX_MENU_NBUF];
    uint8_t *bmap[RDX_MENU_NBUF];
    uint32_t bw, bh, bstride;
    int px, shown_buf;
    /* model */
    int open, sel, cursor, out_idx, nouts, dirty;
    char outs[MAX_OUTS][32];
    char cur_out[32], note[128];
    char mon[32];                          /* the host seat's own monitor showing the app, "" = none */
    int mon_on;
    int mic, mic_muted;                    /* the session sends the microphone; muted */
    int clip, clip_on;                     /* the host offers the clipboard in this session; our switch */
    /* pointer of a window client (input with RDX_MENU_CAP_POINTER): setting, mirror session, source hint */
    int input_caps, ptr_setting, ptr_mirror, ptr_hint, ptr_sent_a, ptr_sent_b;
    int sink_cursor;                       /* the window sink shows the host's cursor itself (POINTER b = 1) */
    /* the link line (rdx_menu_link), its class (0 none, 1 hot, 2 down, 3 back) and the card over the stream */
    char link[256];
    int vrr;                               /* the session runs with a variable refresh (FreeSync / VRR) */
    double vrr_min, vrr_max;
    int link_class;
    uint64_t card_until;                   /* mono_ms, 0 = no card, UINT64_MAX = until the class changes */
    char card_msg[256];                    /* the card shows this instead of the link line ("" = the link) */
    int card_warn;                         /* card_msg is a warning (rdx_menu_warn): also on the placeholder */
    /* the host's other apps, running in the background: "Switch to" / "Quit" them */
    char bg_id[MAX_BG][64], bg_name[MAX_BG][96];
    int nbg, bg_idx;
    char hotkey[48];                       /* the desktop hotkey as shown, e.g. "Ctrl+Alt+H" */
    struct rdx_menu_msg stats;
    int have_stats;
    uint64_t t_last_draw;
    /* start placeholder (rdx_placeholder.h): shown while the menu is closed and no frame came */
    int ph_kind, in_session, ph_wait_s;
    uint64_t ph_t0;                        /* mono_ms of the start (or of the stall) */
    char ph_app[128];
    char ph_note[256];                     /* RDX_PH_ENDED: the host's notice */
    unsigned char *ph_raw;                 /* the icon as the host serves it */
    size_t ph_rawlen;
    uint32_t *ph_icon;                     /* decoded for ph_isz */
    int ph_iw, ph_ih, ph_isz;
} M = {.ep = -1, .lfd = -1, .tfd = -1, .input = -1, .sink = -1, .overlay_ok = -1,
       .bfd = {-1, -1}, .shown_buf = -1, .ptr_hint = -1, .ptr_sent_a = -1, .ptr_sent_b = -1};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int ph_seconds(void) { return M.ph_kind ? (int)((mono_ms() - M.ph_t0) / 1000) : 0; }
static int ph_waiting(void) { return M.ph_kind && M.ph_kind != RDX_PH_ENDED && ph_seconds() >= M.ph_wait_s; }
/* the entries shown, by id: "Mouse" only for a window's input, "Restart" only while the placeholder waits long */
static int item_ids(int *ids)
{
    int n = 0;
    for (int id = 0; id < NITEMS; id++)
        if ((id != IT_MOUSE || (M.input_caps & RDX_MENU_CAP_POINTER)) && ((id != IT_SWITCH && id != IT_QUITBG) || M.nbg))
            ids[n++] = id;
    if (ph_waiting()) ids[n++] = IT_RESTART;
    return n;
}
static int nitems(void) { int ids[NITEMS + 1]; return item_ids(ids); }
static int cur_item(void) { int ids[NITEMS + 1], n = item_ids(ids); return M.sel >= 0 && M.sel < n ? ids[M.sel] : -1; }
static int pos_of(int id) { int ids[NITEMS + 1], n = item_ids(ids); for (int i = 0; i < n; i++) if (ids[i] == id) return i; return 0; }
static int card_on(void) { return M.card_until && mono_ms() < M.card_until; }

/* absolute or captured relative pointer: the setting, else (auto) relative for a mirrored desktop or
 * when the app locks the pointer (the source's hint), absolute otherwise */
static int ptr_relative(void)
{
    if (M.ptr_setting == RDX_PTR_ABSOLUTE) return 0;
    if (M.ptr_setting == RDX_PTR_RELATIVE) return 1;
    return M.ptr_mirror || M.ptr_hint == 1;
}

static void psend(int fd, uint32_t type, int a, int b, int c, int d, int e, const int *fds, int nfd);
static void ptr_push(int force)
{
    if (M.input < 0 || !(M.input_caps & RDX_MENU_CAP_POINTER)) return;
    /* b: 1 = the stream shows the host's cursor (the local one is hidden over the window), 2 = the window's
     * cursor is the host's, the sink sets it (the cursor next to the picture): the input leaves it alone */
    int a = ptr_relative(), b = M.sink_cursor && !M.ptr_mirror ? 2 : M.cursor;
    if (!force && a == M.ptr_sent_a && b == M.ptr_sent_b) return;
    M.ptr_sent_a = a;
    M.ptr_sent_b = b;
    psend(M.input, RDX_MENU_POINTER, a, b, M.ptr_setting, M.ptr_hint, 0, NULL, 0);
    fprintf(stderr, "[menu] pointer: %s (%s%s)\n", a ? "relative, captured" : "absolute",
            M.ptr_setting == RDX_PTR_AUTO ? "auto" : "set",
            M.ptr_setting != RDX_PTR_AUTO ? "" : M.ptr_mirror ? ": a mirrored desktop" : M.ptr_hint == 1 ? ": the app locks the pointer"
            : M.ptr_hint == 0 ? ": the app shows its pointer" : ": no hint from the host");
}

static const char *ptr_text(void)
{
    static char t[80];
    const char *eff = ptr_relative() ? "relative, captured" : "absolute";
    if (M.ptr_setting == RDX_PTR_AUTO)
        snprintf(t, sizeof(t), "auto: %s%s", eff, M.ptr_mirror ? " (desktop)" : M.ptr_hint == 1 ? " (the game locks it)" : "");
    else snprintf(t, sizeof(t), "%s", eff);
    return t;
}

static void psend(int fd, uint32_t type, int a, int b, int c, int d, int e, const int *fds, int nfd)
{
    if (fd < 0) return;
    struct rdx_menu_msg m = {type, a, b, c, d, e};
    struct iovec iov = {.iov_base = &m, .iov_len = sizeof(m)};
    char cbuf[CMSG_SPACE(sizeof(int) * RDX_MENU_NBUF)];
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    if (nfd) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfd);
        struct cmsghdr *c0 = CMSG_FIRSTHDR(&mh);
        c0->cmsg_level = SOL_SOCKET;
        c0->cmsg_type = SCM_RIGHTS;
        c0->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfd);
        memcpy(CMSG_DATA(c0), fds, sizeof(int) * (size_t)nfd);
    }
    if (sendmsg(fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
        fprintf(stderr, "[menu] send to peer failed: %s\n", strerror(errno));
}

static void free_buffers(void)
{
    for (int i = 0; i < RDX_MENU_NBUF; i++) {
        if (M.bmap[i]) munmap(M.bmap[i], (size_t)M.bstride * M.bh);
        if (M.bfd[i] >= 0) close(M.bfd[i]);
        M.bmap[i] = NULL;
        M.bfd[i] = -1;
    }
    M.shown_buf = -1;
}

/* two ARGB8888 memfds for the overlay, sealed against shrinking (udmabuf needs that) */
static int make_buffers(void)
{
    free_buffers();
    rdx_draw_layout(M.mode_w, M.mode_h, &M.bw, &M.bh, &M.px);
    M.bstride = (M.bw * 4 + 255) & ~255u;
    size_t sz = ((size_t)M.bstride * M.bh + 4095) & ~(size_t)4095;
    for (int i = 0; i < RDX_MENU_NBUF; i++) {
        M.bfd[i] = memfd_create("rdx-menu", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (M.bfd[i] < 0 || ftruncate(M.bfd[i], (off_t)sz) || fcntl(M.bfd[i], F_ADD_SEALS, F_SEAL_SHRINK)) return -1;
        M.bmap[i] = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, M.bfd[i], 0);
        if (M.bmap[i] == MAP_FAILED) { M.bmap[i] = NULL; return -1; }
    }
    int x = (int)(M.mode_w - M.bw) / 2, y = (int)(M.mode_h - M.bh) / 2;
    psend(M.sink, RDX_MENU_BUFFERS, (int)M.bw, (int)M.bh, (int)M.bstride, x, y, M.bfd, RDX_MENU_NBUF);
    return 0;
}

static void view(struct rdx_menu_view *v)
{
    memset(v, 0, sizeof(*v));
    char txt[NITEMS + 1][96];
    memset(txt, 0, sizeof(txt));
    v->title = "rdma-display";
    if (M.have_stats) {
        snprintf(v->stats[0], sizeof(v->stats[0]), "%d frames/s on %.1f Hz  \xc2\xb7  arrival \xe2\x86\x92 visible %.2f ms (p50)",
                 M.stats.a, M.stats.e / 1000.0, M.stats.d / 1000.0);
        if (M.vrr && M.vrr_min > 0)
            snprintf(v->stats[1], sizeof(v->stats[1]), "hitches %d/s  \xc2\xb7  dropped %d/s  \xc2\xb7  FreeSync on (%.0f\xe2\x80\x93%.0f Hz)",
                     M.stats.b, M.stats.c, M.vrr_min, M.vrr_max);
        else
            snprintf(v->stats[1], sizeof(v->stats[1]), "hitches %d/s  \xc2\xb7  dropped %d/s  \xc2\xb7  FreeSync %s", M.stats.b,
                     M.stats.c, M.vrr ? "on" : "off");
    } else {
        snprintf(v->stats[0], sizeof(v->stats[0]), "waiting for statistics from the sink");
    }
    snprintf(txt[IT_CURSOR], sizeof(txt[0]), "Cursor            %s", M.cursor ? "on" : "off");
    snprintf(txt[IT_OUTPUT], sizeof(txt[0]), "Output            \xe2\x97\x80 %s \xe2\x96\xb6%s",
             M.nouts ? M.outs[M.out_idx] : "-",
             M.nouts && strcmp(M.outs[M.out_idx], M.cur_out) ? "   (Enter applies)" : "");
    if (M.mon[0])
        snprintf(txt[IT_MONITOR], sizeof(txt[0]), "Monitor there     %s  (%.24s)", M.mon_on ? "on" : "off", M.mon);
    else
        snprintf(txt[IT_MONITOR], sizeof(txt[0]), "Monitor there     -");
    snprintf(txt[IT_MIC], sizeof(txt[0]), "Microphone mute   %s", !M.mic ? "-" : M.mic_muted ? "on (muted)" : "off");
    snprintf(txt[IT_CLIP], sizeof(txt[0]), "Clipboard         %s", !M.clip ? "-" : M.clip_on ? "shared" : "off");
    if (M.nbg) {
        int b = M.bg_idx < M.nbg ? M.bg_idx : 0;
        snprintf(txt[IT_SWITCH], sizeof(txt[0]), "Switch to         %s%.40s%s", M.nbg > 1 ? "\xe2\x97\x80 " : "", M.bg_name[b],
                 M.nbg > 1 ? " \xe2\x96\xb6" : "");
        snprintf(txt[IT_QUITBG], sizeof(txt[0]), "Quit %.40s (in the background)", M.bg_name[b]);
    }
    snprintf(txt[IT_RESUME], sizeof(txt[0]), "Back to the stream");
    /* a mirrored desktop just stops; an app (no output to mirror) keeps running on the host */
    snprintf(txt[IT_QUIT], sizeof(txt[0]), "%s", M.cur_out[0] ? "Disconnect"
             : M.mon[0] ? "End the stream (the app stays on its monitor)" : "Disconnect (the app keeps running)");
    snprintf(txt[IT_QUITAPP], sizeof(txt[0]), "%s", M.cur_out[0] ? "Stop mirroring" : "Quit the app on the other computer");
    if (ph_waiting()) snprintf(txt[IT_RESTART], sizeof(txt[0]), "Restart the host program");
    snprintf(txt[IT_MOUSE], sizeof(txt[0]), "Mouse             %s", ptr_text());
    int ids[NITEMS + 1];
    v->nitems = item_ids(ids);
    for (int i = 0; i < v->nitems; i++) snprintf(v->items[i], sizeof(v->items[i]), "%s", txt[ids[i]]);
    snprintf(v->warn, sizeof(v->warn), "%s", M.link);
    if (M.sel >= v->nitems) M.sel = v->nitems - 1;
    v->sel = M.sel;
    snprintf(v->note, sizeof(v->note), "%s", M.note);
    v->footer = "\xe2\x86\x91\xe2\x86\x93 select  \xc2\xb7  \xe2\x86\x90\xe2\x86\x92 change  \xc2\xb7  "
                "Enter apply  \xc2\xb7  Esc / Ctrl+Alt+Shift+M close";
}

static void arm_timer(int ms)
{
    struct itimerspec it = {.it_value = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L}};
    if (!ms) it.it_value.tv_nsec = 1;
    timerfd_settime(M.tfd, 0, &it, NULL);
}

static void request_draw(void)
{
    if (!M.open && !M.ph_kind && !card_on()) return;
    M.dirty = 1;
    uint64_t now = mono_ms(), next = M.t_last_draw + REDRAW_MS;
    arm_timer(now >= next ? 0 : (int)(next - now));
}

/* the placeholder card; the icon is decoded once per overlay size */
static void ph_render(uint8_t *buf)
{
    int size = rdx_ph_icon_size(M.px);
    if (M.ph_raw && M.ph_isz != size) {
        free(M.ph_icon);
        M.ph_icon = rdx_ph_icon_decode(M.ph_raw, M.ph_rawlen, size, &M.ph_iw, &M.ph_ih);
        M.ph_isz = size;
        if (!M.ph_icon) {                  /* no image stb_image knows (an SVG): the initial instead */
            free(M.ph_raw);
            M.ph_raw = NULL;
        }
    }
    struct rdx_ph_view v = {M.ph_kind ? M.ph_kind : RDX_PH_LINK, M.ph_app, ph_seconds(), ph_waiting(), M.ph_icon, M.ph_iw,
                            M.ph_ih, M.ph_note,
                            M.card_msg[0] && (!M.ph_kind || (M.card_warn && card_on())) ? M.card_msg : M.link};
    rdx_ph_draw(buf, M.bw, M.bh, M.bstride, M.px, &v);
}

static void draw_now(void)
{
    int card = !M.open && !M.ph_kind && card_on();   /* the link card: the menu and the placeholder win */
    if ((!M.open && !M.ph_kind && !card) || !M.dirty || !M.bmap[0] || M.overlay_ok != 1) return;
    int k = M.shown_buf == 0 ? 1 : 0;      /* never paint the buffer on screen */
    int ph = !M.open && !card;             /* the menu wins over the placeholder */
    if (ph || card) {
        ph_render(M.bmap[k]);
    } else {
        struct rdx_menu_view v;
        view(&v);
        rdx_draw_menu(M.bmap[k], M.bw, M.bh, M.bstride, M.px, &v);
    }
    /* the display engine reads RAM, not the CPU caches: write the lines back */
    for (size_t off = 0; off < (size_t)M.bstride * M.bh; off += 64) __builtin_ia32_clflush(M.bmap[k] + off);
    __builtin_ia32_mfence();
    /* the placeholder is shown until the next stream frame: the sink takes it off with that frame;
     * one the host asked for (stall, notice) also over a last frame that is already on screen */
    psend(M.sink, RDX_MENU_SHOW, k, ph ? (M.ph_kind == RDX_PH_STALL || M.ph_kind == RDX_PH_ENDED ? 2 : 1) : 0, 0, 0, 0,
          NULL, 0);
    M.shown_buf = k;
    M.dirty = 0;
    M.t_last_draw = mono_ms();
    if (ph) arm_timer(1000 - (int)((M.t_last_draw - M.ph_t0) % 1000));   /* the seconds tick on */
    else if (card && M.card_until != UINT64_MAX) arm_timer((int)(M.card_until - M.t_last_draw));   /* then off */
}

static void set_open(int open)
{
    if (open && M.overlay_ok != 1) {
        fprintf(stderr, "[menu] Ctrl+Alt+Shift+M: no usable overlay plane, the menu stays closed\n");
        open = 0;
    }
    if (M.open == open) { psend(M.input, RDX_MENU_STATE, open, 0, 0, 0, 0, NULL, 0); return; }
    M.open = open;
    psend(M.input, RDX_MENU_STATE, open, 0, 0, 0, 0, NULL, 0);
    if (open) {
        M.sel = pos_of(IT_RESUME);
        M.note[0] = 0;
        for (int i = 0; i < M.nouts; i++) if (!strcmp(M.outs[i], M.cur_out)) M.out_idx = i;
        M.dirty = 1;
        draw_now();
    } else if (M.ph_kind || card_on()) {
        M.dirty = 1;                       /* still no frame: back to the placeholder (or the link card) */
        draw_now();
    } else {
        psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0);
    }
    fprintf(stderr, "[menu] %s\n", open ? "opened" : "closed");
}

static void toggle_monitor(void)
{
    if (!M.mon[0] || !M.act.monitor) {
        snprintf(M.note, sizeof(M.note), "the app shows on no monitor at the other computer");
        return;
    }
    M.mon_on = !M.mon_on;
    M.act.monitor(M.mon_on);
    snprintf(M.note, sizeof(M.note), "monitor there %s ...", M.mon_on ? "on" : "off");
}

static void toggle_mic(void)
{
    if (!M.mic || !M.act.mic_mute) {
        snprintf(M.note, sizeof(M.note), "the microphone is not sent in this session");
        return;
    }
    M.mic_muted = !M.mic_muted;
    M.act.mic_mute(M.mic_muted);
    snprintf(M.note, sizeof(M.note), "microphone %s", M.mic_muted ? "muted" : "on");
}

static void toggle_clip(void)
{
    if (!M.clip || !M.act.clipboard) {
        snprintf(M.note, sizeof(M.note), "the other computer offers no clipboard");
        return;
    }
    M.clip_on = !M.clip_on;
    M.act.clipboard(M.clip_on);
    snprintf(M.note, sizeof(M.note), "clipboard %s", M.clip_on ? "shared both ways" : "off");
}

/* the menu's "Mouse" entry: auto -> absolute -> relative -> auto (this session) */
static void cycle_mouse(int step)
{
    M.ptr_setting = (M.ptr_setting + step) % 3;
    snprintf(M.note, sizeof(M.note), "mouse %s%s", ptr_text(),
             ptr_relative() ? "  \xc2\xb7  Ctrl+Alt+Shift+Z lets it go" : "");
    ptr_push(0);
}

static void key(int code)
{
    int it = cur_item();
    switch (code) {
    case KEY_UP:   M.sel = (M.sel + nitems() - 1) % nitems(); break;
    case KEY_DOWN: M.sel = (M.sel + 1) % nitems(); break;
    case KEY_ESC:  set_open(0); return;
    case KEY_LEFT: case KEY_RIGHT:
        if (it == IT_OUTPUT && M.nouts) M.out_idx = (M.out_idx + (code == KEY_LEFT ? M.nouts - 1 : 1)) % M.nouts;
        else if (it == IT_CURSOR) { M.cursor = !M.cursor; M.act.update(M.cursor, NULL); snprintf(M.note, sizeof(M.note), "cursor %s ...", M.cursor ? "on" : "off"); }
        else if (it == IT_MONITOR) toggle_monitor();
        else if (it == IT_MIC) toggle_mic();
        else if (it == IT_CLIP) toggle_clip();
        else if (it == IT_MOUSE) cycle_mouse(code == KEY_LEFT ? 2 : 1);
        else if ((it == IT_SWITCH || it == IT_QUITBG) && M.nbg) M.bg_idx = (M.bg_idx + (code == KEY_LEFT ? M.nbg - 1 : 1)) % M.nbg;
        break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        if (it == IT_CURSOR) {
            M.cursor = !M.cursor;
            M.act.update(M.cursor, NULL);
            snprintf(M.note, sizeof(M.note), "cursor %s ...", M.cursor ? "on" : "off");
        } else if (it == IT_OUTPUT && M.nouts) {
            M.act.update(-1, M.outs[M.out_idx]);
            snprintf(M.note, sizeof(M.note), "switching to %s ...", M.outs[M.out_idx]);
        } else if (it == IT_MONITOR) {
            toggle_monitor();
        } else if (it == IT_MIC) {
            toggle_mic();
        } else if (it == IT_CLIP) {
            toggle_clip();
        } else if (it == IT_MOUSE) {
            cycle_mouse(1);
        } else if (it == IT_SWITCH && M.nbg && M.act.switch_app) {
            /* this app goes to the background, that one comes back (the host resumes it) */
            char id[64];
            snprintf(id, sizeof(id), "%s", M.bg_id[M.bg_idx < M.nbg ? M.bg_idx : 0]);
            set_open(0);
            M.act.switch_app(id);
            return;
        } else if (it == IT_QUITBG && M.nbg && M.act.quit_bg) {
            int b = M.bg_idx < M.nbg ? M.bg_idx : 0;
            snprintf(M.note, sizeof(M.note), "quitting %.60s ...", M.bg_name[b]);
            M.act.quit_bg(M.bg_id[b]);
        } else if (it == IT_RESUME) {
            set_open(0);
            return;
        } else if (it == IT_QUIT) {
            set_open(0);
            M.act.quit();
            return;
        } else if (it == IT_QUITAPP) {
            set_open(0);
            if (M.act.quit_app) M.act.quit_app(); else M.act.quit();
            return;
        } else if (it == IT_RESTART && ph_waiting() && M.act.restart_host) {
            set_open(0);
            M.act.restart_host();
            return;
        }
        break;
    default: return;
    }
    request_draw();
}

static void drop_peer(int i)
{
    int fd = M.peer[i];
    epoll_ctl(M.ep, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    if (fd == M.input) { M.input = -1; M.input_caps = 0; if (M.open) set_open(0); }
    if (fd == M.sink) { M.sink = -1; M.overlay_ok = -1; M.open = 0; M.sink_cursor = 0; free_buffers(); }
    M.peer[i] = -1;
}

static void peer_msg(int i, const struct rdx_menu_msg *m)
{
    int fd = M.peer[i];
    switch (m->type) {
    case RDX_MENU_HELLO:
        M.role[i] = m->a;
        if (m->a == RDX_MENU_ROLE_INPUT) {
            M.input = fd;
            M.input_caps = m->b;
            M.ptr_sent_a = M.ptr_sent_b = -1;
            ptr_push(1);                   /* a window's input learns the pointer mode at once */
        }
        if (m->a == RDX_MENU_ROLE_SINK) {
            M.sink = fd;
            M.mode_w = (uint32_t)m->b; M.mode_h = (uint32_t)m->c; M.refresh_mhz = (uint32_t)m->d;
            M.overlay_ok = -1;
            M.have_stats = 0;
            if (make_buffers()) fprintf(stderr, "[menu] cannot create the overlay buffers\n");
        }
        break;
    case RDX_MENU_OVERLAY:
        M.overlay_ok = m->a != 0;
        fprintf(stderr, "[menu] sink overlay %s%s%s\n", m->a ? "ready" : "unavailable: ", m->a ? "" : strerror(m->b), "");
        if (!m->a && M.open) set_open(0);
        if (m->a && M.ph_kind && !M.open) { M.dirty = 1; draw_now(); }     /* the placeholder can show now */
        break;
    case RDX_MENU_FRAME:
        if (m->a == 1 && M.ph_kind) {
            fprintf(stderr, "[menu] first frame after %d s: placeholder off\n", ph_seconds());
            M.ph_kind = 0;
            if (!M.open) M.shown_buf = -1;     /* the sink took it off with that frame */
            if (!M.open && M.card_warn && card_on()) { M.dirty = 1; draw_now(); }   /* the warning stays a while */
        }
        /* a = 0 (older sinks: no frame for RDX_MENU_STALL_MS) is no event: a loading or still screen
         * sends no frames for seconds, a placeholder over it flickered; rdx_menu_stall() */
        break;
    case RDX_MENU_DESKTOP:                 /* keyboard and mouse went to the desktop / came back */
        if (fd == M.input) {
            fprintf(stderr, "[menu] keyboard and mouse: %s\n", m->a ? "desktop" : "stream");
            if (m->a && M.in_session) {
                if (M.open) set_open(0);
                M.card_warn = 0;
                snprintf(M.card_msg, sizeof(M.card_msg), "Desktop \xe2\x80\x93 %s: back to the stream",
                         M.hotkey[0] ? M.hotkey : "Ctrl+Alt+H");
                M.card_until = mono_ms() + LINK_CARD_MS / 2;
                M.dirty = 1;
                draw_now();
            } else if (M.card_msg[0]) {            /* back: the card goes */
                M.card_msg[0] = 0;
                M.card_until = 0;
                if (!M.open && !M.ph_kind && M.shown_buf >= 0) { psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0); M.shown_buf = -1; }
            }
        }
        break;
    case RDX_MENU_POINTER:
        if (fd == M.sink && m->b != M.sink_cursor) {
            M.sink_cursor = m->b == 1;
            fprintf(stderr, "[menu] the host's cursor: %s\n", M.sink_cursor ? "the window's own (next to the picture)"
                    : "in the stream");
            ptr_push(0);
        }
        if (fd == M.sink && m->a != M.ptr_hint) {
            M.ptr_hint = m->a;
            fprintf(stderr, "[menu] the host's pointer hint: %s\n", m->a == 1 ? "relative (the app locks the pointer)"
                    : m->a == 0 ? "absolute" : "none");
            ptr_push(0);
            if (M.open) request_draw();
        }
        break;
    case RDX_MENU_TOGGLE: set_open(!M.open); break;
    case RDX_MENU_KEY:    if (M.open) key(m->a); break;
    case RDX_MENU_MICMUTE:
        toggle_mic();
        if (M.open) request_draw();
        break;
    case RDX_MENU_STATS:
        M.stats = *m;
        M.have_stats = 1;
        request_draw();
        break;
    default: break;
    }
}

int rdx_menu_start(const char *path, const char *ttf, const struct rdx_menu_actions *act)
{
    M.act = *act;
    for (int i = 0; i < MAX_PEERS; i++) M.peer[i] = -1;
    if (rdx_draw_init(ttf)) return -1;
    char dir[108];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) { *sl = 0; mkdir(dir, 0755); }
    unlink(path);
    M.lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (M.lfd < 0 || bind(M.lfd, (struct sockaddr *)&a, sizeof(a)) || chmod(path, 0600) || listen(M.lfd, 8)) {
        fprintf(stderr, "[menu] cannot listen on %s: %s\n", path, strerror(errno));
        return -1;
    }
    M.ep = epoll_create1(EPOLL_CLOEXEC);
    M.tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    struct epoll_event e = {.events = EPOLLIN, .data.u32 = ID_LISTEN};
    epoll_ctl(M.ep, EPOLL_CTL_ADD, M.lfd, &e);
    e.data.u32 = ID_TIMER;
    epoll_ctl(M.ep, EPOLL_CTL_ADD, M.tfd, &e);
    fprintf(stderr, "[menu] in-stream menu socket %s, font %s\n", path, ttf);
    return 0;
}

int rdx_menu_poll_fd(void) { return M.ep; }

void rdx_menu_dispatch(void)
{
    struct epoll_event ev[8];
    int n = M.ep >= 0 ? epoll_wait(M.ep, ev, 8, 0) : 0;
    for (int k = 0; k < n; k++) {
        uint32_t id = ev[k].data.u32;
        if (id == ID_LISTEN) {
            int fd;
            while ((fd = accept4(M.lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
                int i = 0;
                while (i < MAX_PEERS && M.peer[i] >= 0) i++;
                if (i == MAX_PEERS) { close(fd); continue; }
                M.peer[i] = fd;
                M.role[i] = 0;
                struct epoll_event e = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
                epoll_ctl(M.ep, EPOLL_CTL_ADD, fd, &e);
            }
        } else if (id == ID_TIMER) {
            uint64_t x;
            if (read(M.tfd, &x, sizeof(x)) > 0) {
                if (M.ph_kind && !M.open) M.dirty = 1;   /* the placeholder's seconds */
                if (M.card_until && !card_on()) {          /* the link card's time is over */
                    M.card_until = 0;
                    M.card_msg[0] = 0;
                    if (!M.open && !M.ph_kind) { psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0); M.shown_buf = -1; }
                }
                draw_now();
            }
        } else if (id < MAX_PEERS && M.peer[id] >= 0) {
            struct rdx_menu_msg m;
            ssize_t r;
            while ((r = recv(M.peer[id], &m, sizeof(m), MSG_DONTWAIT)) == (ssize_t)sizeof(m)) peer_msg((int)id, &m);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) drop_peer((int)id);
        }
    }
}

void rdx_menu_session(const char *const *outputs, int noutputs, const char *output, int cursor)
{
    M.nouts = 0;
    for (int i = 0; i < noutputs && M.nouts < MAX_OUTS; i++)
        snprintf(M.outs[M.nouts++], sizeof(M.outs[0]), "%s", outputs[i]);
    snprintf(M.cur_out, sizeof(M.cur_out), "%s", output ? output : "");
    M.cursor = cursor;
    M.out_idx = 0;
    for (int i = 0; i < M.nouts; i++) if (!strcmp(M.outs[i], M.cur_out)) M.out_idx = i;
}

void rdx_menu_pointer(int setting, int mirror)
{
    M.ptr_setting = setting >= RDX_PTR_AUTO && setting <= RDX_PTR_RELATIVE ? setting : RDX_PTR_AUTO;
    M.ptr_mirror = mirror;
    M.ptr_hint = -1;
    ptr_push(1);
}

/* the class of a link line (rdx_link_line's wording): 1 hot, 2 down or the card failed, 3 back, 0 nothing */
static int link_class_of(const char *l)
{
    if (!l || !l[0]) return 0;
    if (!strncmp(l, "Link down", 9) || strstr(l, "link is down") || strstr(l, "network card failed") ||
        strstr(l, "network card is not available")) return 2;
    if (!strncmp(l, "The link was down", 17)) return 3;
    return 1;
}

void rdx_menu_display(int vrr, double min_hz, double max_hz)
{
    M.vrr = vrr;
    M.vrr_min = min_hz;
    M.vrr_max = max_hz;
    if (M.open) request_draw();
}

void rdx_menu_hotkey(const char *text)
{
    snprintf(M.hotkey, sizeof(M.hotkey), "%s", text ? text : "");
}

void rdx_menu_link(const char *line)
{
    snprintf(M.link, sizeof(M.link), "%s", line ? line : "");
    int cl = link_class_of(M.link);
    if (cl != M.link_class) {
        M.link_class = cl;
        /* a new kind of news: the card over the stream (down: until the link is back) */
        M.card_until = !cl || !M.in_session ? 0 : cl == 2 ? UINT64_MAX : mono_ms() + LINK_CARD_MS;
        if (!M.card_until && !M.open && !M.ph_kind && M.shown_buf >= 0) {
            psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0);
            M.shown_buf = -1;
        }
        M.card_msg[0] = 0;                 /* the link card replaces a desktop card */
        M.card_warn = 0;
        if (M.card_until) fprintf(stderr, "[menu] link card: %s\n", M.link);
    }
    M.dirty = 1;
    if (M.open || M.ph_kind || card_on()) draw_now();
}

void rdx_menu_background(const char *const *ids, const char *const *names, int n)
{
    char cur[64];
    snprintf(cur, sizeof(cur), "%s", M.nbg && M.bg_idx < M.nbg ? M.bg_id[M.bg_idx] : "");
    M.nbg = 0;
    M.bg_idx = 0;
    for (int i = 0; i < n && M.nbg < MAX_BG; i++) {
        snprintf(M.bg_id[M.nbg], sizeof(M.bg_id[0]), "%s", ids[i]);
        snprintf(M.bg_name[M.nbg], sizeof(M.bg_name[0]), "%s", names[i] && names[i][0] ? names[i] : ids[i]);
        if (!strcmp(ids[i], cur)) M.bg_idx = M.nbg;          /* the choice stays on the same app */
        M.nbg++;
    }
    if (M.open) request_draw();
}

void rdx_menu_warn(const char *text)
{
    if (!text || !text[0]) return;
    snprintf(M.card_msg, sizeof(M.card_msg), "%s", text);
    M.card_warn = 1;
    M.card_until = mono_ms() + WARN_CARD_MS;
    fprintf(stderr, "[menu] warning card: %s\n", text);
    M.dirty = 1;
    if (M.open || M.ph_kind || card_on()) draw_now();
}

void rdx_menu_mic(int available, int muted)
{
    M.mic = available;
    M.mic_muted = muted;
    if (M.open) request_draw();
}

void rdx_menu_clip(int available, int on)
{
    M.clip = available;
    M.clip_on = on;
    if (M.open) request_draw();
}

void rdx_menu_monitor(const char *name)
{
    snprintf(M.mon, sizeof(M.mon), "%s", name ? name : "");
    M.mon_on = 1;
}

void rdx_menu_monitor_result(int ok, const char *msg, int on)
{
    if (ok) {
        M.mon_on = on;
        snprintf(M.note, sizeof(M.note), "monitor there %s", on ? "on" : "off");
    } else {
        M.mon_on = !M.mon_on;                       /* back to what it was */
        snprintf(M.note, sizeof(M.note), "host refused: %s", msg ? msg : "?");
    }
    request_draw();
}

void rdx_menu_result(int ok, const char *msg, int cursor, const char *output)
{
    if (ok) {
        if (cursor >= 0) M.cursor = cursor;
        ptr_push(0);
        if (output && *output) snprintf(M.cur_out, sizeof(M.cur_out), "%s", output);
        snprintf(M.note, sizeof(M.note), "applied: cursor %s, output %s", M.cursor ? "on" : "off", M.cur_out);
    } else {
        snprintf(M.note, sizeof(M.note), "host refused: %s", msg ? msg : "?");
    }
    request_draw();
}

void rdx_menu_placeholder(int kind, const char *app)
{
    M.ph_kind = kind;
    M.in_session = 1;
    M.ph_t0 = mono_ms();
    snprintf(M.ph_app, sizeof(M.ph_app), "%s", app ? app : "");
    free(M.ph_raw);
    M.ph_raw = NULL;
    M.ph_rawlen = 0;
    free(M.ph_icon);
    M.ph_icon = NULL;
    M.ph_isz = 0;
    const char *e = getenv("RDX_PLACEHOLDER_WAIT_S");          /* tests: a shorter wait */
    M.ph_wait_s = e && atoi(e) > 0 ? atoi(e) : RDX_PH_WAIT_S;
    fprintf(stderr, "[menu] start placeholder for %s\n", M.ph_app[0] ? M.ph_app : "the app");
    M.dirty = 1;
    draw_now();
}

void rdx_menu_stall(const char *why)
{
    if (!M.in_session || M.ph_kind) return;
    M.ph_kind = RDX_PH_STALL;              /* until the next frame (the sink takes it off with it) */
    M.ph_t0 = mono_ms();
    fprintf(stderr, "[menu] the host says the picture pauses (%s): placeholder on\n", why ? why : "?");
    M.dirty = 1;
    draw_now();
}

void rdx_menu_notice(const char *text)
{
    if (!M.in_session) return;
    M.ph_kind = RDX_PH_ENDED;              /* the app's name and icon of the start stay */
    M.ph_t0 = mono_ms();
    snprintf(M.ph_note, sizeof(M.ph_note), "%s", text ? text : "");
    fprintf(stderr, "[menu] notice: %s\n", M.ph_note);
    if (M.open) set_open(0);               /* nothing to do in the menu any more */
    M.dirty = 1;
    draw_now();
}

void rdx_menu_placeholder_icon(const char *app, const unsigned char *data, size_t len)
{
    if (!M.ph_kind || !app || strcmp(app, M.ph_app) || !data || !len) return;
    unsigned char *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    free(M.ph_raw);
    M.ph_raw = copy;
    M.ph_rawlen = len;
    M.ph_isz = 0;                          /* decode with the next draw */
    request_draw();
}

void rdx_menu_end(void)
{
    if (M.open) {
        M.open = 0;
        psend(M.input, RDX_MENU_STATE, 0, 0, 0, 0, 0, NULL, 0);
    }
    if (M.ph_kind) psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0);
    if (M.card_until && M.shown_buf >= 0 && !M.ph_kind) psend(M.sink, RDX_MENU_HIDE, 0, 0, 0, 0, 0, NULL, 0);
    M.card_until = 0;
    M.link_class = 0;                      /* the next session shows a standing warning again */
    M.ph_kind = 0;
    M.in_session = 0;
    M.note[0] = 0;
}
