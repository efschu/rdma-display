#!/usr/bin/env python3
"""rdx-seatmgr: hand the multiseat host's input, sound and USB devices to the seat containers, live.

Runs as root on the Proxmox host. One container per seat (docs/MULTISEAT.md); the containers bind
no /dev/input and allow no input major: every keyboard, mouse, hidraw node, sound card and USB
device node reaches a seat only through this daemon.

  device   one physical thing: a USB device (all its event/mouse/js/hidraw nodes, its sound card,
           /dev/bus/usb/BBB/DDD), a PCI sound card (control/pcm/hw nodes and its jack inputs),
           or a lone input device (platform buttons). Every device belongs to at most one seat.
  rules    /etc/rdx/seatmgr.json, "rules": [{"kind": "port"|"device", "match": ..., "seat": N}]
           port   = the device's udev ID_PATH (the USB port or PCI slot it sits in), as logind's
                    multiseat rules key on it
           device = "usb:VVVV:PPPP[:serial]" / "pci:VVVV:DDDD"
  runtime  until the device is unplugged: a claim (first key press after a seat asked for it),
           a manual choice in the UI, or the default a new device got.
  virtual  a uinput or uhid device (streamed input, rdx_usb_host's HID mirrors, Steam Input's
           pads) is created inside a seat container, but its nodes appear in the HOST's devtmpfs.
           It belongs to the container that created it, always and without a rule: the open
           /dev/uinput and /dev/uhid file descriptors of the seat containers' processes are read
           (cgroup lxc/<ct>); every such fd creates at most one device, so a new device belongs to
           the container if all fds not yet matched to a device are there. If they are in
           several containers, the device goes nowhere until that is clear (never to a wrong
           seat). Virtual devices take no rules, no claim and no default and are never listed as
           unassigned.

Which seat a device belongs to, first match wins:
  runtime choice > device rule > port rule > the seat's own GPU sound card ("sound" of the
  seat) > the HOST CONSOLE. A newly plugged physical device belongs to the host console (by
  design: host console and seats only by assignment); a seat gets a physical device only by a
  rule, a claim or a choice in the UI. "seat": "host" (API, rules) names the host console
  explicitly, e.g. to keep a seat's GPU sound card on the host or to remember that a port's
  devices stay with the console.

Grab guard: a seat's keyboard must be read by the seat alone, never by the host's VT keyboard
handler as well, which sees every keyboard of the host kernel whatever container it is bound into
(so taking a node out of a container protects nothing). Who grabs what (EVIOCGRAB) in a seat
container: gamescope's rdx backend (a stream) grabs the client's devices only -- name or phys
"rdx:", or behind vhci_hcd (USB/IP); before it starts the rx/usb_host gate guards those --, its DRM
backend (the seat monitor) grabs every keyboard and pointer, never a joystick; RDX_GS_GRAB=0 in its
environment: nothing. For every keyboard-capable, non-joystick node of a device handed to a running
seat, in a thread of its own (5 times a second, and at once when a gamescope appears: it looks
for new gamescope processes every 50 ms):
  - no running gamescope would grab it: seatmgr grabs it itself at once and its keys are
    swallowed -- the seat loses nothing, nobody there would read it. That holds for EVERY such
    keyboard, whatever its name or creator ("rdx: ..." made by some other process in the
    container is no client device: one reached tty11 while seatmgr left "rdx:" names to
    the rx/usb_host gate). When the gate holds it already (rdx_input_rx, rdx_usb_host grab what
    they make until gamescope runs), seatmgr's grab answers EBUSY: fine, it tries again each pass.
    A new input node is handled when its kernel uevent comes (udevadm monitor --kernel), without
    the 0.3 s gathering of a plug's burst;
  - a gamescope that would grab it runs: seatmgr lets go at once (gamescope retries an EBUSY grab
    0.5 s since rdx-gs9), then, GRAB_GRACE_S after the later of: THAT gamescope appeared (rx makes a
    session's devices 1-3 s before gamescope starts), the device came (a resume or a hotplug
    brings it to a gamescope that runs already), probes it once a second with a
    non-destructive EVIOCGRAB (EBUSY: grabbed; success: nobody holds it, released at once, nothing
    in between); ungrabbed for more than GRAB_FREE_S, seatmgr holds it until a new gamescope runs
    or a grabbing gamescope opens the node anew (an fd not there at the hold: libinput's hotplug,
    a resume), then lets go in the next pass so that open's grab finds it free.
An event node goes into a container only with the host's udev record (libinput skips one
announced without ID_INPUT_*, and the kernel uevent's rescan comes before udevd): UDEV_WAIT_S.
The seat keeps its assignment and its node; the state shows "withheld" with the reason. The guard
acts while the host consoles read keyboards ("host_console_keyboard": true, the default once the
seats grab their input) or with "grab_guard": "enforce"; while the consoles' keyboards are off
(K_OFF protects them) it only logs what it would hold.

The host's own consoles never read a seat's keys (security): every keyboard of a seat --
the uinput/uhid ones a container creates as much as a physical one handed to it -- is an input
device of the HOST kernel, whose VT keyboard handler feeds every keyboard into the active VT (a
getty on a GPU's HDMI: a password typed in a seat landed at the host's login prompt). The seats'
exclusive grabs and the grab guard above keep seat keys away from it, so the consoles keep their
keyboard (by design: switching the physical keyboard off is no option): "host_console_keyboard"
is true by default, and the modes the consoles had are restored once (a console at K_OFF without
a mode seen before: K_UNICODE). "host_console_keyboard": false locks them: seatmgr keeps their
keyboard mode at K_OFF (KDSKBMODE) at its start, after every container start/stop and once a
second, because a restarting getty or chvt set it again. SysRq works either way (its own input
handler). The other host reader of keyboards, systemd-logind (any key device is a "power-switch"
on systemd 257, so a streamed power or sleep key would switch off or suspend the host), is cut off
by seat/70-power-switch.rules, which install-seatmgr.sh puts in /etc/udev/rules.d over systemd's
(only the host's ACPI and platform buttons are power switches then); seatmgr warns when it is
missing.

Card choice (chosen per seat which card it takes; e.g. three seats with their own data and logins,
any of them on any free card): "gpu" of a seat in seatmgr.json is its current card,
"gpus" the cards it may take (default every NVIDIA display device). GET /api/gpus lists the cards with
their state: free, seat N (a running seat has it), llm / busy (processes outside the seats use it, llm when
one is an LLM inference server -- sglang, vllm, llama.cpp, ollama ..., by "llm_names" -- found from
nvidia-smi's compute apps and the open /dev/nvidiaN and DRM nodes in /proc -- a process that holds the
/dev/nvidiaN of EVERY card and no DRM node and no context, an NVML monitor like nvtop or an exporter,
is no user), booked (booked in the optional GPU booking service: a running window of another owner,
"gpuq" = its URL, read only, "" = not asked). POST /api/seat/start
{"seat": N, "gpu": PCI, "force": false, "gpuq_booking": ID} starts seat N on that card: the checks
above (refused with the reason, e.g. "RTX 5090 (0000:15:00.0): it is used by an LLM server: ..."), the card's pin
(rdx_seat_gpu), and a running seat with a stream, a paused app or an app on its monitor is refused
unless "force"; then the container is shut down if it runs, "gpu" goes into seatmgr.json and into the
seat's /etc/rdx/hostd-seatN.json, and `pct start` binds the card (the hookscript checks everything
again). A failed start puts the old card back (and starts the seat on it if it ran). "gpuq_booking":
the caller's own booking-service window for this card, not counted against it. Only clients from "control_from"
(default: this host and the RDMA link 10.0.1.0/24, the cable to the client) may call it; GET is
open like the rest of the API. The IRQs of the seat cards follow the running seats (rdx_seat_gpu
"IRQs"), checked at every container start and stop seatmgr sees.
"""

import argparse
import errno
import fcntl
import json
import os
import re
import select
import shutil
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import ipaddress
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdx_seat_gpu as sg  # noqa: E402  -- the seats' GPU hook (same directory): its checks for the card choice

CONF_DEFAULT = "/etc/rdx/seatmgr.json"
RUNTIME_DEFAULT = "/run/rdx-seatmgr/runtime.json"
DEFAULT_CONF = {
    # loopback only; the link / bridge addresses (e.g. "10.0.1.1:8771") go into /etc/rdx/seatmgr.json.
    # An address that is not (yet) there is retried for 60 s, then skipped.
    "listen": ["127.0.0.1:8771"],
    "seats": {},
    "rules": [],
    "host_console_keyboard": True,      # false: lock the host's VTs' keyboards (ConsoleKeyboard)
    # card choice (module docstring "Card choice")
    "control_from": ["127.0.0.0/8", "10.0.1.0/24"],   # who may stop/start a seat: this host, the RDMA link
    "gpuq": "",                                        # URL of an optional GPU booking service (read only); "" = not asked
    "gpuq_seat_owners": ["rdx-seat"],                  # its owners (prefix) whose windows are the seats' own
    "llm_names": ["sglang", "vllm", "llama", "ollama", "text-generation"],   # process names of LLM servers
}
BLINK_S = 2.0
HOST = "host"                 # the host console as a target ("seat": "host" in the API and in rules)
GRAB_GRACE_S = 5.0            # a gamescope may take this long after it appeared to grab its devices
GRAB_FREE_S = 1.0             # ... and then a device it should grab may be free this long (grab guard)
GUARD_TICK_S = 0.2            # the guard thread: a whole pass this often ...
GS_WATCH_S = 0.05             # ... and a look for new gamescope processes this often (a new one: a pass
                              # at once, so seatmgr lets go well within gamescope's retries)
UDEV_WAIT_S = 3.0             # an event node waits this long for the host's udev record before it goes in bare
PROBE_EVERY_S = 1.0           # ... and probe each device at most this often
EVIOCGRAB = 0x40044590
CLAIM_S = 15.0
# never handed out: /dev/input/mice multiplexes every mouse of the machine
NEVER = {"mice"}
EV_KEY, EV_REL, EV_ABS = 1, 2, 3
EVENT_FMT = "llHHi"           # struct input_event on 64-bit Linux
EVENT_SZ = struct.calcsize(EVENT_FMT)


def log(*a):
    print("[seatmgr]", *a, file=sys.stderr, flush=True)


def rd(path, default=""):
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return default


# ------------------------------------------------------------------------------------- the scan
class Paths:
    """Roots of everything read or written; the tests point them into a temp tree."""

    def __init__(self, sys_="/sys", dev="/dev", udev="/run/udev/data", proc="/proc", pve="/etc/pve/lxc"):
        self.sys, self.dev, self.udev, self.proc, self.pve = sys_, dev, udev, proc, pve
        self.stat = os.stat


def udev_props(P, majmin):
    """E: lines of the host's udev record of a char device."""
    return udev_record(P, "c" + majmin)


def udev_record(P, name):
    """E: lines of the host's udev record `name` ("c13:66", "+sound:card2")."""
    out = {}
    for line in rd(os.path.join(P.udev, name)).splitlines():
        if line.startswith("E:") and "=" in line:
            k, v = line[2:].split("=", 1)
            out[k] = v
    return out


def is_usb_device(path):
    # a usb_device directory has idVendor and its name has no ':' (interfaces are "1-3:1.0")
    return os.path.exists(os.path.join(path, "idVendor")) and ":" not in os.path.basename(path)


def usb_path_id(P, usbdev):
    """udev ID_PATH of a USB device: from its udev record, else built like path_id does."""
    mm = rd(os.path.join(usbdev, "dev"))
    if mm:
        p = udev_props(P, mm).get("ID_PATH")
        if p:
            return p
    name = os.path.basename(usbdev)                     # "1-3.2"
    port = name.split("-", 1)[1] if "-" in name else name
    pci = ""
    d = usbdev
    while d and d != "/" and not pci:
        d = os.path.dirname(d)
        if re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]", os.path.basename(d)):
            pci = os.path.basename(d)
    return ("pci-%s-" % pci if pci else "") + "usb-0:" + port


