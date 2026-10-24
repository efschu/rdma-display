#!/bin/bash
# Provision, update and Steam-bootstrap rdma-display seat containers on a Proxmox host (docs/MULTISEAT.md).
# Run on the host as root. The container comes from tools/pve-seat-create.sh.
#
#   pve-seat-provision.sh install --ct ID --seat N --gpu PCI --build DIR --gs DIR|TGZ [--user user]
#                         [--name "RDMA Display - 5090"] [--priority high|normal|low] [--link-ip IP]
#                         [--seatmgr URL] [--seatmgr-ui URL] [--seatmgr-conf /etc/rdx/seatmgr.json]
#       starts the container, installs the packages, the NVIDIA userspace that matches the host's kernel
#       module (no kernel parts) and the desktop user (--user, default "user"; lingering user manager: pipewire without a
#       display manager), stops it, then does "update", writes /etc/rdx/hostd-seatN.json if there is none
#       (with "desktop_user": that user; an existing one only gets desktop_user if it has none),
#       enables rdx-hostd@N and adds the seat to rdx-seatmgr.
#   pve-seat-provision.sh update --ct ID --seat N --build DIR --gs DIR|TGZ
#       offline, the container stopped (pct mount): /opt/rdx-gs (the patched gamescope with its private
#       prefix: a directory with bin/ and prefix/, or a .tgz whose top directory has them; the manifest of
#       its gamescope WSI layer goes to /usr/share/vulkan/implicit_layer.d), /opt/rdma-display-src (the tree)
#       and, through its desktop/install.sh, /opt/rdma-display, the units, the app entry and the polkit
#       rule. /etc/rdx stays as it is (a copy goes to the host's /var/backups/rdx-etc-<ID>-<time>).
#       Lists libraries the new binaries miss.
#   pve-seat-provision.sh packages --ct ID
#       installs the package list below into an existing seat: with pct exec when the container runs,
#       in a chroot of its file system when it is stopped (no service is started there).
#   pve-seat-provision.sh steam --ct ID [--user user] [--timeout 1500]
#       first start of Steam as the user, software rendering only (Xvfb, Mesa GLX/EGL, lavapipe), until
#       ~/.steam/steam/ubuntu12_32 exists and the login window is up, then "steam -shutdown". Watches the
#       host's NVIDIA process list meanwhile and stops at once if a process of the container shows up.
#       Starts the container if needed and stops it again afterwards.
#
# --build is a built rdma-display tree ("make"), --gs a gamescope install tree, both on the host's file
# system. Nothing here creates a GPU context (the NVIDIA userspace is installed, not run).
set -euo pipefail

CMD=${1:-}; [ $# -gt 0 ] && shift
CT="" SEAT="" GPU="" BUILD="" GS="" NAME="" PRIO=normal LINK="" SMGR="" SMGR_UI="" SMCONF=/etc/rdx/seatmgr.json
USERN=user TIMEOUT=1500
while [ $# -gt 0 ]; do
  case "$1" in
    --ct) CT=$2 ;; --seat) SEAT=$2 ;; --gpu) GPU=$(echo "$2" | tr 'A-F' 'a-f') ;; --build) BUILD=$2 ;; --gs) GS=$2 ;;
    --name) NAME=$2 ;; --priority) PRIO=$2 ;; --link-ip) LINK=$2 ;; --seatmgr) SMGR=$2 ;; --seatmgr-ui) SMGR_UI=$2 ;;
    --seatmgr-conf) SMCONF=$2 ;; --user) USERN=$2 ;; --timeout) TIMEOUT=$2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift 2
done
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 2; }
[ -n "$CT" ] || usage
pct status "$CT" >/dev/null 2>&1 || { echo "no CT $CT" >&2; exit 1; }
ROOT=/var/lib/lxc/$CT/rootfs

state() { pct status "$CT" | awk '{print $2}'; }
start_ct() { [ "$(state)" = running ] || { pct start "$CT"; sleep 5; }; }
mounted=0
mount_ct() {
  [ "$(state)" = stopped ] || { echo "CT $CT must be stopped for this" >&2; exit 1; }
  pct mount "$CT" >/dev/null; mounted=1
}
unmount_ct() {
  mountpoint -q "$ROOT/proc" && umount "$ROOT/proc"
  [ "$mounted" = 1 ] && pct unmount "$CT"; mounted=0
}
trap unmount_ct EXIT

