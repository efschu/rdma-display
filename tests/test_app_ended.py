#!/usr/bin/env python3
"""An app that ends right after its start (a single-instance app handing over to its desktop
instance, as Lutris does), without RDMA or a GPU: a real rdx_hostd on 127.0.0.1 with a stand-in
repo. The stand-in rdx_gs_src records its argv (the app behind "dbus-run-session --"), says
"[gs] stopping: gamescope ended" after 0.5 s and then keeps the sink like the real one until
SIGTERM. A process named like the app, outside rdx-hostd's cgroup, plays the desktop instance.

  tests/test_app_ended.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, subprocess, sys, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-app-ended-test'
CTL, WEB = 47489, 47490
fails = []

GS_SRC = r'''#!/usr/bin/env python3
import json, os, signal, sys, time
json.dump(sys.argv[1:], open(os.environ["RDX_TEST_DIR"] + "/gs_argv.json", "w"))
print("[gs] starting", flush=True)
time.sleep(0.5)
print("[gs] stopping: gamescope ended", flush=True)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
time.sleep(8)
'''


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


shutil.rmtree(TMP, ignore_errors=True)
for d in ('repo/gs', 'repo/p4'):
    os.makedirs(os.path.join(TMP, d))
subprocess.check_call(['cc', '-O2', '-o', TMP + '/repo/p4/rdx_input_rx', REPO + '/tests/dummy_mirror_src.c'])
open(TMP + '/repo/gs/rdx_gs_src', 'w').write(GS_SRC)
os.chmod(TMP + '/repo/gs/rdx_gs_src', 0o755)
# the "desktop instance": a process whose argv[0] is the app's program
shutil.copy('/bin/sleep', TMP + '/rdx-single-probe')
apps = [{'id': 'probe', 'name': 'Single Probe', 'kind': 'gamescope', 'cmd': ['rdx-single-probe', '--x']},
        {'id': 'probe-bus', 'name': 'Probe on the desktop bus', 'kind': 'gamescope', 'cmd': ['rdx-single-probe'],
         'private_dbus': False}]
json.dump({'link_ip': '127.0.0.1', 'ctl_port': CTL, 'web_port': WEB, 'desktop_user': 'root',
           'gamescope_bin': '/bin/true', 'apps': apps,
           'defaults': {'audio': False, 'usb': False, 'mic': False}}, open(TMP + '/hostd.json', 'w'))
env = dict(os.environ, RDX_TEST_DIR=TMP)
log = open(TMP + '/hostd.log', 'w')
hd = subprocess.Popen([REPO + '/host/rdx_hostd', '--repo', TMP + '/repo', '--config', TMP + '/hostd.json',
                       '--web-root', REPO + '/web/host', '--bind', '127.0.0.1', '--ctl-port', str(CTL),
                       '--web-port', str(WEB)], stdout=log, stderr=subprocess.STDOUT, env=env)
nid = [0]
events = []


def request(f, sock, cmd, **kw):
    nid[0] += 1
    kw.update(id=nid[0], cmd=cmd)
    sock.sendall((json.dumps(kw) + '\n').encode())
    while True:
        m = json.loads(f.readline())
        if m.get('id') == nid[0]:
            return m
        events.append(m)


def wait_event(f, sock, name, secs):
    """the first event `name` (already received or within secs)"""
    for e in events:
        if e.get('event') == name:
            events.remove(e)
            return e
    sock.settimeout(secs)
    try:
        while True:
            m = json.loads(f.readline())
            if m.get('event') == name:
                return m
            events.append(m)
    except (socket.timeout, ValueError):
        return None
    finally:
        sock.settimeout(None)


def start(f, s, app):
    events.clear()
    return request(f, s, 'session.start', app=app, fps=0, cursor=True, audio=False, input=True, usb=[], mic=False,
                   client={'link_ip': '127.0.0.1', 'mode': {'w': 1920, 'h': 1080, 'hz': 60}},
                   ports={'video': 18750, 'input': 18760, 'audio': 18770, 'usb': 18780, 'mic': 18790})


desk = None
try:
    for _ in range(50):
        try:
            s = socket.create_connection(('127.0.0.1', CTL), 0.2)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(None)
    f = s.makefile('r')
    request(f, s, 'hello', client_name='test', version=1)

    # 1. no desktop instance: the notice says the app ended right after its start; the host ends
    #    the session after showing it (6 s), never as "stopped by the client", never suspended
    r = start(f, s, 'probe')
    check(r['ok'], 'session.start probe ok (%s)' % r.get('error', ''))
    n = wait_event(f, s, 'session.notice', 5)
    check(n is not None and 'ended right after its start' in n.get('text', '') and n.get('already_open') is False,
          'notice without a desktop instance: %s' % (n or {}).get('text'))
    e = wait_event(f, s, 'session.ended', 9)
    check(e is not None and 'ended right after its start' in e.get('reason', ''),
          'the host ends the session with that reason after the notice: %s' % (e or {}).get('reason'))
    argv = json.load(open(TMP + '/gs_argv.json'))
    tail = argv[argv.index('--') + 1:] if '--' in argv else []
    dbus = shutil.which('dbus-run-session') == '/usr/bin/dbus-run-session' or os.access('/usr/bin/dbus-run-session', os.X_OK)
    check(tail == (['/usr/bin/dbus-run-session', '--'] if dbus else []) + ['rdx-single-probe', '--x'],
          'the app runs on its own session bus: %s' % tail)

    # 2. the app is open on the desktop: "already open", and the client's stop that follows is
    #    the consequence (stream gone), not a stop by the client: no suspend
    desk = subprocess.Popen([TMP + '/rdx-single-probe', '60'])
    time.sleep(0.2)
    r = start(f, s, 'probe-bus')
    check(r['ok'], 'session.start probe-bus ok (%s)' % r.get('error', ''))
    n = wait_event(f, s, 'session.notice', 5)
    check(n is not None and 'is already open on this PC' in n.get('text', '') and n.get('already_open') is True,
          'notice with the desktop instance: %s' % (n or {}).get('text'))
    request(f, s, 'session.stop')
    e = wait_event(f, s, 'session.ended', 3)
    check(e is not None and 'already open' in e.get('reason', ''), 'session.stop after it: ended with the app\'s reason: %s'
          % (e or {}).get('reason'))
    argv = json.load(open(TMP + '/gs_argv.json'))
    check(argv[argv.index('--') + 1:] == ['rdx-single-probe'], 'private_dbus false: the desktop bus (no wrapper)')
    st = request(f, s, 'status')
    check(not st.get('suspended'), 'nothing suspended (%s)' % st.get('suspended'))
    time.sleep(0.3)
    hl = open(TMP + '/hostd.log').read()
    check('stopped by the client' not in hl and 'suspended: keeps running' not in hl,
          'the log has no "stopped by the client" / suspend for an app that ended by itself')
    check('already open on the desktop: pid %d' % desk.pid in hl, 'the log names the desktop instance (pid %d)' % desk.pid)
    check(desk.poll() is None, 'the desktop instance was not touched')

    # 3. a real stop by the client while the app runs is still one (and suspends the app)
    GS_LONG = GS_SRC.replace('time.sleep(0.5)\nprint("[gs] stopping: gamescope ended", flush=True)\n', '')
    open(TMP + '/repo/gs/rdx_gs_src', 'w').write(GS_LONG)
    r = start(f, s, 'probe')
    time.sleep(0.5)
    request(f, s, 'session.stop')
    time.sleep(0.3)
    hl = open(TMP + '/hostd.log').read()
    check('session ends: stopped by the client' in hl and 'probe suspended' in hl,
          'a stop while the app runs: "stopped by the client", the app suspended')
finally:
    if desk:
        desk.kill()
    hd.terminate()
    hd.wait(5)
    log.close()
print('test_app_ended: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
