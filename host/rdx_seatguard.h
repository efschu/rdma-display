/* SPDX-License-Identifier: MIT
 *
 * May rdx-hostd drive a GPU's connectors itself (the home app in gamescope's DRM backend on
 * the seat's own monitor, docs/SEAT-MONITOR.md)? Only if nobody else shows a picture there:
 *  - no graphical logind session (x11 / wayland / mir, active or online) on the card's seat:
 *    a desktop of this computer owns its monitors, even the ones of another card of that seat
 *    (mutter / KWin open every card of their seat);
 *  - no other process is the card's DRM master: open() makes the opener master only if the card
 *    has none, so "we are master after open" means free (non-destructive: closing drops it
 *    again; a compositor that holds the card keeps it).
 * A seat container has no graphical session and its card no master: free. A desktop machine
 * (e.g. GNOME on the iGPU's monitors, the NVIDIA card a render GPU) is refused, so its
 * hostd stays "stream only".
 */
#pragma once
#include <stddef.h>

struct rdx_session_info {
    char id[32], type[16], seat[32], state[16];
};

/* the sessions logind knows (loginctl), at most max; -1 if loginctl is not there */
int rdx_logind_sessions(struct rdx_session_info *out, int max);

/* does one of these sessions own a picture on `seat`? 1 = yes (the first such in why) */
int rdx_sessions_block_seat(const struct rdx_session_info *s, int n, const char *seat, char *why, size_t wn);

/* the card's seat from udev (ID_SEAT), "seat0" without one */
void rdx_card_seat(const char *card, char *seat, size_t n);

/* 1: another process is DRM master of the card, 0: none (we were master for a moment), -1: error */
int rdx_drm_other_master(const char *card);

/* the decision from its parts (unit-tested): 0 = free; else why (session, master, error) */
int rdx_local_decide(int session_blocks, const char *session_why, int other_master, const char *card,
                     char *why, size_t wn);

/* all of it for one card: 0 = free for a local gamescope, else the reason in why */
int rdx_card_free_for_local(const char *card, char *why, size_t wn);
