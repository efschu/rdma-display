#!/usr/bin/env python3
"""rdx_audio_tx --capture keeps capturing when its sink goes away (observed: the default sink
"auto_null" was removed by WirePlumber, rdx_audio_tx logged "target not found" and stayed silent
until the session ended).

A private PipeWire + WirePlumber + D-Bus session in a temporary XDG_RUNTIME_DIR (no sound card
needed, nothing of the machine's own audio is touched) and tests/test_audio_follow.c, which compiles
the capture stream of p5/rdx_audio_tx.c as it is, without the RDMA part. Per scenario the harness
prints "frames N restarts R" every 100 ms; the test checks that frames keep growing right after
the sink goes away (at most 1 s without a new frame), that the stream sits on the monitor of the
other sink and that there is no crash.

  tests/test_audio_follow.py [repo-dir]      exit 0 = PASS, 77 = skipped (no pipewire/wireplumber/dbus)
"""
import json, os, shutil, signal, subprocess, sys, tempfile, threading, time

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what, flush=True)
    if not cond:
        fails.append(what)


for tool in ('pipewire', 'wireplumber', 'dbus-daemon', 'pw-cli', 'pw-dump', 'pw-metadata'):
    if not shutil.which(tool):
        print('SKIP %s not installed' % tool)
        sys.exit(77)
cf = subprocess.run(['pkg-config', '--cflags', '--libs', 'libpipewire-0.3', 'samplerate'], capture_output=True, text=True)
if cf.returncode:
    print('SKIP libpipewire/samplerate development files missing')
    sys.exit(77)

TMP = tempfile.mkdtemp(prefix='rdx-follow-')
os.chmod(TMP, 0o700)
env = dict(os.environ, XDG_RUNTIME_DIR=TMP, DBUS_SESSION_BUS_ADDRESS='unix:path=' + TMP + '/bus')
env.pop('PIPEWIRE_REMOTE', None)
procs = []


def spawn(argv, name):
    p = subprocess.Popen(argv, env=env, stdout=open(TMP + '/' + name + '.log', 'w'), stderr=subprocess.STDOUT)
    procs.append(p)
    return p


def pw_dump():
    return json.loads(subprocess.run(['pw-dump'], env=env, capture_output=True, text=True, timeout=10).stdout)


def node_id(name):
    for o in pw_dump():
        if o.get('type', '').endswith('Node') and o.get('info', {}).get('props', {}).get('node.name') == name:
            return o['id']
    return None


def make_sink(name):
    subprocess.run(['pw-cli', 'create-node', 'adapter',
                    '{ factory.name=support.null-audio-sink node.name=%s media.class=Audio/Sink '
                    'object.linger=true audio.position=[FL,FR] }' % name], env=env, capture_output=True, timeout=10)
    for _ in range(50):
        if node_id(name) is not None:
            return
        time.sleep(0.1)
    raise RuntimeError('sink %s did not appear' % name)


def drop_sink(name):
    subprocess.run(['pw-cli', 'destroy', str(node_id(name))], env=env, capture_output=True, timeout=10)


def set_default(name):
    subprocess.run(['pw-metadata', '-n', 'default', '0', 'default.configured.audio.sink',
                    json.dumps({'name': name}), 'Spa:String:JSON'], env=env, capture_output=True, timeout=10)


def default_sink():
    out = subprocess.run(['pw-metadata', '-n', 'default', '0', 'default.audio.sink'], env=env, capture_output=True, text=True).stdout
    return out


def capture_source_node():
    """name of the node whose monitor feeds 'rdx-audio-tx' (None if unlinked)"""
    objs = pw_dump()
    me = [o['id'] for o in objs if o.get('info', {}).get('props', {}).get('node.name') == 'rdx-audio-tx']
    if not me:
        return None
    ports = {o['id']: o['info']['props'] for o in objs if o.get('type', '').endswith('Port')}
    names = {o['id']: o['info']['props'].get('node.name') for o in objs if o.get('type', '').endswith('Node')}
    for o in objs:
        if o.get('type', '').endswith('Link') and o['info'].get('input-node-id') in me:
            return names.get(o['info'].get('output-node-id'))
    return None


