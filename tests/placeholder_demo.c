/* Look test of the start placeholder on a real sink, without rdx-clientd and without a host:
 * listens on a menu socket the way rdx-clientd does (client/rdx_menu.c), starts the placeholder
 * for APP (icon from FILE, any image stb_image reads; none = the app's initial) and runs for
 * SECONDS; the menu module logs what the sink reports (overlay ready, first frame, stall).
 * Start the sink with --menu-sock SOCK after this one; frames from any source end the placeholder.
 *   tests/placeholder_demo SOCK APP [icon-file|-] [seconds] [font]
 * RDX_PLACEHOLDER_WAIT_S shortens the 2 min until "Still waiting".
 * build: cc -O2 -o tests/placeholder_demo tests/placeholder_demo.c client/rdx_menu.c client/rdx_menu_draw.c \
 *        client/rdx_placeholder.c -lm */
#include "../client/rdx_menu.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void none(void) {}
static void none_update(int c, const char *o) { (void)c; (void)o; }
static void none_monitor(int on) { (void)on; }
static void restart(void) { fprintf(stderr, "[demo] menu: restart the host program\n"); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s SOCK APP [icon-file|-] [seconds] [font]\n", argv[0]); return 2; }
    const char *icon = argc > 3 ? argv[3] : "-";
    int secs = argc > 4 ? atoi(argv[4]) : 30;
    const char *font = argc > 5 ? argv[5] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    struct rdx_menu_actions act = {none, none, none_update, none_monitor, restart};
    if (rdx_menu_start(argv[1], font, &act)) return 1;
    rdx_menu_placeholder(RDX_PH_START, argv[2]);
    if (strcmp(icon, "-")) {
        FILE *f = fopen(icon, "rb");
        static unsigned char data[1 << 20];
        size_t n = f ? fread(data, 1, sizeof(data), f) : 0;
        if (f) fclose(f);
        if (n) rdx_menu_placeholder_icon(argv[2], data, n);
        fprintf(stderr, "[demo] icon %s: %zu bytes\n", icon, n);
    }
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        long el = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= secs * 1000L) break;
        struct pollfd p = {rdx_menu_poll_fd(), POLLIN, 0};
        if (poll(&p, 1, 200) > 0) rdx_menu_dispatch();
    }
    rdx_menu_end();
    return 0;
}
