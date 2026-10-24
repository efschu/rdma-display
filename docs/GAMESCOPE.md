# The gamescope path: gamescope composites straight into NIC-readable buffers

An application runs on a virtual monitor that has exactly the client's mode
(for example 1920x1080 at 240 Hz). gamescope composites each frame directly into
buffers that the RDMA NIC reads; the NIC writes the frame into the client's
scanout buffer. No copy after the composite, no PipeWire, and the client's vblank
paces gamescope and the application.

```
 app (FIFO) -> gamescope --backend rdx --composite--> CUDA VMM buffer (VRAM, linear XRGB8888)
                     ^  FREE / VBLANK                     |  dma-buf MR
                     |  READY                             v
               gs/rdx_gs_src  ------------ RDMA WRITE --> rdx_sink_kms (client, scanout from host RAM)
                     ^------------- credits / "shown" reports (SEND WITH IMM) ------'
```

## Pieces

| File | Role |
|---|---|
| `common/rdx_vmm.{h,c}` | one CUDA VMM allocation, three views: CUDA pointer, opaque fd (Vulkan), dma-buf (NIC). dma-buf via `cuMemGetHandleForAddressRange` when the driver offers it, else the RM export (NVIDIA open-gpu-kernel-modules headers, build with `OGKM=<path>`, see `gs/gs.mk`; GeForce: `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` is 0) |
| `p0a/rdx_gate_vmm.c` | gate G-c1: VMM -> dma-buf MR, opaque fd -> Vulkan LINEAR image, Vulkan writes, CUDA and the NIC read back |
| `gs/rdx_gs_proto.h` | unix SOCK_SEQPACKET protocol rdx_gs_src <-> gamescope (HELLO + fds, ACK, FREE, VBLANK, READY, BYE) |
| `gs/rdx_srcsess.{h,c}` | source side of the sink protocol for producers that render into their own MRs (hello, QP, credits, shown reports, post) |
| `gs/rdx_gsbuf.{h,c}` | the output buffers: VRAM (CUDA VMM, dma-buf MR) or HOST (pinned memfd, plain MR), the `--buffers auto` rule, re-registration for recovery |
| `gs/rdx_hash.h` | frame checksum for the coherency test (`rdx_gs_src --verify` vs `rdx_sink_null --hash`) |
| `gs/rdx_gpu_pressure.c` | test load: BAR1-type and plain VRAM allocations (see "Why host buffers are the default" before using the VRAM mode) |
| `gs/rdx_gs_src.c` | the session: sink connection, one output buffer per sink slot, starts gamescope with the sink's mode, frees slots on credits, posts on READY, paces gamescope |
| `gs/gamescope-rdx.patch` | gamescope 3.16.31: new backend `rdx` (`src/Backends/RdxBackend.cpp`), `--backend rdx` |
| `gs/build_gamescope.sh` | builds the patched gamescope on Ubuntu 24.04 (newer wayland/libdrm/xkbcommon/pixman/wayland-protocols into a private prefix with rpath) |
| `gs/rdx_sink_null.c` | display-less sink with the credit/shown semantics of `rdx_sink_kms` (timer vblank), for tests that must not take a screen |

## The gamescope backend

Modelled on the headless backend (no window, no DRM output):

- `Init` connects to `RDX_GS_SOCKET`, receives the client mode, the slot size and
  one opaque fd per slot, sets `g_nOutputWidth/Height/Refresh` to the client mode.
- `ValidPhysicalDevice` refuses a GPU other than the one that owns the buffers (UUID);
  `rdx_gs_src` passes `--prefer-vk-device vendor:device` so the first pick is right.
