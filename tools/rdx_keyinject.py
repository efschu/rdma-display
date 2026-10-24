#!/usr/bin/env python3
"""Key presses for the end-to-end latency measurement (tools/rdx_perf.py --latency).

    rdx_keyinject.py [--name "rdx-e2e keys"] [--count 80] [--min-ms 120] [--max-ms 280]
                     [--wait-file PATH] [--wait-s 20] [--key 42] [--mouse [--step 40]]

Creates a uinput keyboard (keys 1..127, so udev and libinput take it as a keyboard) and presses only
one key (default Left Shift, harmless on any desktop if it is ever not grabbed), prints its event
node ("[key] device /dev/input/eventN"), and waits for --wait-file to exist (the caller starts rdx_input_tx --input <node> meanwhile, which grabs it).
Then it presses and releases the key --count times at random intervals between --min-ms and
--max-ms (random so the presses fall on every phase of the display's vblank) and prints one line per
press: "[key] press N t_ns=T" with T = CLOCK_MONOTONIC just before the write, the clock
rdx_sink_null's "[watch]" lines use. Raw uinput ioctls, no python-evdev needed.

--mouse: a relative mouse instead (REL_X/REL_Y, left and right button, so udev and libinput take it as
a mouse), and every "press" is one motion of --step counts to the right, then back to the left: the
pointer goes to and fro between two places (the same speed both ways, so the pointer acceleration
moves it back exactly), for the pointer latency (rdx_sink_null --watch-cursor). The lines are the same,
"[key] press N t_ns=T", T just before the motion's write.
"""
import argparse
import fcntl
import glob
import os
import random
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_DEV_CREATE, UI_DEV_DESTROY = 0x40045564, 0x40045565, 0x5501, 0x5502
UI_SET_RELBIT = 0x40045566
EV_SYN, EV_KEY, EV_REL, BUS_VIRTUAL = 0, 1, 2, 6
REL_X, REL_Y, BTN_LEFT, BTN_RIGHT = 0, 1, 0x110, 0x111


def find_node(name, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for p in glob.glob('/sys/class/input/event*/device/name'):
            try:
                if open(p).read().strip() == name:
                    return '/dev/input/' + p.split('/')[4]
            except OSError:
                pass
        time.sleep(0.05)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--name', default='rdx-e2e keys')
    ap.add_argument('--count', type=int, default=80)
    ap.add_argument('--min-ms', type=float, default=120)
    ap.add_argument('--max-ms', type=float, default=280)
    ap.add_argument('--wait-file')
    ap.add_argument('--wait-s', type=float, default=20)
    ap.add_argument('--key', type=int, default=42)   # KEY_LEFTSHIFT
    ap.add_argument('--mouse', action='store_true', help='a relative mouse that moves to and fro')
    ap.add_argument('--step', type=int, default=40, help='--mouse: counts per motion')
    a = ap.parse_args()
    if a.mouse and a.name == 'rdx-e2e keys':
        a.name = 'rdx-e2e mouse'
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    if a.mouse:                      # udev: REL_X + REL_Y + BTN_LEFT = ID_INPUT_MOUSE
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
        fcntl.ioctl(fd, UI_SET_RELBIT, REL_X)
        fcntl.ioctl(fd, UI_SET_RELBIT, REL_Y)
        fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
        fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT)
    else:
        for k in range(1, 128):      # a full keyboard for udev (ID_INPUT_KEYBOARD); only --key is pressed
            fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    # struct uinput_user_dev: name[80], input_id {bustype, vendor, product, version}, ff_effects_max, abs*[64]
    os.write(fd, struct.pack('80sHHHHi', a.name.encode()[:79], BUS_VIRTUAL, 0x1d6b, 0x0e2e, 1, 0) + b'\0' * (4 * 64 * 4))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    try:
        node = find_node(a.name)
        print('[key] device %s' % node, flush=True)
        if not node:
            sys.exit('[key] the uinput device did not appear')
        if a.wait_file:
            end = time.monotonic() + a.wait_s
            while not os.path.exists(a.wait_file):
                if time.monotonic() > end:
                    sys.exit('[key] no %s within %.0f s' % (a.wait_file, a.wait_s))
                time.sleep(0.02)

        def ev(etype, code, value):
            t = time.time()
            os.write(fd, struct.pack('llHHi', int(t), int((t % 1) * 1e6), etype, code, value))

        for i in range(a.count):
            time.sleep(random.uniform(a.min_ms, a.max_ms) / 1e3)
            t_ns = time.monotonic_ns()
            if a.mouse:
                ev(EV_REL, REL_X, a.step if i % 2 == 0 else -a.step)
                ev(EV_SYN, 0, 0)
                print('[key] press %d t_ns=%d' % (i, t_ns), flush=True)
                continue
            ev(EV_KEY, a.key, 1)
            ev(EV_SYN, 0, 0)
            print('[key] press %d t_ns=%d' % (i, t_ns), flush=True)
            time.sleep(0.02)
            ev(EV_KEY, a.key, 0)
            ev(EV_SYN, 0, 0)
        time.sleep(0.5)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == '__main__':
    main()
