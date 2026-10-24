#!/bin/bash
# Install rdma-display from a built tree (run "make" first) for the desktop:
#   sudo desktop/install.sh host     # the computer that streams (rdx-hostd)
#   sudo desktop/install.sh client   # the computer with the screen (rdx-clientd)
#   sudo desktop/install.sh client-window   # a client in a window of this desktop, next to a host
#        (rdx-clientd-window.service, web UI 47992, /etc/rdx/client-window.json from
#        tools/client-window.example.json if there is none; e.g. a desktop PC as client of a multiseat host)
#   sudo desktop/install.sh host --seat N   # one more streamed seat on this host (N = 1..99):
#        rdx-hostd@N.service (ports +100*N, config /etc/rdx/hostd-seatN.json) + its own app entry
# Copies the tree to /opt/rdma-display, installs the systemd unit and ENABLES it (starts at boot; whether
# it runs right now is the caller's business: this script never starts or restarts it, so a stream in
# progress is not cut), the polkit rule that lets the desktop user start/stop it, the app-overview
# entry and the icons. The tray app (desktop/rdx-tray.py) starts the daemon when launched.
# (An older version installed the unit without enabling it: after the next reboot rdx-hostd stayed
# down until someone started it by hand.)
set -eu
ROLE=${1:?host|client|client-window}
SEAT=0
if [ "${2:-}" = "--seat" ]; then SEAT=${3:?--seat N}; fi
case "$SEAT" in ''|*[!0-9]*) echo "--seat needs a number" >&2; exit 2 ;; esac
[ "$SEAT" -le 99 ] || { echo "--seat 0..99" >&2; exit 2; }
[ "$SEAT" = 0 ] || [ "$ROLE" = host ] || { echo "--seat is for the host role" >&2; exit 2; }
case "$ROLE" in host) UNIT=rdx-hostd; ENTRY=rdx-host.desktop ;; client) UNIT=rdx-clientd; ENTRY=rdx-client.desktop ;;
     client-window) UNIT=rdx-clientd-window; ENTRY=rdx-client-window.desktop ;;
     *) echo "usage: $0 host|client|client-window" >&2; exit 2 ;; esac
SRC=$(cd "$(dirname "$0")/.." && pwd)
DST=/opt/rdma-display
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -x "$SRC/$( [ $ROLE = host ] && echo host/rdx_hostd || echo client/rdx_clientd )" ] || { echo "build first (make)" >&2; exit 1; }

# the tray app needs AppIndicator for Python (GNOME shows it via its AppIndicator extension)
if ! python3 -c 'import gi; gi.require_version("AyatanaAppIndicator3", "0.1")' 2>/dev/null; then
    DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y -q gir1.2-ayatanaappindicator3-0.1
fi

# a transient unit of the same name (hand-started) must go before the real one is loaded
if systemctl show -p FragmentPath --value $UNIT.service 2>/dev/null | grep -q '^/run/systemd/transient'; then
    systemctl stop $UNIT.service || true
fi
mkdir -p $DST /etc/rdx
if [ "$SRC" != "$DST" ]; then
    rm -rf $DST.new && mkdir -p $DST.new
    tar -C "$SRC" --exclude=.git --exclude=.claude --exclude=.playwright-mcp -cf - . | tar -C $DST.new -xf -
    rm -rf $DST.old; [ -d $DST ] && mv $DST $DST.old; mv $DST.new $DST; rm -rf $DST.old
fi
chmod 755 $DST/desktop/rdx-tray.py
install -m 644 $DST/tools/$UNIT.service /etc/systemd/system/$UNIT.service
if [ "$ROLE" = client-window ] && [ ! -e /etc/rdx/client-window.json ]; then
    install -m 644 $DST/tools/client-window.example.json /etc/rdx/client-window.json
fi
if [ "$SEAT" != 0 ]; then
    # one more seat: the template unit, and an app entry that starts exactly this seat
    install -m 644 $DST/tools/rdx-hostd@.service /etc/systemd/system/rdx-hostd@.service
    UNIT=rdx-hostd@$SEAT
    ENTRY=rdx-host-seat$SEAT.desktop
    sed -e "s|^Name=.*|Name=RDMA Display Host (seat $SEAT)|" \
        -e "s|^Exec=.*|Exec=/opt/rdma-display/desktop/rdx-tray.py --role host --seat $SEAT|" \
        $DST/desktop/rdx-host.desktop > $DST/desktop/$ENTRY
fi
install -D -m 644 $DST/desktop/50-rdma-display.rules /etc/polkit-1/rules.d/50-rdma-display.rules
if [ "$ROLE" = host ]; then
    # the client's input in a gamescope session reaches that gamescope only, never this desktop:
    # its devices go to the seat "seat-rdx" and the group rdx-input (desktop/72-rdma-display-input.rules;
    # rdx-hostd adds the app user to the group)
    getent group rdx-input >/dev/null || groupadd -r rdx-input
    install -D -m 644 $DST/desktop/72-rdma-display-input.rules /etc/udev/rules.d/72-rdma-display-input.rules
    # the stream's input devices are no power switches of this computer (its logind would act on a
    # client's power / sleep key): systemd's rule with them left out, shadowing it by name
    install -D -m 644 $DST/desktop/70-power-switch.rules /etc/udev/rules.d/70-power-switch.rules
    udevadm control --reload 2>/dev/null || true
fi
install -m 644 $DST/desktop/$ENTRY /usr/share/applications/$ENTRY
install -D -m 644 $DST/desktop/rdx-display.svg /usr/share/icons/hicolor/scalable/apps/rdx-display.svg
install -D -m 644 $DST/desktop/rdx-display-active.svg /usr/share/icons/hicolor/scalable/apps/rdx-display-active.svg
gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
update-desktop-database -q /usr/share/applications 2>/dev/null || true
systemctl daemon-reload
systemctl enable $UNIT.service 2>&1 | sed 's/^/[enable] /' >&2
[ "$(systemctl is-enabled $UNIT.service 2>/dev/null)" = enabled ] || { echo "$UNIT.service is not enabled" >&2; exit 1; }
echo "installed $ROLE: /opt/rdma-display, $UNIT.service (enabled at boot), app \"$(sed -n 's/^Name=//p' $DST/desktop/$ENTRY)\""
