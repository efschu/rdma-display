/* The window client's menu without a display or a host: the real client/rdx_menu.c on its socket,
 * the real p0b/rdx_winmenu.h as the window sink (a fake GL import that only maps the buffers) and a
 * fake window input (p4/rdx_input_win's side: HELLO with RDX_MENU_CAP_POINTER).
 *   start placeholder until the first frame, the pointer mode (auto / hint / setting / menu entry),
 *   the "Mouse" entry only for a window input, the link card (hot: timed, down: until it changes).
 *   tests/test_window_menu [font.ttf]     exit 0 = PASS */
#define _GNU_SOURCE
#include "../client/rdx_menu.h"
#include "../common/rdx_menu_ipc.h"
#include "../p0b/rdx_winmenu.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int fails, imported;
static uint8_t *maps[RDX_MENU_NBUF];
static uint32_t mstride, mh_;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static void act_quit(void) {}
static void act_update(int c, const char *o) { (void)c; (void)o; }
static void act_monitor(int on) { (void)on; }
static void act_restart(void) {}

/* the sink's GL import, here: keep a read-only mapping to look at what clientd drew */
static int fake_import(void *user, int i, int memfd, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)user; (void)w;
    struct stat st;
    if (fstat(memfd, &st)) return -1;
    maps[i] = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, memfd, 0);
    mstride = stride;
    mh_ = h;
    imported++;
    return maps[i] == MAP_FAILED ? -1 : 0;
}

static struct rdx_winmenu W;
static int input = -1;
static struct rdx_menu_msg in_msgs[64];
static int n_in;

/* run clientd's menu loop and the sink's handler for ms; what the input gets is collected */
static int pump(int ms)
{
    int changed = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms) break;
        struct pollfd p[3] = {{rdx_menu_poll_fd(), POLLIN, 0}, {W.sock, POLLIN, 0}, {input, POLLIN, 0}};
        poll(p, 3, (int)(ms - el));
        if (p[0].revents) rdx_menu_dispatch();
        if (p[1].revents && rdx_winmenu_handle(&W, fake_import, NULL) > 0) changed = 1;
        struct rdx_menu_msg m;
        while (input >= 0 && recv(input, &m, sizeof(m), MSG_DONTWAIT) == (ssize_t)sizeof(m))
            if (n_in < 64) in_msgs[n_in++] = m;
    }
    return changed;
}

static void in_send(uint32_t t, int a, int b)
{
    struct rdx_menu_msg m = {t, a, b, 0, 0, 0};
    send(input, &m, sizeof(m), 0);
}

/* the last POINTER the input got: a (relative), -1 = none */
static int last_ptr(int *b)
{
    for (int i = n_in - 1; i >= 0; i--)
        if (in_msgs[i].type == RDX_MENU_POINTER) { if (b) *b = in_msgs[i].b; return in_msgs[i].a; }
    return -1;
}

static long lit_pixels(int k)
{
    long n = 0;
    if (k < 0 || !maps[k]) return 0;
    for (uint32_t y = 0; y < mh_; y++)
        for (uint32_t x = 0; x < mstride / 4; x++) n += (((uint32_t *)(maps[k] + (size_t)y * mstride))[x] >> 24) > 0;
    return n;
}

