#!/usr/bin/env python3
"""Performance and end-to-end latency of a stream, as a table: one run per cell of a matrix
source x sink x mode x load, each run a real stream (rdx_gs_src + gamescope + the app on the source,
a sink on the client), all numbers from the programs' own reports.

    tools/rdx_perf.py --source seat2 --sink null@seat2-sink \\
        [--modes 1920x1080@240,2560x1440@144,3840x2160@60] [--loads vkcube,vkmark,glmark2,flash]
        [--latency] [--seconds 20] [--repeat 1] [--slots auto|N] [--config FILE] [--bin NAME=PATH]
        [--cursor auto|picture] [--out DIR] [--json FILE] [--dry-run]

Run it on a computer that reaches every host of the profile by ssh (BatchMode, keys already set up).
The source's GPU must be free for the run and, for a seat, its container running with "picture": "stream".

Sources: a seat container on a Proxmox multiseat host ("seat1", "seat2", reached as
`ssh pve-host pct exec CT --`) or a desktop PC ("host"); sinks: "null@HOST" (gs/rdx_sink_null, no
display), "win@HOST" (p0b/rdx_sink_win, a window on the client's desktop), "kms@HOST" (p0b/rdx_sink_kms,
takes the client's screen: only when it is free). "null@seat2-sink" is internal to the multiseat host:
the sink in the seat's own container, RoCE loopback in its NIC, no wire and no other computer. Hosts,
link IPs, GPUs, binaries: the built-in EXAMPLE profile below (PROFILE, neutral names: replace them), or
--config FILE (JSON with the same keys, merged over it; see docs/PERF.md) and --bin NAME=PATH. Test
processes on a host with "wrap" (the example "host": flock on a shared test lock, a 2 GB scope, nice 19,
a hard timeout) run inside that wrap. A sink on a computer whose NIC also carries someone's stream
disturbs that stream (shared NIC, PCIe and memory bandwidth): run only when the client is free.

Loads: vkcube (Vulkan, FIFO: the app's rate is the composite rate), vkmark (Vulkan benchmark, immediate:
full GPU load next to the compositor; the app's fps is vkmark's own), glmark2 (GL benchmark, terrain),
flash / flash-novsync / flash-idle (tools/rdx_flash: black/white on a key, the latency probe: a game
loop with vsync, with vsync off, an app that draws only on input; --latency injects keys), cursor /
cursor30 (tools/rdx_flash with a visible cursor: an app that never draws, and one limited to 30 fps; the
mouse moves to and fro all the time, tools/rdx_keyinject.py --mouse, so the table shows what pointer
motion costs the source: gs fps stays at the app's rate when the cursor travels next to the picture).
--cursor: the sink's --cursor (auto: it shows the cursor itself where it can; picture: the source draws
it into the frames as for an older sink), so one run of each is the before / after of the cursor channel.

--latency (the flash loads): on the client tools/rdx_keyinject.py creates a uinput keyboard and
p4/rdx_input_tx sends it to p4/rdx_input_rx on the source (the real input path: rdx_input_tx reads it
through evdev like a physical keyboard); in gamescope tools/rdx_flash toggles black <-> white on every
press; rdx_sink_null --watch reports the first frame of the new colour at its arrival (NIC CQE clock) and
at the vblank that shows it. Key and frame times are on the client's CLOCK_MONOTONIC, so no clock
sync between the computers is needed. Reported: key -> arrival and key -> vblank (the frame on screen;
the beam reaches the watched pixel at the frame's centre half a period later).
--latency with the cursor loads: every mouse motion -> the first update of the pointer the sink sees, a
changed picture (rdx_sink_null --watch-cursor: a sampled hash of each frame) or a position on the cursor
channel, at its arrival and at the vblank that shows it (a cursor plane is latched there).

Columns:
  app fps     the app's own rate (vkmark / glmark2 / rdx_flash; vkcube = FIFO = gs fps)
  gs fps      frames gamescope composited and rdx_gs_src posted, per second
  shown/s     frames the sink showed per second;  drops = frames overtaken before their vblank
  slots       sink slots;  arr->vbl  sink arrival -> vblank p50 / p99 (NIC clock when available; for the
              window sink, which sends no "shown" reports: arrival -> drawn by its GPU)
  comp        rdx_gs_src: gamescope's wake -> its composite + copy into the slot done on the GPU, p50
  post->shown rdx_gs_src: RDMA post -> the sink's "shown" report arrives, p50 (transfer + wait + report)
  lead ms     how far before the sink's vblank gamescope is woken (the closed loop's settled value)
  NIC         bytes the client's RDMA port received (port_rcv_data) / payload (frames x frame bytes)
"""
import argparse
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time

SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5']
PVE_SSH = SSH + ['pve-host']          # the Proxmox multiseat host (an ssh alias)
HOST_WRAP = 'flock /tmp/rdx-testlock systemd-run --scope -q -p MemoryMax=2G -p MemoryHigh=1500M nice -n 19'

# EXAMPLE profile (replace the names and addresses by yours, or merge a --config FILE over it):
# Proxmox seats 3101 (RTX 5090) / 3102 (RTX 3080) -> "host" (40G), "host" (a desktop PC) -> "client" (100G)
PROFILE = {
    'hosts': {
        'host': {'ssh': SSH + ['root@host.example'], 'wrap': HOST_WRAP, 'max_s': 30, 'workdir': '/var/tmp/rdx-perf'},
        'client': {'ssh': SSH + ['root@client.example'], 'wrap': '', 'workdir': '/var/tmp/rdx-perf'},
        # internal to the multiseat host: the sink in the seat's own container, RoCE loopback in its NIC
        # (no wire; a macvlan cannot reach its parent, so the Proxmox host's own IP is not a peer). Never
        # a test keyboard on the Proxmox host itself (its kernel VT would take the keys)
        'seat1-sink': {'ssh': PVE_SSH, 'exec': ['pct', 'exec', '3101', '--'],
                       'wrap': '', 'workdir': '/tmp/rdx-perf-sink'},
        'seat2-sink': {'ssh': PVE_SSH, 'exec': ['pct', 'exec', '3102', '--'],
                       'wrap': '', 'workdir': '/tmp/rdx-perf-sink'},
        'seat1': {'ssh': PVE_SSH, 'exec': ['pct', 'exec', '3101', '--'],
                  'wrap': '', 'workdir': '/tmp/rdx-perf'},
        'seat2': {'ssh': PVE_SSH, 'exec': ['pct', 'exec', '3102', '--'],
                  'wrap': '', 'workdir': '/tmp/rdx-perf'},
    },
    'sources': {
        'seat1': {'host': 'seat1', 'gpu': '0000:15:00.0', 'ip': '10.0.1.11',
                  'peers': {'host': '10.0.1.2', 'seat1-sink': '10.0.1.11'}},
        'seat2': {'host': 'seat2', 'gpu': '0000:18:00.0', 'ip': '10.0.1.12',
                  'peers': {'host': '10.0.1.2', 'seat2-sink': '10.0.1.12'}},
        'host': {'host': 'host', 'gpu': '0000:04:00.0', 'ip': '10.0.0.2', 'peers': {'client': '10.0.0.1'}},
    },
    'user': 'user', 'uid': 1000,
    'ports': {'video': 19900, 'input': 19910},
    'bins': {
        'gs_src': '/opt/rdma-display/gs/rdx_gs_src', 'input_rx': '/opt/rdma-display/p4/rdx_input_rx',
        'sink_null': '/opt/rdma-display/gs/rdx_sink_null', 'sink_win': '/opt/rdma-display/p0b/rdx_sink_win',
        'sink_kms': '/opt/rdma-display/p0b/rdx_sink_kms', 'input_tx': '/opt/rdma-display/p4/rdx_input_tx',
        'keyinject': '/opt/rdma-display/tools/rdx_keyinject.py', 'flash': '/opt/rdma-display/tools/rdx_flash',
    },
    'kms_card': '/dev/dri/card1',
}

REPORT = re.compile(r'^\s+(.+?)\s+n=(\d+)\s+p50=\s*([\d.]+)\s+p90=\s*([\d.]+)\s+p99=\s*([\d.]+)\s+max=\s*([\d.]+)')


def merge(a, b):
    for k, v in b.items():
        if isinstance(v, dict) and isinstance(a.get(k), dict):
            merge(a[k], v)
        else:
            a[k] = v
    return a


