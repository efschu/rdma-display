#!/usr/bin/python3
"""rdx-seat-gpu: a seat container's GPU follows its PCI address, at every start.

Runs as root on the Proxmox host, in three roles (docs/MULTISEAT.md "GPU binding"):

  PVE hookscript   "hookscript: local:snippets/rdx-seat-gpu" (a link to this file) in the
                   seat's /etc/pve/lxc/<ct>.conf. pre-start: the GPU of the seat ("gpu" of its entry
                   in /etc/rdx/seatmgr.json) is looked up on the host NOW and its device lines are
                   written to /run/rdx-seat/<ct>.gpu.conf, which the config includes
                   ("lxc.include: /run/rdx-seat/<ct>.gpu.conf"). Proxmox writes /var/lib/lxc/<ct>/config
                   before the hookscript and lxc-start reads it, includes and all, after it, so every
                   `pct start` binds the numbers the host has at that moment. Any doubt (list below):
                   exit 1, and Proxmox does not start the container. post-stop: the file goes, so a
                   start that bypasses the hookscript (lxc-start, systemctl start pve-container@<ct>)
                   fails on the missing include instead of binding whatever was right last time.
  LXC hook         "lxc.hook.pre-start: /opt/rdx-seatmgr/rdx_seat_gpu.py" in the same config: inside
                   EVERY lxc-start (also the restart after a reboot from inside the container, which
                   runs no hookscript), after lxc has parsed the config and before the container
                   exists, the GPU device lines lxc is about to apply are compared with the host's
                   numbers for the seat's PCI address. Any difference: exit 1, lxc aborts the start.
                   post-start / post-stop: the IRQs of the seat cards follow the seats (below).
  command line     --status, --cards, --check CT, --write CT (the include for a running container, for its
                   restart after an inside reboot), --pin PCI|CT, --unpin PCI|CT, --irqs. Nothing here
                   touches a GPU.
  library          rdx_seatmgr.py imports it for its card choice ("Seat N on card X", docs/MULTISEAT.md
                   "Card choice"): the same checks, the hostd config in the container, the IRQs.

Card choice (every seat keeps its own data and logins and picks a free card at its start):
"gpu" of a seat is its current choice, "gpus" (optional) the cards it may take (default: every NVIDIA
display device of the host). Several seats may name the same card in seatmgr.json; only one RUNNING seat
can have it (refused below). rdx-seatmgr's API writes "gpu" and the seat's hostd config and starts it.

The device numbers of a GPU are read for its PCI address, never derived from the order of the cards:
NVIDIA minor from /proc/driver/nvidia/gpus/<pci>/information ("Device Minor"), DRM card and render
node from /sys/bus/pci/devices/<pci>/drm. They move with the hardware (seen: a second 3080 went in,
the 5090's card0 became card1 and the first 3080's card1 card0, so seat 1's static lines named seat 2's
card) and the majors of nvidia-uvm and nvidia-caps move with every driver load (seen: 505/510 -> 502/508).
Inside the container the nodes keep the HOST's kernel names (/dev/dri/card1, not card0 or a by-path
name): sysfs in the container is the host's, and wlroots/gamescope open the devnode udev names there.

Refused (start or check), with the reason on stderr; by design a refused start is better than a wrong card:
  - the container has no seat in seatmgr.json, or two seats name the same container; the GPU is not one of
    the seat's "gpus"; another RUNNING seat has the same "gpu";
  - the PCI address is no NVIDIA display device bound to the nvidia driver, its procfs entry or its
    DRM nodes are missing or not exactly one card and one render node, a host node (/dev/nvidiaN,
    /dev/dri/cardN, renderD) has other numbers than sysfs and procfs say, /sys/dev/char/226:N points
    to another PCI device, or /dev/dri/by-path/pci-<pci>-card|render points to another node;
  - the GPU's UUID differs from the one pinned for its PCI address (/etc/rdx/seat-gpu.pins.json,
    {"cards": {pci: {uuid, model}}}, written at the first good start on that card): a card added or taken
    out renumbers the PCI bus, and an address may then be another card. Pins are per card, not per seat,
    so a seat that switches cards is no "swap"; a real one is. A card swapped on purpose: --unpin PCI
    (or --pin PCI). Pins of the per-seat form of before are taken over for their card's address;
  - another RUNNING seat's container has one of the GPU's nodes in its effective config, or any
    seat's config (this one included) allows a GPU node of another card, a wildcard (c 195:*,
    c 226:*, a) or a whole dynamic major (>= 234) that is neither nvidia-uvm nor nvidia-caps now;
  - the seat's hostd config in its root file system (/etc/rdx/hostd-seatN.json, "gpu") names
    another PCI address (pre-start only; a missing file is only noted).
A seat whose "gpu" is "" starts without any GPU node (and without nvidia-uvm/nvidia-caps).

IRQs: the seats run on their own CPUs (lxc.cgroup2.cpuset.cpus of the container), the host and its
other workloads on the others. Every PCI function of a seat card (the GPU and its HDMI
sound .1) gets its interrupts on the CPUs of the RUNNING seat that has it, and back on the host's
(/proc/irq/default_smp_affinity) once no running seat has it: at the hookscript's post-start and
post-stop, and by rdx-seatmgr at every container start and stop it sees. A seat without a cpuset leaves
its card's IRQs where they are (noted); an IRQ the kernel will not move (managed) is noted.
A container that is no seat but runs with the same GPU allowed (e.g. a compute container) is only
named: sharing a GPU with it is a decision of its own config, not this hook's.
"""

import io
import json
import os
import re
import stat
import subprocess
import sys
import time

SEATMGR_CONF = "/etc/rdx/seatmgr.json"
PINS = "/etc/rdx/seat-gpu.pins.json"
RUN_DIR = "/run/rdx-seat"
NV_MAJOR, DRM_MAJOR = 195, 226
NV_FIXED = (254, 255)                     # nvidia-modeset, nvidiactl: every seat's, no GPU of its own
NV_DYNAMIC = ("nvidia-uvm", "nvidia-caps")
DYNAMIC_MAJOR_MIN = 234                   # char majors 234-254 and 384-511 are handed out at load
PCI_RE = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")
ALLOW_KEYS = ("lxc.cgroup2.devices.allow", "lxc.cgroup.devices.allow")


