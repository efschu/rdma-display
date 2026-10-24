#!/bin/bash
# Create one rdma-display seat container on a Proxmox host (docs/MULTISEAT.md). Run on the host as root.
#
#   pve-seat-create.sh --ct ID --gpu PCI [--hostname NAME] [--storage local-lvm]
#                      [--template-storage local] [--template ubuntu-24.04-standard_24.04-2_amd64.tar.zst]
#                      [--cores 16] [--memory 16384] [--swap 8192] [--disk 200] [--bridge vmbr0]
#                      [--nvidia-dir /opt/nvidia] [--hookscript local:snippets/rdx-seat-gpu] [--dry-run]
#   pve-seat-create.sh --ct ID --update [--gpu PCI]   an existing seat binds its GPU at every start
#   pve-seat-create.sh --ct ID --remove-gpu            an existing, stopped seat keeps no GPU and no hook
#
# --dry-run prints the container's lines and creates or writes nothing (CT id may exist then).
#
# The seat's GPU is NOT in its config as numbers: they move with the hardware (seen: a second 3080 went
# in, the 5090's card0 became card1 and the first 3080's card1 card0, so seat 1's static lines named
# seat 2's card) and the majors of nvidia-uvm/nvidia-caps with every driver load (seen: 505/510 -> 502/508).
# The config gets
#   lxc.include: /run/rdx-seat/<ID>.gpu.conf               written at every pct start by the hookscript
#   lxc.hook.pre-start: /opt/rdx-seatmgr/rdx_seat_gpu.py   lxc checks every start against the host
#   hookscript: local:snippets/rdx-seat-gpu                (--hookscript VOLID)
# and seat/rdx_seat_gpu.py reads the numbers of the GPU at the PCI address of the seat ("gpu" of the
# seat with this "ct" in /etc/rdx/seatmgr.json) at that moment, or refuses the start (docs/MULTISEAT.md
# "GPU binding"). Install it first: seat/install-seatmgr.sh.
# --update turns an existing seat to that form, stopped or running (a running one keeps what it
# started with; its include is written at once, for its restart after a reboot from inside, which
# runs no hookscript): its GPU's static lines (/dev/nvidiaN, card, render node and their allows) and
# the whole-major allows (an old nvidia-uvm/nvidia-caps major, an old /dev/infiniband c 231:*) go, the
# lines above come; the previous file is kept as <ID>.conf.bak-gpu. --gpu with --update only checks
# it against seatmgr.json: a seat's GPU changes in seatmgr.json (and its hostd-seatN.json), not here.
# --remove-gpu: no GPU and no hook at all (a seat with "gpu": "" in seatmgr.json is the other way).
#
# A privileged container (stopped, onboot 0) that sees exactly ONE GPU. Bound in and allowed besides the
# GPU: nvidiactl, nvidia-modeset, nvidia-uvm(-tools) and nvidia-caps (the nodes; their dynamic majors
# come with the include), uinput and uhid (streamed input), dmabuf_holder and udmabuf when the host has
# them. The RDMA NIC (macvlan, its one uverbs node) comes from tools/pve-seat-net.sh. NO /dev/input and
# no input/sound major: local keyboards, mice and sound cards come only through rdx-seatmgr
# (seat/rdx_seatmgr.py). The NVIDIA installer directory is bound read-only to /opt/nvidia-installer
# (pve-seat-provision.sh installs the userspace part from it; --nvidia-dir, default /opt/nvidia, holds the
# NVIDIA-Linux-x86_64-*.run of the host's driver version). Nothing here touches the GPU.
set -euo pipefail

CT="" GPU="" NAME="" STORAGE=local-lvm TSTORE=local TPL=ubuntu-24.04-standard_24.04-2_amd64.tar.zst
CORES=16 MEM=16384 SWAP=8192 DISK=200 BRIDGE=vmbr0 NVDIR=/opt/nvidia DRY=0 MODE=create
HOOKPY=/opt/rdx-seatmgr/rdx_seat_gpu.py HOOKSCRIPT=local:snippets/rdx-seat-gpu SMCONF=/etc/rdx/seatmgr.json
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1; shift; continue ;;
    --update) MODE=update; shift; continue ;;
    --remove-gpu) MODE=remove; shift; continue ;;
    --ct) CT=$2 ;; --gpu) GPU=$2 ;; --hostname) NAME=$2 ;; --storage) STORAGE=$2 ;;
    --template-storage) TSTORE=$2 ;; --template) TPL=$2 ;; --cores) CORES=$2 ;; --memory) MEM=$2 ;;
    --swap) SWAP=$2 ;; --disk) DISK=$2 ;; --bridge) BRIDGE=$2 ;; --nvidia-dir) NVDIR=$2 ;;
    --hookscript) HOOKSCRIPT=$2 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift 2
