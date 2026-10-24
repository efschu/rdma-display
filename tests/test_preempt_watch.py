#!/usr/bin/env python3
"""tools/rdx-preempt-watch.py, the parts that need no multiseat host: the per-STREAM underrun counters (one
rdx-hostd / rdx-clientd process carries many streams, each starting again at 10 s and a low count)
and the mode timeline backfill.

  tests/test_preempt_watch.py             exit 0 = PASS
"""
import importlib.util, os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rpw", os.path.join(HERE, "..", "tools", "rdx-preempt-watch.py"))
rpw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rpw)
fails = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails.append(what)


A = "audio/arx"
# stream 1 of pid 7 ends at 1516 underruns; stream 2 of the SAME process starts again at 10 s with 6, ends with 11
rows = [(1, "7", A, 10, 57), (2, "7", A, 20, 90), (3, "7", A, 30, 126),     # stream 1: start-up 57, then +69
        (4, "7", A, None, 130),                                             # its "done" line: +4
        (5, "7", A, 10, 6), (6, "7", A, 20, 8), (7, "7", A, 30, 11)]        # stream 2: start-up 6, then +5
t = rpw.UnderrunTracker()
inc, start, rep = t.feed("x", rows)
check(inc == 69 + 4 + 5, "increments of both streams counted, none lost to the old maximum (got %d)" % inc)
check(start == 57 + 6, "start-up of both streams (got %d)" % start)
check(rep == len(rows), "reports")

# a stream first seen mid-life only sets the baseline
t = rpw.UnderrunTracker()
inc, start, rep = t.feed("x", [(1, "9", A, 380, 400), (2, "9", A, 390, 410)])
check((inc, start) == (10, 0), "mid-life stream: baseline, then +10 (got %s)" % ((inc, start),))

# the counters survive a feed boundary (one feed per minute)
t = rpw.UnderrunTracker()
t.feed("x", [(1, "5", A, 10, 3), (2, "5", A, 20, 5)])
inc, start, _ = t.feed("x", [(3, "5", A, 30, 9)])
check((inc, start) == (4, 0), "increment across two feeds (got %s)" % ((inc, start),))

# a counter that went down without a seconds reset is a new stream too
st = {}
rpw.stream_step(st, "k", 100, 50)
new, dn, ds = rpw.stream_step(st, "k", 110, 2)
check(new and dn == 0 and ds == 0, "count went down: new stream")

# other tags (the microphone) never count
t = rpw.UnderrunTracker()
inc, start, rep = t.feed("x", [(1, "5", "mic/atx", 10, 3), (2, "5", "mic/atx", 20, 9)])
check((inc, start, rep) == (0, 0, 0), "mic stream ignored")

# modes: the backfill writes one point per minute from modes.csv, the right mode at each minute
posted = []


class FakeResp:
    status = 204

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


rpw.urllib.request.urlopen = lambda req, timeout=0: (posted.append(req.data.decode()), FakeResp())[1]
with tempfile.TemporaryDirectory() as d:
    with open(os.path.join(d, "modes.csv"), "w") as f:
        f.write("ts_utc,unix,mode,why\n"
                "x,6000.0,voluntary,boot\nx,6300.0,full,switch\nx,6420.0,voluntary,switch\n")
    c = rpw.load_conf("/nonexistent")
    c["out_dir"] = d
    # the default: no metrics database, nothing is written and nothing breaks
    check(rpw.backfill_modes(c, 6600.0) == 0 and rpw.push_vm(c, 6600.0, ["x y=1i"]) is True and not posted
          and rpw.vm_range(c, "up", 0, 60) == [], "vm_write / vm_query \"\" = off")
    c["vm_write"] = "http://metrics.example:8428/write"
    n = rpw.backfill_modes(c, 6600.0)
    lines = "\n".join(posted).splitlines()
    byts = {int(l.rsplit(" ", 1)[1]) // 10**9: l for l in lines}
    check(n == len(lines) == 9, "nine minute points from 6060 to 6540 (got %d)" % n)
    check("mode=voluntary mode_code=1i" in byts[6240] and "mode=full mode_code=2i" in byts[6300]
          and "mode=full" in byts[6360] and "mode=voluntary" in byts[6420], "mode at each minute")

print("PASS" if not fails else "FAIL: %d" % len(fails))
sys.exit(1 if fails else 0)
