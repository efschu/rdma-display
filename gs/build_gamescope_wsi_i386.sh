#!/bin/bash
# Build gamescope's Vulkan WSI layer for i386 (cpu family x86): Proton's 32-bit processes
# (Battle.net.exe, older games and launchers) load the 32-bit Vulkan loader and so get the layer
# only from a 32-bit build; with it they present straight to gamescope on Wayland instead of through
# Xwayland and the NVIDIA X11 WSI (whose present path hangs on RTX 50, open-gpu-kernel-modules #880).
#
#   gs/build_gamescope_wsi_i386.sh [WORKDIR]      (default /usr/local/src/rdx-gs; needs gs/build_gamescope.sh run first)
#
# Prior art (CachyOS lib32-gamescope, Alpine's x86 gamescope): gamescope's own meson builds only the
# layer with -Denable_gamescope=false -Denable_gamescope_wsi_layer=true and an i686 cross file; the
# files are named by meson's cpu family: libVkLayer_FROG_gamescope_wsi_x86.so and
# VkLayer_FROG_gamescope_wsi.x86.json (layer VK_LAYER_FROG_gamescope_wsi_x86). Its version must be the
# 64-bit gamescope's (same tree). Result: WORKDIR/gamescope/build-i386/layer/ ;
# host/install-gamescope.sh installs it together with the x86_64 layer.
set -eu
W=${1:-/usr/local/src/rdx-gs}
NJ=${NINJA_JOBS:+-j$NINJA_JOBS}
[ -f "$W/gamescope/meson.build" ] || { echo "no gamescope tree in $W (run gs/build_gamescope.sh first)" >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture i386
apt-get -o DPkg::Lock::Timeout=900 update -q || true
apt-get -o DPkg::Lock::Timeout=900 install -y -q --no-install-recommends \
  gcc-multilib g++-multilib libvulkan-dev:i386 libwayland-dev:i386 libx11-dev:i386 libx11-xcb-dev:i386 \
  libxcb1-dev:i386 libffi-dev:i386 pkgconf libwayland-bin
# host (i386) pkg-config path: the multiarch libs plus the architecture-independent .pc files
# (wayland-protocols from the build prefix, 1.47, newer than the distribution's); wayland-scanner is native
cat > "$W/i386-cross.ini" <<EOC
[binaries]
c = ['gcc', '-m32']
cpp = ['g++', '-m32']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['/usr/lib/i386-linux-gnu/pkgconfig', '/usr/share/pkgconfig', '$W/prefix/share/pkgconfig']

[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOC
cd "$W/gamescope"
rm -rf build-i386
env -u PKG_CONFIG_PATH meson setup build-i386 --cross-file "$W/i386-cross.ini" -Dbuildtype=release \
  --libdir=lib/i386-linux-gnu -Denable_gamescope=false -Denable_gamescope_wsi_layer=true -Denable_tests=false \
  -Denable_openvr_support=false -Dpipewire=disabled -Dbenchmark=disabled -Db_lto=false \
  --force-fallback-for=libliftoff,vkroots
echo "SETUP $?"
ninja $NJ -C build-i386
echo "EXIT $?"
file build-i386/layer/libVkLayer_FROG_gamescope_wsi_x86.so
