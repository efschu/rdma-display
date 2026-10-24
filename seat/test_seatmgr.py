#!/usr/bin/env python3
"""Tests of rdx_seatmgr against a fake sysfs/dev/udev tree (no root, no device nodes, no containers)."""

import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rdx_seatmgr as sm  # noqa: E402

PCI_USB = "devices/pci0000:00/0000:00:01.2/0000:0a:00.1/usb1"
PCI_SND = "devices/pci0000:00/0000:00:03.1/0000:0d:00.1"
PCI_SND2 = "devices/pci0000:00/0000:00:08.1/0000:10:00.4"
PCI_GPU = "devices/pci0000:00/0000:00:03.1/0000:0d:00.0"


class Tree:
    def __init__(self):
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="seatmgr-test-"))
        self.sys = os.path.join(self.root, "sys")
        self.dev = os.path.join(self.root, "dev")
        self.udev = os.path.join(self.root, "udev")
        self.proc = os.path.join(self.root, "proc")
        for d in (self.sys, self.dev, self.udev, self.proc):
            os.makedirs(d)
        self.nodes = {}          # dev path -> (major, minor, mode, gid)
        self.P = sm.Paths(self.sys, self.dev, self.udev, self.proc)
        self.P.stat = self.fake_stat

    def fake_stat(self, path):
        if path not in self.nodes:
            raise FileNotFoundError(path)
        ma, mi, mode, gid = self.nodes[path]
        return types.SimpleNamespace(st_mode=stat.S_IFCHR | mode, st_rdev=os.makedev(ma, mi), st_gid=gid)

    def w(self, rel, text):
        p = os.path.join(self.sys, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            fh.write(text + "\n")

    def link(self, rel, target_rel):
        p = os.path.join(self.sys, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        os.symlink(os.path.join(self.sys, target_rel), p)

    def cnode(self, cls, name, devpath_rel, devname, ma, mi, mode=0o660, gid=101):
        self.w(devpath_rel + "/dev", "%d:%d" % (ma, mi))
        self.w(devpath_rel + "/uevent", "MAJOR=%d\nMINOR=%d\nDEVNAME=%s" % (ma, mi, devname))
        self.link("class/%s/%s" % (cls, name), devpath_rel)
        self.nodes[os.path.join(self.dev, devname)] = (ma, mi, mode, gid)

    def usb(self, name, vid, pid, busnum, devnum, product, serial="", cls="00", driver=None):
        d = PCI_USB + "/" + name
        for k, v in (("idVendor", vid), ("idProduct", pid), ("busnum", str(busnum)), ("devnum", str(devnum)),
                     ("bDeviceClass", cls), ("product", product), ("dev", "189:%d" % (devnum - 1))):
            self.w(d + "/" + k, v)
        if serial:
            self.w(d + "/serial", serial)
        self.link("bus/usb/devices/" + name, d)
        self.nodes[os.path.join(self.dev, "bus/usb/%03d/%03d" % (busnum, devnum))] = (189, devnum - 1, 0o664, 0)
        if driver:
            self.w(d + "/%s:1.0/bInterfaceClass" % name, "ff")
            os.makedirs(os.path.join(self.sys, "bus/drivers", driver), exist_ok=True)
            os.symlink(os.path.join(self.sys, "bus/drivers", driver), os.path.join(self.sys, d, "%s:1.0" % name, "driver"))
        return d

    def mouse(self, name="1-3", minor=66, devnum=3, serial=""):
        d = self.usb(name, "093a", "2510", 1, devnum, "USB Optical Mouse", serial)
        inp = d + "/%s:1.0/0003:093A:2510.0002/input/input%d" % (name, minor)
        self.w(inp + "/name", "PixArt USB Optical Mouse")
        self.w(inp + "/phys", "usb-0000:0a:00.1-3/input0")
        self.w(inp + "/capabilities/ev", "17")
        self.w(inp + "/capabilities/key", "70000 0 0 0 0")
        self.cnode("input", "event%d" % (minor - 64), inp + "/event%d" % (minor - 64), "input/event%d" % (minor - 64), 13, minor)
        self.cnode("input", "mouse%d" % (minor - 66), inp + "/mouse%d" % (minor - 66), "input/mouse%d" % (minor - 66), 13, 32 + minor - 66)
        self.cnode("hidraw", "hidraw%d" % (minor - 65), d + "/%s:1.0/0003:093A:2510.0002/hidraw/hidraw%d" % (name, minor - 65),
                   "hidraw%d" % (minor - 65), 242, minor - 65, 0o600, 0)
        with open(os.path.join(self.udev, "c13:%d" % minor), "w") as fh:
            fh.write("E:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\nE:ID_PATH=pci-0000:0a:00.1-usb-0:3:1.0\n")
        with open(os.path.join(self.udev, "c189:%d" % (devnum - 1)), "w") as fh:
            fh.write("E:ID_PATH=pci-0000:0a:00.1-usb-0:%s\n" % name.split("-", 1)[1])
        return d

    def gpu_sound(self, pci=PCI_SND, card=1, jack_minor=76):
        self.w(pci + "/vendor", "0x10de")
        self.w(pci + "/device", "0x22e8")
        c = pci + "/sound/card%d" % card
        self.w(c + "/id", "NVidia")
        self.link("class/sound/card%d" % card, c)
        with open(os.path.join(self.udev, "+sound:card%d" % card), "w") as fh:
            fh.write("E:ID_PATH=pci-%s\nE:SOUND_INITIALIZED=1\nG:seat\n" % os.path.basename(pci))
        self.cnode("sound", "controlC%d" % card, c + "/controlC%d" % card, "snd/controlC%d" % card, 116, 20 + card, gid=29)
        self.cnode("sound", "pcmC%dD3p" % card, c + "/pcmC%dD3p" % card, "snd/pcmC%dD3p" % card, 116, 10 + card, gid=29)
        jack = c + "/input%d" % jack_minor
        self.w(jack + "/name", "HDA NVidia HDMI/DP,pcm=3")
        self.w(jack + "/capabilities/ev", "21")
        self.cnode("input", "event%d" % (jack_minor - 64), jack + "/event%d" % (jack_minor - 64), "input/event%d" % (jack_minor - 64), 13, jack_minor)

    def monitor(self, connected=True):
        self.w(PCI_GPU + "/drm/card2/dev", "226:2")
        self.link("class/drm/card2", PCI_GPU + "/drm/card2")
        self.w(PCI_GPU + "/drm/card2/card2-HDMI-A-2/status", "connected" if connected else "disconnected")
        self.link("class/drm/card2-HDMI-A-2", PCI_GPU + "/drm/card2/card2-HDMI-A-2")
        os.symlink(os.path.join(self.sys, PCI_GPU), os.path.join(self.sys, PCI_GPU, "drm/card2/device"))

    def ct_running(self, ct):
        os.makedirs(os.path.join(self.sys, "fs/cgroup/lxc", str(ct)), exist_ok=True)

    def conf(self):
        p = os.path.join(self.root, "seatmgr.json")
        with open(p, "w") as fh:
            json.dump({"listen": [], "seats": {
                "1": {"ct": 3101, "name": "5090", "gpu": "0000:0d:00.0"},
                "2": {"ct": 3102, "name": "3080a", "gpu": "0000:0e:00.0"}}, "rules": []}, fh)
        return p

    def manager(self):
        m = sm.Manager(self.conf(), os.path.join(self.root, "run/runtime.json"), P=self.P, dry=True)
        return m

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


class ScanTest(unittest.TestCase):
    def setUp(self):
        self.t = Tree()

    def tearDown(self):
        self.t.close()

    def test_usb_mouse_is_one_device(self):
        self.t.mouse()
        devs = sm.scan(self.t.P)
        self.assertEqual(list(devs), ["usb:1-3"])
        d = devs["usb:1-3"]
        self.assertEqual(sorted(n.dev for n in d.nodes),
                         ["/dev/bus/usb/001/003", "/dev/hidraw1", "/dev/input/event2", "/dev/input/mouse0"])
        self.assertEqual(d.path, "pci-0000:0a:00.1-usb-0:3")
        self.assertEqual(d.devid, "usb:093a:2510")
        self.assertTrue(d.keys and d.motion and d.has_input)
        self.assertIn("mouse", d.roles)
        self.assertEqual(d.inputs, ["/dev/input/event2"])

    def test_sound_card_takes_its_jacks(self):
        self.t.gpu_sound()
        devs = sm.scan(self.t.P)
        self.assertEqual(list(devs), ["sound:0000:0d:00.1"])
        d = devs["sound:0000:0d:00.1"]
        self.assertEqual(d.path, "pci-0000:0d:00.1")
        self.assertEqual(sorted(n.dev for n in d.nodes), ["/dev/input/event12", "/dev/snd/controlC1", "/dev/snd/pcmC1D3p"])
        self.assertFalse(d.keys)            # a jack switch cannot claim
        self.assertEqual([os.path.basename(c) for c in d.cards], ["card1"])

    def test_hub_and_root_hub_are_no_devices(self):
        self.t.usb("1-1", "0bda", "5411", 1, 2, "Hub", cls="09")
        self.t.mouse(name="1-1.2", devnum=4)
        self.assertEqual(list(sm.scan(self.t.P)), ["usb:1-1.2"])

    def test_usb_path_built_without_udev_record(self):
        d = self.t.usb("1-3.2", "1234", "5678", 1, 7, "Pad")
        os.unlink(os.path.join(self.t.sys, d, "dev"))
        self.assertEqual(sm.usb_path_id(self.t.P, os.path.join(self.t.sys, d)), "pci-0000:0a:00.1-usb-0:3.2")

    def test_host_used_usb_is_not_assignable(self):
        self.t.usb("2-4", "0bda", "8156", 2, 3, "USB LAN", driver="r8152")
        d = sm.scan(self.t.P)["usb:2-4"]
        self.assertFalse(d.assignable())
        self.assertEqual(d.host_drivers, {"r8152"})


class RuleTest(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.t.mouse()
        self.t.gpu_sound()
        self.t.gpu_sound(PCI_SND2, card=3, jack_minor=69)

    def tearDown(self):
        self.t.close()

    def test_precedence(self):
        m = self.t.manager()
        m.rescan()
        mouse = m.devs["usb:1-3"]
        self.assertEqual(m.seat_of(mouse), (None, "host console"))          # no rule: the host console
        self.assertEqual(m.seat_of(m.devs["sound:0000:0d:00.1"]), (1, "GPU sound"))
        self.assertEqual(m.seat_of(m.devs["sound:0000:10:00.4"]), (None, "host console"))
        m.conf["rules"] = [{"kind": "port", "match": "pci-0000:0a:00.1-usb-0:3", "seat": 1}]
        self.assertEqual(m.seat_of(mouse), (1, "port rule"))
        m.conf["rules"].append({"kind": "device", "match": "usb:093a:2510", "seat": 2})
        self.assertEqual(m.seat_of(mouse), (2, "device rule"))              # device beats port
        m.runtime["usb:1-3"] = {"seat": None, "how": "manual"}
        self.assertEqual(m.seat_of(mouse), (None, "manual"))                # runtime beats both

    def test_new_device_is_the_host_consoles(self):
        """host console and seats only by assignment -- no default seat, not even
        the one with a monitor"""
        self.t.monitor(True)
        m = self.t.manager()
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["usb:1-3"]), (None, "host console"))
        self.assertNotIn("usb:1-3", m.runtime)

    def test_host_console_as_explicit_target(self):
        m = self.t.manager()
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["sound:0000:0d:00.1"]), (1, "GPU sound"))
        j = m.assign("sound:0000:0d:00.1", "host", "port")      # the seat's own GPU sound stays on the host
        self.assertEqual((j["seat"], j["how"]), (None, "host console (port rule)"))
        self.assertEqual(m.conf["rules"][-1]["seat"], "host")
        with open(m.conf_path) as fh:
            self.assertEqual(json.load(fh)["rules"][-1]["seat"], "host")
        j = m.assign("usb:1-3", 2)
        self.assertEqual(j["seat"], 2)
        j = m.assign("usb:1-3", "host")                         # back, for now (runtime)
        self.assertEqual((j["seat"], j["how"]), (None, "host console (manual)"))
        j = m.assign("usb:1-3", "host", "device")
        self.assertEqual((j["seat"], j["how"]), (None, "host console (device rule)"))

    def test_no_default_with_two_monitors(self):
        m = self.t.manager()
        m.monitor_seats = lambda: [1, 2]
        m.rescan()
        self.assertIsNone(m.seat_of(m.devs["usb:1-3"])[0])

    def test_unplug_drops_runtime(self):
        m = self.t.manager()
        m.rescan()
        m.assign("usb:1-3", 1)
        self.assertIn("usb:1-3", m.runtime)
        shutil.rmtree(os.path.join(self.t.sys, PCI_USB, "1-3"))
        for cls in ("class/input/event2", "class/input/mouse0", "class/hidraw/hidraw1", "bus/usb/devices/1-3"):
            os.unlink(os.path.join(self.t.sys, cls))
        m.rescan()
        self.assertNotIn("usb:1-3", m.runtime)
        self.assertNotIn("usb:1-3", m.devs)

    def test_remember_port_and_forget(self):
        m = self.t.manager()
        m.rescan()
        j = m.assign("usb:1-3", 2, "port")
        self.assertEqual((j["seat"], j["how"]), (2, "port rule"))
        with open(m.conf_path) as fh:
            self.assertEqual(json.load(fh)["rules"][0]["match"], "pci-0000:0a:00.1-usb-0:3")
        j = m.assign("usb:1-3", 1, "device")            # one rule per device: the port rule goes
        self.assertEqual([r["kind"] for r in m.conf["rules"]], ["device"])
        self.assertEqual((j["seat"], j["how"]), (1, "device rule"))
        j = m.forget("usb:1-3")
        self.assertEqual((j["seat"], m.conf["rules"]), (None, []))

    def test_assign_refuses_host_device_and_unknown_seat(self):
        self.t.usb("2-4", "0bda", "8156", 2, 3, "USB LAN", driver="r8152")
        m = self.t.manager()
        m.rescan()
        with self.assertRaises(ValueError):
            m.assign("usb:2-4", 1)
        with self.assertRaises(ValueError):
            m.assign("usb:1-3", 9)

    def test_claim_takes_first_press_on_unassigned(self):
        m = self.t.manager()
        m.rescan()
        m.claim(2, 10)
        m.on_input("usb:1-3", press=False)                # motion: activity only
        self.assertIsNone(m.seat_of(m.devs["usb:1-3"])[0])
        self.assertTrue(m.state_device("usb:1-3")["active"])
        m.on_input("usb:1-3", press=True)
        self.assertEqual(m.seat_of(m.devs["usb:1-3"]), (2, "claim"))
        self.assertEqual(m.claim_results[2]["id"], "usb:1-3")
        self.assertNotIn(2, m.claims)

    def test_claim_ignores_assigned_devices(self):
        m = self.t.manager()
        m.rescan()
        m.assign("usb:1-3", 1)
        m.claim(2, 10)
        m.on_input("usb:1-3", press=True)
        self.assertEqual(m.seat_of(m.devs["usb:1-3"])[0], 1)
        self.assertIn(2, m.claims)

    def test_apply_wants_nodes_of_running_seats_only(self):
        m = self.t.manager()
        m.rescan()
        m.assign("usb:1-3", 1)
        self.t.ct_running(3101)
        calls = []
        orig, orig_run = sm.ct_init_pid, sm.Applier._run
        sm.ct_init_pid = lambda ct: 4242
        os.makedirs(os.path.join(self.t.proc, "4242"), exist_ok=True)
        try:
            real_exists = os.path.exists
            os.path.exists = lambda p: True if p == "/proc/4242" else real_exists(p)
            sm.Applier._run = lambda self, *argv: calls.append(argv) or True
            m.apply()
        finally:
            sm.ct_init_pid, sm.Applier._run = orig, orig_run
            os.path.exists = real_exists
        added = sorted(a[-1] for a in calls if a[0] == "lxc-device")
        self.assertEqual(added, ["/dev/bus/usb/001/003", "/dev/hidraw1", "/dev/input/event12", "/dev/input/event2",
                                 "/dev/input/mouse0", "/dev/snd/controlC1", "/dev/snd/pcmC1D3p"])
        self.assertNotIn(3102, m.appliers)                # not running: nothing applied


