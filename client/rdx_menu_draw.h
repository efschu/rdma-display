/* SPDX-License-Identifier: MIT
 *
 * In-stream menu: CPU renderer of the menu panel into an ARGB8888 buffer with
 * premultiplied alpha (the KMS default blend mode of overlay planes). Text via
 * stb_truetype (third_party/stb) from a system TTF. Drawn only when the menu changes.
 */
#pragma once
#include <stdint.h>

#define RDX_MENU_MAX_ITEMS 12

struct rdx_menu_view {
    const char *title;
    char stats[2][128];                 /* statistics lines, empty = skipped */
    char warn[256];                     /* a warning under them (the link: module hot, link down), empty = none */
    char items[RDX_MENU_MAX_ITEMS][96];
    int nitems, sel;
    char note[128];                     /* last action result, empty = none */
    const char *footer;
};

/* load the font; 0 = ok */
int  rdx_draw_init(const char *ttf_path);
/* panel size and text height for a sink mode */
void rdx_draw_layout(uint32_t mode_w, uint32_t mode_h, uint32_t *w, uint32_t *h, int *px);
/* render the whole panel; stride in bytes */
void rdx_draw_menu(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int px, const struct rdx_menu_view *v);

/* the primitives, for other panels on the same overlay (client/rdx_placeholder.c) */
uint32_t rdx_draw_pm(uint32_t rgb, uint32_t alpha);          /* premultiplied ARGB colour */
void rdx_draw_fill(uint8_t *buf, uint32_t stride, int x0, int y0, int x1, int y1, uint32_t c);
void rdx_draw_blend(uint32_t *dst, uint32_t c, uint32_t coverage);   /* source-over, coverage 0..255 */
/* UTF-8 text with its baseline at y; returns the pen x after it */
int  rdx_draw_text(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int x, int y, int px, uint32_t c,
                   const char *s);
int  rdx_draw_text_width(int px, const char *s);
