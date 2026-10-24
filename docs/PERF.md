# Measuring a stream: tools/rdx_perf.py

One command runs a matrix of real streams (source x sink x mode x load) and prints one table; every
number comes from the programs' own reports (rdx_gs_src, rdx_sink_null, the apps).

```
tools/rdx_perf.py --source seat2 --sink null@seat2-sink \
    --modes 1920x1080@240,2560x1440@144,3840x2160@60 --loads vkcube,vkmark,glmark2
tools/rdx_perf.py --source seat2 --sink null@seat2-sink --modes 1920x1080@240 \
    --loads flash,flash-novsync,flash-idle --latency --repeat 2
```

- **Sources**: a seat container on a Proxmox multiseat host (`seat1`, `seat2`: `ssh pve-host pct exec
  CT --`) or a desktop PC (`host`). On the source: `rdx_gs_src --buffers host` + gamescope + the app,
  20 s per cell.
- **Sinks**: `null@HOST` (`gs/rdx_sink_null`, no display, a timer at the mode's refresh), `win@HOST`
  (`p0b/rdx_sink_win`, a window on the client's desktop), `kms@HOST` (`p0b/rdx_sink_kms`, the client's
  screen). `null@seat2-sink`: the sink in the seat's own container, RoCE loopback inside the multiseat
  host's NIC (a macvlan cannot reach the Proxmox host's own IP), so a run needs no other computer.
- **Loads**: `vkcube` (FIFO), `vkmark` (Vulkan, `effect2d` blur at full screen, present mode immediate:
  the GPU runs flat out next to gamescope), `glmark2` (GL, `terrain`), `flash` / `flash-novsync` /
  `flash-idle` (`tools/rdx_flash`, the latency probe: a game loop with vsync, with vsync off, and an app
  that draws only on input).
- Run it on a computer that reaches every host of the profile by ssh (BatchMode, keys set up). The
  source's GPU must be free for the run, and a seat container must run with `"picture": "stream"`.
- A sink on a computer whose NIC carries someone's stream shares that NIC, its PCIe link and the
  memory bandwidth with it: run only when the client is free (but see "Gaps in a concurrent stream"
  below: no measurable effect was found).

## The profile (hosts, links, GPUs, binaries)

The built-in `PROFILE` in the script is an EXAMPLE with neutral names: a multiseat Proxmox host reached
as the ssh alias `pve-host` (seats in CT 3101 / 3102, link IPs 10.0.1.11 / .12), a desktop PC
`root@host.example` (10.0.0.2 towards the client, 10.0.1.2 towards the seats) and a client
`root@client.example` (10.0.0.1). `--config FILE` merges a JSON file with the same structure over it
(dicts are merged key by key, everything else is replaced), `--bin NAME=PATH` replaces one binary:

```json
{
  "hosts": {
    "desk":   {"ssh": ["ssh", "-o", "BatchMode=yes", "root@desk.lan"], "workdir": "/var/tmp/rdx-perf",
               "wrap": "systemd-run --scope -q -p MemoryMax=2G nice -n 19", "max_s": 30},
    "laptop": {"ssh": ["ssh", "-o", "BatchMode=yes", "root@laptop.lan"], "wrap": "", "workdir": "/var/tmp/rdx-perf"}
  },
  "sources": {
    "desk": {"host": "desk", "gpu": "0000:01:00.0", "ip": "10.0.0.2", "peers": {"laptop": "10.0.0.1"}}
  },
  "user": "alice", "uid": 1000
}
```

- `hosts.NAME`: `ssh` (argv up to the target), optional `exec` (a prefix inside the target, e.g.
  `["pct", "exec", "3101", "--"]` for a container), `workdir`, `wrap` (a command prefix for the test
  processes: a lock, a memory scope, nice) and `max_s` (a hard cap of each test process in seconds).
- `sources.NAME`: the host that runs it, the GPU's PCI address, its IP, and `peers` (sink host -> the
  sink's IP on that link; a sink without an entry is skipped).
- `user` / `uid`: the desktop user gamescope runs as on the source; `ports`, `bins`, `kms_card`: see
  the script.

## End-to-end latency (--latency)

