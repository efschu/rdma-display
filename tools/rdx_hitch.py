#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Explain display hitches from rdma-display telemetry.

  rdx_hitch.py <kms-sink.csv> [<mirror-src.csv>] [--context N]

A hitch is a flip gap above the running mean gap + half a vblank on the sink (H lines).
For the frame shown after the gap and the frame before it, the script lines up
both sides by sequence number and names the first stage that ran late:

  src-late-wake   the source woke more than 1 ms after its post deadline
  src-slot-wait   the source waited > 1 ms for a free slot (sink held them)
  src-capture     capture + draw took > 2x its median
  src-gap         the source posted the frame > 1.5 intervals after the previous one
  network         post -> report minus sink time > median + 1 ms
  sink-late-flip  the frame arrived in time but its flip landed a vblank later
  sink-dropped    a newer frame replaced it before its flip (drop)
  unexplained     nothing on either side stood out (look at the raw lines)

Times are each host's CLOCK_MONOTONIC; the "# clock" anchor converts them to wall
clock for the report. Only standard library.
"""
import argparse
import datetime
import statistics
import sys


# fields after the type letter; anything shorter is a partial line (log flushed mid-line)
NFIELDS = {"R": 3, "D": 2, "V": 6, "H": 4, "F": 13, "S": 5}
FLIP_LEAD_NS = 2_000_000   # rdx_sink_kms --flip-lead-us default


def read(path):
    anchor = None
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith("# clock"):
                kv = dict(p.split("=") for p in line.split()[2:])
                anchor = (int(kv["mono_ns"]), int(kv["real_ns"]))
                continue
            if not line or line[0] == "#" or "," not in line:
                continue
            p = line.rstrip("\n").split(",")
            if p[1] == "seq" or len(p) - 1 < NFIELDS.get(p[0], 0):
                continue          # header lines, partial last line
            rows.setdefault(p[0], []).append(p[1:])
    return anchor, rows


def wall(anchor, mono):
    if not anchor:
        return "?"
    ns = anchor[1] + (mono - anchor[0])
    return datetime.datetime.fromtimestamp(ns / 1e9).strftime("%H:%M:%S.%f")[:-3]


def median(v):
    return statistics.median(v) if v else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sink")
    ap.add_argument("src", nargs="?")
    ap.add_argument("--context", type=int, default=0, help="print N raw frames around each hitch")
    a = ap.parse_args()

    s_anchor, S = read(a.sink)
    V = {int(r[0]): r for r in S.get("V", [])}            # seq -> t_cqe,t_flip_req,t_vblank,gap,cqe_vbl
    R = {int(r[0]): int(r[2]) for r in S.get("R", [])}    # seq -> t_cqe
    NIC = {int(r[0]): int(r[3]) for r in S.get("R", []) if len(r) > 3 and int(r[3])}  # seq -> NIC arrival
    dropped = {int(r[0]) for r in S.get("D", [])}
    H = [(int(r[0]), int(r[1]), float(r[2]), float(r[3])) for r in S.get("H", [])]
    gaps = [float(r[4]) for r in V.values() if float(r[4]) > 0]
    period = median(gaps) * 1e3 if gaps else 0          # ns, normal flip interval

    F, SR, src_anchor = {}, {}, None
    if a.src:
        src_anchor, C = read(a.src)
        for r in C.get("F", []):
            F[int(r[0])] = [int(x) if i < 8 else x for i, x in enumerate(r[1:])]
        for r in C.get("S", []):
            if r[4] == "1":
                SR[int(r[0])] = (float(r[2]), float(r[3]))   # round_us, sink_us
    caps = [(f[5] - f[4]) for f in F.values()]
    cap_med = median(caps)
    nets = [rt - sk for rt, sk in SR.values()]
    net_med = median(nets)
    posts = sorted((seq, f[6]) for seq, f in F.items())
    post_of = dict(posts)
    interval = median([b[1] - a_[1] for a_, b in zip(posts, posts[1:])]) if len(posts) > 1 else period

    print(f"sink: {len(V)} flips, {len(H)} hitches, {len(dropped)} drops, normal flip gap {period / 1e6:.3f} ms")
    if a.src:
        print(f"src:  {len(F)} frames, capture median {cap_med / 1e6:.3f} ms, network median {net_med / 1e3:.3f} ms, "
              f"post interval median {interval / 1e6:.3f} ms")
    if NIC:
        wakes = sorted(R[s] - NIC[s] for s in NIC if s in R)
        print(f"sink wake-up (NIC arrival -> process saw it): p50 {wakes[len(wakes) // 2] / 1e3:.1f} us, "
              f"p99 {wakes[int(len(wakes) * 0.99)] / 1e3:.1f} us, max {wakes[-1] / 1e3:.1f} us")
    causes = {}
    for seq, t_vb, gap_us, mean_us in H:
        why = []
        # sink timeline of the frame shown after the gap: which step missed the vblank it was due for?
        v = V.get(seq)
        if v and seq in NIC and period:
            t_cqe, t_req, t_vbl = int(v[1]), int(v[2]), int(v[3])     # v = seq,t_cqe,t_req,t_vblank,...
            expected = t_vbl - int(gap_us * 1e3) + int(mean_us * 1e3)    # vblank it should have made
            deadline = expected - FLIP_LEAD_NS                          # late flip requests from here on
            t_nic = NIC[seq]
            if t_nic > deadline:
                why.append(f"arrived-late(seq {seq}: NIC arrival {(t_nic - deadline) / 1e6:.2f} ms after deadline)")
            elif t_cqe > deadline:
                why.append(f"sink-late-wake(seq {seq}: arrived {(deadline - t_nic) / 1e6:.2f} ms early, "
                           f"seen {(t_cqe - t_nic) / 1e6:.2f} ms later)")
            elif t_req > expected - 500_000:
                why.append(f"sink-late-flip-request(seq {seq}: requested {(t_req - deadline) / 1e6:.2f} ms after "
                           f"deadline)")
            elif t_vbl > expected + period // 2:
                why.append(f"drm-flip-late(seq {seq}: requested {(expected - t_req) / 1e6:.2f} ms before vblank, "
                           f"shown one vblank later)")
        for s in (seq - 1, seq):
            f = F.get(s)
            if f:
                t_due, t_wake, t_credit, t_cap0, t_cap1, t_post = f[1], f[2], f[3], f[4], f[5], f[6]
                if t_due and t_wake - t_due > 1_000_000:
                    why.append(f"src-late-wake(seq {s}: +{(t_wake - t_due) / 1e6:.2f} ms)")
                if t_credit - t_wake > 1_000_000:
                    why.append(f"src-slot-wait(seq {s}: {(t_credit - t_wake) / 1e6:.2f} ms)")
                if cap_med and t_cap1 - t_cap0 > 2 * cap_med:
                    why.append(f"src-capture(seq {s}: {(t_cap1 - t_cap0) / 1e6:.2f} ms)")
                prev = post_of.get(s - 1)
                if prev and interval and t_post - prev > 1.5 * interval:
                    why.append(f"src-gap(seq {s}: {(t_post - prev) / 1e6:.2f} ms)")
            sr = SR.get(s)
            if sr and net_med and (sr[0] - sr[1]) - net_med > 1000:
                why.append(f"network(seq {s}: {(sr[0] - sr[1]) / 1e3:.2f} ms)")
            if s in dropped:
                why.append(f"sink-dropped(seq {s})")
        if not why and v and period and float(v[5]) * 1e3 > 1.5 * period:
            why.append(f"sink-late-flip(seq {seq}: CQE->vblank {float(v[5]) / 1e3:.2f} ms)")
        if not why:
            why.append("unexplained")
        key = why[0].split("(")[0]
        causes[key] = causes.get(key, 0) + 1
        print(f"{wall(s_anchor, t_vb)}  hitch seq {seq}: gap {gap_us / 1e3:.2f} ms (mean {mean_us / 1e3:.2f}) -> "
              + "; ".join(why))
        if a.context:
            for s in range(seq - a.context, seq + a.context + 1):
                print(f"      seq {s}: src F={F.get(s)} S={SR.get(s)} | sink R={R.get(s)} V={V.get(s)}"
                      f"{' DROPPED' if s in dropped else ''}")
    if causes:
        print("causes: " + ", ".join(f"{k} {v}" for k, v in sorted(causes.items(), key=lambda x: -x[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