- Each fd becomes a `VK_IMAGE_TILING_LINEAR` B8G8R8A8 image (mutable format for the
  sRGB sampled view), memory imported with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`,
  wrapped with `CVulkanTexture::BInitFromSwapchain`. The row pitch is reported back and
  must equal the sink stride (1920 -> 7680 measured).
- `Present` takes a free slot, `vulkan_composite(frame, nullptr, false, slot)`, waits on
  gamescope's own timeline (`vulkan_wait`), then sends READY. No free slot: the frame is
  skipped (counted), the next vblank tries again.
- A receive thread turns FREE into a free-slot bit and VBLANK into
  `GetVBlankTimer().MarkVBlank(t, true)`, the same hook the DRM backend feeds from its
  page-flip events.

Sync choice: READY is sent after gamescope's timeline wait, not as a sync_fd. The wait
blocks gamescope's paint for the composite only (p50 0.4 ms on a 2080 Ti) and keeps the
patch small; exporting a sync_fd would let rdx_gs_src wait instead.

## Pacing

The sink's "shown" report arrives right after its vblank; its NIC timestamp anchors the
sink's vblank grid. rdx_gs_src sends gamescope a virtual vblank = grid point - lead.
gamescope then wakes (its own draw-time + red-zone estimate) before that point and
composites, the NIC transfers, and the frame lands just before the sink's flip deadline.

The lead is a closed loop, not an estimate: every shown report carries how long the frame
waited at the sink from arrival to its vblank. Target = sink flip lead (2 ms) + margin
(1.5 ms). The error is taken modulo one period (a wait just under one period longer than
the target means "just missed", i.e. late), lead -= 0.2 x error. No transfer-time model is
needed, which matters because the VRAM read rate depends on where GPU and NIC sit in PCIe.

## Variable refresh (FreeSync / VRR)

When the client's screen has variable refresh and shows each frame on arrival, the client's
vblank no longer paces the source; the app does (on by default, `"vrr"` in the host's defaults):

- The client reports `"display": {"vrr", "min_hz", "max_hz", "hz"}` in session.start (and in
  session.update when it changes, e.g. a window client leaving full screen). rdx-hostd
  (`host/rdx_vrr.h`) starts `rdx_gs_src --vrr <cap>` for a gamescope app: cap = max_hz, never above
  the client's mode, times `"vrr_cap_frac"` from the defaults (0.97, never below min_hz; a
  40-144 Hz screen at 143.999 Hz: cap 139.679), so the rate stays inside the range. Fixed refresh as before without "display", with vrr false, with tearing, with
  frame generation (it fills the client's fixed grid), for whole screens and for a stream that
  joins the seat's own monitor when that monitor has no variable refresh.
- A stream that joins the app on the seat's own monitor (picture monitor+stream) gets variable
  refresh only when the monitor's connector reports `vrr_capable` = 1 (read from the card at the
  join, while the seat's gamescope holds DRM master). The seat's gamescope runs with
  `--adaptive-sync` (`"monitor_vrr"` in the seat config, default on; gamescope enables it only on a
  vrr_capable connector): its monitor flips at the app's pace, `rdx_gs_src --vrr <cap>` sends every
  painted frame, and the stream add-on never lets the client lead (a VRR client sends no vblank).
  Measured: an Acer VG270U P on HDMI-A-1 of an RTX 5090 says vrr_capable 0 (its EDID
  has FreeSync over HDMI, AMD VSDB 40-144, but no HDMI 2.1 VRR, which is what NVIDIA drives); on a
  DisplayPort cable it would be G-SYNC Compatible.
- rdx_gs_src sets `RDX_GS_FLAG_VRR` and refresh_mhz = cap in the hello and sends no VBLANK; the lead
  loop and the lead file rest. Each READY is posted at once, but never sooner than one cap period
  after the previous post (`gs/rdx_gs_vrr.h`, what the kernel does with an early VRR flip); a newer
  READY meanwhile replaces a held one. A change during the session arrives as sigqueue(SIGRTMIN,
  cap mHz, 0 = fixed) and goes to gamescope as `RDX_GS_VRR` (same buffers); a resume reads the cap
  from the third field of `<socket>.peer`.
- The rdx backend's connector then reports VRR (GAMESCOPE_VRR_CAPABLE, IsVRRActive), so gamescope
  uses its own adaptive sync path: it paints as soon as the app presents (FlipType::VRR, its
  flip-ready test and its frame limiter, `--framerate-limit` included), the app sees the cap as the
  refresh rate. The backend marks each "flip" (composite done, or one cap period after the
  previous one) as gamescope's vblank, like the DRM backend does from its page-flip events.
  Steam's per-game GAMESCOPE_VRR_ENABLED=0 is logged and ignored: the client's switch decides;
  `RDX_GS_VRR=0` in gamescope's environment turns it off for debugging.
- Below min_hz the client repeats the frame on its screen (low framerate compensation); the source
  does nothing special. /api/state `session.vrr` shows the decision and rdx_gs_src's last
  `[gs] vrr:` line; rdx_gs_src's summary reports `post -> next post` and how many frames were held to
  the cap. `rdx_gs_probe --vrr` exercises the backend without a NIC.

## Measured (test host: RTX 2080 Ti behind the chipset, NIC loopback, null sink)

| | 1080p@60 | 1080p@240 (5 s) |
|---|---|---|
| frames / s | 58.0 | 108.5 (VRAM read bound) |
| sink arrival -> vblank p50 / p90 | 3.52 / 3.74 ms (target 3.50) | 3.52 / 4.46 ms |
| post -> shown report p50 / p90 | 11.2 / 11.4 ms | 14.9 / 16.1 ms |
| gamescope wake -> composite done p50 | 0.41 ms | 0.37 ms |
| composite done -> RDMA post p50 | 0.018 ms | 0.015 ms |
| errors (header, order) / dropped | 0 / 4 of 957 | 0 / 0 |
| CPU rdx_gs_src / null sink | 0.67 % / 0.25 % | 1.31 % / 0.65 % |

The 2080 Ti sits behind the chipset (Gen3 x4 uplink): the NIC reads its VRAM at
1.12 GB/s (gate G-c1), i.e. 7.4 ms per 1080p frame. That, not the method, bounds the rates
above; on the multiseat host the 5090 sits at a CPU root port.

## End to end on the test client (rdx_sink_kms 1920x1080@240, 2080 Ti source)

| | vkcube | glxgears (Xwayland) |
|---|---|---|
| duration | 21.8 s, then local protection error (see below) | 60 s |
| received / shown / dropped (sink summary) | 2342 / 2342 / 0 | 4915 / 4914 / 0 |
| header / CPU probe / order errors | 0 / 0 / 0 | 0 / 0 / 0 |
| shown per second | 107.7 | 82.8 |
| arrival -> visible p50 / p99 (sink) | 2.57 / 4.48 ms | 2.46 / 4.49 ms |
| flip gaps in vblanks (csv) | 1:242 2:1824 3:34 4:188 5:52 35:1 | 1:468 2:1730 3:693 4:2021 15:1 |
| sink hitches (gap > mean + half vblank) | 120 | 103 |
| post -> shown report p50 / p99 (source) | 14.1 / 34.9 ms | 24.2 / 31.6 ms |
| gamescope wake -> composite done p50 | 0.37 ms | 1.25 ms |
| CPU source / sink | 2.27 % / 2.18 % | 0.90 % / 1.82 % |

The rate is bound by the NIC reading the 2080 Ti's VRAM through the chipset (1.1 GB/s,
7.4 ms per frame): 3 slots in flight cannot keep a 4.17 ms grid, hence gaps of 2-4 vblanks.

## Host buffers (`--buffers host`): the NIC never peer-reads VRAM

Where the NIC cannot safely read the GPU's VRAM (see "Why host buffers are the default"), the
default is pinned host memory:

```
 gamescope composite --GPU writes over PCIe (posted)--> host RAM slot --NIC reads host RAM--> sink