```
client: rdx_keyinject.py --uinput--> rdx_input_tx --RDMA--> rdx_input_rx (source) --uinput "rdx: ..."-->
        gamescope (libinput) --> Xwayland --> rdx_flash (black <-> white) --> gamescope composite + copy
        --> rdx_gs_src --RDMA--> rdx_sink_null --watch X,Y: first frame of the new colour
```

- `tools/rdx_keyinject.py` creates a uinput keyboard, prints its node, and after the go file presses
  Left Shift N times at random intervals (120-280 ms, every phase of the vblank), one line with the
  CLOCK_MONOTONIC time per press. `rdx_input_tx --input <node>` grabs it and sends it like a physical
  keyboard (the whole real input path).
- `tools/rdx_flash` (X11/GLX, fullscreen in gamescope) toggles black <-> white on every key press.
- `rdx_sink_null --watch X,Y` (default: the frame centre) prints the arrival (software and NIC CQE
  clock) of each frame whose 16x16-box luma crosses dark <-> bright, and the vblank that shows it.
- The key and the frame are timed on the same machine's CLOCK_MONOTONIC (client; for a sink in the
  seat container the keys come from that container: the same kernel clock). No clock sync is needed.
  Reported: key -> changed frame at the sink's NIC, key -> the vblank that shows it; the beam reaches
  the frame centre half a period later.
- Never a test keyboard on the Proxmox host itself: its kernel VT handler would take the keys (the
  tool puts the keys into the source container when the sink is internal to the multiseat host).

## Pointer motion (the cursor next to the picture, docs/GAMESCOPE.md)

```
tools/rdx_perf.py --source seat2 --sink null@seat2-sink --modes 2560x1440@144 --loads cursor,cursor30 --cursor picture
tools/rdx_perf.py --source seat2 --sink null@seat2-sink --modes 2560x1440@144 --loads cursor,cursor30 --cursor auto
```

- `cursor`: `rdx_flash --idle --cursor` (black, a white 16x16 block cursor, never draws) and
  `tools/rdx_keyinject.py --mouse`, a uinput mouse that moves to and fro (one motion of `--step`
  counts every 120-280 ms) through `rdx_input_tx` like a physical one. `rdx_sink_null --watch-cursor`
  reports each frame whose sampled picture changed (`[watch] arrive`: the drawn cursor moved) and each
  position on the cursor channel (`[watch] cursor`, `[watch] cursor-shown` at the next vblank).
  Reported as for keys: motion -> the first pointer update at the sink, -> the vblank showing it.
- `cursor30`: `rdx_flash --swap-interval 0 --fps 30 --cursor`, the mouse moving ~30 times per second the
  whole run. `gs fps` is what the source sent: with the cursor drawn into the frames and variable
  refresh it follows the mouse up to the cap, next to the picture it stays at the app's 30.
- `--cursor picture` is the sink as an older one (the source draws the cursor), `auto` the channel; the
  two runs are the before and after. Also for `win@` and `kms@` sinks (gs fps; the latency needs the
  null sink).

## Measured

