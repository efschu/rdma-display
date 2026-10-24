/* SPDX-License-Identifier: MIT
 *
 * In-stream menu: local IPC between rdx-clientd (owner of the socket, menu model and
 * renderer), rdx_input_tx (hotkey, keys while the menu is open) and rdx_sink_kms (shows
 * the rendered menu on a DRM overlay plane, reports its stats).
 *
 * One SOCK_SEQPACKET unix socket (default /run/rdx/menu.sock), fixed-size messages,
 * file descriptors only with RDX_MENU_BUFFERS (SCM_RIGHTS). Every peer starts with
 * RDX_MENU_HELLO carrying its role. Nothing here is on a hot path: the sink applies an
 * overlay change with its next stream flip (one atomic commit, no extra vblank).
 */
#pragma once
#include <stdint.h>

#define RDX_MENU_SOCK_PATH "/run/rdx/menu.sock"
#define RDX_MENU_NBUF    2          /* overlay buffers: one shown, one being rendered */

enum rdx_menu_role { RDX_MENU_ROLE_INPUT = 1, RDX_MENU_ROLE_SINK = 2 };

enum rdx_menu_type {
    RDX_MENU_HELLO = 1,     /* any -> clientd: a = role; sink: b,c = mode w,h, d = refresh mHz, e = 1 a window
                             * sink (p0b/rdx_sink_win: the overlay is blended by GL over the frame);
                             * input: b = capabilities RDX_MENU_CAP_* */
    RDX_MENU_TOGGLE,        /* input -> clientd: hotkey Ctrl+Alt+Shift+M pressed */
    RDX_MENU_KEY,           /* input -> clientd: a = evdev key code, b = value (1 press, 2 repeat) */
    RDX_MENU_STATE,         /* clientd -> input: a = 1 menu open (keys go to the menu), 0 closed */
    RDX_MENU_STATS,         /* sink -> clientd, once per second: a = shown/s, b = hitches/s,
                             * c = dropped/s, d = arrival -> visible p50 in us, e = refresh mHz */
    RDX_MENU_BUFFERS,       /* clientd -> sink, with RDX_MENU_NBUF memfds: a,b = w,h, c = stride,
                             * d,e = position x,y on the sink's mode (ARGB8888, premultiplied) */
    RDX_MENU_SHOW,          /* clientd -> sink: a = buffer index to show (fully rendered); b = 1: the start
                             * placeholder (client/rdx_placeholder.h), shown until the next stream frame:
                             * the sink takes it off in the commit that shows that frame; ignored once
                             * frames flow. b = 2: a placeholder the host asked for (session.stall, a
                             * notice): over the last frame, static or not, until the next frame */
    RDX_MENU_HIDE,          /* clientd -> sink */
    RDX_MENU_OVERLAY,       /* sink -> clientd: a = 1 overlay plane usable, 0 not (b = errno) */
    RDX_MENU_FRAME,         /* sink -> clientd: a = 1 the first stream frame after none (or after a
                             * placeholder) goes on screen now; a = 0 is no longer sent (older sinks:
                             * the frames stopped for RDX_MENU_STALL_MS), clientd ignores it */
    RDX_MENU_MICMUTE,       /* input -> clientd: the mic-mute key (KEY_MICMUTE), menu open or not */
    RDX_MENU_POINTER,       /* sink -> clientd: a = the source's pointer hint, on change: -1 none, 0 absolute is
                             * right, 1 the app wants relative motion (its cursor is hidden and the pointer
                             * locked, common/rdx_common.h RDX_HDR_F_REL_MOUSE);
                             * the sink's b = 1: it shows the host's cursor itself as the window's cursor (the
                             * cursor next to the picture, common/rdx_cursor.h), 0 = the stream shows it;
                             * clientd -> input (with RDX_MENU_CAP_POINTER only): a = 1 relative motion with the
                             * pointer captured in the window, 0 absolute; b = 1 the stream shows the host's
                             * cursor (the local one is hidden over the window), 2 = the window's cursor is the
                             * host's, set by the sink (the input leaves it alone); c = the setting
                             * (RDX_PTR_*), d = the hint */
    RDX_MENU_DESKTOP,       /* input -> clientd: a = 1 keyboard and mouse went to the desktop (the desktop
                             * hotkey, common/rdx_hotkey.h), 0 back in the stream */
};

/* input capabilities (HELLO b) */
#define RDX_MENU_CAP_POINTER 1      /* a window's input (p4/rdx_input_win): absolute or captured relative pointer */

/* the pointer setting of a window client (config "pointer", the menu's "Mouse" entry) */
enum rdx_ptr_setting { RDX_PTR_AUTO = 0, RDX_PTR_ABSOLUTE = 1, RDX_PTR_RELATIVE = 2 };

/* no stream frame for this long after frames came: the sink shows overlay changes without waiting
 * for a stream flip (a still picture is no event: nothing is reported, no placeholder) */
#define RDX_MENU_STALL_MS 2500
/* a placeholder the host asked for (SHOW b = 2) shows only if no frame comes within this */
#define RDX_MENU_STALL_GRACE_MS 500

struct rdx_menu_msg {
    uint32_t type;
    int32_t a, b, c, d, e;
};
