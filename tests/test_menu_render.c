/* Review aid for the in-stream menu: renders the panel for a sink mode into a PNG,
 * composited over a gradient so the premultiplied alpha is visible, and checks that
 * every pixel is valid premultiplied ARGB (no channel above alpha).
 *   tests/test_menu_render [font.ttf] [out.png] [WxH]   (exit 0 = ok) */
#include "../client/rdx_menu_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

int main(int argc, char **argv)
{
    const char *font = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char *out = argc > 2 ? argv[2] : "/tmp/rdx_menu.png";
    uint32_t mw = 1920, mh = 1080;
    if (argc > 3) sscanf(argv[3], "%ux%u", &mw, &mh);
    if (rdx_draw_init(font)) { printf("test_menu_render: SKIP (no font %s)\n", font); return 0; }
    uint32_t w, h;
    int px;
    rdx_draw_layout(mw, mh, &w, &h, &px);
    uint32_t stride = (w * 4 + 255) & ~255u;
    uint8_t *buf = calloc((size_t)stride, h);
    struct rdx_menu_view v;
    memset(&v, 0, sizeof(v));
    v.title = "rdma-display";
    snprintf(v.stats[0], sizeof(v.stats[0]), "144 frames/s on 240.0 Hz  \xc2\xb7  arrival \xe2\x86\x92 visible 3.21 ms (p50)");
    snprintf(v.stats[1], sizeof(v.stats[1]), "hitches 0/s  \xc2\xb7  dropped 0/s");
    snprintf(v.warn, sizeof(v.warn), "The other computer's network module: 53.6 \xc2\xb0""C (warning at 53, off at 58 \xc2\xb0""C)");
    snprintf(v.items[0], sizeof(v.items[0]), "Cursor            on");
    snprintf(v.items[1], sizeof(v.items[1]), "Output            \xe2\x97\x80 DP-2 \xe2\x96\xb6   (Enter applies)");
    snprintf(v.items[2], sizeof(v.items[2]), "Monitor there     on  (DELL U2723QE)");
    snprintf(v.items[3], sizeof(v.items[3]), "Microphone mute   off");
    snprintf(v.items[4], sizeof(v.items[4]), "Mouse             auto: relative, captured (the game locks it)");
    snprintf(v.items[5], sizeof(v.items[5]), "Back to the stream");
    snprintf(v.items[6], sizeof(v.items[6]), "End the stream (the app stays on its monitor)");
    snprintf(v.items[7], sizeof(v.items[7]), "Quit the app on the other computer");
    snprintf(v.items[8], sizeof(v.items[8]), "Restart the host program");
    v.nitems = 9;                  /* the most client/rdx_menu.c shows: a window client while the placeholder waits */
    v.sel = 1;
    snprintf(v.note, sizeof(v.note), "applied: cursor on, output DP-1");
    v.footer = "\xe2\x86\x91\xe2\x86\x93 select  \xc2\xb7  \xe2\x86\x90\xe2\x86\x92 change  \xc2\xb7  Enter apply  \xc2\xb7  Esc / Ctrl+Alt+Shift+M close";
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int reps = 20;
    for (int i = 0; i < reps; i++) rdx_draw_menu(buf, w, h, stride, px, &v);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / reps;
    long bad = 0, lit = 0;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = ((uint32_t *)(buf + (size_t)y * stride))[x];
            uint32_t a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > a || g > a || b > a) bad++;
            if (r > 200 && g > 200) lit++;
            /* over a diagonal gradient that stands for the stream */
            uint32_t bg = (x + y) * 255 / (w + h);
            uint8_t *o = rgba + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)(r + bg * (255 - a) / 255);
            o[1] = (uint8_t)(g + (255 - bg) * (255 - a) / 255);
            o[2] = (uint8_t)(b + 128 * (255 - a) / 255);
            o[3] = 255;
        }
    int wrote = stbi_write_png(out, (int)w, (int)h, 4, rgba, (int)w * 4);
    int ok = !bad && lit > 500 && wrote;
    printf("test_menu_render: %s  panel %ux%u (text %d px) for %ux%u, %.2f ms per draw, "
           "%ld bright text pixels, %ld invalid premultiplied pixels, png %s\n",
           ok ? "PASS" : "FAIL", w, h, px, mw, mh, ms, lit, bad, wrote ? out : "not written");
    return ok ? 0 : 1;
}