# pids of NVIDIA processes (compute and graphics) that belong to the container
gpu_pids_of_ct() {
  { nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null
    nvidia-smi pmon -c 1 2>/dev/null | awk '$1 !~ /^#/ && $2 ~ /^[0-9]+$/ {print $2}'; } | sort -u |
    while read -r p; do if grep -qs "/lxc/$CT/" "/proc/$p/cgroup"; then echo "$p"; fi; done
}

# ------------------------------------------------------------------------------------------- update
do_update() {
  [ -n "$SEAT" ] && [ -d "$BUILD" ] && [ -e "$GS" ] || { echo "update needs --seat, --build DIR, --gs DIR|TGZ" >&2; exit 2; }
  [ -x "$BUILD/host/rdx_hostd" ] || { echo "$BUILD is not built (no host/rdx_hostd)" >&2; exit 1; }
  mount_ct
  local bak=/var/backups/rdx-etc-$CT-$(date +%m%d%H%M)
  [ -d "$ROOT/etc/rdx" ] && cp -a "$ROOT/etc/rdx" "$bak"
  # the gamescope tree
  rm -rf "$ROOT/opt/rdx-gs.new" && mkdir -p "$ROOT/opt/rdx-gs.new"
  if [ -d "$GS" ]; then
    tar -C "$GS" -cf - bin prefix | tar -C "$ROOT/opt/rdx-gs.new" -xf -
  else
    tar -C "$ROOT/opt/rdx-gs.new" -xzf "$GS" --strip-components=1
  fi
  [ -x "$ROOT/opt/rdx-gs.new/bin/gamescope" ] || { echo "--gs has no bin/gamescope" >&2; exit 1; }
  rm -rf "$ROOT/opt/rdx-gs.old"; [ -d "$ROOT/opt/rdx-gs" ] && mv "$ROOT/opt/rdx-gs" "$ROOT/opt/rdx-gs.old"
  mv "$ROOT/opt/rdx-gs.new" "$ROOT/opt/rdx-gs"; rm -rf "$ROOT/opt/rdx-gs.old"
  # gamescope's WSI layer (host/install-gamescope.sh puts it into the tree): its manifest where the
  # Vulkan loader and Steam's pressure-vessel find it; a tree without it takes the manifest away again
  # (x86_64 for 64-bit processes, x86 for Proton's 32-bit ones such as Battle.net.exe)
  local wsi
  for wsi in prefix/share/vulkan/implicit_layer.d/rdx-gamescope-wsi.x86_64.json prefix/share/vulkan/implicit_layer.d/rdx-gamescope-wsi.x86.json; do
    if [ -f "$ROOT/opt/rdx-gs/$wsi" ]; then
      install -D -m 0644 "$ROOT/opt/rdx-gs/$wsi" "$ROOT/usr/share/vulkan/implicit_layer.d/${wsi##*/}"
      echo "gamescope WSI layer ${wsi##*/}: on ($(sed -n 's/.*"library_path": *"\([^"]*\)".*/\1/p' "$ROOT/opt/rdx-gs/$wsi"))"
    else
      rm -f "$ROOT/usr/share/vulkan/implicit_layer.d/${wsi##*/}"
      echo "gamescope WSI layer ${wsi##*/}: none in the gamescope tree"
    fi
  done
  # rdma-display through its own installer, in the container's file system
  rm -rf "$ROOT/opt/rdma-display-src" && mkdir -p "$ROOT/opt/rdma-display-src"
  tar -C "$BUILD" --exclude=.git --exclude=.claude --exclude=.playwright-mcp -cf - . | tar -C "$ROOT/opt/rdma-display-src" -xf -
  mount -t proc proc "$ROOT/proc"
  chroot "$ROOT" bash /opt/rdma-display-src/desktop/install.sh host --seat "$SEAT" 2>&1 | grep -v "Running in chroot" | tail -2
  echo "missing libraries: $(chroot "$ROOT" sh -c 'for f in /opt/rdma-display/*/rdx_* /opt/rdx-gs/bin/*; do [ -x "$f" ] && ldd "$f" 2>/dev/null | grep "not found" | sed "s|^|$f: |"; done' | sort -u | tr '\n' ';' | sed 's/;$//' || true)"
  # the seat's sound (seen: crackle at the client): PipeWire runs its graph realtime only with the
  # limits Ubuntu's pipewire ships for the group pipewire (/etc/security/limits.d/25-pw-rlimits.conf),
  # and FluidSynth's user service held an idle stream that xran the graph: mask it
  chroot "$ROOT" sh -c "getent group pipewire >/dev/null && id $USERN >/dev/null 2>&1 && usermod -aG pipewire $USERN"
  mkdir -p "$ROOT/etc/systemd/user" && ln -sfn /dev/null "$ROOT/etc/systemd/user/fluidsynth.service"
  echo "audio: $(chroot "$ROOT" id -nG "$USERN" 2>/dev/null | tr ' ' '\n' | grep -qx pipewire && echo "$USERN in pipewire (realtime)" || echo "no pipewire group"), fluidsynth masked"
  # Battle.net's starter (rdx-hostd app "battlenet"; umu-launcher and GE-Proton are the user's, not set up here)
  install -m 0755 "$BUILD/tools/rdx-battlenet" "$ROOT/usr/local/bin/rdx-battlenet"
  # Rockstar Games Launcher's starter (rdx-hostd app "rockstar"; install the launcher itself once as
  # the desktop user with: rdx-rockstar --install)
  install -m 0755 "$BUILD/tools/rdx-rockstar" "$ROOT/usr/local/bin/rdx-rockstar"
  # GTA Online's BattlEye servers blocked (at the user's own risk): with the Proton BattlEye
  # Runtime (rdx-rockstar) invite-only sessions work. Outside Proxmox's "# --- BEGIN PVE ---" block,
  # which is the only part of /etc/hosts Proxmox rewrites
  grep -q "paradise-s1.battleye.com" "$ROOT/etc/hosts" || printf '%s\n' \
    "# GTA Online: BattlEye of GTA V Legacy/Enhanced blocked (rdx-rockstar, invite-only sessions)" \
    "0.0.0.0 paradise-s1.battleye.com" "0.0.0.0 test-s1.battleye.com" "0.0.0.0 paradiseenhanced-s1.battleye.com" \
    >> "$ROOT/etc/hosts"
  umount "$ROOT/proc"
  if [ -d "$bak" ]; then
    diff -r "$bak" "$ROOT/etc/rdx" >/dev/null && echo "/etc/rdx unchanged (backup $bak)" || echo "WARNING: /etc/rdx changed (backup $bak)"
  fi
  unmount_ct
}

# ------------------------------------------------------------------------------------------- packages
# runtime of rdma-display and the patched gamescope (incl. Xwayland 24.1), pipewire, the session tools,
# software rendering (Steam's first start), Steam, Lutris, Wine, the GPU benchmarks of tools/rdx_perf.py
PKGS="locales ca-certificates curl wget sudo dbus-user-session systemd-container libpam-systemd polkitd
  pipewire pipewire-pulse pipewire-alsa wireplumber libspa-0.2-modules pulseaudio-utils libinput-tools
  xwayland xvfb x11-utils x11-apps xauth x11-xserver-utils mesa-vulkan-drivers mesa-vulkan-drivers:i386 libvulkan1 libvulkan1:i386
  vulkan-tools libgl1 libgl1:i386 libgl1-mesa-dri libgl1-mesa-dri:i386 libegl1 libegl-mesa0 libgles2 libgbm1
  libinput10 libseat1 libxkbcommon0 libxcb-composite0 libxcb-ewmh2 libxcb-res0 libxres1 libxmu6 libxtst6
  libxcomposite1 libdecor-0-0 libluajit-5.1-2 libavif16 libdisplay-info1 libeis1 libxcvt0 libxfont2 libepoxy0
  libibverbs1 ibverbs-providers ibverbs-utils rdma-core libpipewire-0.3-0 libsamplerate0 libcjson1 libsdl2-2.0-0 libyuv0
  python3 python3-gi gir1.2-gtk-3.0 gir1.2-ayatanaappindicator3-0.1 kmod pciutils usbutils patchelf
  steam-installer lutris wine64 wine32 winetricks fonts-dejavu-core
  vkmark glmark2-x11"
APT='export DEBIAN_FRONTEND=noninteractive
sed -i "s/^Components: main$/Components: main restricted universe multiverse/" /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null
dpkg --print-foreign-architectures | grep -qx i386 || dpkg --add-architecture i386
apt-get -o DPkg::Lock::Timeout=600 update -q >/dev/null
echo steam steam/question select "I AGREE" | debconf-set-selections
echo steam steam/license note "" | debconf-set-selections
apt-get -o DPkg::Lock::Timeout=600 install -y -q --no-install-recommends $RDX_PKGS >/tmp/rdx-apt.log 2>&1
rc=$?; tail -2 /tmp/rdx-apt.log; echo "apt EXIT $rc"'

do_packages() {
  if [ "$(state)" = running ]; then
    pct exec "$CT" -- env RDX_PKGS="$PKGS" bash -c "$APT"
  else
    mount_ct
    local d
    for d in proc sys dev dev/pts; do mount --bind /$d "$ROOT/$d"; done
    printf '#!/bin/sh\nexit 101\n' > "$ROOT/usr/sbin/policy-rc.d"; chmod 755 "$ROOT/usr/sbin/policy-rc.d"
    chroot "$ROOT" env RDX_PKGS="$PKGS" bash -c "$APT" || true
    rm -f "$ROOT/usr/sbin/policy-rc.d"
    for d in dev/pts dev sys proc; do umount "$ROOT/$d"; done
    unmount_ct
  fi
}

# ------------------------------------------------------------------------------------------- install
INSIDE='#!/bin/bash
# inside the seat container: packages, NVIDIA userspace (no kernel module), desktop user
set -u
NVVER=$1; U=$2
locale-gen en_US.UTF-8 de_DE.UTF-8 >/dev/null
# NVIDIA userspace of exactly the host kernel module version, no kernel parts
if [ "$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)" != "$NVVER" ]; then
  sh /opt/nvidia-installer/NVIDIA-Linux-x86_64-$NVVER.run --silent --no-kernel-module --no-questions \
    --no-nouveau-check --no-x-check --install-libglvnd --no-systemd 2>&1 | tail -3
fi
echo "nvidia userspace: $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1)"
# the .run puts the 32-bit libraries into /usr/lib32, which the loader of an Ubuntu without libc6-i386
# never searches: 32-bit Vulkan/GL then finds no NVIDIA driver (Battle.net under Proton, 32-bit DXVK)
if ls /usr/lib32/libGLX_nvidia.so.* >/dev/null 2>&1 && ! ldconfig -p | grep -q "libGLX_nvidia.so.0 (libc6)"; then
  echo /usr/lib32 > /etc/ld.so.conf.d/zz-nvidia-lib32.conf && ldconfig
fi
echo "nvidia 32-bit: $(ldconfig -p | grep "libGLX_nvidia.so.0 (libc6)" || echo none)"
# the desktop user (Steam refuses root) with a lingering user manager: pipewire without a display manager
id "$U" >/dev/null 2>&1 || useradd -m -s /bin/bash -G video,render,input,audio,sudo "$U"
# PipeWire realtime (25-pw-rlimits.conf of the group pipewire); no idle FluidSynth stream (update does it too)
getent group pipewire >/dev/null && usermod -aG pipewire "$U"
systemctl --global mask fluidsynth.service >/dev/null 2>&1 || true
loginctl enable-linger "$U"
# the bound-in DRM nodes keep the host gid; give the user a group with that id
for n in /dev/dri/card* /dev/dri/renderD*; do
  [ -e "$n" ] || continue
  g=$(stat -c %g "$n"); name=$(getent group "$g" | cut -d: -f1)
  if [ "$name" != render ] && [ "$name" != video ]; then
    getent group hostdri >/dev/null || groupadd -o -g "$g" hostdri
    usermod -aG hostdri "$U"
  fi
done
id "$U"
'

do_install() {
  [ -n "$SEAT" ] && [ -n "$GPU" ] || { echo "install needs --seat and --gpu" >&2; exit 2; }
  local nv; nv=$(cat /sys/module/nvidia/version)
  # rdx-seatmgr: the new seat (its GPU's own sound card follows by default), before the first start:
  # the container's start binds the GPU of its seat there (seat/rdx_seat_gpu.py, the hookscript)
  if [ -f "$SMCONF" ]; then
    python3 - "$SMCONF" "$SEAT" "$CT" "$GPU" "${NAME:-seat $SEAT}" <<'EOF'
import json, sys
p, seat, ct, gpu, name = sys.argv[1:]
c = json.load(open(p))
c.setdefault("seats", {}).setdefault(seat, {"ct": int(ct), "name": name, "gpu": gpu})
json.dump(c, open(p + ".tmp", "w"), indent=1)
import os; os.replace(p + ".tmp", p)
print("rdx-seatmgr seat", seat, "->", c["seats"][seat])
EOF
    systemctl try-restart rdx-seatmgr.service || true
  fi
  start_ct
  printf '%s' "$INSIDE" > /tmp/rdx-seat-inside-$CT.sh
  pct push "$CT" /tmp/rdx-seat-inside-$CT.sh /var/tmp/rdx-seat-inside.sh --perms 0755
  rm -f /tmp/rdx-seat-inside-$CT.sh
  echo "packages ..."
  do_packages
  echo "provisioning CT $CT inside (log /var/log/rdx-provision.log in the CT) ..."
  pct exec "$CT" -- bash -c "bash /var/tmp/rdx-seat-inside.sh $nv $USERN > /var/log/rdx-provision.log 2>&1; tail -4 /var/log/rdx-provision.log"
  pct stop "$CT"
  do_update
  mount_ct
  mkdir -p "$ROOT/etc/rdx"
  # desktop_user: the lingering user runs gamescope and the apps (a seat container has no graphical
  # session; rdx-hostd would also find the one lingering user itself, this names it explicitly)
  python3 - "$ROOT/etc/rdx/hostd-seat$SEAT.json" "$SEAT" "$GPU" "$NAME" "$PRIO" "$LINK" "$SMGR" "$SMGR_UI" "$USERN" <<'EOF'
import json, os, sys
p, seat, gpu, name, prio, link, smgr, ui, user = sys.argv[1:]
if os.path.exists(p):
    c = json.load(open(p))
    if c.get("desktop_user"):
        print("kept", p)
    else:
        c["desktop_user"] = user
        json.dump(c, open(p, "w"), indent=1)
        print("kept", p, "+ desktop_user", user)
else:
    c = {"seat": int(seat), "gpu": gpu, "priority": prio, "desktop_user": user}
    for k, v in (("seat_name", name), ("link_ip", link), ("seatmgr", smgr), ("seatmgr_ui", ui)):
        if v:
            c[k] = v
    json.dump(c, open(p, "w"), indent=1)
    print("wrote", p, json.dumps(c))
EOF
  local w=$ROOT/etc/systemd/system/multi-user.target.wants
  mkdir -p "$w"; ln -sf /etc/systemd/system/rdx-hostd@.service "$w/rdx-hostd@$SEAT.service"
  unmount_ct
  echo "CT $CT is seat $SEAT, stopped"
}

# ------------------------------------------------------------------------------------------- steam
STEAM_INSIDE='#!/bin/bash
# as the desktop user: first Steam start with software rendering only, until the login window
set -u
T=$1
export DISPLAY=:91 XDG_RUNTIME_DIR=/run/user/$(id -u)
export __GLX_VENDOR_LIBRARY_NAME=mesa __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json:/usr/share/vulkan/icd.d/lvp_icd.i686.json
export LIBGL_ALWAYS_SOFTWARE=1 STEAM_DISABLE_GPU=1
# the Debian steam-installer asks "Install?" once (zenity, default Cancel); running this command is
# that decision, so exactly this question is answered, every other zenity call goes to zenity
mkdir -p /tmp/rdx-steam-bin
printf "%s\n" "#!/bin/sh" "case \" \$* \" in *\"--title=Steam installer\"*) exit 0 ;; esac" "exec /usr/bin/zenity \"\$@\"" > /tmp/rdx-steam-bin/zenity
chmod 755 /tmp/rdx-steam-bin/zenity
export PATH=/tmp/rdx-steam-bin:$PATH
Xvfb :91 -screen 0 1280x800x24 -nolisten tcp >/tmp/rdx-xvfb.log 2>&1 &
XV=$!
sleep 2
steam -cef-disable-gpu -cef-disable-gpu-compositing >/tmp/rdx-steam.log 2>&1 &
S=$!
ok=0 seen=0
for i in $(seq 1 $((T / 5))); do
  sleep 5
  # the login screen: the client UI (steamwebhelper) is up and shows a Steam window -- the
  # update window of the bootstrapper is called "Steam" too, so the state must hold for 20 s
  w=$(xwininfo -root -tree 2>/dev/null | grep -oiE "\"(Sign in to Steam|Steam Sign In|Steam Login|Steam)\"" | head -1)
  if [ -d ~/.steam/steam/ubuntu12_32 ] && [ -n "$w" ] && pgrep -u "$(id -u)" -f steamwebhelper >/dev/null; then
    seen=$((seen + 1)); [ $seen -ge 4 ] && { ok=1; break; }
  else
    seen=0
  fi
  kill -0 $S 2>/dev/null || break
done
echo "after $((i * 5)) s: ubuntu12_32=$([ -d ~/.steam/steam/ubuntu12_32 ] && echo yes || echo no) window=${w:-none}"
xwd -root -silent > /tmp/rdx-steam-login.xwd 2>/dev/null
# quit: "steam -shutdown", then the steam://exit URL, then SIGTERM to the client itself (Steam shuts
# down in order on it); only an installed Steam is asked (otherwise the launcher would install it now)
client() { pgrep -u "$(id -u)" -f "ubuntu12_32/steam( |$)" >/dev/null; }
how=""
for step in "-shutdown" "steam://exit" "SIGTERM"; do
  client || break
  if [ "$step" = SIGTERM ]; then pkill -TERM -u "$(id -u)" -f "ubuntu12_32/steam( |$)"
  elif [ -x ~/.steam/steam/ubuntu12_32/steam ]; then steam "$step" >/dev/null 2>&1 &
  fi
  for j in $(seq 1 15); do client || break; sleep 2; done
  client || how=$step
done
client && { echo "steam did not quit, killed"; pkill -KILL -u "$(id -u)" -f "ubuntu12_32/steam( |$)"; }
[ -n "$how" ] && echo "steam quit after: $how"
client || rm -f ~/.steam/steam.pid ~/.steam/steam.pipe       # runtime leftovers (a copied home must not look running)
for j in $(seq 1 15); do kill -0 $S 2>/dev/null || break; sleep 1; done
kill $S $XV 2>/dev/null
[ $ok = 1 ] && echo "STEAM BOOTSTRAP OK" || { echo "STEAM BOOTSTRAP FAILED"; tail -20 /tmp/rdx-steam.log; }
'

do_steam() {
  local started=0
  [ "$(state)" = running ] || { start_ct; started=1; }
  # the seat daemon may start its home app on a connected monitor (GPU): held off meanwhile
  local held; held=$(pct exec "$CT" -- systemctl list-units --plain --no-legend 'rdx-hostd@*' | awk '{print $1}' | tr '\n' ' ')
  if [ -n "$held" ]; then pct exec "$CT" -- systemctl stop $held; fi
  local before; before=$(gpu_pids_of_ct)
  [ -z "$before" ] || { echo "CT $CT has GPU processes already: $before" >&2; exit 1; }
  printf '%s' "$STEAM_INSIDE" > /tmp/rdx-steam-$CT.sh
  pct push "$CT" /tmp/rdx-steam-$CT.sh /tmp/rdx-steam-bootstrap.sh --perms 0755
  rm -f /tmp/rdx-steam-$CT.sh
  pct exec "$CT" -- su - "$USERN" -c "bash /tmp/rdx-steam-bootstrap.sh $TIMEOUT" > /tmp/rdx-steam-$CT.out 2>&1 &
  local job=$! bad=""
  while kill -0 $job 2>/dev/null; do
    bad=$(gpu_pids_of_ct)
    if [ -n "$bad" ]; then
      echo "GPU process of CT $CT: $bad ($(ps -o comm= -p "$bad" | tr '\n' ' ')) -- stopping Steam" >&2
      pct exec "$CT" -- pkill -u "$USERN" -f steam || true
      break
    fi
    sleep 2
  done
  wait $job || true
  cat /tmp/rdx-steam-$CT.out
  echo "GPU processes of CT $CT after: $(gpu_pids_of_ct | tr '\n' ' ')"
  if [ "$started" = 1 ]; then pct stop "$CT"; elif [ -n "$held" ]; then pct exec "$CT" -- systemctl start $held; fi
  [ -z "$bad" ] && grep -q "STEAM BOOTSTRAP OK" /tmp/rdx-steam-$CT.out
}

case "$CMD" in
  install) do_install ;;
  packages) do_packages ;;
  update) do_update ;;
  steam) do_steam ;;
  *) usage ;;
esac
