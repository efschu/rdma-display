/* The pointer hint path of the gamescope path without a GPU, a NIC or gamescope:
 *  - the wire: RDX_GS_HINT built exactly as gamescope's rdx backend sends it (CRdxBackend::SendHint), over a
 *    SOCK_SEQPACKET socketpair, applied by the session the way rdx_gs_src's receive loop does
 *  - the frame header that results (rdx_srcsess_fill_hdr, what rdx_srcsess_post writes): flags =
 *    RDX_HDR_F_HINTS | (relative ? RDX_HDR_F_REL_MOUSE : 0), every field written
 *  - the layout of struct rdx_frame_hdr (24 bytes) and that a slot stays the same size
 * Build: make tests/test_gs_hint; run: tests/test_gs_hint (exit 0 = all passed). */
#include "../gs/rdx_srcsess.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* gamescope's side: one HINT packet, as CRdxBackend::SendHint builds it */
static int gs_sends_hint(int fd, uint32_t bits)
{
    struct rdx_gs_msg msg = {.magic = RDX_GS_MAGIC, .type = RDX_GS_HINT, .slot = bits, .pad = 0};
    return send(fd, &msg, sizeof(msg), MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof(msg) ? 0 : -1;
}

/* rdx_gs_src's side: what its receive loop does with the packets; returns how many changed the state */
static int src_drains(int fd, struct rdx_srcsess *s)
{
    int changed = 0;
    struct rdx_gs_msg m;
    while (recv(fd, &m, sizeof(m), MSG_DONTWAIT) == (ssize_t)sizeof(m))
        changed += rdx_srcsess_gs_hint(s, &m);
    return changed;
}

static uint32_t hdr_flags_now(const struct rdx_srcsess *s)
{
    struct rdx_frame_hdr h;
    memset(&h, 0xAA, sizeof(h));                       /* memory that was not zeroed */
    rdx_srcsess_fill_hdr(s, &h, 7, 123456789ull);
    CHECK(h.magic == RDX_MAGIC_LIVE && h.seq == 7 && h.t_post_ns == 123456789ull, "magic %x seq %u t %llu", h.magic, h.seq,
          (unsigned long long)h.t_post_ns);
    CHECK(h.pad == 0, "pad %x", h.pad);
    return h.flags;
}

int main(void)
{
    /* layout: the header grew from 16 to 24 bytes; its slot (hdr_off 4096-aligned + sizeof, rounded to 4096) did not */
    CHECK(sizeof(struct rdx_frame_hdr) == 24, "sizeof %zu", sizeof(struct rdx_frame_hdr));
    CHECK(offsetof(struct rdx_frame_hdr, flags) == 16 && offsetof(struct rdx_frame_hdr, pad) == 20, "offsets");
    CHECK(RDX_HDR_F_HINTS == 1u && RDX_HDR_F_REL_MOUSE == 2u, "flag values");
    CHECK(RDX_GS_HINT == 8 && RDX_GS_HINT_REL_MOUSE == 1u, "proto values");
    for (uint64_t hdr_off = 4096; hdr_off <= 4096ull * 4096; hdr_off += 4096 * 37) {
        uint64_t old_slot = (hdr_off + 16 + 4095) & ~4095ull, slot = (hdr_off + sizeof(struct rdx_frame_hdr) + 4095) & ~4095ull;
        CHECK(slot == old_slot, "hdr_off %llu: slot %llu, was %llu", (unsigned long long)hdr_off, (unsigned long long)slot,
              (unsigned long long)old_slot);
    }

    struct rdx_srcsess s;
    memset(&s, 0, sizeof(s));                          /* as rdx_srcsess_init: absolute until gamescope says otherwise */

    /* a gamescope that never sends a HINT: hints present, absolute */
    CHECK(hdr_flags_now(&s) == RDX_HDR_F_HINTS, "default flags %x", hdr_flags_now(&s));

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0, "socketpair");

    /* relative: the app hid its cursor and locked the pointer */
    CHECK(gs_sends_hint(sv[0], RDX_GS_HINT_REL_MOUSE) == 0, "send");
    CHECK(src_drains(sv[1], &s) == 1, "one change");
    CHECK(hdr_flags_now(&s) == (RDX_HDR_F_HINTS | RDX_HDR_F_REL_MOUSE), "rel flags %x", hdr_flags_now(&s));

    /* the same state again (gamescope sends only changes, but a repeat must not matter) */
    CHECK(gs_sends_hint(sv[0], RDX_GS_HINT_REL_MOUSE) == 0, "send");
    CHECK(src_drains(sv[1], &s) == 0, "repeat is no change");
    CHECK(hdr_flags_now(&s) == (RDX_HDR_F_HINTS | RDX_HDR_F_REL_MOUSE), "still rel");

    /* back to absolute */
    CHECK(gs_sends_hint(sv[0], 0) == 0, "send");
    CHECK(src_drains(sv[1], &s) == 1, "one change");
    CHECK(hdr_flags_now(&s) == RDX_HDR_F_HINTS, "abs flags %x", hdr_flags_now(&s));

    /* several packets in one go: the last one counts, each real change is counted */
    CHECK(gs_sends_hint(sv[0], RDX_GS_HINT_REL_MOUSE) == 0 && gs_sends_hint(sv[0], 0) == 0 &&
          gs_sends_hint(sv[0], RDX_GS_HINT_REL_MOUSE) == 0, "send 3");
    CHECK(src_drains(sv[1], &s) == 3, "three changes");
    CHECK(hdr_flags_now(&s) == (RDX_HDR_F_HINTS | RDX_HDR_F_REL_MOUSE), "last wins");

    /* bits the source does not know are ignored, the known bit still decides */
    CHECK(gs_sends_hint(sv[0], 0x80000000u) == 0, "send");
    CHECK(src_drains(sv[1], &s) == 1 && hdr_flags_now(&s) == RDX_HDR_F_HINTS, "unknown bits: absolute");

    /* other packets and wrong magic never touch the hint */
    struct rdx_gs_msg ready = {.magic = RDX_GS_MAGIC, .type = RDX_GS_READY, .slot = RDX_GS_HINT_REL_MOUSE};
    struct rdx_gs_msg bad = {.magic = 0x12345678u, .type = RDX_GS_HINT, .slot = RDX_GS_HINT_REL_MOUSE};
    CHECK(send(sv[0], &ready, sizeof(ready), MSG_NOSIGNAL) == (ssize_t)sizeof(ready), "send ready");
    CHECK(send(sv[0], &bad, sizeof(bad), MSG_NOSIGNAL) == (ssize_t)sizeof(bad), "send bad");
    CHECK(src_drains(sv[1], &s) == 0 && hdr_flags_now(&s) == RDX_HDR_F_HINTS, "READY / wrong magic ignored");

    close(sv[0]);
    close(sv[1]);

    printf("test_gs_hint: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