done
[ -n "$CT" ] && { [ -n "$GPU" ] || [ "$MODE" != create ]; } || { echo "usage: $0 --ct ID --gpu PCI (see --help)" >&2; exit 2; }
C=/etc/pve/lxc/$CT.conf
INC=/run/rdx-seat/$CT.gpu.conf
# the hook in this tree for a dry run on a host where it is not installed yet
HOOKRUN=$HOOKPY
[ -x "$HOOKRUN" ] || HOOKRUN="$(cd "$(dirname "$0")/.." && pwd)/seat/rdx_seat_gpu.py"
hookrun() { python3 "$HOOKRUN" "$@"; }
# the lines that belong to one GPU (the rest of a seat's devices is the same for every GPU): the static
# ones of before (the whole-major allows count to them: nvidia-uvm and nvidia-caps have dynamic majors,
# an old c 231:*) and the dynamic form
GPU_LINES='^lxc\.cgroup2\.devices\.allow: c (195:([0-9]|[0-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-3])|226:[0-9]+|[0-9]+:\*) rwm$|^lxc\.mount\.entry: /dev/(nvidia[0-9]+|dri/(card|renderD)[0-9]+) |^lxc\.include: /run/rdx-seat/|^lxc\.hook\.pre-start: [^ ]*rdx_seat_gpu'

# "c MAJ:MIN" of a host node, empty if the host has none
cnode() { if [ -c "$1" ]; then printf 'c %d:%d' "0x$(stat -c %t "$1")" "0x$(stat -c %T "$1")"; fi; }
# whole majors: nvidia-uvm (+ uvm-tools), nvidia-caps, as the host numbers them now (--remove-gpu only:
# a seat with the hook gets them from its include, at every start anew)
major_lines() {
  local n m
  for n in /dev/nvidia-uvm /dev/nvidia-caps/nvidia-cap1; do
    m=$(cnode "$n")
    if [ -n "$m" ]; then echo "lxc.cgroup2.devices.allow: ${m%:*}:* rwm"; fi
  done
}
# the dynamic form: the GPU's lines come from the include (header)
dyn_lines() {
  echo "lxc.include: $INC"
  echo "lxc.hook.pre-start: $HOOKPY"
}

if [ "$MODE" != create ]; then
  [ -f "$C" ] || { echo "no CT $CT" >&2; exit 1; }
fi
if [ "$MODE" = remove ]; then
  [ "$DRY" = 1 ] || pct status "$CT" | grep -q stopped || { echo "CT $CT is not stopped: stop it first" >&2; exit 1; }
  echo "# CT $CT: GPU lines that go"
  grep -E "$GPU_LINES|^hookscript: " "$C" || true
  echo "# comes:"; major_lines | sed 's/^/+/'
  if [ "$DRY" = 0 ]; then
    cp "$C" "$C.bak-gpu"
    { grep -vE "$GPU_LINES" "$C.bak-gpu" | sed -E '1s/\((GPU [^)]*|no GPU)\)/(no GPU)/'; major_lines; } > "$C.new"
    cat "$C.new" > "$C" && rm -f "$C.new"
    if grep -q '^hookscript: ' "$C"; then pct set "$CT" --delete hookscript; fi
    echo "# written (previous: $C.bak-gpu)"
  fi
  exit 0
fi
[ "$DRY" = 1 ] || [ -x "$HOOKPY" ] || { echo "$HOOKPY missing: seat/install-seatmgr.sh first" >&2; exit 1; }

