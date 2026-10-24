#!/bin/bash
# Install rdx-seatmgr on the Proxmox host: /opt/rdx-seatmgr, /etc/rdx/seatmgr.json (kept if present),
# the systemd unit, the udev rule 70-power-switch.rules (shadows systemd's), and the seats' GPU hook
# rdx_seat_gpu.py with its link in a snippets storage (RDX_SNIPPETS, default local), the
# "hookscript:" of the seat containers (docs/MULTISEAT.md "GPU binding"). Usage: install-seatmgr.sh [--no-start]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
install -D -m 755 "$here/rdx_seatmgr.py" /opt/rdx-seatmgr/rdx_seatmgr.py
sed -n '2,/^"""/p' "$here/rdx_seatmgr.py" | sed '$d' > /opt/rdx-seatmgr/README
# the GPU hook: a new file takes effect at the next container start, a running container is not touched
install -m 755 "$here/rdx_seat_gpu.py" /opt/rdx-seatmgr/rdx_seat_gpu.py.new
mv -f /opt/rdx-seatmgr/rdx_seat_gpu.py.new /opt/rdx-seatmgr/rdx_seat_gpu.py
sed -n '2,/^"""/p' "$here/rdx_seat_gpu.py" | sed '$d' > /opt/rdx-seatmgr/README.seat-gpu
snip=$(pvesm path "${RDX_SNIPPETS:-local}:snippets/rdx-seat-gpu" 2>/dev/null || true)
if [ -n "$snip" ]; then
  mkdir -p "$(dirname "$snip")"
  ln -sfn /opt/rdx-seatmgr/rdx_seat_gpu.py "$snip"
  echo "hookscript ${RDX_SNIPPETS:-local}:snippets/rdx-seat-gpu -> $snip"
else
  echo "no snippets storage ${RDX_SNIPPETS:-local}: the seats' hookscript is missing, their starts fail" >&2
fi
if [ ! -e /etc/rdx/seatmgr.json ]; then
  install -D -m 644 "$here/seatmgr.json" /etc/rdx/seatmgr.json
  echo "wrote /etc/rdx/seatmgr.json"
fi
install -m 644 "$here/rdx-seatmgr.service" /etc/systemd/system/rdx-seatmgr.service
# the seats' keyboards are no power switches of the host (see the rule); devices that exist now
# get it with a change event, logind takes it from their next appearance on
install -D -m 644 "$here/70-power-switch.rules" /etc/udev/rules.d/70-power-switch.rules
rm -f /etc/udev/rules.d/71-rdx-seat-no-power-switch.rules     # an earlier, weaker form (TAG-=)
udevadm control --reload
udevadm trigger --action=change --subsystem-match=input --property-match=ID_INPUT_KEY=1 || true
systemctl daemon-reload
systemctl enable rdx-seatmgr.service >/dev/null 2>&1
if [ "${1:-}" != --no-start ]; then
  systemctl restart rdx-seatmgr.service
  sleep 1
  systemctl is-active rdx-seatmgr.service
fi
