# rdma-display

Stream a Linux computer's picture, sound and input to another Linux computer over an RDMA
(RoCE v2) network link, **uncompressed**. Frames go from GPU-composited buffers straight into the
client's scanout buffer by RDMA WRITE: no video encoder, no decoder, no copy on the client, and the
client's monitor paces the host. Think Moonlight/Sunshine, but over a 40/100 GbE RoCE link and
without video compression - latency is a few milliseconds from the app's frame to the client's
screen.

Two computers, each with an RDMA-capable NIC:

- the **host** runs the desktop, the games and launchers (Steam, Lutris, Heroic, Battle.net, ...),
- the **client** has the screen, keyboard, mouse, gamepad, speakers, microphone and USB devices.

## Features

- **Picture**
  - *One app on a virtual display* (the main path): the app runs in a patched gamescope
    (`--backend rdx`) on a virtual monitor with exactly the client's mode, e.g. 1920x1080 at 240 Hz.
    gamescope composites straight into buffers the NIC reads, the client's vblank paces gamescope
    and the app ([docs/GAMESCOPE.md](docs/GAMESCOPE.md)).
  - *The whole screen*: a real monitor output of the host is mirrored (KMS capture at the
    compositor's commit, de-tiled, written into the client's scanout slot).
  - **Variable refresh (FreeSync / VRR)** where the client's screen has it: the app's own pace up to
    the screen's maximum, low framerate compensation below its minimum.
  - Match mode for whole screens (the client runs at the host output's exact refresh, phase-steered),
    optional tearing mode (lowest latency), the mouse pointer travels next to the picture (a pointer
    motion costs no frame).
- **Input**: the client's keyboards and mice are grabbed and mirrored on the host as uinput devices;
  in a gamescope app they reach only that app, never the host's desktop or console.
- **Sound** from the host to the client (PipeWire on both sides, drift control by rate matching), and
  the **microphone** from the client to the host ("RDMA Display Mic", opened only while an app records).
- **USB**: HID devices (gamepads, wheels, ...) through hidraw -> uhid, all other devices through
  USB/IP over SMC-R.
- **Clipboard** both ways (text up to 1 MiB, also the primary selection).
- **Apps keep running in the background**: ending a stream only detaches the app; up to 4 apps per
  seat stay alive (a Steam download goes on while Battle.net is streamed) and are resumed or quit
  from the client, also at another resolution.
- **Window client and lease mode**: a desktop PC can show a host in a window, or - on X11 with two or
  more monitors - take one monitor directly by a DRM lease for the game while the desktop stays on the
  others; Ctrl+Alt+H moves keyboard and mouse between stream and desktop
  ([docs/WINDOW-CLIENT.md](docs/WINDOW-CLIENT.md)).
- **Multiseat**: one machine, several independent hosts ("seats"), e.g. one LXC container per seat on a
  Proxmox host; every seat starts on any free graphics card, chosen from the client
  ([docs/MULTISEAT.md](docs/MULTISEAT.md)).
- **Multi-GPU**: frame generation on a second GPU, OptiScaler per Steam game
  ([docs/MULTI-GPU.md](docs/MULTI-GPU.md)); "Start here on this PC" runs an app in a window on the
  host's own desktop ([docs/LOCAL-WINDOW.md](docs/LOCAL-WINDOW.md)); a seat's own monitor with the
  stream as an add-on ([docs/SEAT-MONITOR.md](docs/SEAT-MONITOR.md), in progress).
- Plain-language web pages on both sides, tray icons, link health (the NIC's module temperature and
  failures are reported before the link goes down), per-frame telemetry.

## Architecture

```
 HOST                                                     CLIENT
 rdx-hostd  (web UI :47990, control :47989) <--- TCP ---> rdx-clientd  (web UI 127.0.0.1:47991)
  |  starts and supervises per session:                     |  starts and supervises per session:
  |                                                         |
  |  app -> gamescope --backend rdx                         |
  |           |  composite into NIC-readable slots          |
  |  gs/rdx_gs_src  ===== RDMA WRITE (frames) ============> p0b/rdx_sink_kms (own VT, page flips, VRR)
  |    (or p1/rdx_mirror_src: a whole screen)                   or p0b/rdx_sink_win (a window)
  |                 <===== credits / "shown" reports =======
  |  p5/rdx_audio_tx (PipeWire capture) ===== RDMA ======>  p5/rdx_audio_rx (PipeWire playback)
  |  p4/rdx_input_rx (uinput mirrors)   <==== RDMA ======   p4/rdx_input_tx (evdev, grabbed)
  |  usb/rdx_usb_host (uhid, vhci-hcd)  <== RDMA / SMC-R ==  usb/rdx_usb_client (hidraw, usbip-host)
  |  p5/rdx_audio_rx --mic (virtual source) <== RDMA =====  p5/rdx_audio_tx --mic
  |  clip/rdx_clip (on the app's Xwayland) <=== TCP ====>  clip/rdx_clip --desktop
```

Each data channel is its own RC queue pair, set up by a short TCP bootstrap on the link IP. The
control channel is newline-delimited JSON (`common/rdx_ctl.h`). Everything is event-driven; an idle
daemon uses no CPU.

## Requirements

**Network**

- An RDMA NIC with RoCE v2 in both computers, cabled directly or through a switch. Developed and
  tested with Mellanox/NVIDIA **ConnectX-4** and **ConnectX-5** (mlx5) on 40G and 100G links.
- RDMA path MTU 4096 (the default of every part), so the RoCE interfaces need an Ethernet MTU above
  4096; use 9000.
- One IPv4 address per side on the RoCE interface ("link IP"). Defaults: host `10.0.0.2`,
  client `10.0.0.1`.

**Host**

- Ubuntu 24.04 (the gamescope build script targets it); a multiseat host runs Proxmox VE with
  Ubuntu 24.04 containers.
- An NVIDIA GPU for gamescope apps (tested: RTX 2080 Ti, RTX 3080, RTX 5090) with the NVIDIA driver,
  the CUDA headers for building and Vulkan. A whole-screen mirror works with the host's KMS driver.
  An AMD or Intel GPU as the *render* GPU of gamescope apps needs `/dev/udmabuf` and is not finished
  (see [Known limits](#known-limits)); as the frame generation helper it works.
- PipeWire for sound, a desktop user for the apps (Steam refuses to run as root).

**Client**

- Any Linux with a KMS driver for its screen (tested: amdgpu, i915) and PipeWire; the stream takes
  its own virtual console while it runs. A window client additionally needs an X11 (or Xwayland)
  desktop.

**Kernel modules** (loaded on demand where possible): `mlx5_ib` / `ib_uverbs` (rdma-core),
`uinput` and `uhid` on the host, `vhci-hcd` on the host and `usbip-host` on the client for USB/IP,
`smc` on both for the USB/IP transport, `udmabuf` for non-NVIDIA render GPUs.

## Installation

The same steps on host and client; the role is chosen when installing.

### 1. Packages (Ubuntu 24.04)

```sh
sudo apt install build-essential pkg-config git \
    rdma-core ibverbs-providers ibverbs-utils libibverbs-dev \
    libegl-dev libgles-dev libgl-dev libgbm-dev \
    libx11-dev libxcursor-dev libxrandr-dev libxfixes-dev libxcb1-dev libxcb-randr0-dev \
    libpipewire-0.3-dev libsamplerate0-dev linux-libc-dev \
    python3-gi gir1.2-ayatanaappindicator3-0.1
# host, for gamescope apps and frame generation:
sudo apt install libvulkan-dev glslang-tools nvidia-cuda-toolkit
```

The Makefile builds every part whose libraries it finds and leaves the others out (no libgbm: no
whole-screen mirror; no Vulkan or `cuda.h`: no `gs/rdx_gs_src`). `gs/rdx_gs_src` looks for
`cuda.h` in `/usr/local/cuda/include` (NVIDIA's CUDA packages); with Ubuntu's `nvidia-cuda-toolkit`
build with `make CUDA_INC=/usr/include`. libcuda itself is loaded at run time.
Optional: `make OGKM=<checkout of NVIDIA/open-gpu-kernel-modules>` adds the RM dma-buf export that
VRAM buffers need on GeForce cards (see [docs/GAMESCOPE.md](docs/GAMESCOPE.md)); the default host
buffers do not need it.

### 2. Build

```sh
git clone https://github.com/efschu/rdma-display.git
cd rdma-display
make            # one job is enough; the tree is small
make test       # the fast local tests (no RDMA, no GPU needed)
```

### 3. The patched gamescope (host only)

```sh
sudo gs/build_gamescope.sh                  # gamescope 3.16.31 + the rdx backend, Xwayland 24.1.9,
                                            # in /usr/local/src/rdx-gs (log: build.log there)
sudo gs/build_gamescope_wsi_i386.sh         # the 32-bit gamescope WSI layer (Proton's 32-bit processes)
sudo host/install-gamescope.sh              # -> /opt/rdx-gs, plus the WSI layer manifests
```

The build directory is the first argument of all three scripts (default `/usr/local/src/rdx-gs`;
`host/install-gamescope.sh SRC DST` installs from SRC to DST, default `/opt/rdx-gs`). Newer
wayland/libdrm/xkbcommon/pixman are built into a private prefix with an rpath; nothing in `/usr` is
replaced except the Vulkan layer manifests in `/usr/share/vulkan/implicit_layer.d`. On small machines
`NINJA_JOBS=2` limits the parallel compile jobs and `GS_MESON_ARGS=-Db_lto=false` keeps the final link
small.

### 4. Install as a desktop app

```sh
sudo desktop/install.sh host            # on the host
sudo desktop/install.sh client          # on the client
```

`desktop/install.sh` copies the built tree to `/opt/rdma-display`, installs and **enables** the
systemd unit (`rdx-hostd` / `rdx-clientd`; it never starts or restarts it), a polkit rule that lets
members of the `sudo` group start and stop the daemons from the tray without a password, the app
entry ("RDMA Display Host" / "RDMA Display Client") with its tray icon, and on the host the udev rules
that keep the stream's input devices away from the host's desktop and logind (group `rdx-input`).
Further roles:

- `sudo desktop/install.sh client-window` - a client in a window of this desktop
  (`rdx-clientd-window`, web UI `http://127.0.0.1:47992`, config `/etc/rdx/client-window.json`), e.g.
  on a PC that is itself a host;
- `sudo desktop/install.sh host --seat N` - one more streamed seat on this host
  (`rdx-hostd@N`, ports +100·N, config `/etc/rdx/hostd-seatN.json`).

### 5. Network

Give the RoCE interfaces their link IPs and MTU, e.g. with netplan or by hand:

```sh
# host                                         # client
sudo ip link set enp1s0f0np0 mtu 9000 up       sudo ip link set enp1s0f0np0 mtu 9000 up
sudo ip addr add 10.0.0.2/24 dev enp1s0f0np0   sudo ip addr add 10.0.0.1/24 dev enp1s0f0np0
ibv_devinfo                                    # the port must be PORT_ACTIVE, link_layer Ethernet
```

The parts find their RDMA device by the link IP (`"rdma_dev": "auto"`: the device whose GID table
holds that address), so the device's name (`mlx5_0`, `rocep1s0f1`, ...) does not matter. Other
addresses go into the configs:

- host: `"link_ip"` in `/etc/rdx/hostd.json` (or `--bind IP` on `rdx_hostd`);
- client: the client page, Advanced: "Host address" and "This computer's link address"
  (`"host"` and `"link_ip"` in `/etc/rdx/client.json`).

**Ports** (TCP on the link IP; seat N adds 100·N): control 47989, host web page 47990 (also on
127.0.0.1), video 18650, input 18660, audio 18670, USB 18680 (+1 for USB/IP over SMC), microphone 18690,
clipboard 18700; RoCE v2 itself is UDP 4791. The client's web page 47991 (window client 47992) listens
on 127.0.0.1 only; a multiseat host's `rdx-seatmgr` listens on 8771. A firewall on the link interface
must let these through; on a dedicated cable the simplest is to trust that interface.

### 6. Optional: multiseat on Proxmox

One LXC container per seat on a Proxmox host, all sharing the RDMA NIC (one macvlan per seat) and each
starting on any free GPU. Run as root on the Proxmox host, in this order:

```sh
seat/install-seatmgr.sh                         # rdx-seatmgr (input/sound/USB to the seats, card choice)
tools/pve-seat-create.sh --ct 3101 --gpu 0000:01:00.0              # a privileged Ubuntu 24.04 container
tools/pve-seat-net.sh --parent enp1s0f0np0 --seat 3101=10.0.1.11/24 --apply   # its RoCE macvlan
tools/pve-seat-provision.sh install --ct 3101 --seat 1 --gpu 0000:01:00.0 \
    --build /path/to/rdma-display --gs /opt/rdx-gs --link-ip 10.0.1.11
tools/pve-seat-provision.sh steam --ct 3101     # Steam's first start, software rendering only
pct start 3101
```

The tools print their usage without arguments; `pve-seat-create.sh --dry-run` and `pve-seat-net.sh`
without `--apply` only print what they would do. The client lists the seats as
hosts (`"hosts"` in its config) and, with `"seatmgr": "http://10.0.1.1:8771"` (the Proxmox host's
link IP; `"listen"` in `/etc/rdx/seatmgr.json`), lets you pick the card each seat starts on. Details, the GPU binding hook, input and security rules:
[docs/MULTISEAT.md](docs/MULTISEAT.md).

Optional on such a host: `tools/rdx-preempt-watch.py` measures, minute by minute, whether the kernel's
preemption mode (`preempt=full`) helps the seats and what it costs the host (install steps in
`tools/rdx-preempt-watch.service`, config `/etc/rdx/preempt-watch.json`).

## Usage

### Start

1. On the host start **RDMA Display Host**, on the client **RDMA Display Client** from the app
   overview (or rely on the enabled units). Each puts an icon in the tray (green while a session
   runs); its menu opens the settings page, starts and stops sessions and quits a paused app.
2. The host's page, `http://127.0.0.1:47990`, lists the apps the client may start. Whole screens are
   there by themselves. **+ Add app** finds installed programs with ready presets (Steam first, then
   Lutris, Heroic, games); Steam runs as `steam -gamepadui` in gamescope, and a Steam already open on
   the desktop is closed first. Apps run as the desktop user.
3. The client's page, `http://127.0.0.1:47991`, shows the host's apps as tiles: one click starts a
   stream. Settings are in plain language, everything technical is under Advanced.

Other launchers are apps with their own command. The repository has starters for **Battle.net**
(`tools/rdx-battlenet`) and the **Rockstar Games Launcher** (`tools/rdx-rockstar`, `--install` installs
it once), both with umu-launcher and GE-Proton (installed by the desktop user) in their own prefix; copy them
to `/usr/local/bin` (`pve-seat-provision.sh update` does that in a seat) and add
an app in `/etc/rdx/hostd.json` (or edit the command of an added app under "App details" on the host
page):

```json
"apps": [
  {"id": "battlenet", "name": "Battle.net", "kind": "gamescope", "cmd": ["rdx-battlenet"]},
  {"id": "rockstar", "name": "Rockstar Games Launcher", "kind": "gamescope", "cmd": ["rdx-rockstar"]}
]
```

### In the stream

| Keys | Action |
|---|---|
| **Ctrl+Alt+Shift+M** | the in-stream menu (stats, pointer, output, microphone mute, clipboard, switch between running apps, disconnect, quit the app) |
| **LeftCtrl+LeftAlt+Esc** | end the stream and return to the client's desktop (the app keeps running on the host) |
| **Ctrl+Alt+H** | lease mode / window client: keyboard and mouse to the desktop and back (`"desktop_hotkey"`; Ctrl+Alt+Shift+Z is an alias and also releases a captured mouse in a window) |
| **F11** | window client: full screen on and off |
| volume / mute keys | change the client's volume, where the stream's sound plays |

- **Background apps**: switching to another app leaves the previous one running. The client's page
  shows every running app with **Resume** and **Quit**, the menu offers "Switch to <App>" and
  "Quit <App>". Little free graphics memory at the start of one more app is a warning that names the
  apps; nothing is closed automatically. **Quit app** (client page, tray, menu, host page) ends an app.
- **Variable refresh**: on by default on both sides ("FreeSync / variable refresh" on the client page,
  "vrr" in the host's defaults); the menu shows whether it is active and its range.
- **Clipboard**: "Share the clipboard with the app" and "Also the selected text" on the client page or
  in the menu; what was copied before the stream starts is in the app at once.
- **Microphone**: "Send the microphone" and the source on the client page; the client's mic-mute key
  and the menu mute it.
- **Keyboard, mouse, USB**: the client page lists the input devices to grab (none chosen = all
  keyboards and mice) and the USB devices to forward; switched-on USB devices move to the host for the
  stream and come back afterwards.
- **Card choice (multiseat)**: with `"seatmgr"` set, the client page shows the cards of the multiseat
  host (free, used by a seat, used by other programs) per seat and starts a seat on the chosen card.
- **Several hosts**: `"hosts": [{"name", "address", "port"}]` in the client config; the page groups
  the apps by host and starting an app of another host switches to it.

Configuration files: `/etc/rdx/hostd.json` (`hostd-seatN.json` per seat), `/etc/rdx/client.json`,
`/etc/rdx/client-window.json`; the web pages write them. Per-frame telemetry is in `/var/log/rdx`
(CSV); `tools/rdx_hitch.py` names the stage of a hitch, `tools/rdx_perf.py` measures a stream end to
end ([docs/PERF.md](docs/PERF.md)).

## Troubleshooting

- **A game freezes for seconds after every mouse or key input (RTX 50, vkd3d-proton / DXVK).**
  gamescope's Vulkan WSI layer is missing, so the game presents through Xwayland and the NVIDIA X11 WSI.
  Run `gs/build_gamescope_wsi_i386.sh` and `host/install-gamescope.sh` again and check that
  `/usr/share/vulkan/implicit_layer.d/` has `rdx-gamescope-wsi.x86_64.json` and
  `rdx-gamescope-wsi.x86.json` (in a seat: `pve-seat-provision.sh update`). `DISABLE_GAMESCOPE_WSI=1`
  turns the layer off for one app.
- **"no RDMA device carries <IP> (rdma_dev auto)"**, or nothing works after a NIC was replaced or
  moved: the link IP is not on the RoCE interface, or a fixed device name in the config no longer
  exists. Keep `"rdma_dev": "auto"` (host config, client page Advanced: "Network device") and check
  the address with `ip addr` and `ibv_devinfo -v` (its GID table).
- **"local protection error" / the GPU or the whole machine fails while streaming from VRAM**: on
  consumer GPUs behind the chipset or a PCIe switch the NIC must not read the GPU's VRAM. The default
  `--buffers auto` uses pinned host memory there; to pin it, set `"gs_buffers": "host"` in the host
  config. Background in [docs/GAMESCOPE.md](docs/GAMESCOPE.md).
- **Steam's window flickers black and white in gamescope on NVIDIA**: the system Xwayland is older
  than 24.1 (no explicit sync). The gamescope build bundles Xwayland 24.1.9; reinstall it with
  `host/install-gamescope.sh`.
- **The client says the host is not reachable**: ping the host's link IP, check that rdx-hostd runs
  (`systemctl status rdx-hostd`) and that the firewall lets the ports above through.
- **The link warning in the menu** ("module passed its warning threshold", "power-cycle needed"):
  the NIC or its transceiver overheats or failed; improve the cooling. A failed card needs a power
  cycle of the machine.
- **Proxmox host with GPU containers (Docker/NVIDIA container toolkit)**: `systemctl daemon-reload`,
  which `seat/install-seatmgr.sh` runs, can take the GPUs away from running Docker containers (CUDA
  then reports `cudaErrorDevicesUnavailable`). Run the installer while no GPU container runs, or
  restart those containers afterwards.
- **Logs**: `journalctl -u rdx-hostd` / `-u rdx-clientd`; the client page lists the parts of a running
  session with a "log" button each (`GET /api/logs?proc=sink|audio|input|usb|mic|clip`).

## Known limits

- **GeForce VRAM behind a chipset or switch**: peer reads of VRAM failed under memory pressure with
  "local protection error" and can take the machine down. gamescope apps therefore use pinned host-RAM
  buffers by default (the GPU writes, the NIC reads RAM); `--buffers vram` is refused unless GPU and NIC
  sit at CPU root ports and a BAR1-pinning driver (`/dev/dmabuf_holder`) is present.
- **Xwayland**: Ubuntu 24.04's Xwayland 23.2 has no explicit sync; the build bundles 24.1.9 next to
  gamescope, the system Xwayland is untouched.
- An AMD or Intel GPU as the render GPU of gamescope apps needs `/dev/udmabuf` (the host-RAM buffers
  are handed over as dma-bufs) and the dma-buf import in the gamescope patch, which is not finished.
- HDR / 10 bit is not offered (the client scans out XRGB8888).

## Measured

On a test pair (host with an RTX 2080 Ti behind the chipset, small client PC), 100G link, MTU 4096:

| what | result |
|---|---|
| whole screen, match mode with phase steering (1080p@143.9992 on a projector) | arrival -> visible p50 0.97 / p99 1.63 ms, 99.9 % one-vblank frame gaps (without steering: p50 6.88 ms) |
| whole screen, immediate capture, DP-1 144 Hz -> 1080p@240 | host vblank -> post p50 1.27 ms, arrival -> visible p50 2.41 / p99 4.45 ms, 0 drops |
| whole screen, tearing vs flips (144 Hz -> 1080p@240) | arrival -> beam at screen centre p50 2.05 ms instead of 4.37 ms |
| gamescope app, host buffers, 1080p@240 | 216 fps paced; tearing: present -> composite done p50 3.16 ms, post -> on screen 1.32 ms |
| Steam in gamescope with the bundled Xwayland | 0 black / 0 white of 3540 frames (before: 934 black, 136 white of 1263) |
| frame generation, app capped at 60 fps, 1080p@120 | 111 frames/s shown instead of 56 (helper: AMD iGPU, 3.4 ms per frame) |
| input, 1 kHz | send -> ack p50 40 / p99 83 us; CPU idle 0 % (event driven) |
| audio, player quantum 64 | post -> device p50 1.85 / p99 2.28 ms, 2.5 % CPU |
| USB HID, 1 kHz | round trip p50 42.6 / p99 67.1 us; usbip bulk 32.6 MB/s each way |

## Repository layout

| path | what |
|---|---|
| `host/` (`rdx_hostd.c`, `rdx_apps.c`, `rdx_mgpu.c`, ...), `web/host/` | host daemon, app detection, multi-GPU options, host web page |
| `client/` (`rdx_clientd.c`, `rdx_menu*.c`, `rdx_placeholder.c`), `web/client/` | client daemon, in-stream menu, start placeholder, client web page |
| `gs/` | gamescope apps: `rdx_gs_src`, the rdx backend patch, frame generation, build scripts |
| `p0b/` | sinks: `rdx_sink_kms` (KMS), `rdx_sink_win` (window), `rdx_lease` (DRM lease), test pattern source |
| `p1/` | whole-screen source `rdx_mirror_src` |
| `p4/`, `p5/`, `usb/`, `clip/` | input, audio and microphone, USB, clipboard |
| `common/` | RDMA setup, control channel, KMS, input, cursor, link health, process supervision |
| `seat/` | multiseat: `rdx-seatmgr`, the GPU binding hook |
| `desktop/`, `tools/` | install script, tray app, udev/polkit rules, systemd units, Proxmox seat tools, measuring tools, launcher starters |
| `p0a/`, `tests/` | feasibility gates, tests (`make test`) |
| `third_party/` | vendored civetweb, cJSON, stb, afmf-linux, xclip's selection library |

## License

MIT, see [LICENSE](LICENSE). Exceptions and third-party code are listed in [NOTICE](NOTICE):

- `third_party/` keeps its own licenses: civetweb, cJSON, stb and afmf-linux (with AMD FidelityFX
  shaders) are MIT, xclip's selection library is GPL-2.0-or-later.
- `clip/rdx_clip.c` links xclip's library and is GPL-2.0-or-later, so the binary `clip/rdx_clip` is
  GPL-2.0-or-later; everything else stays MIT.
- `gs/gamescope-rdx.patch` is a patch to gamescope (BSD-2-Clause) and is provided under that license;
  gamescope, its submodules and Xwayland are downloaded at build time and are not part of this
  repository.
