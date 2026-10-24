/* Unit test for rdx_kms_synth_mode / rdx_kms_mode_hz (no DRM device needed).
 * Base: CTA-861 1920x1080@120 (VIC 63: 297 MHz, htotal 2200, vtotal 1125), the Optoma's
 * listed 1080p120; EDID range of the Optoma: v 24-240 Hz, max pixel clock 600 MHz.
 *   cc -O2 -Wall -Wextra -Icommon test_mode_synth.c common/rdx_kms.c -o t && ./t */
#include "../common/rdx_kms.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void)
{
    struct drm_mode_modeinfo b;
    memset(&b, 0, sizeof(b));
    b.clock = 297000; b.hdisplay = 1920; b.hsync_start = 2008; b.hsync_end = 2052; b.htotal = 2200;
    b.vdisplay = 1080; b.vsync_start = 1084; b.vsync_end = 1089; b.vtotal = 1125; b.vrefresh = 120;
    snprintf(b.name, sizeof(b.name), "1920x1080");
    struct rdx_kms_range optoma = {24, 240, 600000};
    struct drm_mode_modeinfo m;
    char why[160];

    CHECK(rdx_kms_mode_hz(&b) == 120.0, "base 1080p120 = %.6f Hz", rdx_kms_mode_hz(&b));

    int r = rdx_kms_synth_mode(&b, 144.0, &optoma, &m, why, sizeof(why));
    double hz = rdx_kms_mode_hz(&m);
    CHECK(r == 0 && m.clock == 356400 && hz > 143.9995 && hz < 144.0005, "144.000 -> clock %u kHz = %.5f Hz", m.clock, hz);
    CHECK(m.htotal == 2200 && m.vtotal == 1125 && m.hsync_start == 2008 && m.vsync_end == 1089 && m.vrefresh == 144,
          "timings kept (htotal %u vtotal %u hsync %u-%u vsync %u-%u), vrefresh %u, name %s",
          m.htotal, m.vtotal, m.hsync_start, m.hsync_end, m.vsync_start, m.vsync_end, m.vrefresh, m.name);
    CHECK(!(m.type & DRM_MODE_TYPE_PREFERRED), "not marked preferred");

    r = rdx_kms_synth_mode(&b, 143.998, &optoma, &m, why, sizeof(why));
    hz = rdx_kms_mode_hz(&m);
    CHECK(r == 0 && hz > 143.998 - 0.0005 && hz < 143.998 + 0.0005, "143.998 -> clock %u kHz = %.5f Hz (err %.5f Hz)",
          m.clock, hz, hz - 143.998);

    r = rdx_kms_synth_mode(&b, 240.0, &optoma, &m, why, sizeof(why));
    CHECK(r == 0 && m.clock == 594000, "240 -> clock %u kHz (= CTA 1080p240 clock)", m.clock);

    /* refusals: above 600 MHz, and outside the vertical range */
    struct drm_mode_modeinfo k4 = b;     /* 3840x2160@60 CTA: 594 MHz, 4400 x 2250 */
    k4.clock = 594000; k4.hdisplay = 3840; k4.htotal = 4400; k4.vdisplay = 2160; k4.vtotal = 2250; k4.vrefresh = 60;
    r = rdx_kms_synth_mode(&k4, 144.0, &optoma, &m, why, sizeof(why));
    CHECK(r == -1, "4K@144 refused: %s", why);
    r = rdx_kms_synth_mode(&k4, 60.7, &optoma, &m, why, sizeof(why));
    CHECK(r == -1, "4K@60.7 refused (600.93 MHz, just above 600 MHz): %s", why);
    r = rdx_kms_synth_mode(&b, 250.0, &(struct rdx_kms_range){24, 240, 0}, &m, why, sizeof(why));
    CHECK(r == -1, "1080p@250 refused by the vertical range: %s", why);
    r = rdx_kms_synth_mode(&b, 250.0, NULL, &m, why, sizeof(why));
    CHECK(r == 0 && m.clock == 618750, "no EDID range: not checked, 250 -> %u kHz", m.clock);

    printf(fails ? "SOME FAILED\n" : "ALL OK\n");
    return fails != 0;
}
