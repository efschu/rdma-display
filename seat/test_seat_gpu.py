#!/usr/bin/env python3
"""Tests of rdx_seat_gpu against a fake sysfs/procfs/dev/pve tree (no root, no device nodes, no containers).

The tree is a multiseat host after a second 3080 went in: the DRM card numbers do not follow the PCI order
(5090 at 15:00.0 is card1, the 3080a at 18:00.0 card0), and the configs written before name the old ones."""

import io
import json
import os
import shutil
import stat
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdx_seat_gpu as sg  # noqa: E402

G5090, G3080A, G3080B = "0000:15:00.0", "0000:18:00.0", "0000:19:00.0"
UUID = {G5090: "GPU-00000000-0000-0000-0000-000000005090", G3080A: "GPU-00000000-0000-0000-0000-0000000030a0",
        G3080B: "GPU-00000000-0000-0000-0000-0000000030b0"}
MODEL = {G5090: "NVIDIA GeForce RTX 5090", G3080A: "NVIDIA GeForce RTX 3080", G3080B: "NVIDIA GeForce RTX 3080"}
PROC_DEVICES = """Character devices:
  1 mem
195 nvidia
195 nvidiactl
226 drm
%d nvidia-uvm
506 nvidia-nvswitch
%d nvidia-caps

Block devices:
259 blkext
"""
# the seat lines of a Proxmox config that are the same for every GPU
FIXED = """arch: amd64
hostname: rdx-seat
rootfs: local-zfs:subvol-{ct}-disk-0,size=200G
lxc.cgroup2.devices.allow: c 195:254 rwm
lxc.cgroup2.devices.allow: c 195:255 rwm
lxc.cgroup2.devices.allow: c 10:223 rwm
lxc.mount.entry: /dev/nvidiactl dev/nvidiactl none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-uvm dev/nvidia-uvm none bind,optional,create=file
lxc.cgroup2.devices.allow: c 231:192 rwm
"""


