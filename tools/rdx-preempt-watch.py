#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""rdx-preempt-watch: does the kernel's preemption mode (preempt=full instead of voluntary) help the
seats, and does it cost the host anything?

Runs on the Proxmox host as a systemd service (tools/rdx-preempt-watch.service) and writes one row per
minute. Benefit:
  - wake-up latency of a SCHED_FIFO and a SCHED_OTHER thread (rt-tests' cyclictest, 1 ms interval,
    one thread each, --laptop: /dev/cpu_dma_latency is left alone, so the C-states stay as they are);
  - PipeWire xruns in the running seat containers (pw-top -b, the ERR column, read only);
  - audio underruns of rdx-hostd in the seat containers and of the clients (journal, read only).
Cost:
  - host CPU user/sys/irq/softirq/iowait %, context switches/s, interrupts/s, load, PSI cpu/io;
  - ZFS throughput per pool (sum of the datasets' nread/nwritten kstats).
Mode: /sys/kernel/debug/sched/preempt every minute and the kernel's "Dynamic Preempt: X" lines; every
change, and the mode after a reboot, is an event of its own.

Output (all under /var/log/rdx-preempt-watch, "out_dir"):
  minutes-YYYY-MM-DD.csv   one row per minute (UTC)
  modes.csv                the mode timeline (the host's journal is volatile, so the watcher keeps it)
  alarms.log               one line per NEW finding; follow it on the host with
                             tail -n0 -F /var/log/rdx-preempt-watch/alarms.log
  report-YYYY-MM-DD.txt    the daily comparison (subcommand "report", tools/rdx-preempt-watch-report.timer)
and, when "vm_write" is set, the same numbers to a metrics database that takes the Influx line protocol
(e.g. VictoriaMetrics' /write) as rdx_preempt_*, rdx_preempt_seat_*, rdx_preempt_client_*; "" (the
default) writes nothing there.

A finding is reported once when it starts; the same class is quiet until it has been clear for
"rearm_min" minutes (sources: a line when they fail and one when they are back).

The comparison never switches the mode to measure: voluntary numbers come from history (the host's
node_exporter series, read from "vm_query", a Prometheus query API, with the instance label
"node_instance"; the clients' and seats' journals; and this watcher's own rows while the host ran
voluntary). Without "vm_query" / "node_instance" the report leaves out the host cost history.

Configuration: defaults below, overridden by /etc/rdx/preempt-watch.json (--config FILE; same keys,
merged one level). The clients whose rdx-clientd journals are read are a list of
  {"name": NAME, "ssh": [ssh arguments up to the target], "via": [optional command prefix]}
reached as `[via...] ssh -o BatchMode=yes ... <ssh...> journalctl ...`, e.g.
  "clients": [{"name": "desktop", "ssh": ["root@host.example"]},
              {"name": "client", "via": ["lxc-attach", "-n", "200", "--"],
               "ssh": ["-i", "/etc/rdx/client_key", "-o", "IdentitiesOnly=yes", "root@client.example"]}]
("via": the ssh runs inside that container, e.g. where the client's key lives). Default: no clients.
Usage: rdx-preempt-watch.py run | report [--date YYYY-MM-DD] [--days N] | once (one row to stdout, test)
"""
import argparse
import bisect
import collections
import csv
import datetime as dt
import glob
import json
import os
import re
import resource
import shlex
import signal
import socket
import subprocess
import sys
import time
import urllib.parse
import urllib.request

CONF = {
    "out_dir": "/var/log/rdx-preempt-watch",
    "run_dir": "/run/rdx-preempt-watch",
    "vm_write": "",                                # e.g. "http://metrics.example:8428/write"; "" = off
    "vm_query": "",                                # e.g. "http://metrics.example:8428"; "" = no cost history
    "node_instance": "",                           # this host's node_exporter instance label, e.g. "host.example:9100"
    "host_label": socket.gethostname() or "host",
    "cyclictest": "cyclictest",
    "interval_us": 1000,
    "rt_prio": 80,
    "window_s": 55,                                # cyclictest runs this long inside each minute
    # where the two probe threads run: "seats" = the CPUs of the running seat containers (their cgroup's
    # cpuset.cpus.effective, so the probe measures the seat's side when the seats get a chiplet of their
    # own), a cpu list such as "8-15,24-31", or "" for no pinning
    "wake_cpus": "seats",
    "seats": [{"ct": 3101, "seat": 1}, {"ct": 3102, "seat": 2}, {"ct": 3103, "seat": 3}],
    "seat_user": "user",                           # the desktop user in the seat containers (PipeWire)
    "seat_uid": 1000,
    "clients": [],                                 # see the docstring for the structure
    "client_units": ["rdx-clientd", "rdx-clientd-window"],
    "alarm": {
        "wake_rt_max_ms": 4.0,        # SCHED_FIFO thread woke later than this within a minute
        "wake_other_max_ms": 20.0,    # SCHED_OTHER thread
        "rearm_min": 60,              # a class is quiet until clear for this many minutes
        "cost_factor": 1.5,           # 15-min mean above factor x the 24 h median ...
        "cost_floor": {"cpu_sys_pct": 3.0, "cpu_softirq_pct": 1.0, "ctxt_per_s": 50000.0},  # ... and above this
    },
}

MODES = ("none", "voluntary", "full", "lazy")
CSV_COLS = [
    "ts_utc", "mode",
    "wake_rt_p50_us", "wake_rt_p99_us", "wake_rt_max_us", "wake_rt_over_1ms", "wake_rt_samples",
    "wake_other_p50_us", "wake_other_p99_us", "wake_other_max_us", "wake_other_over_1ms", "wake_other_samples",
    "pw_xruns", "pw_xruns_by_ct", "hostd_underruns", "hostd_underruns_by", "hostd_audio_reports",
    "client_underruns", "client_underruns_by", "client_audio_reports", "startup_underruns",
    "cpu_user_pct", "cpu_sys_pct", "cpu_irq_pct", "cpu_softirq_pct", "cpu_iowait_pct",
    "ctxt_per_s", "intr_per_s", "load1", "psi_cpu_some_pct", "psi_io_some_pct",
    "zfs_read_mbs", "zfs_write_mbs", "zfs_by_pool", "self_cpu_ms", "sources_down",
]


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def load_conf(path="/etc/rdx/preempt-watch.json"):
    c = json.loads(json.dumps(CONF))
    try:
        with open(path) as f:
            over = json.load(f)
        for k, v in over.items():
            if isinstance(v, dict) and isinstance(c.get(k), dict):
                c[k].update(v)
            else:
                c[k] = v
    except FileNotFoundError:
        pass
    return c


def utc(ts):
    return dt.datetime.fromtimestamp(ts, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def run(cmd, timeout=10):
    """(rc, stdout) of a command, never raises; rc 255 for a timeout or a missing program"""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors="replace")
        return p.returncode, p.stdout
    except (subprocess.TimeoutExpired, OSError):
        return 255, ""


# ------------------------------------------------------------------ mode

def read_mode():
    try:
        with open("/sys/kernel/debug/sched/preempt") as f:
            m = re.search(r"\((\w+)\)", f.read())
            return m.group(1) if m else "unknown"
    except OSError:
        return "unknown"


def boot_id():
    try:
        with open("/proc/sys/kernel/random/boot_id") as f:
            return f.read().strip()
    except OSError:
        return ""


def kernel_mode_lines(since):
    """[(ts, mode)] of the kernel's "Dynamic Preempt: X" lines since a unix time (this boot only)"""
    rc, out = run(["journalctl", "-k", "-b", "--no-pager", "-o", "short-unix", "--since", "@%d" % int(since),
                   "--grep", "Dynamic Preempt"], timeout=20)
    res = []
    for line in out.splitlines():
        m = re.match(r"([0-9.]+) .*Dynamic Preempt: (\w+)", line)
        if m:
            res.append((float(m.group(1)), m.group(2)))
    return res


# ------------------------------------------------------------------ wake-up latency (cyclictest)

class Cyclic:
    def __init__(self, c, name, policy_args):
        self.c, self.name, self.args = c, name, policy_args
        self.proc, self.hist = None, os.path.join(c["run_dir"], "cyclic-%s.hist" % name)

    def start(self, cpus=None):
        try:
            os.unlink(self.hist)
        except FileNotFoundError:
            pass
        pin = (lambda: os.sched_setaffinity(0, cpus)) if cpus else None
        cmd = [self.c["cyclictest"], "-q", "-m", "--laptop", "-t1", "-i", str(self.c["interval_us"]),
               "-D", str(self.c["window_s"]), "-h", "20000", "--histfile=" + self.hist] + self.args
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=pin)
        except OSError as e:
            log("[watch] cyclictest %s: %s" % (self.name, e))
            self.proc = None

    def result(self):
        """{p50, p99, max, over_1ms, samples} in us of the finished run, or None"""
        if not self.proc:
            return None
        try:
            self.proc.wait(timeout=self.c["window_s"] + 10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            return None
        self.proc = None
        hist, total, mx, overflow = {}, 0, 0, 0
        try:
            with open(self.hist) as f:
                for line in f:
                    if line.startswith("# Total:"):
                        total = int(line.split()[-1])
                    elif line.startswith("# Max Latencies:"):
                        mx = int(line.split()[-1])
                    elif line.startswith("# Histogram Overflows:"):
                        overflow = int(line.split()[-1])
                    elif line[:1].isdigit():
                        us, n = line.split()[:2]
                        if int(n):
                            hist[int(us)] = int(n)
        except (OSError, ValueError):
            return None
        if not total:
            return None

        def pct(q):
            want, acc = q * total, 0
            for us in sorted(hist):
                acc += hist[us]
                if acc >= want:
                    return us
            return mx            # in the overflow
        over = overflow + sum(n for us, n in hist.items() if us > 1000)
        return {"p50": pct(0.50), "p99": pct(0.99), "max": mx, "over_1ms": over, "samples": total}


# ------------------------------------------------------------------ host counters

def host_snapshot():
    s = {"t": time.monotonic()}
    with open("/proc/stat") as f:
        for line in f:
            p = line.split()
            if p[0] == "cpu":
                s["cpu"] = [int(x) for x in p[1:9]]   # user nice system idle iowait irq softirq steal
            elif p[0] == "ctxt":
                s["ctxt"] = int(p[1])
            elif p[0] == "intr":
                s["intr"] = int(p[1])
    with open("/proc/loadavg") as f:
        s["load1"] = float(f.read().split()[0])
    for res in ("cpu", "io"):
        try:
            with open("/proc/pressure/" + res) as f:
                s["psi_" + res] = int(re.search(r"^some .*total=(\d+)", f.read(), re.M).group(1))
        except (OSError, AttributeError):
            s["psi_" + res] = None
    s["zfs"] = {}
    for d in glob.glob("/proc/spl/kstat/zfs/*/"):
        pool = os.path.basename(d.rstrip("/"))
        r = w = 0
        objs = glob.glob(d + "objset-*")
        if not objs:
            continue
        for o in objs:
            try:
                with open(o) as f:
                    for line in f:
                        p = line.split()
                        if len(p) == 3 and p[0] == "nread":
                            r += int(p[2])
                        elif len(p) == 3 and p[0] == "nwritten":
                            w += int(p[2])
            except (OSError, ValueError):
                pass
        s["zfs"][pool] = (r, w)
    return s


def host_delta(a, b):
    dt_s = max(b["t"] - a["t"], 1e-3)
    d = [y - x for x, y in zip(a["cpu"], b["cpu"])]
    tot = max(sum(d), 1)
    r = {
        "cpu_user_pct": 100.0 * (d[0] + d[1]) / tot, "cpu_sys_pct": 100.0 * d[2] / tot,
        "cpu_iowait_pct": 100.0 * d[4] / tot, "cpu_irq_pct": 100.0 * d[5] / tot,
        "cpu_softirq_pct": 100.0 * d[6] / tot,
        "ctxt_per_s": (b["ctxt"] - a["ctxt"]) / dt_s, "intr_per_s": (b["intr"] - a["intr"]) / dt_s,
        "load1": b["load1"],
    }
    for res in ("cpu", "io"):
        x, y = a.get("psi_" + res), b.get("psi_" + res)
        r["psi_%s_some_pct" % res] = 100.0 * (y - x) / (dt_s * 1e6) if x is not None and y is not None else None
    rd = wr = 0.0
    by = []
    for pool, (r1, w1) in sorted(b["zfs"].items()):
        r0, w0 = a["zfs"].get(pool, (r1, w1))
        pr, pw = max(r1 - r0, 0) / dt_s / 1e6, max(w1 - w0, 0) / dt_s / 1e6
        rd, wr = rd + pr, wr + pw
        by.append("%s:%.1f/%.1f" % (pool, pr, pw))
    r.update({"zfs_read_mbs": rd, "zfs_write_mbs": wr, "zfs_by_pool": ";".join(by)})
    return r


# ------------------------------------------------------------------ seats: PipeWire xruns, hostd underruns

def ct_running(ct):
    return os.path.isdir("/sys/fs/cgroup/lxc/%d" % ct)


def cpulist(text):
    """'2,5,9-12' -> {2, 5, 9, 10, 11, 12}"""
    out = set()
    for part in text.strip().split(","):
        if part:
            a, _, b = part.partition("-")
            out.update(range(int(a), int(b or a) + 1))
    return out


def cpulist_str(cpus):
    """{2, 9, 10, 11} -> '2,9-11'"""
    out, run = [], []
    for x in sorted(cpus):
        if run and x != run[-1] + 1:
            out.append("%d-%d" % (run[0], run[-1]) if len(run) > 1 else str(run[0]))
            run = []
        run.append(x)
    if run:
        out.append("%d-%d" % (run[0], run[-1]) if len(run) > 1 else str(run[0]))
    return ",".join(out)


def wake_cpus(c):
    """(label, cpu set or None) the probe threads run on, see "wake_cpus" """
    want = c.get("wake_cpus", "seats")
    if not want:
        return "unpinned", None
    if want != "seats":
        return want, cpulist(want)
    cpus = set()
    for s in c["seats"]:
        try:
            with open("/sys/fs/cgroup/lxc/%d/cpuset.cpus.effective" % s["ct"]) as f:
                cpus |= cpulist(f.read())
        except (OSError, ValueError):
            pass
    # the watcher's own cgroup may not reach them (system.slice limited to the host's chiplet): then a
    # pinned probe would not start at all, so it stays where it is allowed and the line says so
    cpus &= os.sched_getaffinity(0)
    if not cpus:
        return "unpinned (no running seat)", None
    return "seats " + cpulist_str(cpus), cpus


def pw_errs(c, ct):
    """{(node id, name): ERR} of the DRIVER nodes from pw-top's last block, or None when it cannot be
    read. A driver's ERR is its xrun count since the node started (cumulative); a follower's (" + name")
    counts only while this pw-top runs, so followers are left out."""
    rc, out = run(["lxc-attach", "-n", str(ct), "--", "runuser", "-u", c["seat_user"], "--", "env",
                   "XDG_RUNTIME_DIR=/run/user/%d" % c["seat_uid"], "timeout", "5", "pw-top", "-b", "-n", "2"],
                  timeout=15)
    if rc != 0 or "ERR" not in out:
        return None
    blocks, cur, hdr = [], None, None
    for line in out.splitlines():
        if "ERR" in line and "NAME" in line:
            hdr, cur = line, []
            blocks.append((hdr, cur))
        elif cur is not None and line.strip():
            cur.append(line)
    if not blocks:
        return None
    hdr, lines = blocks[-1]
    e_end, n_at = hdr.index("ERR") + 3, hdr.index("NAME")
    res = {}
    for line in lines:
        f = line.split()
        if len(f) < 2 or not f[1].isdigit():
            continue
        m = re.search(r"(\d+)\s*$", line[:e_end])
        name = line[n_at:].strip()
        if m and not name.startswith("+"):
            res[(f[1], name)] = int(m.group(1))
    return res


UNDERRUN_RE = re.compile(r"\[(\w+)\] \[(atx|arx)\]\s+(?:(\d+) s .*?underruns (\d+)|done: .*?underruns (\d+))")


def journal_underruns(lines_json):
    """[(ts, pid, tag, seconds_or_None, cumulative underruns)] from journalctl -o json lines"""
    res = []
    for line in lines_json.splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        msg = e.get("MESSAGE")
        if isinstance(msg, list):
            msg = bytes(msg).decode("utf-8", "replace")
        if not isinstance(msg, str) or "underruns" not in msg:
            continue
        m = UNDERRUN_RE.search(msg)
        if not m:
            continue
        tag = m.group(1) + "/" + m.group(2)
        secs = int(m.group(3)) if m.group(3) else None
        n = int(m.group(4) if m.group(4) is not None else m.group(5))
        res.append((int(e.get("__REALTIME_TIMESTAMP", "0")) / 1e6, e.get("_PID", "?"), tag, secs, n))
    return res


AUDIO_TAGS = ("audio/arx", "audio_tx/atx")


def stream_step(state, k, secs, n):
    """One report of a stream counter. state: key -> (stream seconds, cumulative underruns).
    The counters are per STREAM, not per process (one rdx-hostd / rdx-clientd process carries many
    streams one after another, each starting again at 10 s and a low count), so a report whose seconds or
    count went down, or a "done" line (secs None), ends the old stream. Returns (new_stream, d_underruns,
    d_seconds); a new stream's first report is only a baseline (d_* 0)."""
    prev = state.get(k)
    if secs is None:                               # the stream's final "done" line
        state.pop(k, None)
        return prev is None, (n - prev[1]) if prev and n > prev[1] else 0, 0
    if prev is None or secs < prev[0] or n < prev[1]:
        state[k] = (secs, n)
        return True, 0, 0
    state[k] = (secs, n)
    return False, n - prev[1], secs - prev[0]


class UnderrunTracker:
    """cumulative per-stream counters -> increments; a stream's first report (its first 10 s) counts as
    start-up, a stream first seen mid-life only sets the baseline"""

    def __init__(self):
        self.last = {}

    def feed(self, key, rows, startup_ok=True):
        inc = start = reports = 0
        for ts, pid, tag, secs, n in rows:
            k = (key, pid, tag)
            audio = tag in AUDIO_TAGS
            if audio:
                reports += 1
            new, dn, ds = stream_step(self.last, k, secs, n)
            if new:
                if startup_ok and audio and secs is not None and secs <= 10:
                    start += n
            elif audio:
                inc += dn
        if len(self.last) > 5000:
            self.last = dict(list(self.last.items())[-2000:])
        return inc, start, reports


# ------------------------------------------------------------------ alarms

class Alarms:
    def __init__(self, c):
        self.c, self.path = c, os.path.join(c["out_dir"], "alarms.log")
        self.active, self.clear = {}, collections.Counter()

    def emit(self, text, ts=None):
        line = "%s %s" % (utc(ts or time.time()), text)
        with open(self.path, "a") as f:
            f.write(line + "\n")
        log("[alarm] " + line)

    def check(self, cls, bad, text, value=None):
        """one line when cls turns bad; again only after rearm_min clear minutes, or when twice as bad"""
        if bad:
            self.clear[cls] = 0
            prev = self.active.get(cls)
            if prev is None:
                self.active[cls] = value
                self.emit(text)
            elif value is not None and prev and value >= 2 * prev:
                self.active[cls] = value
                self.emit(text + " (worse)")
        elif cls in self.active:
            self.clear[cls] += 1
            if self.clear[cls] >= self.c["alarm"]["rearm_min"]:
                del self.active[cls]

    def source(self, name, ok, why=""):
        cls = "SOURCE-" + name
        if not ok and cls not in self.active:
            self.active[cls] = 1
            self.emit("SOURCE %s unavailable%s" % (name, (": " + why) if why else ""))
        elif ok and cls in self.active:
            del self.active[cls]
            self.emit("SOURCE %s back" % name)


# ------------------------------------------------------------------ output

def write_csv(c, row):
    day = row["ts_utc"][:10]
    path = os.path.join(c["out_dir"], "minutes-%s.csv" % day)
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLS, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerow({k: ("" if row.get(k) is None else (("%.3f" % row[k]) if isinstance(row[k], float) else row[k]))
                    for k in CSV_COLS})


def influx_fields(d):
    parts = []
    for k, v in d.items():
        if v is None or isinstance(v, str):
            continue
        parts.append("%s=%s" % (k, ("%di" % v) if isinstance(v, int) and not isinstance(v, bool) else repr(float(v))))
    return ",".join(parts)


def push_vm(c, ts, lines):
    if not c.get("vm_write") or not lines:
        return True
    body = "\n".join("%s %d" % (l, int(ts) * 1000000000) for l in lines if l.split(" ", 1)[-1]).encode()
    try:
        req = urllib.request.Request(c["vm_write"], data=body, method="POST")
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status in (200, 204)
    except OSError:
        return False


def backfill_modes(c, now, hours=24):
    """The mode timeline of the last hours as one rdx_preempt_mode_code point per minute, from modes.csv, so
    the dashboard's timeline shows the voluntary period before the watcher's first minute row too (writing
    a minute twice is harmless: same series, same timestamp). Returns the number of points written."""
    tl = read_modes(c)
    if not tl or not c.get("vm_write"):
        return 0
    first = int(max(tl[0][0], now - hours * 3600) // 60 + 1) * 60
    pts, i, t = [], 0, first
    while t < int(now) // 60 * 60:
        while i + 1 < len(tl) and tl[i + 1][0] <= t:
            i += 1
        m = tl[i][1]
        pts.append("rdx_preempt,host=%s,mode=%s mode_code=%di %d" % (
            c["host_label"], m, MODES.index(m) if m in MODES else -1, t * 1000000000))
        t += 60
    for k in range(0, len(pts), 500):
        try:
            req = urllib.request.Request(c["vm_write"], data="\n".join(pts[k:k + 500]).encode(), method="POST")
            with urllib.request.urlopen(req, timeout=10) as r:
                if r.status not in (200, 204):
                    return k
        except OSError:
            return k
    return len(pts)


def append_mode(c, ts, mode, why):
    path = os.path.join(c["out_dir"], "modes.csv")
    new = not os.path.exists(path)
    with open(path, "a") as f:
        if new:
            f.write("ts_utc,unix,mode,why\n")
        f.write("%s,%.3f,%s,%s\n" % (utc(ts), ts, mode, why))


def read_modes(c):
    """[(unix, mode)] sorted, from modes.csv"""
    res = []
    try:
        with open(os.path.join(c["out_dir"], "modes.csv")) as f:
            for r in csv.DictReader(f):
                res.append((float(r["unix"]), r["mode"]))
    except (OSError, KeyError, ValueError):
        pass
    return sorted(res)


# ------------------------------------------------------------------ the watcher

class Watcher:
    def __init__(self, c):
        self.c = c
        os.makedirs(c["out_dir"], exist_ok=True)
        os.makedirs(c["run_dir"], exist_ok=True)
        self.alarms = Alarms(c)
        self.rt = Cyclic(c, "rt", ["--policy=fifo", "-p", str(c["rt_prio"])])
        self.other = Cyclic(c, "other", ["--policy=other"])
        self.pw_last = {}
        self.under = UnderrunTracker()
        self.since = time.time()
        self.hist = collections.defaultdict(lambda: collections.deque(maxlen=1440))   # 24 h of cost metrics
        self.recent = collections.defaultdict(lambda: collections.deque(maxlen=15))
        self.state_path = os.path.join(c["out_dir"], "state.json")
        self.state = {}
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            pass
        al = self.state.get("alarms", {})
        self.alarms.active.update(al.get("active", {}))
        self.alarms.clear.update(al.get("clear", {}))
        self.ssh_base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "ControlMaster=auto",
                         "-o", "ControlPath=%s/ssh-%%C" % c["run_dir"], "-o", "ControlPersist=10m"]
        self.load_history()

    def start_probes(self):
        label, cpus = wake_cpus(self.c)
        if label != self.state.get("wake_cpus"):     # a line when the probe CPUs change (seat cpuset, seat start/stop)
            self.alarms.emit("WAKE-CPUS probe threads on %s" % label)
            self.state["wake_cpus"] = label
        self.rt.start(cpus)
        self.other.start(cpus)

    def save_state(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.state, f)
        os.replace(tmp, self.state_path)

    def load_history(self):
        """the cost metrics of the last 24 h from our own CSVs, for the medians"""
        cut = time.time() - 86400
        for path in sorted(glob.glob(os.path.join(self.c["out_dir"], "minutes-*.csv")))[-2:]:
            try:
                with open(path) as f:
                    for r in csv.DictReader(f):
                        ts = dt.datetime.strptime(r["ts_utc"], "%Y-%m-%dT%H:%M:%SZ").replace(
                            tzinfo=dt.timezone.utc).timestamp()
                        if ts < cut:
                            continue
                        for k in self.c["alarm"]["cost_floor"]:
                            if r.get(k):
                                self.hist[k].append(float(r[k]))
            except (OSError, ValueError, KeyError):
                pass

    def mode_events(self, now):
        """the mode timeline: the kernel's switch lines, the mode after a reboot, and the minute reading"""
        mode, bid = read_mode(), boot_id()
        st = self.state
        if st.get("boot_id") != bid:
            # a new boot: its mode is an event of its own (a runtime switch does not survive a reboot)
            evs = kernel_mode_lines(0)
            first = evs[0] if evs else (now, mode)
            before = st.get("mode")
            append_mode(self.c, first[0], first[1], "boot")
            self.alarms.emit("MODE after boot: %s%s" % (first[1], (" (was %s before the reboot)" % before)
                                                         if before and before != first[1] else ""), first[0])
            for ts, m in evs[1:]:
                append_mode(self.c, ts, m, "switch")
                self.alarms.emit("MODE switched to %s" % m, ts)
            st.update({"boot_id": bid, "mode": evs[-1][1] if evs else mode,
                       "kernel_since": (evs[-1][0] + 0.001) if evs else now})
        else:
            for ts, m in kernel_mode_lines(st.get("kernel_since", now - 120)):
                if ts < st.get("kernel_since", 0):
                    continue
                append_mode(self.c, ts, m, "switch")
                self.alarms.emit("MODE switched %s -> %s" % (st.get("mode", "?"), m), ts)
                st["mode"], st["kernel_since"] = m, ts + 0.001
        if mode != "unknown" and mode != st.get("mode"):
            append_mode(self.c, now, mode, "read")
            self.alarms.emit("MODE is %s (was %s, no kernel line seen)" % (mode, st.get("mode", "?")))
            st["mode"] = mode
        self.save_state()
        return mode

    def seats(self, since):
        xr_by, ur_by, xr, ur, start, reports, down = [], [], 0, 0, 0, 0, []
        for s in self.c["seats"]:
            ct = s["ct"]
            if not ct_running(ct):
                continue
            errs = pw_errs(self.c, ct)
            self.alarms.source("ct%d-pipewire" % ct, errs is not None, "pw-top failed")
            if errs is None:
                down.append("ct%d-pw" % ct)
            else:
                inc = 0
                for k, n in errs.items():
                    key = (ct,) + k
                    if key in self.pw_last and n > self.pw_last[key]:
                        inc += n - self.pw_last[key]
                    self.pw_last[key] = n
                xr += inc
                xr_by.append("%d:%d" % (ct, inc))
                self.alarms.check("XRUN-%d" % ct, inc > 0, "XRUN ct%d: +%d PipeWire xruns in a minute" % (ct, inc), inc)
            rc, out = run(["lxc-attach", "-n", str(ct), "--", "journalctl", "-u", "rdx-hostd@%d" % s["seat"],
                           "--since", "@%d" % int(since), "--no-pager", "-o", "json",
                           "--output-fields=_PID,MESSAGE,__REALTIME_TIMESTAMP"], timeout=15)
            self.alarms.source("ct%d-journal" % ct, rc == 0, "journalctl failed")
            if rc != 0:
                down.append("ct%d-journal" % ct)
                continue
            i, st, rep = self.under.feed("ct%d" % ct, journal_underruns(out))
            ur, start, reports = ur + i, start + st, reports + rep
            ur_by.append("%d:%d" % (ct, i))
            self.alarms.check("HOSTD-UNDERRUN-%d" % ct, i > 0,
                              "UNDERRUN hostd ct%d (stream to the client): +%d in a minute" % (ct, i), i)
        return {"pw_xruns": xr, "pw_xruns_by_ct": ";".join(xr_by), "hostd_underruns": ur,
                "hostd_underruns_by": ";".join(ur_by), "hostd_audio_reports": reports}, start, down

    def clients(self, since):
        tot, by, start, reports, down = 0, [], 0, 0, []
        units = []
        for u in self.c["client_units"]:
            units += ["-u", u]
        for cl in self.c["clients"]:
            # a client reached through another container: lxc-attach (pct exec starts Perl, ~0.55 s CPU per
            # call) and a multiplexed ssh there (socket in that container's /tmp, gone with it; a fresh
            # handshake cost ~0.25 s): ~0.06 s per minute instead of ~0.8 s
            base = (cl["via"] + ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "ControlMaster=auto",
                                 "-o", "ControlPath=/tmp/rdx-pw-%C", "-o", "ControlPersist=10m"]) \
                if cl.get("via") else self.ssh_base
            cmd = base + cl["ssh"] + [" ".join(["journalctl"] + units + [
                "--since", "@%d" % int(since), "--no-pager", "-o", "json",
                "--output-fields=_PID,MESSAGE,__REALTIME_TIMESTAMP"])]
            rc, out = run(cmd, timeout=20)
            ok = rc == 0
            self.alarms.source(cl["name"], ok, "ssh/journalctl failed (rc %d)" % rc)
            if not ok:
                down.append(cl["name"])
                continue
            i, st, rep = self.under.feed(cl["name"], journal_underruns(out))
            tot, start, reports = tot + i, start + st, reports + rep
            by.append("%s:%d" % (cl["name"], i))
            self.alarms.check("CLIENT-UNDERRUN-" + cl["name"], i > 0,
                              "UNDERRUN client %s: +%d audio underruns in a minute" % (cl["name"], i), i)
        return {"client_underruns": tot, "client_underruns_by": ";".join(by), "client_audio_reports": reports}, \
            start, down

    def cost_alarms(self, row):
        a = self.c["alarm"]
        for k, floor in a["cost_floor"].items():
            v = row.get(k)
            if v is None:
                continue
            self.recent[k].append(v)
            h = sorted(self.hist[k])
            if len(h) >= 240 and len(self.recent[k]) == self.recent[k].maxlen:
                med = h[len(h) // 2]
                mean = sum(self.recent[k]) / len(self.recent[k])
                self.alarms.check("COST-" + k, mean > floor and mean > a["cost_factor"] * med,
                                  "COST %s: 15-min mean %.1f vs 24 h median %.1f (x%.2f, mode %s)" % (
                                      k, mean, med, mean / med if med else 0, row["mode"]), mean)
            self.hist[k].append(v)

    def minute(self, prev_snap, t_end, restart):
        r0 = resource.getrusage(resource.RUSAGE_SELF)
        rc0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        rt, other = self.rt.result(), self.other.result()
        if restart:                       # the next window starts at once, the rest is collected meanwhile
            self.start_probes()
        mode = self.mode_events(t_end)
        row = {"ts_utc": utc(t_end), "mode": mode}
        down = []
        for name, res in (("rt", rt), ("other", other)):
            self.alarms.source("cyclictest-" + name, res is not None, "no result")
            if res is None:
                down.append("cyclictest-" + name)
                continue
            for k in ("p50", "p99", "max", "over_1ms", "samples"):
                row["wake_%s_%s_us" % (name, k) if k in ("p50", "p99", "max") else "wake_%s_%s" % (name, k)] = res[k]
        since, self.since = self.since, time.time() - 1
        seat, st1, d1 = self.seats(since)
        cl, st2, d2 = self.clients(since)
        row.update(seat)
        row.update(cl)
        row["startup_underruns"] = st1 + st2
        down += d1 + d2
        snap = host_snapshot()
        row.update(host_delta(prev_snap, snap))
        r1 = resource.getrusage(resource.RUSAGE_SELF)
        rc1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        row["self_cpu_ms"] = 1000.0 * ((r1.ru_utime + r1.ru_stime - r0.ru_utime - r0.ru_stime)
                                       + (rc1.ru_utime + rc1.ru_stime - rc0.ru_utime - rc0.ru_stime))
        row["sources_down"] = ";".join(down)
        a = self.c["alarm"]
        if rt:
            self.alarms.check("WAKE-RT", rt["max"] > a["wake_rt_max_ms"] * 1000,
                              "WAKE-RT max %.2f ms > %.1f ms (SCHED_FIFO %d, p99 %.3f ms, mode %s)" % (
                                  rt["max"] / 1000, a["wake_rt_max_ms"], self.c["rt_prio"], rt["p99"] / 1000, mode),
                              rt["max"])
        if other:
            self.alarms.check("WAKE-OTHER", other["max"] > a["wake_other_max_ms"] * 1000,
                              "WAKE-OTHER max %.2f ms > %.1f ms (SCHED_OTHER, p99 %.3f ms, mode %s)" % (
                                  other["max"] / 1000, a["wake_other_max_ms"], other["p99"] / 1000, mode),
                              other["max"])
        self.cost_alarms(row)
        return row, snap

    def vm_lines(self, row):
        h = self.c["host_label"]
        main = {k: row.get(k) for k in CSV_COLS if k not in ("ts_utc", "mode") and not isinstance(row.get(k), str)}
        main["mode_code"] = MODES.index(row["mode"]) if row["mode"] in MODES else -1
        lines = ["rdx_preempt,host=%s,mode=%s %s" % (h, row["mode"], influx_fields(main))]
        for part in row.get("pw_xruns_by_ct", "").split(";"):
            if ":" in part:
                ct, n = part.split(":")
                lines.append("rdx_preempt_seat,host=%s,ct=%s pw_xruns=%si" % (h, ct, n))
        for part in row.get("hostd_underruns_by", "").split(";"):
            if ":" in part:
                ct, n = part.split(":")
                lines.append("rdx_preempt_seat,host=%s,ct=%s hostd_underruns=%si" % (h, ct, n))
        for part in row.get("client_underruns_by", "").split(";"):
            if ":" in part:
                name, n = part.rsplit(":", 1)
                lines.append("rdx_preempt_client,host=%s,client=%s audio_underruns=%si" % (h, name, n))
        return lines

    def loop(self, once=False):
        self.mode_events(time.time())
        if not once and self.c.get("vm_write"):
            log("[watch] mode timeline backfilled into the metrics database: %d points" % backfill_modes(self.c, time.time()))

        def to_boundary():
            now = time.time()
            time.sleep(max((int(now) // 60 + 1) * 60 - now, 0))
        if not once:
            to_boundary()
        snap = host_snapshot()
        self.since = time.time() - 1
        self.start_probes()
        while True:
            if once:
                time.sleep(self.c["window_s"] + 1)
            else:
                to_boundary()
            row, snap = self.minute(snap, time.time(), restart=not once)
            if once:
                print(json.dumps(row, indent=1))
                return
            write_csv(self.c, row)
            self.alarms.source("victoriametrics", push_vm(self.c, time.time(), self.vm_lines(row)), "write failed")
            self.state["alarms"] = {"active": self.alarms.active, "clear": dict(self.alarms.clear)}
            self.save_state()


# ------------------------------------------------------------------ daily comparison

def vm_range(c, query, start, end, step=60):
    if not c.get("vm_query"):
        return []
    q = urllib.parse.urlencode({"query": query, "start": int(start), "end": int(end), "step": step})
    try:
        with urllib.request.urlopen(c["vm_query"] + "/api/v1/query_range?" + q, timeout=60) as r:
            res = json.load(r)["data"]["result"]
    except (OSError, ValueError, KeyError):
        return []
    out = []
    for s in res:
        out += [(float(t), float(v)) for t, v in s["values"]]
    return out


def mode_at_fn(c):
    """mode of a unix time: the watcher's timeline; before its first entry the kernel default
    (CONFIG_PREEMPT_* of the running kernel, no preempt= on the command line), marked as assumed"""
    tl = read_modes(c)
    default = "voluntary"
    try:
        with open("/boot/config-%s" % os.uname().release) as f:
            cfg = f.read()
        # the build's default model (with PREEMPT_DYNAMIC the boot mode unless preempt= says otherwise)
        for opt, m in (("CONFIG_PREEMPT_VOLUNTARY=y", "voluntary"), ("CONFIG_PREEMPT_NONE=y", "none"),
                       ("CONFIG_PREEMPT_LAZY=y", "lazy"), ("CONFIG_PREEMPT=y", "full")):
            if opt in cfg:
                default = m
                break
        with open("/proc/cmdline") as f:
            cmd = f.read()
        m = re.search(r"\bpreempt=(\w+)", cmd)
        if m:
            default = m.group(1)
    except OSError:
        pass
    ts = [t for t, _ in tl]

    def f(t):
        i = bisect.bisect_right(ts, t) - 1
        return tl[i][1] if i >= 0 else default + "*"
    return f, default


def summarize(vals):
    if not vals:
        return None
    v = sorted(vals)
    return {"n": len(v), "med": v[len(v) // 2], "p95": v[min(len(v) - 1, int(0.95 * len(v)))], "max": v[-1]}


def report(c, date, days):
    day0 = dt.datetime.strptime(date, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc).timestamp()
    end, start = day0 + 86400, day0 + 86400 - days * 86400
    mode_of, default = mode_at_fn(c)
    inst = c["node_instance"]
    cost_q = {
        "cpu_sys_pct": 'sum(rate(node_cpu_seconds_total{instance="%s",mode="system"}[2m]))/'
                       'sum(rate(node_cpu_seconds_total{instance="%s"}[2m]))*100' % (inst, inst),
        "cpu_softirq_pct": 'sum(rate(node_cpu_seconds_total{instance="%s",mode="softirq"}[2m]))/'
                           'sum(rate(node_cpu_seconds_total{instance="%s"}[2m]))*100' % (inst, inst),
        "cpu_irq_pct": 'sum(rate(node_cpu_seconds_total{instance="%s",mode="irq"}[2m]))/'
                       'sum(rate(node_cpu_seconds_total{instance="%s"}[2m]))*100' % (inst, inst),
        "ctxt_per_s": 'rate(node_context_switches_total{instance="%s"}[2m])' % inst,
        "intr_per_s": 'rate(node_intr_total{instance="%s"}[2m])' % inst,
        "load1": 'node_load1{instance="%s"}' % inst,
        "psi_cpu_some_pct": 'rate(node_pressure_cpu_waiting_seconds_total{instance="%s"}[2m])*100' % inst,
        "zfs_rw_mbs": 'sum(rate(node_zfs_zpool_dataset_nread{instance="%s"}[2m])+'
                      'rate(node_zfs_zpool_dataset_nwritten{instance="%s"}[2m]))/1e6' % (inst, inst),
    }
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for k, q in (cost_q.items() if inst else ()):
        for t, v in vm_range(c, q, start, end):
            by[k][mode_of(t).rstrip("*")].append(v)
    # benefit from our own rows (wake-up latency, xruns) and the journals (underruns)
    for path in sorted(glob.glob(os.path.join(c["out_dir"], "minutes-*.csv"))):
        try:
            with open(path) as f:
                for r in csv.DictReader(f):
                    ts = dt.datetime.strptime(r["ts_utc"], "%Y-%m-%dT%H:%M:%SZ").replace(
                        tzinfo=dt.timezone.utc).timestamp()
                    if not start <= ts < end:
                        continue
                    for k in ("wake_rt_p99_us", "wake_rt_max_us", "wake_other_p99_us", "wake_other_max_us", "pw_xruns"):
                        if r.get(k):
                            by[k][r["mode"]].append(float(r[k]))
        except (OSError, ValueError, KeyError):
            pass
    under = collections.defaultdict(lambda: collections.Counter())   # source -> mode -> underruns / reports
    ssh_base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
    units = []
    for u in c["client_units"]:
        units += ["-u", u]
    sources = [(cl["name"], cl.get("via", []) + ssh_base + cl["ssh"] + [" ".join(["journalctl"] + units + [
        "--since", "@%d" % int(start), "--until", "@%d" % int(end), "--no-pager", "-o", "json",
        "--output-fields=_PID,MESSAGE,__REALTIME_TIMESTAMP"])]) for cl in c["clients"]]
    for s in c["seats"]:
        if ct_running(s["ct"]):
            sources.append(("ct%d" % s["ct"], ["lxc-attach", "-n", str(s["ct"]), "--", "journalctl", "-u",
                            "rdx-hostd@%d" % s["seat"], "--since", "@%d" % int(start), "--until", "@%d" % int(end),
                            "--no-pager", "-o", "json", "--output-fields=_PID,MESSAGE,__REALTIME_TIMESTAMP"]))
    unreachable = []
    for name, cmd in sources:
        rc, out = run(cmd, timeout=120)
        if rc != 0:
            unreachable.append(name)
            continue
        state = {}
        for ts, pid, tag, secs, n in journal_underruns(out):
            if tag not in AUDIO_TAGS:
                continue
            m = mode_of(ts).rstrip("*")
            new, dn, ds = stream_step(state, (pid, tag), secs, n)
            if new:
                if secs is not None:
                    under[name][m + ":streams"] += 1
            else:                                  # between two reports of ONE stream
                under[name][m + ":u"] += dn
                under[name][m + ":s"] += ds

    def fmt(s, unit=""):
        return "-" if not s else "med %.2f%s p95 %.2f%s max %.2f%s (n %d)" % (
            s["med"], unit, s["p95"], unit, s["max"], unit, s["n"])

    out = ["rdx-preempt-watch: %s, window %s .. %s (%d day(s)); before the watcher's first mode entry the mode "
           "is assumed %s (kernel default, no preempt= on the command line)" % (date, utc(start), utc(end), days, default),
           "", "%-22s %-48s %-48s %s" % ("metric", "voluntary", "full", "full vs voluntary (median)")]
    for k in list(cost_q) + ["wake_rt_p99_us", "wake_rt_max_us", "wake_other_p99_us", "wake_other_max_us", "pw_xruns"]:
        v, f = summarize(by[k].get("voluntary", [])), summarize(by[k].get("full", []))
        d = ("%+.1f %%" % (100.0 * (f["med"] - v["med"]) / v["med"])) if v and f and v["med"] else "-"
        out.append("%-22s %-48s %-48s %s" % (k, fmt(v), fmt(f), d))
    out.append("")
    out.append("audio underruns per streamed hour (journals: counter increments between two reports of one "
               "process, over the seconds between them; the first report of a stream is its start-up):")
    for name in sorted(under):
        cells = []
        for m in ("voluntary", "full"):
            u, s = under[name][m + ":u"], under[name][m + ":s"]
            cells.append("%s %s" % (m, ("%.1f/h (%d in %.2f h, %d streams)" % (
                u / (s / 3600.0), u, s / 3600.0, under[name][m + ":streams"])) if s else "no stream"))
        out.append("  %-12s %s" % (name, " | ".join(cells)))
    if unreachable:
        out.append("  not reachable: %s" % ", ".join(unreachable))
    out += ["", "Caveat: the load differs between the windows (games, other GPU jobs, backups, development and test "
            "sessions with their own audio faults); host costs and underruns are only comparable at similar use. "
            "Wake-up latency and xruns exist only for minutes this watcher ran."]
    text = "\n".join(out) + "\n"
    path = os.path.join(c["out_dir"], "report-%s.txt" % date)
    with open(path, "w") as f:
        f.write(text)
    sys.stdout.write(text)
    # one line into the alarm channel
    tl = []
    for k in ("cpu_sys_pct", "cpu_softirq_pct", "ctxt_per_s"):
        v, f = summarize(by[k].get("voluntary", [])), summarize(by[k].get("full", []))
        if v and f and v["med"]:
            tl.append("%s %+.0f%%" % (k, 100.0 * (f["med"] - v["med"]) / v["med"]))
    for name in sorted(under):
        ru = ["%.1f" % (under[name][m + ":u"] / (under[name][m + ":s"] / 3600.0)) if under[name][m + ":s"] else "-"
              for m in ("voluntary", "full")]
        tl.append("%s underruns/h %s->%s" % (name, ru[0], ru[1]))
    Alarms(c).emit("REPORT %s: %s (%s)" % (date, "; ".join(tl) or "no comparable data", path))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=("run", "once", "report"))
    ap.add_argument("--config", default="/etc/rdx/preempt-watch.json")
    ap.add_argument("--date", default=(dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=1)).strftime("%Y-%m-%d"))
    ap.add_argument("--days", type=int, default=7)
    a = ap.parse_args()
    c = load_conf(a.config)
    if a.cmd == "report":
        os.makedirs(c["out_dir"], exist_ok=True)
        report(c, a.date, a.days)
        return
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    Watcher(c).loop(once=a.cmd == "once")


if __name__ == "__main__":
    main()
