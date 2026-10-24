#!/usr/bin/env python3
"""The game monitor by DRM lease, rdx-clientd's side, without a screen, an X server or RDMA: a real
rdx_clientd with stand-ins for rdx_lease (--probe; the lease run), rdx_sink_kms, rdx_sink_win,
rdx_input_tx and rdx_input_win, and a stand-in host on the control channel.
Checks: auto + two monitors + a leasing X server -> rdx_lease --output <the game monitor> with the KMS
sink on the lease fd (--drm-fd, --vrr), keyboard and mouse by rdx_input_tx with the desktop hotkey;
a chosen mode without a monitor -> the lease on the monitor the window opens on; a refused lease (exit 3) -> the full-screen window for that session; "game_monitor": "window" -> no
lease; Xwayland -> no lease under auto.

  tests/test_client_lease.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, subprocess, sys, threading, time, urllib.request

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-lease-test'
CTL, WEB, DATA = 47589, 47593, 39850
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


def script(path, body):
    with open(path, 'w') as f:
        f.write('#!/bin/sh\n' + body.replace('TMPDIR', TMP))
    os.chmod(path, 0o755)


def stand_ins():
    shutil.rmtree(TMP, ignore_errors=True)
    for d in ('repo/p0b', 'repo/p4'):
        os.makedirs(os.path.join(TMP, d))
    script(TMP + '/repo/p0b/rdx_sink_win', r'''
for a in "$@"; do [ "$a" = --list-modes-json ] && { echo '{"modes":[{"w":2560,"h":1440,"hz":144,"preferred":true,"monitor":"DP-1"},{"w":2560,"h":1440,"hz":60,"preferred":false,"monitor":"DP-2"}]}'; exit 0; }; done
echo "$@" > TMPDIR/sink_win.args
echo "[sink] window 0x4a00002 2560x1440, frames 2560x1440"
echo "[sink] display vrr=0 min=0.000 max=144.000 hz=144.000"
echo "[sink] listening on 127.0.0.1:39850"
exec sleep 30
''')
    script(TMP + '/repo/p0b/rdx_lease', r'''
[ "$1" = --probe ] && { cat TMPDIR/probe.json; exit 0; }
echo "$@" > TMPDIR/lease.args
[ -e TMPDIR/refuse ] && { echo "[lease] RRCreateLease refused"; exit 3; }
while [ "$1" != -- ]; do shift; done; shift
bin=$1; shift
args=""; for a in "$@"; do [ "$a" = @LEASEFD@ ] && a=9; args="$args $a"; done
exec "$bin" $args
''')
    script(TMP + '/repo/p0b/rdx_sink_kms', r'''
echo "$@" > TMPDIR/sink_kms.args
echo "[sink] display vrr=1 min=48.000 max=144.000 hz=144.000"
echo "[sink] listening on 127.0.0.1:39850"
exec sleep 30
''')
    script(TMP + '/repo/p4/rdx_input_tx', 'echo "$@" > TMPDIR/input_tx.args\nexec sleep 30\n')
    script(TMP + '/repo/p4/rdx_input_win', 'echo "$@" > TMPDIR/input_win.args\nexec sleep 30\n')


msgs = []


def host():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', CTL))
    ls.listen(4)
    ls.settimeout(20)
    try:
        c, _ = ls.accept()
    except OSError:
        return
    f = c.makefile('r')
    try:
        for line in f:
            m = json.loads(line)
            msgs.append(m)
            r = {'id': m.get('id'), 'ok': True}
            if m.get('cmd') == 'hello':
                r.update(host_name='stand-in', version=1, link_ip='127.0.0.1', outputs=[],
                         apps=[{'id': 'game', 'name': 'game', 'kind': 'gamescope'}], audio=True, usb=False, mic=False)
            c.sendall((json.dumps(r) + '\n').encode())
    except OSError:
        pass
    ls.close()


def run(game_monitor='auto', probe=None, refuse=False, mode=None):
    stand_ins()
    json.dump(probe or {'randr': '1.6', 'lease': True, 'xwayland': False}, open(TMP + '/probe.json', 'w'))
    if refuse:
        open(TMP + '/refuse', 'w').close()
    msgs.clear()
    th = threading.Thread(target=host, daemon=True)
    th.start()
    cfg = {'sink': 'window', 'link_ip': '127.0.0.1', 'hosts': [{'name': 'h', 'address': '127.0.0.1', 'port': CTL}],
           'active_host': 0, 'input': True, 'input_devices': ['/dev/null'], 'audio': False, 'mic': False, 'usb': [],
           'mode': mode, 'game_monitor': game_monitor}
    json.dump(cfg, open(TMP + '/client.json', 'w'))
    log = open(TMP + '/clientd.log', 'w')
    cd = subprocess.Popen([REPO + '/client/rdx_clientd', '--repo', TMP + '/repo', '--config', TMP + '/client.json',
                           '--web-port', str(WEB), '--data-port-base', str(DATA), '--no-menu',
                           '--web-root', REPO + '/web/client'], stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen('http://127.0.0.1:%d/api/state' % WEB, timeout=0.5).read()
                break
            except OSError:
                time.sleep(0.1)
        time.sleep(1.0)
        req = urllib.request.Request('http://127.0.0.1:%d/api/session/start' % WEB, data=b'{"app":"game"}', method='POST')
        urllib.request.urlopen(req, timeout=2).read()
        time.sleep(2.0)
        st = json.loads(urllib.request.urlopen('http://127.0.0.1:%d/api/state' % WEB, timeout=2).read())
        got = {}
        for k in ('lease', 'sink_kms', 'sink_win', 'input_tx', 'input_win'):
            p = '%s/%s.args' % (TMP, k)
            got[k] = open(p).read().split() if os.path.exists(p) else None
        return got, st
    finally:
        cd.terminate()
        cd.wait(5)
        th.join(1)


g, st = run()
check(g['lease'] and g['lease'][:4] == ['--output', 'DP-1', '--state', '/run/rdx/lease-%d.state' % WEB],
      'auto, two monitors, X11: rdx_lease --output DP-1 --state ...: %s' % g['lease'])
check(g['sink_kms'] and g['sink_kms'][:2] == ['--drm-fd', '9'] and '--vrr' in g['sink_kms'] and '--win' not in g['sink_kms'],
      'the KMS sink on the lease fd with --vrr, no window options: %s' % g['sink_kms'])
check(g['sink_win'] is None, 'no window sink')
it = g['input_tx'] or []
check('--desktop-hotkey' in it and 'ctrl+alt+h' in it and 'ctrl+alt+shift+z' in it and '/dev/null' in it,
      'keyboard and mouse by rdx_input_tx with the desktop hotkey: %s' % it)
check(st.get('game_monitor', {}).get('lease') is True, '/api/state game_monitor.lease while streaming')
start = [m for m in msgs if m.get('cmd') == 'session.start']
check(start and start[0].get('display', {}).get('vrr') is True, 'session.start display from the KMS sink on the lease')

g, st = run(mode={'w': 2560, 'h': 1440, 'hz': 144})
check(g['lease'] and g['lease'][:2] == ['--output', 'DP-1'] and g['sink_win'] is None,
      'a chosen mode without a monitor (the client page saves {w, h, hz}): the lease on the preferred monitor: %s' % g['lease'])

g, st = run(refuse=True)
check(g['lease'] is not None and g['sink_win'] is not None and g['sink_kms'] is None,
      'a refused lease (exit 3): the full-screen window instead: lease %s, window %s' % (bool(g['lease']), g['sink_win']))
check(g['input_win'] is not None and g['input_tx'] is None, 'the window input then: %s' % g['input_win'])
check(st.get('session', {}).get('state') in ('running', 'starting'), 'the session goes on: %s' % st.get('session', {}).get('state'))

g, st = run(game_monitor='window')
check(g['lease'] is None and g['sink_win'] is not None, '"game_monitor": "window": no lease')
g, st = run(probe={'randr': '1.6', 'lease': True, 'xwayland': True})
check(g['lease'] is None and g['sink_win'] is not None, 'auto under Xwayland: no lease (the window)')
g, st = run(game_monitor='lease', probe={'randr': '1.6', 'lease': True, 'xwayland': True})
check(g['lease'] is not None, '"lease" forced: tried also under Xwayland (a compositor with wp_drm_lease)')

print('test_client_lease: %s (%d failed)' % ('FAIL' if fails else 'PASS', len(fails)))
sys.exit(1 if fails else 0)