class Tree:
    def __init__(self):
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="seatgpu-test-"))
        j = lambda *a: os.path.join(self.root, *a)  # noqa: E731
        for d in ("sys", "dev/dri/by-path", "proc", "pve", "lxc", "run", "etc", "rootfs"):
            os.makedirs(j(d))
        self.P = sg.Paths(j("sys"), j("dev"), j("proc"), j("pve"), j("lxc"), j("run/rdx-seat"),
                          j("etc/seatmgr.json"), j("etc/seat-gpu.pins.json"))
        self.nodes = {}
        self.P.stat = self.fake_stat
        self.P.rootfs = lambda ct, pve: j("rootfs", str(ct))
        self.logged = []
        self.P.syslog = lambda msg, err=False: self.logged.append((err, msg))
        self.devices(502, 508)
        self.seats({"1": {"ct": 3101, "name": "5090", "gpu": G5090},
                    "2": {"ct": 3102, "name": "3080a", "gpu": G3080A},
                    "3": {"ct": 3103, "name": "3080b", "gpu": G3080B}})
        # (pci, NVIDIA minor, card, render): card numbers out of PCI order
        for pci, minor, card, render in ((G5090, 0, 1, 128), (G3080A, 1, 0, 129), (G3080B, 2, 2, 130)):
            self.gpu(pci, minor, card, render)
        for n in (254, 255):
            self.nodes[os.path.join(self.P.dev, "nvidia" + ("ctl" if n == 255 else "-modeset"))] = (195, n)

    def close(self):
        shutil.rmtree(self.root)

    def fake_stat(self, path):
        if path not in self.nodes:
            raise FileNotFoundError(path)
        ma, mi = self.nodes[path]
        return types.SimpleNamespace(st_mode=stat.S_IFCHR | 0o660, st_rdev=os.makedev(ma, mi))

    def w(self, path, text):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)

    def devices(self, uvm, caps):
        self.w(os.path.join(self.P.proc, "devices"), PROC_DEVICES % (uvm, caps))

    def seats(self, seats):
        self.w(self.P.conf, json.dumps({"seats": seats, "rules": []}))

    def gpu(self, pci, minor, card, render, uuid=None):
        S = self.P.sys
        real = os.path.join(S, "devices/pci0000:00", pci)
        if os.path.isdir(real):
            shutil.rmtree(real)
        self.w(os.path.join(real, "vendor"), "0x10de\n")
        self.w(os.path.join(real, "class"), "0x030000\n")
        os.makedirs(os.path.join(S, "bus/pci/drivers/nvidia"), exist_ok=True)
        os.symlink(os.path.join(S, "bus/pci/drivers/nvidia"), os.path.join(real, "driver"))
        bus = os.path.join(S, "bus/pci/devices", pci)
        os.makedirs(os.path.dirname(bus), exist_ok=True)
        if not os.path.lexists(bus):
            os.symlink(real, bus)
        for name, mi in (("card%d" % card, card), ("renderD%d" % render, render)):
            d = os.path.join(real, "drm", name)
            self.w(os.path.join(d, "dev"), "226:%d\n" % mi)
            os.symlink(real, os.path.join(d, "device"))
            ch = os.path.join(S, "dev/char/226:%d" % mi)
            os.makedirs(os.path.dirname(ch), exist_ok=True)
            if os.path.lexists(ch):
                os.unlink(ch)
            os.symlink(d, ch)
            self.nodes[os.path.join(self.P.dev, "dri", name)] = (226, mi)
            bp = os.path.join(self.P.dev, "dri/by-path", "pci-%s-%s" % (pci, "card" if name.startswith("card") else "render"))
            if os.path.lexists(bp):
                os.unlink(bp)
            os.symlink("../" + name, bp)
        self.nodes[os.path.join(self.P.dev, "nvidia%d" % minor)] = (195, minor)
        self.w(os.path.join(self.P.proc, "driver/nvidia/gpus", pci, "information"),
               "Model: \t\t %s\nIRQ:   \t\t 45\nGPU UUID: \t %s\nBus Location: \t %s\nDevice Minor: \t %d\n"
               % (MODEL[pci], uuid or UUID[pci], pci, minor))

    def pve(self, ct, extra="", dynamic=True):
        """A seat's Proxmox config: the dynamic form (include + hook) or extra (static) lines."""
        dyn = ("lxc.include: %s\nlxc.hook.pre-start: /opt/rdx-seatmgr/rdx_seat_gpu.py\nhookscript: local:snippets/rdx-seat-gpu\n"
               % sg.include_path(self.P, ct)) if dynamic else ""
        self.w(os.path.join(self.P.pve, "%d.conf" % ct), FIXED.format(ct=ct) + dyn + extra + "\n[snap]\nlxc.cgroup2.devices.allow: c 226:* rwm\n")

    def lxc_config(self, ct, extra=""):
        """lxc's config of a start, as Proxmox writes it (lxc syntax, its own includes first)."""
        lines = ["lxc.include = /usr/share/lxc/config/ubuntu.common.conf"]
        for k, v in sg.parse_lines(sg.rd(os.path.join(self.P.pve, "%d.conf" % ct)), pve=True):
            if k.startswith("lxc."):
                lines.append("%s = %s" % (k, v))
        self.w(os.path.join(self.P.lxc, str(ct), "config"), "\n".join(lines) + "\n" + extra)
        return os.path.join(self.P.lxc, str(ct), "config")

    def running(self, ct):
        os.makedirs(os.path.join(self.P.sys, "fs/cgroup/lxc", str(ct)), exist_ok=True)

    def hostd(self, ct, seat, gpu):
        self.w(os.path.join(self.root, "rootfs", str(ct), "etc/rdx/hostd-seat%s.json" % seat), json.dumps({"gpu": gpu}))


def static_lines(minor, card, render, uvm=502, caps=508):
    """The lines pve-seat-create.sh wrote before (one GPU's numbers fixed in the config)."""
    return ("lxc.cgroup2.devices.allow: c 195:%d rwm\nlxc.cgroup2.devices.allow: c 226:%d rwm\n"
            "lxc.cgroup2.devices.allow: c 226:%d rwm\n"
            "lxc.mount.entry: /dev/nvidia%d dev/nvidia%d none bind,optional,create=file\n"
            "lxc.mount.entry: /dev/dri/card%d dev/dri/card%d none bind,optional,create=file\n"
            "lxc.mount.entry: /dev/dri/renderD%d dev/dri/renderD%d none bind,optional,create=file\n"
            "lxc.cgroup2.devices.allow: c %d:* rwm\nlxc.cgroup2.devices.allow: c %d:* rwm\n"
            % (minor, card, render, minor, minor, card, card, render, render, uvm, caps))