class VirtualTest(unittest.TestCase):
    """uinput/uhid devices created inside a seat container: they follow their creator."""

    def setUp(self):
        self.t = Tree()
        self.t.mouse()
        self.t.ct_running(3101)
        self.t.ct_running(3102)
        self.holders = {}
        self.orig = sm.fd_holders
        sm.fd_holders = lambda P, cts, path: {k: ct for k, (ct, p) in self.holders.items() if p == path and ct in cts}

    def tearDown(self):
        sm.fd_holders = self.orig
        self.t.close()

    def uinput(self, n, minor, name="rdx: Keyboard"):
        inp = "devices/virtual/input/input%d" % n
        self.t.w(inp + "/name", name)
        self.t.w(inp + "/capabilities/ev", "3")
        self.t.w(inp + "/capabilities/key", "ffff 0")
        self.t.cnode("input", "event%d" % (minor - 64), inp + "/event%d" % (minor - 64), "input/event%d" % (minor - 64), 13, minor)

    def uhid(self, hid, hidraw, n, minor):
        d = "devices/virtual/misc/uhid/" + hid
        self.t.w(d + "/uevent", "HID_NAME=rdx:Pad\nHID_ID=0003:00001209:00000001")
        self.t.cnode("hidraw", "hidraw%d" % hidraw, d + "/hidraw/hidraw%d" % hidraw, "hidraw%d" % hidraw, 238, hidraw)
        inp = d + "/input/input%d" % n
        self.t.w(inp + "/name", "rdx:Pad")
        self.t.w(inp + "/capabilities/ev", "b")
        self.t.w(inp + "/capabilities/key", "ffff 0")
        self.t.cnode("input", "event%d" % (minor - 64), inp + "/event%d" % (minor - 64), "input/event%d" % (minor - 64), 13, minor)

    def drop(self, rel_dir, *classlinks):
        shutil.rmtree(os.path.join(self.t.sys, rel_dir))
        for c in classlinks:
            os.unlink(os.path.join(self.t.sys, c))

    def test_grouping(self):
        self.uinput(50, 84)
        self.uhid("0003:1209:0001.0007", 5, 51, 85)
        devs = sm.scan(self.t.P)
        self.assertEqual(sorted(n.dev for n in devs["virtual:input50"].nodes), ["/dev/input/event20"])
        u = devs["virtual:uhid-0003:1209:0001.0007"]
        self.assertEqual(sorted(n.dev for n in u.nodes), ["/dev/hidraw5", "/dev/input/event21"])
        self.assertEqual((u.virtual, u.name, u.assignable()), ("uhid", "rdx:Pad", False))
        self.assertEqual(devs["virtual:input50"].virtual, "uinput")

    def test_owner_from_the_open_fd(self):
        self.holders = {"900:5": (3102, "/dev/uinput")}
        self.uinput(50, 84)
        m = self.t.manager()
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["virtual:input50"]), (2, "created in this seat"))
        self.assertEqual(m.vowner["virtual:input50"]["fd"], "900:5")
        st = m.state()
        self.assertEqual([d["seat"] for d in st["devices"] if d["id"] == "virtual:input50"], [2])
        # a second device of another container: only its fd is still unmatched
        self.holders["700:9"] = (3101, "/dev/uinput")
        self.uinput(52, 86, "Steam Virtual Gamepad")
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["virtual:input52"])[0], 1)
        # survives a seatmgr restart (virtual.json)
        m2 = self.t.manager()
        m2.rescan()
        self.assertEqual(m2.seat_of(m2.devs["virtual:input50"])[0], 2)
        self.assertEqual(m2.seat_of(m2.devs["virtual:input52"])[0], 1)
        # closed: gone with its owner record
        self.drop("devices/virtual/input/input50", "class/input/event20")
        m2.rescan()
        self.assertNotIn("virtual:input50", m2.vowner)

    def test_ambiguous_goes_nowhere_until_clear(self):
        self.holders = {"900:5": (3102, "/dev/uinput"), "700:9": (3101, "/dev/uinput")}
        self.uinput(50, 84)
        m = self.t.manager()
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["virtual:input50"]), (None, "virtual, creator not known"))
        st = m.state()
        self.assertNotIn("virtual:input50", [d["id"] for d in st["devices"]])     # never "unassigned"
        self.assertIn("several containers", st["virtual_pending"][0]["why"])
        del self.holders["700:9"]                     # the other container's fd closed
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["virtual:input50"])[0], 2)

    def test_uhid_is_matched_on_dev_uhid_only(self):
        self.holders = {"900:5": (3102, "/dev/uinput"), "800:4": (3101, "/dev/uhid")}
        self.uhid("0003:1209:0001.0007", 5, 51, 85)
        m = self.t.manager()
        m.rescan()
        self.assertEqual(m.seat_of(m.devs["virtual:uhid-0003:1209:0001.0007"])[0], 1)

    def test_no_assign_no_claim_no_default(self):
        self.t.monitor(True)
        self.holders = {"900:5": (3102, "/dev/uinput")}
        self.uinput(50, 84)
        m = self.t.manager()
        m.rescan()
        with self.assertRaises(ValueError):
            m.assign("virtual:input50", 1)
        with self.assertRaises(ValueError):
            m.forget("virtual:input50")
        self.assertNotIn("virtual:input50", m.runtime)             # no monitor default
        self.holders = {}
        self.uinput(53, 87)                                         # nobody holds a fd: creator unknown
        m.rescan()
        m.claim(1, 10)
        m.on_input("virtual:input53", press=True)
        self.assertIsNone(m.seat_of(m.devs["virtual:input53"])[0])  # a claim never takes it
        self.assertIn(1, m.claims)

    def test_apply_sends_it_to_the_creator(self):
        self.holders = {"900:5": (3102, "/dev/uinput")}
        self.uinput(50, 84)
        m = self.t.manager()
        calls = []
        orig, orig_run = sm.ct_init_pid, sm.Applier._run
        real_exists = os.path.exists
        sm.ct_init_pid = lambda ct: 4000 + ct
        os.path.exists = lambda p: True if p.startswith("/proc/7") else real_exists(p)
        sm.Applier._run = lambda self, *argv: calls.append(argv) or True
        try:
            m.rescan()
        finally:
            sm.ct_init_pid, sm.Applier._run = orig, orig_run
            os.path.exists = real_exists
        self.assertIn(("lxc-device", "-n", "3102", "add", "/dev/input/event20"), calls)
        self.assertNotIn(("lxc-device", "-n", "3101", "add", "/dev/input/event20"), calls)


