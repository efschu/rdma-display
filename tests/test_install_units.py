#!/usr/bin/env python3
"""desktop/install.sh must leave the daemon enabled, and every unit it installs must be enable-able.

Observed: after a reboot rdx-hostd stayed down: install.sh put the unit in place but never enabled it, and
the rollout scripts only restart units. This checks the script text and the unit files (no root, no systemd).

  tests/test_install_units.py        exit 0 = PASS
"""
import os
import re
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def read(rel):
    with open(os.path.join(ROOT, rel)) as f:
        return f.read()


class Install(unittest.TestCase):
    def test_script_enables_the_unit_after_daemon_reload(self):
        s = read("desktop/install.sh")
        a = s.index("systemctl daemon-reload")
        self.assertRegex(s[a:], r"systemctl enable \$UNIT\.service")
        self.assertRegex(s[a:], r"is-enabled \$UNIT\.service")

    def test_script_never_starts_or_restarts(self):
        # a stream in progress must not be cut by an install; the rollouts restart explicitly
        code = "\n".join(l for l in read("desktop/install.sh").splitlines() if not l.lstrip().startswith("#"))
        self.assertNotRegex(code, r"systemctl (re)?start")

    def test_every_unit_has_an_install_section(self):
        for unit in ("rdx-hostd.service", "rdx-hostd@.service", "rdx-clientd.service", "rdx-clientd-window.service"):
            s = read("tools/" + unit)
            self.assertRegex(s, r"(?m)^\[Install\]\s*\nWantedBy=multi-user\.target", unit)
            self.assertNotIn("not enabled at boot", s, unit)

    def test_script_installs_those_units(self):
        s = read("desktop/install.sh")
        for unit in ("rdx-hostd@.service", "$UNIT.service"):
            self.assertIn(unit, s)
        self.assertTrue(re.search(r"rdx-clientd-window", s))


if __name__ == "__main__":
    unittest.main(verbosity=1)