class Base(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.P = self.t.P
        for ct, seat, pci in ((3101, 1, G5090), (3102, 2, G3080A), (3103, 3, G3080B)):
            self.t.pve(ct)
            self.t.hostd(ct, seat, pci)
        self.out = io.StringIO()

    def tearDown(self):
        self.t.close()

    def include(self, ct):
        return sg.rd(sg.include_path(self.P, ct)) + "\n"

    def start(self, ct):
        sg.pre_start(self.P, ct, out=self.out)
        return self.include(ct)

    def refused(self, fn, *a, **kw):
        with self.assertRaises(sg.Refuse) as cm:
            fn(*a, **kw)
        return str(cm.exception)


class ResolveTest(Base):
    def test_numbers_come_from_the_pci_address_not_the_order(self):
        g = sg.resolve(self.P, G5090)
        self.assertEqual((g["minor"], g["card"], g["render"], g["card_minor"], g["render_minor"]),
                         (0, "card1", "renderD128", 1, 128))
        g = sg.resolve(self.P, "18:00.0")                              # short form = domain 0000
        self.assertEqual((g["pci"], g["minor"], g["card"], g["render"]), (G3080A, 1, "card0", "renderD129"))
        self.assertEqual(g["uuid"], UUID[G3080A])

    def test_doubts_refuse(self):
        P, S = self.P, self.P.sys
        self.assertIn("no PCI device", self.refused(sg.resolve, P, "0000:0d:00.0"))   # the 5090's old address
        self.assertIn("no PCI address", self.refused(sg.resolve, P, "15:00"))
        dev = os.path.join(S, "bus/pci/devices", G5090)
        os.unlink(os.path.join(dev, "driver"))
        os.makedirs(os.path.join(S, "bus/pci/drivers/vfio-pci"))
        os.symlink(os.path.join(S, "bus/pci/drivers/vfio-pci"), os.path.join(dev, "driver"))
        self.assertIn("bound to vfio-pci", self.refused(sg.resolve, P, G5090))
        # a second card node (a framebuffer driver on the same function) -> no guess which one
        self.t.w(os.path.join(dev, "drm/card3/dev"), "226:3\n")
        os.unlink(os.path.join(dev, "driver"))
        os.symlink(os.path.join(S, "bus/pci/drivers/nvidia"), os.path.join(dev, "driver"))
        self.assertIn("2 DRM card nodes", self.refused(sg.resolve, P, G5090))
        shutil.rmtree(os.path.join(dev, "drm/card3"))
        sg.resolve(P, G5090)

    def test_host_nodes_must_agree_with_sysfs(self):
        P = self.P
        self.t.nodes[os.path.join(P.dev, "dri/card1")] = (226, 0)        # a node with the other card's numbers
        self.assertIn("/dev/dri/card1 is c 226:0", self.refused(sg.resolve, P, G5090))
        self.t.nodes[os.path.join(P.dev, "dri/card1")] = (226, 1)
        del self.t.nodes[os.path.join(P.dev, "nvidia0")]
        self.assertIn("/dev/nvidia0 is missing", self.refused(sg.resolve, P, G5090))
        self.t.nodes[os.path.join(P.dev, "nvidia0")] = (195, 0)
        bp = os.path.join(P.dev, "dri/by-path/pci-%s-render" % G5090)
        os.unlink(bp)
        os.symlink("../renderD129", bp)
        self.assertIn("points to renderD129", self.refused(sg.resolve, P, G5090))
        os.unlink(bp)                                                   # no by-path link at all: fine
        sg.resolve(P, G5090)
        ch = os.path.join(P.sys, "dev/char/226:1")
        os.unlink(ch)
        os.symlink(os.path.join(P.sys, "devices/pci0000:00", G3080A, "drm/card0"), ch)
        self.assertIn("226:1 belongs to %s" % G3080A, self.refused(sg.resolve, P, G5090))

    def test_procfs_minor(self):
        info = os.path.join(self.P.proc, "driver/nvidia/gpus", G5090, "information")
        self.t.w(info, "Model: x\nDevice Minor: 255\n")
        self.assertIn("no GPU minor", self.refused(sg.resolve, self.P, G5090))
        os.unlink(info)
        self.assertIn("no Device Minor", self.refused(sg.resolve, self.P, G5090))


class StartTest(Base):
    def test_include_follows_the_pci_address(self):
        inc = self.start(3101)
        self.assertIn("lxc.cgroup2.devices.allow = c 195:0 rwm\n", inc)
        self.assertIn("lxc.cgroup2.devices.allow = c 226:1 rwm\n", inc)
        self.assertIn("lxc.cgroup2.devices.allow = c 226:128 rwm\n", inc)
        self.assertIn("lxc.cgroup2.devices.allow = c 502:* rwm\nlxc.cgroup2.devices.allow = c 508:* rwm\n", inc)
        # the host's kernel names inside, not by-path, not renumbered
        self.assertIn("lxc.mount.entry = /dev/dri/card1 dev/dri/card1 none bind,create=file\n", inc)
        self.assertIn("lxc.mount.entry = /dev/dri/renderD128 dev/dri/renderD128 none bind,create=file\n", inc)
        self.assertIn("lxc.mount.entry = /dev/nvidia0 dev/nvidia0 none bind,create=file\n", inc)
        self.assertNotIn("optional", inc)
        self.assertIn("/dev/dri/card1", self.out.getvalue())
        self.assertIn("pinned now", self.out.getvalue())
        # and lxc's own check of that start passes
        sg.lxc_verify(self.P, 3101, self.t.lxc_config(3101), out=self.out)

    def test_cards_moved_the_next_start_follows(self):
        self.start(3101)
        self.start(3102)
        # a re-plug: the cards swap their DRM and NVIDIA numbers, the PCI addresses stay
        self.t.gpu(G5090, 1, 0, 129)
        self.t.gpu(G3080A, 0, 1, 128)
        self.t.devices(500, 507)                                         # and the driver load renumbered uvm/caps
        inc = self.start(3101)
        self.assertIn("c 195:1 rwm", inc)
        self.assertIn("/dev/dri/card0 dev/dri/card0", inc)
        self.assertIn("/dev/dri/renderD129 dev/dri/renderD129", inc)
        self.assertIn("c 500:* rwm", inc)
        self.assertNotIn("c 502:*", inc)
        self.assertNotIn("card1", inc.split("\n", 2)[2])

    def test_the_old_static_config_is_refused(self):
        # 3101's config still names card0 (now the 3080a's) -> never started like that
        self.t.pve(3101, static_lines(0, 0, 128), dynamic=False)
        msg = self.refused(sg.pre_start, self.P, 3101)
        self.assertIn("does not include", msg)
        self.assertFalse(os.path.exists(sg.include_path(self.P, 3101)))
        msg = self.refused(sg.lxc_verify, self.P, 3101, self.t.lxc_config(3101))
        self.assertIn("allows c 226:0, the node of %s (card0)" % G3080A, msg)
        self.assertIn("binds /dev/dri/card0 to dev/dri/card0", msg)
        self.assertIn("does not bind /dev/dri/card1", msg)

    def test_a_leftover_static_line_next_to_the_include_is_refused(self):
        self.t.pve(3101, "lxc.cgroup2.devices.allow: c 195:1 rwm\n")
        self.assertIn("allows /dev/nvidia1, the node of %s" % G3080A, self.refused(sg.pre_start, self.P, 3101))
        self.t.pve(3101, "lxc.cgroup2.devices.allow: c 226:* rwm\n")
        self.assertIn("wildcard allow c 226:*", self.refused(sg.pre_start, self.P, 3101))
        self.t.pve(3101, "lxc.mount.entry: /dev/dri/card1 dev/dri/card0 none bind,create=file\n")
        self.assertIn("binds /dev/dri/card1 to dev/dri/card0", self.refused(sg.pre_start, self.P, 3101))
        self.t.pve(3101, "lxc.cgroup2.devices.allow: c 505:* rwm\n")   # an old uvm major: infiniband_mad now
        self.assertIn("whole dynamic major 505", self.refused(sg.pre_start, self.P, 3101))
        self.t.pve(3101, "lxc.cgroup2.devices.allow: c 10:* rwm\n")    # fixed majors are not this hook's
        self.start(3101)

    def test_seat_without_gpu(self):
        self.t.seats({"3": {"ct": 3103, "name": "3080b", "gpu": ""}})
        self.t.hostd(3103, 3, "")
        inc = self.start(3103)
        self.assertNotIn("lxc.", inc)
        self.assertIn("no GPU", self.out.getvalue())
        self.t.pve(3103, static_lines(2, 2, 130))
        self.assertIn("allows /dev/nvidia2", self.refused(sg.pre_start, self.P, 3103))

    def test_seatmgr_conf(self):
        # two seats may name one card (the last choice of each); only one may run with it
        self.t.seats({"1": {"ct": 3101, "gpu": G5090}, "2": {"ct": 3102, "gpu": "15:00.0"}})
        self.t.hostd(3102, 2, G5090)
        self.start(3101)
        self.t.lxc_config(3101)
        self.t.running(3101)
        self.assertIn("GPU %s is seat 1's now (CT 3101 runs with it)" % G5090, self.refused(sg.pre_start, self.P, 3102))
        self.t.seats({"1": {"ct": 3101, "gpu": G5090}, "2": {"ct": 3101, "gpu": G3080A}})
        self.assertIn("is seat 1 and seat 2", self.refused(sg.pre_start, self.P, 3101))
        self.t.seats({"1": {"ct": 3101, "gpu": G5090}})
        self.assertIn("no seat", self.refused(sg.pre_start, self.P, 3102))
        os.unlink(self.P.conf)
        self.assertIn("cannot read", self.refused(sg.pre_start, self.P, 3101))

    def test_hostd_config_must_agree(self):
        self.t.hostd(3101, 1, "0000:0d:00.0")
        self.assertIn("hostd-seat1.json '0000:0d:00.0'", self.refused(sg.pre_start, self.P, 3101))
        os.unlink(os.path.join(self.t.root, "rootfs/3101/etc/rdx/hostd-seat1.json"))
        self.start(3101)
        self.assertIn("not compared", self.out.getvalue())

    def test_refusal_leaves_no_include_of_an_earlier_start(self):
        self.start(3101)
        self.t.hostd(3101, 1, G3080A)
        self.refused(sg.pre_start, self.P, 3101)
        self.assertFalse(os.path.exists(sg.include_path(self.P, 3101)))


class PinTest(Base):
    def pins(self):
        return json.loads(sg.rd(self.P.pins))["cards"]

    def test_renumbered_bus_is_refused_until_unpinned(self):
        self.start(3101)
        self.assertEqual(self.pins()[G5090]["uuid"], UUID[G5090])
        # a card more: the bus renumbers and 15:00.0 is now a 3080
        self.t.gpu(G5090, 0, 1, 128, uuid="GPU-aaaaaaaa-0000-0000-0000-000000000000")
        msg = self.refused(sg.pre_start, self.P, 3101)
        self.assertIn("the card pinned at that address is %s" % UUID[G5090], msg)
        self.assertIn("--unpin %s" % G5090, msg)
        self.assertFalse(os.path.exists(sg.include_path(self.P, 3101)))
        # the 5090 is at 19:00.0 now, where the 3080b was pinned: refused there as well
        self.start(3103)
        self.t.gpu(G3080B, 2, 2, 130, uuid=UUID[G5090])
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B}})
        self.t.hostd(3101, 1, G3080B)
        self.assertIn("pinned at that address is %s" % UUID[G3080B], self.refused(sg.pre_start, self.P, 3101))
        self.assertEqual(sg.main(["--unpin", G3080B], self.P), 0)
        self.assertIn("c 226:2 rwm", self.start(3101))
        self.assertEqual(self.pins()[G3080B]["uuid"], UUID[G5090])

    def test_a_seat_switching_cards_is_no_swap(self):
        # seat 1 on the 5090, then on the 3080b (seat 3 stopped): both cards stay pinned, nothing refused
        self.start(3101)
        self.start(3103)
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B}, "3": {"ct": 3103, "gpu": G3080B}})
        self.t.hostd(3101, 1, G3080B)
        self.assertIn("c 195:2 rwm", self.start(3101))
        self.assertEqual(self.pins()[G5090]["uuid"], UUID[G5090])
        self.assertEqual(self.pins()[G3080B]["uuid"], UUID[G3080B])
        self.assertNotIn("pinned now", self.out.getvalue().splitlines()[-1])

    def test_unpin_after_a_swap_on_purpose(self):
        self.start(3101)
        self.t.gpu(G5090, 0, 1, 128, uuid="GPU-bbbbbbbb-0000-0000-0000-000000000000")
        self.refused(sg.pre_start, self.P, 3101)
        self.assertEqual(sg.main(["--unpin", "3101"], self.P), 0)              # a CT: its seat's card
        self.start(3101)
        self.assertEqual(self.pins()[G5090]["uuid"], "GPU-bbbbbbbb-0000-0000-0000-000000000000")
        self.assertEqual(sg.main(["--pin", G3080A], self.P), 0)
        self.assertEqual(self.pins()[G3080A]["uuid"], UUID[G3080A])

    def test_pins_of_the_per_seat_form_are_taken_over(self):
        # the earlier file format: one pin per container
        self.t.w(self.P.pins, json.dumps({"3101": {"uuid": UUID[G5090], "pci": G5090, "model": MODEL[G5090]},
                                          "3103": {"uuid": UUID[G3080B], "pci": G3080B, "model": MODEL[G3080B]}}))
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B}})
        self.t.hostd(3101, 1, G3080B)
        self.start(3101)                                    # seat 1 on 3103's card: no swap
        self.assertEqual(set(self.pins()), {G5090, G3080B})
        self.assertNotIn("3101", json.loads(sg.rd(self.P.pins)))
        # the old pin of the 5090 still guards its address
        self.t.gpu(G5090, 0, 1, 128, uuid="GPU-cccccccc-0000-0000-0000-000000000000")
        self.t.seats({"1": {"ct": 3101, "gpu": G5090}})
        self.t.hostd(3101, 1, G5090)
        self.refused(sg.pre_start, self.P, 3101)