class Refuse(Exception):
    """The GPU of a seat cannot be bound for certain: the start must not happen."""


class Paths:
    """Roots of everything read or written; the tests point them into a temp tree."""

    def __init__(self, sys_="/sys", dev="/dev", proc="/proc", pve="/etc/pve/lxc", lxc="/var/lib/lxc",
                 run=RUN_DIR, conf=SEATMGR_CONF, pins=PINS):
        self.sys, self.dev, self.proc, self.pve, self.lxc = sys_, dev, proc, pve, lxc
        self.run, self.conf, self.pins = run, conf, pins
        self.stat = os.stat
        self.rootfs = rootfs_path               # ct -> host path of its root file system, or None
        self.syslog = to_syslog                 # refusals and binds also go to the journal (-t rdx-seat-gpu):
                                                # lxc keeps a hook's output out of sight of `pct start`


def to_syslog(msg, err=False):
    try:
        import syslog
        syslog.openlog("rdx-seat-gpu")
        syslog.syslog(syslog.LOG_ERR if err else syslog.LOG_INFO, msg)
    except Exception:                           # the journal is no reason to refuse or allow a start
        pass


def rd(path, default=""):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return default


def norm_pci(s):
    s = (s or "").strip().lower()
    if re.fullmatch(r"[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", s):
        s = "0000:" + s
    return s


# ------------------------------------------------------------------------------------- the seats
def load_seats(P):
    """{ct: {"seat": N, "gpu": pci or "", "gpus": [pci] or None, "name"}} from seatmgr.json; a container
    named by two seats is refused here already (for every seat, not only the two). Two seats may name the
    same card ("gpu" is the last choice): only one of them may RUN with it (decide())."""
    try:
        with open(P.conf) as fh:
            conf = json.load(fh)
    except (OSError, ValueError) as e:
        raise Refuse("cannot read %s: %s" % (P.conf, e))
    out = {}
    for n, s in (conf.get("seats") or {}).items():
        try:
            ct = int(s["ct"])
        except (KeyError, TypeError, ValueError):
            raise Refuse("%s: seat %s has no container id" % (P.conf, n))
        if ct in out:
            raise Refuse("%s: CT %d is seat %s and seat %s" % (P.conf, ct, out[ct]["seat"], n))
        gpu = norm_pci(s.get("gpu", ""))
        if gpu and not PCI_RE.fullmatch(gpu):
            raise Refuse("%s: seat %s: \"gpu\": %r is no PCI address" % (P.conf, n, s.get("gpu")))
        gpus = s.get("gpus")
        if gpus is not None:
            if not isinstance(gpus, list) or not all(isinstance(g, str) and PCI_RE.fullmatch(norm_pci(g)) for g in gpus):
                raise Refuse("%s: seat %s: \"gpus\" must be a list of PCI addresses" % (P.conf, n))
            gpus = [norm_pci(g) for g in gpus]
        out[ct] = {"seat": str(n), "gpu": gpu, "gpus": gpus, "name": s.get("name", "")}
    return out


def nvidia_cards(P):
    """Every NVIDIA display device of the host (vendor 0x10de, class 0x03xxxx), by PCI address."""
    base = os.path.join(P.sys, "bus/pci/devices")
    out = []
    for n in sorted(os.listdir(base)) if os.path.isdir(base) else ():
        d = os.path.join(base, n)
        if PCI_RE.fullmatch(n) and rd(os.path.join(d, "vendor")) == "0x10de" and rd(os.path.join(d, "class")).startswith("0x03"):
            out.append(n)
    return out


def allowed_cards(P, seat):
    """The cards a seat may take: its "gpus", else every NVIDIA display device of the host."""
    return list(seat["gpus"]) if seat.get("gpus") is not None else nvidia_cards(P)


def seat_of(P, ct, seats=None):
    seats = load_seats(P) if seats is None else seats
    if int(ct) not in seats:
        raise Refuse("CT %s is no seat in %s (the hook binds a GPU only to a seat)" % (ct, P.conf))
    return seats[int(ct)]


# --------------------------------------------------------------------------------- the host's GPU
def char_rdev(P, path):
    """(major, minor) of a character device node, None if it is none."""
    try:
        st = P.stat(path)
    except OSError:
        return None
    return (os.major(st.st_rdev), os.minor(st.st_rdev)) if stat.S_ISCHR(st.st_mode) else None


