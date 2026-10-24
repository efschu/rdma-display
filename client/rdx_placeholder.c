/* SPDX-License-Identifier: MIT
 * Placeholder card of the client's sink while an app starts, see rdx_placeholder.h.
 */
#include "rdx_placeholder.h"
#include "rdx_menu_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb/stb_image.h"

int rdx_ph_icon_size(int px) { return px * 5; }

uint32_t *rdx_ph_icon_decode(const unsigned char *data, size_t len, int size, int *w, int *h)
{
    int iw, ih, n;
    if (!data || !len || len > (1u << 24) || size <= 0) return NULL;
    unsigned char *img = stbi_load_from_memory(data, (int)len, &iw, &ih, &n, 4);
    if (!img) return NULL;
    /* fit into size x size, aspect kept; area average when shrinking, nearest when growing */
    double s = (double)size / (iw > ih ? iw : ih);
    int ow = (int)(iw * s + 0.5), oh = (int)(ih * s + 0.5);
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    uint32_t *out = malloc((size_t)ow * (size_t)oh * 4);
    for (int y = 0; out && y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int x0 = (int)(x / s), x1 = (int)((x + 1) / s), y0 = (int)(y / s), y1 = (int)((y + 1) / s);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            if (x1 > iw) x1 = iw;
            if (y1 > ih) y1 = ih;
            double r = 0, g = 0, b = 0, a = 0;
            int cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char *p = img + ((size_t)yy * iw + xx) * 4;
                    double pa = p[3] / 255.0;            /* average premultiplied: no dark fringes */
                    r += p[0] * pa; g += p[1] * pa; b += p[2] * pa; a += p[3];
                    cnt++;
                }
            if (!cnt) cnt = 1;
            uint32_t A = (uint32_t)(a / cnt + 0.5), R = (uint32_t)(r / cnt + 0.5), G = (uint32_t)(g / cnt + 0.5),
                     B = (uint32_t)(b / cnt + 0.5);
            if (R > A) R = A;
            if (G > A) G = A;
            if (B > A) B = A;
            out[(size_t)y * ow + x] = A << 24 | R << 16 | G << 8 | B;
        }
    stbi_image_free(img);
    if (out) { *w = ow; *h = oh; }
    return out;
}

/* s cut down to fit max_w at px, with "…" when it was cut (whole UTF-8 characters only) */
static void fit(char *out, size_t n, const char *s, int px, int max_w)
{
    snprintf(out, n, "%s", s ? s : "");
    if (rdx_draw_text_width(px, out) <= max_w) return;
    size_t len = strlen(out);
    while (len > 0) {
        do len--; while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s\xe2\x80\xa6", (int)len, out);
        if (rdx_draw_text_width(px, tmp) <= max_w || len == 0) { snprintf(out, n, "%s", tmp); return; }
    }
}

static void centred(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int y, int px, uint32_t c, const char *s,
                    int max_w)
{
    char t[256];
    fit(t, sizeof(t), s, px, max_w);
    rdx_draw_text(buf, w, h, stride, ((int)w - rdx_draw_text_width(px, t)) / 2, y, px, c, t);
}

/* a rounded square with the app's initial: the icon when the host has none */
static void initial(uint8_t *buf, uint32_t stride, uint32_t w, uint32_t h, int x0, int y0, int size, int px,
                    const char *app)
{
    const uint32_t box = rdx_draw_pm(0x2a3550, 255), fg = rdx_draw_pm(0xf2f3f5, 255);
    double rad = size * 0.22;
    for (int y = 0; y < size; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)(y0 + y) * stride);
        for (int x = 0; x < size; x++) {
            /* distance outside the rounded corner, 1 px anti-aliased edge */
            double dx = fmax(fmax(rad - (x + 0.5), (x + 0.5) - (size - rad)), 0);
            double dy = fmax(fmax(rad - (y + 0.5), (y + 0.5) - (size - rad)), 0);
            double cov = fmin(fmax(rad + 0.5 - sqrt(dx * dx + dy * dy), 0), 1);
            if (cov > 0) rdx_draw_blend(&row[x0 + x], box, (uint32_t)(cov * 255 + 0.5));
        }
    }
    char ch[8] = "?";
    if (app && app[0]) {
        size_t l = 1;
        while (app[l] && ((unsigned char)app[l] & 0xc0) == 0x80 && l < 4) l++;
        snprintf(ch, sizeof(ch), "%.*s", (int)l, app);
        if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] = (char)(ch[0] - 32);
    }
    int tp = size * 3 / 5;
    rdx_draw_text(buf, w, h, stride, x0 + (size - rdx_draw_text_width(tp, ch)) / 2, y0 + size / 2 + tp * 7 / 20, tp, fg, ch);
    (void)px;
}

