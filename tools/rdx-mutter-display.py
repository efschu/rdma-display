#!/usr/bin/env python3
"""The monitors as GNOME's compositor (mutter) runs them, for p0b/rdx_sink_win: whether a monitor's
current mode has a variable refresh rate (VRR / FreeSync), its range and where it sits.

Reads org.gnome.Mutter.DisplayConfig.GetCurrentState on the desktop user's session bus (mode
property "refresh-rate-mode": "variable" | "fixed", monitor property "min-refresh-rate") and prints
one JSON object:
  {"monitors": [{"connector": "DP-1", "x": 2560, "y": 0, "scale": 1.0, "primary": true,
                 "w": 2560, "h": 1440, "hz": 143.999, "vrr": true, "min_hz": 48}], "error": ""}
Run it as the desktop user (rdx_sink_win does that with runuser when it runs as root).
    rdx-mutter-display.py [--test]
"""
import json
import sys


def parse_state(state):
    """GetCurrentState's (serial, monitors, logical_monitors, properties) -> the list above"""
    _serial, monitors, logical, _props = state
    where = {}
    for lm in logical:
        x, y, scale, _transform, primary, mons, _p = lm
        for m in mons:
            where[m[0]] = (x, y, scale, primary)
    out = []
    for mon in monitors:
        spec, modes, props = mon
        conn = spec[0]
        cur = None
        for mode in modes:
            mid, w, h, hz, _pscale, _scales, mprops = mode
            if mprops.get('is-current'):
                cur = (w, h, hz, mprops.get('refresh-rate-mode', 'fixed'))
        if conn not in where or cur is None:
            continue                                   # not part of the desktop (disabled)
        x, y, scale, primary = where[conn]
        mn = props.get('min-refresh-rate')
        out.append({'connector': conn, 'x': x, 'y': y, 'scale': scale, 'primary': bool(primary),
                    'w': cur[0], 'h': cur[1], 'hz': round(cur[2], 3), 'vrr': cur[3] == 'variable',
                    'min_hz': mn if isinstance(mn, (int, float)) and mn > 0 else 0})
    return out


def query():
    from gi.repository import Gio, GLib
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    r = bus.call_sync('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
                      'org.gnome.Mutter.DisplayConfig', 'GetCurrentState', None, None,
                      Gio.DBusCallFlags.NONE, 3000, None)
    return parse_state(r.unpack())


def selftest():
    """parse_state on a recorded-shape state: one VRR monitor, one fixed, one disabled"""
    st = (1,
          [(('DP-1', 'AOC', 'Q27G2', 'S1'),
            [('2560x1440@143.999', 2560, 1440, 143.999, 1.0, [1.0], {'is-current': True, 'refresh-rate-mode': 'variable'}),
             ('2560x1440@59.951', 2560, 1440, 59.951, 1.0, [1.0], {})],
            {'min-refresh-rate': 48, 'display-name': 'AOC 27"'}),
           (('DP-2', 'AOC', 'Q27G2', 'S2'),
            [('2560x1440@59.951', 2560, 1440, 59.951, 1.0, [1.0], {'is-current': True})], {}),
           (('HDMI-1', 'X', 'Y', 'Z'), [('1920x1080@60', 1920, 1080, 60.0, 1.0, [1.0], {})], {})],
          [(2560, 0, 1.0, 0, True, [('DP-1', 'AOC', 'Q27G2', 'S1')], {}),
           (0, 0, 1.0, 0, False, [('DP-2', 'AOC', 'Q27G2', 'S2')], {})],
          {})
    got = parse_state(st)
    ok = (len(got) == 2 and got[0] == {'connector': 'DP-1', 'x': 2560, 'y': 0, 'scale': 1.0, 'primary': True, 'w': 2560,
                                       'h': 1440, 'hz': 143.999, 'vrr': True, 'min_hz': 48}
          and got[1]['vrr'] is False and got[1]['min_hz'] == 0 and got[1]['x'] == 0)
    print('rdx-mutter-display selftest:', 'PASS' if ok else 'FAIL %r' % got)
    return 0 if ok else 1


def main():
    if '--test' in sys.argv:
        return selftest()
    try:
        print(json.dumps({'monitors': query(), 'error': ''}))
    except Exception as e:                       # no mutter (another desktop, X11 without it): no VRR info
        print(json.dumps({'monitors': [], 'error': str(e)[:200]}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
