#!/usr/bin/env python3
"""Several apps in the background of one seat (rdx-hostd "suspend_multi"), without RDMA or a GPU: a real
rdx_hostd on 127.0.0.1 with a stand-in repo. The stand-in rdx_gs_src logs every SIGUSR2 (detach), SIGUSR1
(resume, with its <socket>.peer) and SIGTERM to a file of its own, runs a "game" below itself and one
re-parented away that only carries its RDX_GS_SOCKET, and ends by itself when a file asks it to. A fake
gamescope binary carries the marker of a gamescope that lets go of the input in the background; a fake
nvidia-smi reports the free memory the test sets and the memory of those processes.

Walk: A starts -> switch to B (A keeps running) -> resume A (B keeps running) -> quit B -> A ends by
itself in the background -> four apps: the fifth is refused -> little free memory: the start warns and
names the apps with their memory, nothing closed -> the same single-instance program under another
entry resumes the running one -> another tearing setting restarts only that app -> an older gamescope
(no marker): one app in the background as before -> shutdown closes them all.

  tests/test_multi_suspend.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, signal, socket, subprocess, sys, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-multi-suspend-test'
CTL, WEB = 47689, 47690
fails = []

GS_SRC = r'''#!/usr/bin/env python3
import os, signal, subprocess, sys, time
signal.pthread_sigmask(signal.SIG_BLOCK, (signal.SIGUSR1, signal.SIGUSR2, signal.SIGTERM))   # like the real one
T = os.environ["RDX_TEST_DIR"]
sock = sys.argv[sys.argv.index("--socket") + 1]
name = os.path.basename(sock)
log = open(T + "/gs-" + name + ".log", "a", buffering=1)
game = subprocess.Popen(["sleep", "1000"])                       # below rdx_gs_src
r, w = os.pipe()
if os.fork() == 0:                                              # re-parented: only its RDX_GS_SOCKET says whose
    os.setsid()
    if os.fork() == 0:
        os.close(r)
        os.write(w, b"%d" % os.getpid())
        os.execve("/bin/sleep", ["sleep", "1000"], dict(os.environ, RDX_GS_SOCKET=sock))
    os._exit(0)
os.close(w)
orphan = int(os.read(r, 32))
gpu = "%s/gpu-%d.pids" % (T, os.getpid())
open(gpu, "w").write("%d 100\n%d 1200\n%d 300\n" % (os.getpid(), game.pid, orphan))
def bye(code):
    for p in (game.pid, orphan):
        try: os.kill(p, signal.SIGKILL)
        except OSError: pass
    try: os.unlink(gpu)
    except OSError: pass
    sys.exit(code)
def on_sig(sig, _):
    if sig == signal.SIGUSR2:
        log.write("detach\n"); print("[gs] DETACHED (requested (SIGUSR2))", flush=True)
    elif sig == signal.SIGUSR1:
        log.write("resume %s\n" % open(sock + ".peer").read().strip()); print("[gs] RESUMED", flush=True)
    else:
        log.write("term\n"); bye(0)
for s in (signal.SIGUSR1, signal.SIGUSR2, signal.SIGTERM):
    signal.signal(s, on_sig)
log.write("start %d %s\n" % (os.getpid(), " ".join(sys.argv[1:])))
signal.pthread_sigmask(signal.SIG_UNBLOCK, (signal.SIGUSR1, signal.SIGUSR2, signal.SIGTERM))
print("[gs] starting", flush=True)
while not os.path.exists(T + "/end-" + name):
    time.sleep(0.05)
log.write("ended by itself\n")
bye(0)
'''

NVSMI = r'''#!/usr/bin/env python3
import glob, os, sys
T = os.environ["RDX_TEST_DIR"]
if any(a.startswith("--query-gpu") for a in sys.argv):
    print("%s, 20480" % open(T + "/vram_free").read().strip())
    sys.exit(0)
print("GPU 00000000:19:00.0\n    Processes")
for f in glob.glob(T + "/gpu-*.pids"):
    for line in open(f):
        pid, mib = line.split()
        print("        Process ID                        : %s\n            Type                          : C+G\n"
              "            Used GPU Memory               : %s MiB" % (pid, mib))
'''


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


shutil.rmtree(TMP, ignore_errors=True)
for d in ('repo/gs', 'repo/p4'):
    os.makedirs(os.path.join(TMP, d))
subprocess.check_call(['cc', '-O2', '-o', TMP + '/repo/p4/rdx_input_rx', REPO + '/tests/dummy_mirror_src.c'])
for path, text in ((TMP + '/repo/gs/rdx_gs_src', GS_SRC), (TMP + '/nvidia-smi', NVSMI),
                   (TMP + '/gamescope', '#!/bin/true\n# a gamescope with RDX_GS_INPUT v1\n')):
    open(path, 'w').write(text)
    os.chmod(path, 0o755)
open(TMP + '/vram_free', 'w').write('12000')
for f in os.listdir('/run'):
    if f.startswith('rdx-gs-0-'):
        os.unlink('/run/' + f)


def app(i, name, cmd, **kw):
    return dict(id=i, name=name, kind='gamescope', cmd=cmd, private_dbus=False, **kw)


apps = [app('steam', 'Steam', ['steam', '-silent']), app('bnet', 'Battle.net', ['battlenet']),
        app('steam-bp', 'Steam Big Picture', ['steam', '-gamepadui']), app('vk', 'vkcube', ['vkcube']),
        app('vrr', 'vrrtest', ['vrrtest']), app('xt', 'xterm', ['xterm'])]
json.dump({'link_ip': '127.0.0.1', 'ctl_port': CTL, 'web_port': WEB, 'desktop_user': 'root',
           'gamescope_bin': TMP + '/gamescope', 'gs_buffers': 'host', 'nvidia_smi': TMP + '/nvidia-smi', 'apps': apps,
           'defaults': {'audio': False, 'usb': False, 'mic': False}}, open(TMP + '/hostd.json', 'w'))
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


def start(app, tearing=False):
    """session.start; a fresh rdx_gs_src is waited for until its signal handlers are in place"""
    n = sum(l.startswith('start ') for l in gslog(app))
    r = request(f, s, 'session.start', app=app, fps=0, cursor=True, audio=False, input=True, usb=[], mic=False,
                tearing=tearing, client={'link_ip': '127.0.0.1', 'mode': {'w': 1920, 'h': 1080, 'hz': 60}},
                ports={'video': 18750, 'input': 18760, 'audio': 18770, 'usb': 18780, 'mic': 18790})
    if r.get('ok') and not r.get('resumed'):
        wait_for(lambda: sum(l.startswith('start ') for l in gslog(app)) > n, 5)
    return r


def stop():
    request(f, s, 'session.stop')


def bg():
    """the app ids in the background, oldest first"""
    return [a['app'] for a in request(f, s, 'status').get('suspended_apps', [])]


def gslog(app):
    try:
        return open('%s/gs-rdx-gs-0-%s.sock.log' % (TMP, app)).read().split('\n')
    except OSError:
        return []


def gspid(app):
    """the pid of the app's newest rdx_gs_src"""
    starts = [l for l in gslog(app) if l.startswith('start ')]
    return int(starts[-1].split()[1]) if starts else 0


