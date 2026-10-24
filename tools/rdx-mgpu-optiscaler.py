#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""OptiScaler for Steam (Proton) games, one game at a time, for rdx-hostd (docs/MULTI-GPU.md).

Runs as the desktop user (the owner of the Steam library). Prints one JSON object on stdout.

  rdx-mgpu-optiscaler.py list                         Steam games: exe dir, upscaler DLLs, OptiScaler state
  rdx-mgpu-optiscaler.py fetch [--version TAG]        download + unpack a release into the cache
  rdx-mgpu-optiscaler.py install (--appid N | --dir D) [--proxy dxgi] [--fg native|fsrfg|xefg] [--version TAG]
  rdx-mgpu-optiscaler.py remove (--appid N | --dir D)
  rdx-mgpu-optiscaler.py status                       cached releases, 7z, Steam libraries

OptiScaler (https://github.com/optiscaler/OptiScaler, GPL-3.0) is downloaded from its GitHub
releases, checked against the sha256 the GitHub API publishes, unpacked with 7z (its archives use
the BCJ2 filter, which Python's own modules cannot read) and copied next to the game's exe. The
official setup_linux.sh does only `mv OptiScaler.dll <proxy>` and writes a remover that leaves
the game's own overwritten DLLs broken; this tool keeps a manifest and a backup of every file it
replaces (<exe dir>/.rdx-optiscaler/), so `remove` restores the game exactly.
The game then needs WINEDLLOVERRIDES=<proxy>=n,b, which rdx-hostd adds to the app's environment
(app option "optiscaler"); without the DLL in the game folder the override changes nothing.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

REPO = "optiscaler/OptiScaler"
PROXIES = ("dxgi", "winmm", "version", "dbghelp", "d3d12", "wininet", "winhttp")
UPSCALER_DLLS = {
    "dlss": re.compile(r"^nvngx_dlss\.dll$", re.I),
    "dlssg": re.compile(r"^nvngx_dlssg\.dll$", re.I),
    "xess": re.compile(r"^libxess(_dx11)?\.dll$", re.I),
    "fsr": re.compile(r"^(amd_fidelityfx_.*|ffx_fsr[23].*)\.dll$", re.I),
}
SKIP_DIRS = re.compile(r"(redist|_commonredist|directx|vcredist|support|installer|easyanticheat|battleye|crashreport)", re.I)
MANIFEST_DIR = ".rdx-optiscaler"


def cache_root():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "rdma-display", "optiscaler")


def out(obj, code=0):
    print(json.dumps(obj))
    sys.exit(code)


def fail(msg):
    out({"ok": False, "error": msg}, 1)


# ---- Steam library ----------------------------------------------------------------------------

def steam_roots():
    home = os.path.expanduser("~")
    cands = [os.path.join(home, ".local/share/Steam"), os.path.join(home, ".steam/steam"),
             os.path.join(home, ".steam/root"), os.path.join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam")]
    seen, roots = set(), []
    for c in cands:
        r = os.path.realpath(c)
        if os.path.isdir(os.path.join(r, "steamapps")) and r not in seen:
            seen.add(r)
            roots.append(r)
    return roots


def libraries():
    libs, seen = [], set()
    for root in steam_roots():
        for p in [root] + vdf_paths(os.path.join(root, "steamapps", "libraryfolders.vdf")):
            r = os.path.realpath(p)
            if r not in seen and os.path.isdir(os.path.join(r, "steamapps")):
                seen.add(r)
                libs.append(r)
    return libs


def vdf_paths(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    return [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', text)]


def acf_fields(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return {}
    return dict(re.findall(r'^\s*"(appid|name|installdir)"\s+"([^"]*)"', text, re.M))


def games():
    res = []
    for lib in libraries():
        sa = os.path.join(lib, "steamapps")
        for f in sorted(os.listdir(sa)):
            if not (f.startswith("appmanifest_") and f.endswith(".acf")):
                continue
            a = acf_fields(os.path.join(sa, f))
            if not a.get("installdir"):
                continue
            d = os.path.join(sa, "common", a["installdir"])
            if not os.path.isdir(d):
                continue
            name = a.get("name", a["installdir"])
            if re.search(r"^(Proton|Steam Linux Runtime|Steamworks Common)", name):
                continue
            res.append({"appid": a.get("appid", ""), "name": name, "dir": d})
    return res


def exe_dir(game_dir):
    """the folder of the game's main exe: where OptiScaler must go (UE: <Project>/Binaries/Win64)"""
    best, best_score = None, None
    base_depth = game_dir.rstrip("/").count("/")
    for root, dirs, files in os.walk(game_dir):
        if root.count("/") - base_depth >= 6:
            dirs[:] = []
        dirs[:] = [d for d in dirs if not SKIP_DIRS.search(d) and d != MANIFEST_DIR]
        exes = [f for f in files if f.lower().endswith(".exe") and not SKIP_DIRS.search(f)]
        if not exes:
            continue
        score = 0.0
        low = {f.lower() for f in files}
        if any(rx.match(f) for rx in UPSCALER_DLLS.values() for f in low):
            score += 100
        rel = os.path.relpath(root, game_dir).replace("\\", "/")
        if re.search(r"binaries/win64$", rel, re.I) or re.search(r"binaries/wingdk$", rel, re.I):
            score += 50
        if re.match(r"engine/", rel, re.I):
            score -= 60
        size = max(os.path.getsize(os.path.join(root, f)) for f in exes)
        score += size / 1e8 - rel.count("/") * 0.1
        if best_score is None or score > best_score:
            best, best_score = root, score
    return best


def upscalers(d):
    found = set()
    if d and os.path.isdir(d):
        for f in os.listdir(d):
            for k, rx in UPSCALER_DLLS.items():
                if rx.match(f):
                    found.add(k)
    return sorted(found)


def manifest(d):
    try:
        return json.load(open(os.path.join(d, MANIFEST_DIR, "manifest.json")))
    except (OSError, ValueError):
        return None


def resolve_dir(args):
    if args.dir:
        d = args.dir
        return d if os.path.basename(d.rstrip("/")) and any(f.lower().endswith(".exe") for f in os.listdir(d)) \
            else exe_dir(d)
    for g in games():
        if g["appid"] == str(args.appid):
            return exe_dir(g["dir"])
    fail(f"Steam app {args.appid} is not installed")


# ---- releases ---------------------------------------------------------------------------------

def sevenzip():
    for b in ("7z", "7zz", "7za", "7zr"):
        p = shutil.which(b)
        if p:
            return p
    return None


def api(url):
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "User-Agent": "rdma-display"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def cached_versions():
    root = cache_root()
    if not os.path.isdir(root):
        return []
    return sorted(v for v in os.listdir(root) if os.path.isfile(os.path.join(root, v, "OptiScaler.dll")))


def fetch(version=None):
    """the unpacked release folder (downloads it once)"""
    if version:
        d = os.path.join(cache_root(), version)
        if os.path.isfile(os.path.join(d, "OptiScaler.dll")):
            return version, d
    rel = api(f"https://api.github.com/repos/{REPO}/releases/" + (f"tags/{version}" if version else "latest"))
    tag = rel["tag_name"]
    d = os.path.join(cache_root(), tag)
    if os.path.isfile(os.path.join(d, "OptiScaler.dll")):
        return tag, d
    assets = [a for a in rel.get("assets", []) if a["name"].endswith(".7z")]
    if not assets:
        fail(f"release {tag} has no .7z asset")
    a = assets[0]
    z = sevenzip()
    if not z:
        fail("7z is missing (Ubuntu: apt install 7zip; OptiScaler's archives need it)")
    os.makedirs(cache_root(), exist_ok=True)
    tmp = tempfile.mkdtemp(dir=cache_root(), prefix=".dl-")
    try:
        arc = os.path.join(tmp, a["name"])
        h = hashlib.sha256()
        with urllib.request.urlopen(urllib.request.Request(a["browser_download_url"], headers={"User-Agent": "rdma-display"}),
                                    timeout=120) as r, open(arc, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                h.update(b)
                f.write(b)
        want = (a.get("digest") or "").removeprefix("sha256:")
        if want and want.lower() != h.hexdigest():
            fail(f"{a['name']}: sha256 {h.hexdigest()} does not match the release's {want}")
        x = os.path.join(tmp, "x")
        subprocess.run([z, "x", "-y", f"-o{x}", arc], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if not os.path.isfile(os.path.join(x, "OptiScaler.dll")):
            fail(f"{a['name']}: no OptiScaler.dll inside")
        os.replace(x, d)
        with open(os.path.join(d, ".rdx-source.json"), "w") as f:
            json.dump({"tag": tag, "asset": a["name"], "sha256": h.hexdigest(), "verified": bool(want)}, f)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return tag, d


# ---- install / remove -------------------------------------------------------------------------

def set_ini(path, section, key, value):
    """set an existing key only (keys differ between releases); True if it was there"""
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    except OSError:
        return False
    cur, done = None, False
    for i, ln in enumerate(lines):
        m = re.match(r"^\s*\[([^\]]+)\]", ln)
        if m:
            cur = m.group(1).strip()
            continue
        if cur == section and re.match(rf"^\s*{re.escape(key)}\s*=", ln):
            lines[i] = f"{key}={value}"
            done = True
    if done:
        open(path, "w", encoding="utf-8").write("\n".join(lines))
    return done


def install(d, tag, src, proxy, fg):
    if manifest(d):
        remove(d)
    mdir = os.path.join(d, MANIFEST_DIR)
    bdir = os.path.join(mdir, "backup")
    os.makedirs(bdir, exist_ok=True)
    added, backed = [], []
    skip = {"setup_linux.sh", "setup_windows.bat", ".rdx-source.json"}
    for root, dirs, files in os.walk(src):
        rel_root = os.path.relpath(root, src)
        for f in files:
            if f in skip or f.startswith("!!"):
                continue
            rel = os.path.normpath(os.path.join(rel_root, f))
            target_rel = f"{proxy}.dll" if rel == "OptiScaler.dll" else rel
            dst = os.path.join(d, target_rel)
            if os.path.lexists(dst):
                bk = os.path.join(bdir, target_rel)
                os.makedirs(os.path.dirname(bk), exist_ok=True)
                shutil.move(dst, bk)
                backed.append(target_rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(os.path.join(root, f), dst)
            added.append(target_rel)
    ini = os.path.join(d, "OptiScaler.ini")
    ini_set = {}
    if fg in ("fsrfg", "xefg"):
        for k, v in (("FGInput", "upscaler"), ("FGOutput", fg)):
            ini_set[k] = set_ini(ini, "FrameGen", k, v)
    m = {"version": tag, "proxy": proxy, "fg": fg, "added": added, "backed_up": backed, "ini": ini_set,
         "winedlloverrides": f"{proxy}=n,b"}
    with open(os.path.join(mdir, "manifest.json"), "w") as f:
        json.dump(m, f, indent=1)
    return m


def remove(d):
    m = manifest(d)
    if not m:
        return False
    mdir = os.path.join(d, MANIFEST_DIR)
    for rel in m.get("added", []):
        p = os.path.join(d, rel)
        if os.path.lexists(p):
            os.remove(p)
    for rel in m.get("backed_up", []):
        bk = os.path.join(mdir, "backup", rel)
        if os.path.lexists(bk):
            os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
            shutil.move(bk, os.path.join(d, rel))
    for extra in ("OptiScaler.log", "fakenvapi.log", "dlssg_to_fsr3.log"):
        p = os.path.join(d, extra)
        if os.path.exists(p):
            os.remove(p)
    for sub in ("D3D12_Optiscaler", "DlssOverrides", "Licenses"):
        p = os.path.join(d, sub)
        if os.path.isdir(p) and not os.listdir(p):
            os.rmdir(p)
    shutil.rmtree(mdir, ignore_errors=True)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("list", "fetch", "install", "remove", "status"))
    ap.add_argument("--appid")
    ap.add_argument("--dir")
    ap.add_argument("--proxy", default="dxgi", choices=PROXIES)
    ap.add_argument("--fg", default="native", choices=("native", "fsrfg", "xefg"))
    ap.add_argument("--version")
    a = ap.parse_args()
    try:
        if a.cmd == "list":
            res = []
            for g in games():
                d = exe_dir(g["dir"])
                m = manifest(d) if d else None
                res.append(dict(g, exe_dir=d or "", upscalers=upscalers(d),
                                optiscaler={"version": m["version"], "proxy": m["proxy"], "fg": m["fg"]} if m else None))
            out({"ok": True, "games": res})
        if a.cmd == "status":
            out({"ok": True, "cached": cached_versions(), "sevenzip": sevenzip() or "", "libraries": libraries(),
                 "cache": cache_root()})
        if a.cmd == "fetch":
            tag, d = fetch(a.version)
            out({"ok": True, "version": tag, "path": d})
        if not (a.appid or a.dir):
            fail("--appid or --dir")
        d = resolve_dir(a)
        if not d:
            fail("no folder with the game's exe found")
        if a.cmd == "install":
            tag, src = fetch(a.version)
            out({"ok": True, "exe_dir": d, "manifest": install(d, tag, src, a.proxy, a.fg)})
        if a.cmd == "remove":
            out({"ok": True, "exe_dir": d, "removed": remove(d)})
    except subprocess.CalledProcessError as e:
        fail(f"7z failed: {e.stderr.decode(errors='replace')[-300:]}")
    except (OSError, ValueError, KeyError) as e:
        fail(f"{type(e).__name__}: {e}")
    except urllib.error.URLError as e:
        fail(f"download: {e}")


if __name__ == "__main__":
    main()
