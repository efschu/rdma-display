/* SPDX-License-Identifier: MIT
 * Clipboard channel: wire format, text checks, echo guard (see rdx_clip_proto.h). */
#include "rdx_clip_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rdx_clip_hdr_pack(uint8_t out[RDX_CLIP_HDR], uint8_t type, uint8_t sel, uint8_t flags, uint32_t len)
{
    memcpy(out, RDX_CLIP_MAGIC, 4);
    out[4] = RDX_CLIP_VERSION;
    out[5] = type;
    out[6] = sel;
    out[7] = flags;
    out[8] = (uint8_t)(len >> 24);
    out[9] = (uint8_t)(len >> 16);
    out[10] = (uint8_t)(len >> 8);
    out[11] = (uint8_t)len;
}

int rdx_clip_hdr_parse(const uint8_t in[RDX_CLIP_HDR], uint32_t max, struct rdx_clip_hdr *h, char *why, size_t whyn)
{
    if (memcmp(in, RDX_CLIP_MAGIC, 4)) { snprintf(why, whyn, "not a clipboard frame"); return -1; }
    if (in[4] != RDX_CLIP_VERSION) { snprintf(why, whyn, "protocol version %u, we speak %u", in[4], RDX_CLIP_VERSION); return -1; }
    h->type = in[5];
    h->sel = in[6];
    h->flags = in[7];
    h->len = (uint32_t)in[8] << 24 | (uint32_t)in[9] << 16 | (uint32_t)in[10] << 8 | in[11];
    if (h->type != RDX_CLIP_HELLO && h->type != RDX_CLIP_TEXT) { snprintf(why, whyn, "unknown frame type %u", h->type); return -1; }
    if (h->type == RDX_CLIP_HELLO && h->len) { snprintf(why, whyn, "HELLO with a payload"); return -1; }
    if (h->type == RDX_CLIP_TEXT && h->sel >= RDX_CLIP_NSEL) { snprintf(why, whyn, "unknown selection %u", h->sel); return -1; }
    if (h->len > max) { snprintf(why, whyn, "%u bytes, over the limit of %u", h->len, max); return -1; }
    return 0;
}

/* length of the UTF-8 sequence at s (n bytes left), 0 = malformed */
static size_t utf8_seq(const uint8_t *s, size_t n)
{
    uint8_t c = s[0];
    if (c < 0x80) return c ? 1 : 0;
    size_t len;
    uint32_t cp, min;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

int rdx_clip_utf8_ok(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        size_t l = utf8_seq(s + i, n - i);
        if (!l) return 0;
        i += l;
    }
    return 1;
}

uint8_t *rdx_clip_to_utf8(const uint8_t *s, size_t n, int latin1, size_t *outn)
{
    const uint8_t *z = memchr(s, 0, n);          /* an inner NUL ends it, trailing ones go with it */
    if (z) n = (size_t)(z - s);
    if (!latin1 && rdx_clip_utf8_ok(s, n)) {
        uint8_t *o = malloc(n + 1);
        if (!o) return NULL;
        memcpy(o, s, n);
        o[n] = 0;
        *outn = n;
        return o;
    }
    uint8_t *o = malloc(2 * n + 1), *p = o;
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x80) *p++ = s[i];
        else { *p++ = (uint8_t)(0xC0 | s[i] >> 6); *p++ = (uint8_t)(0x80 | (s[i] & 0x3F)); }
    }
    *p = 0;
    *outn = (size_t)(p - o);
    return o;
}

void rdx_clip_sync_reset(struct rdx_clip_sync *s)
{
    for (int i = 0; i < RDX_CLIP_NSEL; i++) free(s->last[i]);
    memset(s, 0, sizeof(*s));
}

static int same(const struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n)
{
    return s->have[sel] && s->len[sel] == n && (!n || !memcmp(s->last[sel], t, n));
}

static int remember(struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n)
{
    uint8_t *c = malloc(n ? n : 1);
    if (!c) return 0;
    if (n) memcpy(c, t, n);
    free(s->last[sel]);
    s->last[sel] = c;
    s->len[sel] = n;
    s->have[sel] = 1;
    return 1;
}

int rdx_clip_sync_local(struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n)
{
    if (sel < 0 || sel >= RDX_CLIP_NSEL || same(s, sel, t, n)) return 0;
    return remember(s, sel, t, n);
}

int rdx_clip_sync_remote(struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n)
{
    if (sel < 0 || sel >= RDX_CLIP_NSEL || same(s, sel, t, n)) return 0;
    return remember(s, sel, t, n);
}