class ApplierTest(unittest.TestCase):
    def test_card_is_announced_after_its_nodes_and_withdrawn_first(self):
        t = Tree()
        try:
            t.gpu_sound()
            dv = sm.scan(t.P)["sound:0000:0d:00.1"]
            ap = sm.Applier(t.P, 3101, dry=True)
            ap.pid = 1
            events = []
            ap._run = lambda *argv: events.append(("run",) + argv[3:]) or True
            ap._send = lambda props: events.append((props["ACTION"], props["DEVPATH"].rsplit("/", 1)[1]))
            want = {n.key(): n for n in dv.nodes}
            ap.sync(want, set(dv.cards))
            self.assertEqual(events[-1], ("change", "card1"))          # after every node
            self.assertEqual(ap.card_props(dv.cards[0], "change")["SOUND_INITIALIZED"], "1")
            events.clear()
            ap.sync({}, set())
            self.assertEqual(events[0], ("remove", "card1"))           # before the nodes go
            self.assertEqual(ap.cards, set())
        finally:
            t.close()

    def test_event_node_waits_for_the_hosts_udev_record(self):
        """the kernel uevent's rescan announced the window keyboard to CT 3102 before
        udevd had written c13:87; libinput in gamescope never took it"""
        t = Tree()
        try:
            t.mouse()
            dv = sm.scan(t.P)["usb:1-3"]
            ev = next(n for n in dv.nodes if n.dev == "/dev/input/event2")
            rec = os.path.join(t.udev, "c13:66")
            os.rename(rec, rec + ".later")                               # udevd not done yet
            ap = sm.Applier(t.P, 3101, dry=True)
            ap.pid = 1
            ap.wait_udev = True
            calls = []
            ap._run = lambda *argv: calls.append(argv[3:]) or True
            self.assertFalse(ap.add(ev, now=10.0))
            self.assertFalse(ap.add(ev, now=11.5))
            self.assertEqual(calls, [])                                  # not announced bare
            os.rename(rec + ".later", rec)                               # udevd wrote it
            self.assertTrue(ap.add(ev, now=12.0))
            self.assertEqual(calls, [("add", "/dev/input/event2")])
            self.assertEqual(ap.unready, {})
            ap.applied.clear()
            os.rename(rec, rec + ".later")                               # never comes: in bare after 3 s
            self.assertFalse(ap.add(ev, now=20.0))
            self.assertTrue(ap.add(ev, now=23.1))
            mouse = next(n for n in dv.nodes if n.dev.startswith("/dev/input/mouse"))
            self.assertTrue(ap.add(mouse, now=30.0))                     # only event nodes wait
        finally:
            t.close()

    def test_node_follows_the_hosts_late_group_and_mode(self):
        """CT 3102: udevd set the window pointer's GROUP/MODE after it went in; the
        container's node stayed root 0600, gamescope (the desktop user) got EACCES. The next pass fixes it and
        announces the node again so that libinput opens it"""
        P = sm.Paths("/nonexistent-sys", "/nonexistent-dev", "/nonexistent-udev", "/nonexistent-proc")
        ap = sm.Applier(P, 3102, dry=True)
        ap.pid = 1
        ap.wait_udev = False
        sent = []
        ap._run = lambda *argv: True
        ap._uevent = lambda action, nd: sent.append((action, nd.dev))
        early = sm.Node("/dev/input/event22", 13, 86, 0o600, 0, "input")
        ap.sync({early.key(): early})
        self.assertEqual(sent, [("add", "/dev/input/event22")])
        sent.clear()
        ap.sync({early.key(): early})                                  # nothing changed: nothing sent
        self.assertEqual(sent, [])
        late = sm.Node("/dev/input/event22", 13, 86, 0o660, 996, "input")
        ap.sync({late.key(): late})
        self.assertEqual(sent, [("remove", "/dev/input/event22"), ("add", "/dev/input/event22")])
        self.assertEqual((ap.applied[late.key()].mode, ap.applied[late.key()].gid), (0o660, 996))

    def test_sync_removes_and_revokes_before_adding(self):
        P = sm.Paths("/nonexistent-sys", "/nonexistent-dev", "/nonexistent-udev", "/nonexistent-proc")
        ap = sm.Applier(P, 3101, dry=True)
        ap.pid = 1
        calls = []
        ap._run = lambda *argv: calls.append(argv) or True
        a = sm.Node("/dev/input/event2", 13, 66, 0o660, 101, "input")
        b = sm.Node("/dev/input/event7", 13, 71, 0o660, 101, "input")
        ap.sync({a.key(): a})
        ap.sync({b.key(): b})
        self.assertEqual(calls, [("lxc-device", "-n", "3101", "add", "/dev/input/event2"),
                                 ("lxc-cgroup", "-n", "3101", "devices.deny", "c 13:66 rwm"),
                                 ("lxc-device", "-n", "3101", "add", "/dev/input/event7")])
        self.assertEqual(list(ap.applied), [b.key()])