class Harness:
    def __init__(self, target):
        self.p = subprocess.Popen([TMP + '/taf', target], env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        self.samples = []            # (monotonic time, frames, restarts)
        self.out = []
        self.err = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: [self.err.append(l) for l in self.p.stderr], daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            f = line.split()
            if len(f) == 4 and f[0] == 'frames':
                self.samples.append((time.monotonic(), int(f[1]), int(f[3])))
            else:
                self.out.append(line)

    def frames(self):
        return self.samples[-1][1] if self.samples else 0

    def restarts(self):
        return self.samples[-1][2] if self.samples else 0

    def max_gap(self, t0):
        """longest time since t0 in which the frame counter did not grow"""
        s = [x for x in self.samples if x[0] >= t0]
        gap, last_t, last_f = 0.0, t0, None
        for t, f, _ in s:
            if last_f is None:
                last_f = f
            if f > last_f:
                gap, last_t, last_f = max(gap, t - last_t), t, f
        return max(gap, (s[-1][0] if s else t0) - last_t)

    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        try:
            return self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            return -9


def wait_for(cond, timeout=5.0):
    t = time.monotonic() + timeout
    while time.monotonic() < t:
        if cond():
            return True
        time.sleep(0.05)
    return False


try:
    cc = subprocess.run(['cc', '-O2', '-g', '-Wall', '-Wextra', '-std=gnu11', '-D_REENTRANT', '-o', TMP + '/taf',
                         REPO + '/tests/test_audio_follow.c', REPO + '/common/rdx_common.c', '-libverbs']
                        + cf.stdout.split() + ['-lm', '-lpthread'], capture_output=True, text=True)
    if cc.returncode:
        print(cc.stderr)
        sys.exit(1)
    spawn(['dbus-daemon', '--session', '--nofork', '--address=unix:path=' + TMP + '/bus'], 'dbus')
    time.sleep(0.5)
    spawn(['pipewire'], 'pipewire')
    time.sleep(1.0)
    spawn(['wireplumber'], 'wireplumber')
    check(wait_for(lambda: 'default' in subprocess.run(['pw-metadata', '-n', 'default'], env=env, capture_output=True, text=True).stdout, 10),
          'private PipeWire + WirePlumber session is up')

    # --- 1. follow the default sink: A (default) goes away, B is there ---
    make_sink('sinkA')
    set_default('sinkA')
    time.sleep(0.5)
    h = Harness('default')
    check(wait_for(lambda: h.frames() > 4800, 8), 'capture on the default sink delivers frames')
    check(wait_for(lambda: capture_source_node() == 'sinkA', 5), 'stream sits on the monitor of sinkA (got %s)' % capture_source_node())
    make_sink('sinkB')
    set_default('sinkB')
    f0 = h.frames()
    t0 = time.monotonic()
    time.sleep(0.5)
    check(wait_for(lambda: capture_source_node() == 'sinkB', 3), 'default changed to sinkB: stream moved to its monitor (got %s)' % capture_source_node())
    check(h.frames() > f0 + 4800, 'frames keep flowing after the default changed')
    drop_sink('sinkA')
    f1 = h.frames()
    t1 = time.monotonic()
    time.sleep(2.0)
    check(h.frames() > f1 + 48000, 'frames keep flowing after the old sink is gone (%d frames in 2 s)' % (h.frames() - f1))
    check(h.max_gap(t1) <= 1.0, 'no gap longer than 1 s after the old sink is gone (%.2f s)' % h.max_gap(t1))
    check(capture_source_node() == 'sinkB', 'stream still on sinkB (got %s)' % capture_source_node())
    check(h.p.poll() is None, 'harness still running')
    print('     restarts after scenario 1: %d' % h.restarts())

    # --- 2. the sink vanishes while it is the only sink and the default; a new sink appears later ---
    drop_sink('sinkB')
    t2 = time.monotonic()
    time.sleep(1.0)
    make_sink('sinkC')
    set_default('sinkC')
    check(wait_for(lambda: capture_source_node() == 'sinkC', 3), 'no sink at all for 1 s, then sinkC: stream is on it (got %s)' % capture_source_node())
    f2 = h.frames()
    time.sleep(1.0)
    check(h.frames() > f2 + 24000, 'frames flow again on sinkC (%d frames in 1 s)' % (h.frames() - f2))
    check(h.stop() == 0, 'clean exit on SIGTERM')

    # --- 3. a named target that vanishes: falls back to the default sink ---
    make_sink('sinkD')
    h = Harness('sinkD')
    check(wait_for(lambda: capture_source_node() == 'sinkD', 8), 'named capture sits on sinkD (got %s)' % capture_source_node())
    f3 = h.frames()
    t3 = time.monotonic()
    drop_sink('sinkD')
    time.sleep(2.0)
    check(wait_for(lambda: capture_source_node() == 'sinkC', 3), 'sinkD gone: stream fell back to the default sink sinkC (got %s)' % capture_source_node())
    check(h.frames() > f3 + 48000, 'frames keep flowing after the named sink is gone (%d frames in 2 s)' % (h.frames() - f3))
    check(h.max_gap(t3) <= 1.0, 'no gap longer than 1 s after the named sink is gone (%.2f s)' % h.max_gap(t3))
    check(h.stop() == 0, 'clean exit on SIGTERM')
    print('     harness log of scenario 3:\n       ' + '       '.join(h.out + h.err))

    # --- 4. safety net: the stream ends in an error (its node is destroyed from outside) -> rebuilt ---
    h = Harness('default')
    check(wait_for(lambda: capture_source_node() == 'sinkC', 8), 'default capture sits on sinkC (got %s)' % capture_source_node())
    f4 = h.frames()
    subprocess.run(['pw-cli', 'destroy', str(node_id('rdx-audio-tx'))], env=env, capture_output=True, timeout=10)
    t4 = time.monotonic()
    check(wait_for(lambda: h.restarts() >= 1, 2), 'the stream was rebuilt within 2 s (restarts %d)' % h.restarts())
    time.sleep(1.5)
    check(h.frames() > f4 + 48000, 'frames flow again after the rebuild (%d frames)' % (h.frames() - f4))
    check(capture_source_node() == 'sinkC', 'rebuilt stream is on sinkC (got %s)' % capture_source_node())
    check(h.stop() == 0, 'clean exit on SIGTERM')
    print('     harness log of scenario 4:\n       ' + '       '.join(h.out + h.err))
finally:
    for p in reversed(procs):
        p.terminate()
    for p in procs:
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            p.kill()
    if fails:
        for f in os.listdir(TMP):
            if f.endswith('.log'):
                print('--- %s' % f)
                print(open(TMP + '/' + f).read()[-1500:])
    shutil.rmtree(TMP, ignore_errors=True)

print('PASS' if not fails else 'FAILED: %d' % len(fails))
sys.exit(1 if fails else 0)
