#!/usr/bin/env python3
"""Test helper: creates a uinput keyboard, presses Left Shift, removes the device again.

    rdx-uinput-key.py AFTER_S HOLD_S [NAME [PHYS [PRESSES]]]

Waits AFTER_S seconds after creating the device (so the compositor or rdx_input_tx opens it),
presses and releases Left Shift PRESSES times (default 1, 0.2 s apart), keeps the device HOLD_S
seconds more. NAME defaults to "rdx:test keyboard": a name starting with "rdx:" is what
rdx_input_rx gives the client's devices, so gamescope's seat monitor counts the key as the client's
input (the clock follows the client); any other name is local input. PHYS "rdx:private..." puts the
device on the seat no desktop reads (desktop/72-rdma-display-input.rules): a source for
rdx_input_tx --no-grab in a test that must not type into the desktop.
Raw uinput ioctls, no python-evdev needed (the seat containers do not carry it). Needs /dev/uinput.
"""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_DEV_CREATE, UI_DEV_DESTROY = 0x40045564, 0x40045565, 0x5501, 0x5502
UI_SET_PHYS = 0x4008556c                      # _IOW('U', 108, char *)
EV_SYN, EV_KEY, KEY_LEFTSHIFT, BUS_VIRTUAL = 0, 1, 42, 6


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    after, hold = float(sys.argv[1]), float(sys.argv[2])
    name = (sys.argv[3] if len(sys.argv) > 3 else 'rdx:test keyboard').encode()[:79]
    phys = sys.argv[4].encode() if len(sys.argv) > 4 and sys.argv[4] else b''
    presses = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT)
    if phys:
        fcntl.ioctl(fd, UI_SET_PHYS, phys + b'\0')
    # struct uinput_user_dev: name[80], input_id {bustype, vendor, product, version}, ff_effects_max,
    # absmax/absmin/absfuzz/absflat[64] each
    os.write(fd, struct.pack('80sHHHHi', name, BUS_VIRTUAL, 1, 1, 1, 0) + b'\0' * (4 * 64 * 4))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    try:
        time.sleep(after)

        def ev(etype, code, value):
            t = time.time()
            os.write(fd, struct.pack('llHHi', int(t), int((t % 1) * 1e6), etype, code, value))

        for i in range(presses):
            ev(EV_KEY, KEY_LEFTSHIFT, 1)
            ev(EV_SYN, 0, 0)
            time.sleep(0.05)
            ev(EV_KEY, KEY_LEFTSHIFT, 0)
            ev(EV_SYN, 0, 0)
            if i + 1 < presses:
                time.sleep(0.2)
        print('[key] Left Shift x%d from "%s"%s' % (presses, name.decode(), ' (phys %s)' % phys.decode() if phys else ''),
              flush=True)
        time.sleep(hold)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == '__main__':
    main()
