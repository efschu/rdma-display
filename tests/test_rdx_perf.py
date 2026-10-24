#!/usr/bin/env python3
"""tools/rdx_perf.py without hardware: report parsing, slot rule, key -> frame pairing, table."""
import io
import os
import sys
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
sys.dont_write_bytecode = True
import rdx_perf as P  # noqa: E402

GS = """[gs] done: 4363 frames in 19.3 s = 226.6 fps, shown 4307, dropped 55, vblank anchors 4307, lead 8.524 ms (target wait 3.50 ms), recoveries 0 (flushed WRs 0), refused posts 0
  sink arrival -> vblank (ms) n=4307  p50=   3.497  p90=   3.742  p99=   4.269  max=   6.058 ms
  post -> shown report (ms) n=4307  p50=   5.998  p90=   6.258  p99=   6.832  max=   9.057 ms
[shading] shading=phong: FPS: 7000 FrameTime: 0.143 ms
[effect2d] kernel=blur: FPS: 8000 FrameTime: 0.125 ms
                                   vkmark Score: 7500
"""
SINK = """[null] 20.0 s: received 4363 (218.3/s), shown 4307 (215.5/s), dropped 55, vblanks 4796
[null] payload: 1810.8 MB/s = 14.49 Gbit/s (4363 frames of 7680 x 1080 B)
  CQE -> vblank (ms)       n=4307  p50=   3.497  p90=   3.743  p99=   4.269  max=   6.058 ms
  NIC CQE -> vblank (ms)   n=4307  p50=   3.517  p90=   3.761  p99=   4.292  max=   6.081 ms
[watch] arrive seq=1 bright=0 luma=0 sw_ns=1000 hw_ns=900
[watch] shown seq=1 bright=0 vblank_ns=5000
[watch] arrive seq=50 bright=1 luma=255 sw_ns=20000000 hw_ns=19990000
[watch] shown seq=50 bright=1 vblank_ns=23000000
[watch] arrive seq=90 bright=0 luma=0 sw_ns=230000000 hw_ns=0
[watch] shown seq=90 bright=0 vblank_ns=234000000
"""


def main():
    g = P.parse_gs(GS)
    assert g['frames'] == 4363 and abs(g['fps'] - 226.6) < 1e-9 and g['dropped'] == 55 and g['lead'] == 8.524, g
    assert g['reports']['post -> shown report (ms)']['p50'] == 5.998
    assert g['app_fps'] == 7500 and g['score'] == 7500, g          # mean of the scene FPS lines
    s = P.parse_sink(SINK)
    assert s['received'] == 4363 and s['shown'] == 4307 and s['payload_gbit'] == 14.49, s
    assert s['reports']['NIC CQE -> vblank (ms)']['p99'] == 4.292
    assert len(s['arrive']) == 3 and len(s['shown_ev']) == 3
    # common/rdx_ctl.h rdx_pace_slots with the 8 ms default lead (tests/test_pace_slots.c has the same table)
    assert [P.pace_slots(hz) for hz in (60, 144, 240)] == [3, 3, 4]
    # key 1 at 10 ms -> frame at the NIC 19.99 ms, shown 23 ms; key 2 at 200 ms -> sw clock 230 ms (no hw);
    # key 3 at 300 ms: nothing after it -> a miss
    la, ls, miss = P.pair_latency([10_000_000, 200_000_000, 300_000_000], s['arrive'], s['shown_ev'])
    assert [round(x, 2) for x in la] == [9.99, 30.0] and [round(x, 2) for x in ls] == [13.0, 34.0] and miss == 1, (la, ls)
    assert P.parse_ctr('c0 100 10.0\nc1 1250000100 20.0\n') == 4.0      # 4-byte words -> Gbit/s
    # the cursor next to the picture (rdx_sink_null --watch-cursor): positions and their vblanks, merged with the
    # changed frames into "the first pointer update after a motion"
    c = P.parse_sink(SINK + "[watch] cursor x=-4 y=10 visible=1 sw_ns=12000000 hw_ns=11500000\n"
                     "[watch] cursor-shown vblank_ns=14000000\n"
                     "[null] cursor next to the picture: 1 position changes, 1 pictures (0 invalid messages)\n")
    assert c['cursor'] == [(0, 12000000, 11500000)] and c['cursor_shown'] == [(0, 14000000)], c['cursor']
    assert c['cursor_pos'] == 1 and c['cursor_img'] == 1
    arr = sorted(c['arrive'] + c['cursor'], key=lambda e: e[2] or e[1])
    sh = sorted(c['shown_ev'] + c['cursor_shown'], key=lambda e: e[1])
    la2, ls2, _ = P.pair_latency([10_000_000], arr, sh)
    assert [round(x, 2) for x in la2] == [1.5] and [round(x, 2) for x in ls2] == [4.0], (la2, ls2)
    res = [{'source': 'seat2', 'sink': 'null@x', 'mode': '1920x1080@240', 'load': 'flash', 'slots': 4, 'gs': g,
            'sink_r': s, 'nic_gbit': 14.7, 'lat': {'keys': 3, 'arrive': la, 'shown': ls, 'miss': 1}}]
    out = io.StringIO()
    with redirect_stdout(out):
        P.table(res)
    t = out.getvalue()
    assert '223.2' in t and '3.52 / 4.29' in t and '14.70 / 14.49' in t and '1 without a changed frame' in t, t
    print('test_rdx_perf: ok')


if __name__ == '__main__':
    main()
