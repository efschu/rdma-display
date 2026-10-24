# Several GPUs for one seat: frame generation on a helper GPU, OptiScaler per game

One seat may use several graphics cards. Split rendering (SLI/AFR) does
not exist for current games; what a second card can do for a game is **frame generation**: the
game renders on the seat's GPU, a second GPU computes a picture between two of the game's
pictures, the client gets up to twice the frames. Separately, **OptiScaler** gives games that
ship DLSS / FSR / XeSS other upscalers and frame generation (mainly for the RTX 3080 seats,
which have no DLSS frame generation).

## What exists

| Project | What | GPU | License | Used |
|---|---|---|---|---|
| [lsfg-vk](https://lsfg-vk.dev) 2.0.0 (Lossless Scaling FG on Linux) | implicit Vulkan layer, hooks the app's `vkQueuePresentKHR` | the app's device only. Dual GPU "being worked on" (project site and blog); the swapchain cannot move to another GPU from a layer | CC BY-NC-ND 4.0 since 08/2026 (private use yes, modified copies no); needs `Lossless.dll` from the user's Steam purchase (branch `lsfg-vk`) | as a per-app engine on the seat's GPU (`lsfg`); not for the helper GPU |
| [wizardkof/lsfg-vk-custom](https://github.com/wizardkof/lsfg-vk-custom) | lsfg-vk fork with cross-device probes | dual GPU "not a user-facing production feature yet" (README) | fork of v1 (MIT) | no |
| [afmf-linux](https://github.com/serialexperimentslainnnn/afmf-linux) 1.3.1 | implicit layer, AMD FidelityFX Optical Flow + own interpolation, AFMF-style half-frame pacing, no DLL | the app's device | MIT (FidelityFX: MIT) | its frame generation core runs on the helper GPU (below); the layer is the per-app engine `afmf` |
| NVIDIA Smooth Motion | driver layer `VK_LAYER_NV_present`, `NVPRESENT_ENABLE_SMOOTH_MOTION=1` | the app's device; reported for RTX 40/50 only | proprietary (driver) | per-app engine `nvsm` (5090 seat); present in driver 595.84 on the test machines |
| gamescope | no frame generation ([issue 1400](https://github.com/ValveSoftware/gamescope/issues/1400)) | - | BSD | - |
| FSR 3 frame interpolation (FidelityFX SDK) | needs the game's motion vectors and depth | in the game | MIT | through OptiScaler |
| NVIDIA Optical Flow SDK FRUC | optical-flow interpolation in CUDA | any | SDK licence, developer login | no: afmf's core does the same in Vulkan without a login |
| RIFE (ncnn / TensorRT) | neural interpolation | any | MIT | no: tens of ms per 1080p frame, video-grade, not interactive |
| [OptiScaler](https://github.com/optiscaler/OptiScaler) 0.9.4 | replaces a game's DLSS/FSR/XeSS, adds FSR3/XeSS frame generation | in the game | GPL-3.0 | per game, `tools/rdx-mgpu-optiscaler.py` |
| GOverlay, OptiTux, dlss-manager, 0ptiscaler4linux | OptiScaler installers / GUIs | - | GPL-3.0 / none / MIT / none | no: GUIs or no machine-readable output; ideas taken (sha256 check, own manifest) |

**Where a layer runs inside gamescope.** gamescope sets the environment for itself and the app,
so a layer enabled by environment loads into both. In the app it does its work (it owns the
swapchain the app presents to gamescope). In gamescope itself it does nothing with our backend:
`--backend rdx` composites into imported buffers and never creates a swapchain or calls
`vkQueuePresentKHR` (afmf also detects gamescope by name, `AFMF_GAMESCOPE=1`). Measured on
the test host (below): afmf-linux loads in vkcube inside `gamescope --backend rdx`, passes through in
gamescope, frames reach the sink.

**Why not lsfg-vk on the helper GPU.** Its frame generation takes the frames from the app's
swapchain on the app's device; there is no option to place it on another GPU (configuration
options, environment variables: none select a device), and the licence forbids a modified copy.
It stays a single-GPU engine.

## The helper GPU: frame generation at the host-RAM stage

rdma-display already moves every frame through host memory on GeForce seats (`--buffers host`,
docs/GAMESCOPE.md): gamescope composites on the seat's GPU and one GPU copy writes the frame into a
pinned memfd slot the NIC reads. A second GPU can read the same slots:

```
 game --> gamescope (seat GPU) --copy--> host slot N --------------------------------> NIC --> client
                                             |                                    ^
                                             | helper GPU reads it (PCIe)         | posted first
                                             v                                    |
                                  helper GPU: optical flow (N-1, N) + interpolate --> host buffer G
```

- `gs/rdx_fg.{h,c}`: a Vulkan device on the helper GPU (picked by PCI address, `VK_EXT_pci_bus_info`;
  two identical 3080s are told apart), the host slots imported with `VK_EXT_external_memory_host`
  (no extra CPU copy), afmf-linux's frame generation core (`third_party/afmf-linux`, MIT, pinned
  commit, plus `framegen_host.c`: frames from/to VkBuffers instead of swapchain images, two
  whole-frame image copies fewer), one worker thread, jobs in order, eventfd when done.
- `gs/rdx_fg_stage.{h,c}`: the part of rdx_gs_src's event loop that owns every post.
  gamescope keeps its full rate and all its slots; the sink's slots are a pool (a frame of
  gamescope or a generated one goes into any free slot, the slot's credit returns what it carried).
  Per composite of gamescope:
  - the same picture as before (signature of 64 x 64 sampled pixels; VRAM: 16 rows copied by CUDA)
    is not sent, its slot goes back to gamescope at once;
  - new pictures further apart than 1.5 sink periods (the app is slower than the sink; hysteresis
    down to 1.25, decided after 30 pictures): the helper makes the frame between the previous
    picture and this one, the generated frame goes out as soon as it is ready, the picture is held
    behind it for half the app's frame time in whole sink periods (60 fps on 120 Hz: one period,
    on 240 Hz: two), so the two land in successive vblanks (the sink shows the newest frame at its
    vblank: posted back to back, 771 of 1970 frames were dropped in the first version);
  - new pictures about every vblank (the app keeps up): sent at once, nothing generated, exactly
    as without frame generation.
  Only frames sent as soon as they were ready steer gamescope's lead loop (GAMESCOPE.md); a held
  picture would pull it by the hold.
- Host buffers on a non-NVIDIA helper (or render GPU): amdgpu imports host pointers of anonymous
  memory only, not of the shared memfd slots (`import of host slot 0 failed: -13` on RADV). The
  slots are therefore sealable memfds (`MFD_ALLOW_SEALING`, `F_SEAL_SHRINK`), `/dev/udmabuf`
  turns each into a dma-buf of the same pages (`rdx_gsbuf_dmabuf`), and the helper / gamescope
  import that with `VK_EXT_external_memory_dma_buf`; the NIC keeps reading the same pages through
  its plain MR. rdx_gs_src opens `/dev/udmabuf` as root and hands gamescope only the dma-buf fds
  (`RDX_GS_FLAG_DMABUF` in the HELLO). Seat containers need `/dev/udmabuf` mounted and
  allowed (`lxc.cgroup2.devices.allow = c 10:<minor> rwm`) only when a seat uses an AMD/Intel GPU.
- VRAM slots (a 5090 with an out-of-tree BAR1 pinning driver providing `/dev/dmabuf_holder`, `--buffers vram`): the helper cannot read another card's VRAM
  (no P2P between GeForce cards); the worker first copies the slot with `cuMemcpyDtoH` in the
  seat GPU's CUDA context into a pinned staging buffer the helper imported.
- `rdx_gs_src --fg helper --fg-gpu 0000:0e:00.0 [--fg-quality auto|quality|performance]`;
  rdx-hostd passes it for apps whose engine resolves to `helper` (see below). Tearing mode: off
  (no vblank pacing). A client mode change (RECONFIG, also a resume to a sink with another slot
  count) rebuilds the stage for the new size; if that fails, frames go out without companions.
- Sink slots: hello's app list carries `"sink_slots": 4` for every app whose engine resolves to
  `helper` (`rdx_mgpu_sink_slots`); rdx_clientd starts `rdx_sink_kms --slots 4` for such an app
  (not with tearing). Nothing is fixed in the client: an app without frame generation keeps the
  sink's default.

### Latency

A generated frame needs the next real one, so the real frame N is shown later by
*helper time + one sink period*. At 120 real fps on a 240 Hz client with a 3080 helper on Gen4 x8:
~2.5 ms (estimate below) + 4.17 ms. Lossless Scaling's dual-GPU mode and AFMF pay the same
half-to-one frame.

### PCIe budget on the multiseat host (sysfs)

| GPU | PCI | link now (card max) | slot |
|---|---|---|---|
| RTX 5090 | 0000:0d:00.0 | Gen4 x8 (Gen5 x16) | CPU root port |
| RTX 3080 | 0000:0e:00.0 | Gen4 x8 (Gen4 x16) | CPU root port |
| RTX 3080 | 0000:08:00.0 | Gen4 x4 (Gen4 x16) | chipset (3 bridges) |

Per real frame the helper reads one frame and writes one (both directions of its link, each
at most once): 1080p 8.3 MB, 1440p 14.7 MB, 2160p 33.2 MB. Assumed ~13 GB/s per direction at
Gen4 x8, ~6.5 GB/s at Gen4 x4 (not measured):

| frame | 0e (Gen4 x8) per copy | 08 (Gen4 x4, chipset) per copy | 120 real fps, one direction |
|---|---|---|---|
| 1080p | 0.64 ms | 1.3 ms | 1.0 GB/s |
| 1440p | 1.1 ms | 2.3 ms | 1.8 GB/s |
| 2160p | 2.6 ms | 5.1 ms | 4.0 GB/s |

The 08 card shares the chipset uplink with the NIC path of nothing else here, but its
Gen4 x4 makes it the worse helper; **0e is the helper of choice**, for the 5090 seat as well as
for a seat on 08.

### Measured: `gs/rdx_fg_bench` (test host)

Helper GPU time per generated frame, *including* both copies (frame host -> helper, generated
frame helper -> host), back to back, 300 frames; the interpolated square checked at the halfway
position (2 px tolerance):

| helper | link | 640x480 | 1920x1080 | 2560x1440 | 3840x2160 | square halfway |
|---|---|---|---|---|---|---|
| RTX 2080 Ti (04:00.0) | Gen3 x4 behind the chipset | 1.0 ms | 5.7-7.4 ms | 14.2 ms | 28.1 ms | 0 of 291 off (1440p: auto = half-res flow, max 2.1 px) |
| Radeon Vega iGPU (09:00.0, Cezanne) | none: shares system RAM | 0.5 ms | 3.2 ms | | | 0 of 291 off |

The 2080 Ti's numbers are dominated by its x4 link (8.3 MB each way at ~3 GB/s = 2 x 2.8 ms at
1080p); the iGPU has no copy at all and beats it. **On the test host the iGPU is the natural helper**
(2080 Ti renders, Vega generates). With a textured background that scrolls under the moving
square (`--pan`), up to 4 px off: afmf's block search on a repetitive texture, a content limit.

Estimate for the multiseat host (not measured): a 3080 helper on Gen4 x8
needs ~2 x 0.64 ms copies + ~1-1.5 ms compute at 1080p = ~2.5 ms; at 2160p ~2 x 2.6 ms + ~5 ms =
~10 ms, which fits 60 real fps (16.7 ms) but not 120 at 4K. Measure with
`gs/rdx_fg_bench --gpu 0000:0e:00.0 --mode 3840x2160`.

### Measured: the stage in rdx_gs_src (test host, vkcube, null sink)

Seat GPU = 2080 Ti, helper = Vega iGPU (dma-bufs through udmabuf), 4 sink slots, 20 s each. The
app's own rate is capped with MangoHud (`MANGOHUD=1 MANGOHUD_CONFIG=fps_limit=N,no_display`:
gamescope's `--framerate-limit` did not cap vkcube, it delivered 113 different pictures/s):

| app | sink | without FG: shown / s | with FG: shown / s (new + generated) | dropped | helper READY -> done p50 |
|---|---|---|---|---|---|
| capped at 60 | 1080p@120 | 56.3 | **111.2** (56.5 + 55.0) | 5 | 3.9 ms |
| capped at 100 | 1080p@240 | 94.2 | **186.3** (92 + 91) | 3 | 3.8 ms |
| uncapped (~225 fps) | 1080p@240 | 226.6 | **226.8** (all new, nothing generated) | 1 | - |

Frame generation doubles what the client sees when the app is slower than half the sink rate
(the capped runs reach ~56 and ~94 new pictures/s, MangoHud's own pacing), and changes nothing
when the app keeps up with the sink. The helper needs 3.4 ms GPU per 1080p frame.

Earlier versions, for the record: with a fixed last sink slot for the generated frames and
gamescope at half the sink rate, 1080p@240 showed 100 frames/s (49 + 49) against 205 without
frame generation, and 58 against 114 with the app capped; on one card (2080 Ti as seat GPU and
helper behind its Gen3 x4 link) 100.7 against 205.

## Per-app options (rdx-hostd, `host/rdx_mgpu.{h,c}`)

```json
{"id": "steam", ..., "framegen": {"engine": "auto", "quality": "auto", "multiplier": 2, "flow_scale": 1.0},
                     "optiscaler": {"enabled": true, "proxy": "dxgi"}}
```

| engine | where | how rdx-hostd starts the app |
|---|---|---|
| `auto` (default) | `helper` if the seat has a `helper_gpu` (different from `gpu`), else `afmf` if installed, else `off` | |
| `helper` | helper GPU | `rdx_gs_src --fg helper --fg-gpu <helper_gpu> --fg-quality <q>`, `DISABLE_AFMF=1 DISABLE_LSFGVK=1` |
| `afmf` | seat GPU | `AFMF_ENABLE=1 AFMF_GAMESCOPE=1 AFMF_PERFORMANCE_MODE=<q> DISABLE_LSFGVK=1` |
| `lsfg` | seat GPU | `LSFGVK_ENV=1 LSFGVK_MULTIPLIER=<m> LSFGVK_FLOW_SCALE=<f> LSFGVK_PERFORMANCE_MODE=0/1 DISABLE_AFMF=1` (+ `LSFGVK_DLL_PATH` for ~/.local/share/lsfg-vk/Lossless.dll) |
| `nvsm` | seat GPU (RTX 40/50) | `NVPRESENT_ENABLE_SMOOTH_MOTION=1 DISABLE_AFMF=1 DISABLE_LSFGVK=1` |
| `off` | - | `DISABLE_AFMF=1 DISABLE_LSFGVK=1` |

Never two frame generators on one swapchain (afmf-linux README: they fight over the presents);
every engine disables the other layers. A game's own frame generation (DLSS-FG, FSR3-FG,
OptiScaler's) stacked on a layer engine is the user's choice per game; with `helper` it is
harmless (the helper only sees the composited frames).

`optiscaler.enabled` (default true) adds `WINEDLLOVERRIDES=<proxy>=n,b` (after the app's own
`WINEDLLOVERRIDES` from its "env", merged by rdx-hostd: `<app value>;dxgi=n,b`): Wine then loads the
OptiScaler DLL from the folder of every game of this app that has it installed; for all other
games the override changes nothing (no such DLL next to their exe, the builtin is used).

Layers are installed the way their projects document it, for the desktop user under `~/.local`
(Steam's runtime container sees the home, not `/opt`): `tools/rdx-mgpu-install.sh afmf USER`
(builds the pinned afmf-linux commit inside a 2 GB memory scope), `lsfg USER` (latest release
from builds.lsfg-vk.dev), `7z`, `remove`, `status`. Both layers stay dormant until the app's
environment enables them. Note: the Ubuntu 24.04 loader (1.3.275) ignores
`VK_ADD_IMPLICIT_LAYER_PATH`; a layer outside the implicit-layer dirs needs
`VK_LAYER_PATH` + `VK_INSTANCE_LAYERS` (used for the tests below).

### OptiScaler per Steam game (`tools/rdx-mgpu-optiscaler.py`, runs as the desktop user)

- `list`: Steam libraries (`libraryfolders.vdf`, `appmanifest_*.acf`), per game the folder of the
  main exe (Unreal: `<Project>/Binaries/Win64`, not `Engine/`), the upscaler DLLs found
  (`nvngx_dlss`, `nvngx_dlssg`, `libxess`, `amd_fidelityfx_*`), OptiScaler state.
- `install --appid N [--proxy dxgi] [--fg native|fsrfg|xefg]`: latest release from GitHub,
  sha256 checked against the release's digest, unpacked with 7z (the archives use BCJ2, which
  Python cannot read), copied next to the exe, `OptiScaler.dll` renamed to the proxy. Every game
  file it would overwrite (e.g. the game's own `libxess.dll`) is moved to
  `<exe dir>/.rdx-optiscaler/backup/` with a manifest; `remove` restores exactly that state
  (the official `remove_optiscaler.sh` deletes the overwritten game DLLs instead).
- `--fg fsrfg|xefg` sets `[FrameGen] FGInput=upscaler`, `FGOutput=fsrfg|xefg` (only keys present
  in the shipped ini): OptiScaler's own frame generation for RTX 30 cards, DX12 games; needs
  HUDfix in many games and is labelled experimental upstream. `native` leaves the ini as shipped
  (DLSS inputs on NVIDIA need no spoofing, `Dxgi=auto` is false on NVIDIA).
- FSR 4 on NVIDIA: OptiScaler 0.9.4 can force the INT8 FSR 4 (`Fsr4ForceEnableInt8`); on the 3080
  it costs several times DLSS (no numbers measured); DLSS stays the better upscaler on these
  cards, FSR 4 is a manual opt-in in the OptiScaler overlay (Insert).
- Never for online games with anti-cheat (ban risk, OptiScaler wiki).

Web UI: `web/host/mgpu.js` adds a section "Frame generation and upscalers" (engine, quality,
multiplier, OptiScaler per app; GPUs with link and slot; install buttons; Steam games with
Install / Remove OptiScaler). API: `/api/mgpu`, `/api/mgpu/games`, `/api/mgpu/optiscaler`,
`/api/mgpu/install`, `/api/mgpu/app` (host/rdx_mgpu.h).

## Tests

| Test | Where | Result |
|---|---|---|
| afmf-linux headless (its own ctest, default device) | test host | passed: 120 presents, 119 generated, square halfway (direct and copy ingest) |
| `rdx_fg_bench`, 2080 Ti and Vega iGPU, 640x480 .. 2160p | test host | tables above; interpolation correct on both vendors |
| `rdx_gs_src --fg helper`, vkcube, 1080p@240 | test host | first version (fixed slot, half rate): 0 errors, 0 drops with the one-period hold, but fewer frames than without FG (above) |
| vkcube with `--gpu 0000:09:00.0` (AMD iGPU), host buffers | test host | host-pointer import fails on RADV (-13); with udmabuf: 57.3 of 60 frames/s shown at 1080p@60, 0 errors, `--verify` checksums 21 of 21 equal at source and sink |
| `rdx_fg_bench` on the iGPU through udmabuf (sealed memfds as in rdx_gsbuf) | test host | 1080p 2.8 ms, 1440p 4.4 ms GPU per frame; square halfway (0 off at 1080p, 1440p half-res flow max 2.1 px) |
| 2080 Ti seat + iGPU helper, `rdx_gs_src --fg helper` | test host | table above: 111 vs 56 (app at 60, 120 Hz), 186 vs 94 (app at 100, 240 Hz), 227 vs 227 (uncapped) |
| baseline vkcube in `gamescope --backend rdx`, host buffers, null sink 1080p@240, 20 s | test host | 207.6 fps, 0 errors, 0 drops |
| afmf-linux in vkcube inside our gamescope, 20 s | test host | layer active in vkcube (after `tools/rdx-mgpu-api12.c`: vkcube asks for Vulkan 1.0, afmf needs 1.1), pass-through in gamescope, 221.9 fps at the sink, 0 errors |
| rdx-mgpu-optiscaler.py list / install / remove on a synthetic Steam library | dev container | exe dir found (incl. UE layout, redist skipped), ini keys set, game's own DLL backed up and restored |
| rdx_mgpu: validation, engine resolution, env, gs args, GPU inventory, web API (bare civetweb) | dev container | as specified; inventory shows the multiseat host's three cards with link and slot |
| web UI section | dev container, Playwright | renders, saves per-app options |
| build matrix: `make` / Vulkan without glslang | dev container | rdx_hostd, rdx_sink_null; rdx_gs_src with FG (4.0 MB, SPIR-V embedded) + rdx_fg_bench, or without FG (412 KB) when glslang is missing; no warnings in the new files |
| gamescope with the dma-buf delta, built in a dev container, run-checked with ldd on the test host | dev container / test host | builds; all libraries resolve against /opt/rdx-gs/prefix |
| **open**: a 3080 helper on the multiseat host, lsfg-vk (needs the DLL), Smooth Motion on the 5090, OptiScaler in a real game | test host / multiseat host | not yet tested |

## Every graphics card is selectable

- rdx-hostd lists every GPU (`rdx_mgpu_enumerate`): NVIDIA from nvidia-smi (also without a DRM
  node: nvidia_drm may be blocked, as on the test host), everything with a DRM render node from
  `/sys/class/drm` (vendor from sysfs, name from `lspci -mm`). Usable: NVIDIA = its `/dev/nvidiaN`
  is accessible (a seat container has only its own), others = the render node opens. The seat's
  "gpu" and "helper_gpu" accept any usable card; the default stays the first usable NVIDIA GPU.
- rdx_gs_src `--gpu PCI` finds the Vulkan device by PCI address (`VK_EXT_pci_bus_info`) and gives
  gamescope its deviceUUID. NVIDIA: CUDA as before (VRAM buffers possible). Any other vendor:
  host buffers, no CUDA call at all, slots as dma-bufs (above). libcuda is not linked but loaded
  on first use (`common/rdx_cuda_dl.c`, dlopen of libcuda.so.1), so rdx_gs_src also starts on a
  host without the NVIDIA driver.
- The frame generation helper may be any Vulkan GPU; on the test host the iGPU is the best one.

## Risks / open

- Multiplier 2 only (one generated frame between two pictures). An app at a third of the sink
  rate or less gets one generated frame per picture and repeats for the rest.
- The repeat detection samples 4096 pixels: a change outside them (a blinking cursor in an empty
  area) counts as the same picture and is shown one picture later.
- Helper on the 08 card (chipset, Gen4 x4): its copies share the chipset uplink; prefer 0e.
- `lsfg` engine: environment-only configuration (`LSFGVK_ENV=1`) applies to every Vulkan swapchain
  of the app's process tree (for Steam: games and the Steam UI if it uses Vulkan); untested
  without the DLL.
- afmf-linux on NVIDIA in real games: only user reports (GTX 1050 Ti); the helper path uses
  only its compute core.
- OptiScaler under Proton: DX12 games are the supported case; native Vulkan games unverified.
