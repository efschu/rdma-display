/* SPDX-License-Identifier: MIT
 * GID and RDMA device selection by the seat's link IP (common/rdx_common.c) against faked GID
 * tables: one RoCE NIC shared by three seat containers (a macvlan each, every container sees
 * every GID) next to a second NIC. */
#include "rdx_common.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

static void ip4_gid(uint8_t g[16], int a, int b, int c, int d)
{
    memset(g, 0, 16);
    g[10] = g[11] = 0xff;
    g[12] = (uint8_t)a; g[13] = (uint8_t)b; g[14] = (uint8_t)c; g[15] = (uint8_t)d;
}

/* fe80::... link-local GID whose EUI-64 carries ff:fe (and ff in byte 11) */
static void ll_gid(uint8_t g[16])
{
    static const uint8_t ll[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0xba, 0x59, 0x9f, 0xff, 0xfe, 0x36, 0x70, 0x81};
    memcpy(g, ll, 16);
}

/* the multiseat host's shared NIC: link-local v1/v2, then per seat IP a v1 and a v2 entry */
static int shared_table(struct rdx_gid_rec *t)
{
    int n = 0;
    for (int v = 0; v < 2; v++) { t[n].index = n; ll_gid(t[n].gid); t[n].roce_v2 = v; n++; }
    for (int s = 11; s <= 13; s++)
        for (int v = 0; v < 2; v++) { t[n].index = n; ip4_gid(t[n].gid, 10, 0, 1, s); t[n].roce_v2 = v; n++; }
    return n;
}

static int fake_dev_names_two(char names[][64], int max)
{
    (void)max;
    snprintf(names[0], 64, "mlx5_0");
    snprintf(names[1], 64, "mlx5_1");
    return 2;
}

static int fake_dev_names_one(char names[][64], int max)
{
    (void)max;
    snprintf(names[0], 64, "mlx5_1");
    return 1;
}

static int fake_gid_table(const char *dev, struct rdx_gid_rec *out, int max)
{
    (void)max;
    if (!strcmp(dev, "mlx5_0")) {                        /* a single-host link: only v1 for its IP */
        out[0].index = 0; ll_gid(out[0].gid); out[0].roce_v2 = 0;
        out[1].index = 1; ip4_gid(out[1].gid, 10, 0, 0, 2); out[1].roce_v2 = 0;
        return 2;
    }
    if (!strcmp(dev, "mlx5_1") || !*dev) return shared_table(out);
    return 0;
}

int main(void)
{
    struct rdx_gid_rec t[32];
    int n = shared_table(t);

    int k = rdx_pick_gid(t, n, "10.0.1.12");
    CHECK(k >= 0 && t[k].index == 5 && t[k].roce_v2, "seat 2's IP -> its RoCE v2 GID (index 5), got %d", k >= 0 ? t[k].index : k);
    k = rdx_pick_gid(t, n, "10.0.1.13");
    CHECK(k >= 0 && t[k].index == 7, "seat 3's IP -> index 7, got %d", k >= 0 ? t[k].index : k);
    k = rdx_pick_gid(t, n, NULL);
    CHECK(k >= 0 && t[k].index == 3, "no link IP -> first IPv4 RoCE v2 GID (index 3, the old behaviour), got %d",
          k >= 0 ? t[k].index : k);
    k = rdx_pick_gid(t, n, "0.0.0.0");
    CHECK(k >= 0 && t[k].index == 3, "0.0.0.0 counts as unknown -> index 3, got %d", k >= 0 ? t[k].index : k);
    k = rdx_pick_gid(t, n, "127.0.0.1");
    CHECK(k >= 0 && t[k].index == 3, "loopback counts as unknown -> index 3, got %d", k >= 0 ? t[k].index : k);
    k = rdx_pick_gid(t, n, "10.0.1.99");
    CHECK(k == -2, "an IP no GID carries -> -2 (never another seat's address), got %d", k);
    k = rdx_pick_gid(t, 2, NULL);
    CHECK(k == -1, "only link-local GIDs (ff in byte 11) -> -1, got %d", k);
    /* RoCE v1 only for that IP */
    struct rdx_gid_rec v1[2];
    ll_gid(v1[0].gid); v1[0].index = 0; v1[0].roce_v2 = 1;
    ip4_gid(v1[1].gid, 10, 0, 0, 2); v1[1].index = 1; v1[1].roce_v2 = 0;
    k = rdx_pick_gid(v1, 2, "10.0.0.2");
    CHECK(k == 1, "the IP only as RoCE v1 -> that v1 entry, got %d", k);
    k = rdx_pick_gid(v1, 2, "garbage");
    CHECK(k == 1, "an unparsable IP counts as unknown -> first IPv4 GID, got %d", k);

    /* through the seam: the full lookup and the device choice */
    static const struct rdx_rdma_seam two = {fake_dev_names_two, fake_gid_table};
    static const struct rdx_rdma_seam one = {fake_dev_names_one, fake_gid_table};
    rdx_rdma_seam = &two;
    uint8_t gid[16];
    int idx = rdx_find_gid_index_ip(NULL, "10.0.1.11", gid);
    CHECK(idx == 3 && gid[15] == 11 && gid[10] == 0xff, "rdx_find_gid_index_ip(10.0.1.11) -> 3 with that GID, got %d", idx);
    idx = rdx_find_gid_index_ip(NULL, "10.0.1.50", gid);
    CHECK(idx < 0, "rdx_find_gid_index_ip of an unknown IP fails, got %d", idx);

    char buf[64];
    const char *d = rdx_resolve_dev("auto", "10.0.1.12", buf, sizeof(buf));
    CHECK(d && !strcmp(d, "mlx5_1"), "auto + 10.0.1.12 -> mlx5_1 (the shared NIC), got %s", d ? d : "NULL");
    d = rdx_resolve_dev("", "10.0.0.2", buf, sizeof(buf));
    CHECK(d && !strcmp(d, "mlx5_0"), "\"\" + 10.0.0.2 -> mlx5_0, got %s", d ? d : "NULL");
    d = rdx_resolve_dev("rocep1s0f1", "10.0.1.12", buf, sizeof(buf));
    CHECK(d && !strcmp(d, "rocep1s0f1"), "an explicit name stays, got %s", d ? d : "NULL");
    d = rdx_resolve_dev("auto", NULL, buf, sizeof(buf));
    CHECK(!d, "auto without a link IP and two devices with IPv4 GIDs -> none, got %s", d ? d : "NULL");
    d = rdx_resolve_dev("auto", "10.9.9.9", buf, sizeof(buf));
    CHECK(!d, "auto with an IP no device carries -> none, got %s", d ? d : "NULL");
    rdx_rdma_seam = &one;
    d = rdx_resolve_dev("auto", NULL, buf, sizeof(buf));
    CHECK(d && !strcmp(d, "mlx5_1"), "auto without a link IP, one device -> that one, got %s", d ? d : "NULL");
    rdx_rdma_seam = NULL;

    char lip[64] = "";
    int r = rdx_local_ip_for("127.0.0.1", lip, sizeof(lip));
    CHECK(r == 0 && !strcmp(lip, "127.0.0.1"), "route source towards 127.0.0.1 is 127.0.0.1, got %d %s", r, lip);
    r = rdx_local_ip_for("not-an-ip", lip, sizeof(lip));
    CHECK(r < 0, "rdx_local_ip_for of a non-IP fails, got %d", r);

    printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails ? 1 : 0;
}