int main(int argc, char **argv)
{
    const char *font = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    char path[108];
    snprintf(path, sizeof(path), "/tmp/rdx-test-wmenu-%d.sock", (int)getpid());
    static const struct rdx_menu_actions act = {act_quit, act_quit, act_update, act_monitor, act_restart, NULL};
    if (rdx_menu_start(path, font, &act)) { printf("test_window_menu: SKIP (no font %s)\n", font); return 0; }

    /* the window sink connects, gets the buffers, imports them */
    check(!rdx_winmenu_init(&W, path, 1920, 1080, 60000), "window sink connects to the menu socket");
    pump(150);
    check(imported == 2 && W.usable && W.w > 0 && W.h > 0, "sink: two menu buffers imported (memfd), overlay usable");

    /* session start: the placeholder until the first frame */
    const char *outs[1] = {"DP-1"};
    rdx_menu_session(outs, 0, "", 1);
    rdx_menu_pointer(RDX_PTR_AUTO, 0);
    rdx_menu_placeholder(RDX_PH_START, "vkcube");
    int ch = pump(150);
    int cur = rdx_winmenu_current(&W);
    check(ch && cur >= 0 && W.until_frame && lit_pixels(cur) > 1000, "placeholder shown before the first frame");
    rdx_winmenu_frame(&W);
    check(rdx_winmenu_current(&W) < 0, "the first frame takes it off (in that draw)");
    pump(100);

    /* the window input: pointer mode at once (auto, no hint: absolute; the stream shows the cursor) */
    {
        int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
        if (connect(s, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 2; }
        input = s;
    }
    in_send(RDX_MENU_HELLO, RDX_MENU_ROLE_INPUT, RDX_MENU_CAP_POINTER);
    pump(150);
    int b = -1;
    check(last_ptr(&b) == 0 && b == 1, "input: absolute, host cursor in the stream (auto, no hint)");
    rdx_winmenu_hint(&W, 1);                  /* the frame header says: the app locks the pointer */
    pump(150);
    check(last_ptr(NULL) == 1, "hint relative -> input: relative, captured");
    rdx_winmenu_hint(&W, 0);
    pump(150);
    check(last_ptr(NULL) == 0, "hint absolute -> input: absolute");
    /* the cursor next to the picture: the sink makes the host's cursor the window's own, the input keeps off it */
    rdx_winmenu_cursor(&W, 1);
    pump(150);
    b = -1;
    check(last_ptr(&b) == 0 && b == 2, "sink shows the host's cursor -> input: b = 2, the window's cursor is the sink's");
    rdx_winmenu_hint(&W, 1);
    pump(150);
    b = -1;
    check(last_ptr(&b) == 1 && b == 2, "a hint after that keeps b = 2 (relative: the pointer grab hides it)");
    rdx_winmenu_hint(&W, 0);
    rdx_winmenu_cursor(&W, 0);
    pump(150);
    b = -1;
    check(last_ptr(&b) == 0 && b == 1, "the channel gone -> the stream's cursor again (b = 1, the local one hidden)");
    rdx_menu_pointer(RDX_PTR_RELATIVE, 0);
    pump(100);
    check(last_ptr(NULL) == 1, "setting relative -> relative without a hint");
    rdx_menu_pointer(RDX_PTR_AUTO, 1);
    pump(100);
    check(last_ptr(NULL) == 1, "auto + mirrored desktop -> relative");
    rdx_menu_pointer(RDX_PTR_AUTO, 0);
    pump(100);

    /* the menu: hotkey, the "Mouse" entry (4 up from "Back to the stream": Mic, Mouse ...), Enter cycles */
    in_send(RDX_MENU_TOGGLE, 0, 0);
    pump(150);
    int opened = 0;
    for (int i = 0; i < n_in; i++) if (in_msgs[i].type == RDX_MENU_STATE && in_msgs[i].a == 1) opened = 1;
    check(opened && rdx_winmenu_current(&W) >= 0, "Ctrl+Alt+Shift+M opens the menu over the window");
    in_send(RDX_MENU_KEY, KEY_UP, 1);        /* from "Back to the stream" up to "Mouse" */
    pump(60);
    int before = n_in;
    in_send(RDX_MENU_KEY, KEY_ENTER, 1);     /* auto -> absolute: no change of the mode itself */
    in_send(RDX_MENU_KEY, KEY_ENTER, 1);     /* absolute -> relative */
    pump(150);
    int got_rel = 0;
    for (int i = before; i < n_in; i++) if (in_msgs[i].type == RDX_MENU_POINTER && in_msgs[i].a == 1 && in_msgs[i].c == RDX_PTR_RELATIVE) got_rel = 1;
    check(got_rel, "menu entry \"Mouse\" cycles auto -> absolute -> relative");
    in_send(RDX_MENU_KEY, KEY_ESC, 1);
    pump(150);
    check(rdx_winmenu_current(&W) < 0, "Esc closes the menu (overlay off)");

    /* the link: hot -> a timed card; down -> until it changes; all well -> off */
    rdx_menu_link("The other computer's network module: 53.6 \xc2\xb0""C (warning at 53, off at 58 \xc2\xb0""C)");
    pump(150);
    cur = rdx_winmenu_current(&W);
    check(cur >= 0 && !W.until_frame && lit_pixels(cur) > 1000, "module hot: link card over the stream");
    rdx_menu_link("Link down: the other computer's network module overheated (53.9 \xc2\xb0""C, warning at 53 \xc2\xb0""C)");
    pump(150);
    check(rdx_winmenu_current(&W) >= 0, "link down: card stays");
    rdx_menu_link("");
    pump(150);
    check(rdx_winmenu_current(&W) < 0, "all well again: card off");
    rdx_menu_link("This computer's network card failed (likely overheated): power-cycle needed");
    pump(150);
    check(rdx_winmenu_current(&W) >= 0 && !W.until_frame && lit_pixels(rdx_winmenu_current(&W)) > 1000, "network card failed: card over the stream");
    rdx_menu_link("");
    pump(150);
    check(rdx_winmenu_current(&W) < 0, "the card answers again: card off");

    /* the KMS input (no capability): no pointer messages, no "Mouse" entry */
    rdx_menu_end();
    close(input);
    n_in = 0;
    {
        int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
        if (connect(s, (struct sockaddr *)&a, sizeof(a))) { perror("connect"); return 2; }
        input = s;
    }
    pump(80);
    in_send(RDX_MENU_HELLO, RDX_MENU_ROLE_INPUT, 0);
    rdx_menu_pointer(RDX_PTR_AUTO, 0);
    pump(150);
    check(last_ptr(NULL) == -1, "an input without RDX_MENU_CAP_POINTER (KMS) gets no pointer messages");

    unlink(path);
    printf("test_window_menu: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
