#!/bin/bash
# Keep the game-mode KMS sink running on its own VT, one session after another,
# and give the screen back to the desktop VT when stopped (SIGTERM from systemd,
# e.g. RuntimeMaxSec) or on any exit.
#
#   mirror_sink_loop.sh [<repo-dir>] [rdx_sink_kms options...]   (default: the repo this script is in)
#   RDX_SINK_VT (default 3), RDX_DESK_VT (default 2)
#
# Example as a time-boxed transient unit:
#   systemd-run --unit=rdx-mirror-sink -p RuntimeMaxSec=3600 \
#     /bin/bash tools/mirror_sink_loop.sh /opt/rdma-display --dev mlx5_0 --bind 10.0.0.1 --mode 2560x1440
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)}
shift
VT_SINK=${RDX_SINK_VT:-3}
VT_DESK=${RDX_DESK_VT:-2}

restore() { chvt "$VT_DESK"; }
trap 'restore; exit 0' TERM INT
trap restore EXIT

while true; do
    # the desktop may still hold DRM master right after a VT switch: retry until we are on our VT
    for _ in $(seq 1 15); do
        [ "$(fgconsole)" = "$VT_SINK" ] && break
        chvt "$VT_SINK"
        sleep 2
    done
    "$DIR/p0b/rdx_sink_kms" "$@"
    echo "[loop] sink exited with $?, restarting"
    sleep 1
done