class Node:
    __slots__ = ("dev", "major", "minor", "mode", "gid", "cls")

    def __init__(self, dev, major, minor, mode, gid, cls):
        self.dev, self.major, self.minor, self.mode, self.gid, self.cls = dev, major, minor, mode, gid, cls

    def key(self):
        return (self.dev, self.major, self.minor)

    def json(self):
        return {"dev": self.dev, "major": self.major, "minor": self.minor, "class": self.cls}


class Device:
    def __init__(self, gid, kind, syspath):
        self.id, self.kind, self.syspath = gid, kind, syspath
        self.name, self.path, self.devid = "", "", ""
        self.nodes = []
        self.has_input = self.has_sound = False
        self.keys = False          # an input node that can report key/button presses
        self.motion = False
        self.inputs = []           # event nodes (for claim and activity)
        self.roles = set()         # keyboard, mouse, joystick, ... from udev ID_INPUT_*
        self.host_drivers = set()  # drivers the host binds to its USB interfaces
        self.virtual = ""          # "uinput" / "uhid": created by a process, follows its container
        self.cards = []            # sysfs paths of its ALSA cards (their udev record + change uevent)

    def json(self):
        return {"id": self.id, "kind": self.kind, "name": self.name, "path": self.path,
                "devid": self.devid, "roles": sorted(self.roles), "keys": self.keys,
                "assignable": self.assignable(), "host_drivers": sorted(self.host_drivers), "virtual": self.virtual,
                "nodes": [n.json() for n in self.nodes]}

    def assignable(self):
        if self.kind == "virtual":
            return False                                # it follows the container that created it
        # a USB device the host itself uses (network, bluetooth: a bound driver and no
        # input/sound/hidraw node) is never handed to a seat: its usbfs node could reset it
        return not (self.kind == "usb" and self.host_drivers and
                    not any(n.cls in ("input", "sound", "hidraw") for n in self.nodes))


def _group_of(P, real):
    """(id, kind, syspath) of the device a sysfs node belongs to."""
    m = re.search(r"/devices/virtual/misc/uhid/([^/]+)", real)
    if m:
        return "virtual:uhid-" + m.group(1), "virtual", real[:m.end()]
    m = re.search(r"/devices/virtual/input/(input\d+)", real)
    if m:
        return "virtual:" + m.group(1), "virtual", real[:m.end()]
    d = real
    card = None
    inp = None
    while d.startswith(P.sys + "/devices") and d != P.sys + "/devices":
        if is_usb_device(d):
            if rd(os.path.join(d, "bDeviceClass")) == "09":
                break                                   # a hub: the node is the hub's own
            return "usb:" + os.path.basename(d), "usb", d
        b = os.path.basename(d)
        if card is None and re.fullmatch(r"card\d+", b) and os.path.basename(os.path.dirname(d)) == "sound":
            card = os.path.dirname(os.path.dirname(d))  # the PCI function that has the card
        if inp is None and re.fullmatch(r"input\d+", b):
            inp = d
        d = os.path.dirname(d)
    if card is not None:
        return "sound:" + os.path.basename(card), "sound", card
    if inp is not None:
        phys = rd(os.path.join(inp, "phys")) or rd(os.path.join(inp, "name"))
        return "input:" + re.sub(r"[^A-Za-z0-9_.:-]", "_", phys or os.path.basename(inp)), "input", inp
    return None


def _node(P, classdir, cls):
    mm = rd(os.path.join(classdir, "dev"))
    if not mm or ":" not in mm:
        return None
    devname = ""
    for line in rd(os.path.join(classdir, "uevent")).splitlines():
        if line.startswith("DEVNAME="):
            devname = line[8:]
    if not devname:
        return None
    dev = os.path.join(P.dev, devname)
    try:
        st = P.stat(dev)
    except OSError:
        return None
    ma, mi = (int(x) for x in mm.split(":"))
    if not stat.S_ISCHR(st.st_mode) or os.major(st.st_rdev) != ma or os.minor(st.st_rdev) != mi:
        return None
    return Node("/dev/" + devname, ma, mi, stat.S_IMODE(st.st_mode), st.st_gid, cls)


def scan(P):
    """Every device with its nodes, keyed by device id."""
    devs = {}

    def get(real, cls):
        g = _group_of(P, real)
        if g is None:
            return None
        gid, kind, syspath = g
        dv = devs.get(gid)
        if dv is None:
            dv = devs[gid] = Device(gid, kind, syspath)
        return dv

    for cls, sub in (("input", "class/input"), ("hidraw", "class/hidraw"), ("sound", "class/sound")):
        base = os.path.join(P.sys, sub)
        for name in sorted(os.listdir(base)) if os.path.isdir(base) else ():
            if name in NEVER or (cls == "input" and not re.fullmatch(r"(event|mouse|js)\d+", name)):
                continue
            if cls == "sound" and re.fullmatch(r"card\d+", name):
                real = os.path.realpath(os.path.join(base, name))
                dv = get(real, cls)
                if dv is not None:
                    dv.cards.append(real)
                continue
            if cls == "sound" and not re.fullmatch(r"(controlC|pcmC|hwC)\d+.*", name):
                continue
            cd = os.path.join(base, name)
            real = os.path.realpath(cd)
            nd = _node(P, cd, cls)
            if nd is None:
                continue
            dv = get(real, cls)
            if dv is None:
                continue
            dv.nodes.append(nd)
            if cls == "input":
                dv.has_input = True
                inp = os.path.dirname(real)
                ev = int(rd(os.path.join(inp, "capabilities/ev"), "0") or "0", 16)
                if name.startswith("event"):
                    props = udev_props(P, "%d:%d" % (nd.major, nd.minor))
                    for k in ("KEYBOARD", "MOUSE", "JOYSTICK", "TOUCHPAD", "TABLET", "KEY", "SWITCH"):
                        if props.get("ID_INPUT_" + k) == "1":
                            dv.roles.add(k.lower())
                    if ev & (1 << EV_KEY) and rd(os.path.join(inp, "capabilities/key"), "0").strip("0 "):
                        dv.keys = True
                        dv.inputs.append(nd.dev)
                    elif ev & ((1 << EV_REL) | (1 << EV_ABS)):
                        dv.inputs.append(nd.dev)
                    if ev & ((1 << EV_REL) | (1 << EV_ABS)):
                        dv.motion = True
                    if not dv.name:
                        dv.name = rd(os.path.join(inp, "name"))
            elif cls == "sound":
                dv.has_sound = True
    # USB devices: their usbfs node, names, ids; other USB devices (no input/sound) are listed too
    ub = os.path.join(P.sys, "bus/usb/devices")
    for name in sorted(os.listdir(ub)) if os.path.isdir(ub) else ():
        d = os.path.realpath(os.path.join(ub, name))
        if not is_usb_device(d) or name.startswith("usb") or rd(os.path.join(d, "bDeviceClass")) == "09":
            continue
        gid = "usb:" + name
        dv = devs.get(gid) or devs.setdefault(gid, Device(gid, "usb", d))
        busnum, devnum = rd(os.path.join(d, "busnum")), rd(os.path.join(d, "devnum"))
        mm = rd(os.path.join(d, "dev"))
        if busnum and devnum and mm:
            nd = None
            dev = os.path.join(P.dev, "bus/usb/%03d/%03d" % (int(busnum), int(devnum)))
            try:
                st = P.stat(dev)
                nd = Node("/dev/bus/usb/%03d/%03d" % (int(busnum), int(devnum)), os.major(st.st_rdev),
                          os.minor(st.st_rdev), stat.S_IMODE(st.st_mode), st.st_gid, "usb")
            except OSError:
                pass
            if nd:
                dv.nodes.append(nd)
        for itf in os.listdir(d):
            drv = os.path.join(d, itf, "driver")
            if itf.startswith(name + ":") and os.path.islink(drv):
                dv.host_drivers.add(os.path.basename(os.readlink(drv)))
        vid, pid, ser = rd(os.path.join(d, "idVendor")), rd(os.path.join(d, "idProduct")), rd(os.path.join(d, "serial"))
        dv.devid = "usb:%s:%s" % (vid, pid) + (":" + ser if ser else "")
        dv.path = usb_path_id(P, d)
        prod = " ".join(x for x in (rd(os.path.join(d, "manufacturer")), rd(os.path.join(d, "product"))) if x)
        if prod:
            dv.name = prod
    for dv in devs.values():
        if dv.kind == "sound":
            bdf = os.path.basename(dv.syspath)
            dv.path = "pci-" + bdf
            dv.devid = "pci:%s:%s" % (rd(os.path.join(dv.syspath, "vendor"))[2:], rd(os.path.join(dv.syspath, "device"))[2:])
            cards = [n for n in os.listdir(os.path.join(dv.syspath, "sound")) if n.startswith("card")] \
                if os.path.isdir(os.path.join(dv.syspath, "sound")) else []
            ids = [rd(os.path.join(dv.syspath, "sound", c, "id")) for c in cards]
            dv.name = "%s (%s)" % (", ".join(i for i in ids if i) or "sound card", bdf)
            dv.roles.add("sound")
        elif dv.kind == "virtual":
            dv.virtual = "uhid" if dv.id.startswith("virtual:uhid-") else "uinput"
            if not dv.name:
                for line in rd(os.path.join(dv.syspath, "uevent")).splitlines():
                    if line.startswith("HID_NAME="):
                        dv.name = line[9:]
            dv.name = dv.name or dv.id
        elif dv.kind == "input":
            dv.path = dv.id[len("input:"):]
            dv.devid = "input:" + (dv.name or dv.path)
        if dv.has_sound:
            dv.roles.add("sound")
    return devs


# ----------------------------------------------------------------------------- seats and rules
def gpu_has_monitor(P, bdf):
    base = os.path.join(P.sys, "class/drm")
    for name in os.listdir(base) if os.path.isdir(base) else ():
        if not re.fullmatch(r"card\d+-.+", name):
            continue
        cd = os.path.join(base, name)
        if os.path.basename(os.path.realpath(os.path.join(cd, "device"))) != bdf and \
           os.path.basename(os.path.realpath(os.path.join(base, name.split("-")[0], "device"))) != bdf:
            continue
        if rd(os.path.join(cd, "status")) == "connected":
            return True
    return False


def default_sound_path(seat):
    """The seat's own GPU sound card: config "sound", else function .1 of its GPU."""
    if seat.get("sound"):
        return seat["sound"]
    g = seat.get("gpu", "")
    return "pci-" + g[:-1] + "1" if re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.0", g) else ""


def rule_matches(rule, dv):
    m = str(rule.get("match", ""))
    if rule.get("kind") == "port":
        return bool(m) and m == dv.path
    if rule.get("kind") == "device":
        # without a serial a rule covers every unit of that model
        return bool(m) and (m == dv.devid or dv.devid.startswith(m + ":"))
    return False


def effective(conf, runtime, dv):
    """(seat or None, how) of one device; first match wins (module docstring)."""
    rt = runtime.get(dv.id)
    if rt is not None:
        return rt.get("seat"), rt.get("how", "manual")
    for kind in ("device", "port"):
        for r in conf.get("rules", []):
            if r.get("kind") == kind and rule_matches(r, dv) and str(r.get("seat")) == HOST:
                return None, "host console (%s rule)" % kind
            if r.get("kind") == kind and rule_matches(r, dv) and str(r.get("seat")) in conf["seats"]:
                return int(r["seat"]), kind + " rule"
    for n, seat in sorted(conf["seats"].items()):
        if dv.kind == "sound" and dv.path and dv.path == default_sound_path(seat):
            return int(n), "GPU sound"
    return None, "host console"


