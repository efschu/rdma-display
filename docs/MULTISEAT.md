# Multiseat: one container per GPU on a Proxmox host

One machine, several independent RDMA Display hosts ("seats"). Each seat is a
privileged LXC container on a Proxmox host that sees exactly one GPU and runs its own
`rdx-hostd@N` with its own ports, its own gamescope and its own apps. A client
lists all seats in its host list and switches between them (client config
`hosts`, see `client/rdx_clientd.c`).

New seats and updates: "Creating and updating seats" at the end. Every seat binds its
GPU at each start for its PCI address ("GPU binding").

## Seats

The example layout used throughout this document (three seats, an RTX 5090 and two
RTX 3080s, one RoCE link to the client):

| CT   | hostname        | seat | GPU (PCI)      | GPU             | NVIDIA node   | DRM card / render        | priority | link_ip     |
|------|-----------------|------|----------------|-----------------|---------------|--------------------------|----------|-------------|
| 3101 | rdx-seat-5090   | 1    | `0000:15:00.0` | RTX 5090        | `/dev/nvidia0` | `card1` / `renderD128` | high     | 10.0.1.11 |
| 3102 | rdx-seat-3080a  | 2    | `0000:18:00.0` | RTX 3080        | `/dev/nvidia1` | `card0` / `renderD129` | normal   | 10.0.1.12 |
| 3103 | rdx-seat-3080b  | 3    | `0000:19:00.0` | RTX 3080        | `/dev/nvidia2` | `card2` / `renderD130` | normal   | 10.0.1.13 |

The device numbers are not in the configs: each start binds them anew for the seat's PCI
address ("GPU binding"). They come from the host itself (`/sys/bus/pci/devices/<pci>/drm`,
`/proc/driver/nvidia/gpus/*/information` "Device Minor"); NVIDIA minors, DRM card
and render indices do **not** follow the PCI order, so never derive one from another.
They also change with the hardware: a NIC or a card more renumbers the PCI bus (seen: the
5090 moved from `0d:00.0` / nvidia1 / card2 to `15:00.0`), and when a second 3080 went in,
the 5090's `card0` became `card1` and the first 3080's `card1` `card0`, so static device
lines of seat 1 named seat 2's card. A change of cards that renumbers the PCI bus needs
the seat's `gpu` in `hostd-seatN.json` and in `/etc/rdx/seatmgr.json`; until then the UUID
pin refuses the start ("GPU binding"). Nothing else is done by hand.
The seats' own sound cards follow from the GPU: `15:00.1` (seat 1), `18:00.1` (seat 2).

