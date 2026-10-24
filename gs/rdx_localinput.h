/* The local window's own keyboard and mouse (gs/rdx_local_proto.h INPUT), injected by rdx_gs_src
 * into two uinput devices "rdx: local window keyboard" / "rdx: local window mouse". gamescope's
 * rdx backend reads only devices named "rdx: ..." (the client's mirrors from rdx_input_rx and
 * these), so the desktop's real keyboard and mouse never reach the app; the window forwards
 * only while it has the focus. Created on the first event, released (all keys up) on close. */
#pragma once
struct rdx_input_dev;

struct rdx_localinput {
    int kfd, mfd;                 /* uinput fds, -1 = not created yet */
    unsigned char down[96];       /* keys / buttons currently down (bit per evdev code < 768) */
};

void rdx_localinput_init(struct rdx_localinput *li);
/* the capabilities of the two devices (mouse 0: keyboard, 1: mouse), e.g. for rdx_input_rx's
 * mirrors of a window client (p4/rdx_input_win) */
void rdx_localinput_desc(int mouse, struct rdx_input_dev *d);
/* kind RDX_LOCAL_IN_KEY / _REL / _BUTTON, evdev code, value (key: 1 down / 0 up; rel: delta) */
void rdx_localinput_event(struct rdx_localinput *li, int kind, int code, int value);
/* every key and button that is down goes up (the window lost the focus, or it went away) */
void rdx_localinput_release(struct rdx_localinput *li);
void rdx_localinput_close(struct rdx_localinput *li);
