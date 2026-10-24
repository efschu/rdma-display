#!/usr/bin/env python3
"""In-stream menu, input side: rdx_input_tx with a virtual test keyboard (uinput), a fake
rdx-clientd on --menu-sock, and rdx_input_rx over RDMA loopback on this host. Checks that
Ctrl+Alt+Shift+M opens the menu, everything held is released on the receiving side, keys
then go to the menu and NOT to the receiver, and forwarding resumes when the menu closes.
Only modifier keys and a 'k' after closing reach the receiver's mirror device.

  tests/test_menu_input.py REPO RDMA_DEV LINK_IP [PORT]     (root; exit 0 = PASS)
"""
import fcntl, os, select, socket, struct, subprocess, sys, time

REPO, DEV, IP = sys.argv[1], sys.argv[2], sys.argv[3]
PORT = sys.argv[4] if len(sys.argv) > 4 else '18761'
TMP = '/tmp/rdx-menu-input-test'
SOCK = TMP + '/menu.sock'
FMT = '<Iiiiii'
HELLO, TOGGLE, KEY, STATE = 1, 2, 3, 4
EV_SYN, EV_KEY = 0, 1
K = dict(ESC=1, M=50, K=37, DOWN=108, LCTRL=29, LALT=56, LSHIFT=42)
NAME = 'rdx-menu-test-kbd'
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


def uinput_kbd():
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, 0x40045564, EV_KEY)                      # UI_SET_EVBIT
    for code in K.values():
        fcntl.ioctl(fd, 0x40045565, code)                    # UI_SET_KEYBIT
    dev = struct.pack('<80sHHHHi', NAME.encode(), 0x06, 0x1234, 0x5678, 1, 0) + b'\0' * (4 * 64 * 4)
    os.write(fd, dev)
    fcntl.ioctl(fd, 0x5501)                                  # UI_DEV_CREATE
    return fd


def event_path(name):
    for d in os.listdir('/sys/class/input'):
        try:
            if open('/sys/class/input/%s/device/name' % d).read().strip() == name and d.startswith('event'):
                return '/dev/input/' + d
        except OSError:
            pass
    return None


def emit(fd, code, val):
    os.write(fd, struct.pack('<qqHHi', 0, 0, EV_KEY, code, val) + struct.pack('<qqHHi', 0, 0, EV_SYN, 0, 0))
    time.sleep(0.03)


def read_mirror(fd, t=0.4):
    out = []
    end = time.monotonic() + t
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], max(0, end - time.monotonic()))
        if not r:
            break
        data = os.read(fd, 24 * 64)
        for i in range(0, len(data), 24):
            _, _, ty, code, val = struct.unpack('<qqHHi', data[i:i + 24])
            if ty == EV_KEY:
                out.append((code, val))
    return out


os.makedirs(TMP, exist_ok=True)
if os.path.exists(SOCK):
    os.unlink(SOCK)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
srv.bind(SOCK)
srv.listen(2)
kfd = uinput_kbd()
time.sleep(0.5)
kpath = event_path(NAME)
check(kpath is not None, 'virtual test keyboard %s' % kpath)
logs = [open(TMP + '/rx.log', 'w'), open(TMP + '/tx.log', 'w')]
rx = subprocess.Popen([REPO + '/p4/rdx_input_rx', '--dev', DEV, '--bind', IP, '--port', PORT], stdout=logs[0],
                      stderr=subprocess.STDOUT)
time.sleep(0.5)
tx = subprocess.Popen([REPO + '/p4/rdx_input_tx', '--dev', DEV, '--peer', IP, '--port', PORT, '--input', kpath,
                       '--menu-sock', SOCK], stdout=logs[1], stderr=subprocess.STDOUT)