class Host:
    def __init__(self, name, cfg, dry):
        self.name, self.cfg, self.dry = name, cfg, dry

    def argv(self, cmd):
        remote = self.cfg.get('exec', []) + ['sh', '-c', cmd]
        return self.cfg['ssh'] + [' '.join(shlex.quote(x) for x in remote)]

    def run(self, cmd, timeout=60, stdin=None, check=False):
        if self.dry:
            print('  [%s] %s' % (self.name, cmd))
            return ''
        r = subprocess.run(self.argv(cmd), input=stdin, capture_output=True, text=True, timeout=timeout)
        if check and r.returncode:
            raise RuntimeError('%s: %s -> %d %s' % (self.name, cmd[:80], r.returncode, r.stderr[-400:]))
        return r.stdout

    def put(self, path, text):
        if self.dry:
            print('  [%s] %s:\n%s' % (self.name, path, ''.join('      ' + l + '\n' for l in text.splitlines())))
            return
        self.run('mkdir -p %s && cat > %s' % (shlex.quote(os.path.dirname(path)), shlex.quote(path)), stdin=text,
                 check=True)

    def wrapped(self, cmd, seconds):
        """a test process on this host: inside the host's wrap and a hard timeout"""
        lim = min(seconds, self.cfg.get('max_s', seconds))
        w = self.cfg.get('wrap', '')
        return ('%s timeout -k 2 %d %s' % (w, lim, cmd)).strip()


def wait_for(host, cmd, needle, seconds, dry):
    """poll a host until cmd prints needle (0.3 s steps)"""
    if dry:
        host.run(cmd)
        return True
    end = time.time() + seconds
    while time.time() < end:
        if needle in host.run(cmd):
            return True
        time.sleep(0.3)
    return False


def pace_slots(hz, lead_ms=8.0):
    """common/rdx_ctl.h rdx_pace_slots: one more slot while lead + period > (slots - 0.5) periods"""
    p = 1000.0 / hz
    n = 3
    while n < 8 and lead_ms + p > (n - 0.5) * p:
        n += 1
    return n


def parse_reports(text):
    out = {}
    for line in text.splitlines():
        m = REPORT.match(line)
        if m:
            out[m.group(1).strip()] = {'n': int(m.group(2)), 'p50': float(m.group(3)), 'p90': float(m.group(4)),
                                       'p99': float(m.group(5)), 'max': float(m.group(6))}
    return out


def parse_gs(text):
    r = {'reports': parse_reports(text)}
    m = re.search(r'\[gs\] done: (\d+) frames in ([\d.]+) s = ([\d.]+) fps, shown (\d+), dropped (\d+).*?lead ([\d.]+) ms',
                  text)
    if m:
        r.update(frames=int(m.group(1)), secs=float(m.group(2)), fps=float(m.group(3)), shown=int(m.group(4)),
                 dropped=int(m.group(5)), lead=float(m.group(6)))
    fl = re.search(r'\[flash\] (\d+) frames in ([\d.]+) s = ([\d.]+) fps, (\d+) keys', text)
    if fl:
        r['app_fps'], r['app_keys'] = float(fl.group(3)), int(fl.group(4))
    fps = [float(x) for x in re.findall(r'FPS: (\d+(?:\.\d+)?)', text)]
    if fps and 'app_fps' not in r:
        r['app_fps'] = statistics.mean(fps)
    sc = re.search(r'(vkmark|glmark2) Score: (\d+)', text)
    if sc:
        r['score'] = int(sc.group(2))
    return r


