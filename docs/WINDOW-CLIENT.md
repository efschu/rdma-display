# Window client: a seat in a window, its menu, its mouse; link health

A desktop PC can show a seat of a multiseat host in a window of its own desktop:
`rdx-clientd-window` (`tools/rdx-clientd-window.service`, config `/etc/rdx/client-window.json`,
web UI `http://127.0.0.1:47992`) with `"sink": "window"`: `p0b/rdx_sink_win` draws the stream into
an X11 window, `p4/rdx_input_win` sends that window's keyboard and mouse to the seat.

## In-stream menu and start placeholder in the window

The same menu as on the KMS client (`client/rdx_menu.c`, hotkey **Ctrl+Alt+Shift+M**): statistics,
cursor, output, the seat's own monitor on/off, microphone mute, **Mouse** (window only), back,
disconnect (the app keeps running), quit the app on the other computer, restart the host program
(while the placeholder waits long). Also the "*App* is starting…" placeholder and the link card.

```
 rdx-clientd (rdx_menu.c) --memfd x2, SHOW/HIDE--> rdx_sink_win (p0b/rdx_winmenu.h)
        ^                                            memfd -> udmabuf -> EGLImage (no copy)
        |  TOGGLE / KEY / MICMUTE                    blended (premultiplied) over the letterboxed frame
 rdx_input_win (Ctrl+Alt+Shift+M, keys while open)   at the next frame, or at once when none flow
```

- One socket per clientd instance: the window service uses `--menu-sock /run/rdx/menu-window.sock`
  (it used to run with `--no-menu`), so it never meets a KMS clientd's `/run/rdx/menu.sock`.