# ----------------------------------------------------------------------------- host consoles
KDGKBMODE, KDSKBMODE, K_OFF, K_UNICODE = 0x4B44, 0x4B45, 0x04, 0x03
CONSOLE_STATE = "/run/rdx-seatmgr/vt-kbd-modes.json"
CONSOLE_STATE_OLD = "/run/rdx-vt-kbd-modes"          # the first hand-run mitigation
NO_POWER_RULE = "/etc/udev/rules.d/70-power-switch.rules"     # ours shadows systemd's


class ConsoleKeyboard:
    """The keyboard mode of the host's text consoles (module docstring). open_ and ioctl_ are
    os.open and fcntl.ioctl; the tests pass fakes."""

    def __init__(self, P, state=CONSOLE_STATE, old_state=CONSOLE_STATE_OLD, open_=None, ioctl_=None):
        self.P, self.state, self.old_state = P, state, old_state
        self.open_ = open_ or (lambda path: os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_CLOEXEC))
        self.ioctl_ = ioctl_ or fcntl.ioctl
        self.saved = {}                 # tty -> keyboard mode before seatmgr switched it off
        for p in (state, old_state):
            try:
                with open(p) as fh:
                    for k, v in json.load(fh).items():
                        self.saved.setdefault(k, int(v))
            except (OSError, ValueError):
                pass
        self.restored = False

    def ttys(self):
        """tty1..tty12 (getty's range) and the active VT, if another"""
        names = ["tty%d" % n for n in range(1, 13)]
        act = rd(os.path.join(self.P.sys, "class/tty/tty0/active"))
        if re.fullmatch(r"tty\d+", act) and act not in names:
            names.append(act)
        return [os.path.join(self.P.dev, n) for n in names]

    def _mode(self, fd):
        return struct.unpack("i", self.ioctl_(fd, KDGKBMODE, b"\0" * 4))[0]

    def _save(self):
        try:
            os.makedirs(os.path.dirname(self.state), exist_ok=True)
            with open(self.state + ".tmp", "w") as fh:
                json.dump(self.saved, fh)
            os.replace(self.state + ".tmp", self.state)
        except OSError as e:
            log("%s: %s" % (self.state, e))

    def enforce(self):
        """every console's keyboard off; returns the ttys it had to switch (log once each time)"""
        self.restored = False
        changed = []
        for path in self.ttys():
            try:
                fd = self.open_(path)
            except OSError:
                continue
            try:
                cur = self._mode(fd)
                if cur != K_OFF:
                    self.saved.setdefault(path, cur)
                    self.ioctl_(fd, KDSKBMODE, K_OFF)
                    changed.append("%s %d" % (os.path.basename(path), cur))
            except OSError as e:
                log("%s: keyboard mode: %s" % (path, e))
            finally:
                os.close(fd)
        if changed:
            self._save()
            log("host consoles: keyboard off (K_OFF) on %s (mode before; seat keys must never reach a "
                "console)" % ", ".join(changed))
        return changed

    def restore(self):
        """"host_console_keyboard": true -- the modes seen before, once"""
        if self.restored:
            return
        self.restored = True
        # a console still at K_OFF without a mode seen before (switched off by an earlier hand-run or a
        # lost state file): the console's usual mode, K_UNICODE
        for path in self.ttys():
            if path in self.saved:
                continue
            try:
                fd = self.open_(path)
            except OSError:
                continue
            try:
                if self._mode(fd) == K_OFF:
                    self.saved[path] = K_UNICODE
            except OSError:
                pass
            finally:
                os.close(fd)
        for path, mode in sorted(self.saved.items()):
            try:
                fd = self.open_(path)
            except OSError:
                continue
            try:
                self.ioctl_(fd, KDSKBMODE, mode)
                log("host consoles: %s keyboard mode %d again (host_console_keyboard)" % (path, mode))
            except OSError as e:
                log("%s: keyboard mode: %s" % (path, e))
            finally:
                os.close(fd)
        self.saved = {}
        self._save()
        try:
            os.unlink(self.old_state)                  # the hand-run's modes are back: never apply them again
        except OSError:
            pass


# ----------------------------------------------------------------------------- grab guard
def _input_dir(P, node):
    return os.path.join(P.sys, "class/input", os.path.basename(node))


def node_is_keyboard(P, node):
    """an event node the VT keyboard handler would take: any key below BTN_MISC (0x100)"""
    words = rd(os.path.join(_input_dir(P, node), "device/capabilities/key")).split()
    if not words:
        return False
    low = int(words[-1], 16)                                     # bits 0..63
    nxt = [int(w, 16) for w in words[-4:-1]]                     # bits 64..255
    return bool(low & ~1) or any(nxt)


def node_is_client(P, node):
    """a client's device (gamescope's rdx backend grabs these): name or phys "rdx:", or USB/IP"""
    d = _input_dir(P, node)
    return rd(os.path.join(d, "device/name")).startswith("rdx:") or \
        rd(os.path.join(d, "device/phys")).startswith("rdx:") or "vhci_hcd" in os.path.realpath(d)


def uevent_is_input_add(line):
    """a kernel uevent line of udevadm monitor for a new input event node:
    "KERNEL[123.45] add      /devices/virtual/input/input170/event21 (input)" """
    f = line.split()
    return len(f) >= 4 and f[0].startswith("KERNEL[") and f[1] == "add" and \
        os.path.basename(f[2]).startswith("event") and f[3] == "(input)"


def grab_probe(P, node):
    """"grabbed" (EVIOCGRAB answers EBUSY: someone holds it), "free" (we got it: released at once,
    nothing in between), None (cannot tell)"""
    try:
        fd = os.open(os.path.join(P.dev, node[len("/dev/"):]), os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return None
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
    except OSError as e:
        os.close(fd)
        return "grabbed" if e.errno == errno.EBUSY else None
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 0)
    except OSError:
        pass
    os.close(fd)
    return "free"


