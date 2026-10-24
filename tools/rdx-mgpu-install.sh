#!/bin/bash
# SPDX-License-Identifier: MIT
# Frame-generation layers for gamescope apps (docs/MULTI-GPU.md), installed for the desktop user
# the way their projects document it (implicit Vulkan layers under ~/.local, dormant until the
# app's environment enables them; Steam's runtime container sees ~/.local, not /opt):
#
#   rdx-mgpu-install.sh status [USER]          JSON: what is installed, Lossless.dll found, 7z
#   rdx-mgpu-install.sh afmf USER              afmf-linux (MIT): build the pinned commit, install
#   rdx-mgpu-install.sh lsfg USER [VERSION]    lsfg-vk prebuilt (CC BY-NC-ND 4.0, private use) from
#                                              builds.lsfg-vk.dev, default the latest release
#   rdx-mgpu-install.sh 7z                     the 7z binary OptiScaler's archives need (apt)
#   rdx-mgpu-install.sh remove afmf|lsfg USER
#
# Runs as root (rdx-hostd); files under the user's home belong to the user. Build dependencies
# for afmf: cmake ninja-build gcc glslang-tools libvulkan-dev (installed with apt if missing).
set -euo pipefail
AFMF_REPO=https://github.com/serialexperimentslainnnn/afmf-linux
AFMF_COMMIT=bc56e5bd0e3a6c328aedef664d0d36363fb58b36
LSFG_BASE=https://builds.lsfg-vk.dev

user_home() { getent passwd "$1" | cut -d: -f6; }
as_user() { local u=$1; shift; runuser -u "$u" -- "$@"; }
json_str() { python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1"; }

lossless_dll() {
    local h; h=$(user_home "$1")
    for root in "$h/.local/share/Steam" "$h/.steam/steam" "$h/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -d "$root/steamapps" ] || continue
        for lib in "$root" $(grep -oP '"path"\s+"\K[^"]+' "$root/steamapps/libraryfolders.vdf" 2>/dev/null); do
            f="$lib/steamapps/common/Lossless Scaling/Lossless.dll"
            [ -f "$f" ] && { echo "$f"; return 0; }
        done
    done
    [ -f "$h/.local/share/lsfg-vk/Lossless.dll" ] && { echo "$h/.local/share/lsfg-vk/Lossless.dll"; return 0; }
    return 1
}

status() {
    local u=${1:-}; local h="" afmf="" lsfg="" dll=""
    if [ -n "$u" ]; then
        h=$(user_home "$u")
        [ -f "$h/.local/share/vulkan/implicit_layer.d/afmf-linux.json" ] && afmf="$h/.local/share/vulkan/implicit_layer.d/afmf-linux.json"
        lsfg=$(ls "$h"/.local/share/vulkan/implicit_layer.d/*lsfg* 2>/dev/null | head -1 || true)
        dll=$(lossless_dll "$u" || true)
    fi
    local nvsm=""; nvsm=$(grep -l '"VK_LAYER_NV_present"' /usr/share/vulkan/implicit_layer.d/*.json /etc/vulkan/implicit_layer.d/*.json 2>/dev/null | head -1 || true)
    local z=""; z=$(command -v 7z || command -v 7zz || command -v 7za || true)
    printf '{"ok":true,"user":%s,"afmf":%s,"lsfg":%s,"lossless_dll":%s,"nv_smooth_motion_layer":%s,"sevenzip":%s}\n' \
        "$(json_str "$u")" "$(json_str "$afmf")" "$(json_str "$lsfg")" "$(json_str "$dll")" "$(json_str "$nvsm")" "$(json_str "$z")"
}

apt_need() {
    local miss=()
    for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || miss+=("$p"); done
    [ ${#miss[@]} -eq 0 ] || DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${miss[@]}" >&2
}

install_afmf() {
    local u=$1 h; h=$(user_home "$u")
    apt_need git cmake ninja-build gcc glslang-tools libvulkan-dev
    local src=/var/cache/rdma-display/afmf-linux
    mkdir -p /var/cache/rdma-display
    [ -d "$src/.git" ] || git clone -q "$AFMF_REPO" "$src" >&2
    git -C "$src" fetch -q origin >&2 || true
    git -C "$src" checkout -q "$AFMF_COMMIT" >&2
    # memory-bounded: a build on a desktop must not push the user's session into the OOM killer
    systemd-run --scope -q -p MemoryMax=2G -p MemoryHigh=1500M -- \
        bash -c "cmake -S '$src' -B '$src/build' -G Ninja -DCMAKE_BUILD_TYPE=Release -DAFMF_WERROR=OFF \
                 -DCMAKE_INSTALL_PREFIX='$h/.local' >/dev/null && cmake --build '$src/build' -j2 >/dev/null" >&2
    cmake --install "$src/build" >/dev/null
    chown -R "$u:" "$h/.local/lib/libafmf-linux.so" "$h/.local/share/vulkan"
    status "$u"
}

install_lsfg() {
    local u=$1 ver=${2:-} h; h=$(user_home "$u")
    if [ -z "$ver" ]; then
        ver=$(curl -fsSL "$LSFG_BASE/" | grep -oP 'Latest release[^<]*<[^>]*href="\Klsfg-vk-[^"]+\.tar\.xz' | head -1 || true)
        [ -n "$ver" ] || ver=$(curl -fsSL "$LSFG_BASE/" | grep -oP 'lsfg-vk-[0-9.]+\.tar\.xz' | sort -V | tail -1)
    else
        ver="lsfg-vk-$ver.tar.xz"
    fi
    [ -n "$ver" ] || { echo '{"ok":false,"error":"no lsfg-vk build found"}'; exit 1; }
    local t; t=$(mktemp -d)
    curl -fsSL -o "$t/$ver" "$LSFG_BASE/$ver"
    tar -tJf "$t/$ver" > "$t/files"
    as_user "$u" mkdir -p "$h/.local"
    as_user "$u" tar -xJf "$t/$ver" -C "$h/.local"
    mkdir -p "$h/.local/share/rdma-display"
    sed "s|^|$h/.local/|" "$t/files" > "$h/.local/share/rdma-display/lsfg-vk.files"
    chown -R "$u:" "$h/.local/share/rdma-display"
    rm -rf "$t"
    status "$u"
}

remove_comp() {
    local c=$1 u=$2 h; h=$(user_home "$u")
    case "$c" in
    afmf) rm -f "$h/.local/lib/libafmf-linux.so" "$h/.local/share/vulkan/implicit_layer.d/afmf-linux.json" ;;
    lsfg) f="$h/.local/share/rdma-display/lsfg-vk.files"
          [ -f "$f" ] && while read -r p; do [ -f "$p" ] && rm -f "$p"; done < "$f"; rm -f "$f" ;;
    *) echo '{"ok":false,"error":"remove afmf|lsfg USER"}'; exit 2 ;;
    esac
    status "$u"
}

case "${1:-}" in
status) status "${2:-}" ;;
afmf) [ -n "${2:-}" ] || { echo '{"ok":false,"error":"afmf USER"}'; exit 2; }; install_afmf "$2" ;;
lsfg) [ -n "${2:-}" ] || { echo '{"ok":false,"error":"lsfg USER [VERSION]"}'; exit 2; }; install_lsfg "$2" "${3:-}" ;;
7z) apt_need 7zip; status "" ;;
remove) remove_comp "${2:-}" "${3:-}" ;;
*) sed -n '2,16p' "$0"; exit 2 ;;
esac