- The sink answers like the KMS overlay: SHOW b = 1 (start placeholder) until the first frame,
  b = 2 (the host's stall / notice) over the last frame after 500 ms, b = 0 (menu, link card)
  until HIDE. Before the first frame the sink serves the menu socket while it waits for the
  source (`accept_source_win`), so the placeholder shows while the seat still starts the app.
- Frames are now letterboxed into the window (aspect kept) instead of stretched; the absolute
  pointer uses the same rectangle (`rdx_letterbox`, `p0b/rdx_xwin.h`).
- The test pattern's GPU probes (a `glReadPixels` per frame) run only for the pattern, not for live
  frames.

## Native resolution and full screen

- The window client's mode list starts with every monitor's own mode ("match monitor
  DisplayPort-0", size and refresh of its current XRandR mode; `p0b/rdx_xmon.h`), then the usual
  window sizes. "Automatic" (`"mode": null`) is the native mode of the monitor the window opens on.
- The window opens on the monitor it was on last time (`"window_monitor"`, remembered from the
  sink's `[sink] on monitor NAME` line when a session ends), else on the primary monitor, else the
  one under the pointer. When the frame has exactly that monitor's size it opens full screen
  there from the first map (`_NET_WM_STATE_FULLSCREEN` before `XMapWindow`, position as
  `USPosition`): pixel for pixel, nothing scaled, no title bar eating lines
  (`"window_fullscreen"`, default on, a switch on the client page). F11 toggles.
- Measured on the test host's desktop (read-only): DisplayPort-0 2560x1440 @ 143.999 Hz (primary),
  DisplayPort-1 2560x1440 @ 59.951 Hz, GNOME on Xorg, scale 1, 96 dpi.

## FreeSync / variable refresh (client side)

Config `"vrr"` (default on, switch on the client page; off with tearing). The sinks get `--vrr`:

- **KMS sink** (a dedicated client, a seat's monitor): when the connector says `vrr_capable`, the CRTC's
  `VRR_ENABLED` is set after the mode (`rdx_kms_vrr_enable`, a plain property set: no atomic-only
  property), the late flip is off (each frame flips the moment it is ready, the refresh follows
  it), and below the monitor's minimum (EDID range limits, `rdx_edid_range_bytes`) the frame on
  screen is flipped again at 90 % of the longest refresh it allows (low framerate compensation).
  At the end VRR is switched off again for the desktop.
- **Window sink**: it shows each frame on arrival anyway (swap interval 0). VRR counts when the
  window is fullscreen on a monitor the compositor runs with a variable refresh: GNOME/mutter's
  `org.gnome.Mutter.DisplayConfig.GetCurrentState`, the current mode's `"refresh-rate-mode":
  "variable"` and the monitor's `"min-refresh-rate"` (`tools/rdx-mutter-display.py`, run as the
  desktop user; matched to the X monitor by position), the minimum else from the monitor's EDID in
  sysfs. Below the minimum the last frame is drawn again.
- Both print `[sink] display vrr=1 min=48.000 max=144.000 hz=143.999` (before they listen and on
  every change); rdx-clientd sends it as `session.start` `"display": {"vrr", "min_hz", "max_hz",
  "hz"}` and a later change (fullscreen off, another monitor) as `session.update {"display"}`. With
  `vrr` the host sends at the app's pace within the range instead of at the client's vblank. The menu's statistics say "FreeSync on (48–144 Hz)", the client page
  shows it with the running stream, `/api/state` has `"display"`.

## Game monitor by lease (X11, two monitors or more)

The client drives only the game monitor itself, the desktop stays on the other one. Config `"game_monitor"`: `"auto"` (default), `"lease"`, `"window"`; client page and
`/api/state "game_monitor"` (`lease`, `probe`).

```
 rdx-clientd --> p0b/rdx_lease --output DP-1 --state /run/rdx/lease-<port>.state -- rdx_sink_kms --drm-fd @LEASEFD@ ...
                 1. RandR: DP-1 off (the desktop moves its windows to the other monitors), its CRTC config saved
                 2. RRCreateLease(CRTC, output) -> DRM fd, master of just those two (+ the CRTC's primary plane)
                 3. rdx_sink_kms on that fd: page flips, VRR_ENABLED + LFC, the menu blended into the frames
                 4. sink ends / SIGTERM: RRFreeLease, DP-1 back as saved (screen size, position, primary)
 keyboard + mouse: p4/rdx_input_tx (evdev, grabbed); "desktop_hotkey" (Ctrl+Alt+H) <-> the desktop
```

- auto = lease when `rdx_lease --probe` reports RandR 1.6 on a real X server (not Xwayland: GNOME 46
  offers no wp_drm_lease_device_v1) and the window sink's mode list has two monitors or more; the
  game monitor is the one the window would open on (remembered, else the fastest). A refused lease
  (exit 3: an old driver, Xvfb) falls back to the full-screen window for that session. `"lease"`
  forces the attempt, also under Xwayland: a compositor that leases monitors (GNOME 48+ with
  `for-lease`) answers Xwayland's RRCreateLease through wp_drm_lease, the same path.
- The amdgpu DDX (23.0 on the test host) and Xorg 21.1.12 implement leases (`drmmode_create_lease`:
  CRTC + connector to `drmModeCreateLease`). The kernel gives the lessee the CRTC's primary plane
  only, so `rdx_sink_kms` finds no overlay plane and blends the menu, the placeholder and the cards
  into each frame by CPU (`soft_blend`, ~1-2 ms per frame while one shows; over a still picture into
  a copy in the spare start buffer).
- amdgpu takes no framebuffer from an imported dma-buf (`amdgpu_display_user_framebuffer_create`:
  "Cannot create framebuffer from imported dma_buf", since Linux 5.12), so ADDFB2 of the udmabuf
  slots fails with EINVAL. `rdx_sink_kms` then runs in copy mode: one dumb buffer of the driver per
  slot (on the test host's Cezanne APU in GTT), the frame copied into it before its flip. Measured
  on the test host (2560x1440, ~139 fps, 48853 frames): copy mean 1.08 ms, max 3.45 ms; the sink
  at 14.9 % of one core. Zero copy would need the scanout buffers allocated by amdgpu, exported as
  dma-bufs and registered at the NIC with `ibv_reg_dmabuf_mr` (open).
- VRR on the lease: `VRR_ENABLED` set by the lessee works (amdgpu_dm_dtn_log: OTG vmin 1489 /
  vmax 5363 = 144 / 40 Hz; drm.debug 0x20: `vupdate-vrr:1`, refresh intervals 6.95-7.32 ms at
  frames every 7.17 ms). The vblank timestamps in amdgpu's flip events are quantized to the nominal
  grid under VRR (all on 6.944 ms) and are no instrument for the rate there; LFC therefore times
  from the sink's own clock (`rdx_lfc_due_ns`: when the last flip event was read, the last arrival).
  Timed from the event timestamp it fired 15.8 repeats/s between frames that were flowing.
- Crash safety: the sink dies with rdx_lease (PR_SET_PDEATHSIG); a dead sink closes the lease fd and
  the kernel ends the lease (the DDX notices: `drmmode_validate_leases`). If rdx_lease itself is
  killed, its state file stays and rdx-clientd puts the monitor back (`rdx_lease --restore`) once
  idle (it looks every 10 s; the restore retries for 3 s while the X server still counts the CRTC as
  leased). Tested on the test host with `kill -9`: of rdx_sink_kms, DisplayPort-0 back in 0.4 s by
  rdx_lease; of rdx_lease, back by rdx-clientd at its next look (before the retry its first attempt
  failed and the monitor came 10 s later); `xrandr` before and after identical (mode, position, primary).
- Keyboard and mouse: grabbed from evdev during the session (`EVIOCGRAB`), everything into the
  stream. `"desktop_hotkey"` (default `ctrl+alt+h`, exact modifiers, Ctrl+Alt+Shift+Z as an alias;
  `common/rdx_hotkey.h`) gives them to the desktop: everything held is let go on the host first, the
  grab goes, the devices are still read passively (the combination is seen even while a desktop
  game holds the keyboard; X11 and Wayland alike, no XGrabKey, no portal); the lease monitor keeps
  showing the stream with a short "Desktop - Ctrl+Alt+H: back to the stream" card. Back: the grab
  comes only once every key is up (`EVIOCGKEY`), so the desktop has seen the releases too; nothing
  stays pressed on either side. In the window mode the same hotkey lets the captured mouse go.
- Tested on the test host (GNOME 46 on Xorg 21.1.12, amdgpu DDX 23.0, DISPLAY :1 found through
  gnome-shell's environment, XAUTHORITY /run/user/1000/gdm/Xauthority): `rdx_lease --probe`
  RandR 1.6, lease true, xwayland false; DisplayPort-0 (the primary, 144 Hz FreeSync) leased, the
  stream from a seat on it at 2560x1440 with VRR; at the session's end DisplayPort-0 back with the
  same position and primary (`xrandr` before and after identical), mutter re-applied nothing of its
  own. A chosen mode without a monitor (the client page saves `{w, h, hz}`) uses the monitor the
  window would open on. Also tested: `rdx_lease` against Xvfb (RandR 1.6, the lease refused: output
  off, back on, exit 3), `tests/test_client_lease.py`, `tests/test_hotkey`, `tests/test_vrr`.

## The mouse in the window

Two modes, chosen by clientd (`"pointer"`: `"auto"` default, `"absolute"`, `"relative"`; the menu's
**Mouse** entry changes it for the session; the client web page offers it for window clients):

| mode | what the window does | for |
|---|---|---|
| absolute | a third device "window pointer" with `ABS_X`/`ABS_Y` over the frame (0..w-1, 0..h-1): the app's pointer is where the local one is. The local cursor is hidden over the window while the stream shows the host's cursor (menu "Cursor"), so there is one pointer; a gamescope app sends its cursor next to the picture instead (docs/GAMESCOPE.md): the window's own cursor then is the host's picture, a motion needs no frame | desktop apps, launchers, Steam's UI |
| relative | the relative mouse; a click into the window captures the pointer (X pointer grab confined to the window, invisible cursor, warped back to the centre after each motion: unbounded motion for mouse look). **Ctrl+Alt+Shift+Z**, focus loss and the menu let it go | games with mouse look |

**auto** = relative when the app hides its cursor and locks the pointer, absolute otherwise. That
is gamescope's own rule for its nested backends (SDL / Wayland): `steamcompmgr.cpp` computes
`cursor image empty && pointer constrained && not Steam` and calls
`INestedHints::SetRelativeMouseMode`. The rdx backend now has such a hints object: gamescope sends
`RDX_GS_HINT` to `rdx_gs_src`, which puts it into every frame header (`RDX_HDR_F_HINTS`,
`RDX_HDR_F_REL_MOUSE`, `common/rdx_common.h`); the window sink reports changes to clientd
(`RDX_MENU_POINTER`), which tells the window's input. A seat without the new gamescope sends no
hint: auto is absolute there. A mirrored desktop is relative under auto (an absolute device on a
host desktop with several monitors maps onto all of them, not the mirrored one).

Why not just one mode: absolute alone breaks mouse look (the pointer stops at the window's edge,
the game sees no motion once its locked pointer sits at the border); relative alone makes every
desktop app need a click-to-capture and loses the 1:1 pointer. Parsec and Moonlight make the same
split (Moonlight: "optimize mouse for remote desktop", capture toggle Ctrl+Alt+Shift+Z).

gamescope's own libinput handler fed absolute motion in millimetres straight into
`wlserver_mousewarp`; the patch maps it like touch input (normalised over the output, focused
window offset and scale), so the pointer lands right also when the app renders at a lower size.

## Link health

`common/rdx_link.{c,h}`, a thread in rdx-hostd and rdx-clientd, every 2 s:

- the RDMA port carrying `link_ip` (sysfs `/sys/class/infiniband/<dev>/ports/<p>/gids`, `state`,
  `phys_state`, `rate`; works in a seat container, RDMA devices are not namespaced),
- the module temperature from the NIC's hwmon (mlx5: `tempN_label` "Module<n>", `tempN_input`,
  `tempN_crit` = the module's high warning threshold, `tempN_highest`), no root needed,
- the module's own alarm threshold, vendor and part from its EEPROM by `SIOCETHTOOL`
  (`ETHTOOL_GMODULEINFO` / `GMODULEEEPROM`, what `ethtool -m` reads; SFF-8636 page 03h, SFF-8472
  A2h), when the netdev is in the daemon's namespace,
- the card's own chip temperature from the same hwmon (`sensor0`: `tempN_input`, `tempN_crit` = the limit
  the firmware warns at, `tempN_highest`), where the driver has one, plus the kernel log's heat reports
  (`temp_warn: High temperature on sensors with bit set <hi> <lo>`, bit 0 of `<lo>` is the chip;
  `Health issue observed, High temperature`),
- whether the card is still there (below),
- why the link went down from the kernel log (`/dev/kmsg`, mlx5's "Port module event[error]:
  module 1, Cable error, High Temperature"), else the temperature just before, else "no signal
  from the other side".

Levels: ok, warn (≥ the module's warning threshold, 1 °C hysteresis), alarm (≥ its alarm
threshold: the kernel takes the link down there), down (with the reason), failed (the card itself is
gone), unknown (no port carries `link_ip`). The chip counts too: warn 10 °C below its limit
(`RDX_LINK_CHIP_MARGIN_C`), alarm at the limit or for 10 min after a kernel log heat report; the worse
of module and chip is the level, JSON `hot` says which ("module" / "chip").

Measured: the test host (kernel 6.14, ConnectX-4, firmware 12.28.2006): port to the test client Module1 49 °C,
warning 53, alarm 58 (INTEL SPTSBP2CLCKS, 100G CWDM4); the DAC cable to the multiseat host has no sensor (name only); the ConnectX-4
has **no hwmon at all**, neither module nor chip, so its only warning of heat is the kernel log. The
multiseat host (Proxmox kernel 6.17, ConnectX-5 Ex `enp8s0f0np0`) has `mlx5` hwmon: `sensor0` (chip, 52 °C idle, crit = limit
105 °C, highest 80 °C) and `Module0` (36 °C, crit 53, highest 58).

### The card fails

A ConnectX-4 with two 100G optics in it failed on the test host after running hot. The kernel log told the
story long before: `temp_warn: High temperature on sensors with bit set 0 1` (chip) for about an hour, then
`Health issue observed, High temperature, severity(3) ERROR`, then within minutes `poll_health: Fatal error 1
detected`, `PCI slot is unavailable`, `health recovery flow aborted, PCI reads still not working`; the
driver unloaded the card and the RDMA device vanished. Reconnecting cannot help; the machine has to be
power-cycled. So that is its own level, `failed`, evaluated every 2 s before anything else:

- the PCI function is gone from `/sys/bus/pci/devices` (it was seen there before: a container without
  `/sys/bus/pci` gives no verdict), or its `config` file reads all ones / fails with EIO (the cached
  `vendor` attribute keeps its value on a dead card, the config read does not),
- the kernel log (for our PCI address only) says `Fatal error ... detected`, `PCI slot is unavailable`,
  `health recovery flow aborted` or `PCI reads still not working`; latched until `Link up` /
  `health recovery succeeded` or a minute of answering PCI plus an active port,
- the RDMA device has been missing for 30 s (`RDX_LINK_NODEV_S`) while the card is on the bus: shown as
  "Network card not available (...): reload the driver or power-cycle",
- no port carries `link_ip` at all and some Mellanox function (vendor 0x15b3) does not answer (the daemon
  started after the card died).

The message is `Network card failed (likely overheated): power-cycle needed` ("likely overheated" only
when the module or chip was at warn/alarm or reported heat in the last 2 h, else the cause, e.g. "PCI
device not answering"); the client's line says "This computer's network card failed ...". It is in
`/api/state` `link` (`level: "failed"`, `message`, `nic` {failed, likely_overheated, power_cycle, why,
detail}, `chip`, `hot`), in the hello and the `link.health` event (the same object), in the menu line
and card (until the card answers), in the tray (urgent notification, menu line) and on both web pages
(banner, "Network card" row). A client whose card failed does not retry silently: the connection error
says "no rdx-hostd at ... - Network card failed (...): power-cycle needed". The link-down bookkeeping
carries over: `last_down.reason` is "the network card overheated" / "failed", the outage ends when the
port is up again ("The link was down for 8 min: this computer's network card had overheated").

Where it shows:

- `/api/state` of both daemons: `link` (this computer); clientd also `peer_link` (the host's, from
  hello and the event `link.health` that hostd sends on every level change) and `link_warning`, the
  one line for the menu.
- In the stream: the line in the menu (warning colour), at the bottom of the placeholder, and as a
  card over the stream when its kind changes (module hot: 8 s; link down: until it is back; back:
  8 s "The link was down for 68 s: the other computer's network module overheated"). The client
  combines both sides: its own port down without a reason of its own, after the host reported
  its module hot, reads "Link down: the other computer's network module overheated (53.6 °C,
  warning at 53 °C)".
- A session that ends while the link is down says so in its error.
- The host and client web pages ("Network link" card, banner) and the trays (a menu line while not
  ok, one notification per change: warn, alarm, down, back); the tray reads only `/api/state`.

## Tests

`tests/test_window_menu` (real rdx_menu.c, real rdx_winmenu.h, fake window input: placeholder,
pointer modes, Mouse entry, link card), `tests/test_link` (fake sysfs tree: resolve, levels,
hysteresis, kernel log reason, chip temperature and heat reports, card failure by PCI config / PCI
directory / driver log / RDMA device / dead Mellanox function, EEPROM parser, JSON, the client's line;
`--live IP` reads a real port read-only), `tests/test_gs_hint` (hint message -> frame header flags),
`tests/test_tray_link.py` (tray transitions), `tests/fake_link_ui.py` (fake daemon for the pages).