def majmin(text):
    m = re.fullmatch(r"(\d+):(\d+)", text or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


def resolve(P, pci):
    """The device nodes of the NVIDIA GPU at `pci`, each number read for that address and checked
    against the host's nodes from the other side; Refuse at the first doubt."""
    pci = norm_pci(pci)
    if not PCI_RE.fullmatch(pci):
        raise Refuse("%r is no PCI address" % pci)
    d = os.path.join(P.sys, "bus/pci/devices", pci)
    if not os.path.isdir(d):
        raise Refuse("no PCI device %s on this host" % pci)
    if rd(os.path.join(d, "vendor")) != "0x10de" or not rd(os.path.join(d, "class")).startswith("0x03"):
        raise Refuse("%s is no NVIDIA display device (vendor %s class %s)"
                     % (pci, rd(os.path.join(d, "vendor"), "?"), rd(os.path.join(d, "class"), "?")))
    drv = os.path.basename(os.path.realpath(os.path.join(d, "driver"))) if os.path.exists(os.path.join(d, "driver")) else ""
    if drv != "nvidia":
        raise Refuse("%s is bound to %s, not to nvidia" % (pci, drv or "no driver"))
    info = {}
    for line in rd(os.path.join(P.proc, "driver/nvidia/gpus", pci, "information")).splitlines():
        k, _, v = line.partition(":")
        info[k.strip()] = v.strip()
    if not info.get("Device Minor", "").isdigit():
        raise Refuse("%s: no Device Minor in /proc/driver/nvidia/gpus/%s/information" % (pci, pci))
    minor = int(info["Device Minor"])
    if minor in NV_FIXED or minor > 253:
        raise Refuse("%s: Device Minor %d is no GPU minor" % (pci, minor))
    if info.get("Bus Location", pci).lower() != pci:
        raise Refuse("%s: procfs says Bus Location %s" % (pci, info.get("Bus Location")))
    gpu = {"pci": pci, "minor": minor, "uuid": info.get("GPU UUID", ""), "model": info.get("Model", "")}
    nv = os.path.join(P.dev, "nvidia%d" % minor)
    if char_rdev(P, nv) != (NV_MAJOR, minor):
        raise Refuse("%s: host node /dev/nvidia%d is %s, not c %d:%d"
                     % (pci, minor, char_rdev(P, nv) or "missing", NV_MAJOR, minor))
    drm = os.path.join(d, "drm")
    names = sorted(os.listdir(drm)) if os.path.isdir(drm) else []
    for kind in ("card", "renderD"):
        found = [n for n in names if re.fullmatch(kind + r"\d+", n)]
        if len(found) != 1:
            raise Refuse("%s: %d DRM %s nodes in sysfs (%s), want exactly one"
                         % (pci, len(found), kind, " ".join(found) or "none"))
        name = found[0]
        mm = majmin(rd(os.path.join(drm, name, "dev")))
        if mm is None or mm[0] != DRM_MAJOR or mm[1] != int(name[len(kind):]):
            raise Refuse("%s: sysfs %s has dev %s" % (pci, name, rd(os.path.join(drm, name, "dev"), "none")))
        back = os.path.basename(os.path.realpath(os.path.join(P.sys, "dev/char/%d:%d/device" % mm)))
        if back != pci:
            raise Refuse("%s: /sys/dev/char/%d:%d belongs to %s" % (pci, mm[0], mm[1], back or "nothing"))
        host = char_rdev(P, os.path.join(P.dev, "dri", name))
        if host != mm:
            raise Refuse("%s: host node /dev/dri/%s is %s, sysfs says c %d:%d"
                         % (pci, name, "c %d:%d" % host if host else "missing", mm[0], mm[1]))
        bp = os.path.join(P.dev, "dri/by-path", "pci-%s-%s" % (pci, "card" if kind == "card" else "render"))
        if os.path.lexists(bp) and os.path.basename(os.path.realpath(bp)) != name:
            raise Refuse("%s: %s points to %s, sysfs says %s"
                         % (pci, bp[len(P.dev):], os.path.basename(os.path.realpath(bp)), name))
        gpu["card" if kind == "card" else "render"] = name
        gpu[("card" if kind == "card" else "render") + "_minor"] = mm[1]
    return gpu


def nvidia_majors(P):
    """{name: major} of nvidia-uvm and nvidia-caps as /proc/devices has them now (char section)."""
    out = {}
    for line in rd(os.path.join(P.proc, "devices")).splitlines():
        if line.startswith("Block"):
            break
        f = line.split()
        if len(f) == 2 and f[0].isdigit() and f[1] in NV_DYNAMIC:
            out[f[1]] = int(f[0])
    return out


# -------------------------------------------------------------------------- what a config allows
def parse_lines(text, pve=False):
    """(key, value) of an lxc config ("key = value") or a Proxmox one ("key: value"); a Proxmox
    config's snapshot sections ([name]) are not the current config."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if pve and line.startswith("["):
            break
        if not line or line.startswith("#"):
            continue
        m = re.match(r"([A-Za-z0-9_.\-]+)\s*(?:=|:)\s*(.*)$", line)
        if m:
            out.append((m.group(1), m.group(2).strip()))
    return out


def effective_lines(P, path, pve=False):
    """The lines of a config and of the files it includes, except lxc's own (/usr/share/lxc), which
    allow no GPU node; an include that does not exist is a line of its own ("missing")."""
    out = []
    for k, v in parse_lines(rd(path), pve):
        out += included_lines(k, v)
    return out


def included_lines(k, v):
    """One config line, an include replaced by its lines (one level: ours include nothing)."""
    if k != "lxc.include":
        return [(k, v)]
    if v.startswith("/usr/share/lxc/"):
        return []
    if not os.path.isfile(v):
        return [("rdx.missing-include", v)]
    return [(k2, v2) for k2, v2 in parse_lines(rd(v)) if k2 != "lxc.include"]


def ct_running(P, ct):
    return os.path.isdir(os.path.join(P.sys, "fs/cgroup/lxc", str(ct)))


def running_config(P, ct):
    """What a RUNNING container has: lxc's config (Proxmox writes it before every start and again at
    every `pct set`, so a seat with this hook reads its include there), else the Proxmox config."""
    p = os.path.join(P.lxc, str(ct), "config")
    return effective_lines(P, p) if os.path.isfile(p) else effective_lines(P, os.path.join(P.pve, "%s.conf" % ct), True)


def claims(P, lines):
    """What a config gives of the GPU devices: {"nv": minors of 195 (not modeset/ctl), "drm": minors of
    226, "wild": wildcards, "whole": whole majors allowed, "mounts": {source: target} of /dev/nvidiaN and
    /dev/dri/* bind mounts, "missing": includes that do not exist}."""
    c = {"nv": set(), "drm": set(), "wild": set(), "whole": set(), "mounts": {}, "missing": []}
    for k, v in lines:
        if k in ALLOW_KEYS:
            f = v.split()
            if f and f[0] == "a":
                c["wild"].add("a")
                continue
            m = re.match(r"([cb*])\s+(\d+|\*):(\d+|\*)", v)
            if not m or m.group(1) == "b":
                continue
            ma, mi = m.group(2), m.group(3)
            if ma == "*":
                c["wild"].add("%s *:%s" % (m.group(1), mi))
            elif mi == "*":
                c["whole"].add(int(ma))
                if int(ma) in (NV_MAJOR, DRM_MAJOR):
                    c["wild"].add("c %s:*" % ma)
            elif int(ma) == NV_MAJOR and int(mi) not in NV_FIXED:
                c["nv"].add(int(mi))
            elif int(ma) == DRM_MAJOR:
                c["drm"].add(int(mi))
        elif k == "lxc.mount.entry":
            f = v.split()
            if len(f) >= 2 and (re.fullmatch(r"/dev/nvidia\d+", f[0]) or f[0].startswith("/dev/dri")):
                c["mounts"][f[0]] = f[1]
        elif re.fullmatch(r"dev\d+", k):                     # Proxmox device passthrough: devN: path[,..]
            path = v.split(",")[0].split("=", 1)[-1]
            rdev = char_rdev(P, path)
            if rdev and rdev[0] == NV_MAJOR and rdev[1] not in NV_FIXED:
                c["nv"].add(rdev[1])
            elif rdev and rdev[0] == DRM_MAJOR:
                c["drm"].add(rdev[1])
        elif k == "rdx.missing-include":
            c["missing"].append(v)
    return c


def owner(P, major, minor):
    """Whose node c major:minor is on the host now, for the messages."""
    if major == DRM_MAJOR:
        d = os.path.join(P.sys, "dev/char/%d:%d" % (major, minor))
        if os.path.exists(os.path.join(d, "device")):
            return "the node of %s (%s)" % (os.path.basename(os.path.realpath(os.path.join(d, "device"))),
                                            os.path.basename(os.path.realpath(d)))
    elif major == NV_MAJOR:
        base = os.path.join(P.proc, "driver/nvidia/gpus")
        for pci in sorted(os.listdir(base)) if os.path.isdir(base) else ():
            if re.search(r"^Device Minor:\s*%d\s*$" % minor, rd(os.path.join(base, pci, "information")), re.M):
                return "the node of %s" % pci
    return "no GPU of this host now"


def problems(P, c, gpu, majors, mine=True):
    """What is wrong with the claims `c` of a seat whose GPU is `gpu` (None: no GPU); mine=False checks
    another seat's config only for the GPU `gpu` (nodes of it there are the problem)."""
    out = []
    if not mine:
        if gpu and gpu["minor"] in c["nv"]:
            out.append("allows /dev/nvidia%d (%s)" % (gpu["minor"], gpu["pci"]))
        for n in ("card", "render"):
            if gpu and gpu[n + "_minor"] in c["drm"]:
                out.append("allows /dev/dri/%s (%s)" % (gpu[n], gpu["pci"]))
        if gpu and c["wild"]:
            out.append("allows %s" % ", ".join(sorted(c["wild"])))
        return out
    want_nv = {gpu["minor"]} if gpu else set()
    want_drm = {gpu["card_minor"], gpu["render_minor"]} if gpu else set()
    for inc in c["missing"]:
        out.append("include %s does not exist" % inc)
    if c["wild"]:
        out.append("wildcard allow %s" % ", ".join(sorted(c["wild"])))
    for mi in sorted(c["nv"] - want_nv):
        out.append("allows /dev/nvidia%d, %s" % (mi, owner(P, NV_MAJOR, mi)))
    for mi in sorted(c["drm"] - want_drm):
        out.append("allows c 226:%d, %s" % (mi, owner(P, DRM_MAJOR, mi)))
    if want_nv - c["nv"] or want_drm - c["drm"]:
        out.append("does not allow its GPU's nodes (%s)" % ", ".join(
            ["c 195:%d" % m for m in sorted(want_nv - c["nv"])] + ["c 226:%d" % m for m in sorted(want_drm - c["drm"])]))
    want_mounts = {}
    if gpu:
        want_mounts = {"/dev/nvidia%d" % gpu["minor"]: "dev/nvidia%d" % gpu["minor"],
                       "/dev/dri/" + gpu["card"]: "dev/dri/" + gpu["card"],
                       "/dev/dri/" + gpu["render"]: "dev/dri/" + gpu["render"]}
    for src, dst in sorted(c["mounts"].items()):
        if want_mounts.get(src) != dst.lstrip("/"):
            out.append("binds %s to %s" % (src, dst))
    for src in sorted(set(want_mounts) - set(c["mounts"])):
        out.append("does not bind %s" % src)
    stale = {ma for ma in c["whole"] if ma >= DYNAMIC_MAJOR_MIN and ma not in majors.values()}
    if stale:
        out.append("allows whole dynamic major %s, which is not %s now"
                   % (",".join(map(str, sorted(stale))), "/".join(NV_DYNAMIC)))
    return out


# --------------------------------------------------------------------------------------- the pins
def load_pins(P):
    """{"cards": {pci: {"uuid", "model", "t"}}}. The per-seat form of before ({"<ct>": {"uuid", "pci", ...}})
    is taken over: each pin becomes the pin of the address it was made at ("migrated": True then)."""
    try:
        with open(P.pins) as fh:
            raw = json.load(fh)
    except FileNotFoundError:
        return {"cards": {}}
    except (OSError, ValueError) as e:
        raise Refuse("cannot read %s: %s" % (P.pins, e))
    if not isinstance(raw, dict):
        raise Refuse("%s is no JSON object" % P.pins)
    cards = {norm_pci(k): v for k, v in (raw.get("cards") or {}).items() if isinstance(v, dict)}
    migrated = False
    for k, v in sorted(raw.items()):
        if not (k.isdigit() and isinstance(v, dict) and v.get("uuid") and v.get("pci")):
            continue
        pci = norm_pci(v["pci"])
        migrated = True
        have = cards.get(pci)
        if have and have.get("uuid", "").lower() != v["uuid"].lower():
            raise Refuse("%s: CT %s's old pin says %s at %s, the card pin there %s: decide with --unpin/--pin %s"
                         % (P.pins, k, v["uuid"], pci, have.get("uuid"), pci))
        cards.setdefault(pci, {"uuid": v["uuid"], "model": v.get("model", ""), "t": v.get("t", ""), "from_ct": k})
    out = {"cards": cards}
    if migrated:
        out["migrated"] = True
    return out


def save_pins(P, pins):
    os.makedirs(os.path.dirname(P.pins) or ".", exist_ok=True)
    with open(P.pins + ".tmp", "w") as fh:
        json.dump({"cards": pins["cards"]}, fh, indent=1, sort_keys=True)
        fh.write("\n")
    os.replace(P.pins + ".tmp", P.pins)
    pins.pop("migrated", None)


def check_pin(P, gpu, pins):
    p = pins["cards"].get(gpu["pci"]) if gpu else None
    if p and p.get("uuid") and gpu["uuid"] and p["uuid"].lower() != gpu["uuid"].lower():
        raise Refuse("the GPU at %s is %s (%s), the card pinned at that address is %s (%s): a card was added, taken "
                     "out or swapped and the bus renumbered; pick the card at its new address, or after a swap on "
                     "purpose: rdx_seat_gpu.py --unpin %s"
                     % (gpu["pci"], gpu["uuid"], gpu["model"], p["uuid"], p.get("model", "?"), gpu["pci"]))


def pin(P, gpu, pins):
    """Record the UUID of the card at its address (the first good start on it, or anew after --unpin);
    a migrated pins file is written in the new form as well. True if the card was pinned now."""
    changed = bool(pins.get("migrated"))
    new = bool(gpu and gpu["uuid"] and gpu["pci"] not in pins["cards"])
    if new:
        pins["cards"][gpu["pci"]] = {"uuid": gpu["uuid"], "model": gpu["model"], "t": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    if changed or new:
        save_pins(P, pins)
    return new


def pin_target(P, arg):
    """--pin/--unpin take a PCI address or a seat's container id (its current "gpu")."""
    if PCI_RE.fullmatch(norm_pci(arg)):
        return norm_pci(arg)
    seat = seat_of(P, arg)
    if not seat["gpu"]:
        raise Refuse("CT %s has no \"gpu\"" % arg)
    return seat["gpu"]


# ------------------------------------------------------------------------------------ the IRQs
def cpus_of(text):
    """A CPU list ("8-15,24-31") as a set; None if it is none."""
    out = set()
    for part in (text or "").replace(" ", "").split(","):
        if not part:
            continue
        m = re.fullmatch(r"(\d+)(?:-(\d+))?", part)
        if not m:
            return None
        out.update(range(int(m.group(1)), int(m.group(2) or m.group(1)) + 1))
    return out or None


def cpus_text(cpus):
    """The kernel's list form of a CPU set: "8-15,24-31"."""
    out, run = [], []
    for c in sorted(cpus) + [None]:
        if run and c is not None and c == run[-1] + 1:
            run.append(c)
            continue
        if run:
            out.append("%d-%d" % (run[0], run[-1]) if len(run) > 1 else str(run[0]))
        run = [c]
    return ",".join(out)


def mask_cpus(text):
    """A hex CPU mask ("00ff00ff", "ffffffff,00ff00ff") as a set; None if it is none."""
    h = (text or "").replace(",", "").strip()
    if not re.fullmatch(r"[0-9a-fA-F]+", h):
        return None
    v = int(h, 16)
    return {i for i in range(v.bit_length()) if v >> i & 1} or None


def card_functions(P, pci):
    """Every PCI function of the card at `pci` (the GPU, its HDMI sound .1, a USB-C controller ...)."""
    base = os.path.join(P.sys, "bus/pci/devices")
    stem = pci[:-1]
    return [n for n in sorted(os.listdir(base)) if n.startswith(stem) and PCI_RE.fullmatch(n)] if os.path.isdir(base) else []


def irqs_of(P, fn):
    """The interrupts of one PCI function: its MSI/MSI-X vectors, else its legacy line."""
    d = os.path.join(P.sys, "bus/pci/devices", fn)
    msi = os.path.join(d, "msi_irqs")
    if os.path.isdir(msi):
        return sorted(int(n) for n in os.listdir(msi) if n.isdigit())
    irq = rd(os.path.join(d, "irq"))
    return [int(irq)] if irq.isdigit() and int(irq) > 0 else []


def seat_cpus(P, ct):
    """The CPUs of a seat container: lxc.cgroup2.cpuset.cpus of its Proxmox config; None without one."""
    for k, v in parse_lines(rd(os.path.join(P.pve, "%s.conf" % ct)), pve=True):
        if k in ("lxc.cgroup2.cpuset.cpus", "lxc.cgroup.cpuset.cpus"):
            return cpus_of(v)
    return None


def irq_targets(P, seats=None, running=None):
    """{card pci: (cpus or None, why)} for every card a seat may take: the CPUs of the running seat that has
    it, else the host's (/proc/irq/default_smp_affinity). `running` ({ct: bool}) overrides what the cgroup
    tree says (the hookscript knows it before / after the tree does)."""
    seats = load_seats(P) if seats is None else seats
    running = running or {}
    host = mask_cpus(rd(os.path.join(P.proc, "irq/default_smp_affinity")))
    cards = set()
    for s in seats.values():
        cards.update(allowed_cards(P, s))
        if s["gpu"]:
            cards.add(s["gpu"])
    out = {}
    for pci in sorted(cards):
        owner = [ct for ct, s in sorted(seats.items()) if s["gpu"] == pci and running.get(ct, ct_running(P, ct))]
        if owner:
            cpus = seat_cpus(P, owner[0])
            out[pci] = (cpus, "seat %s (CT %d)" % (seats[owner[0]]["seat"], owner[0]) if cpus else
                        "seat %s (CT %d) has no lxc.cgroup2.cpuset.cpus: IRQs stay" % (seats[owner[0]]["seat"], owner[0]))
        else:
            out[pci] = (host, "no running seat: the host's CPUs" if host else "no running seat, no default_smp_affinity")
    return out


def irq_sync(P, running=None, seats=None):
    """Move the IRQs of the seat cards to their targets (irq_targets); returns what it did or could not."""
    lines = []
    for pci, (cpus, why) in irq_targets(P, seats, running).items():
        if not cpus:
            lines.append("%s: %s" % (pci, why))
            continue
        for fn in card_functions(P, pci):
            for irq in irqs_of(P, fn):
                path = os.path.join(P.proc, "irq", str(irq), "smp_affinity_list")
                if not os.path.exists(path) or cpus_of(rd(path)) == cpus:
                    continue
                try:
                    with open(path, "w") as fh:
                        fh.write(cpus_text(cpus) + "\n")
                    lines.append("irq %d (%s) -> %s, %s" % (irq, fn, cpus_text(cpus), why))
                except OSError as e:
                    lines.append("irq %d (%s) not movable: %s" % (irq, fn, e.strerror or e))
    return lines


# --------------------------------------------------------------------------- the seat's hostd config
CARD_NAME_RE = re.compile(r"(?i)((rtx|gtx)\s*)?\d{3,4}\s*(ti|super)?[a-z]?")


def hostd_conf_path(P, ct, seat):
    root = P.rootfs(ct, P.pve)
    return os.path.join(root, "etc/rdx/hostd-seat%s.json" % seat["seat"]) if root else None


def set_hostd_gpu(P, ct, seat, pci):
    """Write "gpu" of the seat's hostd config in its root file system (the container stopped); a "seat_name"
    that names a card ("5090", "3080a") goes too, it would be wrong after the switch (hostd: "Seat N").
    Returns the old "gpu"; Refuse if the file is not there or not JSON."""
    p = hostd_conf_path(P, ct, seat)
    if not p:
        raise Refuse("root file system of CT %s not found, its hostd config cannot follow" % ct)
    try:
        with open(p) as fh:
            conf = json.load(fh)
        st = os.stat(p)
    except (OSError, ValueError) as e:
        raise Refuse("%s: %s" % (p, e))
    if not isinstance(conf, dict):
        raise Refuse("%s is no JSON object" % p)
    old = conf.get("gpu", "")
    conf["gpu"] = pci
    if isinstance(conf.get("seat_name"), str) and CARD_NAME_RE.fullmatch(conf["seat_name"].strip()):
        conf["seat_name"] = ""
    with open(p + ".tmp", "w") as fh:
        json.dump(conf, fh, indent="\t")
        fh.write("\n")
    os.chmod(p + ".tmp", stat.S_IMODE(st.st_mode))
    try:
        os.chown(p + ".tmp", st.st_uid, st.st_gid)
    except OSError:
        pass
    os.replace(p + ".tmp", p)
    return old


# --------------------------------------------------------------------------------- the decision
def rootfs_path(ct, pve="/etc/pve/lxc"):
    """Host path of the container's root file system (pvesm path of its rootfs volume), None if unknown."""
    for k, v in parse_lines(rd(os.path.join(pve, "%s.conf" % ct)), pve=True):
        if k == "rootfs":
            vol = v.split(",")[0]
            try:
                r = subprocess.run(["pvesm", "path", vol], capture_output=True, text=True, timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                return None
            return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None
    return None


def hostd_gpu(P, ct, seat):
    """("gpu" of the seat's hostd config in its root file system or None, note)."""
    p = hostd_conf_path(P, ct, seat)
    if not p:
        return None, "root file system of CT %s not found, hostd config not compared" % ct
    try:
        with open(p) as fh:
            return norm_pci(json.load(fh).get("gpu", "")), None
    except (OSError, ValueError) as e:
        return None, "/etc/rdx/hostd-seat%s.json not read (%s), not compared" % (seat["seat"], e.__class__.__name__)


def others(P, ct, seats, gpu):
    """(refusals, notes): running containers other than `ct` whose config gives them a node of `gpu`.
    A seat: refused. Any other container (e.g. a compute container): a note."""
    bad, notes = [], []
    if not gpu:
        return bad, notes
    cts = set(seats)
    for f in sorted(os.listdir(P.pve)) if os.path.isdir(P.pve) else ():
        m = re.fullmatch(r"(\d+)\.conf", f)
        if m:
            cts.add(int(m.group(1)))
    for o in sorted(cts - {int(ct)}):
        if not ct_running(P, o):
            continue
        p = problems(P, claims(P, running_config(P, o)), gpu, {}, mine=False)
        if p and o in seats:
            bad.append("running seat CT %d (seat %s) %s" % (o, seats[o]["seat"], "; ".join(p)))
        elif p:
            notes.append("running CT %d (no seat) %s too" % (o, "; ".join(p)))
    return bad, notes


def include_path(P, ct):
    return os.path.join(P.run, "%s.gpu.conf" % ct)


def include_text(ct, seat, gpu, majors):
    t = time.strftime("%Y-%m-%d %H:%M:%S")
    if not gpu:
        return "# rdx-seat-gpu %s: CT %s, seat %s has no GPU (\"gpu\": \"\"): no GPU node\n" % (t, ct, seat["seat"])
    lines = ["# rdx-seat-gpu %s: CT %s, seat %s: GPU %s = %s %s, /dev/nvidia%d, %s, %s"
             % (t, ct, seat["seat"], gpu["pci"], gpu["model"], gpu["uuid"], gpu["minor"], gpu["card"], gpu["render"]),
             "# written at pre-start from the host's numbers; deleted at post-stop (seat/rdx_seat_gpu.py)",
             "lxc.cgroup2.devices.allow = c %d:%d rwm" % (NV_MAJOR, gpu["minor"]),
             "lxc.cgroup2.devices.allow = c %d:%d rwm" % (DRM_MAJOR, gpu["card_minor"]),
             "lxc.cgroup2.devices.allow = c %d:%d rwm" % (DRM_MAJOR, gpu["render_minor"])]
    for n in NV_DYNAMIC:
        if n in majors:
            lines.append("lxc.cgroup2.devices.allow = c %d:* rwm" % majors[n])
    # no "optional": a node that went away since resolve() fails the start
    for src in ("/dev/nvidia%d" % gpu["minor"], "/dev/dri/" + gpu["card"], "/dev/dri/" + gpu["render"]):
        lines.append("lxc.mount.entry = %s %s none bind,create=file" % (src, src[1:]))
    return "\n".join(lines) + "\n"


def decide(P, ct, hostd=False):
    """Everything about binding the seat CT's GPU now: (seat, gpu or None, majors, notes). Refuse if any
    doubt (module docstring); the pin is checked, not written."""
    seats = load_seats(P)
    seat = seat_of(P, ct, seats)
    notes = []
    if seat["gpu"] and seat["gpu"] not in allowed_cards(P, seat):
        raise Refuse("GPU %s is not one of seat %s's cards (%s)"
                     % (seat["gpu"], seat["seat"], ", ".join(allowed_cards(P, seat)) or "none"))
    for o, s in sorted(seats.items()):
        if o != int(ct) and seat["gpu"] and s["gpu"] == seat["gpu"] and ct_running(P, o):
            raise Refuse("GPU %s is seat %s's now (CT %d runs with it)" % (seat["gpu"], s["seat"], o))
    gpu = resolve(P, seat["gpu"]) if seat["gpu"] else None
    majors = nvidia_majors(P)
    if gpu:
        check_pin(P, gpu, load_pins(P))
        for n in NV_DYNAMIC:
            if n not in majors:
                notes.append("%s is not registered (module not loaded): no allow for it, seatmgr adds it live" % n)
    bad, more = others(P, ct, seats, gpu)
    if bad:
        raise Refuse("GPU %s: %s" % (gpu["pci"], "; ".join(bad)))
    notes += more
    if hostd and seat["gpu"] is not None:
        hg, note = hostd_gpu(P, ct, seat)
        if note:
            notes.append(note)
        elif hg != seat["gpu"]:
            raise Refuse("seatmgr.json says GPU %r for seat %s, its hostd-seat%s.json %r: make them agree"
                         % (seat["gpu"], seat["seat"], seat["seat"], hg))
    return seat, gpu, majors, notes


def write_include(P, ct, text):
    os.makedirs(P.run, mode=0o755, exist_ok=True)
    p = include_path(P, ct)
    with open(p + ".tmp", "w") as fh:
        fh.write(text)
    os.chmod(p + ".tmp", 0o644)
    os.replace(p + ".tmp", p)
    return p


def drop_include(P, ct):
    for p in (include_path(P, ct), include_path(P, ct) + ".tmp"):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def config_problems(P, ct, gpu, majors, text):
    """Problems of the seat's Proxmox config as the next start will see it, with `text` as its include."""
    path = os.path.join(P.pve, "%s.conf" % ct)
    lines, included = [], False
    for k, v in parse_lines(rd(path), pve=True):
        if k == "lxc.include" and v == include_path(P, ct):
            lines += parse_lines(text)
            included = True
        else:
            lines += included_lines(k, v)
    if not included:
        return ["%s does not include %s (pve-seat-create.sh --ct %s --update)" % (path, include_path(P, ct), ct)]
    return problems(P, claims(P, lines), gpu, majors)


def describe(gpu):
    return ("GPU %s (%s, %s) = /dev/nvidia%d, /dev/dri/%s, /dev/dri/%s"
            % (gpu["pci"], gpu["model"], gpu["uuid"], gpu["minor"], gpu["card"], gpu["render"]) if gpu else "no GPU")


def pre_start(P, ct, out=sys.stdout):
    """The PVE hookscript's pre-start: the include written from the host's numbers, the whole config
    checked with it, the pin set at the first good start. Raises Refuse."""
    drop_include(P, ct)                                 # nothing of an earlier start survives a refusal
    seat, gpu, majors, notes = decide(P, ct, hostd=True)
    text = include_text(ct, seat, gpu, majors)
    bad = config_problems(P, ct, gpu, majors, text)
    if bad:
        raise Refuse("config of CT %s: %s" % (ct, "; ".join(bad)))
    write_include(P, ct, text)
    pinned = pin(P, gpu, load_pins(P))
    for n in notes:
        print("rdx-seat-gpu: CT %s: note: %s" % (ct, n), file=out)
    print("rdx-seat-gpu: CT %s (seat %s): %s%s, nvidia-uvm/caps majors %s"
          % (ct, seat["seat"], describe(gpu), " (pinned now)" if pinned else "",
             "/".join(str(majors.get(n, "-")) for n in NV_DYNAMIC) if gpu else "-"), file=out)


def lxc_verify(P, ct, config=None, out=sys.stdout):
    """The LXC pre-start hook: the config lxc is starting with must give exactly the seat's GPU as the
    host numbers it now. Raises Refuse."""
    seat, gpu, majors, notes = decide(P, ct)
    path = config or os.path.join(P.lxc, str(ct), "config")
    bad = problems(P, claims(P, effective_lines(P, path)), gpu, majors)
    if bad:
        raise Refuse("lxc config %s: %s" % (path, "; ".join(bad)))
    print("rdx-seat-gpu: CT %s (seat %s): lxc config checked: %s" % (ct, seat["seat"], describe(gpu)), file=out)


# ------------------------------------------------------------------------------------------- main
def status(P, out=sys.stdout):
    seats = load_seats(P)
    pins = load_pins(P)
    rc = 0
    for ct in sorted(seats):
        s = seats[ct]
        run = "running" if ct_running(P, ct) else "stopped"
        try:
            _, gpu, majors, notes = decide(P, ct)
            bad = problems(P, claims(P, running_config(P, ct)), gpu, majors) if run == "running" else \
                config_problems(P, ct, gpu, majors, include_text(ct, seats[ct], gpu, majors))
            line = describe(gpu)
            if gpu and gpu["pci"] not in pins["cards"]:
                line += ", not pinned yet"
            if bad:
                line += "; %s config: %s" % ("its running" if run == "running" else "next start's", "; ".join(bad))
                rc = 1
            for n in notes:
                line += "; note: " + n
        except Refuse as e:
            line, rc = "REFUSED: %s" % e, 1
        print("CT %d seat %s (%s) %s: %s" % (ct, s["seat"], s["name"], run, line), file=out)
    return rc


def refused(P, ct, e):
    msg = "rdx-seat-gpu: CT %s: START REFUSED: %s" % (ct, e)
    print(msg, file=sys.stderr)
    P.syslog(msg, err=True)


def hook_irqs(P, ct, running):
    """post-start / post-stop: the seat cards' IRQs follow (module docstring); never fails the start or stop."""
    try:
        for line in irq_sync(P, {int(ct): running}):
            msg = "rdx-seat-gpu: CT %s %s: %s" % (ct, "started" if running else "stopped", line)
            print(msg)
            P.syslog(msg)
    except Exception as e:                              # noqa: BLE001 -- IRQs are no reason to fail a start
        msg = "rdx-seat-gpu: CT %s: IRQs not moved: %s: %s" % (ct, type(e).__name__, e)
        print(msg, file=sys.stderr)
        P.syslog(msg, err=True)


def cards(P, out=sys.stdout):
    """Every NVIDIA display card: its numbers, its pin, the seats that may take it and the one running with it."""
    seats, pins = load_seats(P), load_pins(P)
    targets = irq_targets(P, seats)
    for pci in nvidia_cards(P):
        try:
            line = describe(resolve(P, pci))
        except Refuse as e:
            line = "REFUSED: %s" % e
        p = pins["cards"].get(pci)
        line += "; pinned %s" % p["uuid"] if p else "; not pinned yet"
        may = [s["seat"] for _, s in sorted(seats.items()) if pci in allowed_cards(P, s)]
        run = [s["seat"] for ct, s in sorted(seats.items()) if s["gpu"] == pci and ct_running(P, ct)]
        line += "; seats %s may take it" % ",".join(may) if may else "; no seat may take it"
        line += "; seat %s runs with it" % run[0] if run else ""
        if pci in targets:
            line += "; IRQs -> %s (%s)" % (cpus_text(targets[pci][0]) if targets[pci][0] else "unchanged", targets[pci][1])
        print("%s: %s" % (pci, line), file=out)
    return 0


def main(argv, P=None):
    P = P or Paths()
    # lxc.hook.pre-start: <this> <name> lxc pre-start (lxc.hook.version 0, Proxmox's default), or no
    # arguments and LXC_NAME/LXC_HOOK_TYPE (version 1). The PVE hookscript's post-stop runs inside lxc's
    # post-stop hook and inherits its LXC_* environment: its arguments (<ct> <phase>) make it the hookscript.
    pve_phase = len(argv) == 2 and argv[0].isdigit() and argv[1] in ("pre-start", "post-start", "pre-stop", "post-stop")
    if (len(argv) >= 3 and argv[1] == "lxc") or \
       (not argv and os.environ.get("LXC_HOOK_VERSION") == "1" and os.environ.get("LXC_HOOK_TYPE")):
        if (argv[2] if argv else os.environ["LXC_HOOK_TYPE"]) != "pre-start":
            return 0
        ct = argv[0] if argv else os.environ.get("LXC_NAME", "")
        try:
            out = io.StringIO()
            lxc_verify(P, ct, os.environ.get("LXC_CONFIG_FILE") if os.environ.get("LXC_NAME") == ct else None, out)
            print(out.getvalue(), end="")
            P.syslog(out.getvalue().strip())
        except Refuse as e:
            refused(P, ct, e)
            return 1
        return 0
    if pve_phase:
        ct, phase = argv
        if phase == "pre-start":
            try:
                out = io.StringIO()
                pre_start(P, ct, out)
                print(out.getvalue(), end="")
                P.syslog(out.getvalue().strip())
            except Refuse as e:
                refused(P, ct, e)
                return 1
        elif phase == "post-stop":
            try:
                drop_include(P, ct)
            except OSError as e:                        # never fail a stop
                print("rdx-seat-gpu: CT %s: %s" % (ct, e), file=sys.stderr)
        if phase in ("post-start", "post-stop"):
            hook_irqs(P, ct, phase == "post-start")
        return 0
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--status", action="store_true", help="every seat: its GPU now, checked (default)")
    ap.add_argument("--check", metavar="CT", help="what the next start of CT would bind, or why it is refused")
    ap.add_argument("--write", metavar="CT", help="write the include of CT now (a running CT: for its restart "
                                                  "after a reboot from inside, which runs no hookscript)")
    ap.add_argument("--pin", metavar="PCI|CT", help="pin the card at PCI (CT: its seat's \"gpu\") to its UUID now")
    ap.add_argument("--unpin", metavar="PCI|CT", help="forget the pin of the card at PCI (the next good start pins anew)")
    ap.add_argument("--cards", action="store_true", help="every NVIDIA card: numbers, pin, seats, IRQ target")
    ap.add_argument("--irqs", action="store_true", help="move the seat cards' IRQs to their seats' CPUs now")
    a = ap.parse_args(argv)
    try:
        if a.cards:
            return cards(P)
        if a.irqs:
            for line in irq_sync(P):
                print(line)
            return 0
        if a.check:
            seat, gpu, majors, notes = decide(P, a.check, hostd=True)
            text = include_text(a.check, seat, gpu, majors)
            print(text, end="")
            for n in notes + ["the Proxmox config: " + b for b in config_problems(P, a.check, gpu, majors, text)]:
                print("# note: " + n)
            return 0
        if a.write:
            seat, gpu, majors, notes = decide(P, a.write, hostd=True)
            text = include_text(a.write, seat, gpu, majors)
            p = write_include(P, a.write, text)
            bad = config_problems(P, a.write, gpu, majors, text)
            print("wrote %s: %s" % (p, describe(gpu)))
            for n in notes + ["the Proxmox config: " + b for b in bad]:
                print("note: " + n)
            return 0
        if a.pin:
            gpu = resolve(P, pin_target(P, a.pin))
            pins = load_pins(P)
            pins["cards"].pop(gpu["pci"], None)
            print("%s pinned to %s" % (gpu["pci"], describe(gpu)) if pin(P, gpu, pins) else "nothing to pin")
            return 0
        if a.unpin:
            pci = pin_target(P, a.unpin)
            pins = load_pins(P)
            old = pins["cards"].pop(pci, None)
            save_pins(P, pins)
            print("%s unpinned (was %s)" % (pci, old))
            return 0
        return status(P)
    except Refuse as e:
        print("refused: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
