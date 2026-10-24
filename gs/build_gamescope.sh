#!/bin/bash
# Build gamescope with the rdx backend (the gamescope path) on Ubuntu 24.04.
#
#   gs/build_gamescope.sh [WORKDIR]      (default /usr/local/src/rdx-gs; log WORKDIR/build.log)
#
# gamescope 3.16's wlroots needs newer wayland, libdrm, xkbcommon, pixman and
# wayland-protocols than noble ships; they are built into WORKDIR/prefix and the
# gamescope binary gets an rpath to it (nothing in /usr is replaced).
# Result: WORKDIR/gamescope/build/src/gamescope (rdx_gs_src --gamescope PATH) and
# WORKDIR/xwayland/bin/Xwayland (24.1, explicit sync; see below), and gamescope's WSI layer
# WORKDIR/gamescope/build/layer/libVkLayer_FROG_gamescope_wsi_x86_64.so (meson builds it by default;
# host/install-gamescope.sh installs it with its manifest, without it Vulkan apps present through Xwayland).
# The 32-bit build of that layer (Proton's 32-bit processes: Battle.net.exe, older games) is
# gs/build_gamescope_wsi_i386.sh, run after this script; host/install-gamescope.sh installs both.
# Shared or small machines: NINJA_JOBS=2 limits the parallel jobs, GS_MESON_ARGS adds gamescope
# meson options (e.g. -Db_lto=false keeps the final link small), e.g.:
#   systemd-run --scope -p MemoryMax=2G nice -n 19 env NINJA_JOBS=2 GS_MESON_ARGS=-Db_lto=false gs/build_gamescope.sh
set -u
W=${1:-/usr/local/src/rdx-gs}
NJ=${NINJA_JOBS:+-j$NINJA_JOBS}
PATCH=$(cd "$(dirname "$0")" && pwd)/gamescope-rdx.patch
TAG=3.16.31
mkdir -p "$W"
exec > >(tee "$W/build.log") 2>&1
P=$W/prefix
L=$P/lib/x86_64-linux-gnu
export PKG_CONFIG_PATH=$L/pkgconfig:$P/share/pkgconfig
export PATH=$P/bin:$PATH
export DEBIAN_FRONTEND=noninteractive
apt-get -o DPkg::Lock::Timeout=900 install -y -q --no-install-recommends \
  build-essential meson ninja-build cmake pkg-config git glslang-tools glslang-dev \
  libvulkan-dev libinput-dev libseat-dev libudev-dev libwayland-dev libx11-dev libx11-xcb-dev \
  libxcb1-dev libxcb-composite0-dev libxcb-icccm4-dev libxcb-res0-dev libxcb-xfixes0-dev libxcb-ewmh-dev \
  libxdamage-dev libxcomposite-dev libxrender-dev libxext-dev libxfixes-dev libxxf86vm-dev libxtst-dev \
  libxres-dev libxmu-dev libxi-dev libxcursor-dev libxrandr-dev libxkbcommon-x11-dev libcap-dev libsdl2-dev \
  libpipewire-0.3-dev libdisplay-info-dev libavif-dev libluajit-5.1-dev libeis-dev libdecor-0-dev hwdata \
  xwayland libgbm-dev libegl-dev libgles-dev libliftoff-dev libstb-dev libglm-dev libxcb-present-dev \
  libxcb-dri3-dev libxcb-xkb-dev libffi-dev libexpat1-dev libxml2-dev libpciaccess-dev bison flex \
  x11-xkb-utils xkb-data vulkan-tools \
  libxkbfile-dev libxfont-dev libxcvt-dev libxshmfence-dev libtirpc-dev libgcrypt20-dev libbsd-dev \
  libepoxy-dev libgl-dev libxau-dev libxdmcp-dev mesa-common-dev
