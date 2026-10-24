#!/bin/bash
# The RDMA link of the seat containers on a Proxmox host (docs/MULTISEAT.md). Run on the host as root.
#
#   pve-seat-net.sh --parent NETDEV [--seat CT=IP/PREFIX ...] [--mtu 9000] [--ifname eth1]
#                   [--macvlan-mode bridge] [--sriov] [--dry-run | --apply]
#
# Default seats (docs/MULTISEAT.md): 3101=10.0.1.11/24 3102=10.0.1.12/24 3103=10.0.1.13/24.
# Default --dry-run: prints what would change, changes nothing. --apply writes it (only on the
# user's word; the containers must be stopped).
#
# macvlan (default): one macvlan of the RoCE port NETDEV per container, with the seat's link IP.
#   The kernel adds a RoCE GID for every IP of an upper device (macvlan) to the parent's RDMA
#   device, so all seats share that device; each container sees every GID (RDMA netns "shared",
#   the kernel default), and the programs pick the GID carrying their own link IP and, with
#   rdma_dev "auto", the device holding it (common/rdx_common.h). /dev/infiniband: only the
#   uverbs node of that device is bound in and allowed (c 231:<its minor>), not the whole major.
# --sriov: one virtual function per container instead (its own netdev and RDMA device, moved
#   into the container as a phys link). Needs SR-IOV on the NIC (sriov_totalvfs > 0) and VFs
#   created before the containers start; for isolation of the RDMA devices themselves:
#   `rdma system set netns exclusive` and `rdma dev set <vf-dev> netns <container pid>` at
#   container start (printed as a note, not done here).
# Nothing here touches a GPU. What it writes in /etc/pve/lxc/<CT>.conf: raw lxc.net.<10..> lines, the
#   uverbs node's c 231:<minor> and its mount entry. Re-running replaces exactly those lines, found by
#   content: Proxmox moves every comment line of a config into the description when it rewrites it
#   (pct mount, pct set), so begin/end markers would not stay next to the block.
set -euo pipefail

PARENT="" MTU=9000 IFNAME=eth1 MODE=bridge SRIOV=0 APPLY=0
SEATS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --parent) PARENT=$2; shift ;;
    --seat) SEATS+=("$2"); shift ;;
    --mtu) MTU=$2; shift ;;
    --ifname) IFNAME=$2; shift ;;
    --macvlan-mode) MODE=$2; shift ;;
    --sriov) SRIOV=1 ;;
    --dry-run) APPLY=0 ;;
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$PARENT" ] || { echo "usage: $0 --parent NETDEV [--seat CT=IP/PREFIX ...] (see --help)" >&2; exit 2; }
[ ${#SEATS[@]} -gt 0 ] || SEATS=("3101=10.0.1.11/24" "3102=10.0.1.12/24" "3103=10.0.1.13/24")
case "$MODE" in bridge|private|vepa|passthru) ;; *) echo "--macvlan-mode bridge|private|vepa|passthru" >&2; exit 2 ;; esac

say() { printf '%s\n' "$*"; }
run() { if [ "$APPLY" = 1 ]; then say "+ $*"; "$@"; else say "  would run: $*"; fi; }

# RDMA device and uverbs node of a netdev ("" if none, e.g. the NIC is not in yet)
rdma_of() { ls "/sys/class/net/$1/device/infiniband" 2>/dev/null | head -1 || true; }
uverbs_of() { ls "/sys/class/infiniband/$1/device/infiniband_verbs" 2>/dev/null | head -1 || true; }
devnum_of() { cat "/sys/class/infiniband_verbs/$1/dev" 2>/dev/null || true; }   # "231:192"
# the lines this script owns in a container config (raw lxc.net from index 10 on, pct's own netN are
# "netN:" keys; the uverbs node's allow and mount; markers of earlier versions, also URL-encoded)
NET_LINES='^lxc\.net\.([1-9][0-9]+)\.|^lxc\.cgroup2\.devices\.allow: c 231:[0-9]+ rwm$|^lxc\.mount\.entry: /dev/infiniband/uverbs[0-9]+ |^# ?rdma-display seat net (BEGIN|END)'

say "# rdma-display seat network: parent $PARENT, mode $([ "$SRIOV" = 1 ] && echo "SR-IOV VF per seat" || echo "macvlan $MODE"), MTU $MTU, $([ "$APPLY" = 1 ] && echo APPLY || echo dry run)"
if [ ! -d "/sys/class/net/$PARENT" ]; then
  say "# note: $PARENT does not exist on this host (yet): device numbers below are placeholders"
fi
PRDMA=$(rdma_of "$PARENT"); PRDMA=${PRDMA:-<rdma-dev-of-$PARENT>}
say "# RDMA device of $PARENT: $PRDMA (RoCE MTU 4096 needs a netdev MTU of at least 4200)"
run ip link set "$PARENT" mtu "$MTU" up

