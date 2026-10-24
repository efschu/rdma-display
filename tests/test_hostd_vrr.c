/* SPDX-License-Identifier: MIT
 * Variable refresh of a session in rdx-hostd (host/rdx_vrr.h): the client's "display" object ->
 * on (cap) or fixed refresh with the reason. The regression guard: without "display", with
 * vrr false, tearing, a whole screen, a joined monitor, a local window or the host's switch off
 * the refresh stays fixed. */
#include "../host/rdx_vrr.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

static struct rdx_vrr run(const char *json, int allow, enum rdx_vrr_kind kind, int tearing)
{
    cJSON *d = json ? cJSON_Parse(json) : NULL;
    struct rdx_vrr v;
    rdx_vrr_decide(d, allow, kind, tearing, 1.0, &v);
    cJSON_Delete(d);
    return v;
}

int main(void)
{
    const char *fs = "{\"vrr\":true,\"min_hz\":48,\"max_hz\":240,\"hz\":240}";
    struct rdx_vrr v = run(fs, 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(v.active && v.cap_hz == 240, "FreeSync 48-240 at 240 Hz, gamescope app: on, cap 240 (%s)", v.why);
    CHECK(strstr(v.why, "below 48.000 Hz the client repeats frames") != NULL, "the reason names the client's LFC: %s", v.why);

    v = run("{\"vrr\":true,\"min_hz\":48,\"max_hz\":165,\"hz\":240}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(v.active && v.cap_hz == 165, "range up to 165 at a 240 Hz mode: cap 165 (%.3f)", v.cap_hz);
    v = run("{\"vrr\":true,\"min_hz\":40,\"max_hz\":144,\"hz\":143.999}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(v.active && v.cap_hz == 143.999, "a test host's DP-1 (EDID 40-144, mode 143.999): cap = the mode (%.4f)", v.cap_hz);
    v = run("{\"vrr\":true,\"hz\":120}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(v.active && v.cap_hz == 120, "no range, mode 120 Hz: cap 120 (%.3f)", v.cap_hz);

    /* the default cap: 97 % of the maximum, never below the minimum, a bad fraction = the default */
    cJSON *d = cJSON_Parse(fs);
    rdx_vrr_decide(d, 1, RDX_VRR_GAMESCOPE, 0, RDX_VRR_CAP_FRAC_DEFAULT, &v);
    CHECK(v.active && v.cap_hz == 232.8, "default 97 %%: 240 Hz -> cap 232.8 (%.3f)", v.cap_hz);
    rdx_vrr_decide(d, 1, RDX_VRR_GAMESCOPE, 0, 7, &v);
    CHECK(v.cap_hz == 232.8, "fraction 7 (nonsense): the default (%.3f)", v.cap_hz);
    cJSON_Delete(d);
    d = cJSON_Parse("{\"vrr\":true,\"min_hz\":40,\"max_hz\":144,\"hz\":143.999}");
    rdx_vrr_decide(d, 1, RDX_VRR_GAMESCOPE, 0, RDX_VRR_CAP_FRAC_DEFAULT, &v);
    CHECK(v.cap_hz == 139.679, "a test host's DP-1 at 97 %%: 143.999 -> 139.679 (%.3f)", v.cap_hz);
    cJSON_Delete(d);
    d = cJSON_Parse("{\"vrr\":true,\"min_hz\":59,\"max_hz\":60,\"hz\":60}");
    rdx_vrr_decide(d, 1, RDX_VRR_GAMESCOPE, 0, 0.5, &v);
    CHECK(v.active && v.cap_hz == 59, "a narrow range 59-60 at 50 %%: never below the minimum (%.3f)", v.cap_hz);
    cJSON_Delete(d);

    /* fixed refresh, as before the feature */
    v = run(NULL, 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && !v.reported && strstr(v.why, "older client"), "no \"display\": fixed (%s)", v.why);
    v = run("[]", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && !v.reported, "\"display\" not an object: fixed (%s)", v.why);
    v = run("{\"vrr\":false,\"min_hz\":0,\"max_hz\":240,\"hz\":240}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && v.reported && strstr(v.why, "no variable refresh"), "vrr false (clientd's fallback): fixed (%s)", v.why);
    v = run("{\"min_hz\":48,\"max_hz\":240,\"hz\":240}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active, "vrr missing: fixed (%s)", v.why);
    v = run(fs, 0, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && strstr(v.why, "off on this host"), "host switch off: fixed (%s)", v.why);
    v = run(fs, 1, RDX_VRR_GAMESCOPE, 1);
    CHECK(!v.active && strstr(v.why, "tearing"), "tearing: fixed (%s)", v.why);
    v = run(fs, 1, RDX_VRR_MIRROR, 0);
    CHECK(!v.active && strstr(v.why, "whole screen"), "whole screen: fixed (%s)", v.why);
    v = run(fs, 1, RDX_VRR_JOIN, 0);
    CHECK(!v.active && strstr(v.why, "seat's own monitor"), "joined the seat's monitor: fixed (%s)", v.why);
    /* a join onto the seat's monitor: only where that monitor has variable refresh (vrr_capable) */
    v = run(fs, 1, RDX_VRR_JOIN_VRR, 0);
    CHECK(v.active && v.cap_hz == 240 && strstr(v.why, "seat's monitor and the client"),
          "joined a seat monitor with vrr_capable 1: on, cap 240 (%s)", v.why);
    v = run(fs, 1, RDX_VRR_JOIN_VRR, 1);
    CHECK(!v.active && strstr(v.why, "tearing"), "joined a vrr_capable monitor with tearing: fixed (%s)", v.why);
    v = run(fs, 0, RDX_VRR_JOIN_VRR, 0);
    CHECK(!v.active && strstr(v.why, "off on this host"), "joined a vrr_capable monitor, host switch off: fixed (%s)", v.why);
    CHECK(rdx_vrr_monitor_capable("/nonexistent/card9", "HDMI-A-1") == -1, "vrr_capable of a missing card: unknown");
    CHECK(rdx_vrr_monitor_capable("/dev/null", "HDMI-A-1") == -1, "vrr_capable of a node that is no DRM card: unknown");
    CHECK(rdx_vrr_monitor_capable(NULL, "") == -1, "vrr_capable without a card: unknown");
    v = run(fs, 1, RDX_VRR_LOCAL_WINDOW, 0);
    CHECK(!v.active && strstr(v.why, "window"), "local window: fixed (%s)", v.why);
    v = run("{\"vrr\":true}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && strstr(v.why, "no refresh range"), "vrr without any rate: fixed (%s)", v.why);
    v = run("{\"vrr\":true,\"min_hz\":240,\"max_hz\":240,\"hz\":240}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active && strstr(v.why, "empty"), "min = max: fixed (%s)", v.why);
    v = run("{\"vrr\":true,\"min_hz\":\"48\",\"max_hz\":-5,\"hz\":1e9}", 1, RDX_VRR_GAMESCOPE, 0);
    CHECK(!v.active, "garbage numbers: fixed (%s)", v.why);

    /* the JSON for /api/state */
    v = run(fs, 1, RDX_VRR_GAMESCOPE, 0);
    cJSON *j = rdx_vrr_json(&v);
    char *t = cJSON_PrintUnformatted(j);
    CHECK(strstr(t, "\"active\":true") && strstr(t, "\"cap_hz\":240") && strstr(t, "\"client\":{\"vrr\":true"),
          "json: %s", t);
    cJSON_free(t);
    cJSON_Delete(j);
    v = run(NULL, 1, RDX_VRR_GAMESCOPE, 0);
    j = rdx_vrr_json(&v);
    t = cJSON_PrintUnformatted(j);
    CHECK(strstr(t, "\"active\":false") && strstr(t, "\"client\":null"), "json without display: %s", t);
    cJSON_free(t);
    cJSON_Delete(j);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