Seat 2 of a multiseat host (CT 3102, RTX 3080 at `18:00.0`), host buffers, sink in the same container
(`null@seat2-sink`, RoCE loopback in the host's ConnectX-4 at PCIe Gen3 x4 behind the chipset), 20 s
per cell, sink slots by `rdx_pace_slots` (8 ms lead), the 3080 otherwise idle.

| mode | load | app fps | gs fps | shown/s | drops | slots | arrival -> vblank p50 / p99 ms | composite p50 ms | post -> shown p50 ms | payload Gbit/s |
|---|---|---|---|---|---|---|---|---|---|---|
| 1920x1080@240 | vkcube | 223.5 | 223.5 | 219.0 | 85 | 4 | 3.51 / 4.60 | 0.78 | 6.08 | 14.31 |
| 1920x1080@240 | vkmark (score 7782) | 7782 | 230.8 | 229.5 | 15 | 4 | 3.51 / 4.15 | 0.58 | 6.08 | 14.71 |
| 1920x1080@240 | glmark2 terrain | 1270 | 230.9 | 230.5 | 9 | 4 | 3.53 / 5.05 | 0.76 | 6.10 | 14.80 |
| 2560x1440@144 | vkcube | 138.3 | 138.3 | 137.6 | 11 | 3 | 3.49 / 7.00 | 1.12 | 8.05 | 15.74 |
| 2560x1440@144 | vkmark (score 5380) | 5380 | 138.8 | 138.4 | 6 | 3 | 3.51 / 3.86 | 0.93 | 8.06 | 15.74 |
| 2560x1440@144 | glmark2 terrain | 968 | 138.1 | 137.7 | 9 | 3 | 3.50 / 4.73 | 1.23 | 8.06 | 15.74 |
| 3840x2160@60 | vkcube | 58.3 | 58.3 | 58.0 | 4 | 3 | 3.47 / 4.81 | 6.37 | 13.69 | 14.93 |
| 3840x2160@60 | vkmark (score 2830) | 2830 | 57.9 | 57.8 | 0 | 3 | 3.50 / 4.56 | 2.23 | 13.70 | 14.72 |
| 3840x2160@60 | glmark2 terrain | 648 | 57.5 | 57.4 | 3 | 3 | 3.48 / 4.51 | 2.63 | 13.68 | 14.76 |

- The payload is ~15-16 Gbit/s (1.9-2.0 GB/s) in every mode: the frames per second fall with the frame
  size. post -> shown grows with the frame (6.1 / 8.1 / 13.7 ms; minus the 3.5 ms wait: ~2.6 / 4.6 /
  10.2 ms transfer of 8.3 / 14.7 / 33 MB = ~3.2 GB/s, the Gen3 x4 slot of the host's NIC).
- A full GPU load (vkmark, glmark2 next to gamescope) costs the stream nothing measurable on the 3080.
- arrival -> vblank p50 3.5 ms is the closed loop's target (sink flip lead 2.0 + margin 1.5 ms).
- The same cells into a test host's `rdx_sink_null` over a 40G cable (while that host streamed a game
  to a second client) gave the same picture (1080p@240 vkcube 226.6 gs fps, vkmark 229.3, glmark2
  229.4, post -> shown 6.0 ms, port 14.9 / payload 14.7 Gbit/s).

End-to-end latency, key press at the client -> the first changed frame (same setup, 50 keys per run):

| mode | app | runs / keys | key -> frame at the sink NIC p50 / p90 / p99 ms | key -> vblank showing it p50 / p90 / p99 ms |
|---|---|---|---|---|
| 1920x1080@240 | game loop, vsync (`flash`) | 2 / 100 | 17.23 / 19.11 / 23.79 | 20.72 / 22.57 / 26.98 |
| 1920x1080@240 | game loop, vsync off (`flash-novsync`) | 2 / 100 | 5.39 / 7.14 / 7.55 | 8.88 / 10.65 / 11.08 |
| 1920x1080@240 | draws on input (`flash-idle`) | 2 / 100 | 10.64 / 13.27 / 17.44 | 14.04 / 18.85 / 21.01 |
| 2560x1440@144 | vsync | 1 / 50 | 28.92 / 31.53 / 32.61 | 32.40 / 35.02 / 36.18 |
| 2560x1440@144 | vsync off | 1 / 50 | 9.20 / 11.75 / 12.32 | 12.78 / 15.23 / 15.88 |
| 2560x1440@144 | draws on input | 1 / 50 | 16.13 / 19.31 / 29.93 | 20.44 / 23.18 / 33.24 |
| 3840x2160@60 | vsync | 1 / 50 | 70.97 / 76.45 / 77.82 | 74.38 / 80.29 / 89.98 |
| 3840x2160@60 | vsync off | 1 / 50 | 21.53 / 27.69 / 28.88 | 25.03 / 31.20 / 32.36 |
| 3840x2160@60 | draws on input | 1 / 50 | 36.27 / 43.26 / 53.30 | 40.76 / 47.05 / 70.40 |

No key was lost (0 without a changed frame). Add half a period for the beam to reach the watched
centre pixel on a real screen. Reading:

- **vsync off** is the pipeline itself: input over RDMA (~0.05 ms) + gamescope / Xwayland to the app,
  the wait for gamescope's next paced composite (on average half a period), composite + copy, the
  transfer, the 3.5 ms wait at the sink. At 1080p@240 that is 8.9 ms to the vblank, ~2.1 periods.
- **vsync in the app** (FIFO) adds the app's own frame queue: ~5 periods at every rate (20.7 ms at
  240 Hz, 32.4 ms at 144 Hz, 74 ms at 60 Hz), i.e. 3 frames more than vsync off. That is gamescope +
  Xwayland + the GL swapchain, not the stream; vsync off in the game is the remedy.