def alive(pid):
    try:
        os.kill(pid, 0)
        return open('/proc/%d/stat' % pid).read().split(') ')[1][0] != 'Z'
    except OSError:
        return False


def wait_for(cond, secs=3.0):
    t = time.time() + secs
    while time.time() < t:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


left = []
try:
    for _ in range(50):
        try:
            s = socket.create_connection(('127.0.0.1', CTL), 0.2)
            break
        except OSError:
            time.sleep(0.1)
    s.settimeout(None)
    f = s.makefile('r')
    h = request(f, s, 'hello', client_name='test', version=1)
    check(h.get('suspend_multi') is True and h.get('apps_max') == 4 and h.get('suspended_apps') == [] and
          h.get('suspended') is None, 'hello: suspend_multi on (gamescope with the marker), apps_max 4, nothing paused')

    # 1. A starts, the client switches to B: A keeps running in the background
    r = start('steam')
    check(r['ok'] and not r.get('resumed') and 'vram_warning' not in r, 'A (steam) starts: fresh, no memory check alone')
    check(any(l.startswith('start ') and ' --buffers host ' in l for l in gslog('steam')),
          'config gs_buffers "host": rdx_gs_src gets --buffers host')
    stop()
    a_pid = gspid('steam')
    check(bg() == ['steam'] and 'detach' in gslog('steam'), 'stop: A in the background, its rdx_gs_src detached (SIGUSR2)')
    r = start('bnet')
    check(r['ok'] and not r.get('resumed'), 'switch to B (bnet): a fresh start')
    check(alive(a_pid) and 'term' not in gslog('steam'), 'A keeps running while B is streamed (no SIGTERM)')
    st = request(f, s, 'status')
    check([a['app'] for a in st['suspended_apps']] == ['steam'] and st['suspended']['app'] == 'steam' and
          st['suspended_apps'][0]['name'] == 'Steam', 'status during B: suspended_apps [A] with its name, "suspended" = A')
    socks = [l.split('--socket ')[1].split()[0] for l in gslog('steam') + gslog('bnet') if l.startswith('start ')]
    check(socks == ['/run/rdx-gs-0-steam.sock', '/run/rdx-gs-0-bnet.sock'], 'one rdx_gs_src socket per app: %s' % socks)
    stop()
    b_pid = gspid('bnet')
    check(bg() == ['steam', 'bnet'], 'stop B: both in the background, oldest first')

    # 2. resume A: its own rdx_gs_src gets the new peer, B keeps running
    r = start('steam')
    check(r['ok'] and r.get('resumed') is True and r.get('app') == 'steam', 'start A again: resumed')
    check(wait_for(lambda: any(l.startswith('resume 127.0.0.1 18750') for l in gslog('steam'))),
          'A\'s rdx_gs_src: SIGUSR1 with "127.0.0.1 18750 ..." in /run/rdx-gs-0-steam.sock.peer')
    check(gspid('steam') == a_pid and alive(b_pid) and bg() == ['bnet'],
          'the same rdx_gs_src (no second start), B still running, the list is [B]')

    # 3. quit B explicitly (app.quit) while A streams
    r = request(f, s, 'app.quit', app='bnet')
    check(r['ok'] and r.get('suspended_apps') == [] and wait_for(lambda: not alive(b_pid)) and 'term' in gslog('bnet'),
          'app.quit B: B ends (SIGTERM), A streams on')
    check(alive(a_pid), 'A untouched by quitting B')

    # 4. A ends by itself in the background: session_tick drops it
    stop()
    check(bg() == ['steam'], 'stop A: in the background again')
    open(TMP + '/end-rdx-gs-0-steam.sock', 'w').close()
    check(wait_for(lambda: bg() == [], 4), 'A ends by itself in the background: gone from the list')
    check('the suspended app steam ended by itself' in open(TMP + '/hostd.log').read(), 'the log says it ended by itself')
    os.unlink(TMP + '/end-rdx-gs-0-steam.sock')

    # 5. the limit: four apps on the seat, a fifth start is refused, nothing closed
    for a in ('steam', 'bnet', 'vk', 'vrr'):
        r = start(a)
        check(r['ok'], 'start %s (%d in the background)' % (a, len(bg())))
        stop()
    four = {a: gspid(a) for a in ('steam', 'bnet', 'vk', 'vrr')}
    check(bg() == ['steam', 'bnet', 'vk', 'vrr'], 'four apps in the background')
    r = start('xt')
    check(not r['ok'] and '4 apps run on this seat already' in r.get('error', '') and 'Steam' in r.get('error', '') and
          len(r.get('suspended_apps', [])) == 4, 'the fifth app is refused with the list: %s' % r.get('error'))
    check(all(alive(p) for p in four.values()) and not gslog('xt'), 'nothing closed for it, it was never started')

    # 6. little free memory: the start warns, names the apps with their memory, closes nothing
    request(f, s, 'app.quit', app='vrr')
    open(TMP + '/vram_free', 'w').write('900')
    r = start('xt')
    w = r.get('vram_warning') or {}
    names = {a['app']: a['vram_mib'] for a in w.get('apps', [])}
    check(r['ok'] and w.get('free_mib') == 900 and w.get('threshold_mib') == 2048,
          'start with 900 MiB free: started, vram_warning {free 900, threshold 2048}')
    check(names == {'steam': 1600, 'bnet': 1600, 'vk': 1600},
          'the warning names the apps with their memory (rdx_gs_src + game below it + re-parented by RDX_GS_SOCKET): %s' % names)
    check('Steam (1600 MiB)' in w.get('text', ''), 'and a line for the client: %s' % w.get('text'))
    check(all(alive(four[a]) for a in ('steam', 'bnet', 'vk')), 'nothing was closed for the warning')
    st = request(f, s, 'status')
    check(st['suspended_apps'][0].get('vram_mib') == 1600, 'the list carries the measured memory (vram_mib)')
    stop()
    open(TMP + '/vram_free', 'w').write('12000')

    # 7. Steam is single-instance: its other entry resumes the running one, never a second start
    n_steam = sum(l.startswith('start ') for l in gslog('steam'))
    r = start('steam-bp')
    check(r['ok'] and r.get('resumed') is True and r.get('resumed_as') == 'steam' and r.get('app') == 'steam',
          'start "Steam Big Picture" while Steam runs: resumed_as steam')
    check(sum(l.startswith('start ') for l in gslog('steam')) == n_steam and not gslog('steam-bp'),
          'no second Steam started')
    check(wait_for(lambda: sum(l.startswith('resume ') for l in gslog('steam')) >= 2), 'Steam\'s rdx_gs_src resumed (SIGUSR1)')
    stop()

    # 8. another tearing setting: only that app restarts, the others keep running
    bnet_old = gspid('bnet')
    r = start('bnet', tearing=True)
    check(r['ok'] and not r.get('resumed') and wait_for(lambda: not alive(bnet_old)) and gspid('bnet') != bnet_old,
          'B with tearing: B closed and started fresh')
    check(alive(four['steam']) and alive(four['vk']) and alive(gspid('xt')), 'the others keep running')
    check('--tearing' in [l for l in gslog('bnet') if l.startswith('start ')][-1], 'the fresh B runs with --tearing')
    stop()

    # 9. an older gamescope (no marker): one app in the background, another start closes it
    open(TMP + '/gamescope.new', 'w').write('#!/bin/true\n# older\n')
    os.chmod(TMP + '/gamescope.new', 0o755)
    os.rename(TMP + '/gamescope.new', TMP + '/gamescope')
    st = request(f, s, 'status')
    check(st.get('suspend_multi') is False and st.get('apps_max') == 1, 'older gamescope: suspend_multi false, apps_max 1')
    before = {a: gspid(a) for a in bg()}
    r = start('vrr')
    check(r['ok'] and wait_for(lambda: not any(alive(p) for p in before.values())),
          'a new app with an older gamescope closes the ones in the background (%s)' % sorted(before))
    stop()
    check(bg() == ['vrr'], 'then one app in the background')
    open(TMP + '/gamescope.new', 'w').write('#!/bin/true\n# RDX_GS_INPUT v1\n')
    os.chmod(TMP + '/gamescope.new', 0o755)
    os.rename(TMP + '/gamescope.new', TMP + '/gamescope')

    # 10. suspend_multi off by the user: the same
    r = start('vk')
    stop()
    check(bg() == ['vrr', 'vk'], 'the new gamescope again: two in the background')
    hj = subprocess.run(['curl', '-s', '-X', 'POST', '-d', '{"defaults": {"suspend_multi": false}}',
                         'http://127.0.0.1:%d/api/config' % WEB], capture_output=True, text=True)
    before = {a: gspid(a) for a in bg()}
    r = start('xt')
    check(r['ok'] and wait_for(lambda: not any(alive(p) for p in before.values())) and request(f, s, 'status')
          .get('suspended_apps') == [], '"suspend_multi": false: the start of another app closes the background ones')
    stop()
    subprocess.run(['curl', '-s', '-X', 'POST', '-d', '{"defaults": {"suspend_multi": true}}',
                    'http://127.0.0.1:%d/api/config' % WEB], capture_output=True, text=True)
    r = start('vk')
    stop()
    js = json.loads(subprocess.run(['curl', '-s', 'http://127.0.0.1:%d/api/state' % WEB], capture_output=True,
                                   text=True).stdout)
    check([a['app'] for a in js.get('suspended_apps', [])] == ['xt', 'vk'] and js['suspended']['app'] == 'vk',
          '/api/state: suspended_apps [xt, vk], "suspended" the newest (vk)')
    hq = json.loads(subprocess.run(['curl', '-s', '-X', 'POST', '-d', '{}', 'http://127.0.0.1:%d/api/apps/quit' % WEB],
                                   capture_output=True, text=True).stdout)
    check(hq.get('ok') and [a['app'] for a in hq.get('suspended_apps', [])] == ['xt'],
          'POST /api/apps/quit without an app: the newest one only')
    left = [gspid('xt')]
finally:
    hd.terminate()
    hd.wait(10)
    log.close()
time.sleep(0.2)
check(left and all(not alive(p) for p in left) and 'term' in gslog('xt'), 'shutdown: the apps in the background are closed')
check('quitting the suspended app xt (rdx-hostd shutting down)' in open(TMP + '/hostd.log').read(),
      'the log names it')
print('test_multi_suspend: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
