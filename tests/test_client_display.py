#!/usr/bin/env python3
"""The client's "display" (FreeSync / VRR) without a screen or RDMA: a real rdx_clientd with a
stand-in window sink (prints "[sink] display ..." and "listening on") and a stand-in host on the
JSON-lines control channel. Checks: the sink gets --vrr (config default), session.start carries
"display" {"vrr", "min_hz", "max_hz", "hz"} from the sink's line, /api/state shows it, a later
change of the sink's line goes to the host as session.update {"display"}, "vrr": false drops --vrr.

  tests/test_client_display.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, stat, subprocess, sys, threading, time, urllib.request

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-display-test'
CTL, WEB, DATA = 47489, 47491, 39950
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


SINK = r'''#!/bin/sh
# stand-in window sink: argv to a file, the display line, listening, a change after 1.5 s
for a in "$@"; do [ "$a" = --list-modes-json ] && { echo '{"modes":[{"w":2560,"h":1440,"hz":144,"preferred":true,"monitor":"DP-1"}]}'; exit 0; }; done
echo "$@" > TMPDIR/sink.args
echo "[sink] window 0x4a00002 2560x1440, frames 2560x1440"
case " $* " in *" --vrr "*) echo "[sink] display vrr=1 min=48.000 max=144.000 hz=143.999";;
               *) echo "[sink] display vrr=0 min=0.000 max=143.999 hz=143.999";; esac
echo "[sink] listening on 127.0.0.1:39950"
sleep 1.5
echo "[sink] display vrr=0 min=0.000 max=143.999 hz=143.999"
exec sleep 30
'''

shutil.rmtree(TMP, ignore_errors=True)
os.makedirs(TMP + '/repo/p0b')
with open(TMP + '/repo/p0b/rdx_sink_win', 'w') as f:
    f.write(SINK.replace('TMPDIR', TMP))
os.chmod(TMP + '/repo/p0b/rdx_sink_win', 0o755)
msgs = []


def host():
    """the stand-in rdx-hostd: hello, session.start (ok), session.update (ok), every request kept"""
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
                         apps=[{'id': 'vkcube', 'name': 'vkcube', 'kind': 'gamescope'}], audio=True, usb=False, mic=False)
            c.sendall((json.dumps(r) + '\n').encode())
    except OSError:                                  # clientd stopped
        pass
    ls.close()


def run(vrr):
    msgs.clear()
    th = threading.Thread(target=host, daemon=True)
    th.start()
    cfg = {'sink': 'window', 'link_ip': '127.0.0.1', 'hosts': [{'name': 'h', 'address': '127.0.0.1', 'port': CTL}],
           'active_host': 0, 'input': False, 'audio': False, 'mic': False, 'usb': [], 'mode': None}
    if vrr is not None:
        cfg['vrr'] = vrr
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
        time.sleep(1.0)                                   # hello
        req = urllib.request.Request('http://127.0.0.1:%d/api/session/start' % WEB, data=b'{"app":"vkcube"}',
                                     method='POST')
        urllib.request.urlopen(req, timeout=2).read()
        time.sleep(1.2)
        st = json.loads(urllib.request.urlopen('http://127.0.0.1:%d/api/state' % WEB, timeout=2).read())
        time.sleep(2.5)                                   # the sink's change after 1.5 s, checked once a second
        args = open(TMP + '/sink.args').read().split()
        return args, st
    finally:
        cd.terminate()
        cd.wait(5)
        th.join(1)


args, st = run(None)
start = [m for m in msgs if m.get('cmd') == 'session.start']
check('--vrr' in args, 'config default: the sink gets --vrr')
check(len(start) == 1 and start[0].get('display') == {'vrr': True, 'min_hz': 48, 'max_hz': 144, 'hz': 143.999},
      'session.start "display" from the sink line: %s' % (start[0].get('display') if start else None))
check(st.get('display', {}) and st['display'].get('vrr') is True, '/api/state "display" while streaming: %s' % st.get('display'))
upd = [m for m in msgs if m.get('cmd') == 'session.update' and 'display' in m]
check(len(upd) == 1 and upd[0]['display'] == {'vrr': False, 'min_hz': 0, 'max_hz': 143.999, 'hz': 143.999},
      'a change of the sink line -> session.update {"display"}: %s' % [u.get('display') for u in upd])
args, st = run(False)
start = [m for m in msgs if m.get('cmd') == 'session.start']
check('--vrr' not in args, '"vrr": false: no --vrr for the sink')
check(start and start[0].get('display', {}).get('vrr') is False, '"vrr": false: session.start display vrr false')

print('test_client_display: %s (%d failed)' % ('FAIL' if fails else 'PASS', len(fails)))
sys.exit(1 if fails else 0)
