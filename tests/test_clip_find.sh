#!/bin/bash
# Test of rdx_clip --x-of-pid: finding the display of the gamescope below rdx_gs_src when its X server
# is NOT below it. gamescope (wlroots) starts Xwayland with a double fork, so Xwayland's parent is
# init (observed in a seat container); gamescope keeps the display's listening sockets and starts the app with
# DISPLAY=:N. Here: Xvfb is double-forked away (parent init / a subreaper), a stand-in "gamescope"
# below a stand-in "rdx_gs_src" holds Xvfb's listening sockets (taken with pidfd_getfd, the same
# sockets as gamescope's), and a stand-in app below another one only has DISPLAY=:N.
# Checks: a lazily started display (socket only, no Xwayland) is never connected to; with Xwayland but
# before the app it waits; found by the socket, found by DISPLAY, nothing found -> no display; --set/--get.
# Needs Xvfb and python3 (pidfd_getfd: root or the same user); without them: SKIP.
#   tests/test_clip_find.sh [path/to/rdx_clip]
set -u
BIN=${1:-$(dirname "$0")/../clip/rdx_clip}
command -v Xvfb >/dev/null && command -v python3 >/dev/null || { echo "test_clip_find: SKIP (needs Xvfb and python3)"; exit 0; }
[ -x "$BIN" ] || { echo "test_clip_find: $BIN not built"; exit 1; }
T=$(mktemp -d /tmp/test_clip_find.XXXXXX)
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do pkill -P "$p" 2>/dev/null; kill "$p" 2>/dev/null; done; [ -s "$T/xvfb.pid" ] && kill "$(cat "$T/xvfb.pid")" 2>/dev/null;
            pkill -f "$T/gs.py" 2>/dev/null; pkill -f "$T/lazy.py" 2>/dev/null; [ -n "${M:-}" ] && rm -f /tmp/.X11-unix/X$M; pkill -f "rdx_clip --set --display :$N\$" 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
N=; M=
trap cleanup EXIT
fails=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
until_t() { local n=$(( $1 * 10 )); shift; while [ $n -gt 0 ]; do "$@" && return 0; sleep 0.1; n=$((n - 1)); done; return 1; }
below() {   # below CHILD ROOT: 0 = CHILD is a descendant of ROOT
    local p=$1
    for _ in $(seq 64); do p=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null) || return 1; [ "$p" = "$2" ] && return 0; [ "$p" -le 1 ] && return 1; done
    return 1
}

freenum() { local n; for n in $(seq "$1" 99); do [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ] || { echo $n; return; }; done; }

# 1. lazy start (wlroots): the compositor listens on :M and would start Xwayland at the first client.
#    Neither an Xwayland nor ... runs yet: rdx_clip must not connect (it did, failed as root and left:
#    Xwayland -terminate quit, gamescope never started Steam)
M=$(freenum 60)
cat > "$T/lazy.py" <<'EOF'
import os, socket, subprocess, sys, threading, time
m, cnt = sys.argv[1], sys.argv[2]
socks = []
for addr in ("/tmp/.X11-unix/X" + m, "\0/tmp/.X11-unix/X" + m):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(addr); s.listen(4); socks.append(s)
n = 0
open(cnt, "w").write("0")
def acc(s):
    global n
    while True:
        c, _ = s.accept(); n += 1; open(cnt, "w").write(str(n)); c.close()
for s in socks: threading.Thread(target=acc, args=(s,), daemon=True).start()
import ctypes; die = lambda: ctypes.CDLL(None).prctl(1, 15)                 # PR_SET_PDEATHSIG
subprocess.Popen(["env", "DISPLAY=:" + m, "sleep", "300"], preexec_fn=die)   # the app (too early)
time.sleep(300)
EOF
bash -c "python3 $T/lazy.py $M $T/lazy.cnt 2>/dev/null; :" & LZ=$!; PIDS+=($LZ)
until_t 5 test -s "$T/lazy.cnt"
"$BIN" --get --x-of-pid "$LZ" >/dev/null 2>"$T/lazy.log" && fail "lazy: got a selection from a fake display"
grep -q "display :$M below process $LZ (the X socket gamescope listens on): waiting until its Xwayland runs" "$T/lazy.log" \
    && ok "lazy start: the display is found by its socket, but without an Xwayland it waits" || { fail "lazy: no waiting line"; cat "$T/lazy.log"; }
[ "$(cat "$T/lazy.cnt")" = 0 ] && ok "lazy start: rdx_clip never connected to the socket (it is never the first client)" \
    || fail "lazy start: rdx_clip connected $(cat "$T/lazy.cnt") times"
