# SPDX-License-Identifier: MIT
"""Link-state logic of the tray app, free of any desktop toolkit so tests/test_tray_link.py can run
it with fake /api/state dicts.

The daemons (rdx-hostd, rdx-clientd) put an object "link" into /api/state (clientd also "peer_link":
the same object as the host reported it, and "link_warning"): RDMA port state, network module
temperature, level ok|warn|alarm|down|unknown, one plain-English "message", the count of link downs
("downs") and the last one ("last_down": reason, down_s ...). The tray only reads that.

  menu_lines(role, st)   -> the disabled menu lines for a state that is not ok. level "failed" is the
                            network card itself gone (message: "Network card failed (likely overheated):
                            power-cycle needed"), the worst case: it notifies as urgent like a link down
  LinkWatcher.poll(role, st) -> notifications for the level transitions, each exactly once
"""
import re

BAD = ("warn", "alarm", "down", "failed")
LEVELS = ("ok", "warn", "alarm", "down", "failed")      # "unknown" and anything else never counts as a transition
OUTAGE = ("down", "failed")                   # no link: the port is down, or the card itself is gone (power-cycle)


def fmt_secs(s):
    """68 -> '1 min 8 s', 45 -> '45 s', 3700 -> '1 h 1 min'"""
    s = int(round(s))
    if s < 60:
        return "%d s" % s
    if s < 3600:
        return "%d min %d s" % (s // 60, s % 60) if s % 60 else "%d min" % (s // 60)
    return "%d h %d min" % (s // 3600, (s % 3600) // 60)


def _past(reason):
    """'the network module overheated' -> 'the network module had overheated'"""
    return re.sub(r"\boverheated$", "had overheated", reason.strip())


def _bad_text(link):
    level = link.get("level")
    msg = (link.get("message") or "").strip()
    if msg:
        return msg
    ld = link.get("last_down") or {}
    if level == "down":
        return ("Link down: " + ld["reason"]) if ld.get("reason") else "Link down"
    if level == "failed":
        return "Network card failed: power-cycle needed"
    if level == "alarm":
        return "The network module is overheating"
    return "The network module is getting hot"


def _back_text(link):
    ld = link.get("last_down") or {}
    secs, reason = ld.get("down_s"), (ld.get("reason") or "").strip()
    text = "Link back" + (" after " + fmt_secs(secs) if isinstance(secs, (int, float)) else "")
    if reason:
        text += " – " + _past(reason)
    return text


def menu_lines(role, st):
    """texts for the disabled warning lines of the tray menu (empty while everything is ok)"""
    if not isinstance(st, dict):
        return []
    out = []
    own = st.get("link")
    if isinstance(own, dict) and own.get("level") in BAD:
        out.append(_bad_text(own))
    if role.startswith("client"):
        peer = st.get("peer_link")
        if isinstance(peer, dict) and peer.get("level") in BAD:
            out.append("Other computer: " + _bad_text(peer))
        if not out and (st.get("link_warning") or "").strip():
            out.append(st["link_warning"].strip())
    return out


class LinkWatcher:
    """Remembers the last level per source ("own", "peer") and returns the notifications a new
    /api/state sample calls for. A source seen for the first time notifies only when it is already
    not ok (a freshly started tray on a hot link); an ok link at start is silent."""

    def __init__(self):
        self.seen = {}

    def update(self, source, link):
        """-> list of (body, urgent). Unknown/absent data changes nothing."""
        if not isinstance(link, dict) or link.get("known") is False:
            return []
        level = link.get("level")
        if level not in LEVELS:
            return []
        downs = link.get("downs")
        downs = downs if isinstance(downs, int) else 0
        prev = self.seen.get(source)
        self.seen[source] = (level, downs)
        out = []
        if prev is None:
            if level in BAD:
                out.append((_bad_text(link), level != "warn"))
            return out
        plevel, pdowns = prev
        if level == plevel:
            if level not in OUTAGE and downs > pdowns:   # down and up again between two polls
                out.append((_back_text(link), False))
            return out
        if level in OUTAGE:                              # also down -> failed: the card is the news now
            out.append((_bad_text(link), True))
        elif plevel in OUTAGE:                           # back up (maybe still hot)
            text = _back_text(link)
            if level in BAD:
                text += ". " + _bad_text(link)
            out.append((text, level == "alarm"))
        elif level in BAD and LEVELS.index(level) > LEVELS.index(plevel):    # ok->warn, ok/warn->alarm
            out.append((_bad_text(link), level == "alarm"))
        # cooling down (alarm->warn, ->ok) is not announced
        return out

    def poll(self, role, st):
        """-> [(title_suffix, source, body, urgent)] for one /api/state sample (None: daemon silent)"""
        if not isinstance(st, dict):
            return []
        res = []
        for source, key, suffix in (("own", "link", ""), ("peer", "peer_link", " - other computer")):
            if source == "peer" and not role.startswith("client"):
                continue
            for body, urgent in self.update(source, st.get(key)):
                res.append((suffix, source, body, urgent))
        return res