```

- `rdx_gs_src` allocates one memfd per slot (2 MiB huge pages if the pool has them, else
  4 KiB pages; size rounded up to 2 MiB), maps it with MAP_POPULATE, mlocks it and
  registers it with `ibv_reg_mr` (LOCAL_WRITE | REMOTE_READ). gamescope gets the memfd.
- The rdx backend maps the memfd and imports the pointer with VK_EXT_external_memory_host
  (`vkGetMemoryHostPointerPropertiesEXT`, size a multiple of
  `minImportedHostPointerAlignment`). Two paths, chosen at runtime and reported in the ACK:
  - direct: if the driver can import a host pointer as a LINEAR B8G8R8A8 image
    (`vkGetPhysicalDeviceImageFormatProperties2` with HOST_ALLOCATION reports IMPORTABLE),
    gamescope composites straight into host RAM, no copy;
  - copy: otherwise (common: many drivers import host pointers only as buffers) gamescope
    composites into a VRAM output image and one `vkCmdCopyImageToBuffer` writes it into
    the host buffer at the sink stride (barrier MEMORY_WRITE -> TRANSFER_READ before,
    TRANSFER_WRITE -> HOST_READ after, same queue as the composite).
    `RDX_GS_HOST_COPY=1` in gamescope's environment forces this path for comparisons.
- Completeness and coherency: READY is sent only after gamescope's timeline wait for the
  composite (and the copy). GPU writes to system memory are snooped on x86 (coherent with
  the CPU caches); the HOST_READ barrier makes them available, so the CPU and the NIC (DMA,
  also coherent) see the whole frame. Proof by test, not by argument: `rdx_gs_src --verify N`
  prints a checksum of every Nth frame as the CPU sees it right after READY, `rdx_sink_null
  --hash` the checksum of every frame the NIC delivered; equal per frame number = complete
  and coherent (`[hash] <seq> <fnv64>` lines on both sides).

Estimate for the 2080 Ti behind the test host's chipset (Gen3 x4 uplink), 1080p (8.29 MB):

| | VRAM (NIC reads the GPU) | host (GPU writes, NIC reads RAM) |
|---|---|---|
| PCIe transfer through the chipset | NIC read of VRAM: 1.12 GB/s measured (gate G-c1) = **7.4 ms** | GPU posted write: ~95 % of Gen3 x4 (the 2080 Ti writes at ~95 %, reads at ~33 % of the wire) = ~3.7 GB/s = **~2.3 ms** |
| NIC side | (same transfer) | NIC (CPU root port, Gen3 x8) reads RAM: ~6.5 GB/s = ~1.3 ms |
| frame period at 240 Hz | 4.17 ms: 7.4 ms does not fit, measured 108 fps | ~2.3 ms GPU + ~1.3 ms NIC fit (estimate, not measured) |

Plus: posted writes cannot produce the read-completion failure described below.

## `--buffers auto`

`vram` only where peer reads of VRAM are known to be safe and fast, else `host`:

1. the GPU directly below a CPU root port: the PCI path from the root complex
   (`realpath /sys/bus/pci/devices/<bdf>`) has exactly one bridge above the GPU; a PCIe
   switch or the chipset adds bridges,
2. the NIC likewise (`/sys/class/infiniband/<dev>/device`),
3. an out-of-tree BAR1 pinning driver providing `/dev/dmabuf_holder` is present.

`rdx_gs_src` prints the decision with its reasons (`[buf] auto: ...`). Examples: a 2080 Ti
behind the chipset, stock driver -> host; a 5090 at a root port, NIC at a root port, BAR1
pinning driver present -> vram. rdx-hostd passes `--buffers` from its config key `gs_buffers`
(default `auto`); a host can pin `"gs_buffers": "host"` in /etc/rdx/hostd.json, so a later card
or slot change cannot turn its GPU into a peer-read source. In vram mode the dma-buf still comes
from `rdx_vmm` (CUDA export or RM export); reusing the BAR1 pinning driver's own BAR1-window
export (`dmabuf_holder`) there is open.

## Why host buffers are the default

On a test host with a GeForce card behind the chipset (RTX 2080 Ti, Gen3 x4 uplink, stock
driver), letting the NIC read the frames straight out of VRAM was not safe:

- **Symptom.** The pixel RDMA WRITE out of the VMM buffer (dma-buf MR on the RM-exported VRAM)
  intermittently completed with `local protection error`, **vendor_err 0x51**; the QP went to the
  error state and everything behind it was flushed. The failing frame had been in flight
  **55-63 ms**: the NIC's read of the GPU's BAR1 stalled until its PCIe completion timed out.
  Every post was checked against its MR (`P` telemetry line): the failing WR was always inside its
  MR with a valid lkey, so it was not the addressing.
- **Trigger.** It happened when an application started rendering on the GPU (also when a game was
  started from Steam), and in bursts under load (glxgears -fullscreen: 7 errors in 9 s).
- **It can take the machine down.** Under such a burst the card itself failed (Xid 62 PMU halted,
  Xid 154 "GPU Reset Required", Xid 45 for the channels of every other GPU process). With plain
  DEVICE_LOCAL pressure (3.4 GB, the card then nearly full) while the NIC was reading the slots,
  the host hard-reset: the journal just ends, no kernel line, pstore empty.
- **What it is not.** Not the dma-buf exporter revoking the mapping: NVIDIA's open exporter
  (`kernel-open/nvidia/nv-dmabuf.c`, 595) implements neither `move_notify` nor `pin`; mlx5
  attaches dynamically, the dma-buf core therefore maps once and caches the sg table. Not BAR1
  exhaustion: with the session running (BAR1 49/256 MiB, of which the 3 slots ~36 MiB) a load
  mapping 192 MiB DEVICE_LOCAL|HOST_VISIBLE drove BAR1 to 242/256 MiB for 8 s with 0 errors. No
  NVRM/Xid, mlx5 or AER line accompanied the protection errors.
- **Reading (a hypothesis consistent with all observations, not a proof).** The NIC reads VRAM
  through BAR1 at physical addresses fixed when the MR was registered. VMM memory is allocated
  `CU_MEM_ALLOCATION_TYPE_PINNED`, so the pages should not move, but the BAR1 translation for
  them can, and the dma-buf path does not report it. Ordinary pressure turns the reads into
  errors; a nearly full card escalates it to a fatal PCIe error on the read completion, which
  the board turns into a reset. Proving it needs a reproduction under AER logging on a machine
  whose reset is acceptable.

**Consequences.**
- On a GeForce card the RM-exported dma-buf is not a guaranteed-stable peer mapping under memory
  pressure. `cuMemGetHandleForAddressRange` (CUDA's own dma-buf, where
  `CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED` = 1) and `gpuDirectRDMACapable` are the supported route;
  the 2080 Ti reports both unavailable.
- Host buffers (above): the GPU only writes (posted writes), the NIC reads host memory and never
  VRAM. Measured on the same host: glxgears 60 s (7093 frames) and glxgears -fullscreen 60 s
  (7099 frames), 0 errors, 0 drops, ~119 fps. One earlier host-buffer run (before the
  instrumentation) failed once; it was not reproduced in two instrumented runs, cause unknown
  (no vendor_err recorded then).
- `--buffers auto` picks host buffers wherever peer reads are not known to be safe. `--buffers
  vram` refuses unless the safety rule holds; `--force-vram` overrides it. Keep the card's VRAM out
  of the "nearly full" region while the NIC reads it.

**Recovery behaviour.** In VRAM mode a failed work request ends the session (no re-post into a
stalled GPU). With host buffers `rdx_gs_src` tries to recover instead of ending the session:
- `rdx_srcsess_poll` skips flushed completions (counted) and marks a real failure
  (`need_recover`, status kept); successful completions after it (credits, shown reports) are
  still delivered, so no slot is lost.
- Once the CQ is drained: every slot's MR is deregistered, the buffer re-exported where needed
  (`rdx_vmm_reexport`: new RM client + new dma-buf on the same VRAM; the CUDA allocation and
  gamescope's Vulkan imports stay) and registered again; `rdx_srcsess_recover` moves the QP
  RESET -> INIT -> RTR/RTS against the same sink QP, our send PSN restarting at the first PSN of
  the oldest unfinished frame (tracked per post: pixel packets of path MTU + header), our receive
  PSN after the last SEND we completed. To the unchanged sink that is a go-back-N
  retransmission. The frames in flight are lost; their slots go back to gamescope.
- READY while the QP is in error: the frame is skipped, its slot returned at once.
- Journal line `[gs] RECOVERY n after "<status>": k frames lost, ... ms`, telemetry line
  `X,t,status,frames_lost,recovery_ms,dmabuf_via`, total in the final `done` line.
- Give-up rule: 5 recoveries within 2 s end the session.
- Measured: when the sink's QP survives, a recovery takes ~5 ms (1 recovery in 5.3 ms, 2 frames
  lost, the session ran on). When the sink's QP has gone to error as well, every post after the
  PSN restart ends in `transport retry counter exceeded`; a reconnect would need a new handshake,
  which the sink does not support, so the session ends.

**Test tool:** `gs/rdx_gpu_pressure` (Vulkan): `--bar1-mib` maps DEVICE_LOCAL|HOST_VISIBLE
memory (BAR1 without resizable BAR), `--vram-mib` allocates plain VRAM, holds, frees.
Do not run the VRAM mode against a card the NIC is reading, see above.

## Suspend / resume (Moonlight semantics)

Ending a stream does not end the game. `rdx_gs_src` survives the sink: when the sink's
bootstrap connection closes, its QP stops answering, or rdx-hostd sends SIGUSR2, it drops
QP and socket and keeps gamescope, the app, the buffers and their MRs; one slot is handed
back every 100 ms, so the game runs on at 10 fps of composites. SIGUSR1 = resume: it reads
"IP PORT" from `<socket>.peer`, connects to that sink with the same MRs and continues; a
sink with another mode (size, stride, slots, refresh) is refused with exit code 3.
rdx-hostd keeps such an app as "suspended" (hello / `/api/state`: `{"app","mode","since_s"}`
or null) when the client stops or disconnects; session.start of the same app with the same
mode resumes it (another app keeps it running, next section); `app.quit {app}` and `POST /api/apps/quit`
end it; the host web UI shows "Disconnect" on a running game and "Quit app" when paused.
Tested (test host, host buffers, glxgears, null sinks): rdx_gs_src alone and through a
private rdx-hostd: stop -> suspended, glxgears pid unchanged, resume -> 674 frames to the
new sink, 0 errors; app.quit ends glxgears.

## Several apps in the background (`suspend_multi`, on by default)

Switching to another app keeps the previous one running: Steam's download goes on while
Battle.net is in the stream.

- **Model**: one `rdx_gs_src` + gamescope per app, as before (SteamOS' gamescope-session keeps one
  gamescope with Steam as its base layer and games below it; that is one compositor per seat that
  only Steam drives, here every app is its own entry with its own mode, VRR and tearing setting, so
  each keeps its own gamescope). Each app has its own socket `/run/rdx-gs-<seat>-<app id>.sock`
  (and `.peer` next to it); at most 4 apps on a seat at once, the one in the stream included.
- **Input**: a detached gamescope lets go of every input device (`RDX_GS_INPUT 0` from
  `rdx_gs_src` on SIGUSR2 or a vanished sink: `libinput_suspend` on both of its libinput contexts,
  every grab released), `RDX_GS_INPUT 1` after a resume. Without it the background gamescope would
  open and grab (EVIOCGRAB) the next session's `rdx:` devices as well, and whichever grabbed first
  would get the keyboard. rdx-hostd looks for the marker `RDX_GS_INPUT v1` in the gamescope binary
  (read again when the binary changes); an older gamescope means one app in the background, as before.
- **rdx-hostd**: `suspended_apps` `[{app, name, mode, since_s, tearing, vram_mib}]` (oldest first) in
  hello, status and `/api/state`, `suspended` = the newest of them for older clients, `suspend_multi`
  and `apps_max`. session.start of an app in the background resumes it; with another tearing setting
  only that app is closed and started fresh; another app keeps the others, a start beyond 4 is refused
  with their names (nothing is closed to make room). The same app twice is a resume, also another
  entry of the same single-instance program (Steam: program `steam`, or `"single_instance": true`):
  the reply says `"resumed_as": "<running app>"` and the client takes that id. One more app while
  others run: `nvidia-smi` on the seat's GPU; below `suspend_vram_warn_mib` (2048) free the reply has
  `vram_warning` `{free_mib, threshold_mib, apps: [{app, name, vram_mib}], text}` (an app's memory:
  every GPU process below its `rdx_gs_src` or with its `RDX_GS_SOCKET`), the start goes on. Closing is
  explicit only: `app.quit {app}`, `POST /api/apps/quit {app}` (no app: the newest), Stop in the host
  page (one row per paused app), "Quit" in the client page and menu; rdx-hostd's shutdown closes all;
  an app that ends by itself in the background leaves the list.
- **Client**: one "Still running" band per paused app with Resume / Quit, every running app marked on
  its tile ("Running · Resume", Artemis' pattern); the in-stream menu has "Switch to <App>" (Left /
  Right picks, Enter: this app waits in the background, that one resumes) and "Quit <App>"; the memory
  warning is a card in the stream and a line on the page.
- **Limits**: the apps in the background share the GPU (each keeps its memory; gamescope paints at
  10 fps, the app itself may still render), the seat's sound (a background game's music plays into
  the stream) and the client's gamepads (a game in the background that reads the controller directly,
  e.g. Steam Input, sees it too). A local join ("the seat's own monitor") and "Start here on this PC" /
  "Send to the client" use the same list.
- Test: `tests/test_multi_suspend.py` (real rdx_hostd, stand-in rdx_gs_src and nvidia-smi).

## Tearing mode (`rdx_gs_src --tearing`)

For a client that shows frames on arrival (`rdx_sink_kms --tearing`: one slot, credit right
after arrival). The client sends `"tearing": true` in session.start, rdx-hostd starts
`rdx_gs_src --tearing`: RDX_GS_FLAG_TEARING in the hello, gamescope with `--immediate-flips`,
no VBLANK messages; the backend reports `SupportsTearing()`, so gamescope composites and
READYs as soon as the app presents. Measured (test host, host buffers, 1080p, null sink
`--tearing --slots 1`, 30 s each, 0 errors, 0 drops):

| app | path | fps | gamescope present -> composite done p50 / p99 | done -> post | post -> on screen p50 |
|---|---|---|---|---|---|
| vkcube --present_mode 0 | composite straight into host RAM | 102.4 | 6.88 / 7.21 ms | 0.024 ms | 1.32 ms |
| glxgears, vsync off | composite straight into host RAM | 114.5 | 6.90 / 6.95 ms | 0.024 ms | 1.32 ms |
| vkcube --present_mode 0 | composite in VRAM + GPU copy | 118.5 | **3.16 / 3.42 ms** | 0.020 ms | 1.32 ms |

The copy path is twice as fast (the copy engine streams to host RAM, the composite shader
writes it slowly), so it is the default for host buffers since; `RDX_GS_HOST_DIRECT=1` selects
the straight path. Paced 1080p@240 with the copy path: 216 fps (straight: 108-119).

## Resume at another client mode

`rdx_gs_src` connects to the new sink; if its mode differs it allocates new buffers + MRs for
it and sends RECONFIG (a hello with type RECONFIG + the new fds). The backend applies it in
the paint thread: wait idle, drop the old slots, import the new ones, ACK, set
`g_nOutputWidth/Height/Refresh` - steamcompmgr re-lays out like after an SDL window resize.
The app keeps its own size and is scaled to the new output. READY carries the slot
generation, so a late READY for an old slot is never posted. rdx-hostd resumes the paused app
whatever the mode; only a change of the tearing setting quits and starts fresh.
Measured: suspend at 1920x1080@240, resume at 1280x720@120: RECONFIG 17-21 ms, same
glxgears pid, 1434 frames at 119.5 fps to the new sink, 0 errors, picture correct (gears
scaled into 1280x720); also through a private rdx-hostd (same pid; then tearing on -> new pid).

## No upscaling of ordinary windows

gamescope's default scaler (`--scaler auto`) fits the focused window to the output and magnifies
it as far as that goes: a 400x700 login dialog (Battle.net) on 2560x1440 came out blurred and
2x its size. gamescope 3.16.31 has no option for
"never above 1:1" (`--max-scale` caps every window, a fullscreen game rendering below the output
included; `--scaler integer` still scales by 2), so the patch adds the scaler `NOUPSCALE`
(`src/main.hpp`, `calc_scale_factor_scaler`): scale = min(1, output/window on both axes), centred.
A window not larger than the output is shown 1:1, a larger one is fitted down as before.

It applies to the focused X11 toplevel only if it is a *plain window* (`window_is_plain_window`,
steamcompmgr.cpp): not Steam, not `_NET_WM_STATE_FULLSCREEN`, not `force_windows_fullscreen`, and
not as large as the X root. A fullscreen game that renders below the output (1920x1080 on 1440p)
declares itself with the atom (Wine and SDL set it also with a mode change) or fills the root
(Xwayland's resolution emulation: window = root, buffer smaller), so it keeps the auto scaler and
is magnified to the output as before. A borderless window without the atom is a window to
gamescope, hence 1:1 (known limit). Native Wayland clients and an explicit `--scaler` other than
auto are left alone. Same code in the rdx backend and the DRM backend (seat monitor).

On by default; `RDX_GS_NOUPSCALE=0` in gamescope's environment restores the stock auto scaler.

## Seats (several streamed desktops on one machine)

One rdx-hostd per seat (e.g. one LXC per GPU on a Proxmox host, all in the host network namespace
with the one RDMA NIC): `rdx_hostd --seat N` / `rdx-hostd@N.service` (`desktop/install.sh host
--seat N`). Seat N moves every default port by 100*N (control 47989, web 47990, video 18650,
input 18660, audio 18670, USB 18680) and reads /etc/rdx/hostd-seatN.json. The listeners on the
host (input, USB) always use the seat's own ports, never the client's request, so two seats
cannot collide. Seat config:
- `"gpu": "0000:0d:00.0"` (default: the first NVIDIA GPU) -> `rdx_gs_src --gpu`: CUDA ordinal by
  `cuDeviceGetByPCIBusId`, gamescope composites on the Vulkan device with that UUID
  (`GAMESCOPE_VK_DEVICE_UUID`, our patch - `--prefer-vk-device vendor:device` cannot tell two
  identical 3080s apart), mirror apps use `/dev/dri/by-path/pci-<gpu>-card|render`. Several
  seats may name the same GPU (one gamescope each).
- `"priority": "high|normal|low"`: gamescope's compositor queue global priority
  (`GAMESCOPE_VK_QUEUE_PRIORITY`, our patch, also without CAP_SYS_NICE), nice -5 / 10 and
  ionice best-effort 0 / 7 of gamescope and the app, the mirror's EGL context priority.
  Set in the host web UI ("This seat").
- `"seat_name"` ("" = the GPU's short name): hello.seat_name "RDMA Display - 5090".
- `"helper_gpu"` (reserved, frame generation), `"seat_env"` {KEY: VALUE} and `"pre_launch"`
  [argv] as hooks (seat_app_hook() in host/rdx_hostd.c; apps get RDX_SEAT, RDX_GPU,
  RDX_HELPER_GPU).
- `--buffers auto` per GPU: vram only with that GPU and the NIC directly at CPU root ports and
  the BAR1 pinning driver (`/dev/dmabuf_holder`); on the multiseat host 0d/0e (CPU root ports) ->
  vram once the NIC sits at a root port, 08 (behind the chipset) -> host.

The client lists several hosts: `"hosts": [{"name","address","port"}]` ([] = the old single
`"host"`), `"active_host"`. The active one has the full control connection, the others a
hello-only connection (every 5 s); the home page groups the tiles by host, starting an app of
another host switches to it. Tested on the test host: seats 10 and 11 on the one 2080 Ti
(priority high / low: nice -5 / 10, ionice 0 / 7, queue high / low), both streaming glxgears at
the same time to two null sinks (0 errors), the client sees both (seat names, apps) and
switches to host 1 when its app is started.

## The cursor next to the picture

gamescope used to draw the cursor into every frame. With variable refresh the source sends at the
app's pace, so each mouse motion over a still app (a launcher) made a new composite and a new frame:
the stream ran at the cap (~140 Hz) for an app drawing nothing, and in a game cursor frames went out
between the game's frames. Now the cursor travels on a channel of its own and the client shows it
(`common/rdx_cursor.h`); a pointer that only moves costs the source nothing but a 48-byte message.

```
gamescope (rdx backend)  --CURPOS / CURIMG-->  rdx_gs_src  --RC QP of its own, SEND-->  sink
  ShouldPaintCursor() false (nested rule):        caches the picture        KMS: the CRTC's cursor plane
  no cursor in the frames, no repaint on          and the position          window: the window's X cursor
  pointer motion                                                            (or blended into the copies)
