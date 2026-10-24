#!/bin/bash
# Install the patched gamescope (gs/build_gamescope.sh builds it in a root-owned tree,
# default /usr/local/src/rdx-gs) to /opt/rdx-gs so the desktop user can run it: rdx-hostd starts gamescope
# apps as that user (Steam refuses to run as root). Rewrites the RUNPATH to the new prefix.
#   host/install-gamescope.sh [/usr/local/src/rdx-gs] [/opt/rdx-gs] [FINAL]
# FINAL (default DST) is where the tree will live: a staging install for a package (DST elsewhere,
# FINAL /opt/rdx-gs) gets runpaths and the layer manifest for FINAL and touches nothing in /usr.
#
# gamescope's WSI layer (VK_LAYER_FROG_gamescope_wsi, built by gamescope's meson next to the binary)
# goes along: prefix/lib/x86_64-linux-gnu/libVkLayer_FROG_gamescope_wsi_x86_64.so with its manifest
# prefix/share/vulkan/implicit_layer.d/rdx-gamescope-wsi.x86_64.json (plus the i386 build of it,
# libVkLayer_FROG_gamescope_wsi_x86.so with rdx-gamescope-wsi.x86.json, for 32-bit Proton processes
# such as Battle.net.exe), and a live install (FINAL = DST)
# puts the manifest into /usr/share/vulkan/implicit_layer.d (pve-seat-provision.sh update does that in
# a seat). gamescope sets ENABLE_GAMESCOPE_WSI=1 for its apps; with the layer a Vulkan app (DXVK,
# vkd3d-proton) presents straight to gamescope on Wayland instead of through Xwayland's Present, as
# every distribution's gamescope package ships it. Without it GTA V Enhanced (vkd3d-proton, RTX 5090)
# stopped presenting for 2-40 s after every mouse or key input (its vkd3d-swapchain thread spun
# in the NVIDIA driver while gamescope, Xwayland and wineserver idled). DISABLE_GAMESCOPE_WSI=1 turns
# it off for one app.
set -eu
SRC=${1:-/usr/local/src/rdx-gs}
DST=${2:-/opt/rdx-gs}
FINAL=${3:-$DST}
command -v patchelf >/dev/null || DEBIAN_FRONTEND=noninteractive apt-get install -y -q patchelf
mkdir -p "$DST/bin"
rm -rf "$DST/prefix"
cp -a "$SRC/prefix" "$DST/prefix"
for b in gamescope gamescopereaper gamescopectl gamescopestream; do
    [ -x "$SRC/gamescope/build/src/$b" ] && install -m 0755 "$SRC/gamescope/build/src/$b" "$DST/bin/$b"
done
# Xwayland 24.1 with explicit sync (gs/build_gamescope.sh): next to gamescope, where the rdx
# backend finds it (WLR_XWAYLAND); without it X11 apps on NVIDIA flicker black/white
[ -x "$SRC/xwayland/bin/Xwayland" ] && install -m 0755 "$SRC/xwayland/bin/Xwayland" "$DST/bin/Xwayland"
LIB=$(dirname "$(find "$DST/prefix/lib" -name 'libwayland-server.so.0' | head -1)")
LIB=${LIB/#$DST/$FINAL}
for f in "$DST"/bin/*; do patchelf --set-rpath "$LIB" "$f"; done
find "$DST/prefix/lib" -name '*.so*' -type f | while read -r f; do
    r=$(patchelf --print-rpath "$f" 2>/dev/null || true)
    case "$r" in *"$SRC"*) patchelf --set-rpath "${r//$SRC/$FINAL}" "$f" ;; esac
done
# the WSI layer: no runpath (it is loaded into the app, also inside Steam's pressure-vessel runtime,
# and takes that process's libwayland-client / libxcb), manifest pointing at FINAL
LAYER=libVkLayer_FROG_gamescope_wsi_x86_64.so
MANIFEST=rdx-gamescope-wsi.x86_64.json
if [ -f "$SRC/gamescope/build/layer/$LAYER" ]; then
    LD=$DST/prefix/lib/x86_64-linux-gnu
    install -D -m 0755 "$SRC/gamescope/build/layer/$LAYER" "$LD/$LAYER"
    patchelf --remove-rpath "$LD/$LAYER"
    mkdir -p "$DST/prefix/share/vulkan/implicit_layer.d"
    sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$FINAL/prefix/lib/x86_64-linux-gnu/$LAYER\"|" \
        "$SRC/gamescope/build/layer/VkLayer_FROG_gamescope_wsi.x86_64.json" \
        > "$DST/prefix/share/vulkan/implicit_layer.d/$MANIFEST"
    if [ "$FINAL" = "$DST" ]; then
        install -D -m 0644 "$DST/prefix/share/vulkan/implicit_layer.d/$MANIFEST" "/usr/share/vulkan/implicit_layer.d/$MANIFEST"
    fi
    echo "WSI layer: $FINAL/prefix/lib/x86_64-linux-gnu/$LAYER"
else
    echo "WSI layer: none in $SRC/gamescope/build/layer (gamescope built without it?)" >&2
    if [ "$FINAL" = "$DST" ]; then rm -f "/usr/share/vulkan/implicit_layer.d/$MANIFEST"; fi
fi
# the 32-bit build of the layer (gs/build_gamescope_wsi_i386.sh builds it into gamescope/build-i386;
# meson names it with cpu family x86): Proton's 32-bit processes (Battle.net.exe, older games) load
# the i386 Vulkan loader and take this one. Same rules as above: no runpath, manifest at FINAL.
LAYER32=libVkLayer_FROG_gamescope_wsi_x86.so
MANIFEST32=rdx-gamescope-wsi.x86.json
B32=$SRC/gamescope/build-i386/layer
if [ -f "$B32/$LAYER32" ]; then
    LD32=$DST/prefix/lib/i386-linux-gnu
    install -D -m 0755 "$B32/$LAYER32" "$LD32/$LAYER32"
    patchelf --remove-rpath "$LD32/$LAYER32"
    mkdir -p "$DST/prefix/share/vulkan/implicit_layer.d"
    sed "s|\"library_path\": *\"[^\"]*\"|\"library_path\": \"$FINAL/prefix/lib/i386-linux-gnu/$LAYER32\"|" \
        "$B32/VkLayer_FROG_gamescope_wsi.x86.json" \
        > "$DST/prefix/share/vulkan/implicit_layer.d/$MANIFEST32"
    if [ "$FINAL" = "$DST" ]; then
        install -D -m 0644 "$DST/prefix/share/vulkan/implicit_layer.d/$MANIFEST32" "/usr/share/vulkan/implicit_layer.d/$MANIFEST32"
    fi
    echo "WSI layer (32-bit): $FINAL/prefix/lib/i386-linux-gnu/$LAYER32"
else
    echo "WSI layer (32-bit): none in $B32 (gs/build_gamescope_wsi_i386.sh not run; 32-bit Proton processes present through Xwayland)" >&2
    if [ "$FINAL" = "$DST" ]; then rm -f "/usr/share/vulkan/implicit_layer.d/$MANIFEST32"; fi
fi
chmod -R a+rX "$DST"
echo "installed $DST/bin/gamescope (runpath $LIB)"
