/* SPDX-License-Identifier: MIT
 * The sinks and the test source share one default MTU (RDX_DEFAULT_MTU, 4096). rdx_sink_win once defaulted to 1024 while every
 * source sends at 4096: the stream stopped after the first frame (WC "Work Request Flushed") whenever a
 * caller left out --mtu. Run from the repository root (make test does).
 */
#include "../common/rdx_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the text of a source file: the declaration of "mtu" in main() and the usage comment */
static char *slurp(const char *p)
{
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    char *b = calloc(1, 1 << 20);
    size_t n = fread(b, 1, (1 << 20) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static void sink(const char *file)
{
    char *s = slurp(file);
    CHECK(s != NULL, "cannot read %s (run from the repository root)", file);
    if (!s) return;
    CHECK(strstr(s, "mtu = RDX_DEFAULT_MTU"), "%s: the default MTU is not RDX_DEFAULT_MTU", file);
    CHECK(!strstr(s, "mtu = 1024"), "%s: still a 1024 default", file);
    CHECK(strstr(s, "[--mtu 4096]"), "%s: the usage comment does not show --mtu 4096", file);
    free(s);
}

int main(void)
{
    CHECK(RDX_DEFAULT_MTU == 4096, "RDX_DEFAULT_MTU is %d", RDX_DEFAULT_MTU);
    CHECK(rdx_parse_mtu(RDX_DEFAULT_MTU) == IBV_MTU_4096, "the default is a 4096 QP MTU");
    CHECK(rdx_parse_mtu(1024) == IBV_MTU_1024 && rdx_parse_mtu(0) == IBV_MTU_1024, "other values keep their meaning");
    sink("p0b/rdx_sink_win.c");
    sink("p0b/rdx_sink_kms.c");
    sink("p0b/rdx_src.c");
    printf("%s: test_default_mtu (%d failures)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
