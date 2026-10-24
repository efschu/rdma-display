/* SPDX-License-Identifier: MIT
 * See rdx_seatguard.h: may rdx-hostd drive a card's connectors itself? */
#define _GNU_SOURCE
#include "rdx_seatguard.h"

#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int rdx_logind_sessions(struct rdx_session_info *out, int max)
{
    FILE *f = popen("loginctl list-sessions --no-legend 2>/dev/null", "r");
    if (!f) return -1;
    char line[256], ids[64][32];
    int nid = 0;
    while (nid < 64 && fgets(line, sizeof(line), f))
        if (sscanf(line, " %31s", ids[nid]) == 1) nid++;
    if (pclose(f) == -1) return -1;
    int n = 0;
    for (int i = 0; i < nid && n < max; i++) {
        char cmd[160];
        if (strspn(ids[i], "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != strlen(ids[i])) continue;
        snprintf(cmd, sizeof(cmd), "loginctl show-session '%.31s' -p Type -p Seat -p State 2>/dev/null", ids[i]);
        if (!(f = popen(cmd, "r"))) continue;
        struct rdx_session_info *s = &out[n];
        memset(s, 0, sizeof(*s));
        snprintf(s->id, sizeof(s->id), "%.31s", ids[i]);   /* logind ids are short */
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "Type=", 5)) snprintf(s->type, sizeof(s->type), "%.15s", line + 5);
            else if (!strncmp(line, "Seat=", 5)) snprintf(s->seat, sizeof(s->seat), "%.31s", line + 5);
            else if (!strncmp(line, "State=", 6)) snprintf(s->state, sizeof(s->state), "%.15s", line + 6);
        }
        pclose(f);
        n++;
    }
    return n;
}

int rdx_sessions_block_seat(const struct rdx_session_info *s, int n, const char *seat, char *why, size_t wn)
{
    for (int i = 0; i < n; i++) {
        int graphical = !strcmp(s[i].type, "x11") || !strcmp(s[i].type, "wayland") || !strcmp(s[i].type, "mir");
        int alive = !strcmp(s[i].state, "active") || !strcmp(s[i].state, "online");
        if (!graphical || !alive || strcmp(s[i].seat, seat)) continue;
        snprintf(why, wn, "a %s desktop session (%s) runs on %s", s[i].type, s[i].id, seat);
        return 1;
    }
    return 0;
}

void rdx_card_seat(const char *card, char *seat, size_t n)
{
    snprintf(seat, n, "seat0");
    char cmd[400], line[256];
    if (strchr(card, '\'')) return;
    snprintf(cmd, sizeof(cmd), "udevadm info -q property -n '%s' 2>/dev/null", card);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "ID_SEAT=", 8)) {
            line[strcspn(line, "\n")] = 0;
            if (line[8]) snprintf(seat, n, "%s", line + 8);
        }
    pclose(f);
}

int rdx_drm_other_master(const char *card)
{
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    /* AUTH_MAGIC is a master-only ioctl: EACCES = not master (someone else is); anything else
     * (magic 0 is never valid) = we became master on open, i.e. nobody held the card */
    struct drm_auth a = {.magic = 0};
    int r = ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &a);
    int other = r < 0 && errno == EACCES;
    close(fd);
    return other;
}

int rdx_local_decide(int session_blocks, const char *session_why, int other_master, const char *card,
                     char *why, size_t wn)
{
    if (session_blocks) { snprintf(why, wn, "%s", session_why); return 1; }
    if (other_master > 0) { snprintf(why, wn, "another program is DRM master of %s (a desktop or compositor)", card); return 1; }
    if (other_master < 0) { snprintf(why, wn, "cannot open %s", card); return 1; }
    why[0] = 0;
    return 0;
}

int rdx_card_free_for_local(const char *card, char *why, size_t wn)
{
    struct rdx_session_info s[64];
    char seat[64], swhy[160] = "";
    rdx_card_seat(card, seat, sizeof(seat));
    int n = rdx_logind_sessions(s, 64);
    int blocks = n > 0 && rdx_sessions_block_seat(s, n, seat, swhy, sizeof(swhy));
    /* the master test only when no session already says no: it opens the card */
    return rdx_local_decide(blocks, swhy, blocks ? 0 : rdx_drm_other_master(card), card, why, wn);
}