# dep <pkg-config name> <min version> <git url> <tag> [meson options...]
dep() {
  local pc=$1 ver=$2 url=$3 tag=$4; shift 4
  pkg-config --atleast-version="$ver" "$pc" && { echo "HAVE $pc $(pkg-config --modversion "$pc")"; return 0; }
  local d=$W/src-$pc
  rm -rf "$d"
  git clone --depth 1 --branch "$tag" "$url" "$d" || { echo "CLONE-FAIL $pc"; exit 1; }
  (cd "$d" && meson setup build --prefix="$P" "$@" && ninja $NJ -C build install) || { echo "BUILD-FAIL $pc"; exit 1; }
  echo "BUILT $pc $(pkg-config --modversion "$pc")"
}
dep wayland-server 1.24 https://gitlab.freedesktop.org/wayland/wayland.git 1.24.0 -Ddocumentation=false -Dtests=false -Ddtd_validation=false
dep libdrm 2.4.129 https://gitlab.freedesktop.org/mesa/drm.git libdrm-2.4.129 -Dtests=false -Dvalgrind=disabled -Dcairo-tests=disabled -Dman-pages=disabled
dep wayland-protocols 1.47 https://gitlab.freedesktop.org/wayland/wayland-protocols.git 1.47 -Dtests=false
dep xkbcommon 1.8.0 https://github.com/xkbcommon/libxkbcommon.git xkbcommon-1.8.1 -Denable-docs=false -Denable-wayland=false -Denable-tools=false -Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale
dep pixman-1 0.46.0 https://gitlab.freedesktop.org/pixman/pixman.git pixman-0.46.4 -Dtests=disabled -Ddemos=disabled -Dgtk=disabled

# Xwayland >= 24.1: explicit sync (wp_linux_drm_syncobj_v1 towards gamescope, DRI3 1.4 towards
# X clients). noble ships 23.2 without it; NVIDIA has no implicit sync, so gamescope then samples
# X11 windows (Steam's steamwebhelper, Proton games) before the GPU finished them: frames of the
# clear colour, i.e. black/white flicker. Own prefix WORKDIR/xwayland (with xorgproto 2024.1 for
# DRI3 1.4), so gamescope's wlroots keeps the system Xwayland as its compiled-in default;
# host/install-gamescope.sh puts it next to gamescope, where the rdx backend picks it up.
XW=$W/xwayland
XWTAG=xwayland-24.1.9
if [ -x "$XW/bin/Xwayland" ] && "$XW/bin/Xwayland" -version 2>&1 | grep -q "Version ${XWTAG#xwayland-}"; then
  echo "HAVE Xwayland ${XWTAG#xwayland-}"
else
  (
    export PKG_CONFIG_PATH=$XW/share/pkgconfig:$XW/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH
    rm -rf "$W/src-xorgproto" "$W/src-xserver"
    git clone --depth 1 --branch xorgproto-2024.1 https://gitlab.freedesktop.org/xorg/proto/xorgproto.git "$W/src-xorgproto" &&
    (cd "$W/src-xorgproto" && meson setup build --prefix="$XW" && ninja $NJ -C build install) &&
    git clone --depth 1 --branch "$XWTAG" https://gitlab.freedesktop.org/xorg/xserver.git "$W/src-xserver" &&
    cd "$W/src-xserver" &&
    meson setup build --prefix="$XW" -Dbuildtype=release -Dxvfb=false -Ddri3=true -Dglamor=true -Dglx=true \
      -Dxwayland_ei=false -Dsha1=libgcrypt -Dlibdecor=true -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false \
      -Dxkb_dir=/usr/share/X11/xkb -Dxkb_bin_dir=/usr/bin -Dxkb_output_dir=/var/lib/xkb \
      -Ddefault_font_path=/usr/share/fonts/X11/misc,/usr/share/fonts/X11/Type1,built-ins \
      -Dc_link_args=-Wl,-rpath,$L &&
    ninja $NJ -C build install
  ) || { echo "BUILD-FAIL Xwayland"; exit 1; }
  echo "BUILT $("$XW/bin/Xwayland" -version 2>&1 | grep -m1 Version)"
fi

if [ ! -d "$W/gamescope/.git" ]; then
  git clone --depth 1 --branch "$TAG" --recurse-submodules --shallow-submodules \
    https://github.com/ValveSoftware/gamescope.git "$W/gamescope" || exit 1
fi
cd "$W/gamescope"
if ! grep -q CRdxBackend src/backends.h; then
  git apply "$PATCH" || { echo "PATCH-FAIL"; exit 1; }
fi
rm -rf build
meson setup build -Dbuildtype=release -Dpipewire=enabled -Denable_openvr_support=false -Denable_tests=false \
  -Dbenchmark=disabled -Dc_link_args=-Wl,-rpath,$L -Dcpp_link_args=-Wl,-rpath,$L \
  --force-fallback-for=wlroots,libliftoff,vkroots,libdisplay-info ${GS_MESON_ARGS:-}
echo "SETUP $?"
ninja $NJ -C build
echo "EXIT $?"