```

- **gamescope** (`RdxBackend.cpp`): `RDX_GS_CURSOR 1` from rdx_gs_src makes the rdx connector behave
  like a nested backend (SDL / Wayland): `ShouldPaintCursor()` is false, so steamcompmgr draws no
  cursor layer and `wlserver_oncursorevent` sets no repaint for a motion. The X cursor's picture
  arrives in `INestedHints::SetCursorImage` (ARGB premultiplied, hotspot) and goes out as `CURIMG`
  chunks of 16 KiB (a socket that is full is continued later from that chunk); every pointer event
  calls `RdxCursorMoved` (wlserver, lock held), which maps the focused surface's position to output
  pixels with the inverse of the touch mapping (`focusedWindowScale/Offset`, the one the window
  client's absolute pointer uses) and sends `CURPOS` (x, y, visible) when it changed;
  `RdxCursorTick` in steamcompmgr's loop covers relayouts and hiding without a motion. The hide
  after inactivity (`checkSuspension`) runs as for a drawn cursor. `--force-relative-mouse` and the
  stream add-on of the seat's monitor keep drawing it.
- **rdx_gs_src** keeps the last picture and position (`struct rdx_gs_cursor`), tells gamescope after
  every connect and resume whether the client shows the cursor, and forwards changes; if the
  channel fails, gamescope draws the cursor again. `--cursor picture` turns it off. Telemetry: `C`
  lines (gamescope's motion time, receive time, x, y, visible).
- **The channel**: a second RC QP on the frames' PD and CQ (wr_ids tagged `RDX_CUR_WR_TAG`). A
  position is one inline SEND; its send queue is not behind a frame's write (a 1440p frame is
  ~15 MB, a few ms on the wire), and a failure there never touches the frames' QP or its recovery.
  Chosen over UDP on the link IP (one more kernel stack and socket per side, no reliability) and
  over a header field (needs a frame, the very thing to avoid). Negotiated in the TCP bootstrap,
  compatible both ways: the sink's hello carries `caps |= RDX_CAP_CURSOR` (the field was padding,
  older sinks send 0), the source answers with `RDX_CONN_F_CURSOR` in its conn's PSN (24-bit PSNs:
  older sources never set it) followed by its cursor QP's conn, the sink sends its own. Old sink,
  old source, old gamescope: the cursor stays in the picture as before.
- **rdx_sink_kms**: the CRTC's cursor plane through the legacy cursor ioctls (`CURSOR2` with the
  hotspot, `CURSOR MOVE`): a motion is one ioctl, no flip. On a DRM lease the plane is there when
  the lessor runs without universal planes (the kernel adds the CRTC's primary and cursor plane,
  `drm_lease.c`); else `EACCES` (`drm_mode_cursor_common` checks the plane's lease). Under VRR a
  cursor update starts no refresh of its own (measured on amdgpu, below); where the driver repeats
  frames below the range itself (amdgpu BTR) the cursor moves at that rate and the refresh does not
  follow the mouse. Elsewhere the frame on screen is flipped once more (a kick, at most one flip at a
  time and only two fastest periods after the last frame, so a game's frames carry the cursor;
  `RDX_CURSOR_KICK=1/0` forces it).
  Without a cursor plane in copy mode (amdgpu lease of a lessor with universal planes) the cursor is
  blended into the scanout copy, never into the slot: a move puts the slot's pixels back under the
  old place and blends it at the new one, in the buffer on screen, plus a kick under VRR. Without
  both the sink does not offer the channel. `--cursor picture` turns it off.
- **rdx_sink_win**: the window's X cursor becomes the host's picture (Xcursor, scaled like the
  letterboxed frame), an empty one while the host hides it. Its position is the local pointer's (the
  window's absolute pointer), so a motion needs neither a frame nor a draw. clientd learns it
  (`RDX_MENU_POINTER` b = 1 from the sink) and tells `rdx_input_win` (b = 2) to leave the window's
  cursor alone instead of hiding it; in relative mode the pointer grab's empty cursor hides it.
- Prior art: gamescope's nested backends (`INestedHints`, the same rule on one machine), its DRM
  backend's cursor plane, wlroots (`wlr_output_cursor`: hardware plane, software fallback), SPICE
  (a cursor channel of its own), RDP pointer updates, VNC's cursor pseudo-encoding, Parsec (cursor
  events with image, hotspot, position, relative, hidden), Looking Glass (a pointer queue next to
  the frames; GPL, the concept only). Sunshine/Moonlight blend the cursor into the video.
- Measured on the test host: its own gamescope (2080 Ti, host buffers) through the NIC loopback into
  `rdx_sink_kms` on a lease of DisplayPort-0 (amdgpu, 2560x1440, VRR 40-144 Hz, BTR by the driver),
  `rdx_flash --cursor`, the mouse synthetic (`rdx_keyinject.py --mouse` -> rdx_input_tx/rx), 20 s each:

  | app | mouse | cursor | source fps | post -> next post p50 | panel refreshes (drm.debug 0x20) |
  |---|---|---|---|---|---|
  | 30 fps | ~30 motions/s | in the picture | 44.6 | 22.1 ms | 63.9 Hz, 8.2-25 ms apart (following the mouse) |
  | 30 fps | ~30 motions/s | next to it | 29.6 | 33.3 ms | 60.0 Hz, 16.4-16.9 ms (2 x 30, BTR) |
  | 30 fps | ~180 motions/s | in the picture | 93.7 (964 frames held to the cap) | 7.0 ms | 125.8 Hz, 6.9-11.8 ms |
  | 30 fps | ~180 motions/s | next to it | 29.6 | 33.3 ms | 60.0 Hz, 16.4-16.9 ms |
  | no frames | ~5 motions/s | in the picture | 3.5 (one per motion) | 203 ms | 74.5 Hz, 8.9-25 ms |
  | no frames | ~5 motions/s | next to it | 0.1 | - | 62.6 Hz, 15.97 ms flat |

  Motion -> the update at the sink (one clock, loopback): in the picture p50 23.9 / p90 24.3 ms to the
  frame's arrival, 28.4 / 35.7 ms to its flip; next to it p50 0.38 / p90 0.61 / p99 1.60 ms to the
  position's arrival (the plane is moved at once), visible at the next BTR refresh (<= 16 ms). The cursor
  plane of the lease works (amdgpu DDX: 2399 moves, 0 failed ioctls). `RDX_CURSOR_KICK=1` made the
  refreshes follow the mouse again (82.6 Hz mean), so it is off where the driver repeats frames itself.
- Tests without hardware: `tests/test_cursor` (layouts, gamescope -> rdx_gs_src packets through a
  socketpair, the wire, the bootstrap step, blending, scaling), `tests/test_window_menu` (b = 2).
  Measuring: `tools/rdx_perf.py --loads cursor,cursor30 --cursor picture|auto` (docs/PERF.md).

## Run

```
gs/build_gamescope.sh /usr/local/src/rdx-gs             # once
rdx_sink_kms --dev mlx5_0 --bind 10.0.0.1 --mode 1920x1080@240     # client
rdx_gs_src --dev rocep1s0f1 --peer 10.0.0.1 -- vkcube              # host
```
`rdx_gs_src` options: `--buffers auto|host|vram` (default auto; vram only with the safety rule), `--force-vram`, `--tearing`, `--verify N`, `--gamescope PATH`, `--gs-arg ARG` (repeatable, e.g. `--gs-arg
--framerate-limit --gs-arg 120`), `--lead-margin-ms`, `--sink-flip-lead-ms`, `--cuda N`,
`--socket PATH`, `--log-dir`. Without an app it waits for an externally started
`gamescope --backend rdx` (env `RDX_GS_SOCKET`).

## Open

- Input works via gamescope's own `CLibInputHandler` on seat0 (started by the rdx
  backend, own poll thread): rdx_input_rx's uinput mirrors reach the app. Tested with a
  uinput keyboard and `xev` inside gamescope: 3/3 KeyPress + KeyRelease, keycode 38 (a).
  seat0 also carries local input devices of the host; a separate seat needs a udev rule
  (ID_SEAT on the rdx uinput devices) and a seat-name parameter in `LibInputHandler.cpp`.
- Cursor: next to the picture where the client can show it (below); drawn into the composite for an
  older client, a mirrored desktop and the stream add-on of the seat's monitor.
- HDR / 10 bit: the client scans out XRGB8888; not offered.
- Whole-desktop mode 1 (scanout of a real output, one detile pass into VMM via
  GL_EXT_memory_object_fd) is not part of this.
