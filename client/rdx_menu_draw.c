/* SPDX-License-Identifier: MIT
 * In-stream menu renderer, see rdx_menu_draw.h.
 */
#include "rdx_menu_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb/stb_truetype.h"

static stbtt_fontinfo g_font;
static unsigned char *g_ttf;

int rdx_draw_init(const char *ttf_path)
{
    FILE *f = fopen(ttf_path, "rb");
    if (!f) { perror(ttf_path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_ttf = malloc((size_t)n);
    int ok = g_ttf && fread(g_ttf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok || !stbtt_InitFont(&g_font, g_ttf, stbtt_GetFontOffsetForIndex(g_ttf, 0))) {
        fprintf(stderr, "[menu] cannot use font %s\n", ttf_path);
        return -1;
    }
    return 0;
}

void rdx_draw_layout(uint32_t mode_w, uint32_t mode_h, uint32_t *w, uint32_t *h, int *px)
{
    int p = (int)(mode_h / 38);                    /* 28 px at 1080p, 56 px at 2160p */
    if (p < 14) p = 14;
    /* 30 x 29 line units: title, 2 statistics lines, a warning line, up to 12 entries, note, footer */
    uint32_t pw = (uint32_t)p * 30, ph = (uint32_t)p * 29;
    if (pw > mode_w) pw = mode_w;
    if (ph > mode_h) ph = mode_h;
    *w = pw & ~7u;
    *h = ph & ~1u;
    *px = p;
}

/* premultiplied ARGB colours */
static uint32_t pm(uint32_t rgb, uint32_t a)
{
    uint32_t r = ((rgb >> 16) & 0xff) * a / 255, g = ((rgb >> 8) & 0xff) * a / 255, b = (rgb & 0xff) * a / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

static void fill(uint8_t *buf, uint32_t stride, int x0, int y0, int x1, int y1, uint32_t c)
{
    for (int y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)(buf + (size_t)y * stride);
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

/* source-over of a premultiplied colour scaled by coverage */
static void blend(uint32_t *d, uint32_t c, uint32_t cov)
{
    uint32_t sa = (c >> 24) * cov / 255, ia = 255 - sa;
    uint32_t r = ((c >> 16) & 0xff) * cov / 255 + ((*d >> 16) & 0xff) * ia / 255;
    uint32_t g = ((c >> 8) & 0xff) * cov / 255 + ((*d >> 8) & 0xff) * ia / 255;
    uint32_t b = (c & 0xff) * cov / 255 + (*d & 0xff) * ia / 255;
    uint32_t a = sa + (*d >> 24) * ia / 255;
    *d = a << 24 | r << 16 | g << 8 | b;
}

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xf0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
    else if (c >= 0xe0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
    else if (c >= 0xc0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
    *s = (const char *)p;
    return c;
}

/* draw a UTF-8 string with its baseline at y; returns the pen x after it */
static int text(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int x, int y, int px, uint32_t color,
                const char *s)
{
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    uint32_t prev = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (prev) x += (int)(scale * (float)stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)cp));
        int adv, lsb, bw, bh, xo, yo;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
        unsigned char *bm = stbtt_GetCodepointBitmap(&g_font, 0, scale, (int)cp, &bw, &bh, &xo, &yo);
        for (int j = 0; bm && j < bh; j++) {
            int yy = y + yo + j;
            if (yy < 0 || yy >= (int)h) continue;
            uint32_t *row = (uint32_t *)(buf + (size_t)yy * stride);
            for (int i = 0; i < bw; i++) {
                int xx = x + xo + i;
                if (xx >= 0 && xx < (int)w && bm[j * bw + i]) blend(&row[xx], color, bm[j * bw + i]);
            }
        }
        stbtt_FreeBitmap(bm, NULL);
        x += (int)(scale * (float)adv);
        prev = cp;
    }
    return x;
}

uint32_t rdx_draw_pm(uint32_t rgb, uint32_t alpha) { return pm(rgb, alpha); }
void rdx_draw_fill(uint8_t *buf, uint32_t stride, int x0, int y0, int x1, int y1, uint32_t c)
{
    fill(buf, stride, x0, y0, x1, y1, c);
}
void rdx_draw_blend(uint32_t *dst, uint32_t c, uint32_t coverage) { blend(dst, c, coverage); }
int rdx_draw_text(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int x, int y, int px, uint32_t c,
                  const char *s)
{
    return text(buf, w, h, stride, x, y, px, c, s);
}

int rdx_draw_text_width(int px, const char *s)
{
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    uint32_t prev = 0;
    int x = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (prev) x += (int)(scale * (float)stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)cp));
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
        x += (int)(scale * (float)adv);
        prev = cp;
    }
    return x;
}

/* s cut to max_w at px with a trailing ellipsis (whole UTF-8 characters) */
static void fit_line(char *out, size_t n, const char *s, int px, int max_w)
{
    snprintf(out, n, "%s", s);
    size_t len = strlen(out);
    while (len > 0 && rdx_draw_text_width(px, out) > max_w) {
        do len--; while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);
        snprintf(out + len, n - len, "\xe2\x80\xa6");
    }
}

void rdx_draw_menu(uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int px, const struct rdx_menu_view *v)
{
    const uint32_t bg = pm(0x16181d, 232), line = pm(0x3a3f4b, 255), fg = pm(0xf2f3f5, 255),
                   dim = pm(0xa0a6b2, 255), acc = pm(0x4f8cff, 255), sel_bg = pm(0x2a3550, 255);
    int pad = px, row = px * 3 / 2;
    fill(buf, stride, 0, 0, (int)w, (int)h, bg);
    /* 2 px frame */
    fill(buf, stride, 0, 0, (int)w, 2, line);
    fill(buf, stride, 0, (int)h - 2, (int)w, (int)h, line);
    fill(buf, stride, 0, 0, 2, (int)h, line);
    fill(buf, stride, (int)w - 2, 0, (int)w, (int)h, line);
    int y = pad + px;
    text(buf, w, h, stride, pad, y, px * 6 / 5, fg, v->title);
    y += row / 2;
    fill(buf, stride, pad, y, (int)w - pad, y + 2, line);
    y += row;
    for (int i = 0; i < 2; i++)
        if (v->stats[i][0]) { text(buf, w, h, stride, pad, y, px * 4 / 5, dim, v->stats[i]); y += row * 4 / 5; }
    if (v->warn[0]) {                              /* the link (module hot, link down): warning colour */
        char line[264];
        fit_line(line, sizeof(line), v->warn, px * 4 / 5, (int)w - 2 * pad);
        text(buf, w, h, stride, pad, y, px * 4 / 5, pm(0xf0b429, 255), line);
        y += row * 4 / 5;
    }
    y += row / 3;
    for (int i = 0; i < v->nitems; i++) {
        if (i == v->sel) {
            fill(buf, stride, pad / 2, y - px, (int)w - pad / 2, y + px / 2, sel_bg);
            fill(buf, stride, pad / 2, y - px, pad / 2 + px / 5, y + px / 2, acc);
        }
        text(buf, w, h, stride, pad + px / 2, y, px, i == v->sel ? fg : dim, v->items[i]);
        y += row;
    }
    if (v->note[0]) text(buf, w, h, stride, pad, (int)h - pad - px * 3 / 2, px * 4 / 5, acc, v->note);
    if (v->footer) text(buf, w, h, stride, pad, (int)h - pad / 2 - px / 4, px * 2 / 3, dim, v->footer);
}
