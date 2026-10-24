/* SPDX-License-Identifier: MIT
 *
 * Placeholder of the client's sink while an app starts: from session.start (start or resume)
 * until the first real frame, and again when the frames stop for a while (the host
 * reconfigures, the app hangs). Rendered by rdx-clientd into the in-stream menu's overlay
 * buffers (rdx_menu.c) over the sink's black start buffer; the sink takes it off in the same
 * atomic commit that shows the first frame (common/rdx_menu_ipc.h, RDX_MENU_SHOW b = 1).
 *
 * The card: the app's icon (decoded with stb_image, or its initial in a rounded square when
 * there is none), the app's name, "<App> is starting…", the seconds since the start and a
 * moving bar; after RDX_PH_WAIT_S without a frame "Still waiting – check the host" and how
 * to open the menu (Quit the app / Restart the host program).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RDX_PH_WAIT_S 120           /* "still waiting" after this many seconds without a frame */

/* RDX_PH_ENDED: the app ended right after its start, the card shows the host's notice
 * ("Lutris is already open on this PC – close it there and start again") instead of the bar */
/* RDX_PH_LINK: the link card (the network module got hot, the link is down or came back): the
 * app's icon and name and the link line in large letters, over the stream for a few seconds (down:
 * until the link is back) */
enum rdx_ph_kind { RDX_PH_NONE = 0, RDX_PH_START, RDX_PH_RESUME, RDX_PH_STALL, RDX_PH_ENDED, RDX_PH_LINK };

struct rdx_ph_view {
    int kind;
    const char *app;                /* the app's name as the host lists it */
    int seconds;                    /* since the start (or since the frames stopped) */
    int waiting;                    /* seconds >= the "still waiting" limit */
    const uint32_t *icon;           /* premultiplied ARGB, icon_w x icon_h; NULL = initial instead */
    int icon_w, icon_h;
    const char *notice;             /* RDX_PH_ENDED: the host's notice */
    const char *link;               /* the link line (common/rdx_link.h rdx_link_line), NULL / "" = none:
                                     * at the bottom of every card, the text of RDX_PH_LINK */
};

/* edge length of the icon box for menu text height px */
int rdx_ph_icon_size(int px);
/* decode a PNG / JPEG / ... icon and scale it into a size x size box (aspect kept) as
 * premultiplied ARGB; malloc'd, NULL when the data is no image stb_image knows (e.g. SVG) */
uint32_t *rdx_ph_icon_decode(const unsigned char *data, size_t len, int size, int *w, int *h);
/* render the card into the whole overlay buffer (ARGB8888 premultiplied, stride in bytes) */
void rdx_ph_draw(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int px, const struct rdx_ph_view *v);