MURMUR_C = r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
/* MurmurHash2 by Austin Appleby (public domain), as systemd's string_hash32 uses it */
static uint32_t MurmurHash2(const void *key, int len, uint32_t seed) {
  const uint32_t m = 0x5bd1e995; const int r = 24; uint32_t h = seed ^ len;
  const unsigned char *data = (const unsigned char *)key;
  while (len >= 4) { uint32_t k; memcpy(&k, data, 4); k *= m; k ^= k >> r; k *= m; h *= m; h ^= k; data += 4; len -= 4; }
  switch (len) { case 3: h ^= data[2] << 16; /* fall through */ case 2: h ^= data[1] << 8; /* fall through */
                 case 1: h ^= data[0]; h *= m; }
  h ^= h >> 13; h *= m; h ^= h >> 15; return h; }
int main(int c, char **v) { for (int i = 1; i < c; i++) printf("%u\n", MurmurHash2(v[i], strlen(v[i]), 0)); return 0; }
"""


PROC_DEVICES = """Character devices:
  1 mem
 10 misc
195 nvidia
195 nvidia-modeset
195 nvidiactl
231 infiniband_mad
231 infiniband_verbs
%d nvidia-uvm
505 infiniband_mad
506 nvidia-nvswitch
507 nvidia-nvlink
%d nvidia-caps
510 fwctl