class CardChoiceTest(Base):
    def test_allowed_cards(self):
        self.assertEqual(sg.nvidia_cards(self.P), [G5090, G3080A, G3080B])
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B, "gpus": ["15:00.0", G3080A]}})
        self.t.hostd(3101, 1, G3080B)
        self.assertIn("not one of seat 1's cards (%s, %s)" % (G5090, G3080A), self.refused(sg.pre_start, self.P, 3101))
        self.t.seats({"1": {"ct": 3101, "gpu": G5090, "gpus": "all"}})
        self.assertIn("must be a list", self.refused(sg.pre_start, self.P, 3101))

    def test_set_hostd_gpu(self):
        p = os.path.join(self.t.root, "rootfs/3101/etc/rdx/hostd-seat1.json")
        self.t.w(p, json.dumps({"seat": 1, "gpu": G5090, "seat_name": "5090", "apps": [{"id": "steam"}]}))
        seat = sg.seat_of(self.P, 3101)
        self.assertEqual(sg.set_hostd_gpu(self.P, 3101, seat, G3080B), G5090)
        conf = json.loads(sg.rd(p))
        self.assertEqual((conf["gpu"], conf["seat_name"], conf["apps"]), (G3080B, "", [{"id": "steam"}]))
        self.t.w(p, json.dumps({"gpu": G5090, "seat_name": "Wohnzimmer"}))
        sg.set_hostd_gpu(self.P, 3101, seat, G3080A)
        self.assertEqual(json.loads(sg.rd(p))["seat_name"], "Wohnzimmer")       # a real name stays
        os.unlink(p)
        self.assertIn("hostd-seat1.json", self.refused(sg.set_hostd_gpu, self.P, 3101, seat, G5090))


