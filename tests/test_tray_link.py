#!/usr/bin/env python3
"""Tray link warnings (desktop/rdx_link_watch.py) against fake /api/state dicts; no desktop, no GTK.
Also checks that desktop/rdx-tray.py still parses and imports the module.

  tests/test_tray_link.py        exit 0 = PASS
"""
import ast
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "desktop"))
import rdx_link_watch as lw  # noqa: E402


def link(level="ok", message="", downs=0, last_down=None, **kw):
    d = {"known": True, "link_ip": "10.0.0.2", "up": level != "down", "level": level, "message": message,
         "downs": downs, "last_down": last_down, "module": None}
    d.update(kw)
    return d


HOT = "Network module 53.6 °C – warning at 53 °C, the link goes down at 58 °C"
DOWN = "Link down: the network module overheated (54 °C, warning at 53 °C)"
FAILED = "Network card failed (likely overheated): power-cycle needed"
LD = {"at": 1761159476, "reason": "the network module overheated",
      "detail": "Port module event[error]: module 1, Cable error, High Temperature",
      "temp_c": 53.6, "back_at": 1761159544, "down_s": 68}


class Watch(unittest.TestCase):
    def feed(self, w, role, *states):
        return [w.poll(role, s) for s in states]

    def test_ok_at_start_is_silent(self):
        w = lw.LinkWatcher()
        out = self.feed(w, "host", {"link": link()}, {"link": link()}, {"link": link()})
        self.assertEqual(out, [[], [], []])

    def test_missing_or_unknown_link_is_silent(self):
        w = lw.LinkWatcher()
        out = self.feed(w, "client", {}, None, {"link": {"known": False, "message": "no port"}},
                        {"link": link("unknown")}, {"link": link()}, {"link": link("unknown")}, {"link": link()})
        self.assertEqual(out, [[]] * 7)

    def test_each_transition_once(self):
        w = lw.LinkWatcher()
        warn, down_l, ok_l = link("warn", HOT), link("down", DOWN, 1, LD), link("ok", "", 1, LD)
        seq = [{"link": link()}, {"link": warn}, {"link": warn}, {"link": warn},
               {"link": down_l}, {"link": down_l}, {"link": ok_l}, {"link": ok_l}]
        out = self.feed(w, "host", *seq)
        self.assertEqual(out[0], [])
        self.assertEqual(out[1], [("", "own", HOT, False)])            # ok -> warn
        self.assertEqual(out[2], [])
        self.assertEqual(out[3], [])
        self.assertEqual(out[4], [("", "own", DOWN, True)])            # warn -> down
        self.assertEqual(out[5], [])
        self.assertEqual(out[6], [("", "own", "Link back after 1 min 8 s – the network module had overheated", False)])
        self.assertEqual(out[7], [])

    def test_ok_to_alarm_and_alarm_to_ok(self):
        w = lw.LinkWatcher()
        alarm = link("alarm", "Network module 58.2 °C – the link goes down any moment")
        out = self.feed(w, "host", {"link": link()}, {"link": alarm}, {"link": link("warn", HOT)}, {"link": link()})
        self.assertEqual(out[1], [("", "own", alarm["message"], True)])
        self.assertEqual(out[2], [])                                  # cooling down: no news
        self.assertEqual(out[3], [])

    def test_warn_to_alarm_notifies(self):
        w = lw.LinkWatcher()
        out = self.feed(w, "host", {"link": link("warn", HOT)}, {"link": link("alarm", "hot!")})
        self.assertEqual(out[0], [("", "own", HOT, False)])           # already hot at tray start: told once
        self.assertEqual(out[1], [("", "own", "hot!", True)])

    def test_back_up_but_still_hot(self):
        w = lw.LinkWatcher()
        d = link("down", DOWN, 1, LD)
        out = self.feed(w, "host", {"link": d}, {"link": link("warn", HOT, 1, LD)})
        self.assertEqual(out[0], [("", "own", DOWN, True)])
        self.assertEqual(out[1], [("", "own", "Link back after 1 min 8 s – the network module had overheated. " + HOT, False)])

    def test_outage_between_two_polls(self):
        w = lw.LinkWatcher()
        out = self.feed(w, "host", {"link": link()}, {"link": link("ok", "", 1, dict(LD, down_s=1))})
        self.assertEqual(out[1], [("", "own", "Link back after 1 s – the network module had overheated", False)])

    def test_daemon_silent_in_between_does_not_renotify(self):
        w = lw.LinkWatcher()
        warn = link("warn", HOT)
        out = self.feed(w, "host", {"link": link()}, {"link": warn}, None, {"link": warn}, {"link": link("unknown")}, {"link": warn})
        self.assertEqual([len(o) for o in out], [0, 1, 0, 0, 0, 0])

    def test_peer_only_for_client_roles(self):
        host_side = link("warn", HOT)
        st = {"link": link(), "peer_link": host_side, "link_warning": "x"}
        self.assertEqual(lw.LinkWatcher().poll("host", st), [])
        w = lw.LinkWatcher()
        self.assertEqual(w.poll("client", {"link": link(), "peer_link": link()}), [])
        self.assertEqual(w.poll("client", st), [(" - other computer", "peer", HOT, False)])
        self.assertEqual(w.poll("client-window", st), [])               # a second watcher would tell again, this one not

    def test_own_and_peer_independent(self):
        w = lw.LinkWatcher()
        w.poll("client", {"link": link(), "peer_link": link()})
        r = w.poll("client", {"link": link("down", DOWN, 1, LD), "peer_link": link("down", DOWN, 1, LD)})
        self.assertEqual([x[1] for x in r], ["own", "peer"])

    def test_fallback_texts_without_message(self):
        w = lw.LinkWatcher()
        r = w.poll("host", {"link": link("down", "", 1, LD)})
        self.assertEqual(r[0][2], "Link down: the network module overheated")
        r = lw.LinkWatcher().poll("host", {"link": link("down")})
        self.assertEqual(r[0][2], "Link down")

    def test_back_without_duration_or_reason(self):
        w = lw.LinkWatcher()
        w.poll("host", {"link": link("down", DOWN, 1, None)})
        r = w.poll("host", {"link": link("ok", "", 1, None)})
        self.assertEqual(r[0][2], "Link back")


