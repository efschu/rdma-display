#!/bin/bash
# Seat monitor test (docs/SEAT-MONITOR.md). Runs INSIDE a seat container, as root, with the seat's
# rdx-hostd@N stopped for the duration (the test hostd takes its place, "restore" starts the unit again).
#
#   seat-monitor-test.sh hostd [auto|client|monitor] [vkcube present mode]
#                                    test hostd with home app vkcube (RDX_GS_CLOCK as given)
#   seat-monitor-test.sh state | log [n]
#   seat-monitor-test.sh stream [WxH@HZ] [s]   stream add-on probe without VBLANKs (monitor leads)
#   seat-monitor-test.sh clock [WxH@HZ] [s]    the same with one VBLANK per frame (client clock)
#   seat-monitor-test.sh key [after_s] [hold_s] [device name]
#                                    one key press from a uinput device; "rdx: ..." names count as
#                                    the client's input, anything else as local input
#   seat-monitor-test.sh mirror [n] | console  stage 1 capture probe / what the connector shows
#   seat-monitor-test.sh input | sound | stop | restore
#
# Defaults are seat 1 of the example layout in docs/MULTISEAT.md (RTX 5090 at 0000:15:00.0, card1,
# HDMI-A-1, desktop user "user"); override with the environment:
#   SEAT GPU_PCI LINK_IP CARD RENDER CONN UUID (default: nvidia-smi's uuid of GPU_PCI) APP_USER
# Seconds: the probe exits 0 with frames and no black/white frame. Nothing here reserves the GPU:
# make sure nothing else uses it (nvidia-smi) before "hostd".
set -u
R=/opt/rdma-display
L=/tmp/seat-monitor-test
SEAT=${SEAT:-1}
GPU_PCI=${GPU_PCI:-0000:15:00.0}
LINK_IP=${LINK_IP:-10.0.1.11}
CARD=${CARD:-/dev/dri/card1}
RENDER=${RENDER:-/dev/dri/renderD128}
CONN=${CONN:-HDMI-A-1}
UUID=${UUID:-$(nvidia-smi --query-gpu=pci.bus_id,uuid --format=csv,noheader 2>/dev/null |
                awk -F', ' -v p="${GPU_PCI#*:}" 'tolower($1) ~ (p "$") {print $2; exit}')}
APP_USER=${APP_USER:-user}
CONF=/var/tmp/seat-monitor-test-seat$SEAT.json
SOCK=/run/rdx-gs-local-$SEAT.sock
PORT=$((48090 + (SEAT - 1) * 100))
mkdir -p $L
st() { python3 -c "import json,sys,urllib.request; s=json.load(urllib.request.urlopen('http://127.0.0.1:$PORT/api/state')); print(json.dumps(s.get('local')))"; }
clean() { sed 's/\x1b\[[0-9;]*m//g' | cut -c1-220; }
probe() { $R/gs/rdx_gs_probe --socket $SOCK --uuid $UUID --mode "$1" --user $APP_USER --seconds "$2" "${@:3}"; }
case "${1:-}" in
hostd)
    CLK=${2:-auto}
    PM=${3:+, \"--present_mode\", \"$3\"}
    printf '%s\n' "{\"seat\": $SEAT, \"gpu\": \"$GPU_PCI\", \"seat_name\": \"seat monitor test\", \"link_ip\": \"$LINK_IP\", \"desktop_user\": \"$APP_USER\", \"gamescope_bin\": \"/opt/rdx-gs/bin/gamescope\", \"log_dir\": \"$L\", \"home_app\": \"vkcube\", \"apps\": [{\"id\": \"vkcube\", \"name\": \"vkcube\", \"kind\": \"gamescope\", \"cmd\": [\"vkcube\"$PM], \"gamescope_args\": [], \"env\": {\"RDX_GS_CLOCK\": \"$CLK\"}}]}" > $CONF
    systemctl stop rdx-hostd@$SEAT
    pkill -f "rdx_hostd --repo $R --config $CONF"; sleep 2
    setsid $R/host/rdx_hostd --repo $R --config $CONF --seat $SEAT > $L/hostd.log 2>&1 < /dev/null &
    sleep 12
    st
    pgrep -a gamescope | cut -c1-200
    grep -aE "monitor|local_gs.*(rdx:|Xwayland|drm:|DRM|rror)" $L/hostd.log | clean | tail -20
    ;;
state) st ;;
log) tail -${2:-40} $L/hostd.log | clean ;;
stream) probe "${2:-1280x720@240}" "${3:-10}" --dump $L/stream.ppm ;;
clock)
    probe "${2:-1280x720@240}" "${3:-10}" --vblank --dump $L/stream-clock.ppm
    grep -aE "clock|stream attached|stream detached" $L/hostd.log | clean | tail -4
    ;;
key) python3 "$(dirname "$0")/rdx-uinput-key.py" "${2:-2}" "${3:-2}" "${4:-rdx:test keyboard}" ;;
mirror) $R/p1/rdx_mirror_src --probe ${2:-240} --dump $L/mirror.ppm --card $CARD --render $RENDER --output $CONN ;;
console) $R/p1/rdx_mirror_src --probe 30 --dump $L/console.ppm --card $CARD --render $RENDER --output $CONN ;;
input)
    P=$(pgrep -f "^/opt/rdx-gs/bin/gamescope --backend drm" | head -1)
    echo "gamescope $P input fds:"
    for f in /proc/$P/fd/*; do readlink "$f"; done 2>/dev/null | grep "^/dev/input/event" | sort -u
    for e in /sys/class/input/event*; do echo "$(basename $e) $(cat $e/device/name)"; done
    ;;
sound)
    U=$(id -u $APP_USER)
    runuser -u $APP_USER -- env XDG_RUNTIME_DIR=/run/user/$U pactl info | grep -E "Default Sink"
    runuser -u $APP_USER -- env XDG_RUNTIME_DIR=/run/user/$U pactl list sinks short
    ;;
stop) pkill -f "rdx_hostd --repo $R --config $CONF"; sleep 6; pgrep -a gamescope; tail -4 $L/hostd.log | clean ;;
restore) rm -f $CONF; systemctl start rdx-hostd@$SEAT; systemctl is-active rdx-hostd@$SEAT ;;
*) sed -n 2,21p "$0"; exit 2 ;;
esac