class IrqTest(Base):
    def setUp(self):
        super().setUp()
        self.t.w(os.path.join(self.P.proc, "irq/default_smp_affinity"), "00ff00ff\n")
        self.irq = {}
        for pci, nums in ((G5090, (172, 173)), (G3080A, (180,)), (G3080B, (181,))):
            self.msi(pci, nums)
            self.msi(pci[:-1] + "1", (nums[0] - 10,))                           # its HDMI sound function
        self.t.pve(3101, "lxc.cgroup2.cpuset.cpus: 8-15,24-31\n")
        self.t.pve(3103, "lxc.cgroup2.cpuset.cpus: 8-15,24-31\n")

    def msi(self, fn, nums):
        real = os.path.join(self.P.sys, "devices/pci0000:00", fn)
        os.makedirs(os.path.join(real, "msi_irqs"), exist_ok=True)
        bus = os.path.join(self.P.sys, "bus/pci/devices", fn)
        if not os.path.lexists(bus):
            os.symlink(real, bus)
        for n in nums:
            self.t.w(os.path.join(real, "msi_irqs", str(n)), "msix\n")
            self.t.w(os.path.join(self.P.proc, "irq", str(n), "smp_affinity_list"), "0-7,16-23\n")
            self.irq[n] = fn

    def aff(self, n):
        return sg.rd(os.path.join(self.P.proc, "irq", str(n), "smp_affinity_list"))

    def test_cpu_lists(self):
        self.assertEqual(sg.cpus_text(sg.cpus_of("8-15,24-31")), "8-15,24-31")
        self.assertEqual(sg.cpus_text(sg.mask_cpus("00ff00ff")), "0-7,16-23")
        self.assertEqual(sg.cpus_text(sg.mask_cpus("ffffffff,00000001")), "0,32-63")
        self.assertEqual(sg.cpus_text({3}), "3")
        self.assertIsNone(sg.cpus_of("x"))

    def test_the_seat_card_follows_the_running_seat(self):
        # seat 1 starts on the 3080b: GPU and HDMI sound to the seat's CPUs, the others stay on the host's
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B}, "3": {"ct": 3103, "gpu": G3080B}})
        self.t.hostd(3101, 1, G3080B)
        self.assertEqual(sg.main(["3101", "post-start"], self.P), 0)
        self.assertEqual((self.aff(181), self.aff(171)), ("8-15,24-31", "8-15,24-31"))
        self.assertEqual((self.aff(172), self.aff(162), self.aff(180)), ("0-7,16-23",) * 3)
        self.assertTrue(any("irq 181 (%s) -> 8-15,24-31, seat 1 (CT 3101)" % G3080B in m for _, m in self.t.logged))
        # stopped: back to the host's CPUs (the cgroup may still be there during post-stop)
        self.t.running(3101)
        self.assertEqual(sg.main(["3101", "post-stop"], self.P), 0)
        self.assertEqual((self.aff(181), self.aff(171)), ("0-7,16-23", "0-7,16-23"))
        # seat 1 on the 5090 now
        self.t.seats({"1": {"ct": 3101, "gpu": G5090}, "3": {"ct": 3103, "gpu": G3080B}})
        sg.main(["3101", "post-start"], self.P)
        self.assertEqual((self.aff(172), self.aff(173), self.aff(162), self.aff(181)),
                         ("8-15,24-31", "8-15,24-31", "8-15,24-31", "0-7,16-23"))

    def test_without_a_cpuset_the_irqs_stay(self):
        self.t.seats({"2": {"ct": 3102, "gpu": G3080A}})
        lines = sg.irq_sync(self.P, {3102: True})
        self.assertIn("%s: seat 2 (CT 3102) has no lxc.cgroup2.cpuset.cpus: IRQs stay" % G3080A, lines)
        self.assertEqual(self.aff(180), "0-7,16-23")

    def test_an_irq_that_does_not_move_is_noted(self):
        self.t.seats({"1": {"ct": 3101, "gpu": G3080B}})
        path = os.path.join(self.P.proc, "irq/181/smp_affinity_list")
        os.unlink(path)
        os.mkdir(path)                                     # a write that fails as root too (a managed IRQ: EIO)
        lines = sg.irq_sync(self.P, {3101: True})
        self.assertTrue(any(l.startswith("irq 181 (%s) not movable" % G3080B) for l in lines), lines)