Block devices:
259 blkext
"""


class NvidiaMajorTest(unittest.TestCase):
    """The dynamic nvidia-uvm / nvidia-caps majors: current ones allowed, stale config allows revoked."""

    def setUp(self):
        self.t = Tree()
        self.t.P.pve = os.path.join(self.t.root, "pve")
        os.makedirs(self.t.P.pve)
        with open(os.path.join(self.t.P.pve, "3103.conf"), "w") as fh:
            fh.write("arch: amd64\n"
                     "lxc.cgroup2.devices.allow: c 195:254 rwm\n"
                     "lxc.cgroup2.devices.allow: c 505:* rwm\n"
                     "lxc.cgroup2.devices.allow: c 510:* rwm\n"
                     "lxc.cgroup2.devices.allow: c 10:* rwm\n"
                     "\n[snap1]\nlxc.cgroup2.devices.allow: c 499:* rwm\n")

    def tearDown(self):
        self.t.close()

    def devices(self, uvm, caps):
        with open(os.path.join(self.t.proc, "devices"), "w") as fh:
            fh.write(PROC_DEVICES % (uvm, caps))
        return sm.char_majors(self.t.P)

    def test_grant_revoke_follow(self):
        ap = sm.Applier(self.t.P, 3103)
        calls = []
        ap._run = lambda *argv: calls.append(argv[3:]) or True
        majors = self.devices(502, 508)
        self.assertEqual(sm.nvidia_majors(majors), {"nvidia-uvm": 502, "nvidia-caps": 508})
        self.assertEqual(majors[505], {"infiniband_mad"})    # the second major of that name is kept
        self.assertNotIn(259, majors)                        # block majors are another namespace
        ap.nvidia(majors)
        # after an RDMA NIC went in: the old uvm/caps numbers are the NIC's umad (505) and fwctl (510) now;
        # 10:* is a fixed major and stays
        self.assertEqual(calls, [("devices.allow", "c 502:* rwm"), ("devices.allow", "c 508:* rwm"),
                                 ("devices.deny", "c 505:* rwm"), ("devices.deny", "c 510:* rwm")])
        calls.clear()
        ap.nvidia(majors)
        self.assertEqual(calls, [])                          # unchanged: nothing to do
        ap.nvidia(self.devices(500, 508))                    # driver reloaded while the container runs
        self.assertEqual(calls, [("devices.allow", "c 500:* rwm"), ("devices.deny", "c 502:* rwm"),
                                 ("devices.deny", "c 505:* rwm"), ("devices.deny", "c 510:* rwm")])

    def test_stale_names(self):
        majors = self.devices(502, 508)
        with open(os.path.join(self.t.P.pve, "3102.conf"), "w") as fh:
            fh.write("lxc.cgroup2.devices.allow: c 502:* rwm\nlxc.cgroup2.devices.allow: c 506:* rwm\n"
                     "lxc.cgroup2.devices.allow: c 1:* rwm\nlxc.cgroup2.devices.allow: c 195:* rwm\n"
                     "lxc.cgroup2.devices.allow: c 510:* rwm\nlxc.cgroup2.devices.allow: c 240:* rwm\n")
        # dynamic and not uvm/caps: 506 (nvswitch), 510 (fwctl), 240 (unregistered); fixed 1 and 195 stay
        self.assertEqual(sm.stale_majors(self.t.P, 3102, majors), {240, 506, 510})

    def test_stale_in_the_gpu_include(self):
        # the GPU include rdx_seat_gpu.py writes at a start (lxc syntax) counts like the config's own lines;
        # lxc's own includes do not
        majors = self.devices(500, 508)
        inc = os.path.join(self.t.root, "3101.gpu.conf")
        with open(inc, "w") as fh:
            fh.write("# rdx-seat-gpu\nlxc.cgroup2.devices.allow = c 195:0 rwm\n"
                     "lxc.cgroup2.devices.allow = c 502:* rwm\nlxc.cgroup2.devices.allow = c 508:* rwm\n")
        with open(os.path.join(self.t.P.pve, "3101.conf"), "w") as fh:
            fh.write("lxc.include: /usr/share/lxc/config/nonexistent.conf\nlxc.include: %s\n"
                     "lxc.cgroup2.devices.allow: c 10:223 rwm\n" % inc)
        self.assertEqual(sm.conf_whole_majors(self.t.P, 3101), {502, 508})
        self.assertEqual(sm.stale_majors(self.t.P, 3101, majors), {502})      # uvm moved to 500


class GrabGuardTest(unittest.TestCase):
    """Who grabs a seat keyboard (module docstring): with no gamescope that would grab it seatmgr
    holds it, a gamescope that would grab it gets it at once and is checked GRAB_GRACE_S after it
    appeared. Fake probe, hold, gamescope list and client test; time passed in."""

    INP = PCI_USB + "/1-3/1-3:1.0/0003:093A:2510.0002/input/input66"

    def setUp(self):
        self.t = Tree()
        self.t.mouse()
        # the PixArt "mouse" also has keyboard keys here (many mice do: macro keys) -> keyboard-capable
        self.t.w(self.INP + "/capabilities/key", "70000 0 0 0 0 0 0 0 fffffffffffffffe")
        if not os.path.exists(os.path.join(self.t.sys, "class/input/event2/device")):
            self.t.link("class/input/event2/device", self.INP)
        self.t.ct_running(3101)
        self.m = self.t.manager()
        self.m.conf["host_console_keyboard"] = True   # the consoles read keyboards: the guard acts
        self.m.rescan()
        self.m.assign("usb:1-3", 1)
        self.state = {"/dev/input/event2": "free"}
        self.gs = {3101: {}}
        self.client = False
        self.held, self.closed = [], []
        self.m.grab_probe = lambda path: self.state.get(path)
        self.m.grab_hold = lambda path: self.held.append(path) or os.open("/dev/null", os.O_RDONLY)
        self.m.gamescopes = lambda ct: self.gs.get(ct, {})
        self.m.is_client = lambda path: self.client
        self.fds = {}                                  # pid -> fds it has open on the device
        self.m.fds_on = lambda pid, rdevs: {(pid, f) for f in self.fds.get(pid, ())}

    def tearDown(self):
        self.t.close()

    def w(self, did="usb:1-3"):
        return self.m.withheld.get(did)

    def test_keyboard_and_client_detection(self):
        self.assertTrue(sm.node_is_keyboard(self.t.P, "/dev/input/event2"))
        self.assertFalse(sm.node_is_keyboard(self.t.P, "/dev/input/mouse0"))   # no capabilities there
        self.assertFalse(sm.node_is_client(self.t.P, "/dev/input/event2"))    # "PixArt ...", usb phys
        self.t.w(self.INP + "/name", "rdx: window keyboard (desktop)")
        self.assertTrue(sm.node_is_client(self.t.P, "/dev/input/event2"))
        self.t.w(self.INP + "/capabilities/key", "70000 0 0 0 0")
        self.assertFalse(sm.node_is_keyboard(self.t.P, "/dev/input/event2"))   # BTN_LEFT.. only: a mouse

    def test_no_gamescope_physical_keyboard_is_held_at_once(self):
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")
        self.assertEqual(self.held, ["/dev/input/event2"])
        self.assertIn("no gamescope of seat 1 reads it", self.m.state_device("usb:1-3")["withheld"])
        self.assertEqual(self.m.seat_of(self.m.devs["usb:1-3"])[0], 1)          # the assignment stays
        self.m.guard_pass(now=1)
        self.assertEqual(self.held, ["/dev/input/event2"])                       # once

    def test_drm_gamescope_gets_it_at_once_and_is_checked_after_its_grace(self):
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")
        self.gs[3101] = {500: {"backend": "drm", "grab": True}}                 # the seat monitor starts
        self.m.guard_pass(now=10)
        self.assertIsNone(self.w())                                              # let go in the same pass
        self.m.guard_pass(now=14.9)                                              # free, but within its grace
        self.assertIsNone(self.w())
        self.state["/dev/input/event2"] = "grabbed"
        for t in (15.1, 16.2, 30):
            self.m.guard_pass(now=t)
        self.assertIsNone(self.w())
        self.state["/dev/input/event2"] = "free"                                 # it let go of it (a fault)
        self.m.guard_pass(now=31)
        self.assertIsNone(self.w())                                              # first seen free
        self.m.guard_pass(now=32.1)
        self.assertEqual(self.w()["kind"], "nograb")
        self.assertIn("not grabbed by the gamescope of seat 1", self.w()["why"])
        for t in (33, 40, 60):                                                   # the same gamescope: stays held
            self.m.guard_pass(now=t)
        self.assertEqual(self.w()["kind"], "nograb")
        self.gs[3101] = {501: {"backend": "drm", "grab": True}}                 # a new one
        self.m.guard_pass(now=61)
        self.assertIsNone(self.w())

    def test_rdx_backend_does_not_read_a_local_keyboard(self):
        self.gs[3101] = {600: {"backend": "rdx", "grab": True}}                 # a stream runs
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")                               # nobody reads it: held
        self.m.guard_pass(now=20)
        self.assertEqual(self.w()["kind"], "idle")

    def test_rdx_name_without_gamescope_is_held_whatever_made_it(self):
        """seen: "rdx: o3 vt key", made in the seat by some process, reached tty11 while seatmgr left
        "rdx:" names to the gate. Now: held at once like any keyboard nobody would grab."""
        self.client = True
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")
        self.assertEqual(self.held, ["/dev/input/event2"])
        self.gs[3101] = {700: {"backend": "rdx", "grab": True}}                 # a stream's gamescope would grab it
        self.m.guard_pass(now=3)
        self.assertIsNone(self.w())                                              # let go at once
        self.m.guard_pass(now=6)                                                 # 3 s after gamescope: its grace
        self.assertIsNone(self.w())
        self.m.guard_pass(now=8.1)                                               # grace over: free seen
        self.m.guard_pass(now=9.2)
        self.assertEqual(self.w()["kind"], "nograb")

    def test_a_device_its_gate_holds_already_is_not_recorded(self):
        self.client = True
        self.m.grab_hold = lambda path: None                                     # EBUSY: rx / usb_host hold it
        self.m.guard_pass(now=0)
        self.assertIsNone(self.w())
        self.assertIn("usb:1-3", self.m.gated)
        self.m.grab_hold = lambda path: self.held.append(path) or os.open("/dev/null", os.O_RDONLY)
        self.m.guard_pass(now=0.2)                                               # the gate let go: seatmgr holds it
        self.assertEqual(self.w()["kind"], "idle")
        self.assertNotIn("usb:1-3", self.m.gated)

    def test_kernel_uevent_of_a_new_input_node(self):
        self.assertTrue(sm.uevent_is_input_add("KERNEL[5170.442841] add      /devices/virtual/input/input170/event21 (input)"))
        self.assertFalse(sm.uevent_is_input_add("KERNEL[5170.442800] add      /devices/virtual/input/input170 (input)"))
        self.assertFalse(sm.uevent_is_input_add("UDEV  [5170.450000] add      /devices/virtual/input/input170/event21 (input)"))
        self.assertFalse(sm.uevent_is_input_add("KERNEL[5170.442841] remove   /devices/virtual/input/input170/event21 (input)"))

    def test_a_device_that_comes_to_a_running_gamescope_gets_its_grace(self):
        """seen: a window client's keyboard came on resume to a gamescope running for 58 s and was
        held 1 s later -- the grace must count from the device's arrival too"""
        self.m.devs_backup = dict(self.m.devs)
        self.m.devs.pop("usb:1-3")                                               # not there yet
        self.gs[3101] = {900: {"backend": "drm", "grab": True}}
        self.m.guard_pass(now=0)                                                 # gamescope runs
        self.m.devs.update(self.m.devs_backup)                                   # the device comes at 58 s
        self.m.guard_pass(now=58)
        self.m.guard_pass(now=59.2)
        self.m.guard_pass(now=62.9)                                              # free, but within its grace
        self.assertIsNone(self.w())
        self.state["/dev/input/event2"] = "grabbed"                              # gamescope took it
        for t in (63.1, 64.2, 70):
            self.m.guard_pass(now=t)
        self.assertIsNone(self.w())

    def test_a_late_open_by_its_gamescope_lets_go_at_once(self):
        """held as not grabbed, then gamescope's libinput opens it (hotplug, resume): its grab
        retries 0.5 s on EBUSY, so seatmgr lets go in the next pass and holds again only if that
        open does not grab it either"""
        self.gs[3101] = {900: {"backend": "drm", "grab": True}}
        self.fds[900] = {"31"}                                                   # some other open, before
        for t in (0, 5.1, 6.2):
            self.m.guard_pass(now=t)
        self.assertEqual(self.w()["kind"], "nograb")
        self.m.guard_pass(now=6.4)
        self.assertEqual(self.w()["kind"], "nograb")                             # nothing new opened
        self.fds[900] = {"31", "44"}                                             # libinput opens it now
        self.m.guard_pass(now=6.6)
        self.assertIsNone(self.w())
        self.state["/dev/input/event2"] = "grabbed"                              # its grab got it
        for t in (6.8, 7.7, 9):
            self.m.guard_pass(now=t)
        self.assertIsNone(self.w())
        self.state["/dev/input/event2"] = "free"                                 # opened, never grabbed
        for t in (10, 11.1):
            self.m.guard_pass(now=t)
        self.assertEqual(self.w()["kind"], "nograb")
        for t in (11.3, 12.5, 14):                                               # the same opens: no flapping
            self.m.guard_pass(now=t)
        self.assertEqual(self.w()["kind"], "nograb")

    def test_grab_disabled_counts_as_no_grabber(self):
        self.gs[3101] = {800: {"backend": "drm", "grab": False}}                # RDX_GS_GRAB=0
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")

    def test_joystick_is_never_held(self):
        with open(os.path.join(self.t.udev, "c13:66"), "a") as fh:
            fh.write("E:ID_INPUT_JOYSTICK=1\n")
        self.m.guard_pass(now=0)
        self.assertIsNone(self.w())

    def test_virtual_keyboard_made_in_the_seat_is_held_too(self):
        self.m.devs["usb:1-3"].kind = "virtual"       # as if created in the container (Steam Input)
        self.m.vowner["usb:1-3"] = {"ct": 3101, "fd": "1:3", "t": 0}
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")

    def test_observes_only_while_the_consoles_keyboards_are_off(self):
        self.m.conf["host_console_keyboard"] = False  # K_OFF protects the consoles
        for t in (0, 5, 9):
            self.m.guard_pass(now=t)
        self.assertIsNone(self.w())
        self.assertEqual(self.held, [])
        self.assertIn("usb:1-3", self.m.observed)
        self.m.conf["grab_guard"] = "enforce"
        self.m.guard_pass(now=10)
        self.assertEqual(self.w()["kind"], "idle")

    def test_host_console_device_is_not_watched_and_a_held_one_is_let_go(self):
        self.m.guard_pass(now=0)
        self.assertEqual(self.w()["kind"], "idle")
        self.m.assign("usb:1-3", "host")
        self.m.guard_pass(now=1)
        self.assertIsNone(self.w())


