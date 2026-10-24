/* SPDX-License-Identifier: MIT
 * System keys never reach a host (common/rdx_input.h rdx_input_system_key): the list, and - where
 * /dev/uinput is writable - a mirror made by rdx_uinput_create_phys has no KEY_POWER, so a KEY_POWER
 * written into it never comes out of its event node, while KEY_A does.
 *   tests/test_syskeys        exit 0 = PASS (the uinput part is skipped without /dev/uinput) */
#include "rdx_input.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fails;
#define CHECK(cond, ...)                                                                            \
    do {                                                                                            \
        if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } \
        else { printf("ok   "); printf(__VA_ARGS__); puts(""); }                                     \
    } while (0)

static void wr(int ufd, unsigned type, unsigned code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint16_t)type;
    ev.code = (uint16_t)code;
    ev.value = value;
    if (write(ufd, &ev, sizeof(ev)) < 0) perror("write");
}

int main(void)
{
    const unsigned sys[] = {KEY_POWER, KEY_POWER2, KEY_SLEEP, KEY_SUSPEND, KEY_WAKEUP, KEY_RESTART, KEY_BATTERY, KEY_RFKILL};
    const unsigned keep[] = {KEY_A, KEY_ESC, KEY_VOLUMEUP, KEY_MUTE, KEY_MICMUTE, KEY_LEFTMETA, KEY_F12, BTN_LEFT};
    int ok = 1;
    for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++) ok &= rdx_input_system_key(sys[i]);
    CHECK(ok, "power, power2, sleep, suspend, wake-up, restart, battery, rfkill are system keys");
    ok = 1;
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) ok &= !rdx_input_system_key(keep[i]);
    CHECK(ok, "letters, Esc, volume, mute, mic mute, Meta, F12, mouse buttons are not");

    if (access("/dev/uinput", W_OK)) {
        printf("skip the uinput part: no writable /dev/uinput here\n");
    } else {
        struct rdx_input_dev d;
        memset(&d, 0, sizeof(d));
        snprintf(d.name, sizeof(d.name), "test-syskeys %d", (int)getpid());
        d.iid.bustype = BUS_VIRTUAL;
        d.ev[EV_KEY / 8] |= 1u << (EV_KEY % 8);
        d.key[KEY_A / 8] |= 1u << (KEY_A % 8);
        d.key[KEY_POWER / 8] |= 1u << (KEY_POWER % 8);
        d.key[KEY_SLEEP / 8] |= 1u << (KEY_SLEEP % 8);
        int ufd = rdx_uinput_create_phys(&d, "rdx: ", NULL);
        char path[64];
        int efd = ufd >= 0 && !rdx_uinput_event_path(ufd, path, sizeof(path)) ? open(path, O_RDONLY | O_NONBLOCK) : -1;
        CHECK(efd >= 0, "mirror created (%s)", efd >= 0 ? path : "-");
        if (efd >= 0) {
            uint8_t bits[RDX_BITS(KEY_CNT)];
            memset(bits, 0, sizeof(bits));
            ioctl(efd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits);
            CHECK(rdx_test_bit(bits, KEY_A) && !rdx_test_bit(bits, KEY_POWER) && !rdx_test_bit(bits, KEY_SLEEP),
                  "the mirror has KEY_A, not KEY_POWER / KEY_SLEEP");
            wr(ufd, EV_KEY, KEY_POWER, 1); wr(ufd, EV_SYN, SYN_REPORT, 0);
            wr(ufd, EV_KEY, KEY_POWER, 0); wr(ufd, EV_SYN, SYN_REPORT, 0);
            wr(ufd, EV_KEY, KEY_A, 1); wr(ufd, EV_SYN, SYN_REPORT, 0);
            wr(ufd, EV_KEY, KEY_A, 0); wr(ufd, EV_SYN, SYN_REPORT, 0);
            struct pollfd p = {efd, POLLIN, 0};
            int power = 0, a = 0;
            while (poll(&p, 1, 300) > 0) {
                struct input_event ev[16];
                ssize_t r = read(efd, ev, sizeof(ev));
                if (r <= 0) break;
                for (size_t i = 0; i < (size_t)r / sizeof(ev[0]); i++) {
                    if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER) power++;
                    if (ev[i].type == EV_KEY && ev[i].code == KEY_A) a++;
                }
            }
            CHECK(power == 0 && a == 2, "a KEY_POWER written into the mirror never comes out (%d), KEY_A does (%d)", power, a);
            close(efd);
        }
        if (ufd >= 0) close(ufd);
    }
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
