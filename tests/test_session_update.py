#!/usr/bin/env python3
"""session.update (in-stream menu -> rdx-hostd -> rdx_mirror_src by sigqueue), without RDMA:
a real rdx_hostd on 127.0.0.1 with a stand-in repo (tests/dummy_mirror_src.c as mirror_src
and input_rx) and a JSON-lines control client.

  tests/test_session_update.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, subprocess, sys, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-update-test'
CTL, WEB = 47389, 47390
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


shutil.rmtree(TMP, ignore_errors=True)
for d in ('repo/p1', 'repo/p4'):
    os.makedirs(os.path.join(TMP, d))
dummy = TMP + '/repo/p1/rdx_mirror_src'
subprocess.check_call(['cc', '-O2', '-o', dummy, REPO + '/tests/dummy_mirror_src.c'])
shutil.copy(dummy, TMP + '/repo/p4/rdx_input_rx')
json.dump({'link_ip': '127.0.0.1', 'ctl_port': CTL, 'web_port': WEB}, open(TMP + '/hostd.json', 'w'))
env = dict(os.environ, RDX_DUMMY_LOG=TMP + '/signals.log')
log = open(TMP + '/hostd.log', 'w')
hd = subprocess.Popen([REPO + '/host/rdx_hostd', '--repo', TMP + '/repo', '--config', TMP + '/hostd.json',
                       '--web-root', REPO + '/web/host', '--bind', '127.0.0.1', '--ctl-port', str(CTL),
                       '--web-port', str(WEB)], stdout=log, stderr=subprocess.STDOUT, env=env)
nid = [0]


def request(f, sock, cmd, **kw):
    nid[0] += 1
    kw.update(id=nid[0], cmd=cmd)
    sock.sendall((json.dumps(kw) + '\n').encode())
    while True:
        m = json.loads(f.readline())
        if m.get('id') == nid[0]:
            return m


try:
    for _ in range(50):
        try:
            s = socket.create_connection(('127.0.0.1', CTL), 0.2)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(None)                         # the connect timeout above, not for the replies
    f = s.makefile('r')
    h = request(f, s, 'hello', client_name='test', version=1)
    check([o['name'] for o in h['outputs']] == ['DP-1', 'DP-2'], 'hello lists the stand-in outputs DP-1, DP-2')
    r = request(f, s, 'session.update', cursor=False)
    check(not r['ok'] and 'no running session' in r.get('error', ''), 'update without a session is refused')
    r = request(f, s, 'session.start', app='mirror:DP-1', fps=0, cursor=True, audio=False, input=True, usb=[],
                client={'link_ip': '127.0.0.1', 'mode': {'w': 1920, 'h': 1080, 'hz': 240}},
                ports={'video': 18750, 'input': 18760, 'audio': 18770, 'usb': 18780})
    check(r['ok'], 'session.start ok (%s)' % r.get('error', ''))
    r = request(f, s, 'session.update', cursor=False)
    check(not r['ok'] and 'still starting' in r.get('error', ''), 'update right after the start is refused (no signal before the handlers)')
    time.sleep(0.6)
    t0 = time.monotonic()
    r = request(f, s, 'session.update', cursor=False)
    t_upd = (time.monotonic() - t0) * 1e3
    check(r['ok'] and r['cursor'] is False and r['output'] == 'DP-1', 'cursor off -> ok (%.1f ms)' % t_upd)
    r = request(f, s, 'session.update', output='DP-2')
    check(r['ok'] and r['output'] == 'DP-2', 'output DP-2 -> ok')
    r = request(f, s, 'session.update', output='HDMI-9')
    check(not r['ok'] and 'unknown output' in r['error'], 'unknown output refused')
    r = request(f, s, 'session.update', cursor=True, output='DP-1')
    check(r['ok'] and r['cursor'] is True and r['output'] == 'DP-1', 'cursor on + DP-1 in one update')
    st = request(f, s, 'status')
    p = st['session']['params']
    check(st['session']['app'] == 'mirror:DP-1' and p['output'] == 'DP-1' and p['cursor'] is True,
          'status reflects the update (app %s)' % st['session']['app'])
    time.sleep(0.2)
    sig = open(TMP + '/signals.log').read().split('\n')[:-1] if os.path.exists(TMP + '/signals.log') else []
    check(sig == ['cursor 0', 'output 1', 'cursor 1', 'output 0'],
          'mirror_src got the signals with their values: %s' % sig)
    alive = [l for l in subprocess.run(['ps', '-C', 'rdx_mirror_src', '-o', 'args='], capture_output=True,
                                       text=True).stdout.split('\n') if TMP in l]
    check(len(alive) == 1, 'mirror_src still the same process (stream stays connected)')
    request(f, s, 'session.stop')
finally:
    hd.terminate()
    hd.wait(5)
    log.close()
print('test_session_update: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
