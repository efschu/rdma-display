/* Unit test for clip/rdx_clip_proto (no X, no network): frame header, the size limit, UTF-8 checks and
 * conversion, and the echo guard - including the loops gamescope and a desktop clipboard manager would
 * build (they re-own what we set with the same text).
 *   cc -O2 -Wall -Wextra tests/test_clip_proto.c clip/rdx_clip_proto.c -o t && ./t */
#include "../clip/rdx_clip_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); } else printf("ok   "); printf(__VA_ARGS__); printf("\n"); } while (0)

#define T(s) (const uint8_t *)(s), strlen(s)

/* two sides with their guards and a wire: what one sends, the other sets if its guard lets it */
struct side { struct rdx_clip_sync g; int sent, set; };
static int local_change(struct side *a, struct side *b, int sel, const char *t)
{
    if (!rdx_clip_sync_local(&a->g, sel, T(t))) return 0;
    a->sent++;
    if (rdx_clip_sync_remote(&b->g, sel, T(t))) b->set++;
    return 1;
}

int main(void)
{
    char why[128];
    uint8_t h[RDX_CLIP_HDR];
    struct rdx_clip_hdr ph;

    /* header */
    rdx_clip_hdr_pack(h, RDX_CLIP_TEXT, RDX_CLIP_PRIMARY, 0, 0x01020304);
    CHECK(!memcmp(h, "RDXC", 4) && h[4] == 1 && h[8] == 1 && h[11] == 4, "header: magic, version, big-endian length");
    CHECK(rdx_clip_hdr_parse(h, 0x7fffffff, &ph, why, sizeof(why)) == 0 && ph.type == RDX_CLIP_TEXT &&
          ph.sel == RDX_CLIP_PRIMARY && ph.len == 0x01020304, "header round trip");
    rdx_clip_hdr_pack(h, RDX_CLIP_TEXT, 0, 0, RDX_CLIP_MAX_DEFAULT);
    CHECK(rdx_clip_hdr_parse(h, RDX_CLIP_MAX_DEFAULT, &ph, why, sizeof(why)) == 0, "exactly 1 MiB is allowed");
    rdx_clip_hdr_pack(h, RDX_CLIP_TEXT, 0, 0, RDX_CLIP_MAX_DEFAULT + 1);
    CHECK(rdx_clip_hdr_parse(h, RDX_CLIP_MAX_DEFAULT, &ph, why, sizeof(why)) == -1 && strstr(why, "limit"),
          "1 MiB + 1 is refused before the payload is read (%s)", why);
    rdx_clip_hdr_pack(h, RDX_CLIP_TEXT, 2, 0, 1);
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == -1, "unknown selection refused (%s)", why);
    rdx_clip_hdr_pack(h, 7, 0, 0, 1);
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == -1, "unknown type refused (%s)", why);
    rdx_clip_hdr_pack(h, RDX_CLIP_HELLO, 0, RDX_CLIP_F_PRIMARY, 0);
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == 0 && ph.flags == RDX_CLIP_F_PRIMARY, "HELLO with the PRIMARY flag");
    rdx_clip_hdr_pack(h, RDX_CLIP_HELLO, 0, 0, 5);
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == -1, "HELLO with a payload refused");
    h[4] = 2;
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == -1 && strstr(why, "version"), "other version refused (%s)", why);
    memcpy(h, "HTTP", 4);
    CHECK(rdx_clip_hdr_parse(h, 100, &ph, why, sizeof(why)) == -1, "not our magic refused");

    /* UTF-8 */
    CHECK(rdx_clip_utf8_ok(T("ABCD-EFGH-1234 Steam key")), "ASCII is UTF-8");
    CHECK(rdx_clip_utf8_ok(T("Grüße \xe2\x82\xac \xf0\x9f\x8e\xae")), "umlauts, euro sign, emoji");
    CHECK(!rdx_clip_utf8_ok(T("Gr\xfc\xdf" "e")), "Latin-1 bytes are not UTF-8");
    CHECK(!rdx_clip_utf8_ok(T("\xc0\xaf")), "overlong refused");
    CHECK(!rdx_clip_utf8_ok(T("\xed\xa0\x80")), "surrogate refused");
    CHECK(!rdx_clip_utf8_ok(T("\xf4\x90\x80\x80")), "above U+10FFFF refused");
    CHECK(!rdx_clip_utf8_ok(T("\xe2\x82")), "truncated sequence refused");
    CHECK(!rdx_clip_utf8_ok((const uint8_t *)"a\0b", 3), "NUL refused on the wire");
    size_t n;
    uint8_t *u = rdx_clip_to_utf8(T("Gr\xfc\xdf" "e"), 0, &n);
    CHECK(u && n == 7 && !memcmp(u, "Gr\xc3\xbc\xc3\x9f" "e", 7), "Latin-1 in UTF8_STRING converted to UTF-8");
    free(u);
    u = rdx_clip_to_utf8(T("caf\xc3\xa9"), 0, &n);
    CHECK(u && n == 5 && !memcmp(u, "caf\xc3\xa9", 5) && u[5] == 0, "valid UTF-8 kept as it is");
    free(u);
    u = rdx_clip_to_utf8(T("caf\xe9"), 1, &n);
    CHECK(u && n == 5 && !memcmp(u, "caf\xc3\xa9", 5), "STRING (ISO 8859-1) converted");
    free(u);
    u = rdx_clip_to_utf8((const uint8_t *)"key\0\0", 5, 0, &n);
    CHECK(u && n == 3 && rdx_clip_utf8_ok(u, n), "trailing NULs dropped");
    free(u);
    u = rdx_clip_to_utf8((const uint8_t *)"ab\0cd", 5, 0, &n);
    CHECK(u && n == 2, "an inner NUL ends the text");
    free(u);
    for (int c = 0; c < 256; c++) {              /* every Latin-1 byte becomes valid UTF-8 */
        uint8_t b = (uint8_t)(c ? c : 'x');
        u = rdx_clip_to_utf8(&b, 1, 1, &n);
        if (!u || !rdx_clip_utf8_ok(u, n)) { fails++; printf("FAIL Latin-1 byte %d\n", c); }
        free(u);
    }
    printf("ok   all 256 Latin-1 bytes convert to valid UTF-8\n");

    /* echo guard: client (desktop) and host (gamescope's Xwayland) */
    struct side cl = {0}, ho = {0};
    CHECK(local_change(&cl, &ho, RDX_CLIP_CLIPBOARD, "AAAAA-BBBBB-CCCCC") && ho.set == 1, "desktop copy reaches the app");
    /* gamescope takes the selection over on all its Xwaylands with the same text: the host reads it back */
    CHECK(!local_change(&ho, &cl, RDX_CLIP_CLIPBOARD, "AAAAA-BBBBB-CCCCC") && ho.sent == 0,
          "gamescope's copy of what we set is not sent back (no loop)");
    CHECK(local_change(&ho, &cl, RDX_CLIP_CLIPBOARD, "XYZ12-34567-89ABC") && cl.set == 1, "copy in the app reaches the desktop");
    /* the desktop's clipboard manager re-owns it with the same text */
    CHECK(!local_change(&cl, &ho, RDX_CLIP_CLIPBOARD, "XYZ12-34567-89ABC") && cl.sent == 1,
          "a clipboard manager's copy is not sent back");
    CHECK(local_change(&cl, &ho, RDX_CLIP_CLIPBOARD, "AAAAA-BBBBB-CCCCC") && ho.set == 2,
          "copying the first text again on the desktop goes over again (it differs from the last)");
    /* PRIMARY is guarded on its own */
    CHECK(local_change(&ho, &cl, RDX_CLIP_PRIMARY, "AAAAA-BBBBB-CCCCC") && cl.set == 2,
          "PRIMARY with the same text as CLIPBOARD still travels (own selection)");
    CHECK(!rdx_clip_sync_remote(&ho.g, RDX_CLIP_PRIMARY, T("AAAAA-BBBBB-CCCCC")), "a repeated frame is not set twice");
    /* simultaneous copies on both sides: each takes the other's, nothing bounces afterwards */
    struct side a = {0}, b = {0};
    int a_ok = rdx_clip_sync_local(&a.g, 0, T("left")), b_ok = rdx_clip_sync_local(&b.g, 0, T("right"));
    int a_set = rdx_clip_sync_remote(&a.g, 0, T("right")), b_set = rdx_clip_sync_remote(&b.g, 0, T("left"));
    CHECK(a_ok && b_ok && a_set && b_set, "crossing copies: both sides take the other's text");
    CHECK(!rdx_clip_sync_local(&a.g, 0, T("right")) && !rdx_clip_sync_local(&b.g, 0, T("left")),
          "and their re-reads stop at the guard: no ping-pong");
    /* a new connection forgets */
    rdx_clip_sync_reset(&ho.g);
    CHECK(rdx_clip_sync_local(&ho.g, 0, T("AAAAA-BBBBB-CCCCC")), "after a reconnect the same text may go again");
    CHECK(!rdx_clip_sync_local(&ho.g, 5, T("x")) && !rdx_clip_sync_remote(&ho.g, -1, T("x")), "bad selection index ignored");
    CHECK(rdx_clip_sync_remote(&cl.g, 0, (const uint8_t *)"", 0) && !rdx_clip_sync_remote(&cl.g, 0, (const uint8_t *)"", 0),
          "empty text handled");
    rdx_clip_sync_reset(&cl.g); rdx_clip_sync_reset(&ho.g); rdx_clip_sync_reset(&a.g); rdx_clip_sync_reset(&b.g);

    printf(fails ? "test_clip_proto: %d FAILED\n" : "test_clip_proto: all passed\n", fails);
    return fails != 0;
}
