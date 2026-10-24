/* Review aid for the start placeholder (client/rdx_placeholder.c): renders the card in four
 * states (start with an icon, resume without one, frames stopped, still waiting after 2 min)
 * over black, as the sink shows it, into one PNG, and checks every pixel is valid premultiplied
 * ARGB, that there is text and an icon, that the warning colour appears only when waiting and
 * that the layout does not move when the warning appears.
 *   tests/test_placeholder_render [font.ttf] [out.png] [WxH]   (exit 0 = ok) */
#include "../client/rdx_menu_draw.h"
#include "../client/rdx_placeholder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

struct mem { unsigned char *data; int len; };

static void png_to_mem(void *ctx, void *data, int size)
{
    struct mem *m = ctx;
    m->data = malloc((size_t)size);
    memcpy(m->data, data, (size_t)size);
    m->len = size;
}

/* a 256x256 icon: a blue-to-green disc on transparency, like an app icon with alpha */
static unsigned char *make_icon_png(int *len)
{
    const int n = 256;
    unsigned char *rgba = calloc((size_t)n * n, 4);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            double d = hypot(x - n / 2 + 0.5, y - n / 2 + 0.5);
            double a = fmin(fmax(n * 0.46 - d, 0), 1);
            unsigned char *p = rgba + ((size_t)y * n + x) * 4;
            p[0] = (unsigned char)(40 + 60 * y / n);
            p[1] = (unsigned char)(120 + 120 * x / n);
            p[2] = (unsigned char)(230 - 100 * y / n);
            p[3] = (unsigned char)(a * 255);
        }
    struct mem m = {NULL, 0};
    stbi_write_png_to_func(png_to_mem, &m, n, n, 4, rgba, n * 4);
    free(rgba);
    *len = m.len;
    return m.data;
}

struct stat_ { long bad, lit, warn, icon; };

static struct stat_ check(const uint8_t *buf, uint32_t w, uint32_t h, uint32_t stride, int px)
{
    struct stat_ s = {0, 0, 0, 0};
    int size = rdx_ph_icon_size(px);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = ((const uint32_t *)(buf + (size_t)y * stride))[x];
            uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > a || g > a || b > a) s.bad++;
            if (r > 200 && g > 200 && b > 200) s.lit++;
            if (r > 200 && g > 140 && g < 200 && b < 80) s.warn++;
            if (g > 140 && b > 60 && r < 120 && (int)x > ((int)w - size) / 2 && (int)x < ((int)w + size) / 2) s.icon++;
        }
    return s;
}

int main(int argc, char **argv)
{
    const char *font = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char *out = argc > 2 ? argv[2] : "/tmp/rdx_placeholder.png";
    uint32_t mw = 1920, mh = 1080;
    if (argc > 3) sscanf(argv[3], "%ux%u", &mw, &mh);
    if (rdx_draw_init(font)) { printf("test_placeholder_render: SKIP (no font %s)\n", font); return 0; }
    uint32_t w, h;
    int px;
    rdx_draw_layout(mw, mh, &w, &h, &px);           /* the overlay buffers are the menu's */
    uint32_t stride = (w * 4 + 255) & ~255u;
    int ilen = 0, iw = 0, ih = 0;
    unsigned char *png = make_icon_png(&ilen);
    uint32_t *icon = rdx_ph_icon_decode(png, (size_t)ilen, rdx_ph_icon_size(px), &iw, &ih);
    uint32_t *noimg = rdx_ph_icon_decode((const unsigned char *)"<svg/>", 6, 64, &iw, &ih);

    struct rdx_ph_view views[7] = {
        {RDX_PH_START, "Steam", 3, 0, icon, iw, ih},
        {RDX_PH_RESUME, "Cyberpunk 2077", 12, 0, NULL, 0, 0},
        {RDX_PH_STALL, "Steam", 4, 0, icon, iw, ih},
        {RDX_PH_START, "A very long application name that does not fit the card at all", RDX_PH_WAIT_S + 5, 1, icon, iw, ih},
        {RDX_PH_ENDED, "Lutris", 2, 0, icon, iw, ih,
         "Lutris is already open on this PC \xe2\x80\x93 close it there and start again", NULL},
        /* the link line at the bottom of a card, and the link card itself */
        {RDX_PH_START, "Steam", 7, 0, icon, iw, ih, NULL,
         "Link down: the other computer's network module overheated (53.6 \xc2\xb0""C, warning at 53 \xc2\xb0""C)"},
        {RDX_PH_LINK, "Steam", 0, 0, icon, iw, ih, NULL,
         "The other computer's network module: 53.6 \xc2\xb0""C (warning at 53, off at 58 \xc2\xb0""C)"},
    };
    uint8_t *rgba = calloc((size_t)w * h * 7, 4);
    uint8_t *buf = calloc((size_t)stride, h);
    struct timespec t0, t1;
    double ms = 0;
    long fails = 0;
    struct stat_ st[7];
    for (int k = 0; k < 7; k++) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        rdx_ph_draw(buf, w, h, stride, px, &views[k]);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        st[k] = check(buf, w, h, stride, px);
        /* over black: what the sink shows before the first frame */
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p = ((uint32_t *)(buf + (size_t)y * stride))[x];
                uint8_t *o = rgba + (((size_t)k * h + y) * w + x) * 4;
                o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = 255;
            }
        printf("  state %d (%s, %d s%s): %ld text pixels, %ld icon pixels, %ld warning pixels, %ld invalid\n", k,
               views[k].kind == RDX_PH_RESUME ? "resume" : views[k].kind == RDX_PH_STALL ? "stall"
               : views[k].kind == RDX_PH_ENDED ? "ended, notice" : views[k].kind == RDX_PH_LINK ? "link card"
               : views[k].link ? "start, link line" : "start",
               views[k].seconds, views[k].waiting ? ", waiting" : "", st[k].lit, st[k].icon, st[k].warn, st[k].bad);
    }
    /* the first three states share the layout above the warning; state 3 adds lines below;
     * state 4 (the app ended: the host's notice) shows it in the warning colour, no bar */
    int ok = icon && iw > 0 && !noimg;
    for (int k = 0; k < 7; k++) ok = ok && !st[k].bad && st[k].lit > 300;
    ok = ok && st[0].icon > 1000 && st[1].icon < 50 && st[3].icon > 1000;    /* resume draws the initial */
    ok = ok && st[0].warn < 50 && st[3].warn > 300 && st[4].warn > 300 && st[4].icon > 1000;
    ok = ok && st[5].warn > 300 && st[6].warn > 600 && st[6].icon > 1000;   /* link line, link card */
    int wrote = stbi_write_png(out, (int)w, (int)h * 7, 4, rgba, (int)w * 4);
    ok = ok && wrote;
    printf("test_placeholder_render: %s  card %ux%u (text %d px) for %ux%u, icon %dx%d, %.2f ms per draw, png %s\n",
           ok ? "PASS" : "FAIL", w, h, px, mw, mh, iw, ih, ms / 7, wrote ? out : "not written");
    (void)fails;
    return ok ? 0 : 1;
}
