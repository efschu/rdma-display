/* See rdx_localinput.h. */
#define _GNU_SOURCE
#include "rdx_localinput.h"
#include "rdx_local_proto.h"
#include "../common/rdx_input.h"

#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void set_bit(uint8_t *b, unsigned n) { b[n / 8] |= (uint8_t)(1u << (n % 8)); }

void rdx_localinput_desc(int mouse, struct rdx_input_dev *dp)
{
    struct rdx_input_dev d;
    memset(&d, 0, sizeof(d));
    d.iid.bustype = BUS_VIRTUAL;
    d.iid.vendor = 0x1d6b;            /* Linux Foundation: virtual */
    d.iid.product = mouse ? 0x0102 : 0x0101;
    set_bit(d.ev, EV_KEY);
    if (mouse) {
        snprintf(d.name, sizeof(d.name), "local window mouse");
        set_bit(d.ev, EV_REL);
        for (unsigned b = BTN_LEFT; b <= BTN_TASK; b++) set_bit(d.key, b);
        set_bit(d.rel, REL_X); set_bit(d.rel, REL_Y); set_bit(d.rel, REL_WHEEL); set_bit(d.rel, REL_HWHEEL);
    } else {
        snprintf(d.name, sizeof(d.name), "local window keyboard");
        for (unsigned k = KEY_ESC; k < BTN_MISC; k++) set_bit(d.key, k);
        for (unsigned k = KEY_OK; k < BTN_DPAD_UP; k++) set_bit(d.key, k);
    }
    *dp = d;
}

static int make(int mouse)
{
    struct rdx_input_dev d;
    rdx_localinput_desc(mouse, &d);
    /* for our gamescope only: on the desktop it would also type into the focused window, i.e.
     * into our own window, which sends it back here (common/rdx_input.h, RDX_INPUT_PRIVATE_PHYS) */
    int fd = rdx_uinput_create_phys(&d, "rdx: ", RDX_INPUT_PRIVATE_PHYS);
    if (fd >= 0) printf("[gs] local window input: uinput \"rdx: %s\" (seat " RDX_INPUT_PRIVATE_SEAT ")\n", d.name);
    return fd;
}

static void emit(int fd, int type, int code, int value)
{
    struct input_event e[2];
    memset(e, 0, sizeof(e));
    e[0].type = (unsigned short)type; e[0].code = (unsigned short)code; e[0].value = value;
    e[1].type = EV_SYN; e[1].code = SYN_REPORT;
    if (write(fd, e, sizeof(e)) != (ssize_t)sizeof(e)) { /* a full uinput queue drops the event */ }
}

void rdx_localinput_init(struct rdx_localinput *li)
{
    memset(li, 0, sizeof(*li));
    li->kfd = li->mfd = -1;
}

void rdx_localinput_event(struct rdx_localinput *li, int kind, int code, int value)
{
    if (code < 0 || code >= KEY_CNT) return;
    int mouse = kind != RDX_LOCAL_IN_KEY;
    int *fd = mouse ? &li->mfd : &li->kfd;
    if (*fd < 0 && (*fd = make(mouse)) < 0) return;
    if (kind == RDX_LOCAL_IN_REL) {
        if (code < REL_CNT && value) emit(*fd, EV_REL, code, value);
        return;
    }
    if ((unsigned)code / 8 >= sizeof(li->down)) return;
    if (value) li->down[code / 8] |= (unsigned char)(1u << (code % 8));
    else li->down[code / 8] &= (unsigned char)~(1u << (code % 8));
    emit(*fd, EV_KEY, code, value ? 1 : 0);
}

void rdx_localinput_release(struct rdx_localinput *li)
{
    for (unsigned c = 0; c < sizeof(li->down) * 8; c++) {
        if (!(li->down[c / 8] & (1u << (c % 8)))) continue;
        int fd = c >= BTN_MOUSE && c < BTN_JOYSTICK ? li->mfd : li->kfd;
        if (fd >= 0) emit(fd, EV_KEY, (int)c, 0);
    }
    memset(li->down, 0, sizeof(li->down));
}

void rdx_localinput_close(struct rdx_localinput *li)
{
    rdx_localinput_release(li);
    if (li->kfd >= 0) close(li->kfd);
    if (li->mfd >= 0) close(li->mfd);
    li->kfd = li->mfd = -1;
}
