# The seat's own monitor: local picture plus stream

A seat (docs/MULTISEAT.md) shows its picture on the monitor of its own GPU, directly, and a
client can stream the same running app at its own resolution.

```
                  seat GPU (e.g. the 5090 in CT 3101)
 home app (Steam) --> gamescope --backend drm -O HDMI-A-2 --scanout/flip--> monitor (direct, VRR, HDR)
                          |
                          | stream add-on (RDX_GS_STREAM_SOCKET): the same layers again,
                          v at the client's size, into rdx_gs_src's slots
                     rdx_gs_src --RDMA--> client (rdx_sink_kms)
```

By design: local output and stream at the same time, no buffer detour for the local output
(scanout on the GPU's own connector); the clock follows the side of the last input; sound on both.

## Pieces

| Where | What |
|---|---|
| `gs/gamescope-rdx.patch` | `src/Backends/RdxStream.h`: hooks outside the rdx backend. `RdxBackend.cpp`: a second instance of the backend class in *stream mode* (never the backend itself): connects to `RDX_GS_STREAM_SOCKET` whenever rdx_gs_src listens there (retry 500 ms), imports the slots in the paint thread, detaches when rdx_gs_src goes. `steamcompmgr.cpp`: after the monitor's `Present`, `rdx_stream_paint` composites the same frame (all layers: app, Steam overlay, notifications, cursor) into a free slot at the client's size (the cursor next to the picture, docs/GAMESCOPE.md, is the rdx backend's alone: the stream add-on ignores `RDX_GS_CURSOR` and keeps the monitor's cursor in its frames). `wlserver.cpp`: the device of every key / pointer / touch event goes to `RdxNoteInput`. `DRMBackend.cpp`: the page flip drives gamescope's vblank only while the monitor leads; while the client leads the flip commit does not wait for the flip and the monitor's picture is always composited. `vblankmanager.cpp`: the vblank timer ticks at the client's refresh while it leads. `main.cpp`: `RdxPickXwayland` for every backend (Xwayland 24.1 with explicit sync next to gamescope, also on the monitor), `RdxStreamInit`; after the children are reaped gamescope ends with `_exit(0)`: exit()'s library finalizers unloaded the driver libraries under threads that are never joined, and every exit crashed (SIGSEGV in gamescope-wl / gamescope-shdr, exit status 139, upstream #1526 / #1808; `gamescope --backend headless -- true`: 139 before, 0 after). `rendervulkan.cpp`: VK_EXT_external_memory_host for the stream's host slots. |
| `host/rdx_hostd.c` | per seat `"picture"`: `monitor+stream` (default), `monitor`, `stream`; `"monitor_connector"` (`""` = the first connected one), `"home_app"` (default `steam`), `"stream_via"`: `gamescope` (the add-on, default) or `mirror` (stage 1: rdx_mirror_src captures the monitor's CRTC; gamescope then runs with `--disable-layers`), `"local_libseat"` (default `noop`). Starts / stops the home app on the monitor (every 2 s: hotplug, setting, crash with restart), a client starting the home app joins it, `monitor` refuses clients with a clear message, `session.update {"monitor": false/true}` sleeps / wakes the monitor (`gamescopectl drm_sleep_external_screen`), the stream goes on. `hello.local`: state, connectors with the monitor names from their EDID. Connectors are read from sysfs (`/sys/class/drm/cardN-*/{status,edid,modes}`), never by opening the card: an open while no DRM master exists makes the opener master for a moment, and gamescope could lose that race when it starts. |
| `web/host/index.html` | "This seat": Picture, Monitor (connectors with EDID names), App on the monitor, state line. |
| `p1/rdx_mirror_src.c` | `--probe N [--dump FILE.ppm]`: the capture path alone (no sink, no NIC) for seat bring-up. |
| `gs/rdx_gs_probe.c` | stand-in for rdx_gs_src without sink or NIC: hands host slots to a gamescope (rdx backend or stream add-on), classifies every frame (content / black / white), optional VBLANK at the mode's rate, dumps the last frame. |
| `tools/seat-monitor-test.sh` | the seat monitor test inside a seat container: a test hostd in place of rdx-hostd@N (home app vkcube, `RDX_GS_CLOCK`, vkcube present mode), the stream probe with and without VBLANKs, a key from a uinput device (`tools/rdx-uinput-key.py`: a name `rdx: ...` counts as the client's input, anything else as local), the stage 1 capture probe, `restore`. Defaults are seat 1 of the example layout (5090, HDMI-A-1, desktop user `user`), the environment overrides them. Make sure nothing else uses the GPU first. |

## Guard: never another desktop's monitor

hostd takes a monitor only if nobody else shows a picture on that card (`host/rdx_seatguard.c`,
test `tests/test_seatguard.c`): no graphical logind session (x11 / wayland / mir, active or
online) on the card's seat (udev `ID_SEAT`, default `seat0`; mutter and KWin open every card of
their seat), and no other DRM master (open the card: the opener becomes master only if there is
none; `DRM_IOCTL_AUTH_MAGIC` answers EACCES to a non-master, as libdrm's `drmIsMaster` uses it;
close drops it again). Otherwise the picture falls back to "stream only", logged once, the
reason in `hello.local.why_not`, `effective_picture: "stream"`; checked every 15 s. A seat
container has neither and gets its monitor; a desktop machine (e.g. GNOME on the iGPU's
monitors, Wayland session on seat0) stays stream only whatever render GPU is picked.

## The stream add-on (stage 2)

- Same protocol as the rdx backend (`gs/rdx_gs_proto.h`); rdx_gs_src runs **without an app** and
  listens on the socket the local gamescope polls (`/run/rdx-gs-local-<seat>.sock`, handed to the
  desktop user). Its hello carries the client's mode; the monitor keeps its own.
- Composite: blit shader (`vulkan_screenshot`) over the monitor's frame info; each layer's
  transform is scaled by client size / output size (the shaders sample a layer at
  `(pixel + offset) * scale`, so `offset *= k`, `scale /= k`); upscaler passes off; colour like
  gamescope's PipeWire stream (screenshot LUTs, gamma 2.2), so an HDR monitor still streams SDR.
- Host slots: the composite goes into a VRAM image and one GPU copy writes the host slot
  (`CopyToHost`, same as the rdx backend's copy path); VRAM slots import as before.
- Prior art: Steam Remote Play and Sunshine-on-SteamOS use exactly this second composite (the
  gamescope PipeWire node); its known black-frame bugs come from the PipeWire buffer handling,
  which this path does not use.

## Clock: the side of the last input leads

- Input devices named `rdx: ...` are rdx_input_rx's mirrors of the client's devices; anything
  else is local. Gamepads reach Steam directly (not through gamescope) and do not switch the clock.
- Monitor leads (default, and whenever no stream is attached): the DRM page flips drive
  gamescope's vblank; the stream gets every frame the monitor gets, as it comes.
- Client leads: gamescope's vblank timer ticks at the client's refresh (the HELLO's refresh_mhz)
  and rdx_gs_src's VBLANK messages (the client's grid minus network time) set its phase; the page
  flips no longer mark vblanks. The monitor follows without holding the compositor: the flip commit
  does not wait for its completion (upstream gamescope blocks every commit until the flip is done,
  which paced everything at the monitor's refresh), a frame painted while a flip is still in flight
  goes to the stream only, and the monitor shows the newest frame at its next flip (it judders when
  the rates differ). The monitor's picture is always composited while the client leads, so no app
  buffer stays locked by a scanout until the monitor's next flip.
- VRR as the other way: the monitor would follow the frame times itself. Not usable on the test
  monitor: the Acer VG270U P announces only AMD FreeSync over HDMI (vendor block), no HDMI 2.1 VRR in
  its HF-VSDB, and NVIDIA drives VRR over HDMI only as HDMI 2.1 VRR (or G-SYNC Compatible over DP).

## Sound

The seat's default sink plays locally (the GPU's HDMI audio when it is the default) and
rdx_audio_tx captures that sink's monitor for the stream, so both play the same sound without a
combine module. Routing the HDMI audio device into the seat container is the device assignment's job
(rdx-seatmgr).

## Stage 1 (kept as `"stream_via": "mirror"`)

rdx_mirror_src captures the CRTC on commit (kmsgrab, as Sunshine does in SteamOS game mode). The
stream then has the monitor's resolution, and gamescope must keep the whole picture on the primary
plane (`--disable-layers`; Sunshine's known black stream with two planes is this case). A monitor
put to sleep stops the capture; the add-on keeps streaming.

On NVIDIA the capture draws into a GL texture and reads it back into host memory the NIC sends
from (`[cap] output buffers: host read-back`): NVIDIA's GBM refuses a linear render target
(`gbm_bo_create(GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)`: EINVAL) and its dma-bufs are not
NIC-registrable anyway. The import of the scanout buffer itself works (NVIDIA block-linear
modifier). Other GPUs keep the linear GBM buffer the NIC reads directly; the read-back is the
fallback whenever that buffer or its dma-buf registration is refused.

## In-stream menu

A client that joined the app on the seat's monitor gets "Monitor there on/off" (hostd puts the
monitor's EDID name, else the connector, into the session.start reply as `"monitor"`; the menu
sends `session.update {"monitor": bool}`, hostd sleeps / wakes the connector through gamescope's
`drm_sleep_external_screen`, the stream goes on) and "End the stream (the app stays on its
monitor)" instead of "Disconnect". The panel is two rows taller for the extra entry.

## Measured (seat 1, RTX 5090 at 0000:15:00.0, Acer VG270U P on HDMI-A-1)

- First stream from a seat (rdx_gs_src + vkcube on the 5090, host buffers, RoCE 10.0.1.11 ->
  a test host at 10.0.1.2, rdx_sink_null 1080p@240): 5454 frames in 23.6 s = 230.9 fps, 12 dropped,
  header/order errors 0, CQE -> vblank p50 3.50 ms.
- Seat monitor (hostd from HEAD, home app vkcube): gamescope --backend drm took HDMI-A-1 at
  2560x1440@144 (guard: free; Xwayland 24.1.9 next to gamescope), local mouse (PixArt, handed in by
  rdx-seatmgr) opened by gamescope, default sink alsa_output.pci-0000_15_00.1.hdmi-stereo
  (RUNNING). Stream add-on with rdx_gs_probe (1280x720@240): 1440 frames in 10 s = 144 fps (the
  monitor's rate), 0 black/white, composite + copy p50 0.74 ms, picture checked. ~200 MiB on the
  5090.
- Clock pinned to the client (RDX_GS_CLOCK=client), first build: no stall, 1440 VBLANK, still
  144 fps. Cause (strace + a stack of the compositor thread): CDRMBackend::Commit waits for every
  page flip to complete (`uPendingFlipCount.wait`), so the compositor paints at the monitor's
  refresh whatever the vblank timer does (timer measured on the 240 Hz grid, vkcube with
  `--present_mode 0` also 144). Fixed: client leads -> 2401 frames in 10 s =
  240.0 fps to the stream, READY p50 4.14 ms, while the monitor still flips at 144 Hz (288 atomic
  commits/s = 144 flips on the two DRM fds, 0 flip errors, 0 black/white frames). Monitor pinned
  or no input yet: 144.0 fps (unchanged). Auto by input: a key from a uinput device `rdx:probe-kbd`
  switches to the client ("rdx: clock: the client leads (input from rdx:probe-kbd)", 10 s average
  203 fps = 4 s at 144 + 6 s at 240), a key from a local device switches back ("the monitor leads
  (input from probe-local-kbd)").
- After stopping: the primary plane shows framebuffer "allocated by [fbcon]" again (console back
  by itself, no rebind needed), 5090 back to 18 MiB.
- Stage 1 (stream_via mirror) on NVIDIA, first build: gbm_bo_create(LINEAR) of the capture target
  refused by NVIDIA's GBM. With the host read-back: probe 240 captures of
  HDMI-A-1 (scanout XB30, modifier 0x0300000000606014), 239 buffer changes, 0 failed, import + draw
  p50 0.10 ms; stream rdx_mirror_src -> rdx_sink_null on the test host (RoCE, 2560x1440 scaled to
  1920x1080): 2000 frames in 13.9 s = 144.0 fps, 0 dropped, header/order errors 0, capture + draw
  + read-back p50 0.83 ms p99 1.46 ms, sink CQE -> vblank p50 3.83 ms, picture checked (vkcube,
  upright, colours right).

Streamed input: rdx_input_rx's uinput devices are created inside the seat container but appear in
the host's devtmpfs; rdx-seatmgr hands them to the container that created them (its "virtual"
devices, `seat/rdx_seatmgr.py`), so the client's input reaches the local gamescope and can switch
the clock to the client.