def grab_hold(P, node):
    """grab a node and keep the fd: its events go nowhere else"""
    try:
        fd = os.open(os.path.join(P.dev, node[len("/dev/"):]), os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return None
    try:
        fcntl.ioctl(fd, EVIOCGRAB, 1)
        return fd
    except OSError:
        os.close(fd)
        return None


def proc_fds_on(P, pid, rdevs):
    """the fd numbers of process pid open on one of the char devices rdevs (st_rdev values); the
    /proc fd links resolve across the container's mount namespace"""
    out = set()
    d = os.path.join(P.proc, str(pid), "fd")
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in names:
        try:
            st = os.stat(os.path.join(d, n))
        except OSError:
            continue
        if stat.S_ISCHR(st.st_mode) and st.st_rdev in rdevs:
            out.add((pid, n))
    return out


def gamescopes(P, ct):
    """{pid: {"backend": "rdx"|"drm", "grab": bool}} of the container's gamescope processes"""
    out = {}
    for pid in ct_pids(P, ct):
        if rd(os.path.join(P.proc, str(pid), "comm")) not in ("gamescope", "gamescope-wl"):
            continue
        try:
            with open(os.path.join(P.proc, str(pid), "cmdline"), "rb") as fh:
                argv = fh.read().split(b"\0")
            with open(os.path.join(P.proc, str(pid), "environ"), "rb") as fh:
                env = fh.read().split(b"\0")
        except OSError:
            continue
        backend = "drm"                                          # gamescope's own pick on a seat
        if b"--backend" in argv and argv.index(b"--backend") + 1 < len(argv):
            backend = argv[argv.index(b"--backend") + 1].decode(errors="replace")
        out[pid] = {"backend": backend, "grab": b"RDX_GS_GRAB=0" not in env}
    return out


# ----------------------------------------------------------------------------- containers
def ct_running(P, ct):
    return os.path.isdir(os.path.join(P.sys, "fs/cgroup/lxc", str(ct)))


def parse_lxc_info(out):
    """Init pid of a RUNNING container from `lxc-info -s -p` ("State: RUNNING / PID: 123"), else 0."""
    st = re.search(r"^(?:State:\s*)?(\w+)\s*$", out, re.M)
    pid = re.search(r"^(?:PID:\s*)?(\d+)\s*$", out, re.M)
    return int(pid.group(1)) if st and st.group(1) == "RUNNING" and pid else 0


def ct_pids(P, ct):
    """Every process of a running container (all cgroups below lxc/<ct>)."""
    out = []
    for base, _dirs, files in os.walk(os.path.join(P.sys, "fs/cgroup/lxc", str(ct))):
        if "cgroup.procs" in files:
            out += [int(x) for x in rd(os.path.join(base, "cgroup.procs")).split() if x.isdigit()]
    return out


def fd_holders(P, cts, path):
    """{"pid:fd": ct} of the open file descriptors on `path` (/dev/uinput, /dev/uhid) of the containers' processes."""
    out = {}
    for ct in cts:
        for pid in ct_pids(P, ct):
            fdd = os.path.join(P.proc, str(pid), "fd")
            try:
                fds = os.listdir(fdd)
            except OSError:
                continue
            for fd in fds:
                try:
                    if os.readlink(os.path.join(fdd, fd)) == path:
                        out["%d:%s" % (pid, fd)] = int(ct)
                except OSError:
                    pass
    return out


NV_DYNAMIC = ("nvidia-uvm", "nvidia-caps")   # whole majors a seat needs, numbered anew at module load
DYNAMIC_MAJOR_MIN = 234                       # the kernel hands out char majors 234-254 and 384-511


def char_majors(P):
    """{major: {names}} of the character devices the host has registered now (/proc/devices); one
    name can hold several majors (infiniband_mad: 231 and a dynamic one), one major several names."""
    out = {}
    for line in rd(os.path.join(P.proc, "devices")).splitlines():
        if line.startswith("Block"):
            break
        f = line.split()
        if len(f) == 2 and f[0].isdigit():
            out.setdefault(int(f[0]), set()).add(f[1])
    return out


def nvidia_majors(majors):
    """{name: major} of NV_DYNAMIC as registered now."""
    return {n: ma for ma, names in sorted(majors.items()) for n in sorted(names) if n in NV_DYNAMIC}


def conf_whole_majors(P, ct):
    """Majors the container's Proxmox config allows whole ("lxc.cgroup2.devices.allow: c N:* rwm"),
    current section only (snapshots follow in [sections]), and the ones of the files it includes
    outside /usr/share/lxc ("c N:* rwm" lines of the GPU include /run/rdx-seat/<ct>.gpu.conf that
    seat/rdx_seat_gpu.py writes at every start, lxc syntax "key = value")."""
    out = set()
    pat = r"lxc\.cgroup2\.devices\.allow\s*[:=]\s*c\s+(\d+):\*\s"
    for line in rd(os.path.join(P.pve, "%s.conf" % ct)).splitlines():
        if line.startswith("["):
            break
        m = re.match(pat, line + " ")
        if m:
            out.add(int(m.group(1)))
        inc = re.match(r"lxc\.include\s*[:=]\s*(/\S+)\s*$", line)
        if inc and not inc.group(1).startswith("/usr/share/lxc/"):
            for iline in rd(inc.group(1)).splitlines():
                m = re.match(pat, iline.strip() + " ")
                if m:
                    out.add(int(m.group(1)))
    return out


def stale_majors(P, ct, majors):
    """Whole-major allows of the container's config on a DYNAMIC major that is not nvidia-uvm or
    nvidia-caps now. Such a number goes to whatever driver loads first: on a host after an RDMA NIC went
    in, the old uvm/caps majors 505/510 were infiniband_mad (the NIC's umad) and fwctl (firmware
    control), so a stale line is an open door, not just a dead one. Fixed majors (< 234) stay."""
    want = set(nvidia_majors(majors).values())
    return {ma for ma in conf_whole_majors(P, ct) if ma >= DYNAMIC_MAJOR_MIN and ma not in want}


def ct_init_pid(ct):
    try:
        out = subprocess.run(["lxc-info", "-n", str(ct), "-s", "-p"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return 0
    return parse_lxc_info(out)


def ct_group_gid(P, pid, host_gid):
    """The container's gid for a node: the group of the same NAME as on the host (input, audio)."""
    name = None
    for line in rd("/etc/group").splitlines():
        f = line.split(":")
        if len(f) > 2 and f[2] == str(host_gid):
            name = f[0]
    if name is None:
        return host_gid
    for line in rd(os.path.join(P.proc, str(pid), "root/etc/group")).splitlines():
        f = line.split(":")
        if len(f) > 2 and f[0] == name:
            return int(f[2])
    return 0 if host_gid == 0 else host_gid


def murmur2(data, seed=0):
    """MurmurHash2 (32 bit), the hash libudev puts into its netlink header for the BPF filters."""
    m, r = 0x5bd1e995, 24
    h = (seed ^ len(data)) & 0xffffffff
    n = len(data) // 4
    for i in range(n):
        k = struct.unpack_from("<I", data, i * 4)[0]
        k = (k * m) & 0xffffffff
        k ^= k >> r
        k = (k * m) & 0xffffffff
        h = (h * m) & 0xffffffff
        h ^= k
    tail = data[n * 4:]
    if len(tail) >= 3:
        h ^= tail[2] << 16
    if len(tail) >= 2:
        h ^= tail[1] << 8
    if len(tail) >= 1:
        h ^= tail[0]
        h = (h * m) & 0xffffffff
    h ^= h >> 13
    h = (h * m) & 0xffffffff
    h ^= h >> 15
    return h


def udev_message(props):
    """A libudev netlink message ("libudev" header + KEY=VALUE\\0 ...), as udevd broadcasts it."""
    body = b"".join(("%s=%s" % kv).encode() + b"\0" for kv in props.items())
    sub = props.get("SUBSYSTEM", "").encode()
    devtype = props.get("DEVTYPE", "").encode()
    hdr_size = 40
    # magic, hashes and the tag bloom are big-endian (the receivers' BPF filter loads them so),
    # the sizes host order; an all-ones bloom passes every tag filter
    hdr = b"libudev\0" + struct.pack("!I", 0xfeedcafe) + struct.pack("=III", hdr_size, hdr_size, len(body)) \
        + struct.pack("!IIII", murmur2(sub), murmur2(devtype) if devtype else 0, 0xffffffff, 0xffffffff)
    return hdr + body


INJECT_PY = r"""
import socket, sys
msg = sys.stdin.buffer.read()
s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 15)   # NETLINK_KOBJECT_UEVENT
s.bind((0, 0))
s.sendto(msg, (0, 2))                                       # group 2: udev
"""


class Applier:
    """Adds/removes nodes in one running container and remembers what it added."""

    def __init__(self, P, ct, dry=False):
        self.P, self.ct, self.dry = P, ct, dry
        self.pid = 0
        self.applied = {}      # (dev, major, minor) -> Node
        self.cards = set()     # sysfs paths of the ALSA cards announced in the container
        self.nv = {}           # NVIDIA device name -> major allowed whole (NV_DYNAMIC)
        self.seq = int(time.time()) << 16
        self.unready = {}      # node key -> monotonic time its add first found no udev record of the host
        self.wait_udev = not dry

    def root(self, rel):
        return os.path.join(self.P.proc, str(self.pid), "root", rel.lstrip("/"))

    def _run(self, *argv):
        if self.dry:
            log("dry:", " ".join(argv))
            return True
        r = subprocess.run(list(argv), capture_output=True, text=True, timeout=15)
        if r.returncode:
            log("%s: rc %d %s" % (" ".join(argv), r.returncode, (r.stderr or r.stdout).strip()[:200]))
        return r.returncode == 0

    def _uevent(self, action, nd):
        props = {"ACTION": action, "SUBSYSTEM": nd.cls if nd.cls != "usb" else "usb",
                 "DEVNAME": nd.dev, "MAJOR": str(nd.major), "MINOR": str(nd.minor)}
        hp = os.path.join(self.P.sys, "dev/char/%d:%d" % (nd.major, nd.minor))
        if os.path.exists(hp):
            props["DEVPATH"] = os.path.realpath(hp)[len(self.P.sys):]
            for line in rd(os.path.join(hp, "uevent")).splitlines():
                if line.startswith("DEVTYPE="):
                    props["DEVTYPE"] = line[8:]
        if "DEVPATH" not in props:
            return
        if action == "add":
            props.update(udev_props(self.P, "%d:%d" % (nd.major, nd.minor)))
        self._send(props)

    def _send(self, props):
        self.seq += 1
        props["SEQNUM"] = str(self.seq)
        props["USEC_INITIALIZED"] = str(int(time.monotonic() * 1e6))
        if self.dry:
            return
        # libudev/sd-device switch a monitor of the udev group off when /run/udev/control is
        # missing and /dev is no devtmpfs -- both true in a container without udevd
        ctl = self.root("/run/udev/control")
        try:
            if not os.path.lexists(ctl):
                os.makedirs(os.path.dirname(ctl), exist_ok=True)
                open(ctl, "a").close()
        except OSError as e:
            log("%s in CT %s: %s" % (ctl, self.ct, e))
        try:
            subprocess.run(["nsenter", "--net=/proc/%d/ns/net" % self.pid, sys.executable, "-c", INJECT_PY],
                           input=udev_message(props), capture_output=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired) as e:
            log("uevent into CT %s: %s" % (self.ct, e))

    def _udev_db(self, nd, add):
        self._udev_record("c%d:%d" % (nd.major, nd.minor), add)

    def _udev_record(self, name, add):
        dst = self.root("/run/udev/data/" + name)
        if self.dry:
            return
        try:
            if add:
                src = os.path.join(self.P.udev, name)
                if os.path.exists(src):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copyfile(src, dst)
            elif os.path.exists(dst):
                os.unlink(dst)
        except OSError as e:
            log("udev record %s in CT %s: %s" % (name, self.ct, e))

    def _node_in_ct(self, nd):
        try:
            st = os.lstat(self.root(nd.dev))
        except OSError:
            return False
        return stat.S_ISCHR(st.st_mode) and os.major(st.st_rdev) == nd.major and os.minor(st.st_rdev) == nd.minor

    def add(self, nd, now=None):
        # the kernel's uevent rescans before the host's udevd has written the node's record: an event
        # node announced without it carries no ID_INPUT_* and libinput (gamescope's) skips it for
        # good (seen with a window client's keyboard). udevd writes the record by rename, so it
        # is complete once it exists; its own event then rescans, the 1 s apply retries too
        if self.wait_udev and nd.dev.startswith("/dev/input/event") and \
           not os.path.exists(os.path.join(self.P.udev, "c%d:%d" % (nd.major, nd.minor))):
            now = time.monotonic() if now is None else now
            t0 = self.unready.setdefault(nd.key(), now)
            if now - t0 < UDEV_WAIT_S:
                return False
            log("CT %s: %s has no udev record of the host after %.0f s: added without one, libinput "
                "may not take it" % (self.ct, nd.dev, now - t0))
        self.unready.pop(nd.key(), None)
        if not self._run("lxc-device", "-n", str(self.ct), "add", nd.dev):
            # already there (a seatmgr restart): keep the node, make sure it is allowed
            if self.dry or not self._node_in_ct(nd) or \
               not self._run("lxc-cgroup", "-n", str(self.ct), "devices.allow", "c %d:%d rwm" % (nd.major, nd.minor)):
                return False
        if not self.dry:
            p = self.root(nd.dev)
            try:
                os.chown(p, 0, ct_group_gid(self.P, self.pid, nd.gid))
                os.chmod(p, nd.mode)
            except OSError as e:
                log("chown %s in CT %s: %s" % (nd.dev, self.ct, e))
        self._udev_db(nd, True)
        self._uevent("add", nd)
        self.applied[nd.key()] = nd
        log("CT %s + %s (%d:%d)" % (self.ct, nd.dev, nd.major, nd.minor))
        return True

    def perms(self, nd):
        """The host's node got another group / mode after it went into the container: udevd applies
        GROUP/MODE a moment after the kernel made the node (seen: a window pointer stayed root:root 0600,
        gamescope (the desktop user) got EACCES and libinput dropped it for good).
        The container's node follows, and remove + add tell libinput to open it again."""
        if not self.dry:
            p = self.root(nd.dev)
            try:
                os.chown(p, 0, ct_group_gid(self.P, self.pid, nd.gid))
                os.chmod(p, nd.mode)
            except OSError as e:
                log("chown %s in CT %s: %s" % (nd.dev, self.ct, e))
                return
        old = self.applied[nd.key()]
        self.applied[nd.key()] = nd
        if nd.dev.startswith("/dev/input/event"):
            self._uevent("remove", nd)
            self._uevent("add", nd)
        log("CT %s: %s now group %d mode %o (was %d %o), announced again" % (self.ct, nd.dev, nd.gid, nd.mode,
                                                                           old.gid, old.mode))

    def remove(self, nd):
        self._uevent("remove", nd)
        self._udev_db(nd, False)
        if not self.dry:
            p = self.root(nd.dev)
            try:
                st = os.lstat(p)
                if stat.S_ISCHR(st.st_mode) and os.major(st.st_rdev) == nd.major and os.minor(st.st_rdev) == nd.minor:
                    os.unlink(p)
            except OSError as e:
                if e.errno != errno.ENOENT:
                    log("rm %s in CT %s: %s" % (nd.dev, self.ct, e))
        # revoke the allow: the minor may come back as another seat's device
        self._run("lxc-cgroup", "-n", str(self.ct), "devices.deny", "c %d:%d rwm" % (nd.major, nd.minor))
        self.applied.pop(nd.key(), None)
        log("CT %s - %s (%d:%d)" % (self.ct, nd.dev, nd.major, nd.minor))

    def sweep(self, want, majors):
        """First sync with a running container: remove the nodes of the managed majors that a
        previous seatmgr left there and that are not wanted any more."""
        if self.dry:
            return
        for d in ("/dev/input", "/dev/snd", "/dev", "/dev/bus/usb"):
            top = self.root(d)
            for base, dirs, files in os.walk(top):
                if d == "/dev" and base != top:
                    break                                   # only the hidraw nodes right in /dev
                for f in files:
                    p = os.path.join(base, f)
                    try:
                        st = os.lstat(p)
                    except OSError:
                        continue
                    if not stat.S_ISCHR(st.st_mode) or os.major(st.st_rdev) not in majors:
                        continue
                    dev = "/" + os.path.relpath(p, self.root("/"))
                    k = (dev, os.major(st.st_rdev), os.minor(st.st_rdev))
                    if k not in want:
                        cls = {13: "input", 116: "sound", 189: "usb"}.get(k[1], "hidraw")
                        self.remove(Node(dev, k[1], k[2], 0, 0, cls))

    def card_props(self, syspath, action):
        """The uevent of an ALSA card: "change" with the host's record (SOUND_INITIALIZED=1, ID_PATH, ...)
        after its nodes are there, the trigger for pipewire's ALSA monitor; "remove" before they go."""
        props = {"ACTION": action, "SUBSYSTEM": "sound", "DEVPATH": syspath[len(self.P.sys):]}
        if action != "remove":
            props.update(udev_record(self.P, "+sound:" + os.path.basename(syspath)))
        return props

    def add_card(self, syspath):
        self._udev_record("+sound:" + os.path.basename(syspath), True)
        self._send(self.card_props(syspath, "change"))
        self.cards.add(syspath)
        log("CT %s + sound card %s" % (self.ct, os.path.basename(syspath)))

    def remove_card(self, syspath):
        self._send(self.card_props(syspath, "remove"))
        self._udev_record("+sound:" + os.path.basename(syspath), False)
        self.cards.discard(syspath)
        log("CT %s - sound card %s" % (self.ct, os.path.basename(syspath)))

    def nvidia(self, majors):
        """Allow the current majors of NV_DYNAMIC whole, revoke the ones of an earlier pass that moved
        and the stale whole-major allows of the config (module docstring)."""
        want = nvidia_majors(majors)
        if want == self.nv:
            return
        for n, ma in want.items():
            if self.nv.get(n) != ma:
                self._run("lxc-cgroup", "-n", str(self.ct), "devices.allow", "c %d:* rwm" % ma)
                log("CT %s + %s major %d (whole)" % (self.ct, n, ma))
        gone = {ma for ma in self.nv.values() if ma not in want.values()}
        for ma in sorted(gone | stale_majors(self.P, self.ct, majors)):
            self._run("lxc-cgroup", "-n", str(self.ct), "devices.deny", "c %d:* rwm" % ma)
            log("CT %s - stale whole-major allow c %d:* (%s now; the container's next start binds the current ones)"
                % (self.ct, ma, "/".join(sorted(majors.get(ma, ()))) or "unregistered"))
        self.nv = want

    def sync(self, want, cards=()):
        """want: {(dev, major, minor): Node}, cards: sysfs paths of ALSA cards. Remove first, then add;
        a card is announced after its nodes and withdrawn before them."""
        for c in [c for c in self.cards if c not in cards]:
            self.remove_card(c)
        for k in [k for k in self.applied if k not in want]:
            self.remove(self.applied[k])
        for k in [k for k in self.unready if k not in want]:
            self.unready.pop(k)
        for k, nd in want.items():
            if k not in self.applied:
                self.add(nd)
            elif (nd.mode, nd.gid) != (self.applied[k].mode, self.applied[k].gid):
                self.perms(nd)
        for c in cards:
            if c not in self.cards:
                self.add_card(c)


# ----------------------------------------------------------------------------- the manager
# ----------------------------------------------------------------------------- card choice
class SeatRefused(Exception):
    """A seat cannot start on the card asked for (HTTP 409, the reason for the user)."""


DOCKER_CONTAINERS = "/var/lib/docker/containers"


def bus_pci(s):
    """nvidia-smi's "00000000:15:00.0" as the kernel's "0000:15:00.0"."""
    s = (s or "").strip().lower()
    return sg.norm_pci(s[-12:]) if len(s) >= 12 else sg.norm_pci(s)


def compute_apps(run=subprocess.run):
    """[{"pid", "pci", "name", "mib"}] of every process with a context on an NVIDIA GPU (any container)."""
    r = run(["nvidia-smi", "--query-compute-apps=pid,gpu_bus_id,process_name,used_memory",
             "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=20)
    if r.returncode:
        raise RuntimeError("nvidia-smi: %s" % (r.stderr or r.stdout or "exit %d" % r.returncode).strip()[:200])
    out = []
    for line in r.stdout.splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) >= 4 and f[0].isdigit():
            out.append({"pid": int(f[0]), "pci": bus_pci(f[1]), "name": f[2], "mib": int(f[3]) if f[3].isdigit() else None})
    return out


def node_holders(P, rdevs, me=None):
    """{pid: {rdev, ...}} of every process holding one of the char devices `rdevs` open (all containers:
    the host's /proc)."""
    me = os.getpid() if me is None else me
    out = {}
    for n in os.listdir(P.proc) if os.path.isdir(P.proc) else ():
        if not n.isdigit() or int(n) == me:
            continue
        d = os.path.join(P.proc, n, "fd")
        try:
            names = os.listdir(d)
        except OSError:
            continue
        hit = set()
        for f in names:
            try:
                st = os.stat(os.path.join(d, f))
            except OSError:
                continue
            if stat.S_ISCHR(st.st_mode) and st.st_rdev in rdevs:
                hit.add(st.st_rdev)
        if hit:
            out[int(n)] = hit
    return out


def docker_name(cid, root=DOCKER_CONTAINERS):
    try:
        with open(os.path.join(root, cid, "config.v2.json")) as fh:
            return json.load(fh).get("Name", "").lstrip("/") or cid[:12]
    except (OSError, ValueError):
        return cid[:12]


def proc_where(P, pid, docker_root=DOCKER_CONTAINERS):
    """(where, ct): "CT 200" / "docker <name>" / "host <unit>" for a process, ct its container or None."""
    lines = rd(os.path.join(P.proc, str(pid), "cgroup")).splitlines()
    unified = [l for l in lines if l.startswith("0::")] or lines[-1:]
    path = unified[0].split(":", 2)[-1] if unified else ""
    m = re.match(r"/lxc(?:\.payload)?[/.](\d+)(?:/|$)", path)
    if m:
        return "CT %s" % m.group(1), int(m.group(1))
    m = re.search(r"docker-([0-9a-f]{12,})\.scope", path)
    if m:
        return "docker %s" % docker_name(m.group(1), docker_root), None
    return "host %s" % (path.rstrip("/").rsplit("/", 1)[-1] or "/"), None


def gpuq_cards(url, timeout=3.0):
    """The optional GPU booking service's GET <url>/api/v1/cards (read only): [{"uuid", "pci_bus_id",
    "timeline": [{"state": "running", "id", "owner", "purpose", "end": {"local"}, "mib"}]}]."""
    with urllib.request.urlopen(url.rstrip("/") + "/api/v1/cards", timeout=timeout) as r:
        return json.load(r)


def short_model(model):
    """"NVIDIA GeForce RTX 5090" -> "RTX 5090"."""
    m = re.search(r"((RTX|GTX|RX|Arc)\s+)?[A-Z]?\d{3,4}\w*(\s+(Ti|SUPER|XT))?$", model or "")
    return m.group(0) if m else (model or "GPU")


class Manager:
    def __init__(self, conf_path, runtime_path, P=None, dry=False):
        self.conf_path, self.runtime_path = conf_path, runtime_path
        self.P = P or Paths()
        self.dry = dry
        self.lock = threading.RLock()
        self.conf = self.load_conf()
        self.runtime = self.load_runtime()
        self.virtual_path = os.path.join(os.path.dirname(runtime_path) or ".", "virtual.json")
        self.vowner = self.load_json(self.virtual_path)    # virtual device id -> {"ct", "fd", "t"}
        self.vpending = {}                                  # virtual device id -> why it has no owner yet
        self.devs = {}
        self.seen = set()
        self.activity = {}             # device id -> monotonic time of the last press/motion
        self.claims = {}               # seat -> {"until": monotonic, "result": id|None}
        self.claim_results = {}        # seat -> {"id", "t"}
        self.appliers = {}
        self.pids = {}
        self.no_pid = set()
        self.apply_lock = threading.Lock()
        self.dirty = threading.Event()
        self.reader_wake = threading.Event()
        self.first_scan = True
        self.console = ConsoleKeyboard(self.P)
        # grab guard (module docstring); the callables are the tests' seam
        self.withheld = {}             # device id -> {"kind": "idle"|"nograb", "gs": grabbing pids then, "why", "fds"}
        self.gs_seen = {}              # gamescope pid -> monotonic time seatmgr first saw it
        self.dev_seen = {}             # device id -> monotonic time it was first seen in a running seat
        self.free_since = {}           # device id -> monotonic time it was first seen ungrabbed
        self.probed = {}               # device id -> monotonic time of its last probe
        self.observed = set()          # devices the observing guard has named already
        self.gated = set()             # devices another holds already (rx / usb_host gate), named once
        self.hurry = threading.Event() # a new input node: rescan and guard at once
        self.grab_probe = lambda path: grab_probe(self.P, path)
        self.grab_hold = lambda path: grab_hold(self.P, path)
        self.gamescopes = lambda ct: gamescopes(self.P, ct)
        self.is_client = lambda path: node_is_client(self.P, path)
        self.fds_on = lambda pid, rdevs: proc_fds_on(self.P, pid, rdevs)
        # card choice (module docstring); the callables are the tests' seam
        self.GP = sg.Paths(sys_=self.P.sys, dev=self.P.dev, proc=self.P.proc, pve=self.P.pve, conf=conf_path)
        self.run_cmd = subprocess.run
        self.compute_apps = lambda: compute_apps(self.run_cmd)
        self.node_holders = lambda rdevs: node_holders(self.P, rdevs)
        self.gpuq_cards = lambda: gpuq_cards(self.conf.get("gpuq", ""))
        self.docker_root = DOCKER_CONTAINERS
        self.switch_lock = threading.Lock()
        self.irq_running = None        # the running seat containers at the last IRQ pass
        self.gpus_cache = (0.0, None)

    # -- persistence
    def load_conf(self):
        c = json.loads(json.dumps(DEFAULT_CONF))
        try:
            with open(self.conf_path) as fh:
                c.update(json.load(fh))
        except FileNotFoundError:
            pass
        c["seats"] = {str(k): v for k, v in c.get("seats", {}).items()}
        c.setdefault("rules", [])
        return c

    def save_conf(self):
        tmp = self.conf_path + ".tmp"
        os.makedirs(os.path.dirname(self.conf_path) or ".", exist_ok=True)
        with open(tmp, "w") as fh:
            json.dump(self.conf, fh, indent=1)
            fh.write("\n")
        os.replace(tmp, self.conf_path)

    def load_runtime(self):
        try:
            with open(self.runtime_path) as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {}

    @staticmethod
    def load_json(path):
        try:
            with open(path) as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {}

    def save_virtual(self):
        os.makedirs(os.path.dirname(self.virtual_path) or ".", exist_ok=True)
        with open(self.virtual_path + ".tmp", "w") as fh:
            json.dump(self.vowner, fh)
        os.replace(self.virtual_path + ".tmp", self.virtual_path)

    def resolve_virtual(self, devs):
        """Match new uinput/uhid devices to the container whose process created them (module docstring)."""
        changed = False
        for i in [i for i in self.vowner if i not in devs]:
            del self.vowner[i]                          # closed: its fd is free again
            changed = True
        cts = [int(s["ct"]) for s in self.conf["seats"].values() if ct_running(self.P, s["ct"])]
        pending = {}
        for i, dv in devs.items():
            if dv.kind == "virtual" and i not in self.vowner:
                pending.setdefault(dv.virtual, []).append(i)
        self.vpending = {}
        for src, ids in pending.items():
            holders = fd_holders(self.P, cts, "/dev/" + src)
            taken = {o.get("fd") for o in self.vowner.values()}
            free = {k: ct for k, ct in holders.items() if k not in taken}
            for i in sorted(ids, key=lambda x: [int(n) for n in re.findall(r"\d+", x)] or [0]):
                owners = set(free.values())
                if len(owners) == 1:
                    ct = owners.pop()
                    key = sorted(k for k, c in free.items() if c == ct)[0]
                    del free[key]
                    self.vowner[i] = {"ct": ct, "fd": key, "t": time.time()}
                    changed = True
                    log("%s (%s) created in CT %s (fd %s)" % (i, devs[i].name, ct, key))
                else:
                    self.vpending[i] = ("open %s in several containers (%s)" % (src, ", ".join(map(str, sorted(owners))))
                                        if owners else "no seat container holds %s open" % src)
        if changed:
            self.save_virtual()

    def save_runtime(self):
        os.makedirs(os.path.dirname(self.runtime_path) or ".", exist_ok=True)
        tmp = self.runtime_path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(self.runtime, fh)
        os.replace(tmp, self.runtime_path)

    # -- state
    def monitor_seats(self):
        return [int(n) for n, s in self.conf["seats"].items() if s.get("gpu") and gpu_has_monitor(self.P, s["gpu"])]

    def rescan(self):
        devs = scan(self.P)
        with self.lock:
            self.resolve_virtual(devs)
            gone = [i for i in self.runtime if i not in devs]
            for i in gone:
                del self.runtime[i]                    # runtime choices last until unplug
            # a new device without a rule stays with the host console (no default seat any more)
            self.seen = set(devs)
            self.devs = devs
            if gone or devs:
                self.save_runtime()
        self.apply()
        self.reader_wake.set()

    def seat_of(self, dv):
        if dv.kind == "virtual":
            o = self.vowner.get(dv.id)
            seat = next((int(n) for n, s in self.conf["seats"].items() if o and int(s["ct"]) == o["ct"]), None)
            return (seat, "created in this seat") if seat is not None else (None, "virtual, creator not known")
        return effective(self.conf, self.runtime, dv)

    def apply(self):
        with self.apply_lock:                         # API calls and the main loop: one at a time
            self._apply()

    def managed_majors(self):
        majors = {13, 116, 189}                       # input, sound, usb_device
        for line in rd(os.path.join(self.P.proc, "devices")).splitlines():
            f = line.split()
            if len(f) == 2 and f[1] == "hidraw" and f[0].isdigit():
                majors.add(int(f[0]))
        return majors

    def _apply(self):
        with self.lock:
            want = {}
            for n, s in self.conf["seats"].items():
                want[int(s["ct"])] = {}
            sound_cts = set()
            want_cards = {}
            for dv in self.devs.values():
                seat, _ = self.seat_of(dv)
                if seat is None or str(seat) not in self.conf["seats"] or (not dv.assignable() and dv.kind != "virtual"):
                    continue
                ct = int(self.conf["seats"][str(seat)]["ct"])
                for nd in dv.nodes:
                    want[ct][nd.key()] = nd
                if dv.has_sound:
                    sound_cts.add(ct)
                    want_cards.setdefault(ct, set()).update(dv.cards)
            # ALSA's shared nodes go with any sound card
            for name in ("snd/timer", "snd/seq"):
                p = os.path.join(self.P.dev, name)
                try:
                    st = self.P.stat(p)
                except OSError:
                    continue
                nd = Node("/dev/" + name, os.major(st.st_rdev), os.minor(st.st_rdev), stat.S_IMODE(st.st_mode), st.st_gid, "sound")
                for ct in sound_cts:
                    want[ct][nd.key()] = nd
        majors = char_majors(self.P)
        for ct, nodes in want.items():
            if not ct_running(self.P, ct):
                self.appliers.pop(ct, None)
                self.pids.pop(ct, None)
                continue
            ap = self.appliers.get(ct)
            pid = ct_init_pid(ct) if self.pids.get(ct) is None or not os.path.exists("/proc/%d" % self.pids[ct]) else self.pids[ct]
            if not pid:
                if ct not in self.no_pid:
                    log("CT %s runs but lxc-info names no init pid yet" % ct)
                    self.no_pid.add(ct)
                continue
            self.no_pid.discard(ct)
            if ap is None or ap.pid != pid:
                ap = self.appliers[ct] = Applier(self.P, ct, self.dry)    # a (re)started container: all anew
                ap.pid = pid
                self.pids[ct] = pid
                log("CT %s running (init %d): applying %d nodes" % (ct, pid, len(nodes)))
                ap.sweep(nodes, self.managed_majors())
            ap.nvidia(majors)
            ap.sync(nodes, want_cards.get(ct, set()))
        if not self.dry:
            self.irq_pass({ct for ct in want if ct_running(self.P, ct)})
            self.console_pass()                       # after container starts and stops too

    def irq_pass(self, running):
        """the seat cards' IRQs follow the running seats (rdx_seat_gpu "IRQs"), at every start and stop seen"""
        if running == self.irq_running:
            return
        self.irq_running = running
        try:
            for line in sg.irq_sync(self.GP):
                log("irq: " + line)
        except Exception as e:                    # noqa: BLE001 -- never stop the device handling
            log("irq: %s: %s" % (type(e).__name__, e))

    # -- API actions
    def assign(self, did, seat, remember=None):
        with self.lock:
            dv = self.devs.get(did)
            if dv is None:
                raise ValueError("no device %r" % did)
            if dv.kind == "virtual":
                raise ValueError("%s was created inside a seat container and stays with it" % (dv.name or did))
            if str(seat) == HOST:
                seat = None                            # the host console, explicitly
            if seat is not None and str(seat) not in self.conf["seats"]:
                raise ValueError("no seat %r" % seat)
            if seat is not None and not dv.assignable():
                raise ValueError("%s is used by the host (%s) and cannot go to a seat" % (dv.name or did, ", ".join(sorted(dv.host_drivers))))
            seat = int(seat) if seat is not None else None
            if remember in ("port", "device"):
                self._drop_rules(dv)
                match = dv.path if remember == "port" else dv.devid
                if not match:
                    raise ValueError("device %s has no %s to remember" % (did, remember))
                self.conf["rules"].append({"kind": remember, "match": match, "seat": seat if seat is not None else HOST,
                                           "name": dv.name})
                self.save_conf()
                self.runtime.pop(did, None)
            elif remember in (None, "", "none"):
                self.runtime[did] = {"seat": seat, "how": "manual" if seat is not None else "host console (manual)",
                                     "t": time.time()}
            else:
                raise ValueError("remember must be port, device or null")
            self.save_runtime()
        self.apply()
        return self.state_device(did)

    def _drop_rules(self, dv):
        self.conf["rules"] = [r for r in self.conf["rules"] if not rule_matches(r, dv)]

    def forget(self, did):
        with self.lock:
            dv = self.devs.get(did)
            if dv is None:
                raise ValueError("no device %r" % did)
            if dv.kind == "virtual":
                raise ValueError("%s was created inside a seat container and stays with it" % (dv.name or did))
            self._drop_rules(dv)
            self.save_conf()
            self.runtime.pop(did, None)
            self.save_runtime()
        self.apply()
        return self.state_device(did)

    def claim(self, seat, seconds=CLAIM_S):
        if str(seat) not in self.conf["seats"]:
            raise ValueError("no seat %r" % seat)
        seconds = max(1.0, min(float(seconds), 120.0))
        with self.lock:
            self.claims[int(seat)] = {"until": time.monotonic() + seconds}
            self.claim_results.pop(int(seat), None)
        self.reader_wake.set()
        return {"seat": int(seat), "seconds": seconds}

    def cancel_claim(self, seat):
        with self.lock:
            self.claims.pop(int(seat), None)
        return {"seat": int(seat)}

    def on_input(self, did, press):
        """An event on one of the device's input nodes (reader thread)."""
        now = time.monotonic()
        winner = None
        with self.lock:
            self.activity[did] = now
            if not press:
                return
            dv = self.devs.get(did)
            if dv is None or dv.kind == "virtual" or self.seat_of(dv)[0] is not None:
                return
            live = [(c["until"], s) for s, c in self.claims.items() if c["until"] > now]
            if not live:
                return
            _, seat = min(live)                       # the oldest open claim
            self.claims.pop(seat, None)
            self.runtime[did] = {"seat": seat, "how": "claim", "t": time.time()}
            self.claim_results[seat] = {"id": did, "name": dv.name, "t": time.time()}
            self.save_runtime()
            winner = seat
        log("claim: %s -> seat %s" % (did, winner))
        self.apply()

    # -- views
    def state_device(self, did):
        dv = self.devs.get(did)
        if dv is None:
            return None
        seat, how = self.seat_of(dv)
        j = dv.json()
        age = time.monotonic() - self.activity[did] if did in self.activity else None
        w = self.withheld.get(did)
        j.update(withheld=w["why"] if w else None)
        j.update(seat=seat, how=how, active=age is not None and age < BLINK_S,
                 last_input_s=round(age, 1) if age is not None else None,
                 rules=[r for r in self.conf["rules"] if rule_matches(r, dv)])
        return j

    def _let_go(self, did, why):
        for fd in self.withheld.pop(did, {}).get("fds", []):
            try:
                os.close(fd)
            except OSError:
                pass
        self.free_since.pop(did, None)
        log("grab guard: %s let go (%s)" % (did, why))

    def _opens(self, pids, dv, nodes):
        rdevs = {os.makedev(n.major, n.minor) for n in dv.nodes if n.dev in nodes}
        return frozenset(o for pid in pids for o in self.fds_on(pid, rdevs))

    def _hold(self, did, dv, nodes, kind, gs, why, enforce):
        if not enforce:
            if did not in self.observed:           # K_OFF protects the consoles: only say it, once
                self.observed.add(did)
                log("grab guard (observing: the consoles' keyboards are off): %s (%s) %s; with the consoles "
                    "on seatmgr would hold it" % (did, dv.name, why))
            return
        fds = [fd for fd in (self.grab_hold(n) for n in nodes) if fd is not None]
        if not fds:                                # EBUSY: its creator's gate holds it; again next pass
            if did not in self.gated:
                self.gated.add(did)
                log("grab guard: %s (%s) %s: already grabbed by another (its creator's gate)" % (did, dv.name, why))
            return
        self.gated.discard(did)
        self.withheld[did] = {"kind": kind, "gs": gs, "why": why, "fds": fds,
                              "opens": self._opens(gs, dv, nodes) if kind == "nograb" else frozenset()}
        log("grab guard: %s (%s) %s: held by seatmgr, its keys go nowhere" % (did, dv.name, why))

    def guard_pass(self, now=None):
        """the grab guard, GUARD_TICK_S apart (module docstring)"""
        now = time.monotonic() if now is None else now
        enforce = bool(self.conf.get("host_console_keyboard") or self.conf.get("grab_guard") == "enforce")
        with self.lock:
            seat_ct = {int(n): int(s["ct"]) for n, s in self.conf["seats"].items()}
            procs = {ct: self.gamescopes(ct) for ct in seat_ct.values() if ct_running(self.P, ct)}
            alive = {pid for gs in procs.values() for pid in gs}
            for pid in alive:
                self.gs_seen.setdefault(pid, now)
            for pid in [p for p in self.gs_seen if p not in alive]:
                del self.gs_seen[pid]
            watched = set()
            for did, dv in self.devs.items():
                ct = seat_ct.get(self.seat_of(dv)[0])
                nodes = [n for n in dv.inputs if node_is_keyboard(self.P, n) and not self._joystick(dv, n)]
                if ct not in procs or not nodes:
                    continue
                watched.add(did)
                self.dev_seen.setdefault(did, now)
                client = any(self.is_client(n) for n in nodes)
                grabbers = frozenset(pid for pid, g in procs[ct].items()
                                     if g["grab"] and (g["backend"] == "drm" or (g["backend"] == "rdx" and client)))
                held = self.withheld.get(did)
                if not grabbers:
                    if held and held["kind"] == "nograb":
                        self._let_go(did, "its gamescope is gone")
                        held = None
                    if not held:                        # whatever its name or creator (docstring)
                        self._hold(did, dv, nodes, "idle", grabbers, "no gamescope of seat %s reads it" %
                                   self.seat_of(dv)[0], enforce)
                    continue
                if held and (held["kind"] == "idle" or held["gs"] != grabbers):
                    self._let_go(did, "a gamescope that grabs it runs now")
                    held = None
                elif held and self._opens(grabbers, dv, nodes) - held["opens"]:
                    # it opens the device late (libinput hotplug, a resume): its grab (0.5 s of EBUSY
                    # retries) must find it free; probed again PROBE_EVERY_S on
                    self._let_go(did, "the gamescope of seat %s opens it now, its grab takes it" % self.seat_of(dv)[0])
                    self.probed[did] = now
                    continue
                # the grace counts from the later of: that gamescope appeared, the device came (rx makes a
                # session's devices before gamescope starts; a resume or hotplug brings them later)
                since = max(max(self.gs_seen[p] for p in grabbers), self.dev_seen.setdefault(did, now))
                if held or now - since < GRAB_GRACE_S:
                    continue
                if now - self.probed.get(did, -1e9) < PROBE_EVERY_S:
                    continue
                self.probed[did] = now
                if all(self.grab_probe(n) != "free" for n in nodes):
                    self.free_since.pop(did, None)
                    self.observed.discard(did)
                    continue
                if now - self.free_since.setdefault(did, now) < GRAB_FREE_S:
                    continue
                self._hold(did, dv, nodes, "nograb", grabbers, "not grabbed by the gamescope of seat %s (%.0f s "
                           "after it and the device were there)" % (self.seat_of(dv)[0], now - since), enforce)
            for did in [d for d in self.withheld if d not in watched]:
                self._let_go(did, "no longer a keyboard of a running seat")
            for d in [d for d in list(self.free_since) + list(self.probed) + list(self.dev_seen) if d not in watched]:
                self.free_since.pop(d, None)
                self.probed.pop(d, None)
                self.dev_seen.pop(d, None)

    def _joystick(self, dv, node):
        nd = next((n for n in dv.nodes if n.dev == node), None)
        return bool(nd and udev_props(self.P, "%d:%d" % (nd.major, nd.minor)).get("ID_INPUT_JOYSTICK") == "1")

    def guard_thread(self):
        last, seen = 0.0, None
        while True:
            try:
                with self.lock:
                    cts = [int(s["ct"]) for s in self.conf["seats"].values()]
                now_gs = {ct: frozenset(self.gamescopes(ct)) for ct in cts if ct_running(self.P, ct)}
                if now_gs != seen or time.monotonic() - last >= GUARD_TICK_S:
                    seen = now_gs
                    self.guard_pass()
                    last = time.monotonic()
            except Exception as e:                # noqa: BLE001 -- the guard must outlive one bad pass
                log("grab guard: %s: %s" % (type(e).__name__, e))
            time.sleep(GS_WATCH_S)

    def console_pass(self):
        """the host's VTs read no keyboard, unless "host_console_keyboard" (module docstring)"""
        try:
            if self.conf.get("host_console_keyboard"):
                self.console.restore()
            else:
                self.console.enforce()
        except Exception as e:                    # noqa: BLE001 -- never stop the device handling
            log("host consoles: %s: %s" % (type(e).__name__, e))

    def state(self):
        now = time.monotonic()
        with self.lock:
            mon = set(self.monitor_seats())
            seats = []
            for n, s in sorted(self.conf["seats"].items(), key=lambda kv: int(kv[0])):
                c = self.claims.get(int(n))
                seats.append({"seat": int(n), "ct": int(s["ct"]), "name": s.get("name", "seat %s" % n),
                              "gpu": s.get("gpu", ""), "sound": default_sound_path(s),
                              "running": ct_running(self.P, s["ct"]), "monitor": int(n) in mon,
                              "claim_s": round(c["until"] - now, 1) if c and c["until"] > now else None,
                              "claimed": self.claim_results.get(int(n))})
            # a virtual device whose creator is not known yet is no "unassigned" device: listed apart
            devices = [self.state_device(i) for i in sorted(self.devs, key=lambda i: (self.devs[i].kind, i))
                       if not (self.devs[i].kind == "virtual" and i in self.vpending)]
            pending = [{"id": i, "name": self.devs[i].name, "why": w} for i, w in self.vpending.items() if i in self.devs]
            return {"seats": seats, "devices": devices, "virtual_pending": pending, "rules": self.conf["rules"], "blink_s": BLINK_S,
                    "host_console_keyboard": bool(self.conf.get("host_console_keyboard")),
                    "no_power_switch_rule": os.path.exists(NO_POWER_RULE)}

    def seat_state(self, n):
        st = self.state()
        seat = next((s for s in st["seats"] if s["seat"] == int(n)), None)
        if seat is None:
            raise ValueError("no seat %r" % n)
        return {"seat": seat, "devices": [d for d in st["devices"] if d["seat"] == int(n)]}

    # -- card choice (module docstring "Card choice")
    def _card_users(self, seats, cards):
        """{pci: {"seat": (ct, seat) running with it or None, "procs": [...], "monitors": [...], "gpuq": [...]}}
        plus the notes of what could not be asked."""
        notes = []
        try:
            apps = self.compute_apps()
        except Exception as e:                    # noqa: BLE001 -- the fd scan still sees every user
            apps = []
            notes.append("nvidia-smi not asked (%s)" % e)
        nv_all = {os.makedev(sg.NV_MAJOR, g["minor"]) for g in cards.values()}
        rdevs = {}
        for pci, g in cards.items():
            rdevs[pci] = {os.makedev(sg.NV_MAJOR, g["minor"]), os.makedev(sg.DRM_MAJOR, g["card_minor"]),
                          os.makedev(sg.DRM_MAJOR, g["render_minor"])}
        held = self.node_holders(set().union(*rdevs.values()) if rdevs else set())
        ctx = {a["pid"] for a in apps}
        gq = None
        if self.conf.get("gpuq"):
            try:
                gq = self.gpuq_cards()
            except Exception as e:                # noqa: BLE001 -- the booking service only plans; the hardware checks stand
                notes.append("GPU booking service not asked (%s)" % e)
        out = {}
        for pci, g in cards.items():
            u = {"seat": None, "procs": [], "monitors": [], "gpuq": []}
            for ct, s in sorted(seats.items()):
                if s["gpu"] == pci and ct_running(self.P, ct):
                    u["seat"] = (ct, s)
                    break
            pids = {a["pid"]: a["name"] for a in apps if a["pci"] == pci}     # contexts first, then open nodes
            for pid, have in sorted(held.items()):
                if not have & rdevs[pci]:
                    continue
                name = rd(os.path.join(self.P.proc, str(pid), "comm")) or "?"
                # an NVML monitor (nvtop, an exporter, a dashboard) opens every card's /dev/nvidiaN, no DRM node
                if pid not in ctx and have <= nv_all and nv_all <= have and len(cards) > 1:
                    u["monitors"].append({"pid": pid, "name": name})
                    continue
                pids.setdefault(pid, name)
            for pid, name in pids.items():
                where, ct = proc_where(self.P, pid, self.docker_root)
                u["procs"].append({"pid": pid, "name": name, "where": where, "ct": ct})
            for c in gq or ():
                if c.get("uuid", "").lower() != g["uuid"].lower() and bus_pci(c.get("pci_bus_id", "")) != pci:
                    continue
                for b in c.get("timeline") or ():
                    if b.get("state") == "running":
                        u["gpuq"].append({"id": b.get("id"), "owner": b.get("owner", ""), "purpose": b.get("purpose", ""),
                                          "until": (b.get("end") or {}).get("local", ""), "mib": b.get("mib")})
            out[pci] = u
        return out, notes

    def _foreign(self, u, ct=None, booking=None):
        """(reason or "", status) of a card for seat container `ct`: what makes it not free for that seat."""
        mine = ({u["seat"][0]} if u["seat"] else set()) | ({ct} if ct else set())
        procs = [p for p in u["procs"] if p["ct"] is None or p["ct"] not in mine]
        own = tuple(self.conf.get("gpuq_seat_owners") or ())
        books = [b for b in u["gpuq"] if b["id"] != booking and not (own and str(b["owner"]).startswith(own))]
        if u["seat"] and u["seat"][0] != ct:
            o, s = u["seat"]
            return "it is seat %s's (CT %d runs with it)" % (s["seat"], o), "seat"
        if procs:
            names = [n.lower() for n in self.conf.get("llm_names") or ()]
            llm = any(n in p["name"].lower() for p in procs for n in names)
            shown = ", ".join("%s (pid %d, %s)" % (p["name"], p["pid"], p["where"]) for p in procs[:4])
            more = " and %d more" % (len(procs) - 4) if len(procs) > 4 else ""
            return ("it is used by an LLM server: " if llm else "it is used by other programs: ") + shown + more, "llm" if llm else "busy"
        if books:
            return "booked in the GPU booking service: " + "; ".join("%s (%s) until %s" % (b["owner"], b["purpose"][:80], b["until"])
                                                       for b in books), "booked"
        return "", "free"

    def gpus_state(self, max_age=2.0):
        """GET /api/gpus: every card with its state, every seat with its card (cached for max_age s)."""
        t, cached = self.gpus_cache
        if cached is not None and time.monotonic() - t < max_age:
            return cached
        seats = sg.load_seats(self.GP)
        cards, out = {}, []
        for pci in sg.nvidia_cards(self.GP):
            try:
                cards[pci] = sg.resolve(self.GP, pci)
            except sg.Refuse as e:
                out.append({"pci": pci, "status": "unusable", "why": str(e)})
        users, notes = self._card_users(seats, cards)
        pins = sg.load_pins(self.GP)["cards"]
        for pci, g in sorted(cards.items()):
            u = users[pci]
            why, status = self._foreign(u)
            p = pins.get(pci)
            if p and p.get("uuid", "").lower() != g["uuid"].lower():
                why, status = "the card at this address is not the one pinned there (%s): a swap?" % p.get("model", p["uuid"]), "unusable"
            out.append({"pci": pci, "model": g["model"], "short": short_model(g["model"]), "uuid": g["uuid"],
                        "status": status, "why": why, "seat": int(u["seat"][1]["seat"]) if u["seat"] else None,
                        "procs": [{k: p[k] for k in ("pid", "name", "where")} for p in u["procs"]],
                        "monitors": [m["name"] for m in u["monitors"]], "gpuq": u["gpuq"]})
        out.sort(key=lambda c: c["pci"])
        seat_list = [{"seat": int(s["seat"]), "ct": ct, "name": s["name"], "running": ct_running(self.P, ct),
                      "gpu": s["gpu"], "gpus": sg.allowed_cards(self.GP, s)} for ct, s in sorted(seats.items())]
        res = {"cards": out, "seats": seat_list, "notes": notes}
        self.gpus_cache = (time.monotonic(), res)
        return res

    def seat_busy(self, ct, seat):
        """"" if nobody uses the seat now, else what runs there (a stream, a paused app, an app on its monitor)."""
        port = 47990 + 100 * int(seat["seat"])
        p = sg.hostd_conf_path(self.GP, ct, seat)
        try:
            with open(p) as fh:
                port = int(json.load(fh).get("web_port") or port)
        except (OSError, ValueError, TypeError):
            pass
        r = self.run_cmd(["pct", "exec", str(ct), "--", "curl", "-s", "-m", "3", "http://127.0.0.1:%d/api/state" % port],
                         capture_output=True, text=True, timeout=20)
        if r.returncode == 7:                    # curl: connection refused, no hostd: nobody can stream from it
            return ""
        try:
            st = json.loads(r.stdout)
        except ValueError:
            return "cannot ask its hostd (pct exec / curl exit %d %s)" % (r.returncode, (r.stderr or "").strip()[:120])
        s = st.get("session") or {}
        why = []
        if s.get("state", "idle") != "idle":
            why.append("a stream of %s runs (to %s)" % (s.get("app") or "an app", s.get("client") or "a client"))
        if st.get("suspended"):
            why.append("%s is paused there" % (st["suspended"].get("app") or "an app"))
        if (st.get("local") or {}).get("running"):
            why.append("%s runs on its monitor" % (st["local"].get("app") or "an app"))
        return "; ".join(why)

    def _pct(self, *args, timeout=180):
        r = self.run_cmd(["pct"] + [str(a) for a in args], capture_output=True, text=True, timeout=timeout)
        return r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()

    def seat_start(self, n, pci, force=False, booking=None):
        """POST /api/seat/start: seat n on the card at pci (module docstring "Card choice")."""
        if not self.switch_lock.acquire(blocking=False):
            raise SeatRefused("another seat is being switched right now, try again in a moment")
        try:
            return self._seat_start(str(n), sg.norm_pci(str(pci or "")), bool(force), booking)
        finally:
            self.switch_lock.release()
            self.gpus_cache = (0.0, None)

    def _seat_start(self, n, pci, force, booking):
        try:
            seats = sg.load_seats(self.GP)
            ct = next((c for c, s in seats.items() if s["seat"] == n), None)
            if ct is None:
                raise ValueError("no seat %r" % n)
            seat = seats[ct]
            if not sg.PCI_RE.fullmatch(pci):
                raise ValueError("\"gpu\" must be a PCI address")
            if pci not in sg.allowed_cards(self.GP, seat):
                raise SeatRefused("seat %s may not take %s (its cards: %s)" % (n, pci, ", ".join(sg.allowed_cards(self.GP, seat))))
            g = sg.resolve(self.GP, pci)
            sg.check_pin(self.GP, g, sg.load_pins(self.GP))
            cards = {}
            for c in sg.nvidia_cards(self.GP):
                try:
                    cards[c] = sg.resolve(self.GP, c)
                except sg.Refuse:
                    pass
            cards[pci] = g
        except sg.Refuse as e:
            raise SeatRefused("%s (%s) cannot be bound: %s" % (pci, "seat %s" % n, e))
        users, notes = self._card_users(seats, cards)
        why, _ = self._foreign(users[pci], ct, booking)
        label = "%s (%s)" % (short_model(g["model"]), pci)
        if why:
            raise SeatRefused("%s: %s" % (label, why))
        running = ct_running(self.P, ct)
        if running and not force:
            busy = self.seat_busy(ct, seat)
            if busy:
                raise SeatRefused("seat %s is in use: %s (end it first, or force)" % (n, busy))
        old = seat["gpu"]
        log("seat %s (CT %d): start on %s%s%s" % (n, ct, label, ", was %s" % old if old != pci else "",
                                                  ", shutting it down first" if running else ""))
        steps = []
        if running:
            rc, out = self._pct("shutdown", ct, "--timeout", 60, "--forceStop", 1)
            steps.append("shutdown: " + (out or "ok"))
            if rc or ct_running(self.P, ct):
                raise SeatRefused("seat %s: CT %d did not stop (%s)" % (n, ct, out[-300:]))

        def set_gpu(v):
            with self.lock:
                self.conf["seats"][n]["gpu"] = v
                self.save_conf()
        set_gpu(pci)
        try:
            sg.set_hostd_gpu(self.GP, ct, seat, pci)
        except sg.Refuse as e:
            set_gpu(old)
            if running:
                self._pct("start", ct)
            raise SeatRefused("seat %s: its hostd config cannot follow: %s" % (n, e))
        rc, out = self._pct("start", ct)
        steps.append("start: " + (out or "ok"))
        if rc:
            set_gpu(old)
            back = ""
            try:
                if old:
                    sg.set_hostd_gpu(self.GP, ct, seat, old)
                if running:
                    rc2, out2 = self._pct("start", ct)
                    back = "; started again on %s: %s" % (old or "no GPU", "ok" if not rc2 else out2[-200:])
            except sg.Refuse as e:
                back = "; hostd config not put back: %s" % e
            log("seat %s (CT %d): start on %s failed: %s%s" % (n, ct, label, out[-300:], back))
            raise SeatRefused("seat %s: pct start on %s failed: %s%s" % (n, label, out[-600:], back))
        self.dirty.set()
        log("seat %s (CT %d): runs on %s" % (n, ct, label))
        return {"ok": True, "seat": int(n), "ct": ct, "gpu": pci, "model": g["model"], "short": short_model(g["model"]),
                "was": old, "restarted": running, "log": steps, "notes": notes}

    # -- threads
    def udev_thread(self):
        argv = ["udevadm", "monitor", "--kernel", "--udev", "-s", "input", "-s", "sound", "-s", "hidraw", "-s", "usb",
                "-s", "drm"]
        while True:
            try:
                p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                for line in p.stdout:
                    if uevent_is_input_add(line):  # the kernel's event: the node exists, the grab guard acts now
                        self.hurry.set()
                        self.dirty.set()
                    elif line.startswith("UDEV"):
                        self.dirty.set()
                p.wait()
            except OSError as e:
                log("udevadm monitor: %s" % e)
            time.sleep(2)

    def reader_thread(self):
        opened = {}                                   # (path, device id) -> fd
        fds = {}                                      # fd -> device id
        while True:
            self.reader_wake.clear()
            with self.lock:
                want = {(path, dv.id) for dv in self.devs.values() for path in dv.inputs}
            for k in [k for k in opened if k not in want]:
                fd = opened.pop(k)
                fds.pop(fd, None)
                os.close(fd)
            for k in want - set(opened):
                try:
                    # read-only and never grabbed: the seat's own reader gets every event too
                    fd = os.open(os.path.join(self.P.dev, k[0][len("/dev/"):]), os.O_RDONLY | os.O_NONBLOCK)
                except OSError:
                    continue
                opened[k] = fd
                fds[fd] = k[1]
            deadline = time.monotonic() + 5
            while not self.reader_wake.is_set() and time.monotonic() < deadline:
                try:
                    ready, _, _ = select.select(list(fds), [], [], 0.5)
                except (OSError, ValueError):
                    break
                for fd in ready:
                    try:
                        buf = os.read(fd, EVENT_SZ * 64)
                    except OSError as e:
                        if e.errno == errno.ENODEV:          # unplugged: closed with the next rescan
                            fds.pop(fd, None)
                        continue
                    press = motion = False
                    for i in range(0, len(buf) - EVENT_SZ + 1, EVENT_SZ):
                        _, _, typ, _, val = struct.unpack_from(EVENT_FMT, buf, i)
                        if typ == EV_KEY and val == 1:
                            press = True
                        elif typ in (EV_REL, EV_ABS):
                            motion = True
                    if (press or motion) and fd in fds:
                        self.on_input(fds[fd], press)

    def run(self):
        threading.Thread(target=self.udev_thread, daemon=True, name="udev").start()
        threading.Thread(target=self.reader_thread, daemon=True, name="input").start()
        last = 0.0
        self.console_pass()
        if not os.path.exists(NO_POWER_RULE):
            log("WARNING: %s is missing: the host's logind takes every seat keyboard as a power switch "
                "(a streamed power/sleep key switches the host off); install-seatmgr.sh installs it" % NO_POWER_RULE)
        self.rescan()
        threading.Thread(target=self.guard_thread, daemon=True, name="guard").start()
        while True:
            fired = self.dirty.wait(1.0)
            hurry = self.hurry.is_set()
            if fired:
                if not hurry:
                    time.sleep(0.3)                   # one rescan for the burst of a plug
                self.dirty.clear()
                self.hurry.clear()
            if fired or time.monotonic() - last > 5:
                try:
                    self.rescan()
                    if hurry:
                        self.guard_pass()             # a new keyboard is held before the next guard tick
                except Exception as e:                # noqa: BLE001 -- the daemon must outlive one bad scan
                    log("rescan: %s: %s" % (type(e).__name__, e))
                last = time.monotonic()
            else:
                try:
                    self.apply()                      # container starts and stops between scans
                except Exception as e:                # noqa: BLE001
                    log("apply: %s: %s" % (type(e).__name__, e))


# ----------------------------------------------------------------------------- HTTP
def control_allowed(nets, addr):
    """the client address is in one of the networks ("127.0.0.0/8", "10.0.1.2")"""
    try:
        a = ipaddress.ip_address(addr)
        if a.version == 6 and a.ipv4_mapped:
            a = a.ipv4_mapped
    except ValueError:
        return False
    for n in nets:
        try:
            if a in ipaddress.ip_network(n, strict=False):
                return True
        except ValueError:
            continue
    return False


def make_handler(mgr):
    class H(BaseHTTPRequestHandler):
        server_version = "rdx-seatmgr"

        def log_message(self, fmt, *a):
            pass

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            try:
                if path == "/api/state":
                    return self._json(mgr.state())
                if path == "/api/gpus":
                    return self._json(mgr.gpus_state())
                m = re.fullmatch(r"/api/seat/(\d+)", path)
                if m:
                    return self._json(mgr.seat_state(m.group(1)))
                return self._json({"error": "not found"}, 404)
            except ValueError as e:
                return self._json({"error": str(e)}, 400)
            except Exception as e:                    # noqa: BLE001
                return self._json({"error": "%s: %s" % (type(e).__name__, e)}, 500)

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            try:
                n = int(self.headers.get("Content-Length") or 0)
                if n > 65536:
                    return self._json({"error": "body too large"}, 413)
                body = json.loads(self.rfile.read(n) or b"{}") if n else {}
                if not isinstance(body, dict):
                    raise ValueError("body must be a JSON object")
                if path == "/api/assign":
                    return self._json(mgr.assign(str(body.get("id", "")), body.get("seat"), body.get("remember")))
                if path == "/api/forget":
                    return self._json(mgr.forget(str(body.get("id", ""))))
                if path == "/api/claim":
                    return self._json(mgr.claim(body.get("seat"), body.get("seconds", CLAIM_S)))
                if path == "/api/claim/cancel":
                    return self._json(mgr.cancel_claim(body.get("seat")))
                if path == "/api/seat/start":
                    if not control_allowed(mgr.conf.get("control_from") or [], self.client_address[0]):
                        return self._json({"error": "only %s may start or stop a seat" % ", ".join(mgr.conf.get("control_from") or ["nobody"])}, 403)
                    return self._json(mgr.seat_start(body.get("seat"), body.get("gpu"), body.get("force"), body.get("gpuq_booking")))
                return self._json({"error": "not found"}, 404)
            except SeatRefused as e:
                return self._json({"error": str(e), "refused": True}, 409)
            except ValueError as e:
                return self._json({"error": str(e)}, 400)
            except Exception as e:                    # noqa: BLE001
                return self._json({"error": "%s: %s" % (type(e).__name__, e)}, 500)

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()

    return H


def serve(mgr):
    for addr in mgr.conf.get("listen", []):
        host, port = addr.rsplit(":", 1)
        for attempt in range(30):                     # the bridge address may come up after us
            try:
                srv = ThreadingHTTPServer((host, int(port)), make_handler(mgr))
                break
            except OSError as e:
                if attempt == 29:
                    log("cannot listen on %s: %s" % (addr, e))
                    srv = None
                time.sleep(2)
        if srv:
            srv.daemon_threads = True
            threading.Thread(target=srv.serve_forever, daemon=True, name="http " + addr).start()
            log("listening on http://%s" % addr)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default=CONF_DEFAULT)
    ap.add_argument("--runtime", default=RUNTIME_DEFAULT)
    ap.add_argument("--dry-run", action="store_true", help="scan and print what would be applied, change nothing")
    ap.add_argument("--list", action="store_true", help="print the current devices and their seats, then exit")
    a = ap.parse_args(argv)
    mgr = Manager(a.config, a.runtime, dry=a.dry_run or a.list)
    if a.list:
        mgr.devs = scan(mgr.P)
        for d in mgr.state()["devices"]:
            print("%-28s %-6s seat=%-4s %-18s %s  [%s]" % (d["id"], d["kind"], d["seat"], d["how"], d["name"],
                                                         " ".join(n["dev"] for n in d["nodes"])))
        return 0
    serve(mgr)
    mgr.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
