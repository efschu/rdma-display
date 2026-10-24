#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""rdma-display tray app for the desktop session (host or client role).

Launching it from the app overview starts the root daemon (systemd unit rdx-hostd or
rdx-clientd; a polkit rule lets the desktop user start/stop exactly these units) and
shows a tray icon (StatusNotifierItem via AyatanaAppIndicator, shown by GNOME's
AppIndicator extension) while it runs. The menu shows the session state and opens the
daemon's web UI; "Quit" stops the daemon. A second launch only opens the web UI.

    rdx-tray.py --role host|client|client-window [--seat N]

client-window: the second client instance whose stream shows in a window of this desktop
(rdx-clientd-window.service, web UI on 47992, tools/rdx-clientd-window.service).

--seat N (host role, N > 0): the seat's daemon rdx-hostd@N.service, web UI on 47990+100*N.
"""
import argparse
import json
import subprocess
import sys
import urllib.request

import rdx_link_watch                  # link warnings: pure logic, same directory (tests/test_tray_link.py)

import gi
gi.require_version("Gtk", "3.0")
gi.require_version("AyatanaAppIndicator3", "0.1")
from gi.repository import AyatanaAppIndicator3 as AppIndicator  # noqa: E402
from gi.repository import Gio, GLib, Gtk  # noqa: E402

ROLES = {
    "host": dict(unit="rdx-hostd.service", url="http://127.0.0.1:47990/", title="RDMA Display Host",
                 app_id="io.github.efschu.RdmaDisplayHost"),
    "client": dict(unit="rdx-clientd.service", url="http://127.0.0.1:47991/", title="RDMA Display Client",
                   app_id="io.github.efschu.RdmaDisplayClient"),
    "client-window": dict(unit="rdx-clientd-window.service", url="http://127.0.0.1:47992/",
                          title="RDMA Display Client (window)", app_id="io.github.efschu.RdmaDisplayClientWindow"),
}
POLL_S = 2


def systemctl(verb, unit):
    """start/stop the daemon; polkit allows it for the desktop user without a password"""
    r = subprocess.run(["systemctl", "--no-ask-password", verb, unit], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or r.stdout).strip()


def http_json(url, body=None, timeout=1.5):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST" if body is not None else "GET",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode() or "null")


def describe(role, st):
    """one status line and whether a session runs, from the daemon's /api/state"""
    sess = st.get("session") or {}
    state = sess.get("state", "idle") if isinstance(sess, dict) else "idle"
    app = sess.get("app") if isinstance(sess, dict) else None
    if role.startswith("client"):
        host = st.get("host") or {}
        if not host.get("connected"):
            return "Host %s not reachable" % host.get("address", "?"), False
    if state in ("running", "starting", "request", "listen", "vt"):
        return "Session %s%s" % (state, (": " + app) if app else ""), state == "running"
    return "Ready, no session", False