The majors of `nvidia-uvm` and `nvidia-caps` are dynamic and can change with any host
boot or driver reload, even without a change of cards: after a reboot with a new NIC
they were 502/508 instead of 505/510, the configs still allowed 505/510, and CUDA in
the container failed (`cuInit` 999, `[vmm] no CUDA device at PCI ...`, `rdx_gs_src`
exit 2). Worse, the old numbers went to other drivers: 505 became `infiniband_mad` (the
NIC's umad) and 510 `fwctl` (firmware control), so a stale line is an open door.

`rdx-seatmgr` handles this live: at every pass it reads `/proc/devices` and allows each
running seat container the CURRENT majors of `nvidia-uvm` (with uvm-tools) and
`nvidia-caps` (`lxc-cgroup devices.allow c N:* rwm`), and revokes every whole-major allow of
the container's config on a dynamic major (≥ 234) that is not one of those now (`devices.deny`,
logged as "stale whole-major allow"; the whole-major lines of the GPU include count as the
config's); fixed majors (< 234) stay. A driver reload while a container runs is followed the
same way. A start takes the majors of that moment from the GPU include ("GPU binding"), with
or without seatmgr. Measured with the old 505/510 put back into the configs: a seat without
a GPU: `/dev/nvidia-uvm` EPERM → ok, `c 505:200` and `c 510:200` ENXIO (allowed) → EPERM; a
seat with a 3080: `cuInit` 999 → 0 with 1 device.

### Ports (every port + 100·N for seat N)

| seat | control | web UI | video | input | audio | USB   |
|------|---------|--------|-------|-------|-------|-------|
| 0 (classic single host) | 47989 | 47990 | 18650 | 18660 | 18670 | 18680 |
| 1    | 48089   | 48090  | 18750 | 18760 | 18770 | 18780 |
| 2    | 48189   | 48190  | 18850 | 18860 | 18870 | 18880 |
| 3    | 48289   | 48290  | 18950 | 18960 | 18970 | 18980 |

Each seat has its own IP, so the offsets are not needed for separation; they keep
seats distinguishable in logs and let several seats share one IP (several seats
per GPU in one container, tested on a test host).

### Seat configs

`/etc/rdx/hostd-seatN.json` inside each container (the template unit
`rdx-hostd@.service` runs `rdx_hostd --repo /opt/rdma-display --seat %i`, which
reads that file):

```json
{"seat": 1, "gpu": "0000:15:00.0", "seat_name": "5090",  "priority": "high",   "link_ip": "10.0.1.11", "rdma_dev": "auto", "desktop_user": "user", "seatmgr": "http://192.0.2.1:8771"}
{"seat": 2, "gpu": "0000:18:00.0", "seat_name": "3080a", "priority": "normal", "link_ip": "10.0.1.12", "rdma_dev": "auto", "desktop_user": "user", "seatmgr": "..."}
{"seat": 3, "gpu": "",             "seat_name": "3080b", "priority": "normal", "link_ip": "10.0.1.13", "rdma_dev": "auto", "desktop_user": "user", "seatmgr": "..."}
```

`seat_name` is the short name; hostd shows it as "RDMA Display - 3080a" (`""`: "Seat N").

`seatmgr` is the URL of rdx-seatmgr as the browser that opens the seat's web UI reaches it
(the page fetches `<seatmgr>/api/seat/N` itself), e.g. the Proxmox host's LAN address, which
must then be in seatmgr's `"listen"`. `seatmgr_ui` (optional) links to a page where the seat's
devices are assigned (a dashboard on top of the seatmgr API).

`desktop_user` runs gamescope and the apps (Steam refuses root). A seat container has no
display manager and no graphical logind session, only the lingering user manager of the
desktop user (pipewire, wireplumber, `/run/user/1000` with its bus); without one, a session
is refused with "nobody is logged in on this computer's desktop". hostd takes, in
this order: `desktop_user` if set, else the user of the active graphical session on seat0,
else - no graphical session at all - the one lingering regular user
(`/var/lib/systemd/linger`, uid 1000..65533; none or several: no user). A real desktop
session always wins over the lingering user, so a desktop PC behaves as
before. `XDG_RUNTIME_DIR`, the session bus and PipeWire (`PIPEWIRE_RUNTIME_DIR`) are that
user's `/run/user/<uid>`. The seat's own monitor needs no session either: gamescope runs
there with the DRM backend and `LIBSEAT_BACKEND=noop` as that user.
`pve-seat-provision.sh install` writes `desktop_user` into the seat config (and adds it to
an existing one that has none). Test: `tests/test_desktop_user` (faked linger directories);
run in a seat's root (chroot, container stopped) it picks the lingering user (uid 1000)
with no `desktop_user` set.

`rdma_dev` defaults to `"auto"`: the RDMA device whose GID table holds the seat's `link_ip`;
every program also takes the source GID carrying that IP, not the first IPv4 GID, so seats
sharing one NIC through a macvlan each send with their own address (`common/rdx_common.h`,
`tests/test_gid_pick.c`). An explicit device name still works. The macvlan (or SR-IOV VF)
per container, its IP and the `/dev/infiniband` entries: `tools/pve-seat-net.sh --parent
<RoCE netdev>` (dry run by default; `--apply` writes), see "Network".

What is in each container: `/opt/rdma-display` (built against the open GPU kernel module
headers of the host's driver), the patched gamescope under `/opt/rdx-gs` with its private
prefix, `rdx-hostd@N.service` enabled, the polkit rule
(`/etc/polkit-1/rules.d/50-rdma-display.rules`), the app entry
`rdx-host-seatN.desktop`, the NVIDIA userspace of the host's driver version (no kernel
module), the desktop user with a lingering user manager (pipewire, wireplumber
without a display manager), the runtime packages (all binaries resolve their libraries,
`ldd` "not found": none), Steam bootstrapped for the desktop user. The build tree is in
`/opt/rdma-display-src`.

## GPU isolation

`nvidia-smi` inside a container lists **every** GPU of the machine (it talks to
nvidiactl), but only the seat's own `/dev/nvidiaN` is bound in and allowed by the
device cgroup. Per container, `/etc/pve/lxc/<id>.conf` has the GPU-independent part

```
lxc.cgroup2.devices.allow: c 195:254 rwm         # nvidia-modeset
lxc.cgroup2.devices.allow: c 195:255 rwm         # nvidiactl
lxc.cgroup2.devices.allow: c 231:<minor> rwm     # the RoCE port's uverbs node only (pve-seat-net.sh)
lxc.include: /run/rdx-seat/<id>.gpu.conf         # the GPU's lines, written at every start
lxc.hook.pre-start: /opt/rdx-seatmgr/rdx_seat_gpu.py
hookscript: local:snippets/rdx-seat-gpu
```

and the include, written by the hookscript at every `pct start` for the seat's PCI address
(here seat 1, the 5090):

```
lxc.cgroup2.devices.allow = c 195:0 rwm          # this seat's /dev/nvidiaN only
lxc.cgroup2.devices.allow = c 226:1 rwm          # this seat's DRM card
lxc.cgroup2.devices.allow = c 226:128 rwm        # this seat's render node
lxc.cgroup2.devices.allow = c 502:* rwm          # nvidia-uvm, nvidia-uvm-tools (dynamic major, see "Seats")
lxc.cgroup2.devices.allow = c 508:* rwm          # nvidia-caps (dynamic major)
lxc.mount.entry = /dev/nvidia0 dev/nvidia0 none bind,create=file
lxc.mount.entry = /dev/dri/card1 dev/dri/card1 none bind,create=file
lxc.mount.entry = /dev/dri/renderD128 dev/dri/renderD128 none bind,create=file
```

Never allow `c 195:*` or `c 226:*`: with that, a `/dev/nvidia0` created inside the
container (nvidia-smi / nvidia-modprobe do that) opens another seat's GPU. Each container
allows only its own minors; nodes such as `/dev/nvidia0` and `/dev/nvidia1` made inside a
seat stay closed by the cgroup.

## GPU binding

The GPU of a seat follows its PCI address (`gpu` of the seat in `/etc/rdx/seatmgr.json`,
the same as in its `hostd-seatN.json`), at every start, after a re-plug, a host reboot or
a driver reload, without a hand step. `seat/rdx_seat_gpu.py` (installed by
`seat/install-seatmgr.sh` to `/opt/rdx-seatmgr/` and linked into the snippets storage
`RDX_SNIPPETS`, default `local`, as `local:snippets/rdx-seat-gpu`; the storage needs the
content type "snippets") does it in two places:

- **PVE hookscript, pre-start.** Proxmox writes `/var/lib/lxc/<id>/config` from
  `/etc/pve/lxc/<id>.conf` *before* the hookscript and `lxc-start` reads it, includes and
  all, *after* it (`PVE::LXC::vm_start`). The hook reads the GPU's numbers for its PCI
  address at that moment (NVIDIA minor and UUID from procfs, card and render node from
  `/sys/bus/pci/devices/<pci>/drm`, the majors of `nvidia-uvm`/`nvidia-caps` from
  `/proc/devices`), checks them, writes `/run/rdx-seat/<id>.gpu.conf` and prints what it
  bound (task log, journal `-t rdx-seat-gpu`). Any doubt: exit 1, Proxmox does not start
  the container. post-stop deletes the file: a start that bypasses the hookscript
  (`lxc-start`, `systemctl start pve-container@<id>`) fails on the missing include
  (`Failed to load config`) instead of binding last time's numbers.
- **LXC pre-start hook.** Inside every `lxc-start`, also the restart after a reboot from
  inside the container (Proxmox's post-stop hook restarts it without `vm_start`, so without
  hookscript, and the include of the first start stays), the GPU lines lxc is about to
  apply are compared with the host's numbers now. A difference: exit 1, lxc aborts.

Inside the container the nodes keep the host's kernel names (`/dev/dri/card1`, not
`card0` and not a `by-path` name): sysfs there is the host's, and wlroots/gamescope open
the devnode udev names for it.

Refused, with the reason (rather no start than the wrong card):

- the container is no seat in `seatmgr.json`, two seats name the same container, the GPU is
  not one of the seat's `gpus`, or another **running** seat has the same `gpu` ("Card choice");
- the address is no NVIDIA display device bound to `nvidia`, has no procfs entry, not
  exactly one card and one render node, a host node (`/dev/nvidiaN`, `/dev/dri/cardN`,
  `renderD`) has other numbers than procfs/sysfs, `/sys/dev/char/226:N` belongs to another
  device, or `/dev/dri/by-path/pci-<pci>-card|render` points elsewhere;