def parse_sink(text):
    r = {'reports': parse_reports(text)}
    m = re.search(r'\[null\] ([\d.]+) s: received (\d+) \(([\d.]+)/s\), shown (\d+) \(([\d.]+)/s\), dropped (\d+)', text)
    if m:
        r.update(secs=float(m.group(1)), received=int(m.group(2)), rx_rate=float(m.group(3)), shown=int(m.group(4)),
                 shown_rate=float(m.group(5)), dropped=int(m.group(6)))
    p = re.search(r'\[null\] payload: ([\d.]+) MB/s = ([\d.]+) Gbit/s', text)
    if p:
        r['payload_gbit'] = float(p.group(2))
    w = re.search(r'frames (\d+) in ([\d.]+) s = ([\d.]+) fps, ([\d.]+) GB/s payload', text)   # rdx_sink_win
    if w and 'shown' not in r:
        r.update(shown=int(w.group(1)), secs=float(w.group(2)), shown_rate=float(w.group(3)),
                 payload_gbit=float(w.group(4)) * 8)
    r['arrive'] = [(int(a), int(b), int(c)) for a, b, c in
                   re.findall(r'\[watch\] arrive seq=\d+ bright=(\d) luma=\d+ sw_ns=(\d+) hw_ns=(\d+)', text)]
    r['shown_ev'] = [(int(a), int(b)) for a, b in re.findall(r'\[watch\] shown seq=\d+ bright=(\d) vblank_ns=(\d+)', text)]
    # the cursor next to the picture (rdx_sink_null --watch-cursor): positions as (0, sw, hw), their vblanks
    r['cursor'] = [(0, int(a), int(b)) for a, b in
                   re.findall(r'\[watch\] cursor x=-?\d+ y=-?\d+ visible=\d sw_ns=(\d+) hw_ns=(\d+)', text)]
    r['cursor_shown'] = [(0, int(a)) for a in re.findall(r'\[watch\] cursor-shown vblank_ns=(\d+)', text)]
    c = re.search(r'\[null\] cursor next to the picture: (\d+) position changes, (\d+) pictures', text)
    if c:
        r['cursor_pos'], r['cursor_img'] = int(c.group(1)), int(c.group(2))
    return r


def parse_ctr(text):
    c = dict()
    for line in text.splitlines():
        f = line.split()
        if len(f) == 3 and f[0] in ('c0', 'c1'):
            c[f[0]] = (int(f[1]), float(f[2]))
    if 'c0' in c and 'c1' in c and c['c1'][1] > c['c0'][1]:
        return (c['c1'][0] - c['c0'][0]) * 4 * 8 / 1e9 / (c['c1'][1] - c['c0'][1])   # port_rcv_data: 4-byte words
    return None


