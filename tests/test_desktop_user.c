/* SPDX-License-Identifier: MIT
 * The desktop user without a graphical session (host/rdx_apps.c): a seat container has only a
 * lingering user manager, so rdx-hostd takes the one lingering regular user. Faked linger
 * directories; the regular users are this machine's own (cases without one are skipped). */
#define _GNU_SOURCE
#include "../host/rdx_apps.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

static char dir[] = "/tmp/rdx-linger-XXXXXX";

static void linger(const char *name)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) fclose(f);
}

static void unlinger(const char *name)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    unlink(p);
}

int main(void)
{
    struct rdx_desktop_user u;
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }

    CHECK(rdx_linger_user(dir, &u) == -1, "nobody lingers: no user");
    CHECK(rdx_linger_user("/nonexistent/linger", &u) == -1, "no linger directory: no user");
    linger("root");
    linger("nobody");
    linger("no-such-user-rdx");
    CHECK(rdx_linger_user(dir, &u) == -1, "only root, nobody and an unknown name linger: no user");

    /* this machine's regular users (uid 1000..65533) */
    char reg[2][64] = {"", ""};
    int nreg = 0;
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) && nreg < 2)
        if (pw->pw_uid >= 1000 && pw->pw_uid < 65534) snprintf(reg[nreg++], sizeof(reg[0]), "%s", pw->pw_name);
    endpwent();
    if (nreg >= 1) {
        linger(reg[0]);
        CHECK(rdx_linger_user(dir, &u) == 0 && !strcmp(u.name, reg[0]) && u.uid >= 1000 && u.home[0],
              "one regular user lingers (%s, besides root and nobody): that one", reg[0]);
        if (nreg == 2) {
            linger(reg[1]);
            CHECK(rdx_linger_user(dir, &u) == -1, "two regular users linger (%s, %s): ambiguous, none", reg[0], reg[1]);
            unlinger(reg[1]);
        } else
            printf("skip two lingering users: this machine has one regular user\n");
        unlinger(reg[0]);
    } else
        printf("skip one lingering user: this machine has no regular user\n");

    /* the configured user wins over everything (a seat config's "desktop_user") */
    CHECK(rdx_desktop_user("root", &u) == 0 && u.uid == 0 && !strcmp(u.name, "root"), "\"desktop_user\" set: that user");
    CHECK(rdx_desktop_user("no-such-user-rdx", &u) == -1, "\"desktop_user\" naming no user: none (no fallback)");

    /* the program of an app's cmd (rdx_app_program) */
    struct { const char *cmd, *prog; } pc[] = {
        {"[\"lutris\"]", "lutris"},
        {"[\"/usr/games/lutris\", \"--debug\"]", "lutris"},
        {"[\"sh\", \"-c\", \"if pgrep -x steam; then steam -shutdown; fi; exec steam \\\"$@\\\"\", \"steam\", \"-gamepadui\"]", "steam"},
        {"[\"env\", \"DXVK_HUD=1\", \"/usr/bin/heroic\"]", "heroic"},
        {"[\"flatpak\", \"run\", \"--branch=stable\", \"net.lutris.Lutris\"]", "net.lutris.Lutris"},
        {"[]", ""},
    };
    for (size_t i = 0; i < sizeof(pc) / sizeof(pc[0]); i++) {
        cJSON *c = cJSON_Parse(pc[i].cmd);
        char prog[128];
        rdx_app_program(c, prog, sizeof(prog));
        CHECK(!strcmp(prog, pc[i].prog), "program of %s: \"%s\"", pc[i].cmd, prog);
        cJSON_Delete(c);
    }

    /* a desktop instance (rdx_desktop_instance) in a fake /proc: the streamed one runs in
     * rdx-hostd's cgroup and does not count, the desktop one (an interpreter running it) does */
    char proc[] = "/tmp/rdx-proc-XXXXXX";
    if (!mkdtemp(proc)) { perror("mkdtemp"); return 1; }
    struct { const char *pid, *comm, *cg, *argv; size_t alen; } ps[] = {
        {"200", "lutris", "0::/system.slice/rdx-hostd.service\n", "/usr/bin/python3\0/usr/games/lutris\0", 35},
        {"300", "bash", "0::/user.slice/user-1000.slice/session-2.scope\n", "bash\0", 5},
    };
    for (size_t i = 0; i < 2; i++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/%s", proc, ps[i].pid);
        mkdir(p, 0755);
        const char *f[3] = {"comm", "cgroup", "cmdline"}, *v[3] = {ps[i].comm, ps[i].cg, ps[i].argv};
        size_t l[3] = {strlen(ps[i].comm), strlen(ps[i].cg), ps[i].alen};
        for (int k = 0; k < 3; k++) {
            snprintf(p, sizeof(p), "%s/%s/%s", proc, ps[i].pid, f[k]);
            FILE *fp = fopen(p, "w");
            if (fp) { fwrite(v[k], 1, l[k], fp); fclose(fp); }
        }
    }
    char what[256];
    uid_t me = getuid();
    CHECK(rdx_desktop_instance(proc, me, "lutris", what, sizeof(what)) == 0, "only the streamed lutris runs: no desktop instance");
    snprintf(what, sizeof(what), "%s/400", proc);
    mkdir(what, 0755);
    const char *f4[3] = {"comm", "cgroup", "cmdline"};
    const char *v4[3] = {"lutris", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-lutris.scope\n",
                         "/usr/bin/python3\0/usr/games/lutris\0"};
    size_t l4[3] = {6, strlen(v4[1]), 35};
    for (int k = 0; k < 3; k++) {
        char p[256];
        snprintf(p, sizeof(p), "%s/400/%s", proc, f4[k]);
        FILE *fp = fopen(p, "w");
        if (fp) { fwrite(v4[k], 1, l4[k], fp); fclose(fp); }
    }
    CHECK(rdx_desktop_instance(proc, me, "lutris", what, sizeof(what)) == 400 && strstr(what, "/usr/games/lutris"),
          "lutris open on the desktop: pid 400 (%s)", what);
    CHECK(rdx_desktop_instance(proc, me + 1, "lutris", what, sizeof(what)) == 0, "another user's processes do not count");
    CHECK(rdx_desktop_instance(proc, me, "steam", what, sizeof(what)) == 0, "no steam anywhere");
    char rm[300];
    snprintf(rm, sizeof(rm), "rm -rf '%s'", proc);
    if (system(rm)) { /* leftovers in /tmp only */ }

    /* not a check (depends on the machine): what rdx-hostd would take here without "desktop_user" */
    if (!rdx_desktop_user("", &u)) printf("info desktop user here: %s (uid %u, %s)\n", u.name, (unsigned)u.uid, u.home);
    else printf("info desktop user here: none\n");

    unlinger("root");
    unlinger("nobody");
    unlinger("no-such-user-rdx");
    rmdir(dir);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