- the GPU's UUID differs from the pin of its PCI address in `/etc/rdx/seat-gpu.pins.json`
  (`{"cards": {pci: {uuid, model}}}`, written at the first good start on that card; per-seat
  pins of the earlier format are taken over): a card added or taken out renumbers the bus,
  and an address may then be another card. A seat that switches cards is no swap ("Card
  choice"). After a swap on purpose `rdx_seat_gpu.py --unpin <pci>` (or a seat's CT id: its card);
- another **running** seat has one of the GPU's nodes in its config, or the seat's own
  config allows a node of another GPU, a wildcard (`c 195:*`, `c 226:*`, `a`) or a whole
  dynamic major (≥ 234) that is neither `nvidia-uvm` nor `nvidia-caps` now (a leftover
  static line next to the include);
- `hostd-seatN.json` in the seat's root file system names another `gpu` (pre-start only).

A seat with `"gpu": ""` starts without any GPU node. A running container that is no seat
and allows the same GPU (e.g. one that allows `c 195:*`) is only named in a note.

```
/opt/rdx-seatmgr/rdx_seat_gpu.py --status          # every seat: its GPU now, checked; rc 1 on a problem
/opt/rdx-seatmgr/rdx_seat_gpu.py --check 3101      # the include the next start would write, or why not
/opt/rdx-seatmgr/rdx_seat_gpu.py --write 3103      # the include now (a running seat's inside reboot)
/opt/rdx-seatmgr/rdx_seat_gpu.py --pin 0000:15:00.0 | --unpin 0000:15:00.0
/opt/rdx-seatmgr/rdx_seat_gpu.py --cards           # every card: numbers, pin, seats, IRQ target
/opt/rdx-seatmgr/rdx_seat_gpu.py --irqs            # move the seat cards' IRQs now
journalctl -t rdx-seat-gpu                         # what each start bound or why it was refused
```

Checked without starting a seat: the hookscript's pre-start for 3101 wrote
`c 195:0`, `c 226:1`, `c 226:128`, `c 502:*`, `c 508:*`, `card1`, `renderD128`; lxc's own
parser (`lxc-info -c lxc.cgroup2.devices.allow`) reads exactly these from the include; the
lxc hook passed, and after post-stop `lxc-info` and the lxc hook refused (include missing).
A config with the old static lines (`/var/lib/lxc/3101/config`) is refused by the lxc hook with
"allows c 226:0, the node of 0000:18:00.0 (card0) ... does not bind /dev/dri/card1".
A seat can be migrated while it runs a session (`--update`: include written first, then the
config; its init pid and its allows unchanged).

Prior art: Proxmox's `devN: /dev/dri/by-path/...` resolves the link at start but names
the node in the container by that path (no `/dev/dri/cardN`) and cannot name
`/dev/nvidiaN` by PCI address; the community answers to the moving card numbers
(`initcall_blacklist=simpledrm_platform_driver_init`, a udev symlink, rewriting `devN` at
boot) fix one GPU or rewrite the config outside the start; a bind of the by-path link to a
fixed name needs `c 226:*`. None binds per PCI address at the start itself.

How the software copes:

- `rdx_hostd` takes the configured `gpu`. Without one it picks the first GPU from
  `nvidia-smi` whose device node (minor from
  `/proc/driver/nvidia/gpus/<bdf>/information`) is openable, so the default is
  right inside a seat container too (`nvidia-smi` has no minor field).
- `rdx_gs_src --gpu <bdf>` maps it with `cuDeviceGetByPCIBusId` and sets
  `GAMESCOPE_VK_DEVICE_UUID`, so gamescope's Vulkan device is that GPU's
  (the patch filters in `selectPhysDev`).
- Mirror apps get the seat GPU's `/dev/dri/by-path/pci-<bdf>-*` nodes. The binds
  above create no `by-path` inside the container, so hostd then looks the bound
  `card*`/`renderD*` nodes up by `/sys/dev/char/226:N/device` (`drm_node_for`;
  checked on a test host against its by-path links). Without that it took the config
  default `card1`/`renderD128`, another seat's GPU for every seat but the one
  that card belongs to.
- The DRM nodes keep the host's `render` gid (103 on the Proxmox host here), which inside
  the Ubuntu container is `ssl-cert`. The setup adds a group `hostdri` with that gid and puts
  the desktop user in it (`id user` shows it as `103(ssl-cert)`, same id).

Priority instead of a VRAM limit: `priority` high|low sets nice -5|10 and
ioprio BE 0|7 of the seat's gamescope and its apps and the Vulkan global queue
priority (`GAMESCOPE_VK_QUEUE_PRIORITY`); normal leaves them as they are. The GPUs of
the seats are disjoint here, so priority matters only for CPU and disk; it
matters for the GPU when several seats share one.

Fixed device entries every seat container has besides its GPU: `/dev/uinput`
(10:223) and `/dev/uhid` (10:239) for the streamed input, `/dev/dmabuf_holder`
(an out-of-tree BAR1 pinning driver, when the host has it), and `/dev/udmabuf` (its
misc minor differs between hosts, e.g. 10:259 or 10:124: check with `ls -l /dev/udmabuf`;
`pve-seat-create.sh` reads it from the node):

```
lxc.cgroup2.devices.allow: c 10:259 rwm
lxc.mount.entry: /dev/udmabuf dev/udmabuf none bind,optional,create=file
```

udmabuf is needed only by a seat that renders on, or takes its frame-generation
helper from, an AMD or Intel GPU; for NVIDIA-only seats the entry is harmless (`optional`).

## Card choice

By design every seat keeps its own container (its data, its Steam and Battle.net logins) and
starts on any card that is free at that moment; the card is no longer part of a seat's identity
(names "Seat 1", "Seat 2", "Seat 3"; the CT hostnames `rdx-seat-5090` etc. stay, a launcher may
tie its login to the hostname).

- `seatmgr.json`: `"gpu"` of a seat is its current card (its last choice), `"gpus"` (optional)
  the cards it may take, default every NVIDIA display device. Two seats may name the same card;
  only one may run with it (the hook refuses the other, "GPU binding").
- Pins are per card (`{"cards": {pci: {uuid, model}}}`): a seat moving to another card is no
  "swap"; a card added or taken out (renumbered bus) or replaced still is (`--unpin PCI`). The
  per-seat pins of before are taken over for their addresses at the next start.
- `rdx-seatmgr` `GET /api/gpus`: every card with its state: `free`, `seat` (a running seat has
  it), `llm` / `busy` (processes outside the seats use it: nvidia-smi's compute apps plus the
  open `/dev/nvidiaN`/DRM nodes in the host's `/proc`; `llm` when one of them is an LLM
  inference server, by process name, `"llm_names"`, default sglang, vllm, llama, ollama,
  text-generation; a process holding the `/dev/nvidiaN` of every card and no DRM node and no
  context is an NVML monitor -- nvtop, an exporter, a dashboard -- and does not count),
  `booked` (booked in the optional GPU booking service: a running window of another owner, read
  from `<"gpuq">/api/v1/cards`; `"gpuq": ""`, the default, asks none), `unusable`.
- `POST /api/seat/start {"seat": N, "gpu": PCI, "force": false, "gpuq_booking": ID}`: the checks
  above, refused with the reason ("RTX 5090 (0000:15:00.0): it is used by an LLM server:
  sglang::scheduler_TP0 (pid ..., docker ...)"); a running seat with a stream, a paused app or
  an app on its monitor (its hostd's `/api/state`, asked with `pct exec`) only with `force`;
  then `pct shutdown` if it runs, `"gpu"` into `seatmgr.json` and into the seat's
  `/etc/rdx/hostd-seatN.json` (a card-like `seat_name` such as "3080b" is cleared), `pct start`
  (the hookscript binds and checks again). A failed start puts the old card back and starts the
  seat there if it ran. Accepted only from `"control_from"` (default `127.0.0.0/8` and
  `10.0.1.0/24`, the RDMA cable to the client; add the link address `10.0.1.1:8771` to
  `"listen"` so the client reaches seatmgr).
  `gpuq_booking`: the caller's own booking-service window for the card does not count against
  it; windows of owners starting with `"gpuq_seat_owners"` (default `rdx-seat`) neither.
- IRQs: the seats run on their own CPUs (`lxc.cgroup2.cpuset.cpus` of the container, e.g.
  `8-15,24-31` for all three seats); every PCI function of the card a running seat has (GPU and
  its HDMI sound `.1`) gets its interrupts there, and back on `/proc/irq/default_smp_affinity`
  when no running seat has it: in the hookscript's post-start and post-stop and in seatmgr at
  every container start/stop it sees (`rdx_seat_gpu.py --irqs` by hand, `--cards` shows the
  targets). A seat without a cpuset leaves its card's IRQs where they are.
- hostd follows `"gpu"` in everything card-dependent: DRM nodes by sysfs, the monitor on that
  card's connectors, `rdx_gs_src --gpu` (CUDA by PCI bus id, `GAMESCOPE_VK_DEVICE_UUID` and
  `--prefer-vk-device` from the Vulkan device of that UUID), and the local gamescope on the
  seat's monitor (`GAMESCOPE_VK_DEVICE_UUID` from `/proc/driver/nvidia/gpus/<pci>/information`,
  `--prefer-vk-device` from the card's PCI ids). hello `seat_name` is "RDMA Display - Seat N",
  `gpu_name` the card.
- Client (`rdx-clientd`, config `"seatmgr": "http://10.0.1.1:8771"`, hosts `{"name": "Seat 1",
  ..., "seat": 1}`): `GET /api/seats` passes `/api/gpus` through, `POST /api/seats/start {"host",
  "gpu", "force"}` calls seatmgr (refused while this client streams from that seat). The page
  shows under each seat its card now and a list of the cards it may take with their state
  ("RTX 3080 (19:00.0) – free", "– Seat 3", ...), and "Start" / "Restart on this
  card": seatmgr starts it, the page waits for the seat's hello on that card and starts the
  session (the configured app). The in-stream menu has no host list: switching cards needs the
  stream ended anyway.

```
curl -s 10.0.1.1:8771/api/gpus | jq '.cards[] | {pci, short, status, why}'
curl -s -X POST -d '{"seat": 1, "gpu": "0000:19:00.0"}' 10.0.1.1:8771/api/seat/start
/opt/rdx-seatmgr/rdx_seat_gpu.py --cards
```

Prior art: Proxmox hookscripts and forum answers pick or switch a GPU for VMs (vfio
rebinds, `qm` stop of the other VM) or write `devN:` lines outside the start; LXC's
`/usr/share/lxc/hooks/nvidia` with `NVIDIA_VISIBLE_DEVICES=<uuid>` (libnvidia-container) binds one
GPU by UUID but only its `/dev/nvidiaN` (no DRM card/render node for gamescope), injects the
host's driver libraries over the container's own and checks no occupancy; Sunshine/Apollo pick
one adapter per instance (`adapter_name`) and run several instances for several GPUs, with no
"is this card free" check. None checks a card against other seats, foreign compute processes
and a GPU booking before the start, so this is built on the existing hook.

## Input, sound and USB: rdx-seatmgr

The containers bind **no** `/dev/input` and allow no input major (with the whole
`/dev/input` bound and `c 13:*` allowed, every seat sees every keyboard). Local
keyboards, mice, gamepads, sound cards and USB devices reach a seat only through
`rdx-seatmgr` (`seat/rdx_seatmgr.py`), a small root service on the Proxmox host:

- One **device** is one physical thing: a USB device with all its nodes
  (`event*`, `mouse*`, `js*`, `hidraw*`, its sound card, `/dev/bus/usb/BBB/DDD`),
  a PCI sound card (`controlC*`, `pcmC*`, `hwC*` and its HDMI/jack inputs), or a
  lone platform input. Every device belongs to at most one seat. USB devices the
  host itself uses (a bound network or bluetooth driver and no input/sound/hidraw
  node, e.g. a USB LAN adapter) are never handed out; `/dev/input/mice` never.
- Which seat, first match wins: a runtime choice (claim or manual; lasts until
  unplug) > a device rule (`usb:VVVV:PPPP[:serial]`) > a port rule (udev `ID_PATH`,
  the USB port or PCI slot, the key logind's multiseat rules use) > the seat's own
  GPU sound card (function `.1` of its GPU, by PCI path, never by card number; or the
  seat's `sound`) > the **host console**. A newly plugged device belongs to the Proxmox
  host's own console (by design: host console and seats only by assignment); a seat
  gets a physical device only by a rule, a claim or a choice in a dashboard. The host
  console is a target of its own (`"seat": "host"` in `/api/assign` and in rules), so a
  port or a device can be remembered for the console too, e.g. to keep a seat's GPU sound
  card on the host.
- Grab guard: a seat's keyboard must be read by the seat alone. The host's VT
  keyboard handler sees every keyboard of the host kernel, whatever container its
  node is bound into, so taking a node out of a container protects nothing: the
  guard grabs instead. Who grabs what in a seat container (`EVIOCGRAB`): gamescope's
  rdx backend (a stream) grabs the client's devices only (name or phys `rdx:`, or
  behind `vhci_hcd`; before gamescope starts the rx/usb_host gate guards those), its
  DRM backend (the seat monitor) grabs every keyboard and pointer, never a joystick;
  `RDX_GS_GRAB=0`: nothing. For every keyboard-capable, non-joystick node (a key
  below `BTN_MISC`: what the VT handler takes) of a device handed to a running seat:
  - no running gamescope would grab it: seatmgr grabs it at once and its keys go
    nowhere (nobody in the seat would read it anyway) -- every such keyboard,
    whatever its name or creator: a device named "rdx: …" but made by some other
    process in the container is no client device (one such keyboard reached tty11
    while seatmgr still left "rdx:" names to the rx/usb_host gate).
    Where the gate holds it already, seatmgr's grab answers `EBUSY`: fine, it tries
    again each pass. A new input node is handled on its kernel uevent (`udevadm
    monitor --kernel`), without the 0.3 s gathering of a plug's burst: measured
    76 ms from creating a virtual keyboard in a seat container to seatmgr's hold, 0 of 5
    F12 (the first 0.3 s after creation) at a second, non-grabbing host reader;
  - a gamescope that would grab it runs: seatmgr lets go at once (it looks for new
    gamescope processes every 50 ms; gamescope retries an `EBUSY` grab 0.5 s since
    rdx-gs9, 25 × 2 ms before), and 5 s after the later of: that gamescope appeared
    (rx makes a session's devices 1–3 s before gamescope starts), the device came (a
    resume or a hotplug brings it to a gamescope that runs already; counting from the
    gamescope alone held a resumed window keyboard) it probes the
    node once a second with a non-destructive `EVIOCGRAB` (`EBUSY`: grabbed; success:
    nobody holds it, released at once, nothing in between). Free for more than 1 s:
    seatmgr holds it until a new gamescope runs, or until a grabbing gamescope opens
    the node anew (an fd on it in `/proc/<pid>/fd` that was not there at the hold:
    libinput's hotplug, a resume): then it lets go in the next pass (≤ 0.2 s) so
    that open's grab, retrying 0.5 s, finds it free; held again only when that open
    does not grab it either.
  The assignment and the node stay; the state shows it as held ("its keys
  go nowhere"). The guard acts while the consoles read keyboards
  (`host_console_keyboard: true`) or with `"grab_guard": "enforce"`; while `K_OFF`
  protects the consoles it only logs what it would hold. Measured in a seat container
  without a GPU with a virtual keyboard made in the container and a stand-in
  DRM gamescope (python started as `gamescope … --backend drm`, grabbing at once
  with gamescope's 25 × 2 ms retries): seatmgr held it while no gamescope ran (a
  second, non-grabbing host reader like the VT handler: 0 of 12 presses), let go in
  time (the stand-in's grab succeeded at the first try and it read 8 of 8), held it
  again after it ended (0 of 8). With a 200 ms check instead of 50 ms the stand-in
  lost its 50 ms window once, so gamescope's retry window should be longer (≥ 0.5 s)
  to be safe against a loaded host.
- Rules live in `/etc/rdx/seatmgr.json` (seats: `ct`, `name`, `gpu`, optional
  `gpus`, `sound`), runtime choices in `/run/rdx-seatmgr/runtime.json`.
- Applying, for each **running** seat container: `lxc-device add <node>` (node in
  the container plus the cgroup2 allow, no restart), owned by the container's
  own `input`/`audio` group, the host's udev record copied to
  `/run/udev/data/`, and an add uevent sent into the container's network
  namespace in libudev's netlink format (pipewire, libinput and `udevadm
  monitor` there see the hotplug; seatmgr creates `/run/udev/control`, without
  which libudev monitors stay deaf in a container without udevd). A device that
  moves away or is unplugged: remove uevent, node deleted, allow revoked
  (`lxc-cgroup devices.deny`) before its minor can come back as another seat's
  device. An event node goes in only once the host's udevd has written its record
  (by rename: once it exists it is complete), at most 3 s, then bare with a log
  line: the kernel uevent's rescan comes before udevd, and a node announced without
  `ID_INPUT_*` is skipped by libinput for good (seen: gamescope grabbed the window
  mouse and pointer, never the window keyboard, until a seatmgr restart announced it
  again). `/dev/snd/timer` and `seq` go with any sound card. A container that
  starts gets its devices within a second; a seatmgr restart sweeps nodes of the
  managed majors (13, 116, 189, hidraw) that are not wanted any more.
- An ALSA card is a udev device of its own (`/sys/class/sound/cardN`, no node):
  its record `+sound:cardN` (with `SOUND_INITIALIZED=1`) is copied too and a
  `change` uevent sent after the card's nodes, `remove` before they go. Without
  it pipewire's ALSA monitor ignores the card and only "Dummy Output" exists.
  Without a monitor on the GPU every HDMI profile is "available: no" and the card
  stays on profile "off" (no sink): correct, nothing to play on.
- **Virtual devices** (uinput: rdx_input_rx's streamed keyboard/mouse/pad,
  Steam Input; uhid: rdx_usb_host's HID mirrors) are created inside a seat
  container, but their nodes appear in the HOST's devtmpfs, so without a
  `/dev/input` bind the container would never see them. They belong to the
  container that created them, automatically and without a rule: seatmgr reads
  the open `/dev/uinput` / `/dev/uhid` fds of the seat containers' processes
  (cgroup `lxc/<ct>`); every fd creates at most one device, so a new device goes
  to the container if all fds not yet matched to a device are there. If they
  are in several containers it goes nowhere until that is clear (state
  `virtual_pending`), never to a wrong seat. Virtual devices take no rules, no
  claim and no default, cannot be assigned by hand and are never "unassigned";
  the matches survive a seatmgr restart (`/run/rdx-seatmgr/virtual.json`).
  Measured: node in the container 0.33 s after `UI_DEV_CREATE`
  / `UHID_CREATE2` (event and hidraw), libinput sees it, gone 0.3 s after close.
- Claim: a seat asks "next key press is mine" (`POST /api/claim`); the first
  key/button press on an unassigned input device within the time assigns it.
  Activity ("blink"): a key press or mouse motion flags the device for 2 s.
- API on the addresses of `"listen"` in `/etc/rdx/seatmgr.json` (built-in default
  `127.0.0.1:8771`; the shipped `seat/seatmgr.json` adds the link address `10.0.1.1:8771`;
  add the host's LAN address, e.g. `192.0.2.1:8771`, for browsers on the LAN; an address
  the host does not have is retried for 60 s at the start, then skipped):
  `GET /api/state`, `GET /api/seat/N`, `GET /api/gpus`, `POST /api/assign {"id","seat","remember":
  null|"port"|"device"}`, `POST /api/forget {"id"}`, `POST /api/claim {"seat",
  "seconds"}`, `POST /api/claim/cancel {"seat"}`, `POST /api/seat/start` ("Card choice").
  seatmgr has no page of its own; a dashboard can be built on this API (CORS is open).
  rdx-hostd's web UI lists its seat's devices from `/api/seat/N` and has the
  claim button when the seat config names `"seatmgr"` (e.g. `"http://192.0.2.1:8771"`),
  and links to `"seatmgr_ui"` when set.
- Install: `bash seat/install-seatmgr.sh` on the host (`/opt/rdx-seatmgr`,
  `rdx-seatmgr.service`, `/etc/rdx/seatmgr.json` from `seat/seatmgr.json` if there is none;
  `--no-start` leaves the service stopped); `rdx_seatmgr.py --list` prints devices and seats,
  `--dry-run` scans and prints what would be applied. Tests: `python3 seat/test_seatmgr.py`
  and `python3 seat/test_seat_gpu.py` (fake sysfs tree, no root).

Measured on the multiseat host (seat 1 started for the test with `rdx-hostd@1`
held off, no GPU use): 16 nodes in the container within a second of its start;
a foreign minor (13:77) and a revoked one (13:66) both `EPERM`; `udevadm
monitor --udev` inside saw add/remove with `ID_INPUT_MOUSE`; claim and blink
with events written into a USB mouse's evdev node; a seatmgr restart with
the mouse moved to seat 2 removed its four nodes from 3101.

## Security: the host never reads a seat's keyboard

Every keyboard of a seat is an input device of the **host** kernel: the uinput/uhid ones a
container creates (streamed keyboard, Steam Input, rdx_usb_host's HID mirrors) as much as a
physical one seatmgr hands to a seat. Two readers on the Proxmox host would take their keys:

- **The VT keyboard handler** feeds every keyboard into the active text console, e.g. tty1
  with a getty on a GPU's HDMI: keys pressed into a seat restarted that getty, and a password
  typed in a seat would land at the host's login prompt. The lasting protection is the
  exclusive grab: the seats' consumers (gamescope, rdx_input_rx) grab their devices with
  `EVIOCGRAB`, so the VT handler never sees seat input, and seatmgr's grab guard (above) holds
  a seat keyboard nobody in the seat grabs. So the host's physical keyboard keeps working on
  the console (by design: switching it off is no option): `"host_console_keyboard"` is `true`
  by default, and the modes the consoles had are restored once (tty1–12: 3, `K_UNICODE`; a
  console at `K_OFF` without a mode seen before gets `K_UNICODE`). `false` locks the consoles:
  rdx-seatmgr keeps the keyboard mode of the host's VTs (tty1..12 and the active one) at
  `K_OFF` (`KDSKBMODE`) at its start, after every container start or stop and once a second,
  because a restarting getty, a DRM gamescope giving the console back or `chvt` set it again
  (`ConsoleKeyboard`, logged as "host consoles: keyboard off"); the modes it found are kept in
  `/run/rdx-seatmgr/vt-kbd-modes.json`. SysRq works either way (its own input handler).
  Measured with the real service: a virtual keyboard made in a seat container (no gamescope),
  held by seatmgr, let go to a stand-in DRM gamescope (its grab succeeded at the first try, it
  read 8 of 8 F13), held again after it ended; a second, non-grabbing host reader like the VT
  handler got 0 F13 in all three phases, the console's getty was not restarted.
- **systemd-logind**: systemd 257's `70-power-switch.rules` tags *every* key device
  `power-switch`, and the host's logind watched the seats' keyboards ("Watching system buttons
  on /dev/input/event20 (rdx: window keyboard (desktop))"); with `HandlePowerKey=poweroff` a
  power key pressed in a stream would switch the host off, a sleep key suspend it.
  `seat/70-power-switch.rules`, installed by `install-seatmgr.sh` into `/etc/udev/rules.d`
  where it shadows systemd's file of that name, gives the tag only to the host's ACPI/platform
  buttons; seatmgr warns at its start when it is missing. (Taking the tag away in a later rule,
  `TAG-=`, is not enough: it stays in `TAGS`, and logind still watches the device.)

Inside the seat containers nothing but the seat's own programs reads these devices: a
container has no VT (`/dev/console` and `/dev/tty1`, `tty2` are LXC ptys for `pct console`,
their gettys get no keyboard input), and the only process with an input node open is the
seat's gamescope. Tests: `ConsoleKeyboardTest` in `seat/test_seatmgr.py` (fake open/ioctl).

## Network

One RoCE port of the host, one macvlan per container, only that port's uverbs node bound in.

Example: a ConnectX-5 Ex (MT28800) at `08:00.0`/`.1`, PCIe Gen4 x4 (16 GT/s, about
7.9 GB/s raw), port 0 `enp8s0f0np0` = RDMA device `rocep8s0f0` (`uverbs0`, c 231:192), host IP
10.0.1.1/24, MTU 9000, cabled straight to the client's NIC (10.0.1.2). Over a 40G DAC
ib_write_bw client -> host reaches 39.21 Gbit/s (line rate); with 100G the Gen4 x4 slot is the
limit. A ConnectX-4 at Gen3 x4 behind the chipset carried about 15-16 Gbit/s of payload.

1. `tools/pve-seat-net.sh --parent enp8s0f0np0 --seat 3101=10.0.1.11/24 --seat
   3102=10.0.1.12/24` (dry run; then `--apply` with the containers stopped) writes
   per container raw lines into `/etc/pve/lxc/<id>.conf`. Not a `pct set --net1
   bridge=…` veth: RoCE GIDs live on the NIC's netdev and its upper devices, a veth
   behind a Linux bridge has none. A macvlan of the RoCE port is an upper device:

   ```
   lxc.net.10.type: macvlan                         # lxc.net.11 for the second seat given, ...
   lxc.net.10.link: enp8s0f0np0
   lxc.net.10.macvlan.mode: bridge
   lxc.net.10.name: eth1
   lxc.net.10.ipv4.address: 10.0.1.11/24          # .12 for 3102, .13 for 3103
   lxc.net.10.mtu: 9000
   lxc.net.10.flags: up
   lxc.cgroup2.devices.allow: c 231:192 rwm         # uverbs0 only, not the whole major
   lxc.mount.entry: /dev/infiniband/uverbs0 dev/infiniband/uverbs0 none bind,optional,create=file
   ```

   The seats use plain verbs (no rdma_cm), so `/dev/infiniband/uverbsN` is all they
   open; `ibv_devinfo` comes with `ibverbs-utils` (in the seat package list).
   Proxmox moves every comment line of a config into its description when it
   rewrites the file (`pct mount`, `pct set`), so the script finds its lines by
   content (raw `lxc.net.<10..>`, `c 231:<minor>`, the uverbs mount) and a re-run
   replaces them instead of adding a second block.
2. The mlx5 driver adds a RoCE v1 and v2 GID for every IP of the port's upper
   devices. The RDMA devices are not network-namespaced (`rdma system show`: netns
   shared), so every container sees the whole GID table, the host's and the other
   seats' entries included; each program takes the GID carrying its own `link_ip`
   (`rdx_find_gid_index_ip`, `common/rdx_common.c`) and with `rdma_dev` `"auto"`
   the device holding it. Example in seat 2: index 3 = 10.0.1.1 (host), index 5 =
   10.0.1.12 (RoCE v2, `eth1`), the tools print
   `[gid] rocep3s0f0 index 5 = 10.0.1.12 (RoCE v2), link IP 10.0.1.12`.
   The macvlan gets a new random MAC at every container start; runs right after a
   restart connected at once (ARP), no stale neighbour entry was seen.
3. Measured (ConnectX-4 at Gen3 x4 behind the chipset; containers started with
   `"picture": "stream"` for the test, no GPU process of the container at any time; the
   client at 10.0.1.2):

   | seat | jumbo ping 8972 B to 10.0.1.2 | audio CT → client (null sink) | input client → CT (probe, 200 Hz) |
   |------|------|------|------|
   | 3101 | 0 % loss, 0.13 ms | net p50 1.72 / p99 1.76 ms, e2e p50 3.1 / p99 13.6 ms, gaps 0 | 1494/1494 acked, round p50 0.051 / p99 0.48 ms, 0 misses |
   | 3102 | 0 % loss, 0.14 ms | net p50 1.77 / p99 5.1 ms, e2e p50 6.2 / p99 9.1 ms, gaps 0 | 2000/2000 acked, round p50 0.047 / p99 0.83 ms, 0 misses |

   Audio: 0 content errors, 0 underruns at the receiver's 10 s mark; its final count
   has some (3101: 62, 3102: 2) from between that mark and the end, where the
   sender (18 s) stops before the receiver (25 s). Input 3101: the receiver's 25 s
   timeout ended the run after 1494 of 2000 probes.

   First GPU stream of a seat (3102, the 3080):
   `rdx_gs_src --gpu 0000:18:00.0 --buffers host --user user -- vkcube` in the
   container → `rdx_sink_null` 1920x1080@240 on the client, 19 s: 4071 frames =
   213 fps, shown 3982 (199/s), dropped 88, 216 without a free slot, CQE → vblank
   p50 3.50 / p90 3.84 / p99 4.41 ms, post → shown report p50 6.0 ms, GPU done →
   post p50 0.008 ms; only the 3080 in use (gamescope + vkcube at most 154 MiB);
   last frame the cube (56 % non-black). In the container `/dev/cpu_dma_latency`
   is missing (PM QoS off, warning only).

   Sink slots (same setup, two runs each):

   | sink slots | fps | shown/s | dropped | without a free slot | lead |
   |-----------|-----|---------|---------|---------------------|------|
   | 3 | 222.7 / 223.0 | 210.5 / 212.4 | 73 / 77 | ? / 124 (first run: 216) | 8.2 / 7.8 ms |
   | 4 | 228.3 / 230.1 | 218.4 / 220.6 | 48 / 54 | ? / 0 | 8.0 / 8.5 ms |

   A slot is gamescope's from its wake (the lead before the sink's vblank) until the sink
   returns it one period after showing it; at 240 Hz with an 8 ms lead that is 2.95 periods,
   so 3 slots run dry whenever a frame is slow. The lead is long here because the host's
   NIC sat at PCIe Gen3 x4 behind the chipset (8.3 MB per frame ≈ 2.5 ms DMA; at Gen3 x8
   ≈ 1.3 ms). Two 1080p@240 seats need ≈ 4 GB/s, more than that slot
   carries (≈ 3.3 GB/s). Default now: the client gives its sink
   `rdx_pace_slots(refresh, lead)` slots (`common/rdx_ctl.h`: one more while lead + period
   > (slots − 0.5) periods); the lead is the seat's last settled one (`rdx_gs_src
   --lead-file`, hostd's hello `stream_lead_ms`), 8 ms before the first stream, so 4 slots
   from ~188 Hz. `"sink_slots_auto": false` in the hostd defaults turns it off. The
   remaining ~50 drops at 4 slots have another cause (arrival jitter, not slots).
   The input mirror inside the container (`rdx: rdx-probe`) was handed to that
   container by seatmgr through its uinput fd (virtual devices, above).
4. Alternative with real isolation: SR-IOV, one VF per container
   (`tools/pve-seat-net.sh --sriov`). A ConnectX-4 offers 3 VFs
   (`sriov_totalvfs`); then every container has its own RDMA device and GID table
   (`rdma system set netns exclusive`). Not needed for the GID pick, which already
   keeps the seats apart.
5. A client on the link reaches the seats by their link IPs; its config:

   ```json
   "hosts": [
     {"name": "RDMA Display - 5090",  "address": "10.0.1.11", "port": 48089},
     {"name": "RDMA Display - 3080a", "address": "10.0.1.12", "port": 48189}
   ],
   "active_host": 0
   ```

6. VRAM vs host buffers: `--buffers auto` uses VRAM only when the GPU and the NIC
   sit at CPU root ports and `/dev/dmabuf_holder` is there. With the NIC behind the
   chipset the seats use host buffers. Never force VRAM (`--buffers vram` /
   `--force-vram`) with a GPU or NIC behind a chipset (a test host with its GPU behind
   the chipset crashed that way).

### A seat without a GPU

A seat whose card is out gets `"gpu": ""` in its `hostd-seat<N>.json` and in
`seatmgr.json`: its next start binds no GPU node (the older static form of this state:
`pve-seat-create.sh --remove-gpu`, which drops the hook too). Its installation
(rdma-display, gamescope, Steam, `/etc/rdx`) is kept. When the card is back:

```
# "gpu": "<PCI>" in /etc/rdx/hostd-seat3.json (pct mount 3103) and in /etc/rdx/seatmgr.json,
# systemctl restart rdx-seatmgr (it reads seatmgr.json at its start)
/opt/rdx-seatmgr/rdx_seat_gpu.py --check 3103                        # what the start will bind
tools/pve-seat-net.sh --parent enp8s0f0np0 --seat 3103=10.0.1.13/24 --apply   # if it has no network block
```

A card more renumbers the bus: check the PCI addresses of the other seats at the same
time (`rdx_seat_gpu.py --status`; a seat whose address now holds another card is refused
by its pin).

## Starting a seat

```
pct start 3101                                   # its macvlan comes up with it
pct exec 3101 -- systemctl status rdx-hostd@1
```

A seat container uses its GPU only once a stream starts (gamescope) or an app runs on its
monitor. If the host's GPUs are shared with other workloads, check that the card is free
first (`GET /api/gpus` of rdx-seatmgr, or `nvidia-smi`).

## Creating and updating seats

Two tools on the Proxmox host (root), both print their usage with no arguments:

- `tools/pve-seat-create.sh --ct ID --gpu PCI [--hostname NAME] [--storage local-lvm]
  [--template-storage local] [--nvidia-dir /opt/nvidia] [--hookscript local:snippets/rdx-seat-gpu]`
  creates the privileged container, stopped, onboot 0, with the GPU-independent
  device list of "GPU isolation" above (misc minors of uinput, uhid,
  dmabuf_holder, udmabuf from their nodes), the GPU include, the lxc hook and the
  hookscript ("GPU binding"; `seat/install-seatmgr.sh` first). No `/dev/input`,
  no wildcard. `--nvidia-dir` (default `/opt/nvidia`) is the host directory with the
  `NVIDIA-Linux-x86_64-<version>.run` of the host's driver; it is bound read-only to
  `/opt/nvidia-installer` in the container. Its start is refused until `seatmgr.json`
  has the seat (`pve-seat-provision.sh install` adds it before its first start). The
  default hostname is `rdx-seat-<model>`; two GPUs of one model need `--hostname`.
  `--dry-run` only prints the lines.
  `--ct ID --update` turns an existing seat to the GPU binding, stopped or
  running (a running one keeps what it started with; its include is written first,
  for a restart after a reboot from inside): the GPU's static lines (its
  `/dev/nvidiaN`, card, render node and their allows, the whole-major allows, also
  an old `c 231:*`) go, the include, the hook and the hookscript come; the previous
  file stays as `<ID>.conf.bak-gpu`, and it prints what a start would bind now.
  `--gpu` with `--update` only checks it against `seatmgr.json`. `--ct ID
  --remove-gpu` takes GPU and hook away (stopped).
- `tools/pve-seat-provision.sh`
  - `install --ct ID --seat N --gpu PCI --build DIR --gs DIR|TGZ [--user user] [--name 4090]
    [--priority ...] [--link-ip ...] [--seatmgr URL] [--seatmgr-ui URL]`:
    the seat added to `/etc/rdx/seatmgr.json` (seatmgr restarted) before the
    first start, packages, NVIDIA userspace of the host's driver version (no
    kernel parts), the desktop user (`--user`, default `user`) with linger, then the
    offline `update`, the seat config (only if there is none), `rdx-hostd@N` enabled.
    The inside step logs to `/var/log/rdx-provision.log` in the container.
  - `update --ct ID --seat N --build DIR --gs DIR|TGZ`: offline (pct mount,
    container stopped): `/opt/rdx-gs`, the tree in `/opt/rdma-display-src` and through
    its `desktop/install.sh` `/opt/rdma-display`, units, app entry, polkit rule.
    `/etc/rdx` is kept (and backed up to the host's `/var/backups/rdx-etc-<ID>-<time>`);
    missing libraries are listed.
  - `packages --ct ID`: the package list into an existing seat (pct exec, or a
    chroot when stopped, no service started); it includes `ibverbs-utils`
    (`ibv_devinfo`) for checking the RDMA device inside.
  - `steam --ct ID [--user user]`: Steam's first start as the desktop user with software
    rendering only (Xvfb, Mesa GLX/EGL via glvnd, lavapipe, `LIBGL_ALWAYS_SOFTWARE`, CEF
    without GPU), until `~/.steam/steam/ubuntu12_32` exists and the login window is up,
    then quits it; `rdx-hostd@N` is held off meanwhile and the host's NVIDIA
    process list is watched: a process of the container stops Steam at once.
    Ubuntu's `steam-installer` asks "Install?" once (zenity, default Cancel):
    running the command is that decision, a zenity wrapper answers exactly that
    question. At the login screen the client ignores `steam -shutdown` and
    `steam://exit`; it quits in order on SIGTERM (its log: "Shutdown"), which is
    the third step after those two. Measured: login screen after 240 s
    (first start, 505 MB client update) and 25 s (second start), software
    rendered, no NVIDIA process of the container at any time. A seat's `~/.steam` can
    also be copied offline from another seat (both stopped).

`--build` is a built rdma-display tree (`make OGKM=<ogkm headers of the host
driver>`), `--gs` the patched gamescope install tree (bin/, prefix/) or its
tarball; both on the host's file system.

A fourth seat for the GPU at `0000:0b:00.0`, end to end:

```
tools/pve-seat-create.sh --ct 3104 --gpu 0000:0b:00.0 --hostname rdx-seat-4
tools/pve-seat-provision.sh install --ct 3104 --seat 4 --gpu 0000:0b:00.0 \
  --build /path/to/rdma-display --gs /path/to/rdx-gs-deploy.tgz --name 4090 \
  --link-ip 10.0.1.14 --seatmgr http://192.0.2.1:8771
tools/pve-seat-provision.sh steam --ct 3104
```

Seat 4 then has ports 48389/48390/19050/19060/19070/19080, its GPU's sound card
(function .1) by default and every local device it is given through the seatmgr API;
the network (macvlan, `/dev/infiniband`) comes from `tools/pve-seat-net.sh`.
Updating all seats to a new build: `pve-seat-provision.sh update` per container
while it is stopped.
