#!/usr/bin/env python3
"""In-stream menu IPC test against a real rdx_clientd, without a display or a host:
fake sink and fake input sender on the menu socket (common/rdx_menu_ipc.h).

  tests/test_menu_ipc.py [repo-dir]      exit 0 = PASS
"""
import array, mmap, os, socket, struct, subprocess, sys, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-menu-test'
SOCK = TMP + '/menu.sock'
FMT = '<Iiiiii'
HELLO, TOGGLE, KEY, STATE, STATS, BUFFERS, SHOW, HIDE, OVERLAY = range(1, 10)
KEY_ESC, KEY_ENTER, KEY_UP, KEY_DOWN = 1, 28, 103, 108
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


def panel(mode_w, mode_h):
    """The panel size of client/rdx_menu_draw.c rdx_draw_layout(): text height p = mode_h / 38
    (at least 14 px), 30 x 29 line units (title, 2 statistics lines, a warning line, 12 entries, note,
    footer), clamped to the mode, width a multiple of 8, height even."""
    p = max(mode_h // 38, 14)
    return min(p * 30, mode_w) & ~7, min(p * 29, mode_h) & ~1


def msg(t, a=0, b=0, c=0, d=0, e=0):
    return struct.pack(FMT, t, a, b, c, d, e)


def recv(s, timeout=1.0):
    s.settimeout(timeout)
    try:
        fds = array.array('i')
        data, anc, _, _ = s.recvmsg(24, socket.CMSG_SPACE(2 * fds.itemsize))
    except socket.timeout:
        return None, []
    for lvl, typ, cd in anc:
        if lvl == socket.SOL_SOCKET and typ == socket.SCM_RIGHTS:
            fds.frombytes(cd[:len(cd) - len(cd) % fds.itemsize])
    return struct.unpack(FMT, data), list(fds)


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.connect(SOCK)
    return s


os.makedirs(TMP, exist_ok=True)
if os.path.exists(SOCK):
    os.unlink(SOCK)                 # a socket left by an earlier run would pass the "listens" wait too early
log = open(TMP + '/clientd.log', 'w')
cd = subprocess.Popen([REPO + '/client/rdx_clientd', '--config', TMP + '/client.json', '--no-vt',
                       '--web-port', '47291', '--host-port', '47299', '--menu-sock', SOCK],
                      stdout=log, stderr=subprocess.STDOUT)
try:
    for _ in range(100):
        if os.path.exists(SOCK):
            break
        time.sleep(0.05)
    check(os.path.exists(SOCK), 'clientd listens on the menu socket')

    sink = connect()
    sink.send(msg(HELLO, 2, 1920, 1080, 240000))
    m, fds = recv(sink)
    check(m is not None and m[0] == BUFFERS and len(fds) == 2, 'sink gets BUFFERS with 2 fds')
    w, h, stride, x, y = m[1:6]
    check((w, h) == panel(1920, 1080) and stride % 256 == 0 and x == (1920 - w) // 2 and y == (1080 - h) // 2,
          'panel %dx%d stride %d at %d,%d (centred on 1920x1080)' % (w, h, stride, x, y))
    maps = [mmap.mmap(f, os.fstat(f).st_size) for f in fds]
    seals = [__import__('fcntl').fcntl(f, 1034) for f in fds]          # F_GET_SEALS
    check(all(sv & 0x2 for sv in seals), 'memfds sealed against shrinking (udmabuf needs it)')
    sink.send(msg(OVERLAY, 1))

    inp = connect()
    inp.send(msg(HELLO, 1))
    t0 = time.monotonic()
    inp.send(msg(TOGGLE))
    st, _ = recv(inp)
    sh, _ = recv(sink)
    t_open = (time.monotonic() - t0) * 1e3
    check(st is not None and st[0] == STATE and st[1] == 1, 'input told: menu open')
    check(sh is not None and sh[0] == SHOW, 'sink told: SHOW buffer %s (%.1f ms after the hotkey)' % (sh and sh[1], t_open))
    k = sh[1]
    px = struct.unpack_from('<%dI' % (stride // 4 * h), maps[k])
    opaque = sum(1 for p in px if (p >> 24) > 200)
    check(opaque > w * h * 0.8, 'buffer %d is rendered (%d of %d pixels opaque)' % (k, opaque, w * h))

    t0 = time.monotonic()
    inp.send(msg(KEY, KEY_DOWN, 1))
    sh2, _ = recv(sink)
    check(sh2 is not None and sh2[0] == SHOW and sh2[1] != k,
          'KEY_DOWN -> redraw into the other buffer %s (%.1f ms)' % (sh2 and sh2[1], (time.monotonic() - t0) * 1e3))
    sink.send(msg(STATS, 144, 0, 0, 3210, 240000))
    sh3, _ = recv(sink)
    check(sh3 is not None and sh3[0] == SHOW, 'sink STATS -> menu redrawn')

    inp.send(msg(KEY, KEY_ESC, 1))
    a, _ = recv(sink)
    b, _ = recv(inp)
    check(a is not None and a[0] == HIDE and b is not None and b[0] == STATE and b[1] == 0, 'Esc -> HIDE + input released')

    inp.send(msg(TOGGLE))
    recv(inp), recv(sink)
    inp.send(msg(KEY, KEY_DOWN, 1))        # Back to the stream -> Quit session
    recv(sink)
    inp.send(msg(KEY, KEY_ENTER, 1))
    a, _ = recv(sink)
    b, _ = recv(inp)
    check(a is not None and a[0] == HIDE and b is not None and b[1] == 0, 'Quit session -> menu closes')
    inp.send(msg(TOGGLE))
    recv(inp), recv(sink)
    inp.send(msg(TOGGLE))
    a, _ = recv(sink)
    b, _ = recv(inp)
    check(a is not None and a[0] == HIDE and b[1] == 0, 'hotkey again closes the menu')

    # a sink without a usable overlay plane: the hotkey must not swallow the keyboard
    sink.close()
    time.sleep(0.1)
    sink2 = connect()
    sink2.send(msg(HELLO, 2, 3840, 2160, 60000))
    m, fds2 = recv(sink2)
    check(m is not None and m[0] == BUFFERS and (m[1], m[2]) == panel(3840, 2160), 'new sink 3840x2160 -> panel %dx%d' % (m[1], m[2]))
    sink2.send(msg(OVERLAY, 0, 19))
    time.sleep(0.05)
    inp.send(msg(TOGGLE))
    b, _ = recv(inp)
    a, _ = recv(sink2, 0.3)
    check(b is not None and b[1] == 0 and a is None, 'no overlay plane -> menu stays closed, input keeps forwarding')

    time.sleep(1.0)
    t = open('/proc/%d/stat' % cd.pid).read().split(')')[1].split()
    c0 = int(t[11]) + int(t[12])
    time.sleep(5.0)
    t = open('/proc/%d/stat' % cd.pid).read().split(')')[1].split()
    c1 = int(t[11]) + int(t[12])
    check(c1 - c0 <= 2, 'clientd idle: %d CPU ticks in 5 s' % (c1 - c0))
finally:
    cd.terminate()
    cd.wait(5)
    log.close()
print('test_menu_ipc: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
