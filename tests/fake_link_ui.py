#!/usr/bin/env python3
"""A stand-in daemon for looking at the link card / banner of the host and client web pages without
RDMA hardware: serves web/<role>/ and a fake /api/state in one of the link scenarios.

  tests/fake_link_ui.py host|client [port]        scenario: GET /__scenario?name=ok|warn|alarm|down|back|nosensor|nomodule|old|unknown|failed|chip|chipevent
  tests/fake_link_ui.py --dump host|client NAME   print the /api/state JSON of a scenario

Default ports 47590 (host) / 47591 (client), 127.0.0.1 only. Used by hand and with Playwright
(tests/README of the link UI patch); not part of `make test`.
"""
import copy
import json
import os
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")
NOW = int(time.time())

MODULE = {"label": "Module1", "temp_c": 49.1, "warn_c": 53.0, "alarm_c": 58.0, "highest_c": 58.0,
          "vendor": "INTEL CORP", "part": "SPTSBP2CLCKS"}
CHIP = {"label": "sensor0", "temp_c": 53.0, "limit_c": 105.0, "highest_c": 80.0, "event": False, "event_text": ""}
LD = {"at": NOW - 300, "reason": "the network module overheated",
      "detail": "Port module event[error]: module 1, Cable error, High Temperature",
      "temp_c": 53.6, "back_at": NOW - 232, "down_s": 68}


def link(level="ok", **kw):
    d = {"known": True, "link_ip": "10.0.0.2", "rdma_dev": "rocep1s0f1", "port": 1, "pci": "0000:01:00.1",
         "netdev": "enp1s0f1np1", "up": True, "state": "ACTIVE", "phys_state": "LinkUp", "rate": "100 Gb/sec (4X EDR)",
         "module": dict(MODULE), "chip": dict(CHIP), "nic": None, "hot": None, "level": level, "message": "", "downs": 0,
         "last_down": None, "since": NOW - 3600}
    d.update(kw)
    return d


SCEN = {
    "ok": (link(), None),
    "warn": (link("warn", module=dict(MODULE, temp_c=53.6),
                  message="Network module 53.6 °C – warning at 53 °C, the link goes down at 58 °C"),
             "Network module 53.6 °C – warning at 53 °C, the link goes down at 58 °C"),
    "alarm": (link("alarm", module=dict(MODULE, temp_c=58.4),
                   message="Network module 58.4 °C – the link goes down at 58 °C"), None),
    "down": (link("down", up=False, state="DOWN", phys_state="Polling", rate="", module=dict(MODULE, temp_c=54.0), downs=1,
                  last_down=dict(LD, back_at=None, down_s=None),
                  message="Link down: the network module overheated (54 °C, warning at 53 °C)"), None),
    "back": (link("ok", downs=2, last_down=LD), None),
    "nosensor": (link("ok", rate="40 Gb/sec (4X QDR)", module={"label": "", "temp_c": None, "warn_c": None, "alarm_c": None,
                                                                 "highest_c": None, "vendor": "FS", "part": "QSFP56-PC01"}), None),
    "nomodule": (link("ok", module=None), None),
    "failed": (link("failed", up=False, state="card failed", phys_state="", rate="", hot=None, downs=1,
                    nic={"failed": True, "likely_overheated": True, "power_cycle": True, "why": "PCI device not answering",
                         "detail": "PCI config space of 0000:01:00.1 reads all ones (\"PCI slot is unavailable\")"},
                    last_down=dict(LD, reason="the network card overheated", detail="", temp_c=None, back_at=None, down_s=None),
                    message="Network card failed (likely overheated): power-cycle needed"),
               "This computer's network card failed (likely overheated): power-cycle needed"),
    "chip": (link("warn", hot="chip", chip=dict(CHIP, temp_c=96.0),
                  message="Network card chip 96.0 °C – close to its limit of 105 °C"),
             "This computer's network card chip: 96.0 °C (limit 105 °C)"),
    "chipevent": (link("alarm", hot="chip", module={"label": "", "temp_c": None, "warn_c": None, "alarm_c": None, "highest_c": None,
                                                     "vendor": "", "part": ""},
                       chip={"label": "", "temp_c": None, "limit_c": None, "highest_c": None, "event": True,
                             "event_text": "the kernel log reports a high chip temperature"},
                       message="Network card too hot – the kernel log reports a high chip temperature, the card may fail: improve its cooling"),
                  "This computer's network card is too hot – it may fail: improve its cooling"),
    "old": (None, None),                      # a daemon without "link"
    "unknown": ({"known": False, "message": "No network port carries the link address 10.0.0.2"}, None),
}