class Tray(Gtk.Application):
    def __init__(self, role, seat=0):
        self.role, self.cfg = role, dict(ROLES[role])
        if role == "host" and seat > 0:
            self.cfg.update(unit="rdx-hostd@%d.service" % seat, url="http://127.0.0.1:%d/" % (47990 + 100 * seat),
                            title="RDMA Display Host (seat %d)" % seat, app_id="io.github.efschu.RdmaDisplayHost.seat%d" % seat)
        super().__init__(application_id=self.cfg["app_id"], flags=Gio.ApplicationFlags.FLAGS_NONE)
        self.ind = None
        self.running = False
        self.link_watch = rdx_link_watch.LinkWatcher()

    def do_activate(self):
        if self.ind:                       # second launch from the app overview
            self.open_settings()
            return
        self.hold()                        # no window: keep the app alive for the tray icon
        ok, msg = systemctl("start", self.cfg["unit"])
        if not ok:
            self.notify_error("Could not start %s: %s" % (self.cfg["unit"], msg))
        self.ind = AppIndicator.Indicator.new(self.cfg["app_id"], "rdx-display",
                                              AppIndicator.IndicatorCategory.APPLICATION_STATUS)
        self.ind.set_title(self.cfg["title"])
        self.ind.set_status(AppIndicator.IndicatorStatus.ACTIVE)
        self.build_menu()
        self.refresh()
        GLib.timeout_add_seconds(POLL_S, self.refresh)

    def build_menu(self):
        m = Gtk.Menu()
        self.status_item = Gtk.MenuItem(label="Starting ...")
        self.status_item.set_sensitive(False)
        m.append(self.status_item)
        # network link warnings (module too hot, link down): up to two disabled lines, hidden while ok
        self.link_items = []
        for _ in range(2):
            li = Gtk.MenuItem(label="")
            li.set_sensitive(False)
            li.set_no_show_all(True)
            m.append(li)
            self.link_items.append(li)
        m.append(Gtk.SeparatorMenuItem())
        it = Gtk.MenuItem(label="Open settings")
        it.connect("activate", lambda *_: self.open_settings())
        m.append(it)
        if self.role.startswith("client"):
            self.start_item = Gtk.MenuItem(label="Start session")
            self.start_item.connect("activate", lambda *_: self.session("start"))
            m.append(self.start_item)
        if self.role == "host":
            # an app on this computer's desktop window <-> the client, it keeps running
            self.here_item = Gtk.MenuItem(label="Back to this PC")
            self.here_item.connect("activate", lambda *_: self.local("here"))
            m.append(self.here_item)
            self.toclient_item = Gtk.MenuItem(label="Send to the client")
            self.toclient_item.connect("activate", lambda *_: self.local("to_client"))
            m.append(self.toclient_item)
        self.quit_app_item = Gtk.MenuItem(label="Quit paused app")
        self.quit_app_item.connect("activate", lambda *_: self.quit_paused())
        m.append(self.quit_app_item)
        self.paused_app = None
        if self.role.startswith("client"):
            it = Gtk.MenuItem(label="Restart the host program")
            it.connect("activate", lambda *_: self.restart_host())
            m.append(it)
        self.stop_item = Gtk.MenuItem(label="Stop session")
        self.stop_item.connect("activate", lambda *_: self.session("stop"))
        m.append(self.stop_item)
        m.append(Gtk.SeparatorMenuItem())
        it = Gtk.MenuItem(label="Quit %s" % self.cfg["title"])
        it.connect("activate", lambda *_: self.quit_all())
        m.append(it)
        m.show_all()
        self.ind.set_menu(m)
        self.ind.set_secondary_activate_target(self.status_item)

    def refresh(self):
        st = None
        try:
            st = http_json(self.cfg["url"] + "api/state")
            text, running = describe(self.role, st)
        except Exception:
            text, running = "Daemon not answering", False
        # an app left running on the host after a disconnect: offer to quit it
        sus = (st or {}).get("host", {}).get("suspended") if self.role.startswith("client") else (st or {}).get("suspended")
        self.paused_app = sus.get("app") if isinstance(sus, dict) else None
        self.quit_app_item.set_label("Quit paused app: %s" % self.paused_app if self.paused_app else "Quit paused app")
        self.quit_app_item.set_visible(bool(self.paused_app))
        self.status_item.set_label(text)
        self.show_link(st)
        self.stop_item.set_sensitive(running)
        if self.role == "host":
            sess = (st or {}).get("session") or {}
            app = sess.get("app") if sess.get("state", "idle") != "idle" else self.paused_app
            game = bool(app) and not str(app).startswith("mirror:")
            local_win = bool(sess.get("local_window")) and sess.get("state", "idle") != "idle"
            self.toclient_item.set_visible(local_win)
            self.here_item.set_visible(game and not local_win)
            self.here_item.set_label("Back to this PC: %s" % app if game else "Back to this PC")
        if self.role.startswith("client"):
            self.start_item.set_sensitive(not running and "not" not in text)
        if running != self.running:
            self.running = running
            self.ind.set_icon_full("rdx-display-active" if running else "rdx-display",
                                   "session running" if running else "ready")
        return True

    def show_link(self, st):
        """warning lines in the menu and one notification per level change (only /api/state is read)"""
        lines = rdx_link_watch.menu_lines(self.role, st)
        for i, li in enumerate(self.link_items):
            li.set_label(lines[i] if i < len(lines) else "")
            li.set_visible(i < len(lines))
        for suffix, source, body, urgent in self.link_watch.poll(self.role, st):
            self.notify("Network link" + suffix, body, "rdx-link-" + source, urgent)

    def restart_host(self):
        try:
            http_json(self.cfg["url"] + "api/host/restart", body={}, timeout=10)
        except Exception as e:
            self.notify_error("Restart of the host program failed: %s" % e)
        GLib.timeout_add(3000, lambda: self.refresh() and False)

    def local(self, action):
        try:
            http_json(self.cfg["url"] + "api/local", body={"action": action}, timeout=10)
        except Exception as e:
            self.notify_error(("Back to this PC" if action == "here" else "Send to the client") + " failed: %s" % e)
        GLib.timeout_add(800, lambda: self.refresh() and False)

    def quit_paused(self):
        if not self.paused_app:
            return
        try:
            http_json(self.cfg["url"] + "api/apps/quit", body={"app": self.paused_app}, timeout=10)
        except Exception as e:
            self.notify_error("Quit %s failed: %s" % (self.paused_app, e))
        GLib.timeout_add(500, lambda: self.refresh() and False)

    def open_settings(self):
        Gio.AppInfo.launch_default_for_uri(self.cfg["url"], None)

    def session(self, verb):
        try:
            http_json(self.cfg["url"] + "api/session/" + verb, body={}, timeout=5)
        except Exception as e:
            self.notify_error("Session %s failed: %s" % (verb, e))
        GLib.timeout_add(500, lambda: self.refresh() and False)

    def quit_all(self):
        systemctl("stop", self.cfg["unit"])
        self.release()
        self.quit()

    def notify_error(self, text):
        print("[rdx-tray] " + text, file=sys.stderr)
        n = Gio.Notification.new(self.cfg["title"])
        n.set_body(text)
        self.send_notification(None, n)

    def notify(self, title, text, nid, urgent=False):
        """desktop notification; the same id replaces the previous one of that source"""
        print("[rdx-tray] %s: %s" % (title, text), file=sys.stderr)
        n = Gio.Notification.new("%s - %s" % (self.cfg["title"], title))
        n.set_body(text)
        n.set_priority(Gio.NotificationPriority.URGENT if urgent else Gio.NotificationPriority.HIGH)
        self.send_notification(nid, n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", choices=sorted(ROLES), required=True)
    ap.add_argument("--seat", type=int, default=0)
    a = ap.parse_args()
    return Tray(a.role, a.seat).run([sys.argv[0]])


if __name__ == "__main__":
    sys.exit(main())
