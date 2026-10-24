#!/usr/bin/env python3
"""Variable refresh (FreeSync / VRR) through rdx-hostd, without RDMA or a GPU: a real rdx_hostd on
127.0.0.1 with a stand-in repo. The stand-in rdx_gs_src (C, built here) records its argv, every signal
with its sigqueue value, and prints a "[gs] vrr:" line like the real one.

Checks: session.start with the client's "display" -> rdx_gs_src --vrr <cap>, the reply and /api/state
carry "vrr"; session.update {"display"} -> SIGRTMIN with the cap in mHz (0 = fixed); a resume writes
the cap as the third field of <socket>.peer; no "display", vrr false, tearing or the host's switch off
-> no --vrr at all (as before the feature).

  tests/test_hostd_vrr_session.py [repo-dir]      exit 0 = PASS
"""
import json, os, shutil, socket, subprocess, sys, time, urllib.request

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
TMP = '/tmp/rdx-vrr-session-test'
CTL, WEB = 47589, 47590
fails = []

GS_SRC = r'''
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/gs_argv.txt", getenv("RDX_TEST_DIR"));
    FILE *f = fopen(p, "a");
    for (int i = 1; i < argc; i++) fprintf(f, "%s%s", argv[i], i + 1 < argc ? "\x1f" : "\n");
    fclose(f);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1); sigaddset(&s, SIGUSR2); sigaddset(&s, SIGRTMIN); sigaddset(&s, SIGTERM);
    sigprocmask(SIG_BLOCK, &s, NULL);
    int fd = signalfd(-1, &s, 0);
    int vrr = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--vrr")) vrr = 1;
    printf("[gs] vrr: %s, stand-in\n", vrr ? "on" : "off");
    fflush(stdout);
    snprintf(p, sizeof(p), "%s/gs_sig.txt", getenv("RDX_TEST_DIR"));
    for (;;) {
        struct signalfd_siginfo si;
        if (read(fd, &si, sizeof(si)) != sizeof(si)) return 1;
        f = fopen(p, "a");
        fprintf(f, "%s %d\n", si.ssi_signo == SIGUSR1 ? "USR1" : si.ssi_signo == SIGUSR2 ? "USR2" :
                si.ssi_signo == (unsigned)SIGRTMIN ? "RTMIN" : "TERM", si.ssi_int);
        fclose(f);
        if (si.ssi_signo == SIGTERM) return 0;
        if (si.ssi_signo == (unsigned)SIGRTMIN) {
            printf("[gs] vrr: %s, stand-in %d\n", si.ssi_int ? "on" : "off", si.ssi_int);
            fflush(stdout);
        }
    }
}
'''


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


shutil.rmtree(TMP, ignore_errors=True)
for d in ('repo/gs', 'repo/p4'):
    os.makedirs(os.path.join(TMP, d))
subprocess.check_call(['cc', '-O2', '-o', TMP + '/repo/p4/rdx_input_rx', REPO + '/tests/dummy_mirror_src.c'])
open(TMP + '/gs_src.c', 'w').write(GS_SRC)
subprocess.check_call(['cc', '-O2', '-o', TMP + '/repo/gs/rdx_gs_src', TMP + '/gs_src.c'])
apps = [{'id': 'game', 'name': 'Game', 'kind': 'gamescope', 'cmd': ['/bin/sleep', '60'], 'private_dbus': False},
        {'id': 'game2', 'name': 'Game 2', 'kind': 'gamescope', 'cmd': ['/bin/sleep', '60'], 'private_dbus': False}]
cfg = {'link_ip': '127.0.0.1', 'ctl_port': CTL, 'web_port': WEB, 'desktop_user': 'root',
       'gamescope_bin': '/bin/true', 'apps': apps,
       'defaults': {'audio': False, 'usb': False, 'mic': False, 'vrr_cap_frac': 1.0}}   # exact caps; 0.97 at the end
json.dump(cfg, open(TMP + '/hostd.json', 'w'))
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


FS = {'vrr': True, 'min_hz': 48, 'max_hz': 240, 'hz': 240}


def start(f, s, app, display=None, tearing=False):
    kw = dict(app=app, fps=0, cursor=True, audio=False, input=True, usb=[], mic=False, tearing=tearing,
              client={'link_ip': '127.0.0.1', 'mode': {'w': 1920, 'h': 1080, 'hz': 240}},
              ports={'video': 18750, 'input': 18760, 'audio': 18770, 'usb': 18780, 'mic': 18790})
    if display is not None:
        kw['display'] = display
    return request(f, s, 'session.start', **kw)


def last_argv():
    lines = open(TMP + '/gs_argv.txt').read().splitlines()
    return lines[-1].split('\x1f'), len(lines)


def opt(argv, name):
    return argv[argv.index(name) + 1] if name in argv else None


def sigs():
    try:
        return open(TMP + '/gs_sig.txt').read().split('\n')[:-1]
    except FileNotFoundError:
        return []


def state():
    return json.load(urllib.request.urlopen('http://127.0.0.1:%d/api/state' % WEB, timeout=3))