class ConsoleKeyboardTest(unittest.TestCase):
    """The host's VTs read no keyboard (KDSKBMODE K_OFF), through a fake open/ioctl seam."""

    def setUp(self):
        self.t = Tree()
        self.modes = {}                 # tty path -> keyboard mode
        self.fds = {}                   # fd (a real /dev/null fd) -> tty path
        for n, mode in ((1, 3), (2, 1), (3, sm.K_OFF), (13, 3)):     # XLATE, RAW, off; tty13 is active
            self.modes[os.path.join(self.t.dev, "tty%d" % n)] = mode
        self.t.w("class/tty/tty0/active", "tty13")
        self.state = os.path.join(self.t.root, "run/vt.json")
        self.old = os.path.join(self.t.root, "run-old.json")

    def tearDown(self):
        for fd in list(self.fds):
            try:
                os.close(fd)
            except OSError:
                pass
        self.t.close()

    def open_(self, path):
        if path not in self.modes:
            raise FileNotFoundError(path)
        fd = os.open("/dev/null", os.O_RDONLY)
        self.fds[fd] = path
        return fd

    def ioctl_(self, fd, req, arg):
        path = self.fds[fd]
        if req == sm.KDGKBMODE:
            return struct.pack("i", self.modes[path])
        if req == sm.KDSKBMODE:
            self.modes[path] = arg
            return 0
        raise OSError("unexpected ioctl %#x" % req)

    def kb(self):
        return sm.ConsoleKeyboard(self.t.P, state=self.state, old_state=self.old, open_=self.open_, ioctl_=self.ioctl_)

    def test_off_and_kept_off(self):
        kb = self.kb()
        changed = kb.enforce()
        self.assertEqual(sorted(changed), ["tty1 3", "tty13 3", "tty2 1"])
        self.assertTrue(all(m == sm.K_OFF for m in self.modes.values()))
        self.assertEqual(json.load(open(self.state)), {os.path.join(self.t.dev, "tty1"): 3,
                                                       os.path.join(self.t.dev, "tty2"): 1,
                                                       os.path.join(self.t.dev, "tty13"): 3})
        self.assertEqual(kb.enforce(), [])                       # every second: nothing to do
        self.modes[os.path.join(self.t.dev, "tty1")] = 3         # agetty restarted / chvt / gamescope gave it back
        self.assertEqual(kb.enforce(), ["tty1 3"])
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], sm.K_OFF)
        self.assertEqual(kb.saved[os.path.join(self.t.dev, "tty1")], 3)    # the first mode seen is kept

    def test_restore_on_request(self):
        kb = self.kb()
        kb.enforce()
        kb.restore()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], 3)
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty2")], 1)
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty3")], sm.K_UNICODE)  # off without a mode seen: unicode
        self.assertEqual(json.load(open(self.state)), {})
        self.modes[os.path.join(self.t.dev, "tty1")] = 2
        kb.restore()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], 2)          # once, not every second

    def test_a_console_off_without_a_saved_mode_gets_unicode(self):
        self.modes[os.path.join(self.t.dev, "tty1")] = sm.K_OFF               # lost state file
        kb = self.kb()
        kb.restore()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], sm.K_UNICODE)
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty2")], 1)      # not off: untouched

    def test_modes_of_the_first_mitigation(self):
        with open(self.old, "w") as fh:          # /run/rdx-vt-kbd-modes: the consoles were switched off by hand
            json.dump({os.path.join(self.t.dev, "tty1"): 3}, fh)
        self.modes[os.path.join(self.t.dev, "tty1")] = sm.K_OFF
        kb = self.kb()
        kb.restore()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], 3)

    def test_manager_switch(self):
        m = self.t.manager()
        m.console = self.kb()
        m.conf["host_console_keyboard"] = False                                     # the lock option
        m.console_pass()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], sm.K_OFF)
        m.conf["host_console_keyboard"] = True
        m.console_pass()
        self.assertEqual(self.modes[os.path.join(self.t.dev, "tty1")], 3)
        self.assertIs(sm.DEFAULT_CONF["host_console_keyboard"], True)    # the seats grab: consoles on
        st = m.state()
        self.assertTrue(st["host_console_keyboard"])