def pair_latency(keys, arrive, shown):
    """each key press -> the first changed frame after it (and before the next press)"""
    la, ls, miss = [], [], 0
    keys = sorted(keys)
    for i, t in enumerate(keys):
        nxt = keys[i + 1] if i + 1 < len(keys) else float('inf')
        a = next(((hw or sw) for _, sw, hw in arrive if (hw or sw) > t), None)
        s = next((v for _, v in shown if v > t), None)
        if a is None or a >= nxt:
            miss += 1
            continue
        la.append((a - t) / 1e6)
        if s is not None and s < nxt + 50e6:
            ls.append((s - t) / 1e6)
    return la, ls, miss


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def app_cmd(load, seconds, cfg):
    if load == 'vkcube':
        return 'vkcube'
    if load == 'vkmark':
        # one fragment-heavy scene for the whole run; immediate (no vsync, mailbox is not offered under
        # gamescope's Xwayland on NVIDIA): the GPU runs flat out next to gamescope
        return ('vkmark --winsys xcb --fullscreen --present-mode immediate '
                '-b effect2d:kernel=blur:background-resolution=1920x1080:duration=%d' % max(5, seconds - 4))
    if load == 'glmark2':
        return 'glmark2 --fullscreen --run-forever -b terrain:duration=%d' % max(4, (seconds - 6) // 2)
    if load == 'flash':                       # a game loop with vsync (FIFO)
        return '%s --log /tmp/rdx_flash.log' % cfg['bins']['flash']
    if load == 'flash-novsync':               # a game loop with vsync off: the compositor takes the newest
        return '%s --swap-interval 0 --log /tmp/rdx_flash.log' % cfg['bins']['flash']
    if load == 'flash-idle':                  # draws only on input: the pipeline without a frame queue
        return '%s --idle --log /tmp/rdx_flash.log' % cfg['bins']['flash']
    if load == 'cursor':                      # never draws (no keys): only the pointer moves over it
        return '%s --idle --cursor --log /tmp/rdx_flash.log' % cfg['bins']['flash']
    if load == 'cursor30':                    # a still app with a frame limiter (a launcher; vkcube at fps_limit 30)
        return '%s --swap-interval 0 --fps 30 --cursor --log /tmp/rdx_flash.log' % cfg['bins']['flash']
    raise SystemExit('unknown load %s' % load)


def run_cell(cfg, args, src_name, sink_spec, mode, load, rep, outdir):
    src = cfg['sources'][src_name]
    kind, chost = sink_spec.split('@')
    host = Host(src['host'], cfg['hosts'][src['host']], args.dry_run)
    client = Host(chost, cfg['hosts'][chost], args.dry_run)
    sink_ip = src['peers'][chost]
    w, h, hz = re.match(r'(\d+)x(\d+)@([\d.]+)', mode).groups()
    w, h, hz = int(w), int(h), float(hz)
    slots = pace_slots(hz) if args.slots == 'auto' else int(args.slots)
    vport, iport = cfg['ports']['video'], cfg['ports']['input']
    pointer = load.startswith('cursor')       # the mouse moves (rdx_keyinject.py --mouse) instead of keys
    latency = (args.latency and load.startswith('flash')) or pointer
    D = args.seconds
    tag = '%s-%s-%s-%dx%d-%g-%s-%d' % (src_name, kind, chost, w, h, hz, load, rep)
    cdir = cfg['hosts'][chost]['workdir'] + '/' + tag
    hdir = cfg['hosts'][src['host']]['workdir']
    b = cfg['bins']
    sink_secs = D + 6
    if kind == 'null':
        sink = '%s --dev auto --bind %s --port %d --mode %dx%d@%g --slots %d --seconds %d --cursor %s' % (
            b['sink_null'], sink_ip, vport, w, h, hz, slots, sink_secs, args.cursor)
        if pointer:
            sink += ' --watch-cursor'
        elif latency:
            sink += ' --watch %d,%d' % (w // 2, h // 2)
        ready = 'waiting on'
    elif kind == 'win':
        sink = '%s --dev auto --bind %s --port %d --mtu 4096 --mode %dx%d@%g --slots %d --win 960x540 --title rdx_perf --cursor %s' % (
            b['sink_win'], sink_ip, vport, w, h, hz, slots, args.cursor)
        ready = 'listening on'
    elif kind == 'kms':
        sink = '%s --dev auto --bind %s --port %d --mtu 4096 --mode %dx%d@%g --slots %d --card %s --log-dir - --cursor %s' % (
            b['sink_kms'], sink_ip, vport, w, h, hz, slots, cfg['kms_card'], args.cursor)
        ready = 'waiting'
    else:
        raise SystemExit('sink kind null|win|kms')
    if latency and kind != 'null' and not pointer:
        raise SystemExit('--latency needs a null sink (rdx_sink_null --watch)')
    watch_needle = '\\[watch\\] ' if pointer else '\\[watch\\] arrive'
    if pointer and kind != 'null':
        latency = False                       # the motion still runs (gs fps), the latency needs rdx_sink_null
    # the client's part of the run: one script under the host's wrap (one lock for all its processes)
    cell = ['set -u', 'D=%s; mkdir -p $D; cd $D; rm -f go done *.log ctr.txt' % cdir,
            'IF=$(ip -o -4 addr show | awk -v ip=%s \'index($4, ip "/") == 1 {print $2; exit}\')' % sink_ip,
            'DEV=$(ls /sys/class/net/$IF/device/infiniband/ 2>/dev/null | head -1)',
            'CTR=/sys/class/infiniband/$DEV/ports/1/counters/port_rcv_data',
            '%s > sink.log 2>&1 &' % sink, 'SP=$!']
    inject_src = cfg['hosts'][chost].get('inject_on') == 'source'
    keys = []
    if latency:   # injector + rdx_input_tx: on the client, or on the source when the client has no safe uinput
        n_keys = max(10, int((D - 9) / 0.22))     # presses start ~6 s into the run (gamescope + 2.5 s)
        if pointer:                           # the mouse: about 30 motions/s for the 30 fps app, ~5/s to time each
            n_keys = int((D - 9) / 0.033) if load == 'cursor30' else n_keys
        kopt = (' --mouse' + (' --min-ms 25 --max-ms 41' if load == 'cursor30' else '')) if pointer else ''
        keys = ['python3 %s%s --count %d --wait-file $D/go --wait-s %d > $D/keys.log 2>&1 &' % (b['keyinject'], kopt, n_keys, D + 10),
                'KP=$!',
                'for i in $(seq 100); do grep -q "^\\[key\\] device /dev" $D/keys.log && break; sleep 0.05; done',
                'NODE=$(sed -n "s/^\\[key\\] device //p" $D/keys.log)',
                # in a container the node appears when rdx-seatmgr hands it over (~0.3 s)
                'for i in $(seq 100); do [ -e "$NODE" ] && break; sleep 0.05; done',
                '%s --dev auto --peer %s --port %d --input $NODE --duration-s %d > $D/itx.log 2>&1 &' % (
                    b['input_tx'], src['ip'], iport, D + 12),
                'TP=$!']
        keys_end = ['wait $KP', 'kill $TP 2>/dev/null', 'wait $TP']
        if not inject_src:
            cell += keys
    cell += ['for i in $(seq 600); do grep -q "connected" sink.log && break; kill -0 $SP 2>/dev/null || break; sleep 0.05; done',
             'echo "c0 $(cat $CTR) $(date +%s.%N)" > ctr.txt']
    cell += ['wait $SP', 'echo "c1 $(cat $CTR) $(date +%s.%N)" >> ctr.txt']
    if latency and not inject_src:
        cell += keys_end
    cell += ['touch done']
    sock = '/run/user/%d/rdx-gs-perf.sock' % cfg['uid']
    gs = '%s --dev auto --peer %s --port %d --gpu %s --buffers host --user %s --socket %s --log-dir - -- %s' % (
        b['gs_src'], sink_ip, vport, src['gpu'], cfg['user'], sock, app_cmd(load, D, cfg))
    # the source's part, also one script under its host's wrap: the input receiver listens before the
    # client's rdx_input_tx connects, rdx_gs_src starts once the sink listens (file go-gs)
    hs = ['set -u', 'D=%s; mkdir -p $D; cd $D; rm -f go go-gs rx-up host-done gs.log irx.log keys.log itx.log' % hdir]
    if latency:
        hs += ['%s --dev auto --bind %s --port %d > irx.log 2>&1 &' % (b['input_rx'], src['ip'], iport), 'RP=$!']
        if inject_src:
            hs += ['sleep 0.3'] + keys
    hs += ['touch rx-up',
           'for i in $(seq 1200); do [ -e go-gs ] && break; sleep 0.05; done',
           '[ -e go-gs ] || { echo "no sink"; exit 1; }',
           'cd /tmp; rm -f /tmp/rdx_flash.log',
           'timeout -s TERM -k 5 %d %s > $D/gs.log 2>&1' % (D, gs),
           'cat /tmp/rdx_flash.log >> $D/gs.log 2>/dev/null']
    if latency:
        hs += (keys_end if inject_src else []) + ['kill $RP 2>/dev/null; wait $RP']
    hs += ['touch $D/host-done']
    print('== %s' % tag, flush=True)
    host.put(hdir + '/host.sh', '\n'.join(hs) + '\n')
    host.run('rm -f %s/rx-up %s/host-done; setsid sh -c %s > /dev/null 2>&1 < /dev/null &' % (hdir, hdir, shlex.quote(
        host.wrapped('sh %s/host.sh' % hdir, D + 45) + ' > %s/host.out 2>&1; touch %s/host-done' % (hdir, hdir))))
    if not wait_for(host, '[ -e %s/rx-up ] && echo yes' % hdir, 'yes', 90, args.dry_run):
        print('   source script did not start (test lock?): %s' % host.run('cat %s/host.out' % hdir)[-400:])
        return None
    client.put(cdir + '/cell.sh', '\n'.join(cell) + '\n')
    client.run('rm -f %s/done; setsid sh -c %s > /dev/null 2>&1 < /dev/null &' % (cdir, shlex.quote(
        client.wrapped('sh %s/cell.sh' % cdir, D + 10) + ' > %s/cell.out 2>&1; touch %s/done' % (cdir, cdir))))
    if not wait_for(client, 'cat %s/sink.log 2>/dev/null' % cdir, ready, 90, args.dry_run):  # lock held elsewhere
        print('   sink did not come up: %s' % client.run('cat %s/cell.out %s/sink.log 2>/dev/null' % (cdir, cdir))[-600:])
        host.run('touch %s/go-gs' % hdir)     # let the source script run out instead of waiting
        return None
    host.run('touch %s/go-gs' % hdir)
    if latency:   # keys once the app's first frame is at the sink and its input device had time to appear
        if wait_for(client, 'grep -q "^%s" %s/sink.log && echo yes' % (watch_needle, cdir), 'yes', 20, args.dry_run) or pointer:
            time.sleep(2.5)
        (host if inject_src else client).run('touch %s/go' % (hdir if inject_src else cdir))
    wait_for(host, '[ -e %s/host-done ] && echo yes' % hdir, 'yes', D + 50, args.dry_run)
    wait_for(client, '[ -e %s/done ] && echo yes' % cdir, 'yes', 30, args.dry_run)
    if args.dry_run:
        return None
    logs = {'gs': host.run('cat %s/gs.log' % hdir), 'sink': client.run('cat %s/sink.log' % cdir),
            'ctr': client.run('cat %s/ctr.txt' % cdir), 'cell': client.run('cat %s/cell.out 2>/dev/null' % cdir),
            'keys': (host if inject_src else client).run('cat %s/keys.log 2>/dev/null' % (hdir if inject_src else cdir)),
            'itx': (host if inject_src else client).run('cat %s/itx.log 2>/dev/null' % (hdir if inject_src else cdir)),
            'irx': host.run('cat %s/irx.log 2>/dev/null' % hdir) if latency else ''}
    os.makedirs(outdir, exist_ok=True)
    for k, v in logs.items():
        if v:
            with open(os.path.join(outdir, '%s.%s.log' % (tag, k)), 'w') as f:
                f.write(v)
    g, s = parse_gs(logs['gs']), parse_sink(logs['sink'])
    res = {'tag': tag, 'source': src_name, 'sink': sink_spec, 'mode': mode, 'load': load, 'slots': slots, 'gs': g,
           'sink_r': s, 'nic_gbit': parse_ctr(logs['ctr'])}
    if latency:
        keys = [int(t) for t in re.findall(r'\[key\] press \d+ t_ns=(\d+)', logs['keys'])]
        arrive, shown = s['arrive'], s['shown_ev']
        if pointer:                           # the first pointer update either way: a changed frame or a position
            arrive = sorted(arrive + s['cursor'], key=lambda e: e[2] or e[1])
            shown = sorted(shown + s['cursor_shown'], key=lambda e: e[1])
        la, ls, miss = pair_latency(keys, arrive, shown)
        res['lat'] = {'keys': len(keys), 'arrive': la, 'shown': ls, 'miss': miss}
    return res


def fmt(v, f='%.1f'):
    return '-' if v is None or v != v else f % v


def table(results):
    hdr = ('source', 'sink', 'mode', 'load', 'app fps', 'gs fps', 'shown/s', 'drops', 'slots', 'arr->vbl p50/p99',
           'comp p50', 'post->shown p50', 'lead ms', 'NIC / payload Gbit/s')
    rows = []
    for r in results:
        g, s = r['gs'], r['sink_r']
        rep = s['reports'].get('NIC CQE -> vblank (ms)') or s['reports'].get('CQE -> vblank (ms)') or \
            g['reports'].get('sink arrival -> vblank (ms)') or s['reports'].get('CQE -> GPU done')   # window: drawn
        ps = g['reports'].get('post -> shown report (ms)')
        cp = g['reports'].get('gs wake -> GPU done (ms)')
        app = g.get('app_fps')
        if app is None and r['load'] == 'vkcube':
            app = g.get('fps')
        # one time base for the rates: rdx_gs_src's streaming time (the sink's also counts gamescope's start)
        shown = s['shown'] / g['secs'] if s.get('shown') is not None and g.get('secs') else s.get('shown_rate')
        if shown is None and g.get('shown') is not None and g.get('secs'):
            shown = g['shown'] / g['secs']
        drops = s.get('dropped', g.get('dropped'))
        rows.append((r['source'], r['sink'], r['mode'], r['load'] + (' (score %d)' % g['score'] if g.get('score') else ''),
                     fmt(app), fmt(g.get('fps')), fmt(shown), fmt(drops, '%d'), str(r['slots']),
                     '%s / %s' % (fmt(rep and rep['p50'], '%.2f'), fmt(rep and rep['p99'], '%.2f')),
                     fmt(cp and cp['p50'], '%.2f'), fmt(ps and ps['p50'], '%.2f'), fmt(g.get('lead'), '%.2f'),
                     '%s / %s' % (fmt(r['nic_gbit'], '%.2f'), fmt(s.get('payload_gbit'), '%.2f'))))
    wid = [max(len(str(x[i])) for x in rows + [hdr]) for i in range(len(hdr))]
    line = lambda x: '| ' + ' | '.join(str(c).ljust(wid[i]) for i, c in enumerate(x)) + ' |'
    print(line(hdr))
    print('|' + '|'.join('-' * (n + 2) for n in wid) + '|')
    for x in rows:
        print(line(x))
    lat = [r for r in results if r.get('lat')]
    kf = lambda r: (r['source'], r['sink'], r['mode'], r['load'])
    for key in sorted(set(kf(r) for r in lat)):
        sel = [r for r in lat if kf(r) == key]
        la = [v for r in sel for v in r['lat']['arrive']]
        ls = [v for r in sel for v in r['lat']['shown']]
        nk, miss = sum(r['lat']['keys'] for r in sel), sum(r['lat']['miss'] for r in sel)
        hz = float(key[2].split('@')[1])
        what = 'motions' if key[3].startswith('cursor') else 'keys'
        print('latency %s -> %s %s %s: %d %s, %d without a changed frame' % (key + (nk, what, miss)))
        print('  key -> changed frame at the sink NIC  p50 %s  p90 %s  p99 %s  max %s ms' % (
            fmt(pct(la, .5), '%.2f'), fmt(pct(la, .9), '%.2f'), fmt(pct(la, .99), '%.2f'), fmt(max(la) if la else None, '%.2f')))
        print('  key -> vblank showing it               p50 %s  p90 %s  p99 %s  max %s ms  (+%.2f ms beam to the centre)' % (
            fmt(pct(ls, .5), '%.2f'), fmt(pct(ls, .9), '%.2f'), fmt(pct(ls, .99), '%.2f'),
            fmt(max(ls) if ls else None, '%.2f'), 500.0 / hz))


def main():
    ap = argparse.ArgumentParser(description='stream performance and latency matrix (see the header)')
    ap.add_argument('--source', required=True, help='comma list of profile sources, e.g. seat1,seat2,host')
    ap.add_argument('--sink', required=True, help='comma list KIND@HOST, e.g. null@host, null@client, win@host, kms@client, null@seat2-sink')
    ap.add_argument('--modes', default='1920x1080@240,2560x1440@144,3840x2160@60')
    ap.add_argument('--loads', default='vkcube,vkmark,flash')
    ap.add_argument('--latency', action='store_true', help='inject keys for the flash load')
    ap.add_argument('--seconds', type=int, default=20)
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--slots', default='auto')
    ap.add_argument('--cursor', default='auto', choices=('auto', 'picture'),
                    help="the sink's cursor: auto = next to the picture where it can, picture = drawn into the frames")
    ap.add_argument('--config', help='JSON profile merged over the built-in example profile')
    ap.add_argument('--bin', action='append', default=[], metavar='NAME=PATH', help='override a binary path')
    ap.add_argument('--out', default='rdx-perf-%s' % time.strftime('%Y%m%d-%H%M%S'))
    ap.add_argument('--json', help='write all results (with raw latency samples) here')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()
    cfg = json.loads(json.dumps(PROFILE))
    if args.config:
        merge(cfg, json.load(open(args.config)))
    for kv in args.bin:
        k, v = kv.split('=', 1)
        cfg['bins'][k] = v
    results = []
    for src in args.source.split(','):
        for sink in args.sink.split(','):
            if sink.split('@')[1] not in cfg['sources'][src]['peers']:
                print('skip %s -> %s: no link' % (src, sink))
                continue
            for mode in args.modes.split(','):
                for load in args.loads.split(','):
                    for rep in range(args.repeat):
                        r = run_cell(cfg, args, src, sink, mode, load, rep, args.out)
                        if r:
                            results.append(r)
    if results:
        table(results)
        if args.json:
            json.dump(results, open(args.json, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
