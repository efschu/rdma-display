/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "rdx_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/uinput.h>

int rdx_evdev_describe(int fd, uint32_t id, struct rdx_input_dev *d)
{
    memset(d, 0, sizeof(*d));
    d->id = id;
    if (ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name) < 0) strcpy(d->name, "unknown");
    if (ioctl(fd, EVIOCGID, &d->iid) < 0) return -1;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(d->ev)), d->ev) < 0) return -1;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(d->key)), d->key);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(d->rel)), d->rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(d->abs)), d->abs);
    ioctl(fd, EVIOCGBIT(EV_MSC, sizeof(d->msc)), d->msc);
    ioctl(fd, EVIOCGPROP(sizeof(d->prop)), d->prop);
    for (unsigned a = 0; a < ABS_CNT; a++)
        if (rdx_test_bit(d->abs, a)) ioctl(fd, EVIOCGABS(a), &d->absinfo[a]);
    return 0;
}

static void set_bits(int ufd, unsigned long req, const uint8_t *bits, unsigned cnt)
{
    for (unsigned i = 0; i < cnt; i++)
        if (rdx_test_bit(bits, i)) ioctl(ufd, req, i);
}

int rdx_uinput_create(const struct rdx_input_dev *d, const char *name_prefix)
{
    return rdx_uinput_create_phys(d, name_prefix, NULL);
}

int rdx_uinput_create_phys(const struct rdx_input_dev *d, const char *name_prefix, const char *phys)
{
    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) { perror("/dev/uinput"); return -1; }
    /* before UI_DEV_CREATE: udev's rules see it with the "add" event */
    if (phys && *phys && ioctl(ufd, UI_SET_PHYS, phys) < 0) perror("UI_SET_PHYS");
    for (unsigned t = 0; t < EV_CNT; t++)
        if (rdx_test_bit(d->ev, t) && t != EV_SYN && t != EV_REP) ioctl(ufd, UI_SET_EVBIT, t);
    uint8_t key[sizeof(d->key)];           /* never a system key on a mirror (rdx_input_system_key) */
    memcpy(key, d->key, sizeof(key));
    for (unsigned k = 0; k < KEY_CNT; k++)
        if (rdx_input_system_key(k)) key[k / 8] &= (uint8_t)~(1u << (k % 8));
    set_bits(ufd, UI_SET_KEYBIT, key, KEY_CNT);
    set_bits(ufd, UI_SET_RELBIT, d->rel, REL_CNT);
    set_bits(ufd, UI_SET_MSCBIT, d->msc, MSC_CNT);
    set_bits(ufd, UI_SET_PROPBIT, d->prop, INPUT_PROP_CNT);
    for (unsigned a = 0; a < ABS_CNT; a++) {
        if (!rdx_test_bit(d->abs, a)) continue;
        ioctl(ufd, UI_SET_ABSBIT, a);
        struct uinput_abs_setup as = {.code = (uint16_t)a, .absinfo = d->absinfo[a]};
        if (ioctl(ufd, UI_ABS_SETUP, &as) < 0) perror("UI_ABS_SETUP");
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id = d->iid;
    snprintf(us.name, sizeof(us.name), "%s%s", name_prefix ? name_prefix : "", d->name);
    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        perror("uinput setup/create");
        close(ufd);
        return -1;
    }
    return ufd;
}

int rdx_evdev_kbd_relevant(int fd)
{
    uint8_t keys[KEY_CNT / 8 + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return 0;
    for (unsigned k = BTN_JOYSTICK; k < BTN_DIGI; k++)
        if (rdx_test_bit(keys, k)) return 0;               /* a joystick / gamepad: Steam reads it */
    for (unsigned k = KEY_ESC; k < BTN_MISC; k++)
        if (rdx_test_bit(keys, k)) return 1;
    return 0;
}

void rdx_grab_gate_init(struct rdx_grab_gate *g, const char *evpath, const char *what)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->what, sizeof(g->what), "%s", what);
    g->efd = open(evpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);   /* never read: its queue may overflow */
    if (g->efd >= 0 && !rdx_evdev_kbd_relevant(g->efd)) {
        close(g->efd);
        g->efd = -1;
    }
}

int rdx_grab_gate_pass(struct rdx_grab_gate *g)
{
    if (g->efd < 0) return 1;
    uint64_t now = rdx_now_ns();
    if (g->grabbed && now - g->t_ok < 500000000ull) return 1;
    if (ioctl(g->efd, EVIOCGRAB, 1) == 0) {                 /* nobody holds it: release, hold back */
        ioctl(g->efd, EVIOCGRAB, 0);
        if (!g->held++)
            printf("[grab] \"%s\": keys held back until its consumer grabs it (the host's console would "
                   "see them)\n", g->what);
        g->grabbed = 0;
        return 0;
    }
    if (errno != EBUSY) return 1;                           /* cannot tell (device gone): no block */
    if (!g->grabbed && g->held)
        printf("[grab] \"%s\": grabbed by its consumer, keys pass (%ld report(s) held back)\n", g->what, g->held);
    g->grabbed = 1;
    g->t_ok = now;
    return 1;
}

void rdx_grab_gate_close(struct rdx_grab_gate *g)
{
    if (g->efd >= 0) close(g->efd);
    g->efd = -1;
}

int rdx_uinput_event_path(int ufd, char *path, size_t len)
{
    char sys[64] = {0}, dir[160];
    if (ioctl(ufd, UI_GET_SYSNAME(sizeof(sys) - 1), sys) < 0) { perror("UI_GET_SYSNAME"); return -1; }
    snprintf(dir, sizeof(dir), "/sys/devices/virtual/input/%s", sys);
    for (int tries = 0; tries < 200; tries++) {
        DIR *dp = opendir(dir);
        struct dirent *e;
        int found = 0;
        while (dp && (e = readdir(dp))) {
            if (!strncmp(e->d_name, "event", 5)) {
                snprintf(path, len, "/dev/input/%s", e->d_name);
                found = 1;
                break;
            }
        }
        if (dp) closedir(dp);
        if (found && access(path, R_OK) == 0) return 0;
        usleep(10000);
    }
    fprintf(stderr, "no event node for %s\n", dir);
    return -1;
}