- **draws on input** sits between: the app's first swap after a pause waits for a frame callback.
- Over the cable to the test host the 1080p@240 numbers were the same within 0.5 ms (vsync off 9.39,
  vsync 21.22, draws on input 13.46 ms to the vblank, p50).

### Desktop PC -> client, window sink, a concurrent stream

A test host (RTX 2080 Ti behind the B550 chipset at Gen3 x4, host buffers) -> `rdx_sink_null` on the
test client over a 100G link, vkcube (other apps held 5.6 GB of the 2080 Ti):

| mode | gs fps | shown/s | drops | slots | arrival -> vblank p50 / p99 ms | composite p50 ms | post -> shown p50 ms | port / payload Gbit/s |
|---|---|---|---|---|---|---|---|---|
| 1920x1080@240 | 233.9 | 234.4 | 0 | 4 | 3.63 / 3.77 | 2.85 | 5.01 | 15.17 / 14.95 |
| 2560x1440@144 | 140.7 | 140.8 | 0 | 3 | 3.68 / 4.17 | 4.87 | 6.05 | 16.28 / 16.05 |
| 3840x2160@60 | 58.6 | 58.4 | 1 | 3 | 3.64 / 4.27 | 10.65 | 8.79 | 15.19 / 14.99 |

The ConnectX-5 at Gen3 x8 moves a frame in about half the multiseat host's time (post -> shown 5.0 /
6.1 / 8.8 ms vs 6.1 / 8.1 / 13.7 ms); the 2080 Ti's copy into host RAM through the chipset (composite
2.9 / 4.9 / 10.7 ms) is the larger share here. No latency run with a desktop PC as the source: the
source's input mirror would be an ungrabbed keyboard on its desktop.

Window sink (`p0b/rdx_sink_win`, a 960x540 window on the test host's GNOME desktop, iGPU draw), seat 2
vkcube, 2 runs each: 1080p@60 / @120 / @240 all ran at 58.3 fps (the desktop's refresh paces the
window's credits), 0 drops, arrival -> drawn on the iGPU p50 0.77-0.81 ms, p99 1.3-4.0 ms, 3.9 Gbit/s.
`rdx_sink_win` used to default to `--mtu 1024` (now `RDX_DEFAULT_MTU` = 4096 like rdx_sink_kms;
`tests/test_default_mtu`); with rdx_gs_src's 4096 the stream stopped after the first frame (WC "Work
Request Flushed"); rdx-clientd always passes `--mtu`, the tool now does too.

**Gaps in a concurrent stream.** A game stream from the test host to the test client (~98 fps, about
an hour) was checked frame by frame in its telemetry (`/var/log/rdx/kms-sink-*.csv` on the client: V
lines; `gs-src-*.csv` on the source: F lines), gaps of 30-500 ms between shown frames, while rdx_perf
runs streamed into the same test host:

| window | time | gaps | per minute |
|---|---|---|---|
| seat 2 -> test host null sink, 1080p@240 (15 Gbit/s inbound) | 146 s | 5 | 2.05 |
| the same at 60 / 120 / 240 Hz (4 / 7.5 / 15 Gbit/s), 2 runs each | 120 s | 1 | 0.50 |
| runs internal to the multiseat host (test host untouched) | 431 s | 3 | 0.42 |
| everything else of the session | 2725 s | 124 | 2.73 |

Every gap at the sink is a gap of the same length in rdx_gs_src's posts, and there a gap between two
composite begins (gamescope had no new frame); composite (3 ms) and post (0.0 ms) of the late frame
are normal. The gaps are the game's own hitches, a multiple of the 4.17 ms composite grid (41.7, 45.8,
54.2 ms), as frequent without a second stream as with one: no measurable effect of an inbound
stream (up to 15 Gbit/s on the other port of the same ConnectX-5) on the game stream.