void rdx_ph_draw(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int px, const struct rdx_ph_view *v)
{
    const uint32_t bg = rdx_draw_pm(0x101216, 244), line = rdx_draw_pm(0x3a3f4b, 255), fg = rdx_draw_pm(0xf2f3f5, 255),
                   dim = rdx_draw_pm(0xa0a6b2, 255), acc = rdx_draw_pm(0x4f8cff, 255), warn = rdx_draw_pm(0xf0b429, 255),
                   track = rdx_draw_pm(0x2a2e37, 255);
    rdx_draw_fill(buf, stride, 0, 0, (int)w, (int)h, bg);
    rdx_draw_fill(buf, stride, 0, 0, (int)w, 2, line);
    rdx_draw_fill(buf, stride, 0, (int)h - 2, (int)w, (int)h, line);
    rdx_draw_fill(buf, stride, 0, 0, 2, (int)h, line);
    rdx_draw_fill(buf, stride, (int)w - 2, 0, (int)w, (int)h, line);
    int max_w = (int)w - 2 * px;
    const char *app = v->app && v->app[0] ? v->app : "The app";
    /* the link at the bottom, the same place on every card */
    if (v->link && v->link[0] && v->kind != RDX_PH_LINK)
        centred(buf, w, h, stride, (int)h - px, px * 2 / 3, warn, v->link, max_w);

    /* icon, centred at the top */
    int size = rdx_ph_icon_size(px);
    if (size > (int)h * 2 / 5) size = (int)h * 2 / 5;
    int bh = px / 5 > 3 ? px / 5 : 3;
    /* centred vertically, with the room of the "still waiting" lines kept: nothing moves at 120 s */
    int content = size + px * 2 + px * 3 / 2 + px + bh + px * 3 / 2 + px * 3 / 2 + px * 5 / 4 + px / 2;
    int top = ((int)h - content) / 2;
    if (top < px / 2) top = px / 2;
    if (v->icon && v->icon_w > 0 && v->icon_h > 0 && v->icon_w <= size && v->icon_h <= size) {
        int ix = ((int)w - v->icon_w) / 2, iy = top + (size - v->icon_h) / 2;
        for (int y = 0; y < v->icon_h; y++) {
            uint32_t *row = (uint32_t *)(buf + (size_t)(iy + y) * stride);
            for (int x = 0; x < v->icon_w; x++) {
                uint32_t p = v->icon[(size_t)y * v->icon_w + x];
                if (p >> 24) rdx_draw_blend(&row[ix + x], p, 255);
            }
        }
    } else {
        initial(buf, stride, w, h, ((int)w - size) / 2, top, size, px, app);
    }

    /* name, what happens, seconds, a bar that moves once per second */
    int y = top + size + px * 2;
    centred(buf, w, h, stride, y, px * 5 / 4, fg, app, max_w);
    y += px * 3 / 2;
    if (v->kind == RDX_PH_LINK) {
        /* the link line in two lines at ": " when it is too wide, and what happens next */
        char a[256], b[256] = "";
        snprintf(a, sizeof(a), "%s", v->link && v->link[0] ? v->link : "The network link");
        char *colon = strstr(a, ": "), *paren = strstr(a, " (");
        if (rdx_draw_text_width(px, a) > max_w && (colon || paren)) {
            char *cut = colon ? colon + 1 : paren;      /* "...: | rest" or "... | (details)" */
            snprintf(b, sizeof(b), "%s", cut + 1);
            *cut = 0;
        }
        centred(buf, w, h, stride, y, px, warn, a, max_w);
        if (b[0]) {
            y += px * 5 / 4;
            centred(buf, w, h, stride, y, px, warn, b, max_w);
        }
        y += px * 2;
        centred(buf, w, h, stride, y, px * 2 / 3, dim,
                !strncmp(v->link ? v->link : "", "Link down", 9) ? "The picture comes back with the link"
                                                                 : "Ctrl+Alt+Shift+M: menu", max_w);
        return;
    }
    if (v->kind == RDX_PH_ENDED) {
        /* the host's notice instead of bar and seconds; too wide: in two lines at its dash */
        char a[256], b[256] = "";
        snprintf(a, sizeof(a), "%s", v->notice && v->notice[0] ? v->notice : "The app ended");
        char *dash = strstr(a, " \xe2\x80\x93 ");
        if (rdx_draw_text_width(px, a) > max_w && dash) {
            snprintf(b, sizeof(b), "%s", dash + 5);
            *dash = 0;
        }
        centred(buf, w, h, stride, y, px, warn, a, max_w);
        if (b[0]) {
            y += px * 5 / 4;
            centred(buf, w, h, stride, y, px, warn, b, max_w);
        }
        y += px * 2;
        centred(buf, w, h, stride, y, px * 2 / 3, dim, "The stream ends in a moment", max_w);
        return;
    }
    /* a long name is cut, never what happens to it */
    const char *pre = v->kind == RDX_PH_STALL ? "Waiting for the picture of " : "",
               *post = v->kind == RDX_PH_RESUME ? " is resuming\xe2\x80\xa6"
                     : v->kind == RDX_PH_STALL ? "\xe2\x80\xa6" : " is starting\xe2\x80\xa6";
    char name[256], line1[512];
    snprintf(line1, sizeof(line1), "%s%s", pre, post);
    fit(name, sizeof(name), app, px, max_w - rdx_draw_text_width(px, line1));
    snprintf(line1, sizeof(line1), "%s%s%s", pre, name, post);
    centred(buf, w, h, stride, y, px, dim, line1, max_w);
    y += px;
    int bw = (int)w / 2, bx = ((int)w - bw) / 2;
    rdx_draw_fill(buf, stride, bx, y, bx + bw, y + bh, track);
    int seg = bw / 4, pos = (v->seconds % 8) * (bw - seg) / 7;
    rdx_draw_fill(buf, stride, bx + pos, y, bx + pos + seg, y + bh, v->waiting ? warn : acc);
    y += bh + px * 3 / 2;
    char secs[64];
    if (v->seconds < 120) snprintf(secs, sizeof(secs), "%d s", v->seconds);
    else snprintf(secs, sizeof(secs), "%d min %02d s", v->seconds / 60, v->seconds % 60);
    centred(buf, w, h, stride, y, px * 4 / 5, dim, secs, max_w);

    if (v->waiting) {
        y += px * 3 / 2;
        centred(buf, w, h, stride, y, px, warn, "Still waiting \xe2\x80\x93 check the host", max_w);
        y += px * 5 / 4;
        centred(buf, w, h, stride, y, px * 2 / 3, dim,
                "Ctrl+Alt+Shift+M: menu  \xc2\xb7  Quit the app  \xc2\xb7  Restart the host program", max_w);
    }
}