class CardFailed(unittest.TestCase):
    LDF = {"at": 1761159476, "reason": "the network card overheated", "detail": "", "temp_c": None, "back_at": None, "down_s": None}

    def test_failed_notifies_urgent_once(self):
        w = lw.LinkWatcher()
        f = link("failed", FAILED, 1, self.LDF, up=False)
        out = self.feed(w, "host", {"link": link()}, {"link": link("alarm", "card too hot")}, {"link": f}, {"link": f})
        self.assertEqual(out[1], [("", "own", "card too hot", True)])
        self.assertEqual(out[2], [("", "own", FAILED, True)])        # alarm -> failed
        self.assertEqual(out[3], [])

    def test_down_to_failed_tells_the_card(self):
        w = lw.LinkWatcher()
        out = self.feed(w, "client", {"link": link("down", DOWN, 1, LD)},
                        {"link": link("failed", FAILED, 1, self.LDF)})
        self.assertEqual(out[1], [("", "own", FAILED, True)])

    def test_failed_at_start_and_back(self):
        w = lw.LinkWatcher()
        f = link("failed", FAILED, 1, self.LDF)
        back = link("ok", "", 1, dict(self.LDF, back_at=1761159976, down_s=500))
        out = self.feed(w, "host", {"link": f}, {"link": back})
        self.assertEqual(out[0], [("", "own", FAILED, True)])
        self.assertEqual(out[1], [("", "own", "Link back after 8 min 20 s – the network card had overheated", False)])

    def test_peer_failed_and_fallback_text(self):
        w = lw.LinkWatcher()
        r = w.poll("client", {"link": link(), "peer_link": link("failed", FAILED, 1, self.LDF)})
        self.assertEqual(r, [(" - other computer", "peer", FAILED, True)])
        self.assertEqual(lw.LinkWatcher().poll("host", {"link": link("failed")})[0][2], "Network card failed: power-cycle needed")

    def test_menu_lines(self):
        st = {"link": link("failed", FAILED), "peer_link": link("warn", HOT)}
        self.assertEqual(lw.menu_lines("client", st), [FAILED, "Other computer: " + HOT])
        self.assertEqual(lw.menu_lines("host", {"link": link("failed", FAILED)}), [FAILED])

    def feed(self, w, role, *states):
        return [w.poll(role, s) for s in states]


class Menu(unittest.TestCase):
    def test_ok_has_no_lines(self):
        self.assertEqual(lw.menu_lines("host", {"link": link()}), [])
        self.assertEqual(lw.menu_lines("client", {"link": link(), "peer_link": link(), "link_warning": ""}), [])
        self.assertEqual(lw.menu_lines("client", None), [])
        self.assertEqual(lw.menu_lines("client", {}), [])

    def test_host_own_only(self):
        st = {"link": link("warn", HOT), "peer_link": link("down", DOWN)}
        self.assertEqual(lw.menu_lines("host", st), [HOT])

    def test_client_own_and_peer(self):
        st = {"link": link("down", DOWN), "peer_link": link("warn", HOT)}
        self.assertEqual(lw.menu_lines("client", st), [DOWN, "Other computer: " + HOT])

    def test_client_link_warning_fallback(self):
        st = {"link": link(), "peer_link": None, "link_warning": "The other computer's network module is at 53.6 °C"}
        self.assertEqual(lw.menu_lines("client-window", st), [st["link_warning"]])

    def test_unknown_level_no_line(self):
        self.assertEqual(lw.menu_lines("host", {"link": link("unknown", "x")}), [])


class Fmt(unittest.TestCase):
    def test_fmt_secs(self):
        self.assertEqual([lw.fmt_secs(x) for x in (0, 45, 60, 68, 125, 3700)],
                         ["0 s", "45 s", "1 min", "1 min 8 s", "2 min 5 s", "1 h 1 min"])


class TrayFile(unittest.TestCase):
    def test_tray_parses_and_uses_module(self):
        with open(os.path.join(HERE, "..", "desktop", "rdx-tray.py")) as f:
            src = f.read()
        ast.parse(src)
        self.assertIn("rdx_link_watch.LinkWatcher()", src)
        self.assertIn("rdx_link_watch.menu_lines(", src)
        self.assertNotIn("ethtool", src)
        self.assertNotIn("/sys/", src)


if __name__ == "__main__":
    unittest.main(verbosity=1)
