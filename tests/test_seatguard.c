/* Unit test for host/rdx_seatguard (no DRM device, no logind needed): which logind sessions
 * keep rdx-hostd off a card's monitors, and the decision from sessions + DRM master.
 *   cc -O2 -Wall -Wextra tests/test_seatguard.c host/rdx_seatguard.c -o t && ./t */
#include "../host/rdx_seatguard.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void)
{
    char why[200];
    /* a desktop host: GNOME on Wayland, seat0, plus the root ssh sessions */
    struct rdx_session_info desk[] = {
        {"2", "wayland", "seat0", "active"}, {"16901", "tty", "", "active"}, {"c1", "unspecified", "", "online"}};
    CHECK(rdx_sessions_block_seat(desk, 3, "seat0", why, sizeof(why)) == 1 && strstr(why, "wayland"),
          "a Wayland desktop on seat0 blocks seat0 (%s)", why);
    CHECK(rdx_sessions_block_seat(desk, 3, "seat1", why, sizeof(why)) == 0, "it does not block another seat");
    /* seat container: only the lingering user manager and ssh */
    struct rdx_session_info ct[] = {{"c1", "unspecified", "", "online"}, {"5", "tty", "", "active"}};
    CHECK(rdx_sessions_block_seat(ct, 2, "seat0", why, sizeof(why)) == 0, "a seat container without a desktop is free");
    struct rdx_session_info x11[] = {{"3", "x11", "seat0", "online"}};
    CHECK(rdx_sessions_block_seat(x11, 1, "seat0", why, sizeof(why)) == 1, "an X11 desktop in the background (online) blocks");
    struct rdx_session_info closing[] = {{"4", "wayland", "seat0", "closing"}};
    CHECK(rdx_sessions_block_seat(closing, 1, "seat0", why, sizeof(why)) == 0, "a closing session does not");
    struct rdx_session_info headless[] = {{"6", "wayland", "", "active"}};
    CHECK(rdx_sessions_block_seat(headless, 1, "seat0", why, sizeof(why)) == 0,
          "a headless (seatless) remote desktop does not own a monitor");

    /* the decision */
    CHECK(rdx_local_decide(1, "a wayland desktop session (2) runs on seat0", 0, "/dev/dri/card1", why, sizeof(why)) == 1 &&
          strstr(why, "desktop"), "session blocks: refused (%s)", why);
    CHECK(rdx_local_decide(0, "", 1, "/dev/dri/card1", why, sizeof(why)) == 1 && strstr(why, "DRM master"),
          "another DRM master: refused (%s)", why);
    CHECK(rdx_local_decide(0, "", -1, "/dev/dri/card9", why, sizeof(why)) == 1, "card cannot be opened: refused (%s)", why);
    CHECK(rdx_local_decide(0, "", 0, "/dev/dri/card2", why, sizeof(why)) == 0 && !why[0], "no desktop, no master: free");

    /* the live check on a card that does not exist must refuse, never pass */
    CHECK(rdx_card_free_for_local("/dev/dri/card-none", why, sizeof(why)) != 0, "missing card: refused (%s)", why);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