if [ "$MODE" = update ]; then
  if [ -n "$GPU" ]; then
    want=$(python3 -c 'import json, sys
c = json.load(open(sys.argv[1]))
print(next((s.get("gpu", "") for s in c.get("seats", {}).values() if str(s.get("ct")) == sys.argv[2]), "-"))' "$SMCONF" "$CT")
    g=$(echo "$GPU" | tr 'A-F' 'a-f')
    [ "$want" = "$g" ] || { echo "$SMCONF says GPU \"$want\" for CT $CT, not $g: change it there (and in hostd-seatN.json)" >&2; exit 1; }
  fi
  running=0
  if pct status "$CT" | grep -q running; then running=1; fi
  echo "# CT $CT: GPU of its seat in $SMCONF, bound at every start"
  echo "# goes:"; grep -E "$GPU_LINES" "$C" | sed 's/^/-/' || true
  echo "# comes:"; dyn_lines | sed 's/^/+/'; echo "+hookscript: $HOOKSCRIPT"
  echo "# a start now would bind (include):"
  hookrun --check "$CT" | sed 's/^/  /' || echo "# a start now is REFUSED (above)"
  [ "$DRY" = 0 ] || exit 0
  if [ "$running" = 1 ]; then
    # first the include, then the config: an inside reboot in between must find both
    "$HOOKPY" --write "$CT" || { echo "CT $CT runs and its include cannot be written: its config is left as it is" >&2; exit 1; }
  fi
  cp "$C" "$C.bak-gpu"
  { grep -vE "$GPU_LINES" "$C.bak-gpu" | sed -E '1s/\((GPU [^)]*|no GPU)\)/(GPU of its seat in \/etc\/rdx\/seatmgr.json, bound at every start)/'; dyn_lines; } > "$C.new"
  cat "$C.new" > "$C" && rm -f "$C.new"
  pct set "$CT" --hookscript "$HOOKSCRIPT"
  echo "# written (previous: $C.bak-gpu)"
  "$HOOKPY" --status | grep "^CT $CT " || true
  exit 0
fi

GPU=$(echo "$GPU" | tr 'A-F' 'a-f')
[[ "$GPU" =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$ ]] || { echo "--gpu: PCI address like 0000:0d:00.0" >&2; exit 2; }
if [ "$DRY" = 0 ] && pct status "$CT" >/dev/null 2>&1; then echo "CT $CT exists already" >&2; exit 1; fi

# --- the GPU's device numbers now, for the messages and the default hostname (the container's start
# reads them anew)
info=/proc/driver/nvidia/gpus/$GPU/information
[ -r "$info" ] || { echo "$GPU is no NVIDIA GPU of this host ($info missing)" >&2; exit 1; }
MINOR=$(sed -n 's/^Device Minor:[[:space:]]*//p' "$info")
CARD=$(basename "$(ls -d /sys/bus/pci/devices/$GPU/drm/card[0-9]* | head -1)")
RENDER=$(basename "$(ls -d /sys/bus/pci/devices/$GPU/drm/renderD[0-9]* | head -1)")
[ -n "$MINOR" ] && [ -n "$CARD" ] && [ -n "$RENDER" ] || { echo "cannot read the device numbers of $GPU" >&2; exit 1; }
model=$(sed -n 's/^Model:[[:space:]]*//p' "$info")
NAME=${NAME:-rdx-seat-$(echo "$model" | grep -oE '[0-9]{4}[A-Za-z]*' | tail -1 | tr 'A-Z' 'a-z')}

# --- the container's device lines
device_lines() {
  local n m
  dyn_lines
  echo "lxc.cgroup2.devices.allow: c 195:254 rwm"
  echo "lxc.cgroup2.devices.allow: c 195:255 rwm"
  for n in /dev/uinput /dev/uhid /dev/dmabuf_holder /dev/udmabuf; do
    m=$(cnode "$n")
    if [ -n "$m" ]; then echo "lxc.cgroup2.devices.allow: $m rwm"; fi
  done
  for n in nvidiactl nvidia-modeset nvidia-uvm nvidia-uvm-tools; do
    echo "lxc.mount.entry: /dev/$n dev/$n none bind,optional,create=file"
  done
  echo "lxc.mount.entry: /dev/nvidia-caps dev/nvidia-caps none bind,optional,create=dir"
  for n in dmabuf_holder udmabuf uinput uhid; do
    if [ -c /dev/$n ]; then echo "lxc.mount.entry: /dev/$n dev/$n none bind,optional,create=file"; fi
  done
  if [ -d "$NVDIR" ]; then echo "lxc.mount.entry: $NVDIR opt/nvidia-installer none bind,ro,optional,create=dir"; fi
}

if [ "$DRY" = 1 ]; then
  echo "# CT $CT ($NAME): GPU $GPU, now nvidia$MINOR, $CARD, $RENDER (bound at every start)"
  echo "hookscript: $HOOKSCRIPT"
  device_lines
  exit 0
fi

# --- the container
[ -f "$(pvesm path "$TSTORE:vztmpl/$TPL" 2>/dev/null)" ] || pveam download "$TSTORE" "$TPL"
pct create "$CT" "$TSTORE:vztmpl/$TPL" --hostname "$NAME" --unprivileged 0 --features nesting=1 \
  --cores "$CORES" --memory "$MEM" --swap "$SWAP" --rootfs "$STORAGE:$DISK" --ostype ubuntu \
  --net0 "name=eth0,bridge=$BRIDGE,ip=dhcp,type=veth" --onboot 0 --hookscript "$HOOKSCRIPT" \
  --description "rdma-display seat: $NAME (GPU of its seat in /etc/rdx/seatmgr.json, bound at every start; $GPU at creation). Stopped until used."
device_lines >> "$C"
echo "created CT $CT ($NAME): GPU $GPU, now nvidia$MINOR, $CARD, $RENDER"
echo "its start is refused until $SMCONF has a seat with \"ct\": $CT and \"gpu\": \"$GPU\" (pve-seat-provision.sh install)"
grep -E '^(hostname|rootfs|onboot|hookscript|lxc\.)' "$C"