class LxcInfoTest(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(sm.parse_lxc_info("State:          RUNNING\nPID:            16552\n"), 16552)
        self.assertEqual(sm.parse_lxc_info("RUNNING\n16552\n"), 16552)
        self.assertEqual(sm.parse_lxc_info("State:          STOPPED\n"), 0)
        self.assertEqual(sm.parse_lxc_info(""), 0)


class WireTest(unittest.TestCase):
    WORDS = ["input", "sound", "hidraw", "usb", "usb_device", "a", "ab", "abc", "abcd", "abcde"]

    @unittest.skipUnless(shutil.which("gcc"), "no gcc")
    def test_murmur2_matches_reference(self):
        d = tempfile.mkdtemp()
        try:
            with open(os.path.join(d, "m.c"), "w") as fh:
                fh.write(MURMUR_C)
            subprocess.run(["gcc", "-O1", "-o", os.path.join(d, "m"), os.path.join(d, "m.c")], check=True)
            ref = subprocess.run([os.path.join(d, "m")] + self.WORDS, capture_output=True, text=True, check=True).stdout.split()
        finally:
            shutil.rmtree(d)
        self.assertEqual([sm.murmur2(w.encode()) for w in self.WORDS], [int(x) for x in ref])

    def test_udev_message_layout(self):
        msg = sm.udev_message({"ACTION": "add", "SUBSYSTEM": "input", "DEVPATH": "/devices/x/event2"})
        self.assertEqual(msg[:8], b"libudev\0")
        self.assertEqual(struct.unpack_from("!I", msg, 8)[0], 0xfeedcafe)
        hsize, off, plen = struct.unpack_from("=III", msg, 12)
        self.assertEqual((hsize, off), (40, 40))
        self.assertEqual(msg[off:off + plen], b"ACTION=add\0SUBSYSTEM=input\0DEVPATH=/devices/x/event2\0")
        self.assertEqual(struct.unpack_from("!I", msg, 24)[0], sm.murmur2(b"input"))


class CardChoiceTest(unittest.TestCase):
    """Seat N on card X (module docstring "Card choice"), against rdx_seat_gpu's fake host: an LLM server on
    the 5090 and the 3080a, seat 3 running on the 3080b, nvtop and an exporter holding every card."""

    def setUp(self):
        import test_seat_gpu as tsg
        self.tsg = tsg
        self.t = tsg.Tree()
        G = self.G = tsg
        for ct, seat, pci in ((3101, 1, G.G5090), (3102, 2, G.G3080A), (3103, 3, G.G3080B)):
            self.t.pve(ct)
            self.t.hostd(ct, seat, pci)
        self.t.w(self.t.P.conf, json.dumps({"seats": {"1": {"ct": 3101, "name": "Seat 1", "gpu": G.G5090},
                                                       "2": {"ct": 3102, "name": "Seat 2", "gpu": G.G3080A},
                                                       "3": {"ct": 3103, "name": "Seat 3", "gpu": G.G3080B}}, "rules": [],
                                            "gpuq": "http://booking.example:8770"}))
        P = sm.Paths(self.t.P.sys, self.t.P.dev, os.path.join(self.t.root, "udev"), self.t.P.proc, self.t.P.pve)
        self.m = sm.Manager(self.t.P.conf, os.path.join(self.t.root, "run/runtime.json"), P=P, dry=True)
        self.m.GP = self.t.P
        self.m.docker_root = os.path.join(self.t.root, "docker")
        self.t.w(os.path.join(self.m.docker_root, "afa737e2e6c2" + "0" * 52, "config.v2.json"), json.dumps({"Name": "/sglang"}))
        nv = lambda m: os.makedev(195, m)      # noqa: E731
        drm = lambda m: os.makedev(226, m)     # noqa: E731
        self.apps = [{"pid": 2064348, "pci": G.G5090, "name": "sglang::scheduler_TP0", "mib": 28302},
                     {"pid": 2064349, "pci": G.G3080A, "name": "sglang::scheduler_TP1", "mib": 10978},
                     {"pid": 2120455, "pci": G.G3080B, "name": "/opt/rdx-gs/bin/gamescope", "mib": 103}]
        self.held = {2060058: {nv(0), nv(1)}, 2120455: {nv(2), drm(130)}, 124171: {nv(0), nv(1), nv(2)},
                     7046: {nv(0), nv(1), nv(2)}}
        self.proc(2064348, "sglang::schedul", "/system.slice/docker-afa737e2e6c2%s.scope" % ("0" * 52))
        self.proc(2064349, "sglang::schedul", "/system.slice/docker-afa737e2e6c2%s.scope" % ("0" * 52))
        self.proc(2060058, "python", "/system.slice/docker-afa737e2e6c2%s.scope" % ("0" * 52))
        self.proc(2120455, "gamescope-wl", "/lxc/3103/ns/system.slice/rdx-hostd@3.service")
        self.proc(124171, "nvtop", "/user.slice/user-0.slice/session-707.scope")
        self.proc(7046, "nvidia_gpu_expo", "/lxc/200/ns/system.slice/nvidia-gpu-exporter.service")
        self.gq = [{"uuid": G.UUID[G.G5090], "pci_bus_id": "00000000:15:00.0", "timeline": [
            {"id": "vjwbpu", "owner": "someone-else", "purpose": "a long benchmark", "state": "running",
             "end": {"local": "2030-01-01T12:00:00+01:00"}}]}]
        self.m.compute_apps = lambda: list(self.apps)
        self.m.node_holders = lambda rdevs: {p: h & rdevs for p, h in self.held.items() if h & rdevs}
        self.m.gpuq_cards = lambda: self.gq
        self.calls, self.session, self.fail_start = [], {"session": {"state": "idle"}, "suspended": None, "local": {}}, False
        self.m.run_cmd = self.fake_run
        self.t.lxc_config(3103)
        self.t.running(3103)

    def tearDown(self):
        self.t.close()

    def proc(self, pid, comm, cgroup):
        self.t.w(os.path.join(self.t.P.proc, str(pid), "comm"), comm + "\n")
        self.t.w(os.path.join(self.t.P.proc, str(pid), "cgroup"), "0::%s\n" % cgroup)

    def fake_run(self, argv, **kw):
        self.calls.append(argv)
        ok = types.SimpleNamespace(returncode=0, stdout="", stderr="")
        if argv[:2] == ["pct", "exec"]:
            return types.SimpleNamespace(returncode=0, stdout=json.dumps(self.session), stderr="")
        ct = argv[2] if len(argv) > 2 else ""
        cg = os.path.join(self.t.P.sys, "fs/cgroup/lxc", ct)
        if argv[:2] == ["pct", "shutdown"]:
            if os.path.isdir(cg):
                os.rmdir(cg)
            return ok
        if argv[:2] == ["pct", "start"]:
            if self.fail_start:
                self.fail_start = False
                return types.SimpleNamespace(returncode=255, stdout="", stderr="rdx-seat-gpu: START REFUSED: x")
            sg_out = io.StringIO()
            self.tsg.sg.pre_start(self.t.P, int(ct), out=sg_out)     # the hookscript, as pct runs it
            os.makedirs(cg, exist_ok=True)
            return types.SimpleNamespace(returncode=0, stdout=sg_out.getvalue(), stderr="")
        raise AssertionError(argv)

    def card(self, st, pci):
        return next(c for c in st["cards"] if c["pci"] == pci)

    def conf_gpu(self, n):
        return json.loads(sm.rd(self.t.P.conf))["seats"][str(n)]["gpu"]

    def hostd_gpu(self, ct, seat):
        return json.loads(sm.rd(os.path.join(self.t.root, "rootfs/%d/etc/rdx/hostd-seat%d.json" % (ct, seat))))["gpu"]

    def refused(self, *a, **kw):
        with self.assertRaises(sm.SeatRefused) as cm:
            self.m.seat_start(*a, **kw)
        return str(cm.exception)

    def test_card_states(self):
        G = self.G
        self.gq.append({"uuid": G.UUID[G.G3080A], "timeline": [{"id": "x1", "owner": "someone", "purpose": "a test",
                                                                  "state": "running", "end": {"local": "16:00"}}]})
        self.apps = [a for a in self.apps if a["pci"] != G.G3080A]
        self.held.pop(2060058)
        st = self.m.gpus_state(max_age=0)
        c = self.card(st, G.G5090)
        self.assertEqual((c["status"], c["short"], c["seat"]), ("llm", "RTX 5090", None))
        self.assertIn("used by an LLM server: sglang::scheduler_TP0 (pid 2064348, docker sglang)", c["why"])
        self.assertEqual(sorted(c["monitors"]), ["nvidia_gpu_expo", "nvtop"])     # monitors are no users
        self.assertEqual((self.card(st, G.G3080A)["status"], self.card(st, G.G3080A)["gpuq"][0]["owner"]), ("booked", "someone"))
        self.assertEqual((self.card(st, G.G3080B)["status"], self.card(st, G.G3080B)["seat"]), ("seat", 3))
        self.assertEqual([(s["seat"], s["running"], s["gpu"]) for s in st["seats"]],
                         [(1, False, G.G5090), (2, False, G.G3080A), (3, True, G.G3080B)])
        self.assertEqual(st["seats"][0]["gpus"], [G.G5090, G.G3080A, G.G3080B])
        self.held = {}
        self.apps = []
        self.gq = []
        st = self.m.gpus_state(max_age=0)
        self.assertEqual(self.card(st, G.G5090)["status"], "free")

    def test_seat_1_onto_the_3080b(self):
        G = self.G
        # 3103 runs on it: refused, nothing written, nothing started
        self.assertIn("RTX 3080 (%s): it is seat 3's (CT 3103 runs with it)" % G.G3080B, self.refused(1, G.G3080B))
        self.assertEqual(self.conf_gpu(1), G.G5090)
        self.assertEqual(self.calls, [])
        # 3103 stopped: seat 1 starts on the 3080b, seatmgr.json and its hostd config follow
        os.rmdir(os.path.join(self.t.P.sys, "fs/cgroup/lxc/3103"))
        self.apps = [a for a in self.apps if a["pci"] != G.G3080B]
        self.held.pop(2120455)
        r = self.m.seat_start(1, "19:00.0")
        self.assertEqual((r["ok"], r["gpu"], r["was"], r["restarted"]), (True, G.G3080B, G.G5090, False))
        self.assertEqual(self.calls, [["pct", "start", "3101"]])
        self.assertEqual((self.conf_gpu(1), self.hostd_gpu(3101, 1)), (G.G3080B, G.G3080B))
        self.assertIn("c 226:130 rwm", sm.rd(self.tsg.sg.include_path(self.t.P, 3101)))
        # and seat 3 cannot take it back while seat 1 runs with it
        self.assertIn("it is seat 1's (CT 3101 runs with it)", self.refused(3, G.G3080B))

    def test_the_llm_card_is_refused(self):
        G = self.G
        msg = self.refused(1, G.G5090)
        self.assertIn("RTX 5090 (%s): it is used by an LLM server: sglang::scheduler_TP0 (pid 2064348, docker sglang)" % G.G5090, msg)
        self.assertNotIn("nvtop", msg)
        # the LLM server gone, its booking-service window still running: refused for that
        self.apps = [a for a in self.apps if a["pci"] != G.G5090]
        self.held.pop(2060058)
        self.assertIn("booked in the GPU booking service: someone-else (a long benchmark) until 2030-01-01T12:00:00+01:00",
                      self.refused(1, G.G5090))
        # the caller's own window, or a seat's: not counted
        self.m.seat_start(1, G.G5090, booking="vjwbpu")
        self.gq[0]["timeline"][0]["owner"] = "rdx-seat-1"
        self.m.seat_start(1, G.G5090)
        self.assertEqual(self.conf_gpu(1), G.G5090)

    def test_a_seat_in_use_is_not_stopped_unless_forced(self):
        G = self.G
        self.apps, self.held = [], {}
        self.session = {"session": {"state": "running", "app": "steam", "client": "10.0.1.2"}, "suspended": None, "local": {}}
        self.assertIn("seat 3 is in use: a stream of steam runs (to 10.0.1.2)", self.refused(3, G.G3080A))
        self.assertEqual(self.calls[0][:5], ["pct", "exec", "3103", "--", "curl"])
        self.assertIn("http://127.0.0.1:48290/api/state", self.calls[0][-1])
        self.session = {"session": {"state": "idle"}, "suspended": {"app": "battlenet"}, "local": {}}
        self.assertIn("battlenet is paused there", self.refused(3, G.G3080A))
        self.calls = []
        r = self.m.seat_start(3, G.G3080A, force=True)
        self.assertEqual([c[:3] for c in self.calls], [["pct", "shutdown", "3103"], ["pct", "start", "3103"]])
        self.assertTrue(r["restarted"])
        self.assertEqual((self.conf_gpu(3), self.hostd_gpu(3103, 3)), (G.G3080A, G.G3080A))

    def test_a_failed_start_puts_the_old_card_back(self):
        G = self.G
        self.apps, self.held = [], {}
        self.fail_start = True
        self.assertIn("pct start on RTX 3080 (%s) failed" % G.G3080A, self.refused(3, G.G3080A))
        self.assertEqual((self.conf_gpu(3), self.hostd_gpu(3103, 3)), (G.G3080B, G.G3080B))
        self.assertEqual([c[:3] for c in self.calls[1:]], [["pct", "shutdown", "3103"], ["pct", "start", "3103"],
                                                           ["pct", "start", "3103"]])   # again, on its old card
        self.assertTrue(os.path.isdir(os.path.join(self.t.P.sys, "fs/cgroup/lxc/3103")))

    def test_requests_that_make_no_sense(self):
        G = self.G
        with self.assertRaises(ValueError):
            self.m.seat_start(7, G.G5090)
        with self.assertRaises(ValueError):
            self.m.seat_start(1, "the big one")
        self.assertIn("seat 1 may not take 0000:0d:00.0", self.refused(1, "0000:0d:00.0"))   # the 5090's old address
        conf = json.loads(sm.rd(self.t.P.conf))
        conf["seats"]["1"]["gpus"] = [G.G5090, "0000:0d:00.0"]
        self.t.w(self.t.P.conf, json.dumps(conf))
        self.assertIn("seat 1 may not take %s" % G.G3080B, self.refused(1, G.G3080B))
        self.assertIn("cannot be bound: no PCI device", self.refused(1, "0000:0d:00.0"))

    def test_control_from(self):
        nets = sm.DEFAULT_CONF["control_from"]
        self.assertTrue(sm.control_allowed(nets, "127.0.0.1"))
        self.assertTrue(sm.control_allowed(nets, "10.0.1.2"))
        self.assertTrue(sm.control_allowed(nets, "::ffff:10.0.1.2"))
        self.assertFalse(sm.control_allowed(nets, "192.0.2.89"))
        self.assertFalse(sm.control_allowed(["junk"], "10.0.1.2"))

    def test_irq_pass_only_on_a_change(self):
        seen = []
        orig = sm.sg.irq_sync
        sm.sg.irq_sync = lambda P, running=None, seats=None: seen.append(1) or ["irq 181 (x) -> 8-15"]
        try:
            self.m.irq_pass({3103})
            self.m.irq_pass({3103})
            self.m.irq_pass({3101, 3103})
        finally:
            sm.sg.irq_sync = orig
        self.assertEqual(len(seen), 2)


if __name__ == "__main__":
    unittest.main()