try:
    srv.settimeout(5)
    cl, _ = srv.accept()
    cl.settimeout(1)
    m = struct.unpack(FMT, cl.recv(24))
    check(m[0] == HELLO and m[1] == 1, 'input sender says HELLO as role input')
    time.sleep(0.5)
    mpath = event_path('rdx: ' + NAME)
    check(mpath is not None, 'receiver created its mirror device %s' % mpath)
    mfd = os.open(mpath, os.O_RDONLY | os.O_NONBLOCK)

    emit(kfd, K['K'], 1); emit(kfd, K['K'], 0)
    got = read_mirror(mfd)
    check((K['K'], 1) in got and (K['K'], 0) in got, 'menu closed: k forwarded %s' % got)

    for k in ('LCTRL', 'LALT', 'LSHIFT'):
        emit(kfd, K[k], 1)
    t0 = time.monotonic()
    emit(kfd, K['M'], 1)
    m = struct.unpack(FMT, cl.recv(24))
    check(m[0] == TOGGLE, 'Ctrl+Alt+Shift+M -> TOGGLE to clientd (%.1f ms incl. 30 ms test pacing)' % ((time.monotonic() - t0) * 1e3))
    cl.send(struct.pack(FMT, STATE, 1, 0, 0, 0, 0))
    got = read_mirror(mfd)
    downs = [c for c, v in got if v == 1]
    ups = {c for c, v in got if v == 0}
    check(K['M'] not in downs, 'M itself never reaches the receiver')
    check({K['LCTRL'], K['LALT'], K['LSHIFT']} <= ups, 'modifiers released on the receiver: %s' % got)

    emit(kfd, K['M'], 0)
    for k in ('LSHIFT', 'LALT', 'LCTRL'):
        emit(kfd, K[k], 0)
    emit(kfd, K['DOWN'], 1); emit(kfd, K['DOWN'], 0)
    keys = []
    try:
        while True:
            keys.append(struct.unpack(FMT, cl.recv(24)))
    except socket.timeout:
        pass
    check(any(k[0] == KEY and k[1] == K['DOWN'] and k[2] == 1 for k in keys), 'menu open: DOWN goes to clientd')
    check(not any(k[0] == KEY and k[2] == 0 for k in keys), 'only presses/repeats go to the menu, no releases')
    got = read_mirror(mfd)
    check(not got, 'menu open: nothing reaches the receiver %s' % got)

    cl.send(struct.pack(FMT, STATE, 0, 0, 0, 0, 0))     # clientd closed the menu (Esc / resume)
    time.sleep(0.1)
    emit(kfd, K['K'], 1); emit(kfd, K['K'], 0)
    got = read_mirror(mfd)
    check((K['K'], 1) in got and (K['K'], 0) in got, 'menu closed again: forwarding resumes %s' % got)

    for k in ('LCTRL', 'LALT', 'LSHIFT'):
        emit(kfd, K[k], 1)
    emit(kfd, K['M'], 1)
    cl.recv(24)
    cl.close()                                           # clientd dies while the menu is open
    time.sleep(0.2)
    emit(kfd, K['M'], 0)
    for k in ('LSHIFT', 'LALT', 'LCTRL'):
        emit(kfd, K[k], 0)
    read_mirror(mfd, 0.2)
    emit(kfd, K['K'], 1); emit(kfd, K['K'], 0)
    got = read_mirror(mfd)
    check((K['K'], 1) in got, 'clientd gone -> the keyboard is forwarded again %s' % got)

    emit(kfd, K['LCTRL'], 1); emit(kfd, K['LALT'], 1); emit(kfd, K['ESC'], 1)
    tx.wait(3)
    check(tx.returncode == 10, 'release combo still ends rdx_input_tx with 10 (got %s)' % tx.returncode)
    os.close(mfd)
finally:
    for p in (tx, rx):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                p.kill()
    fcntl.ioctl(kfd, 0x5502)                             # UI_DEV_DESTROY
    os.close(kfd)
    for f in logs:
        f.close()
print('test_menu_input: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
