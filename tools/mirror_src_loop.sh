#!/bin/bash
# Keep the mirror source running (endless sessions, reconnect after any exit,
# e.g. sink restart, monitor blanked, link down).
#
#   mirror_src_loop.sh [<repo-dir>] [rdx_mirror_src options...]   (pass --frames 0; default repo: the one
#   this script is in)
#
# Example as a time-boxed transient unit:
#   systemd-run --unit=rdx-mirror-src -p RuntimeMaxSec=3600 \
#     /bin/bash tools/mirror_src_loop.sh /opt/rdma-display --peer 10.0.0.1 --output DP-1 --frames 0
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)}
shift
trap 'exit 0' TERM INT
while true; do
    "$DIR/p1/rdx_mirror_src" "$@"
    echo "[loop] source exited with $?, reconnecting in 2 s"
    sleep 2
done
