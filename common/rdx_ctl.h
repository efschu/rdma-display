/* SPDX-License-Identifier: MIT
 *
 * rdma-display: control channel between rdx-clientd and rdx-hostd.
 * Newline-delimited JSON (cJSON, one object per line) over plain TCP,
 * separate from the RDMA data path. Sockets are non-blocking; rdx_ctl_send
 * waits for socket space itself (up to 2 s) and is serialized per connection,
 * so web-server threads may send events while the main loop reads.
 */
#pragma once
#include "../third_party/cjson/cJSON.h"

#define RDX_CTL_PORT     47989          /* hostd control channel, link IP only */
#define RDX_CTL_VERSION  1
#define RDX_CTL_MAX_LINE (1 << 20)      /* a longer line closes the connection */

struct rdx_ctl;                                      /* one connection */
int  rdx_ctl_listen(const char *ip, int port);       /* returns listening fd */
struct rdx_ctl *rdx_ctl_accept(int lfd);
struct rdx_ctl *rdx_ctl_connect(const char *ip, int port, int timeout_ms);
int  rdx_ctl_fd(const struct rdx_ctl *c);
int  rdx_ctl_send(struct rdx_ctl *c, const cJSON *msg);       /* one line, blocking, thread-safe */
int  rdx_ctl_recv(struct rdx_ctl *c, cJSON **msg);  /* 1 = got msg (caller cJSON_Delete), 0 = need more, -1 = closed */
void rdx_ctl_close(struct rdx_ctl *c);

/* extra: peer address as "a.b.c.d" (empty if unknown) */
const char *rdx_ctl_peer(const struct rdx_ctl *c);

/* Sink slots a vblank-paced stream needs (hello "stream_lead_ms", clientd --slots). A slot is
 * gamescope's from its wake, lead_ms before the sink's vblank, until the sink hands it back one
 * period after showing it: busy lead + period. N slots carry that with half a period to spare while
 * lead + period <= (N - 0.5) * period. Measured on a seat with an RTX 3080 (the NIC at PCIe
 * Gen3 x4 behind the chipset, vkcube 1080p@240, lead 7.8-8.5 ms): 3 slots 124-216 of ~4300 frames
 * found no free slot (222-223 fps, 73-77 dropped), 4 slots none (228-230 fps, 48-54 dropped).
 * lead_ms <= 0 (no measurement yet): RDX_LEAD_DEFAULT_MS, so 4 slots from ~188 Hz on.
 * Returns 3 .. 8 (RDX_MAX_SLOTS); hz <= 0 (unknown): 3. */
#define RDX_LEAD_DEFAULT_MS 8.0
static inline int rdx_pace_slots(double hz, double lead_ms)
{
    if (hz <= 0) return 3;
    if (lead_ms <= 0) lead_ms = RDX_LEAD_DEFAULT_MS;
    double p = 1000.0 / hz;
    int n = 3;
    while (n < 8 && lead_ms + p > (n - 0.5) * p) n++;
    return n;
}