def apps():
    return [{"id": "mirror:DP-1", "name": "Mirror DP-1", "kind": "mirror", "output": "DP-1"},
            {"id": "steam", "name": "Steam", "kind": "app"}]


def state(role, name):
    own, warn = SCEN[name]
    out_peer = {"known": True, "link_ip": "10.0.0.2", "level": "ok", "up": True, "state": "ACTIVE", "rate": "100 Gb/sec (4X EDR)",
                "module": dict(MODULE), "message": "", "downs": 0, "last_down": None, "rdma_dev": "rocep1s0f1", "port": 1}
    if role == "host":
        st = {"seat_name": "RDMA Display", "host_name": "desktop", "session": {"state": "idle"}, "apps": apps(),
              "outputs": [{"name": "DP-1", "w": 2560, "h": 1440, "hz": 144, "x": 0, "y": 0}], "defaults": {}, "config": {}}
        if own is not None:
            st["link"] = copy.deepcopy(own)
        return st
    info = {"host_name": "desktop", "apps": apps(), "outputs": [{"name": "DP-1", "w": 2560, "h": 1440, "hz": 144}]}
    st = {"host": {"connected": True, "address": "10.0.0.2", "info": info}, "hosts": [],
          "session": {"state": "idle", "procs": []},
          "config": {"mode": None, "pacing": "latency", "cursor": True, "tearing": False, "match_host": False, "audio": True,
                     "audible": True, "input": True, "mic": True, "mic_source": "", "input_devices": [], "usb": [],
                     "host": "10.0.0.2", "host_web_port": 47990, "fps": 0, "gpu_prio": "high", "rdma_dev": "rocep1s0f1",
                     "link_ip": "10.0.0.1", "mtu": 4096, "card": "card0", "sink_vt": 8, "desk_vt": 7},
          "lists": {"modes": {"modes": []}, "inputs": [], "usb": {"devices": []}, "mic_sources": []}}
    if own is not None:
        st["link"] = copy.deepcopy(own)
        peer = copy.deepcopy(own) if name in ("warn", "alarm", "down") else out_peer
        if own.get("known") is False:
            peer = None
        st["peer_link"] = peer
        st["link_warning"] = warn or {"down": own["message"], "alarm": "Network module 58.4 °C – the link goes down at 58 °C"}.get(name, "")
        if name == "warn":
            st["link"] = link()                     # only the other computer is hot
            st["peer_link"] = own
            st["link_warning"] = "The other computer's network module is at 53.6 °C (warning at 53 °C)"
    return st


class H(BaseHTTPRequestHandler):
    role = "host"
    scen = "ok"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        self._send(200, "{}", "application/json")

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/__scenario":
            n = urllib.parse.parse_qs(u.query).get("name", ["ok"])[0]
            if n not in SCEN:
                return self._send(400, "unknown scenario", "text/plain")
            H.scen = n
            return self._send(200, n, "text/plain")
        if u.path == "/api/state":
            return self._send(200, json.dumps(state(self.role, self.scen)), "application/json")
        if u.path.startswith("/api/"):
            return self._send(404, "{}", "application/json")
        rel = "index.html" if u.path == "/" else u.path.lstrip("/")
        f = os.path.normpath(os.path.join(ROOT, self.role, rel))
        if not f.startswith(os.path.normpath(os.path.join(ROOT, self.role))) or not os.path.isfile(f):
            return self._send(404, "not found", "text/plain")
        ctype = {".html": "text/html", ".js": "text/javascript", ".css": "text/css"}.get(os.path.splitext(f)[1], "application/octet-stream")
        with open(f, "rb") as fh:
            self._send(200, fh.read(), ctype + "; charset=utf-8")


def main(argv):
    if argv[:1] == ["--dump"]:
        print(json.dumps(state(argv[1], argv[2]), indent=1, ensure_ascii=False))
        return 0
    role = argv[0] if argv else "host"
    port = int(argv[1]) if len(argv) > 1 else (47590 if role == "host" else 47591)
    H.role = role
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
