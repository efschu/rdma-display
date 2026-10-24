#!/bin/bash
# End-to-end test of clip/rdx_clip: two Xvfb displays stand for the client's desktop (:CL) and the
# seat's gamescope Xwayland (:HO), the two helpers talk over TCP on 127.0.0.1, xclip plays the
# programs that copy and paste. Checks: the desktop's clipboard reaches the app at the connection,
# both directions with UTF-8, PRIMARY, no echo after a clipboard manager re-owns a text, 1 MiB goes
# (served by INCR), 1 MiB + 1 does not, a reconnect after the host helper restarts, --get.
# Needs Xvfb and xclip; without them it says so and exits 0 (skip).
#   tests/test_clip_e2e.sh [path/to/rdx_clip]
set -u
BIN=${1:-$(dirname "$0")/../clip/rdx_clip}
command -v Xvfb >/dev/null && command -v xclip >/dev/null || { echo "test_clip_e2e: SKIP (needs Xvfb and xclip)"; exit 0; }
[ -x "$BIN" ] || { echo "test_clip_e2e: $BIN not built"; exit 1; }
T=$(mktemp -d /tmp/test_clip_e2e.XXXXXX)
PORT=$((20000 + RANDOM % 20000)); CL=; HO=
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; [ -n "$CL" ] && pkill -f "xclip -display ($CL|$HO) " 2>/dev/null; wait 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fails=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
# until <seconds> <command...>: poll every 0.1 s
until_t() { local n=$(( $1 * 10 )); shift; while [ $n -gt 0 ]; do "$@" && return 0; sleep 0.1; n=$((n - 1)); done; return 1; }
put() { printf '%s' "$3" | xclip -display "$1" -selection "$2" -i; }       # put DISPLAY SEL TEXT
get() { timeout 3 xclip -display "$1" -selection "$2" -o 2>/dev/null; }
is()  { [ "$(get "$1" "$2")" = "$3" ]; }                                     # is DISPLAY SEL TEXT
sent() { grep -c -- "-> $2" "$T/$1.log"; }                                   # sent host|client CLIPBOARD

# two Xvfb on free display numbers (-displayfd); no GLX (its NVIDIA EGL vendor crashed Xvfb in a container)
xvfb() { Xvfb -displayfd 3 -nolisten tcp -extension GLX 3>"$T/$1.num" >/dev/null 2>&1 & PIDS+=($!); }
xvfb cl; xvfb ho
until_t 10 sh -c "[ -s $T/cl.num ] && [ -s $T/ho.num ]" || { echo "test_clip_e2e: Xvfb did not start"; exit 1; }
CL=:$(cat "$T/cl.num"); HO=:$(cat "$T/ho.num")
start_host() { "$BIN" --listen 127.0.0.1 --port $PORT --peer 127.0.0.1 --display "$HO" --primary >>"$T/host.log" 2>&1 & HP=$!; PIDS+=($HP); }

put "$CL" clipboard "copied on the desktop before the stream"
start_host
"$BIN" --connect 127.0.0.1 --port $PORT --display "$CL" --primary >"$T/client.log" 2>&1 & PIDS+=($!)

until_t 5 is "$HO" clipboard "copied on the desktop before the stream" \
    && ok "the desktop's clipboard is in the app right after the connection" || fail "initial clipboard did not arrive"
grep -q "peer ready: CLIPBOARD and PRIMARY" "$T/client.log" && ok "both sides agreed on PRIMARY" || fail "no PRIMARY agreement"

K1="Grüße – ДЕМО-KEY 😀 €"
put "$CL" clipboard "$K1"
until_t 5 is "$HO" clipboard "$K1" && ok "desktop -> app, UTF-8" || fail "desktop -> app: got '$(get "$HO" clipboard)'"

K2="ABCDE-FGHIJ-KLMNO"                      # the Steam key case: copied in the app, pasted on the desktop
put "$HO" clipboard "$K2"
until_t 5 is "$CL" clipboard "$K2" && ok "app -> desktop" || fail "app -> desktop: got '$(get "$CL" clipboard)'"

put "$HO" primary "selected in the app"
until_t 5 is "$CL" primary "selected in the app" && ok "PRIMARY app -> desktop" || fail "PRIMARY did not arrive"

sleep 0.5
h0=$(sent host CLIPBOARD); c0=$(sent client CLIPBOARD)
# a clipboard manager (or gamescope on its other Xwaylands) re-owns what we set, with the same text
get "$CL" clipboard | xclip -display "$CL" -selection clipboard -i
get "$HO" clipboard | xclip -display "$HO" -selection clipboard -i
sleep 1.5
h1=$(sent host CLIPBOARD); c1=$(sent client CLIPBOARD)
[ "$h0" = "$h1" ] && [ "$c0" = "$c1" ] && ok "re-owned copies are not sent back (host $h0 -> $h1, client $c0 -> $c1 frames)" \
    || fail "echo: host $h0 -> $h1, client $c0 -> $c1"
is "$CL" clipboard "$K2" && is "$HO" clipboard "$K2" && ok "both still hold the same text" || fail "texts diverged"

head -c 1048576 /dev/zero | tr '\0' 'k' > "$T/mib"
xclip -display "$HO" -selection clipboard -i < "$T/mib"
until_t 8 sh -c "[ \"\$(timeout 3 xclip -display $CL -selection clipboard -o 2>/dev/null | wc -c)\" = 1048576 ]" \
    && ok "1 MiB app -> desktop (served by INCR on the desktop side)" || fail "1 MiB did not arrive: $(get "$CL" clipboard | wc -c) bytes"
printf 'x' >> "$T/mib"
xclip -display "$CL" -selection clipboard -i < "$T/mib"
until_t 5 grep -q "1048577 bytes, over the limit" "$T/client.log" && ok "1 MiB + 1 is not sent (log line)" || fail "no limit line"
[ "$(get "$HO" clipboard | wc -c)" = 1048576 ] && ok "the app keeps its last text" || fail "the app's clipboard changed"

kill $HP; wait $HP 2>/dev/null
put "$CL" clipboard "after the reconnect"
start_host
until_t 6 is "$HO" clipboard "after the reconnect" && ok "the client reconnects and sends the desktop's clipboard again" \
    || fail "no reconnect"

[ "$("$BIN" --get --display "$CL" 2>/dev/null)" = "after the reconnect" ] && ok "--get prints the clipboard" || fail "--get"
grep -q "ABCDE" "$T/host.log" "$T/client.log" && fail "a text shows in a log" || ok "no text in the logs"

if [ $fails -gt 0 ]; then echo "--- host.log"; cat "$T/host.log"; echo "--- client.log"; cat "$T/client.log"; fi
echo "test_clip_e2e: $([ $fails = 0 ] && echo "all passed" || echo "$fails FAILED")"
exit $((fails != 0))
