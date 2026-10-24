/* SPDX-License-Identifier: MIT
 *
 * In-stream menu, clientd side: owns the menu socket (common/rdx_menu_ipc.h), keeps the
 * menu model, renders it (rdx_menu_draw) into two memfd buffers that the sink shows on
 * an overlay plane. Opened with Ctrl+Alt+Shift+M (detected by rdx_input_tx).
 *
 * Entries: statistics (from the sink), the link line (common/rdx_link.h: the network module is hot,
 * the link is down and why; also on the placeholder, and for a few seconds as a card over the stream
 * when it is new), cursor on/off, output, the host's own monitor off/on (an
 * app that also shows on the host seat's monitor, docs/SEAT-MONITOR.md), microphone mute (also
 * the mic-mute key: RDX_MENU_MICMUTE from rdx_input_tx, menu open or not), clipboard shared / off, for a window client
 * (p4/rdx_input_win, RDX_MENU_CAP_POINTER) the mouse: auto / absolute / relative (captured; auto =
 * relative when the app locks the pointer, the source's hint), the host's other apps waiting in the
 * background ("Switch to <App>": this one waits there instead, that one resumes; "Quit <App>"), resume,
 * disconnect / end the stream, quit app; while the start placeholder has waited RDX_PH_WAIT_S also "Restart
 * the host program". The same overlay buffers carry the start placeholder when the menu is closed.
 * Cursor and output go to the host as session.update; disconnect ends this client's session
 * (the host keeps a gamescope app running, suspended, until it is resumed or quit).
 * Everything runs in clientd's main loop (one epoll fd), under its lock.
 */
#pragma once
#include <stddef.h>
#include "../common/rdx_menu_ipc.h"
#include "rdx_placeholder.h"

struct rdx_menu_actions {
    void (*quit)(void);                          /* end the session (an app keeps running) */
    void (*quit_app)(void);                      /* end the session and close the app on the host */
    void (*update)(int cursor, const char *output);   /* session.update; cursor -1 / output NULL = unchanged */
    void (*monitor)(int on);                     /* session.update {monitor}: the host seat's own monitor */
    void (*restart_host)(void);                  /* host.restart (offered while the placeholder waits long) */
    void (*mic_mute)(int on);                    /* mute / unmute the microphone sender (local) */
    void (*clipboard)(int on);                   /* share the clipboard (config "clipboard", kept) */
    void (*switch_app)(const char *app);         /* this app to the background, that one (in the background) back */
    void (*quit_bg)(const char *app);            /* close that app in the background on the host */
};

/* listen on path (created, mode 0600) and load the font; 0 = ok, the menu is optional */
int  rdx_menu_start(const char *path, const char *ttf, const struct rdx_menu_actions *act);
int  rdx_menu_poll_fd(void);                     /* epoll fd: readable when the menu has work */
void rdx_menu_dispatch(void);
/* a session starts: outputs of the host (names), the current one, cursor state */
void rdx_menu_session(const char *const *outputs, int noutputs, const char *output, int cursor);
/* the host answered a session.update */
void rdx_menu_result(int ok, const char *msg, int cursor, const char *output);
/* the app also shows on the host's own monitor (its name, "" = no such monitor) */
void rdx_menu_monitor(const char *name);
/* the host answered "monitor off / on" */
void rdx_menu_monitor_result(int ok, const char *msg, int on);
/* the start placeholder (client/rdx_placeholder.h) from now until the sink shows the first frame:
 * kind RDX_PH_START or RDX_PH_RESUME, app = the app's name; redrawn every second, back after
 * the menu closes, again when the frames stop (RDX_MENU_FRAME a = 0) until the next frame */
void rdx_menu_placeholder(int kind, const char *app);
/* the app's icon as the host serves it (PNG, JPEG ...); ignored when the placeholder is gone or
 * shows another app (an icon fetch that came back late) */
void rdx_menu_placeholder_icon(const char *app, const unsigned char *data, size_t len);
/* the host says the picture pauses (session.stall: the source reconfigures, switches its output,
 * was detached): the placeholder "Waiting for the picture of <app>" over the last frame until the
 * next one. Never from a mere absence of frames: a still picture stays untouched. */
void rdx_menu_stall(const char *why);
/* the host's notice (session.notice: the app ended right after its start, e.g. it is already
 * open on the host's desktop) on the placeholder card, with the app's name and icon, until the
 * session ends; closes the menu */
void rdx_menu_notice(const char *text);
/* the session sends the microphone (available) and its mute state; also after outside changes */
void rdx_menu_mic(int available, int muted);
/* the host's other apps running in the background (ids and names, at most 4): the entries "Switch to
 * <App>" (Left/Right picks, Enter: this app waits in the background, that one resumes) and "Quit <App>" */
void rdx_menu_background(const char *const *ids, const char *const *names, int n);
/* a warning from the host (little free memory on its graphics card with apps in the background): a card
 * over the stream for a few seconds, also on the start placeholder */
void rdx_menu_warn(const char *text);
/* the session shares the clipboard: the host offers it (available) and our switch */
void rdx_menu_clip(int available, int on);
/* the session's pointer setting (RDX_PTR_*, config "pointer") and whether it mirrors a desktop */
void rdx_menu_pointer(int setting, int mirror);
/* the link line of both computers (rdx_link_line, "" = all well): in the menu, on the placeholder, and
 * as a card over the stream when its kind changes (hot / back: LINK_CARD_MS, down: until it changes) */
void rdx_menu_link(const char *line);
/* the session's display (rdx-clientd "display"): VRR on and its range, shown in the statistics */
void rdx_menu_display(int vrr, double min_hz, double max_hz);
/* the desktop hotkey as shown on the "Desktop" card (RDX_MENU_DESKTOP from the input), e.g. "Ctrl+Alt+H" */
void rdx_menu_hotkey(const char *text);
/* the session ended: menu closed, placeholder off */
void rdx_menu_end(void);