rm -f /tmp/.X11-unix/X$M

# 2. the X server, double-forked away like gamescope's Xwayland (named Xwayland, display on its command line)
N=$(freenum 70)
ln -s "$(command -v Xvfb)" "$T/Xwayland"
( "$T/Xwayland" ":$N" -nolisten tcp -extension GLX >/dev/null 2>&1 & echo $! > "$T/xvfb.pid" )
until_t 10 test -S /tmp/.X11-unix/X$N || { echo "test_clip_find: Xvfb did not start"; exit 1; }
XP=$(cat "$T/xvfb.pid")
sleep 0.3
grep -q Xwayland "/proc/$XP/comm" || fail "the stand-in X server is not named Xwayland"

# stand-in rdx_gs_src -> stand-in gamescope holding the X server's listening sockets; it starts the
# app (DISPLAY=:N) only when told ($T/go), as gamescope does once Xwayland is ready
cat > "$T/gs.py" <<'EOF'
import ctypes, os, subprocess, sys, time
libc = ctypes.CDLL(None, use_errno=True)
xp, n, go = int(sys.argv[1]), sys.argv[2], sys.argv[3]
pfd = os.pidfd_open(xp)
held = []
for fd in os.listdir(f"/proc/{xp}/fd"):
    try: l = os.readlink(f"/proc/{xp}/fd/{fd}")
    except OSError: continue
    if l.startswith("socket:"):
        r = libc.syscall(438, pfd, int(fd), 0)          # pidfd_getfd
        if r >= 0: held.append(r)
print(len(held), flush=True)
while not os.path.exists(go): time.sleep(0.05)
subprocess.Popen(["env", "DISPLAY=:" + n, "sleep", "300"], preexec_fn=lambda: libc.prctl(1, 15))   # dies with us
time.sleep(300)
EOF
bash -c "python3 $T/gs.py $XP $N $T/go > $T/held 2>/dev/null; :" & GS=$!; PIDS+=($GS)
until_t 5 test -s "$T/held"
[ "$(cat "$T/held" 2>/dev/null || echo 0)" -gt 0 ] || { echo "test_clip_find: SKIP (pidfd_getfd not allowed here)"; exit 0; }
below "$XP" "$GS" && fail "the X server is below the stand-in (the test would prove nothing)" \
    || ok "X server :$N (pid $XP, parent $(awk '{print $4}' /proc/$XP/stat)) is not below the stand-in rdx_gs_src $GS"

printf '%s' "KEY-FROM-THE-APP-42" | "$BIN" --set --display ":$N" 2>"$T/set.log" & PIDS+=($!)
until_t 5 grep -q "<- CLIPBOARD" "$T/set.log" || fail "--set did not own the clipboard"

"$BIN" --get --x-of-pid "$GS" >/dev/null 2>"$T/get0.log" && fail "connected before the app was started"
grep -q "waiting until its Xwayland runs and the app is started" "$T/get0.log" \
    && ok "Xwayland runs, the app not yet: it waits" || { fail "no waiting before the app"; cat "$T/get0.log"; }
touch "$T/go"
sleep 0.3
out=$("$BIN" --get --x-of-pid "$GS" 2>"$T/get1.log")
[ "$out" = "KEY-FROM-THE-APP-42" ] && ok "--x-of-pid finds the display by the socket the stand-in gamescope listens on" \
    || { fail "socket: got '$out'"; cat "$T/get1.log"; }
grep -q "the X socket gamescope listens on" "$T/get1.log" && ok "and says so" || fail "no 'socket' line"

# stand-in app with DISPLAY only (no socket held anywhere below)
bash -c "env DISPLAY=:$N sleep 300; :" & AP=$!; PIDS+=($AP)
sleep 0.3
out=$("$BIN" --get --x-of-pid "$AP" 2>"$T/get2.log")
[ "$out" = "KEY-FROM-THE-APP-42" ] && grep -q "DISPLAY of the app" "$T/get2.log" \
    && ok "--x-of-pid finds the display by the app's DISPLAY" || { fail "DISPLAY: got '$out'"; cat "$T/get2.log"; }

bash -c "sleep 300; :" & NP=$!; PIDS+=($NP)
sleep 0.3
"$BIN" --get --x-of-pid "$NP" >/dev/null 2>"$T/get3.log" && fail "found a display below a process without one" \
    || { grep -q "waiting for the display of the gamescope below process $NP" "$T/get3.log" && ok "nothing below: no display, it waits and says so"; }

echo "test_clip_find: $([ $fails = 0 ] && echo "all passed" || echo "$fails FAILED")"
exit $((fails != 0))
