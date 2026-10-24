#!/bin/bash
# Run rdx_input_tx for one session. If the user leaves with the release combo
# (LeftCtrl+LeftAlt+Esc, exit code 10), also stop the local KMS sink unit so the
# screen returns to the desktop VT, i.e. the combo ends the whole remote session.
#
#   input_tx_session.sh [<repo-dir>] [rdx_input_tx options...]   (default: the repo this script is in)
#   RDX_SINK_UNIT (default rdx-mirror-sink)
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)}
shift
"$DIR/p4/rdx_input_tx" "$@"
rc=$?
if [ "$rc" = 10 ]; then
    echo "[session] release combo: stopping ${RDX_SINK_UNIT:-rdx-mirror-sink}"
    systemctl stop "${RDX_SINK_UNIT:-rdx-mirror-sink}"
fi
exit $rc
