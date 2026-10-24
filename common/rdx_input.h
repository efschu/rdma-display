/* SPDX-License-Identifier: MIT
 *
 * rdma-display input path (P4): wire format and evdev/uinput helpers shared by
 * rdx_input_tx (host with keyboard/mouse, "B") and rdx_input_rx (host running
 * the program, "A"). Plain kernel UAPI (linux/input.h, linux/uinput.h).
 */
#pragma once
#include <stdint.h>
#include <linux/input.h>
#include "rdx_common.h"

#define RDX_INPUT_MAGIC   0x31504e49u   /* "INP1" */
#define RDX_INPUT_MAX_DEV 8
#define RDX_INPUT_MAX_EV  24
#define RDX_INPUT_INLINE  256
#define RDX_PROBE_NAME    "rdx-probe"   /* MSC_SCAN-only test device, ignored by desktops */
#define RDX_EXIT_RELEASED 10            /* rdx_input_tx exit code: user pressed the release combo */
#define RDX_PROBE_SILENT  "rdx-probe-silent" /* same, but rx neither reads back nor acks:
                                              * CPU of the plain path, as with a real device */

#define RDX_BITS(n) (((n) + 7) / 8)

/* one captured device, sent over TCP during bootstrap */
struct rdx_input_dev {
    uint32_t id;
    char name[80];
    struct input_id iid;
    uint8_t ev[RDX_BITS(EV_CNT)];
    uint8_t key[RDX_BITS(KEY_CNT)];
    uint8_t rel[RDX_BITS(REL_CNT)];
    uint8_t abs[RDX_BITS(ABS_CNT)];
    uint8_t msc[RDX_BITS(MSC_CNT)];
    uint8_t prop[RDX_BITS(INPUT_PROP_CNT)];
    struct input_absinfo absinfo[ABS_CNT];
};

/* tx -> rx during bootstrap, followed by ndev struct rdx_input_dev */
struct rdx_input_hello {
    uint32_t magic;
    uint32_t ndev;
    struct rdx_conn conn;
};

struct rdx_input_ev {
    uint16_t type, code;
    int32_t value;
};

/* one evdev report (events up to and including SYN_REPORT) per RDMA SEND;
 * only 8 + n * 8 bytes are sent */
struct rdx_input_pkt {
    uint32_t seq;
    uint16_t dev, n;
    struct rdx_input_ev ev[RDX_INPUT_MAX_EV];
};
#define RDX_INPUT_PKT_LEN(n) (8u + (uint32_t)(n) * (uint32_t)sizeof(struct rdx_input_ev))

/* ack immediate (rx -> tx): bits 0..19 seq, bits 20..31 rx time from uinput
 * write to the event being readable on the mirror's evdev node, in us
 * (probe packets only; RDX_ACK_NO_INJ otherwise) */
#define RDX_ACK_NO_INJ 0xFFFu
static inline uint32_t rdx_ack_imm(uint32_t seq, uint32_t inj_us)
{
    if (inj_us > RDX_ACK_NO_INJ) inj_us = RDX_ACK_NO_INJ - 1;
    return (inj_us << 20) | (seq & 0xFFFFFu);
}
static inline uint32_t rdx_ack_seq(uint32_t imm)    { return imm & 0xFFFFFu; }
static inline uint32_t rdx_ack_inj_us(uint32_t imm) { return imm >> 20; }

static inline int rdx_test_bit(const uint8_t *bits, unsigned n) { return (bits[n / 8] >> (n % 8)) & 1; }

/* System keys: power, sleep, suspend, wake-up, restart, battery, radio kill. They never go to a
 * host and are never a key of a mirror device there: on a host every key device is a
 * "power-switch" of its logind (systemd's 70-power-switch.rules) and the desktop acts on them as
 * well, so a power key pressed at a client would switch the HOST off. They stay on the
 * client (rdx_input_tx gives them to its own logind / desktop); rdx_uinput_create_phys leaves them
 * out of every mirror, and the kernel drops events of keys a device does not have. */
static inline int rdx_input_system_key(unsigned code)
{
    switch (code) {
    case KEY_POWER: case KEY_POWER2: case KEY_SLEEP: case KEY_SUSPEND: case KEY_WAKEUP:
    case KEY_RESTART: case KEY_BATTERY: case KEY_RFKILL:
        return 1;
    default:
        return 0;
    }
}

/* PM QoS, CPU accounting, completion vector and RT priority live in rdx_common */

/* read capabilities of an open evdev fd into d */
int rdx_evdev_describe(int fd, uint32_t id, struct rdx_input_dev *d);
/* create a uinput device with the capabilities in d (name prefixed), without the system keys
 * (rdx_input_system_key); returns the uinput fd */
int rdx_uinput_create(const struct rdx_input_dev *d, const char *name_prefix);

/* Devices meant for ONE gamescope only (the client's keyboard / mouse in a gamescope session, a
 * local window's): their phys starts with RDX_INPUT_PRIVATE_PHYS. desktop/72-rdma-display-input.rules
 * puts them on the seat RDX_INPUT_PRIVATE_SEAT, so no desktop (Xorg, Mutter, KWin, wlroots: all
 * take seat0 only) sees them, and gives them to the group RDX_INPUT_GROUP instead of the seat0
 * session's ACL; gamescope's rdx backend reads that seat as well. Without the phys a device is an
 * ordinary seat0 device (a mirrored desktop gets the client's input). */
#define RDX_INPUT_PRIVATE_PHYS "rdx:private"
#define RDX_INPUT_PRIVATE_SEAT "seat-rdx"
#define RDX_INPUT_GROUP        "rdx-input"
/* rdx_uinput_create with a phys (NULL or "" = none) */
int rdx_uinput_create_phys(const struct rdx_input_dev *d, const char *name_prefix, const char *phys);

/* Grab gate (security: the host's console keyboard stays on). The consumer of a
 * streamed device (gamescope, both backends) grabs it (EVIOCGRAB), so the host's VT keyboard
 * handler never sees its keys. Until it has, key events of the device are held back (dropped):
 * otherwise keys typed in the moment before gamescope opens a new device reach the host console.
 * The check: EVIOCGRAB on our own evdev fd of the device; EBUSY = the consumer holds it (pass),
 * success = nobody does (release at once, hold back). Only devices the VT keyboard handler takes
 * (a key below BTN_MISC, no joystick / gamepad buttons) are gated; mice and gamepads pass. */
struct rdx_grab_gate {
    int efd;                    /* our evdev fd of the device, -1 = no gate (always pass) */
    int grabbed;                /* the consumer held it at the last check */
    uint64_t t_ok;              /* time of that check (re-checked after 500 ms) */
    long held;                  /* reports held back */
    char what[96];
};
/* the VT keyboard handler would take this evdev device (key below BTN_MISC, no joystick buttons) */
int rdx_evdev_kbd_relevant(int fd);
/* open the gate for evpath (/dev/input/eventN); a device that is no keyboard gets no gate */
void rdx_grab_gate_init(struct rdx_grab_gate *g, const char *evpath, const char *what);
/* 1: forward, 0: hold back (prints the first hold and the first pass after holds) */
int rdx_grab_gate_pass(struct rdx_grab_gate *g);
void rdx_grab_gate_close(struct rdx_grab_gate *g);
/* /dev/input/eventN of a uinput device created on ufd; waits up to 2 s for the node */
int rdx_uinput_event_path(int ufd, char *path, size_t len);
