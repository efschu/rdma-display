/* SPDX-License-Identifier: MIT
 *
 * Clipboard channel between the client's desktop and the app in the stream (clip/rdx_clip.c):
 * wire format, text checks and the echo guard. No X, no sockets: unit-tested in
 * tests/test_clip_proto.c.
 *
 * Wire (TCP, one connection per session, client -> host): frames of a 12-byte header and a payload
 *   "RDXC" | version 1 | type | selection | flags | payload length (u32, big endian)
 *   HELLO  (type 0, no payload): flags bit 0 = this side syncs PRIMARY too; both sides send one
 *          first, PRIMARY travels only when both set it
 *   TEXT   (type 1): selection 0 = CLIPBOARD, 1 = PRIMARY; payload UTF-8 text, at most the limit
 *          (RDX_CLIP_MAX_DEFAULT, 1 MiB), no NUL. A frame over the limit or with bytes that are not
 *          UTF-8 is a protocol error: the connection is closed (the sender never sends one).
 *
 * Echo guard (struct rdx_clip_sync): per selection the last text that crossed the wire, in either
 * direction. A local change is sent only when it differs from it; a text from the peer is set
 * locally only when it differs from it. That ends every loop: our own ownership never produces a
 * read (rdx_clip ignores owner changes to its own window), and the copies others make of what we
 * set (gamescope takes the selection over on all its Xwaylands with the same text, a desktop's
 * clipboard manager re-owns it) read back as the same text and stop here.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RDX_CLIP_MAGIC        "RDXC"
#define RDX_CLIP_VERSION      1
#define RDX_CLIP_HDR          12
#define RDX_CLIP_MAX_DEFAULT  (1u << 20)        /* 1 MiB of text */

enum { RDX_CLIP_HELLO = 0, RDX_CLIP_TEXT = 1 };
enum { RDX_CLIP_CLIPBOARD = 0, RDX_CLIP_PRIMARY = 1, RDX_CLIP_NSEL = 2 };
#define RDX_CLIP_F_PRIMARY    1u

struct rdx_clip_hdr { uint8_t type, sel, flags; uint32_t len; };

/* the 12 header bytes of a frame */
void rdx_clip_hdr_pack(uint8_t out[RDX_CLIP_HDR], uint8_t type, uint8_t sel, uint8_t flags, uint32_t len);
/* parse a received header against the limit: 0 = ok, -1 = not ours / bad version / bad type or
 * selection / payload over max (why names it) */
int rdx_clip_hdr_parse(const uint8_t in[RDX_CLIP_HDR], uint32_t max, struct rdx_clip_hdr *h,
                       char *why, size_t whyn);

/* 1 = s[0..n) is well-formed UTF-8 (no overlongs, no surrogates, <= U+10FFFF) without NUL */
int rdx_clip_utf8_ok(const uint8_t *s, size_t n);

/* what a local owner gave us, made sendable: trailing NULs dropped; UTF-8 stays as it is; anything
 * else (a STRING selection is ISO 8859-1, some programs put Latin-1 into UTF8_STRING) is read as
 * Latin-1 and converted; an inner NUL ends the text (X programs treat it as the end). Returns a
 * malloc'd buffer of *outn bytes (not NUL-terminated beyond outn, but one spare byte holds a NUL),
 * NULL on no memory. */
uint8_t *rdx_clip_to_utf8(const uint8_t *s, size_t n, int latin1, size_t *outn);

/* echo guard, one per connection */
struct rdx_clip_sync {
    uint8_t *last[RDX_CLIP_NSEL];
    size_t len[RDX_CLIP_NSEL];
    int have[RDX_CLIP_NSEL];
};
void rdx_clip_sync_reset(struct rdx_clip_sync *s);       /* forget (a new connection) */
/* a local owner has text t: 1 = send it (remembered as the last one), 0 = the same as the last */
int rdx_clip_sync_local(struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n);
/* the peer sent text t: 1 = take it over locally (remembered), 0 = we have it already */
int rdx_clip_sync_remote(struct rdx_clip_sync *s, int sel, const uint8_t *t, size_t n);
