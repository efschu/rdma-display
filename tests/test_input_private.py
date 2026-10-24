#!/usr/bin/env python3
"""The client's input reaches a gamescope app only, a mirrored desktop gets it as ordinary devices
(by design: the client's input must not reach the whole host desktop, only the streamed game).
Without RDMA or a GPU: a real rdx_hostd on 127.0.0.1 with a stand-in repo whose
rdx_input_rx, rdx_usb_host, rdx_gs_src and rdx_mirror_src record their argv.

  gamescope app  -> rdx_input_rx --private --wait-grab, rdx_usb_host --private --wait-grab
  mirror:<out>   -> neither gets --private or --wait-grab (a desktop does not grab its devices)
The devices themselves (phys "rdx:private", seat "seat-rdx", group rdx-input, xinput on the desktop,
gamescope reading them) are checked on a desktop machine (README "What happens by default",
desktop/72-rdma-display-input.rules).

  tests/test_input_private.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, subprocess, sys, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-input-private-test'
CTL, WEB = 47589, 47590
fails = []

# a stand-in that records its argv as <name>.json and stays until SIGTERM; mirror_src also answers
# --list-json (refresh_outputs), input_rx / usb_host print the line rdx-hostd waits for
STANDIN = r'''#!/usr/bin/env python3
import json, os, signal, sys, time
name = os.path.basename(sys.argv[0])
if '--list-json' in sys.argv:
    print(json.dumps({"outputs": [{"name": "DP-1", "w": 1920, "h": 1080, "hz": 60, "x": 0, "y": 0}]}))
    sys.exit(0)
json.dump(sys.argv[1:], open(os.environ["RDX_TEST_DIR"] + "/" + name + ".json", "w"))
print({"rdx_input_rx": "[rx] listening on 127.0.0.1:0", "rdx_usb_host": "[usb-host] listening on 127.0.0.1:0"}
      .get(name, "[%s] running" % name), flush=True)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
time.sleep(30)
'''


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


shutil.rmtree(TMP, ignore_errors=True)
for d, name in (('p4', 'rdx_input_rx'), ('usb', 'rdx_usb_host'), ('gs', 'rdx_gs_src'), ('p1', 'rdx_mirror_src')):
    os.makedirs(os.path.join(TMP, 'repo', d), exist_ok=True)
    p = os.path.join(TMP, 'repo', d, name)
    open(p, 'w').write(STANDIN)
    os.chmod(p, 0o755)
apps = [{'id': 'game', 'name': 'Game', 'kind': 'gamescope', 'cmd': ['true'], 'private_dbus': False}]
json.dump({'link_ip': '127.0.0.1', 'ctl_port': CTL, 'web_port': WEB, 'desktop_user': 'root',
           'gamescope_bin': '/bin/true', 'apps': apps,
           'defaults': {'audio': False, 'usb': True, 'mic': False}}, open(TMP + '/hostd.json', 'w'))
env = dict(os.environ, RDX_TEST_DIR=TMP)
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


def argv_of(name, secs=5):
    p = '%s/%s.json' % (TMP, name)
    for _ in range(int(secs * 20)):
        if os.path.exists(p):
            time.sleep(0.05)
            return json.load(open(p))
        time.sleep(0.05)
    return None


def session(f, s, app):
    for n in ('rdx_input_rx', 'rdx_usb_host'):
        if os.path.exists('%s/%s.json' % (TMP, n)):
            os.unlink('%s/%s.json' % (TMP, n))
    r = request(f, s, 'session.start', app=app, fps=0, cursor=True, audio=False, input=True,
                usb=[{'busid': '1-1', 'name': 'Test pad', 'kind': 'hid'}], mic=False,
                client={'link_ip': '127.0.0.1', 'mode': {'w': 1920, 'h': 1080, 'hz': 60}},
                ports={'video': 18750, 'input': 18760, 'audio': 18770, 'usb': 18780, 'mic': 18790})
    rx, usb = argv_of('rdx_input_rx'), argv_of('rdx_usb_host')
    request(f, s, 'session.stop')
    time.sleep(0.5)
    return r, rx, usb


try:
    for _ in range(50):
        try:
            s = socket.create_connection(('127.0.0.1', CTL), 0.2)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(10)
    f = s.makefile('r')
    request(f, s, 'hello', client_name='test', version=1)

    r, rx, usb = session(f, s, 'game')
    check(r.get('ok'), 'session.start game ok (%s)' % r.get('error', ''))
    check(rx is not None and '--private' in rx, 'gamescope app: rdx_input_rx --private (%s)' % rx)
    check(usb is not None and '--private' in usb, 'gamescope app: rdx_usb_host --private (%s)' % usb)
    check(rx is not None and '--wait-grab' in rx and usb is not None and '--wait-grab' in usb,
          'gamescope app: both --wait-grab (gamescope grabs, keys wait for it)')

    r, rx, usb = session(f, s, 'mirror:DP-1')
    check(r.get('ok'), 'session.start mirror:DP-1 ok (%s)' % r.get('error', ''))
    check(rx is not None and '--private' not in rx, 'mirrored desktop: rdx_input_rx without --private (%s)' % rx)
    check(usb is not None and '--private' not in usb, 'mirrored desktop: rdx_usb_host without --private (%s)' % usb)
    check(rx is not None and '--wait-grab' not in rx and usb is not None and '--wait-grab' not in usb,
          'mirrored desktop: no --wait-grab (the desktop does not grab)')
finally:
    hd.terminate()
    hd.wait(5)
    log.close()
print('test_input_private: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
