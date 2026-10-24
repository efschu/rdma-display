# "Start here on this PC": an app in a window on the host's own desktop

On a desktop machine (for example a host with an RTX 2080 Ti as the seat GPU and its monitors on
an AMD iGPU running GNOME), an app runs as for streaming (gamescope's rdx backend on the seat GPU)
but its picture goes into a window on the machine's own desktop. One click sends it to the client, one click
brings it back; the app keeps running.

```
 app -> gamescope --backend rdx (2080 Ti) --composite + copy--> host RAM slots (rdx_gs_src)
                                                                    | dma-buf (udmabuf), no copy
                                    rdx_sink_win --local (iGPU, EGL) <-+-- FRAME / CREDIT / SHOWN
                                    window on the desktop, vsync         (unix socket, gs/rdx_local_proto.h)
```

## Pieces

| Where | What |
|---|---|
| `gs/rdx_local_proto.h` | the local sink protocol: the window's hello (a `struct rdx_hello`), BUFS (dma-bufs of the slots), FRAME, CREDIT, SHOWN (+ arrival → visible), DROP, END |
| `gs/rdx_srcsess.{c,h}` | a peer `local:PATH` is a window: unix socket instead of QP, the same events (credits, shown, drops) from its messages; `rdx_srcsess_local_buffers` hands it the slots |
| `gs/rdx_gs_src.c` | `--peer local:PATH`: host buffers, dma-bufs of them to the window (again after every resume and RECONFIG); frame generation off for a window |
| `p0b/rdx_sink_win.c` | `--local PATH`: X11/EGL window, imports the slots (EGL_EXT_image_dma_buf_import, linear XRGB8888), draws the newest frame letterboxed with vsync, credit after the draw, shown after the swap; F11 fullscreen; closing the window ends it |
| `host/rdx_hostd.c` (patch) | `POST /api/local {"action": "here", "app"}` = "Start here on this PC" / "Back to this PC", `{"action": "to_client"}` = "Send to the client" (suspend + `app.offer` event to the connected clients). The window runs as the desktop user with DISPLAY / XAUTHORITY of the desktop shell; its size and refresh are the desktop monitor's (config `local_output`, default the first output). A client starting the same app takes it over. |
| `client/rdx_clientd.c` (patch) | `app.offer` while idle: starts (= resumes) that app |
| `web/host/index.html`, `desktop/rdx-tray.py` (patches) | "Start here on this PC" per app; "Send to the client" / "Back to this PC" for the running or paused app |

Suspend / resume and RECONFIG are rdx_gs_src's existing ones: the window is a sink like a client,
so window ↔ client moves at the other's size with the same app.

## Measured (test host: vkcube on the 2080 Ti, window on the AMD iGPU under GNOME Xorg)

| frame size, announced refresh | frames shown / s | arrival → visible p50 / p90 / p99 | dropped |
|---|---|---|---|
| 1920x1080 @ 60 | 59.7 (vsync-paced) | 3.46 / 3.76 / 4.33 ms (lead loop target 3.5) | 0 |
| 2560x1440 @ 144 | 109.5 | 0.67 / 0.73 / 1.09 ms | 0 |

At 1440p the window is not vsync-paced (a composited window's swap does not wait for the
monitor) and the rate is gamescope's composite + copy into host RAM through the chipset; in
fullscreen GNOME unredirects the window and the swap paces on the monitor.

## Window size and input

Measured on the test host (vkcube, 2080 Ti -> window on the iGPU): the window resized
960x540 -> 1280x720 by xdotool gave one MODE, RECONFIG of gamescope in 18.5 ms, new slots
imported, 0 frames dropped; key events sent to the window (xdotool key --window, the desktop
focus untouched) arrived on "rdx: local window keyboard" (6 of 6 for x y z); gamescope logs
"input: virtual devices and USB/IP only". hostd flow (a test instance of rdx-hostd): Start here 0.08 s,
Send to the client = suspended, Back to this PC = the same rdx_gs_src resumed into a new window,
141 fps on the 144 Hz monitor, arrival -> visible p50 3.5 ms.

- Frames have the window's size (fullscreen: the monitor's). A resize sends MODE ~300 ms after the
  last change; rdx_gs_src RECONFIGs gamescope to it (new slots, the window imports them), so the
  game renders at the window's size instead of being scaled. Reports about the old slots carry
  an older generation and are ignored.
- The window's own keyboard and mouse go to the app while it has the focus: X key / button /
  motion events -> INPUT messages -> rdx_gs_src's uinput devices "rdx: local window keyboard /
  mouse" (keys by evdev code = X keycode - 8, mouse relative in the frame's pixels, wheel steps).
  Losing the focus releases everything that is down. F11 stays the window's (fullscreen).
- gamescope's rdx backend takes every virtual input device (sysfs path under /devices/virtual:
  uinput and uhid, i.e. the client's mirrors from rdx_input_rx, the window's keyboard and mouse,
  Steam Input's virtual devices, HID forwarded by rdx_usb_host) and USB/IP (vhci_hcd), and ignores
  only this host's physical devices, so the desktop's own keyboard and mouse no longer reach the
  app, also while a client streams. `RDX_GS_INPUT=all` in gamescope's environment takes every
  device of the seat. The DRM backend (a seat's own monitor) uses wlroots' libinput and is not
  filtered.

## Not yet

- rdx_gs_src still opens the RDMA device for its protection domain even for a window; a host
  without an RDMA NIC needs a path without it.
- Mouse capture for games that need unbounded relative motion (pointer grab / warp in the
  window) is not there: motion stops at the window's edge.