def stop(f, s):
    request(f, s, 'session.stop')
    time.sleep(0.4)


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

    # 1. FreeSync 48-240 Hz: --vrr 240, reply and state say so, the source's line is shown
    r = start(f, s, 'game', FS)
    check(r.get('ok'), 'session.start with display ok (%s)' % r.get('error', ''))
    check((r.get('vrr') or {}).get('active') is True and (r.get('vrr') or {}).get('cap_hz') == 240,
          'the reply carries vrr: %s' % r.get('vrr'))
    time.sleep(0.8)
    argv, n0 = last_argv()
    check(opt(argv, '--vrr') == '240.000', 'rdx_gs_src --vrr 240.000 (%s)' % opt(argv, '--vrr'))
    sv = state()['session'].get('vrr') or {}
    check(sv.get('active') is True and sv.get('cap_hz') == 240 and (sv.get('client') or {}).get('min_hz') == 48,
          '/api/state session.vrr: %s' % {k: sv.get(k) for k in ('active', 'cap_hz', 'client')})
    check(sv.get('source') == 'on, stand-in', '/api/state session.vrr.source = the "[gs] vrr:" line (%s)' % sv.get('source'))
    check(state()['session']['params'].get('display') == FS, 'params.display is the client\'s display')

    # 2. session.update display: fullscreen off on the client (no VRR) -> SIGRTMIN 0; then 144 Hz -> 144000
    r = request(f, s, 'session.update', display={'vrr': False, 'min_hz': 0, 'max_hz': 240, 'hz': 240})
    check(r.get('ok') and (r.get('vrr') or {}).get('active') is False, 'session.update display vrr=false ok: %s' % r.get('vrr'))
    time.sleep(0.3)
    check(sigs()[-1:] == ['RTMIN 0'], 'rdx_gs_src got SIGRTMIN 0 (fixed refresh): %s' % sigs())
    r = request(f, s, 'session.update', display={'vrr': True, 'min_hz': 40, 'max_hz': 144, 'hz': 240})
    time.sleep(0.3)
    check(sigs()[-1:] == ['RTMIN 144000'], 'then 40-144 Hz: SIGRTMIN 144000: %s' % sigs())
    n_sig = len(sigs())
    r = request(f, s, 'session.update', display={'vrr': True, 'min_hz': 40, 'max_hz': 144, 'hz': 240})
    time.sleep(0.3)
    check(len(sigs()) == n_sig, 'the same display again: no signal')
    sv = state()['session'].get('vrr') or {}
    check(sv.get('cap_hz') == 144 and sv.get('source') == 'on, stand-in 144000', 'state follows: %s / %s'
          % (sv.get('cap_hz'), sv.get('source')))

    # 3. stop (suspend) and resume with another client: the cap is the third field of <socket>.peer
    stop(f, s)
    check(state().get('suspended') not in (None, {}), 'game suspended')
    r = start(f, s, 'game', {'vrr': True, 'min_hz': 48, 'max_hz': 165, 'hz': 165})
    time.sleep(0.4)
    peer = open('/run/rdx-gs-0-game.sock.peer').read().split()
    check(peer == ['127.0.0.1', '18750', '165.000'], '.peer for the resume: %s' % peer)
    check(sigs()[-1:] == ['USR1 0'] and last_argv()[1] == n0, 'resumed by SIGUSR1, no new rdx_gs_src')
    stop(f, s)
    r = start(f, s, 'game', None)              # an older client: resume at fixed refresh
    time.sleep(0.4)
    peer = open('/run/rdx-gs-0-game.sock.peer').read().split()
    check(peer == ['127.0.0.1', '18750', '0.000'], 'resume without display: third field 0 (%s)' % peer)
    stop(f, s)

    # 4. fixed refresh exactly as before: no display, vrr false, tearing, host switch off
    for what, disp, tear in (('no display', None, False), ('vrr false', dict(FS, vrr=False), False),
                             ('tearing', FS, True)):
        r = start(f, s, 'game2' if what != 'tearing' else 'game', disp, tear)
        time.sleep(0.6)
        argv, _ = last_argv()
        check(r.get('ok') and '--vrr' not in argv and (r.get('vrr') or {}).get('active') is False,
              '%s: no --vrr (%s)' % (what, (r.get('vrr') or {}).get('why')))
        stop(f, s)
        request(f, s, 'app.quit', app='game2' if what != 'tearing' else 'game')
        time.sleep(0.3)
    # the default fraction 0.97: 240 Hz -> --vrr 232.800
    req = urllib.request.Request('http://127.0.0.1:%d/api/config' % WEB, method='POST',
                                 data=json.dumps({'defaults': {'vrr_cap_frac': 0.97}}).encode(),
                                 headers={'Content-Type': 'application/json'})
    urllib.request.urlopen(req, timeout=3).read()
    r = start(f, s, 'game2', FS)
    time.sleep(0.6)
    argv, _ = last_argv()
    check(opt(argv, '--vrr') == '232.800', 'vrr_cap_frac 0.97: --vrr 232.800 (%s)' % opt(argv, '--vrr'))
    stop(f, s)
    request(f, s, 'app.quit', app='game2')
    time.sleep(0.3)
    req = urllib.request.Request('http://127.0.0.1:%d/api/config' % WEB, method='POST',
                                 data=json.dumps({'defaults': {'vrr': False}}).encode(),
                                 headers={'Content-Type': 'application/json'})
    urllib.request.urlopen(req, timeout=3).read()
    r = start(f, s, 'game2', FS)
    time.sleep(0.6)
    argv, _ = last_argv()
    check('--vrr' not in argv and 'off on this host' in (r.get('vrr') or {}).get('why', ''),
          'host switch off: no --vrr (%s)' % (r.get('vrr') or {}).get('why'))
    stop(f, s)
finally:
    hd.terminate()
    hd.wait(5)
    log.close()
print('test_hostd_vrr_session: %s (%d failure(s))' % ('PASS' if not fails else 'FAIL', len(fails)))
sys.exit(1 if fails else 0)