class OthersTest(Base):
    def test_a_running_seat_with_the_card_refuses(self):
        # 3102 runs with the old lines that name the 5090's card1 (the 3080a's card before the re-plug)
        self.t.pve(3102, static_lines(1, 1, 129), dynamic=False)
        self.t.lxc_config(3102)
        self.t.running(3102)
        msg = self.refused(sg.pre_start, self.P, 3101)
        self.assertIn("running seat CT 3102 (seat 2) allows /dev/dri/card1", msg)
        # stopped, its config is its own start's business
        os.rmdir(os.path.join(self.P.sys, "fs/cgroup/lxc/3102"))
        self.start(3101)

    def test_a_running_seat_is_judged_by_what_it_started_with(self):
        # 3103 runs (static lines of its start); its Proxmox config is migrated meanwhile: no difference
        self.t.pve(3103, static_lines(2, 2, 130), dynamic=False)
        self.t.lxc_config(3103)
        self.t.running(3103)
        self.t.pve(3103)
        self.start(3101)
        self.start(3102)
        self.assertEqual(sg.status(self.P, out=self.out), 0)
        self.assertIn("CT 3103 seat 3 (3080b) running: GPU %s" % G3080B, self.out.getvalue())

    def test_a_non_seat_container_is_only_named(self):
        self.t.w(os.path.join(self.P.pve, "200.conf"), "lxc.cgroup.devices.allow: c 195:* rwm\n")
        self.t.w(os.path.join(self.P.lxc, "200/config"), "lxc.cgroup.devices.allow = c 195:* rwm\n"
                 "lxc.mount.entry = /dev/nvidia0 dev/nvidia0 none bind,optional,create=file\n")
        self.t.running(200)
        self.start(3101)
        self.assertIn("note: running CT 200 (no seat) allows c 195:* too", self.out.getvalue())