VFS=()
if [ "$SRIOV" = 1 ]; then
  total=$(cat "/sys/class/net/$PARENT/device/sriov_totalvfs" 2>/dev/null || echo 0)
  [ "$total" -ge "${#SEATS[@]}" ] || say "# WARNING: $PARENT offers $total VFs, ${#SEATS[@]} needed (sriov_totalvfs; enable SR-IOV in the NIC firmware: mlxconfig SRIOV_EN=1 NUM_OF_VFS=N)"
  run sh -c "echo ${#SEATS[@]} > /sys/class/net/$PARENT/device/sriov_numvfs"
  for i in $(seq 0 $((${#SEATS[@]} - 1))); do
    vf=$(ls "/sys/class/net/$PARENT/device/virtfn$i/net" 2>/dev/null | head -1 || true)
    VFS+=("${vf:-<vf$i-netdev>}")
  done
  say "# persistence: the VFs are gone after a reboot; recreate them before the containers start (e.g. a systemd unit with the line above)"
fi

i=0
for s in "${SEATS[@]}"; do
  CT=${s%%=*}; IP=${s#*=}
  [[ "$CT" =~ ^[0-9]+$ ]] && [[ "$IP" =~ ^[0-9.]+/[0-9]+$ ]] || { echo "--seat CT=IP/PREFIX, got $s" >&2; exit 2; }
  CONF=/etc/pve/lxc/$CT.conf
  if [ "$SRIOV" = 1 ]; then
    LINK=${VFS[$i]}; DEV=$(rdma_of "$LINK"); DEV=${DEV:-<rdma-dev-of-$LINK>}
  else
    LINK=$PARENT; DEV=$PRDMA
  fi
  UV=$(uverbs_of "$DEV"); NUM=$(devnum_of "${UV:-x}")
  UV=${UV:-<uverbsN-of-$DEV>}; NUM=${NUM:-231:<minor-of-$UV>}
  # raw LXC network: pct's netN knows only veth, so the macvlan / phys link is an lxc.net entry with
  # an index far from pct's own lxc.net.0..
  N=$((10 + i))
  BLOCK="lxc.net.$N.type: $([ "$SRIOV" = 1 ] && echo phys || echo macvlan)
lxc.net.$N.link: $LINK"
  [ "$SRIOV" = 1 ] || BLOCK+="
lxc.net.$N.macvlan.mode: $MODE"
  BLOCK+="
lxc.net.$N.name: $IFNAME
lxc.net.$N.ipv4.address: $IP
lxc.net.$N.mtu: $MTU
lxc.net.$N.flags: up
lxc.cgroup2.devices.allow: c $NUM rwm
lxc.mount.entry: /dev/infiniband/$UV dev/infiniband/$UV none bind,optional,create=file"
  say ""
  say "## CT $CT: $IFNAME = $([ "$SRIOV" = 1 ] && echo "VF $LINK" || echo "macvlan of $LINK") with $IP, RDMA device $DEV ($UV, c $NUM)"
  say "# $CONF: its lxc.net.<10..>, c 231:<minor> and /dev/infiniband/uverbs lines become"
  say "$BLOCK"
  say "# the container's hostd config /etc/rdx/hostd-seat*.json: \"link_ip\": \"${IP%/*}\", \"rdma_dev\": \"auto\""
  if grep -q '^lxc.cgroup2.devices.allow: c 231:\* rwm' "$CONF" 2>/dev/null; then
    say "# $CONF also allows the whole /dev/infiniband major (c 231:* rwm, from pve-seat-create.sh):"
    say "#   with this block it can go; it is left alone here (remove it by hand)"
  fi
  if [ "$APPLY" = 1 ]; then
    [ -f "$CONF" ] || { echo "$CONF missing" >&2; exit 1; }
    pct status "$CT" | grep -q stopped || { echo "CT $CT is not stopped: stop it first" >&2; exit 1; }
    case "$NUM$UV$LINK$DEV" in *'<'*) echo "device numbers unknown (NIC missing?): not applying CT $CT" >&2; exit 1 ;; esac
    tmp=$(mktemp)
    # by content, plus the begin/end marker lines an earlier version wrote (empty after Proxmox moved them)
    grep -vE "$NET_LINES" "$CONF" > "$tmp" || true
    printf '%s\n' "$BLOCK" >> "$tmp"
    cp "$CONF" "$CONF.bak-seatnet"
    cat "$tmp" > "$CONF"
    rm -f "$tmp"
    say "+ written to $CONF (previous: $CONF.bak-seatnet)"
  fi
  i=$((i + 1))
done

say ""
say "# notes"
say "# - RDMA netns: keep 'rdma system show' at netns shared for macvlan seats (every container sees the"
say "#   parent's RDMA device; the GID by link IP keeps the seats apart). Exclusive mode only with --sriov."
say "# - Clients on the link: the client's link IP must reach 10.0.1.0/24 (same L2 segment, or a route)."
say "# - Seats talking to each other over macvlan bridge mode stay inside the NIC; a client on the wire"
say "#   sees one MAC per seat."
[ "$APPLY" = 1 ] || say "# dry run: nothing changed (--apply writes it)"