class HookTest(Base):
    def test_roles_by_arguments(self):
        # PVE hookscript pre-start, then lxc's pre-start hook of the same start
        self.assertEqual(sg.main(["3101", "pre-start"], self.P), 0)
        cfg = self.t.lxc_config(3101)
        os.environ["LXC_NAME"], os.environ["LXC_CONFIG_FILE"] = "3101", cfg
        try:
            self.assertEqual(sg.main(["3101", "lxc", "pre-start"], self.P), 0)
            self.assertEqual(sg.main(["3101", "lxc", "start"], self.P), 0)       # other lxc hooks: nothing
            # the PVE hookscript's post-stop runs inside lxc's post-stop hook with its LXC_* environment:
            # the arguments, not the environment, make it the hookscript
            os.environ["LXC_HOOK_TYPE"] = "post-stop"
            self.assertEqual(sg.main(["3101", "post-stop"], self.P), 0)
            self.assertFalse(os.path.exists(sg.include_path(self.P, 3101)))
            # a start that bypasses the hookscript finds no include: refused
            self.assertEqual(sg.main(["3101", "lxc", "pre-start"], self.P), 1)
            self.assertEqual(sg.main(["3101", "post-stop"], self.P), 0)          # twice: still no failure
        finally:
            for k in ("LXC_NAME", "LXC_CONFIG_FILE", "LXC_HOOK_TYPE"):
                os.environ.pop(k, None)
        self.assertEqual(sg.main(["3101", "pre-stop"], self.P), 0)
        self.assertEqual(sg.main(["3101", "post-start"], self.P), 0)

    def test_lxc_hook_version_1_without_arguments(self):
        sg.main(["3101", "pre-start"], self.P)
        cfg = self.t.lxc_config(3101)
        env = {"LXC_HOOK_VERSION": "1", "LXC_HOOK_TYPE": "pre-start", "LXC_NAME": "3101", "LXC_CONFIG_FILE": cfg}
        os.environ.update(env)
        try:
            self.assertEqual(sg.main([], self.P), 0)
            os.unlink(sg.include_path(self.P, 3101))
            self.assertEqual(sg.main([], self.P), 1)
        finally:
            for k in env:
                os.environ.pop(k, None)

    def test_restart_after_an_inside_reboot_is_checked_again(self):
        # the reboot path runs no hookscript: the include of the first start is used and checked again
        sg.main(["3101", "pre-start"], self.P)
        cfg = self.t.lxc_config(3101)
        sg.lxc_verify(self.P, 3101, cfg, out=self.out)
        self.t.gpu(G5090, 1, 0, 129)                                         # moved in between (driver reload)
        self.t.gpu(G3080A, 0, 1, 128)
        self.assertIn("allows /dev/nvidia0, the node of %s" % G3080A,
                      self.refused(sg.lxc_verify, self.P, 3101, cfg))

    def test_hookscript_refusal_exit_code(self):
        self.t.pve(3101, static_lines(0, 0, 128), dynamic=False)
        self.assertEqual(sg.main(["3101", "pre-start"], self.P), 1)
        # lxc hides a hook's output from pct start: the reason is in the journal as well
        self.assertTrue(self.t.logged[-1][0])
        self.assertIn("CT 3101: START REFUSED: config of CT 3101", self.t.logged[-1][1])
        self.t.pve(3101)
        self.assertEqual(sg.main(["3101", "pre-start"], self.P), 0)
        self.assertEqual(self.t.logged[-1][0], False)
        self.assertIn("/dev/dri/card1", self.t.logged[-1][1])

    def test_write_for_a_running_container(self):
        self.assertEqual(sg.main(["--write", "3103"], self.P), 0)
        self.assertIn("c 226:130 rwm", self.include(3103))
        self.assertEqual(sg.main(["--check", "3101"], self.P), 0)
        self.assertFalse(os.path.exists(sg.include_path(self.P, 3101)))     # --check writes nothing
        self.assertFalse(os.path.exists(self.P.pins))


if __name__ == "__main__":
    unittest.main()
