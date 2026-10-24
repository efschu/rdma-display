/* SPDX-License-Identifier: MIT
 *
 * rdx-clientd: the receiver's control daemon (Moonlight-like), runs as root.
 *
 * - Web UI on 127.0.0.1:47991 (static files from --web-root, JSON API below),
 *   opened in a browser of the desktop session (tools/rdx-client.desktop).
 * - Keeps a control connection to rdx-hostd (TCP 47989 on the host's link IP,
 *   newline-delimited JSON, common/rdx_ctl.h), reconnecting every 5 s while down.
 * - Runs one session at a time and supervises its parts (common/rdx_proc.h):
 *   p0b/rdx_sink_kms (on its own VT), p5/rdx_audio_rx, p4/rdx_input_tx,
 *   usb/rdx_usb_client and the microphone sender p5/rdx_audio_tx --mic.
 *
 * Microphone (config "mic", default on; "mic_source": "" = the default source, else a node
 * name from lists.mic_sources): session.start asks for it, the host replies "mic_port" when it
 * takes it. The sender connects for the whole session, but opens this computer's microphone
 * only while an app on the host records (p5/rdx_audio_mic.h). Mute (POST /api/mic
 * {"muted": bool}, the in-stream menu, the mic-mute key) sends silence; it stays local.
 *
 * Clipboard (config "clipboard", default on; "clipboard_primary", default on: the selected text and
 * middle click too): both ways between this computer's desktop and the app in the stream. The host
 * runs clip/rdx_clip on the app's Xwayland and replies "clip_port"; we connect our clip/rdx_clip
 * --desktop to it (the desktop session's display, as the window sink finds it; it pushes the
 * desktop's clipboard at once). Text only, up to 1 MiB. The switch decides at the session start
 * whether the host runs its side at all; mid-session (web page, in-stream menu) clip_sync() stops or
 * restarts ours (on again only if the session started with it on); its ending ends nothing.
 *
 * Session start (contract v1): switch to the sink VT (retry until it is active,
 * the desktop may hold DRM master for a moment), start the sink and the audio
 * player, wait for their "listening on" lines, send session.start; once the host
 * replies, start the input sender and the USB client. The session ends when the
 * sink or the input sender exits (exit 10 = release combo LeftCtrl+LeftAlt+Esc),
 * when the host reports session.ended or the control connection drops, or on
 * POST /api/session/stop; the screen then goes back to the desktop VT.
 *
 * API (all JSON):
 *   GET  /api/state            daemon, host, session, lists (modes/inputs/usb), config
 *   GET  /api/config           POST /api/config {partial}: merge, persist, reply full
 *   POST /api/session/start    {"app": "..."} optional, else config.app / first app
 *   POST /api/session/stop
 *   POST /api/refresh          re-read modes, input devices, USB devices; hello again
 *   GET  /api/logs?proc=NAME   text/plain tail of that part's output
 *   POST /api/apps/quit        {"app": ID}: close the host's suspended app (control "app.quit")
 *   POST /api/mic              {"muted": bool}: mute the microphone sender (stays local)
 *   POST /api/host/restart     restart the host program (control "host.restart"; ends a running
 *                              session and closes a paused app there)
 *   GET  /api/seats            the cards of a multiseat host and their state (rdx-seatmgr, config
 *                              "seatmgr"), and which seat each host entry is
 *   POST /api/seats/start      {"host": index, "gpu": PCI, "force": bool}: start that seat on that card
 *
 * Suspended apps: ending a session of a gamescope app only disconnects; the host keeps the
 * app running and reports it as "suspended": {"app","mode","since_s"} in hello and status
 * (polled every 2 s also while idle, passed through as host.suspended). Starting that app
 * again resumes it, with the suspended mode instead of the configured one. A host with several
 * apps in the background ("suspend_multi", up to "apps_max") lists them all in "suspended_apps"
 * [{app,name,mode,since_s,tearing,vram_mib}] (host.suspended_apps; an older host: its one
 * "suspended" as a list of one): starting another app keeps them running, the page marks each
 * running app and offers Resume / Quit for every one, the in-stream menu "Switch to <App>" (this
 * app waits in the background, that one resumes) and "Quit <App>". The host may answer the start
 * of one more app with "vram_warning" {free_mib, threshold_mib, apps, text}: little free memory on
 * its graphics card; session.vram_warning in /api/state and a card in the stream, nothing closed.
 * "resumed_as" in the reply: the host resumed the running instance of the same single-instance
 * program (Steam) under its other entry; the session takes that app id.
 *   GET  /api/icon?app=ID      the app's icon, fetched from the host's web UI (port
 *                              host_web_port, path hello.apps[].icon) and cached, so the
 *                              browser only ever talks to this daemon
 *
 * Waiting is event driven: one epoll over the child pipes/pidfds, the control
 * socket and an eventfd for requests from the web threads; the only timers are the
 * reconnect (5 s while disconnected), the session start steps and the host status
 * poll (2 s while a session runs).
 *
 *   rdx_clientd [--config /etc/rdx/client.json] [--repo DIR] [--web-root DIR]
 *               [--web-port 47991] [--web-bind 127.0.0.1] [--host-port 47989]
 *               [--data-port-base 18650]   sink = base, input = +10, audio = +20, usb = +30
 *               [--no-vt]                  do not switch VTs (tests without a display)
 *               [--menu-sock /run/rdx/menu.sock] [--menu-font TTF] [--no-menu]
 *
 * Several hosts (e.g. one per seat on a multiseat machine): config "hosts":
 * [{"name","address","port"}] ([] = the single host "host" on --host-port, as before) and
 * "active_host" (index). The active host has the full control connection; every other host
 * a light one that only says hello (every 5 s, for its apps and its paused app). Starting an
 * app of another host makes that one active (/api/session/start {"app","host": index}).
 * /api/state carries "hosts": [{index,name,address,port,active,connected,error,info}].
 * Card choice (docs/MULTISEAT.md "Card choice"): with "seatmgr" set, every seat entry (hosts[].seat,
 * else its hello "seat", else its port) can be started on any free card of the multiseat host: the page
 * lists the cards (free / seat N / used by an LLM server ...) per seat; "Start" calls /api/seats/start, waits
 * for the seat's hello and starts the session.
 *
 * Start placeholder (client/rdx_placeholder.h): from session.start until the sink shows the first
 * frame, the sink shows the app's icon, name, "<App> is starting…" and the seconds since the start
 * (resume: "is resuming"), again when the frames stop until the next one; after 2 min it says to
 * check the host and the menu offers "Restart the host program". Drawn on the menu's overlay.
 *
 * Window client (config "sink": "window", e.g. on a computer that is itself a host): the stream
 * shows in a window of this desktop (p0b/rdx_sink_win, frames of the chosen mode letterboxed into
 * the window) instead of on its own VT. Input: the window's own keyboard and mouse while it has the
 * focus (p4/rdx_input_win, attached to the sink's window), never a grab of this computer's devices
 * (only the pointer, captured inside the window in relative mode). The in-stream menu and the
 * start placeholder are drawn into the window (p0b/rdx_winmenu.h), same hotkey. Pointer
 * ("pointer": "auto" | "absolute" | "relative", menu entry "Mouse"): auto = relative and captured
 * when the app hides its cursor and locks the pointer (the seat's hint) or for a mirrored desktop,
 * absolute otherwise (docs/WINDOW-CLIENT.md). Monitor: "mode" null = the native mode of the monitor
 * the window opens on (the sink's mode list has every monitor's mode, "preferred" on that one); the
 * window opens fullscreen there when the frame has that monitor's size ("window_fullscreen", default
 * on), on the monitor it was on last time ("window_monitor", remembered from the sink's
 * "[sink] on monitor" line when a session ends). No VT switch; run it as a second instance with its
 * own --config, --web-port, --data-port-base and --menu-sock (tools/rdx-clientd-window.service).
 *
 * Link health (common/rdx_link.h): this computer's RDMA port and network module ("link" in
 * /api/state) and the host's as it reports them (hello "link", event link.health: "peer_link"). The
 * one line both give ("link_warning": the module passed its warning threshold, the link is down and
 * why, the network card failed: power-cycle needed, it was down for N s and why) is shown in the
 * in-stream menu and on the start placeholder, and
 * a session that ends while the link is down says so.
 *
 * Variable refresh (FreeSync / VRR, config "vrr", default on): the sink gets --vrr; the KMS sink
 * turns VRR on when the monitor can, the window sink reports it when its window is fullscreen on a
 * monitor the compositor drives with a variable refresh. Both print "[sink] display vrr=.. min=..
 * max=.. hz=.."; session.start carries it as "display": {"vrr", "min_hz", "max_hz", "hz"} (with
 * vrr the host sends at the app's pace within min..max instead of at our vblank), a later change
 * (fullscreen off, another monitor) goes to the host as session.update {"display"}. The menu and
 * /api/state ("display") show it ("FreeSync on (48-144 Hz)").
 *
 * Game monitor by lease (a window client on an X11 desktop with two or more monitors, config
 * "game_monitor": "auto" | "lease" | "window"): the monitor the window would open on is taken from
 * the X server (p0b/rdx_lease: RandR off + RRCreateLease) and driven by p0b/rdx_sink_kms on the
 * lease's DRM fd (page flips, VRR, LFC); the desktop stays on the other monitors. Keyboard and mouse
 * are grabbed from evdev (p4/rdx_input_tx) and toggle between stream and desktop with
 * "desktop_hotkey" (default "ctrl+alt+h", Ctrl+Alt+Shift+Z as an alias). auto = lease when
 * rdx_lease --probe says RandR 1.6 on a real X server (not Xwayland) and there are two monitors or
 * more; a refused lease falls back to the window for that session. The monitor comes back when the
 * sink ends; after a crash the state file /run/rdx/lease-<web port>.state brings it back.
 *
 * In-stream menu (default on, client/rdx_menu.h): Ctrl+Alt+Shift+M in the stream opens a
 * menu on an overlay plane of the sink (stats, cursor, output, resume, quit). The sink and
 * the input sender get --menu-sock; cursor/output go to the host as session.update.
 */
#define _GNU_SOURCE
#include "../common/rdx_proc.h"
#include "../common/rdx_ctl.h"
#include "../third_party/cjson/cJSON.h"
#include "../third_party/civetweb/civetweb.h"
#include "rdx_menu.h"
#include "../common/rdx_link.h"
#include "../common/rdx_hotkey.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/vt.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION        1
#define EXIT_RELEASED  10       /* rdx_input_tx: release combo, see common/rdx_input.h */
#define NPROC          6
#define NOTICE_NS      6000000000ull    /* a host notice (session.notice) stays this long */

enum { P_SINK, P_AUDIO, P_INPUT, P_USB, P_MIC, P_CLIP };
static const char *proc_names[NPROC] = {"sink", "audio", "input", "usb", "mic", "clip"};

enum sstate { S_IDLE, S_VT, S_LISTEN, S_REQUEST, S_RUNNING, S_STOPPING };
static const char *sstate_names[] = {"idle", "starting", "starting", "starting", "running", "stopping"};

/* requests from the web threads to the main loop */
enum { CMD_NONE, CMD_START, CMD_STOP, CMD_REFRESH, CMD_RECONNECT, CMD_QUIT_APP, CMD_RESTART_HOST };

static struct {
    pthread_mutex_t mu;                 /* guards everything below for the web threads */
    /* options */
    const char *config_path, *repo, *web_root, *web_bind;
    int web_port, host_port, port_base, no_vt;
    /* persistent settings */
    cJSON *config;
    /* host */
    struct rdx_ctl *ctl;
    int connected;
    cJSON *host_info;                   /* hello reply */
    cJSON *host_status;                 /* last status reply while a session runs */
    cJSON *suspended;                   /* host's suspended app {"app","mode","since_s"}, NULL = none */
    cJSON *suspended_list;              /* every app in the host's background (oldest first), never NULL once known */
    int suspend_multi;                  /* the host keeps several apps in the background */
    cJSON *vram_warning;                /* the host's warning at this session's start, NULL = none */
    cJSON *resume_mode;                 /* mode of the session that resumes a suspended app, else NULL */
    uint64_t t_idle_status_ns;          /* next status poll while idle */
    int id_quit;
    char host_error[256];
    uint64_t next_connect_ns;
    int next_id, id_hello, id_start, id_stop, id_status, id_update;
    int update_monitor;                 /* the pending session.update is "monitor" on (1) / off (0), -1 other */
    /* in-stream menu: socket path while it runs, NULL = off */
    const char *menu_sock, *menu_font;
    int no_menu;
    /* session */
    enum sstate st;
    char app[128], error[256], stage[64];
    uint64_t t_state_ns, t_since_ns, t_retry_ns, t_status_ns;
    uint64_t t_notice_end_ns;           /* the host's notice is on the placeholder until then */
    int vt_tries;
    struct rdx_proc *proc[NPROC];
    int proc_status[NPROC];             /* last waitpid status, -1 = none */
    int input_port, usb_port;           /* from the host's session.start reply (0: our default) */
    int mic_port;                       /* the host's microphone player, 0 = it takes none */
    int mic_muted;                      /* mute of the microphone sender (this daemon's lifetime) */
    int clip_port;                      /* the host's clipboard helper, 0 = none in this session */
    int clip_primary;                   /* our helper runs with PRIMARY (restarted when the switch changes) */
    int clip_kick;                      /* clip_sync may start our helper (session start, settings changed) */
    int clip_menu;                      /* what the menu shows: available * 2 + on, -1 = not told yet */
    cJSON *mic_sources;                 /* rdx_audio_tx --list-sources */
    char monitor[32];                   /* the app also shows on this monitor of the host seat ("": none) */
    /* lists */
    cJSON *modes, *inputs, *usb;
    char usb_note[PATH_MAX + 64];
    /* web -> main */
    int cmd, efd;
    char cmd_app[128];
    int cmd_host;                       /* host index of the request, -1 = the active one */
    int ctl_epfd;                       /* control socket fd in the epoll set, -1 = none */
    char pending_app[128];              /* start this once the newly active host said hello */
    /* link health: the host's link as it reported it (kept over a lost control connection: it says
     * why our link went down), for the host at peer_addr; the line shown in the menu */
    cJSON *peer_link;
    char peer_addr[64], link_line[256];
    /* the sink's display: VRR and its range ("[sink] display ..."), as sent to the host */
    cJSON *display;
    /* game monitor by lease: this session uses it; refused once (window instead); retry the sink */
    int lease_on, lease_refused, lease_retry, sink_only;
    cJSON *lease_probe;
    char lease_state[96];
    char disp_line[160];
    uint64_t t_disp_ns;
    int id_display;
} G;

/* ---------------- hosts ---------------- */

#define MAXH 8
struct hostent { char name[64], addr[64]; int port, seat; };     /* seat: configured, 0 = not given */
/* the other hosts' light connections (hello only), by host index */
static struct probe {
    struct rdx_ctl *ctl;
    cJSON *info;
    char error[160];
    uint64_t next_ns, hello_ns;
    int id_hello, epfd;
} P[MAXH];

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------- config ---------------- */

static cJSON *config_defaults(void)
{
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "host", "10.0.0.2");
    /* several hosts / seats: [{"name","address","port"[,"seat"]}], [] = just "host" above */
    cJSON_AddItemToObject(c, "hosts", cJSON_CreateArray());
    cJSON_AddNumberToObject(c, "active_host", 0);
    /* rdx-seatmgr of a multiseat host ("http://10.0.1.1:8771"): the seats' card choice; "" = none */
    cJSON_AddStringToObject(c, "seatmgr", "");
    cJSON_AddStringToObject(c, "link_ip", "10.0.0.1");
    cJSON_AddStringToObject(c, "rdma_dev", "auto");      /* the RDMA device carrying link_ip */
    cJSON_AddStringToObject(c, "card", "/dev/dri/card1");
    cJSON_AddStringToObject(c, "sink", "kms");          /* "kms": own VT; "window": a window on this desktop */
    cJSON_AddNumberToObject(c, "mtu", 4096);
    cJSON_AddNullToObject(c, "mode");               /* null = the monitor's preferred mode */
    cJSON_AddNumberToObject(c, "fps", 0);            /* 0 = auto (min of host output and our refresh) */
    cJSON_AddBoolToObject(c, "cursor", 1);
    /* a window client's pointer: "auto" (relative + captured when the app locks the pointer, absolute
     * otherwise), "absolute", "relative" */
    cJSON_AddStringToObject(c, "pointer", "auto");
    /* a window client on a desktop with more monitors: the game monitor by DRM lease (auto/lease/window) */
    cJSON_AddStringToObject(c, "game_monitor", "auto");
    /* keyboard and mouse between the stream and the desktop (lease); Ctrl+Alt+Shift+Z works too */
    cJSON_AddStringToObject(c, "desktop_hotkey", "ctrl+alt+h");
    /* variable refresh (FreeSync / VRR) where the monitor and the compositor can */
    cJSON_AddBoolToObject(c, "vrr", 1);
    /* a window client: fullscreen when the mode is the monitor's own; the monitor it was on last time */
    cJSON_AddBoolToObject(c, "window_fullscreen", 1);
    cJSON_AddStringToObject(c, "window_monitor", "");
    cJSON_AddStringToObject(c, "gpu_prio", "high");
    /* "latency": every new frame at once (fps 0); "smooth": an even cadence on this display,
     * e.g. 120 fps from a 144 Hz host output on a 240 Hz mode (sent as fps, plus the hint) */
    cJSON_AddStringToObject(c, "pacing", "latency");
    /* whole-screen (mirror) apps: run the display at exactly the host output's refresh
     * (hello.outputs[].hz_exact, e.g. 143.999 Hz), synthesized by the sink if not listed;
     * the size stays the chosen mode's. Every host frame then gets exactly one vblank. */
    /* no vsync: the sink scans out the buffer the NIC writes into (--tearing), lowest latency,
     * may tear on motion; off until the user decides; wins over match_host */
    cJSON_AddBoolToObject(c, "tearing", 0);
    /* on by default: the screen runs at the host
     * output's exact rate and the capture phase is steered, arrival -> visible ~1 ms, no judder */
    cJSON_AddBoolToObject(c, "match_host", 1);
    cJSON_AddNumberToObject(c, "host_web_port", 47990);
    cJSON_AddBoolToObject(c, "audio", 1);
    cJSON_AddBoolToObject(c, "audible", 1);
    cJSON_AddBoolToObject(c, "mic", 1);                 /* send the microphone (opened on demand) */
    cJSON_AddStringToObject(c, "mic_source", "");       /* "" = the default source */
    cJSON_AddBoolToObject(c, "clipboard", 1);           /* the clipboard both ways (clip/rdx_clip) */
    cJSON_AddBoolToObject(c, "clipboard_primary", 1);   /* and the selected text (middle click) */
    cJSON_AddBoolToObject(c, "input", 1);
    cJSON_AddItemToObject(c, "input_devices", cJSON_CreateArray());   /* empty = all keyboards + mice */
    cJSON_AddItemToObject(c, "usb", cJSON_CreateArray());   /* "ID[:hid|:usbip]" from the USB list */
    cJSON_AddNullToObject(c, "app");
    cJSON_AddNumberToObject(c, "sink_vt", 3);
    cJSON_AddNumberToObject(c, "desk_vt", 2);
    cJSON_AddStringToObject(c, "audio_runtime_dir", "/run/user/1000");   /* PipeWire of the desktop user */
    return c;
}

static int same_type(const cJSON *a, const cJSON *b)
{
    if (cJSON_IsBool(a) && cJSON_IsBool(b)) return 1;
    return (a->type & 0xFF) == (b->type & 0xFF);
}

/* known keys only; "mode" and "app" may be null or their real type */
static int config_merge(cJSON *dst, const cJSON *src, char *err, size_t errn)
{
    if (!cJSON_IsObject(src)) { snprintf(err, errn, "config must be an object"); return -1; }
    const cJSON *it;
    cJSON_ArrayForEach(it, src) {
        cJSON *cur = cJSON_GetObjectItemCaseSensitive(dst, it->string);
        if (!cur) { snprintf(err, errn, "unknown setting \"%s\"", it->string); return -1; }
        int nullable = !strcmp(it->string, "mode") || !strcmp(it->string, "app");
        if (!(nullable && (cJSON_IsNull(it) || cJSON_IsNull(cur) ||
                           (!strcmp(it->string, "mode") ? cJSON_IsObject(it) : cJSON_IsString(it))))
            && !same_type(cur, it)) {
            snprintf(err, errn, "setting \"%s\" has the wrong type", it->string); return -1;
        }
        if (!strcmp(it->string, "sink") &&
            (!cJSON_IsString(it) || (strcmp(it->valuestring, "kms") && strcmp(it->valuestring, "window")))) {
            snprintf(err, errn, "sink is \"kms\" or \"window\""); return -1;
        }
        if (!strcmp(it->string, "game_monitor") &&
            (!cJSON_IsString(it) || (strcmp(it->valuestring, "auto") && strcmp(it->valuestring, "lease") &&
                                     strcmp(it->valuestring, "window")))) {
            snprintf(err, errn, "game_monitor is \"auto\", \"lease\" or \"window\""); return -1;
        }
        struct rdx_hotkey hk_chk;
        if (!strcmp(it->string, "desktop_hotkey") && (!cJSON_IsString(it) || rdx_hotkey_parse(it->valuestring, &hk_chk))) {
            snprintf(err, errn, "desktop_hotkey: modifiers and one key, e.g. \"ctrl+alt+h\""); return -1;
        }
        if (!strcmp(it->string, "pointer") &&
            (!cJSON_IsString(it) || (strcmp(it->valuestring, "auto") && strcmp(it->valuestring, "absolute") &&
                                     strcmp(it->valuestring, "relative")))) {
            snprintf(err, errn, "pointer is \"auto\", \"absolute\" or \"relative\""); return -1;
        }
        if (!strcmp(it->string, "pacing") &&
            (!cJSON_IsString(it) || (strcmp(it->valuestring, "latency") && strcmp(it->valuestring, "smooth")))) {
            snprintf(err, errn, "pacing is \"latency\" or \"smooth\""); return -1;
        }
        if (!strcmp(it->string, "hosts")) {
            const cJSON *e;
            if (cJSON_GetArraySize(it) > MAXH) { snprintf(err, errn, "at most %d hosts", MAXH); return -1; }
            cJSON_ArrayForEach(e, it) {
                const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "address"), *p = cJSON_GetObjectItemCaseSensitive(e, "port");
                const cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "seat");
                if (!cJSON_IsObject(e) || !cJSON_IsString(a) || !a->valuestring[0] || (p && !cJSON_IsNumber(p))) {
                    snprintf(err, errn, "hosts: every entry needs an \"address\" (and a numeric \"port\")"); return -1;
                }
                if (st && (!cJSON_IsNumber(st) || st->valueint < 1)) {
                    snprintf(err, errn, "hosts: \"seat\" is the seat number (1, 2, ...)"); return -1;
                }
            }
        }
        if (!strcmp(it->string, "seatmgr") && cJSON_IsString(it) && it->valuestring[0] &&
            strncmp(it->valuestring, "http://", 7)) {
            snprintf(err, errn, "seatmgr is \"\" or an http:// URL (\"http://10.0.1.1:8771\")"); return -1;
        }
        if (!strcmp(it->string, "mode") && cJSON_IsObject(it)) {
            const cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "w"), *h = cJSON_GetObjectItemCaseSensitive(it, "h");
            if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h) || w->valueint <= 0 || h->valueint <= 0) {
                snprintf(err, errn, "mode needs w and h"); return -1;
            }
        }
        cJSON_ReplaceItemInObjectCaseSensitive(dst, it->string, cJSON_Duplicate(it, 1));
    }
    return 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) { cap = (cap + 4096) * 2; buf = realloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (!r) break;
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static void config_load(void)
{
    G.config = config_defaults();
    char *s = read_file(G.config_path, NULL);
    if (!s) return;
    cJSON *j = cJSON_Parse(s);
    free(s);
    char err[160];
    if (!j || config_merge(G.config, j, err, sizeof(err)))
        fprintf(stderr, "[clientd] %s: %s, using defaults where invalid\n", G.config_path, j ? err : "not JSON");
    cJSON_Delete(j);
}

static const char *cfg_str(const char *k);
static int config_save(void)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s", G.config_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; mkdir(dir, 0755); }
    snprintf(tmp, sizeof(tmp), "%s.tmp", G.config_path);
    char *s = cJSON_Print(G.config);
    FILE *f = fopen(tmp, "w");
    int rc = -1;
    if (f) {
        rc = fputs(s, f) < 0 || fputc('\n', f) < 0;
        rc |= fclose(f);
        if (!rc) rc = rename(tmp, G.config_path);
    }
    cJSON_free(s);
    if (rc) fprintf(stderr, "[clientd] cannot write %s: %s\n", G.config_path, strerror(errno));
    rdx_link_target(cfg_str("link_ip"));                /* the link IP may have changed */
    return rc ? -1 : 0;
}

static const char *cfg_str(const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(G.config, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static int cfg_int(const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(G.config, k); return cJSON_IsNumber(v) ? v->valueint : 0; }

/* the configured hosts; none listed = the single classic host ("host", --host-port) */
static int hosts_list(struct hostent *h)
{
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(G.config, "hosts")) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "address"), *p = cJSON_GetObjectItemCaseSensitive(e, "port");
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
        if (n == MAXH || !cJSON_IsString(a) || !a->valuestring[0]) continue;
        snprintf(h[n].addr, sizeof(h[n].addr), "%s", a->valuestring);
        snprintf(h[n].name, sizeof(h[n].name), "%s", cJSON_IsString(nm) ? nm->valuestring : "");
        h[n].port = cJSON_IsNumber(p) && p->valueint > 0 ? p->valueint : G.host_port;
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "seat");
        h[n].seat = cJSON_IsNumber(st) && st->valueint > 0 ? st->valueint : 0;
        n++;
    }
    if (!n) {
        snprintf(h[0].addr, sizeof(h[0].addr), "%s", cfg_str("host"));
        h[0].name[0] = 0;
        h[0].port = G.host_port;
        h[0].seat = 0;
        n = 1;
    }
    return n;
}

static int active_index(void)
{
    struct hostent h[MAXH];
    int n = hosts_list(h), a = cfg_int("active_host");
    return a >= 0 && a < n ? a : 0;
}

static struct hostent active_host(void)
{
    struct hostent h[MAXH];
    hosts_list(h);
    return h[active_index()];
}
static int cfg_bool(const char *k) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(G.config, k)); }
/* the stream in a window of this desktop (p0b/rdx_sink_win) */
static int window_sink(void) { return !strcmp(cfg_str("sink"), "window"); }

/* ---------------- lists ---------------- */

static cJSON *run_json(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) { cap = (cap + 4096) * 2; buf = realloc(buf, cap); }
        size_t r = fread(buf + n, 1, cap - n - 1, p);
        n += r;
        if (!r) break;
    }
    pclose(p);
    buf[n] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static void list_modes(void)
{
    char cmd[PATH_MAX * 2];
    if (window_sink())        /* the monitors' modes first, "preferred" on the one the window opens on */
        snprintf(cmd, sizeof(cmd), "'%s/p0b/rdx_sink_win' --monitor '%s' --list-modes-json 2>/dev/null", G.repo,
                 cfg_str("window_monitor"));
    else
        snprintf(cmd, sizeof(cmd), "'%s/p0b/rdx_sink_kms' --list-modes-json --card '%s' 2>/dev/null", G.repo, cfg_str("card"));
    cJSON *j = run_json(cmd);
    cJSON_Delete(G.modes);
    G.modes = j ? j : cJSON_Parse("{\"modes\":[],\"error\":\"rdx_sink_kms --list-modes-json failed\"}");
    if (window_sink()) {          /* can this desktop lease a monitor (RandR 1.6, not Xwayland)? */
        snprintf(cmd, sizeof(cmd), "'%s/p0b/rdx_lease' --probe 2>/dev/null", G.repo);
        cJSON_Delete(G.lease_probe);
        G.lease_probe = run_json(cmd);
    }
}

/* does the input device /sys/class/input/<ev> report key <code>? (capabilities/key: hex words,
 * most significant first, one unsigned long each) */
static int input_has_key(const char *ev, unsigned code)
{
    char sys[PATH_MAX + 64], buf[1024];
    snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/capabilities/key", ev);
    FILE *f = fopen(sys, "r");
    if (!f) return 0;
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok) return 0;
    unsigned long w[64];
    int n = 0;
    for (char *s = strtok(buf, " \n"); s && n < 64; s = strtok(NULL, " \n")) w[n++] = strtoul(s, NULL, 16);
    unsigned bits = 8 * sizeof(unsigned long), i = code / bits;
    return i < (unsigned)n && (w[n - 1 - i] >> (code % bits)) & 1;
}

static void list_inputs(void)
{
    cJSON *a = cJSON_CreateArray();
    DIR *d = opendir("/dev/input/by-id");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const char *ev = strstr(e->d_name, "-event-");
        if (!ev) continue;
        char path[PATH_MAX], real[PATH_MAX], sys[PATH_MAX + 64], name[256] = "";
        snprintf(path, sizeof(path), "/dev/input/by-id/%s", e->d_name);
        const char *kind = ev + 7;                      /* kbd, mouse, joystick, ... */
        if (realpath(path, real)) {
            const char *base = strrchr(real, '/');
            /* a keyboard's second interface (media/"Consumer Control" keys) is no "kbd" for udev
             * (by-id "...-event-if01"), but carries the volume keys rdx_input_tx acts on here */
            if (strcmp(kind, "kbd") && strcmp(kind, "mouse") && strcmp(kind, "joystick") &&
                (input_has_key(base ? base + 1 : real, KEY_VOLUMEUP) ||
                 input_has_key(base ? base + 1 : real, KEY_MUTE)))
                kind = "keys";
            snprintf(sys, sizeof(sys), "/sys/class/input/%s/device/name", base ? base + 1 : real);
            FILE *f = fopen(sys, "r");
            if (f) {
                if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
                fclose(f);
            }
            /* some devices pad their name with spaces */
            size_t lead = strspn(name, " ");
            memmove(name, name + lead, strlen(name + lead) + 1);
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddStringToObject(o, "name", name[0] ? name : e->d_name);
        cJSON_AddStringToObject(o, "kind", kind);
        cJSON_AddItemToArray(a, o);
    }
    if (d) closedir(d);
    cJSON_Delete(G.inputs);
    G.inputs = a;
}

static void list_usb(void)
{
    char bin[PATH_MAX], cmd[PATH_MAX + 64];
    snprintf(bin, sizeof(bin), "%s/usb/rdx_usb_client", G.repo);
    cJSON_Delete(G.usb);
    G.usb = NULL;
    G.usb_note[0] = 0;
    if (access(bin, X_OK)) {
        snprintf(G.usb_note, sizeof(G.usb_note), "USB passthrough not built (%s missing)", bin);
    } else {
        snprintf(cmd, sizeof(cmd), "'%s' --list-json 2>/dev/null", bin);
        cJSON *j = run_json(cmd);
        cJSON *devs = j ? cJSON_DetachItemFromObjectCaseSensitive(j, "devices") : NULL;
        cJSON_Delete(j);
        if (cJSON_IsArray(devs)) G.usb = devs;
        else { cJSON_Delete(devs); snprintf(G.usb_note, sizeof(G.usb_note), "rdx_usb_client --list-json failed"); }
    }
    if (!G.usb) G.usb = cJSON_CreateArray();
}

/* the microphones one may pick (PipeWire of the desktop user, never a sink monitor) */
static void list_mic_sources(void)
{
    char bin[PATH_MAX], cmd[PATH_MAX + 64];
    snprintf(bin, sizeof(bin), "%s/p5/rdx_audio_tx", G.repo);
    /* bounded: this runs in the main loop, a hanging PipeWire must not stall it */
    snprintf(cmd, sizeof(cmd), "timeout 3 '%s' --list-sources 2>/dev/null", bin);
    cJSON *j = access(bin, X_OK) ? NULL : run_json(cmd);
    cJSON_Delete(G.mic_sources);
    G.mic_sources = cJSON_IsArray(j) ? j : cJSON_CreateArray();
    if (!cJSON_IsArray(j)) cJSON_Delete(j);
}

static void refresh_lists(void)
{
    /* the mode list probes the connector: never while our sink drives it */
    if (G.st == S_IDLE) list_modes();
    list_inputs();
    list_usb();
    list_mic_sources();
}

/* ---------------- host connection ---------------- */

static void host_drop(const char *why)
{
    if (G.ctl) rdx_ctl_close(G.ctl);
    G.ctl = NULL;
    G.connected = 0;
    G.ctl_epfd = -1;            /* closing removed it from epoll; a new socket may reuse the number */
    if (why) snprintf(G.host_error, sizeof(G.host_error), "%s", why);
    G.next_connect_ns = now_ns() + 5000000000ull;
}

static int host_send(cJSON *msg)
{
    if (!G.ctl) { cJSON_Delete(msg); return -1; }
    int rc = rdx_ctl_send(G.ctl, msg);
    cJSON_Delete(msg);
    if (rc) host_drop("control connection: send failed");
    return rc;
}

static int host_request(const char *cmd, cJSON *msg)
{
    if (!msg) msg = cJSON_CreateObject();
    int id = ++G.next_id;
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "cmd", cmd);
    return host_send(msg) ? -1 : id;
}

static void host_connect(void)
{
    struct hostent ah = active_host();
    const char *host = ah.addr;
    G.next_connect_ns = now_ns() + 5000000000ull;
    G.ctl = rdx_ctl_connect(host, ah.port, 500);
    if (!G.ctl) {
        struct rdx_link_info own;
        rdx_link_get(&own);
        if (own.level == RDX_LINK_FAILED)       /* not a silent retry every 5 s: say what is wrong */
            snprintf(G.host_error, sizeof(G.host_error), "no rdx-hostd at %s:%d \xe2\x80\x93 %.150s", host, ah.port, own.message);
        else
            snprintf(G.host_error, sizeof(G.host_error), "no rdx-hostd at %s:%d", host, ah.port);
        return;
    }
    G.connected = 1;
    G.host_error[0] = 0;
    cJSON *m = cJSON_CreateObject();
    char name[64] = "client";
    gethostname(name, sizeof(name) - 1);
    cJSON_AddStringToObject(m, "client_name", name);
    cJSON_AddNumberToObject(m, "version", VERSION);
    G.id_hello = host_request("hello", m);
    fprintf(stderr, "[clientd] connected to rdx-hostd %s:%d\n", host, ah.port);
}

/* ---------------- the other hosts (hello only) ---------------- */

static void probe_close(int i)
{
    if (P[i].ctl) rdx_ctl_close(P[i].ctl);
    P[i].ctl = NULL;
    P[i].epfd = 0;              /* see host_drop */
    cJSON_Delete(P[i].info);
    P[i].info = NULL;
}

static int probe_request(int i, const char *cmd, cJSON *msg)
{
    if (!msg) msg = cJSON_CreateObject();
    int id = ++G.next_id;
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "cmd", cmd);
    int rc = P[i].ctl ? rdx_ctl_send(P[i].ctl, msg) : -1;
    cJSON_Delete(msg);
    if (rc && P[i].ctl) { probe_close(i); snprintf(P[i].error, sizeof(P[i].error), "control connection: send failed"); }
    return rc ? -1 : id;
}

/* connect / hello the non-active hosts; returns ms until the next due step */
static int probe_tick(void)
{
    struct hostent h[MAXH];
    int n = hosts_list(h), act = active_index(), t = -1;
    uint64_t now = now_ns();
    for (int i = 0; i < MAXH; i++) {
        if (i >= n || i == act) { probe_close(i); continue; }
        if (!P[i].ctl && now >= P[i].next_ns) {
            P[i].next_ns = now + 5000000000ull;
            P[i].ctl = rdx_ctl_connect(h[i].addr, h[i].port, 300);
            if (!P[i].ctl) snprintf(P[i].error, sizeof(P[i].error), "no rdx-hostd at %s:%d", h[i].addr, h[i].port);
            else { P[i].error[0] = 0; P[i].hello_ns = 0; }
        }
        if (P[i].ctl && now >= P[i].hello_ns) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "client_name", "rdx-clientd (list)");
            cJSON_AddNumberToObject(m, "version", VERSION);
            P[i].id_hello = probe_request(i, "hello", m);
            P[i].hello_ns = now + 5000000000ull;
        }
        uint64_t due = P[i].ctl ? P[i].hello_ns : P[i].next_ns;
        int ms = due > now ? (int)((due - now) / 1000000ull) + 1 : 0;
        if (t < 0 || ms < t) t = ms;
    }
    return t;
}

static void probe_message(int i, cJSON *m)
{
    const cJSON *idj = cJSON_GetObjectItemCaseSensitive(m, "id");
    if (!cJSON_IsNumber(idj) || idj->valueint != P[i].id_hello) return;
    cJSON_Delete(P[i].info);
    P[i].info = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "ok")) ? cJSON_Duplicate(m, 1) : NULL;
}

/* ---------------- VT ---------------- */

static int vt_active(void)
{
    int fd = open("/dev/tty0", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct vt_stat vs;
    int r = ioctl(fd, VT_GETSTATE, &vs);
    close(fd);
    return r ? -1 : vs.v_active;
}

static void vt_switch(int vt)
{
    if (G.no_vt || window_sink()) return;
    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("/dev/tty0"); return; }
    if (ioctl(fd, VT_ACTIVATE, vt)) fprintf(stderr, "[clientd] VT_ACTIVATE %d: %s\n", vt, strerror(errno));
    close(fd);
}

/* ---------------- session ---------------- */

static void set_state(enum sstate s, const char *stage)
{
    G.st = s;
    G.t_state_ns = now_ns();
    snprintf(G.stage, sizeof(G.stage), "%s", stage ? stage : "");
}

static void proc_exit(struct rdx_proc *p, int status, void *user);

static int spawn(int idx, char **argv)
{
    G.proc_status[idx] = -1;
    if (G.proc[idx] && !rdx_proc_running(G.proc[idx])) rdx_proc_free(G.proc[idx]);   /* from the last session */
    G.proc[idx] = rdx_proc_spawn(proc_names[idx], argv, proc_exit, (void *)(intptr_t)idx);
    if (!G.proc[idx]) {
        snprintf(G.error, sizeof(G.error), "cannot start %s (%s)", proc_names[idx], argv[0]);
        return -1;
    }
    return 0;
}

static int config_save(void);
static void list_modes(void);
/* the window sink's last "[sink] on monitor NAME": the monitor the next window opens on */
static void remember_monitor(void)
{
    static char buf[16384];
    if (!window_sink() || !G.proc[P_SINK]) return;
    size_t len = rdx_proc_log_tail(G.proc[P_SINK], buf, sizeof(buf) - 1);
    buf[len] = 0;
    const char *last = NULL, *q = buf;
    while ((q = strstr(q, "[sink] on monitor "))) { last = q; q++; }
    if (!last) return;
    char name[32];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(last + 18, " \n"), last + 18);
    if (!name[0] || !strcmp(name, cfg_str("window_monitor"))) return;
    if (cJSON_GetObjectItemCaseSensitive(G.config, "window_monitor"))
        cJSON_ReplaceItemInObjectCaseSensitive(G.config, "window_monitor", cJSON_CreateString(name));
    else
        cJSON_AddStringToObject(G.config, "window_monitor", name);
    config_save();
    list_modes();                                   /* "preferred" (the mode null stands for) follows */
    fprintf(stderr, "[clientd] the window opens on monitor %s next time\n", name);
}

static void session_end(const char *reason, int tell_host)
{
    if (G.st == S_IDLE || G.st == S_STOPPING) return;
    remember_monitor();
    G.t_notice_end_ns = 0;
    if (G.menu_sock) rdx_menu_end();
    struct rdx_link_info own;
    rdx_link_get(&own);
    if (own.level >= RDX_LINK_DOWN && G.link_line[0] && reason && strcmp(reason, "stopped") && !G.error[0])
        snprintf(G.error, sizeof(G.error), "%.120s \xe2\x80\x93 %.130s", reason, G.link_line);   /* not a silent reconnect */
    fprintf(stderr, "[clientd] session ends: %s%s%s\n", reason, G.error[0] && strstr(G.error, G.link_line) && G.link_line[0]
            ? " \xe2\x80\x93 " : "", G.error[0] && strstr(G.error, G.link_line) && G.link_line[0] ? G.link_line : "");
    if (reason && !G.error[0] && strcmp(reason, "stopped")) snprintf(G.error, sizeof(G.error), "%s", reason);
    set_state(S_STOPPING, reason);
    if (tell_host && G.connected) G.id_stop = host_request("session.stop", NULL);
    /* input first: it holds the grabbed devices; the sink last, it holds the screen */
    static const int order[NPROC] = {P_INPUT, P_CLIP, P_MIC, P_USB, P_AUDIO, P_SINK};
    for (int i = 0; i < NPROC; i++) {
        struct rdx_proc *p = G.proc[order[i]];
        if (p && rdx_proc_running(p)) rdx_proc_stop(p, 2000);
    }
    /* this may run inside an exit callback: the main loop frees the ended parts after dispatch */
    if (!G.no_vt) vt_switch(cfg_int("desk_vt"));
    cJSON_Delete(G.host_status);
    G.host_status = NULL;
    set_state(S_IDLE, NULL);
}

static void proc_exit(struct rdx_proc *p, int status, void *user)
{
    int idx = (int)(intptr_t)user;
    (void)p;
    G.proc_status[idx] = status;
    if (G.st == S_IDLE || G.st == S_STOPPING) return;
    if (idx == P_SINK && G.lease_on && G.st == S_LISTEN && WIFEXITED(status) && WEXITSTATUS(status) == 3) {
        fprintf(stderr, "[clientd] no lease for the game monitor: a full-screen window instead\n");
        G.lease_refused = 1;
        G.lease_retry = 1;        /* the main loop starts the window sink (not inside this callback) */
        return;
    }
    char why[128];
    int released = idx == P_INPUT && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_RELEASED;
    if (released)
        snprintf(why, sizeof(why), "released (LeftCtrl+LeftAlt+Esc)");
    else if (WIFEXITED(status))
        snprintf(why, sizeof(why), "%s exited with %d", proc_names[idx], WEXITSTATUS(status));
    else
        snprintf(why, sizeof(why), "%s killed by signal %d", proc_names[idx], WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fprintf(stderr, "[clientd] %s\n", why);
    if (idx == P_CLIP) return;          /* also stopped by its switch; /api/state session.clipboard tells */
    if (idx == P_SINK || idx == P_INPUT) {
        session_end(why, 1);
        if (released) G.error[0] = 0;       /* a normal way out, not an error */
    } else {
        /* audio / USB are not essential: the picture keeps running, the UI shows the warning */
        snprintf(G.error, sizeof(G.error), "%s (session continues)", why);
    }
}

/* the microphone sender's last word: "capture opened" after the last "capture closed" */
static int mic_is_open(void)
{
    static char buf[16384];
    size_t n = rdx_proc_log_tail(G.proc[P_MIC], buf, sizeof(buf) - 1);
    buf[n] = 0;
    char *o = NULL, *c = NULL, *x;
    for (x = buf; (x = strstr(x, "mic: capture opened")); x++) o = x;
    for (x = buf; (x = strstr(x, "mic: capture closed")); x++) c = x;
    return o && (!c || o > c);
}

/* mute / unmute the microphone sender (SIGUSR1 / SIGUSR2), also for the next session */
static void mic_set_muted(int on)
{
    G.mic_muted = on;
    if (G.proc[P_MIC] && rdx_proc_running(G.proc[P_MIC])) kill(rdx_proc_pid(G.proc[P_MIC]), on ? SIGUSR1 : SIGUSR2);
    fprintf(stderr, "[clientd] microphone %s\n", on ? "muted" : "unmuted");
}

static int listening(int idx)
{
    char buf[8192];
    if (!G.proc[idx]) return 0;
    size_t n = rdx_proc_log_tail(G.proc[idx], buf, sizeof(buf) - 1);
    buf[n] = 0;
    return strstr(buf, "listening on") != NULL;
}

/* a selection "ID[:hid|:usbip]" -> the listed device and the path to use (default: its "kind") */
static const cJSON *usb_dev(const char *sel, char *kind, size_t kn)
{
    const char *colon = strrchr(sel, ':');
    size_t idlen = colon ? (size_t)(colon - sel) : strlen(sel);
    const cJSON *d;
    cJSON_ArrayForEach(d, G.usb) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, "id");
        if (!cJSON_IsString(v) || strlen(v->valuestring) != idlen || strncmp(v->valuestring, sel, idlen)) continue;
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(d, "kind");
        snprintf(kind, kn, "%s", colon ? colon + 1 : cJSON_IsString(k) ? k->valuestring : "hid");
        return d;
    }
    return NULL;
}

/* the host's app `app` in its background (hello / status "suspended_apps", else "suspended"), NULL = none */
static const cJSON *suspended_entry(const char *app)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, G.suspended_list) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "app");
        if (cJSON_IsString(a) && !strcmp(a->valuestring, app)) return e;
    }
    const cJSON *a = G.suspended ? cJSON_GetObjectItemCaseSensitive(G.suspended, "app") : NULL;
    return cJSON_IsString(a) && !strcmp(a->valuestring, app) ? G.suspended : NULL;
}

/* the in-stream menu: the host's apps in the background other than this session's ("Switch to", "Quit") */
static void menu_bg_push(void)
{
    if (!G.menu_sock) return;
    const char *ids[8], *names[8];
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, G.suspended_list) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "app"), *nm = cJSON_GetObjectItemCaseSensitive(e, "name");
        if (!cJSON_IsString(a) || n == 8 || (G.st != S_IDLE && !strcmp(a->valuestring, G.app))) continue;
        ids[n] = a->valuestring;
        names[n] = cJSON_IsString(nm) ? nm->valuestring : "";
        const cJSON *ha;                      /* an older host names no app: its hello.apps */
        cJSON_ArrayForEach(ha, cJSON_GetObjectItemCaseSensitive(G.host_info, "apps")) {
            const cJSON *hid = cJSON_GetObjectItemCaseSensitive(ha, "id"), *hn = cJSON_GetObjectItemCaseSensitive(ha, "name");
            if (!names[n][0] && cJSON_IsString(hid) && !strcmp(hid->valuestring, a->valuestring) && cJSON_IsString(hn))
                names[n] = hn->valuestring;
        }
        n++;
    }
    rdx_menu_background(ids, names, n);
}

static void session_start(const char *app)
{
    if (G.st != S_IDLE) { snprintf(G.error, sizeof(G.error), "a session is already %s", sstate_names[G.st]); return; }
    G.error[0] = 0;
    if (!G.connected || !G.host_info) {
        snprintf(G.error, sizeof(G.error), "not connected to rdx-hostd (%.200s)", G.host_error[0] ? G.host_error : "waiting");
        return;
    }
    /* app: explicit, else the saved one, else the host's first */
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(G.host_info, "apps");
    const char *pick = app && app[0] ? app : cfg_str("app");
    if (!pick[0] && cJSON_GetArraySize(apps) > 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(apps, 0), "id");
        pick = cJSON_IsString(id) ? id->valuestring : "";
    }
    if (!pick[0]) { snprintf(G.error, sizeof(G.error), "the host offers no app"); return; }
    snprintf(G.app, sizeof(G.app), "%s", pick);
    if (app && app[0]) {
        cJSON_ReplaceItemInObjectCaseSensitive(G.config, "app", cJSON_CreateString(app));
        config_save();
    }
    /* the host's suspended app: this start resumes it, in the mode it runs in */
    cJSON_Delete(G.resume_mode);
    G.resume_mode = NULL;
    const cJSON *se = suspended_entry(G.app);
    const cJSON *sa = se ? cJSON_GetObjectItemCaseSensitive(se, "app") : NULL;
    const cJSON *sm = se ? cJSON_GetObjectItemCaseSensitive(se, "mode") : NULL;
    cJSON_Delete(G.vram_warning);
    G.vram_warning = NULL;
    if (cJSON_IsString(sa) && !strcmp(sa->valuestring, G.app) && cJSON_IsObject(sm) &&
        cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(sm, "w")) && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(sm, "h"))) {
        G.resume_mode = cJSON_Duplicate(sm, 1);
        fprintf(stderr, "[clientd] resuming %s in its mode %dx%d@%g\n", G.app,
                cJSON_GetObjectItemCaseSensitive(sm, "w")->valueint, cJSON_GetObjectItemCaseSensitive(sm, "h")->valueint,
                cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(sm, "hz")) ? cJSON_GetObjectItemCaseSensitive(sm, "hz")->valuedouble : 0.0);
    }
    G.t_since_ns = now_ns();
    G.vt_tries = 0;
    set_state(S_VT, "switching to the stream VT");
    G.t_retry_ns = 0;                   /* first try right away (main loop timer) */
}

/* the exact refresh of the current mirror app's host output if "match_host" applies, else 0 */
static double match_hz_out(int *ow, int *oh)
{
    if (!cfg_bool("match_host") || cfg_bool("tearing") || strncmp(G.app, "mirror:", 7) || !G.host_info) return 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(G.host_info, "outputs")) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "name");
        if (!cJSON_IsString(nm) || strcmp(nm->valuestring, G.app + 7)) continue;
        const cJSON *jw = cJSON_GetObjectItemCaseSensitive(o, "w"), *jh = cJSON_GetObjectItemCaseSensitive(o, "h");
        if (ow) *ow = cJSON_IsNumber(jw) ? jw->valueint : 0;
        if (oh) *oh = cJSON_IsNumber(jh) ? jh->valueint : 0;
        const cJSON *ex = cJSON_GetObjectItemCaseSensitive(o, "hz_exact"), *hz = cJSON_GetObjectItemCaseSensitive(o, "hz");
        return cJSON_IsNumber(ex) && ex->valuedouble > 0 ? ex->valuedouble : cJSON_IsNumber(hz) ? hz->valuedouble : 0;
    }
    return 0;
}

static double match_hz(void) { return match_hz_out(NULL, NULL); }

/* the display mode of this session: a resumed app keeps the mode it was started with */
static const cJSON *cur_mode(void)
{
    if (G.resume_mode) return G.resume_mode;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(G.config, "mode");
    if (cJSON_IsObject(m) || !window_sink()) return m;
    /* a window without a chosen mode: the native mode of the monitor it opens on (fullscreen there) */
    const cJSON *it, *list = G.modes ? cJSON_GetObjectItemCaseSensitive(G.modes, "modes") : NULL;
    cJSON_ArrayForEach(it, list)
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "preferred"))) return it;
    return m;
}

/* the display size for a matched refresh hz: the chosen mode's; without one the largest size the
 * screen lists (or derives) at hz or faster - the same timings then fit its pixel clock and
 * refresh range - preferring the host output's aspect ratio (ow x oh), else the screen's
 * preferred mode */
static int mode_size(double hz, int ow, int oh, int *w, int *h)
{
    const cJSON *m = cur_mode();
    if (cJSON_IsObject(m)) {
        *w = cJSON_GetObjectItemCaseSensitive(m, "w")->valueint;
        *h = cJSON_GetObjectItemCaseSensitive(m, "h")->valueint;
        return 0;
    }
    const cJSON *it, *list = G.modes ? cJSON_GetObjectItemCaseSensitive(G.modes, "modes") : NULL;
    long best = 0;
    cJSON_ArrayForEach(it, list) {
        int mw = cJSON_GetObjectItemCaseSensitive(it, "w")->valueint, mh = cJSON_GetObjectItemCaseSensitive(it, "h")->valueint;
        long score = (long)mw * mh + ((long)mw * oh == (long)mh * ow ? 1L << 40 : 0);   /* same aspect first */
        if (cJSON_GetObjectItemCaseSensitive(it, "hz")->valuedouble >= hz - 0.5 && score > best) {
            best = score; *w = mw; *h = mh;
        }
    }
    if (best) return 0;
    cJSON_ArrayForEach(it, list)
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "preferred"))) {
            *w = cJSON_GetObjectItemCaseSensitive(it, "w")->valueint;
            *h = cJSON_GetObjectItemCaseSensitive(it, "h")->valueint;
            return 0;
        }
    return -1;
}

/* the refresh the sink will run at: the matched host rate, else the chosen mode's, else the
 * screen's preferred mode's; 0 = unknown */
static double session_hz(double matched)
{
    if (matched > 0) return matched;
    const cJSON *m = cur_mode(), *hz = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "hz") : NULL;
    if (cJSON_IsNumber(hz) && hz->valuedouble > 0) return hz->valuedouble;
    const cJSON *it, *list = G.modes ? cJSON_GetObjectItemCaseSensitive(G.modes, "modes") : NULL;
    cJSON_ArrayForEach(it, list)
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "preferred"))) {
            hz = cJSON_GetObjectItemCaseSensitive(it, "hz");
            return cJSON_IsNumber(hz) ? hz->valuedouble : 0;
        }
    return 0;
}

/* the monitor the game would use, NULL = none: the chosen mode's monitor; a chosen mode without one
 * (a plain {w, h, hz}, as the client page saves it) is shown on the monitor the window opens on, the
 * preferred entry of the window sink's mode list (it follows "window_monitor") */
static const char *game_monitor_name(void)
{
    const cJSON *m = cur_mode(), *mon = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "monitor") : NULL;
    if (cJSON_IsString(mon) && mon->valuestring[0]) return mon->valuestring;
    const cJSON *it, *list = G.modes ? cJSON_GetObjectItemCaseSensitive(G.modes, "modes") : NULL;
    cJSON_ArrayForEach(it, list)
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "preferred"))) {
            mon = cJSON_GetObjectItemCaseSensitive(it, "monitor");
            return cJSON_IsString(mon) && mon->valuestring[0] ? mon->valuestring : NULL;
        }
    return NULL;
}

/* this session's game monitor by lease? (header: "game_monitor") */
static int use_lease(void)
{
    const char *gm = cfg_str("game_monitor");
    if (!window_sink() || G.lease_refused || !strcmp(gm, "window") || !game_monitor_name()) return 0;
    if (!strcmp(gm, "lease")) return 1;
    int nmon = 0;
    const cJSON *it, *ml = G.modes ? cJSON_GetObjectItemCaseSensitive(G.modes, "modes") : NULL;
    cJSON_ArrayForEach(it, ml)
        nmon += cJSON_IsString(cJSON_GetObjectItemCaseSensitive(it, "monitor"));
    return G.lease_probe && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(G.lease_probe, "lease")) &&
           !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(G.lease_probe, "xwayland")) && nmon >= 2;
}

static void start_listeners(void)
{
    char port[16], mtu[16], mode[48], aport[16], match[48], wsize[32], title[400];
    snprintf(port, sizeof(port), "%d", G.port_base);
    snprintf(aport, sizeof(aport), "%d", G.port_base + 20);
    snprintf(mtu, sizeof(mtu), "%d", cfg_int("mtu"));
    char bin[PATH_MAX], abin[PATH_MAX];
    /* the desktop may have come up (or changed) since the lists were read: clientd starts at boot,
     * before the user's X login, and a probe without a desktop never leases */
    if (window_sink()) list_modes();
    G.lease_on = use_lease();
    snprintf(bin, sizeof(bin), "%s/p0b/%s", G.repo, window_sink() && !G.lease_on ? "rdx_sink_win" : "rdx_sink_kms");
    snprintf(abin, sizeof(abin), "%s/p5/rdx_audio_rx", G.repo);
    char *argv[40];
    int n = 0;
    static char lbin[PATH_MAX], lmon[64];
    if (G.lease_on) {             /* rdx_lease takes the monitor and runs the KMS sink on the lease's fd */
        snprintf(lbin, sizeof(lbin), "%s/p0b/rdx_lease", G.repo);
        snprintf(lmon, sizeof(lmon), "%s", game_monitor_name());
        argv[n++] = lbin;
        argv[n++] = "--output"; argv[n++] = lmon;
        argv[n++] = "--state"; argv[n++] = G.lease_state;
        argv[n++] = "--";
    }
    argv[n++] = bin;
    if (G.lease_on) { argv[n++] = "--drm-fd"; argv[n++] = "@LEASEFD@"; }
    argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
    argv[n++] = "--bind"; argv[n++] = (char *)cfg_str("link_ip");
    argv[n++] = "--port"; argv[n++] = port;
    argv[n++] = "--mtu";  argv[n++] = mtu;
    if (!window_sink()) { argv[n++] = "--card"; argv[n++] = (char *)cfg_str("card"); }
    if (G.lease_on) fprintf(stderr, "[clientd] game monitor %s by lease, the desktop stays on the others\n", lmon);
    /* the menu, the start placeholder and the link card: an overlay plane (KMS) or blended into the window */
    if (G.menu_sock) { argv[n++] = "--menu-sock"; argv[n++] = (char *)G.menu_sock; }
    const cJSON *m = cur_mode();
    if (window_sink() && !G.lease_on) {
        /* the window starts at the frame size (F11 = fullscreen), titled with the app */
        int ww = 1920, wh = 1080;
        if (cJSON_IsObject(m)) { ww = cJSON_GetObjectItemCaseSensitive(m, "w")->valueint; wh = cJSON_GetObjectItemCaseSensitive(m, "h")->valueint; }
        snprintf(wsize, sizeof(wsize), "%dx%d", ww, wh);
        snprintf(title, sizeof(title), "%s - %s - rdma-display", G.app, active_host().addr);
        argv[n++] = "--win"; argv[n++] = wsize;
        argv[n++] = "--title"; argv[n++] = title;
        if (cfg_str("window_monitor")[0]) { argv[n++] = "--monitor"; argv[n++] = (char *)cfg_str("window_monitor"); }
        if (!cfg_bool("window_fullscreen")) argv[n++] = "--no-auto-fullscreen";
    }
    if (cJSON_IsObject(m)) {
        const cJSON *hz = cJSON_GetObjectItemCaseSensitive(m, "hz");
        if (cJSON_IsNumber(hz) && hz->valueint > 0)
            snprintf(mode, sizeof(mode), "%dx%d@%d", cJSON_GetObjectItemCaseSensitive(m, "w")->valueint,
                     cJSON_GetObjectItemCaseSensitive(m, "h")->valueint, hz->valueint);
        else
            snprintf(mode, sizeof(mode), "%dx%d", cJSON_GetObjectItemCaseSensitive(m, "w")->valueint,
                     cJSON_GetObjectItemCaseSensitive(m, "h")->valueint);
    }
    /* match the host: --mode WxH@<exact Hz>; the chosen mode stays as --mode-fallback in case the
     * screen cannot run that rate at that size (pixel clock / refresh range) */
    int ow = 0, oh = 0, mw, mhh;
    double mh = match_hz_out(&ow, &oh);
    if (mh > 0 && !mode_size(mh, ow, oh, &mw, &mhh)) {
        snprintf(match, sizeof(match), "%dx%d@%.4f", mw, mhh, mh);
        argv[n++] = "--mode"; argv[n++] = match;
        argv[n++] = "--mode-fallback"; argv[n++] = cJSON_IsObject(m) ? mode : "auto";
        /* one host frame per vblank: flip each frame at once, a late-flip deadline would only make
         * frames that arrive just after it wait a whole period (measured: 76 % vs 99.97 % 1-vblank
         * gaps, together with capture at the host's vblank, see match_hz in session.start) */
        argv[n++] = "--no-late-flip";
    } else if (cJSON_IsObject(m)) {
        argv[n++] = "--mode"; argv[n++] = mode;
    }
    if (cfg_bool("tearing")) argv[n++] = "--tearing";
    else if (cfg_bool("vrr")) argv[n++] = "--vrr";          /* the sink reports what it really runs */
    /* sink slots, not with tearing (one front buffer): the host's app may ask for more ("sink_slots"
     * in hello: frame generation on a helper GPU keeps one slot for the generated frames), and with
     * "sink_slots_auto" the slots a paced stream needs at this refresh with the seat's measured lead
     * (rdx_pace_slots: 4 from ~188 Hz at the default 8 ms lead); the larger of the two */
    char slots[16];
    const cJSON *ap, *ss = NULL, *happs = G.host_info ? cJSON_GetObjectItemCaseSensitive(G.host_info, "apps") : NULL;
    cJSON_ArrayForEach(ap, happs) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(ap, "id");
        if (cJSON_IsString(id) && !strcmp(id->valuestring, G.app)) ss = cJSON_GetObjectItemCaseSensitive(ap, "sink_slots");
    }
    int want = cJSON_IsNumber(ss) && ss->valueint >= 2 && ss->valueint <= 8 ? ss->valueint : 0;
    const cJSON *sa = G.host_info ? cJSON_GetObjectItemCaseSensitive(G.host_info, "sink_slots_auto") : NULL;
    if (G.host_info && !cJSON_IsFalse(sa)) {
        const cJSON *jl = cJSON_GetObjectItemCaseSensitive(G.host_info, "stream_lead_ms");
        double lead = cJSON_IsNumber(jl) ? jl->valuedouble : 0, hz = session_hz(mh);
        int pace = rdx_pace_slots(hz, lead);
        fprintf(stderr, "[clientd] sink slots: %d for %.2f Hz, lead %.2f ms%s%s\n", pace, hz,
                lead > 0 ? lead : RDX_LEAD_DEFAULT_MS, lead > 0 ? " (measured)" : " (default)",
                want > pace ? ", the app asks for more" : "");
        if (pace > 3 && pace > want) want = pace;
    }
    if (!cfg_bool("tearing") && want) {
        snprintf(slots, sizeof(slots), "%d", want);
        argv[n++] = "--slots"; argv[n++] = slots;
    }
    argv[n] = NULL;
    if (spawn(P_SINK, argv)) { session_end(G.error, 0); return; }
    if (G.sink_only) { G.sink_only = 0; return; }   /* the lease was refused: the window, audio runs */
    if (cfg_bool("audio")) {
        n = 0;
        argv[n++] = abin;
        argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
        argv[n++] = "--bind"; argv[n++] = (char *)cfg_str("link_ip");
        argv[n++] = "--port"; argv[n++] = aport;
        argv[n++] = "--mtu";  argv[n++] = mtu;
        if (cfg_bool("audible")) argv[n++] = "--audible";
        argv[n] = NULL;
        if (spawn(P_AUDIO, argv)) snprintf(G.error, sizeof(G.error), "audio player failed to start (session continues)");
    }
    set_state(S_LISTEN, "starting sink and audio player");
}

/* the sink's last "[sink] display vrr=%d min=%f max=%f hz=%f": 0 = found (line copied to line) */
static int sink_display(char *line, size_t ln, int *vrr, double *mn, double *mx, double *hz)
{
    static char buf[16384];
    if (!G.proc[P_SINK]) return -1;
    size_t len = rdx_proc_log_tail(G.proc[P_SINK], buf, sizeof(buf) - 1);
    buf[len] = 0;
    const char *last = NULL, *q = buf;
    while ((q = strstr(q, "[sink] display vrr="))) { last = q; q++; }
    if (!last || sscanf(last, "[sink] display vrr=%d min=%lf max=%lf hz=%lf", vrr, mn, mx, hz) != 4) return -1;
    snprintf(line, ln, "%.*s", (int)strcspn(last, "\n"), last);
    return 0;
}

/* the "display" object of session.start / session.update; fallback: no VRR at the mode's refresh */
static cJSON *display_json(void)
{
    char line[160];
    int vrr = 0;
    double mn = 0, mx = 0, hz = 0;
    cJSON *d = cJSON_CreateObject();
    if (sink_display(line, sizeof(line), &vrr, &mn, &mx, &hz)) {
        const cJSON *m = cur_mode();
        const cJSON *mh = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "hz") : NULL;
        hz = cJSON_IsNumber(mh) ? mh->valuedouble : 0;
        vrr = 0; mn = 0; mx = hz;
        line[0] = 0;
    }
    cJSON_AddBoolToObject(d, "vrr", vrr);
    cJSON_AddNumberToObject(d, "min_hz", mn);
    cJSON_AddNumberToObject(d, "max_hz", mx);
    cJSON_AddNumberToObject(d, "hz", hz);
    snprintf(G.disp_line, sizeof(G.disp_line), "%s", line);
    cJSON_Delete(G.display);
    G.display = cJSON_Duplicate(d, 1);
    if (G.menu_sock) rdx_menu_display(vrr, mn, mx);
    return d;
}

/* while a session runs: a changed display (fullscreen off, another monitor) goes to the host */
static void display_check(void)
{
    if (G.st != S_RUNNING || !G.connected || now_ns() < G.t_disp_ns) return;
    G.t_disp_ns = now_ns() + 1000000000ull;
    char line[160];
    int vrr;
    double mn, mx, hz;
    if (sink_display(line, sizeof(line), &vrr, &mn, &mx, &hz) || !strcmp(line, G.disp_line)) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddItemToObject(m, "display", display_json());
    fprintf(stderr, "[clientd] display changed: %s -> session.update\n", G.disp_line);
    G.id_display = host_request("session.update", m);
}

/* frame rate cap for session.start: the configured one, else for "smooth" pacing the highest
 * even cadence of the display mode (hz / k) that the app's host output can feed */
static int session_fps(void)
{
    int fps = cfg_int("fps");
    if (fps > 0 || strcmp(cfg_str("pacing"), "smooth")) return fps;
    if (match_hz() > 0) return 0;                  /* display runs at the host's rate: already even */
    const cJSON *m = cur_mode();
    const cJSON *hz = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "hz") : NULL;
    int disp = cJSON_IsNumber(hz) ? hz->valueint : 0;
    int src = 0;
    const cJSON *o;
    if (!strncmp(G.app, "mirror:", 7) && G.host_info)
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(G.host_info, "outputs")) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "name"), *h = cJSON_GetObjectItemCaseSensitive(o, "hz");
            if (cJSON_IsString(nm) && !strcmp(nm->valuestring, G.app + 7) && cJSON_IsNumber(h)) src = h->valueint;
        }
    if (disp <= 0) return 0;                       /* unknown display rate: nothing to align to */
    if (src <= 0 || src >= disp) return disp;      /* app renders at the display rate (gamescope) */
    int k = 1;
    while (disp / k > src) k++;
    return disp / k;
}

/* ---------------- start placeholder ---------------- */

static int icon_bytes(int hidx, const char *app_id, unsigned char **data, size_t *len, char *type, size_t tlen);
static uint32_t g_ph_gen;               /* placeholder of this session start; an icon fetched later for an older one is dropped */
struct ph_job { uint32_t gen; int hidx; char app_id[160], name[128]; };

static void *ph_icon_thread(void *arg)
{
    struct ph_job *j = arg;
    unsigned char *data = NULL;
    size_t len = 0;
    char type[64];
    if (!icon_bytes(j->hidx, j->app_id, &data, &len, type, sizeof(type))) {   /* no lock held over the network */
        pthread_mutex_lock(&G.mu);
        if (j->gen == g_ph_gen) rdx_menu_placeholder_icon(j->name, data, len);
        pthread_mutex_unlock(&G.mu);
        free(data);
    }
    free(j);
    return NULL;
}

/* from session.start until the first frame: the app's name from the host's hello, its icon fetched aside */
static void placeholder_start(void)
{
    char name[128] = "";
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(G.host_info, "apps")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(a, "id"), *nm = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (cJSON_IsString(id) && !strcmp(id->valuestring, G.app) && cJSON_IsString(nm))
            snprintf(name, sizeof(name), "%s", nm->valuestring);
    }
    if (!name[0] && !strncmp(G.app, "mirror:", 7)) snprintf(name, sizeof(name), "Screen %.100s", G.app + 7);
    if (!name[0]) snprintf(name, sizeof(name), "%.120s", G.app);
    rdx_menu_placeholder(G.resume_mode ? RDX_PH_RESUME : RDX_PH_START, name);
    g_ph_gen++;
    struct ph_job *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->gen = g_ph_gen;
    j->hidx = active_index();
    snprintf(j->app_id, sizeof(j->app_id), "%s", G.app);
    snprintf(j->name, sizeof(j->name), "%s", name);
    pthread_t t;
    if (pthread_create(&t, NULL, ph_icon_thread, j)) free(j);
    else pthread_detach(t);
}

static void send_start(void)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "app", G.app);
    cJSON_AddNumberToObject(m, "fps", session_fps());
    cJSON_AddStringToObject(m, "pacing", cfg_str("pacing"));
    /* the display runs at exactly this host output refresh: the host should capture at its own
     * vblank (fixed phase to ours) instead of on each commit (phase spread over the period) */
    if (match_hz() > 0) cJSON_AddNumberToObject(m, "match_hz", match_hz());
    cJSON_AddBoolToObject(m, "cursor", cfg_bool("cursor"));
    /* the sink runs --tearing: gamescope apps then flip immediately, unpaced */
    cJSON_AddBoolToObject(m, "tearing", cfg_bool("tearing"));
    /* the client's display: with "vrr" the host sends at the app's pace (min_hz..max_hz) */
    cJSON_AddItemToObject(m, "display", display_json());
    cJSON_AddStringToObject(m, "gpu_prio", cfg_str("gpu_prio"));
    /* only with a live player: the host ends the whole session when its audio sender fails */
    cJSON_AddBoolToObject(m, "audio", cfg_bool("audio") && G.proc[P_AUDIO] && rdx_proc_running(G.proc[P_AUDIO]));
    cJSON_AddBoolToObject(m, "input", cfg_bool("input"));
    char mbin[PATH_MAX];
    snprintf(mbin, sizeof(mbin), "%s/p5/rdx_audio_tx", G.repo);
    cJSON_AddBoolToObject(m, "mic", cfg_bool("mic") && !access(mbin, X_OK));
    /* only with the switch on: off, the host starts no helper on the app's display either (the
     * switch alone must keep the clipboard out of a seat); turned off mid-session it stops our
     * side, turned on again it needs the host's side from the start of the session */
    snprintf(mbin, sizeof(mbin), "%s/clip/rdx_clip", G.repo);
    cJSON_AddBoolToObject(m, "clipboard", cfg_bool("clipboard") && !access(mbin, X_OK));
    cJSON *usb = cJSON_AddArrayToObject(m, "usb");
    const cJSON *id;
    cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(G.config, "usb")) {
        char kind[16];
        const cJSON *d = cJSON_IsString(id) ? usb_dev(id->valuestring, kind, sizeof(kind)) : NULL;
        if (!d) continue;
        cJSON *o = cJSON_Duplicate(d, 1);            /* the listed device, with the chosen path */
        cJSON_ReplaceItemInObjectCaseSensitive(o, "kind", cJSON_CreateString(kind));
        cJSON_AddItemToArray(usb, o);
    }
    cJSON *c = cJSON_AddObjectToObject(m, "client");
    cJSON_AddStringToObject(c, "link_ip", cfg_str("link_ip"));
    cJSON_AddItemToObject(c, "mode", cJSON_Duplicate(cur_mode(), 1));
    /* contract extension shared with rdx-hostd: the data ports of this session
     * (production defaults 18650 video, 18660 input, 18670 audio, 18680 USB) */
    cJSON *ports = cJSON_AddObjectToObject(m, "ports");
    cJSON_AddNumberToObject(ports, "video", G.port_base);
    cJSON_AddNumberToObject(ports, "input", G.port_base + 10);
    cJSON_AddNumberToObject(ports, "audio", G.port_base + 20);
    cJSON_AddNumberToObject(ports, "usb", G.port_base + 30);
    G.id_start = host_request("session.start", m);
    if (G.id_start < 0) { session_end("control connection lost while starting", 0); return; }
    set_state(S_REQUEST, "waiting for the host");
    if (G.menu_sock) placeholder_start();
}

/* the X window of a window sink, from its log line "[sink] window 0x..." */
static int sink_window_id(char *out, size_t n)
{
    static char buf[16384];
    if (!G.proc[P_SINK]) return -1;
    size_t len = rdx_proc_log_tail(G.proc[P_SINK], buf, sizeof(buf) - 1);
    buf[len] = 0;
    const char *w = strstr(buf, "[sink] window 0x");
    if (!w) return -1;
    snprintf(out, n, "%.*s", (int)strspn(w + 14, "0123456789abcdefx"), w + 14);
    return out[0] ? 0 : -1;
}

/* our clipboard helper follows the switch while a session runs: started (once per session start or
 * change of the settings: G.clip_kick), stopped, restarted for another "clipboard_primary". One that
 * ended by itself stays down until the next kick. */
static void clip_sync_proc(void);
static void clip_sync(void)
{
    clip_sync_proc();
    int mv = (G.clip_port > 0) * 2 + cfg_bool("clipboard");
    if (G.menu_sock && G.st == S_RUNNING && mv != G.clip_menu) {
        G.clip_menu = mv;
        rdx_menu_clip(G.clip_port > 0, cfg_bool("clipboard"));
    }
}

static void clip_sync_proc(void)
{
    int run = G.proc[P_CLIP] && rdx_proc_running(G.proc[P_CLIP]);
    int want = G.st == S_RUNNING && G.clip_port > 0 && cfg_bool("clipboard");
    if (run && (!want || G.clip_primary != cfg_bool("clipboard_primary"))) {
        rdx_proc_stop(G.proc[P_CLIP], 1000);
        fprintf(stderr, "[clientd] clipboard %s\n", want ? "restarts (PRIMARY setting)" : "off");
        run = 0;
        G.clip_kick = want;
    }
    if (run || !want || !G.clip_kick) return;
    G.clip_kick = 0;
    static char bin[PATH_MAX], peer[64], port[16];
    snprintf(bin, sizeof(bin), "%s/clip/rdx_clip", G.repo);
    snprintf(peer, sizeof(peer), "%s", active_host().addr);
    snprintf(port, sizeof(port), "%d", G.clip_port);
    G.clip_primary = cfg_bool("clipboard_primary");
    char *argv[] = {bin, "--connect", peer, "--port", port, "--desktop", G.clip_primary ? "--primary" : NULL, NULL};
    if (spawn(P_CLIP, argv)) snprintf(G.error, sizeof(G.error), "clipboard helper failed to start (session continues)");
    else fprintf(stderr, "[clientd] clipboard on (%s:%s%s)\n", peer, port, G.clip_primary ? ", with PRIMARY" : "");
}

static void start_senders(void)
{
    char port[16], mtu[16];
    static char peer[64];
    snprintf(peer, sizeof(peer), "%s", active_host().addr);
    snprintf(port, sizeof(port), "%d", G.input_port ? G.input_port : G.port_base + 10);
    snprintf(mtu, sizeof(mtu), "%d", cfg_int("mtu"));
    char bin[PATH_MAX];
    char *argv[96];
    int n = 0;
    if (cfg_bool("input") && window_sink() && !G.lease_on) {
        /* the window's keyboard and mouse while it has the focus; nothing of this computer is grabbed */
        static char xid[32], frame[32];
        const cJSON *m = cur_mode();
        snprintf(frame, sizeof(frame), "%dx%d", cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "w")->valueint : 1920,
                 cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "h")->valueint : 1080);
        if (sink_window_id(xid, sizeof(xid))) {
            snprintf(G.error, sizeof(G.error), "the window sink named no window: no keyboard and mouse (session continues)");
        } else {
            snprintf(bin, sizeof(bin), "%s/p4/rdx_input_win", G.repo);
            argv[n++] = bin;
            argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
            argv[n++] = "--peer"; argv[n++] = peer;
            argv[n++] = "--port"; argv[n++] = port;
            argv[n++] = "--mtu";  argv[n++] = mtu;
            argv[n++] = "--window"; argv[n++] = xid;
            argv[n++] = "--frame"; argv[n++] = frame;
            /* the menu hotkey and the pointer mode (rdx_menu_pointer) */
            if (G.menu_sock) { argv[n++] = "--menu-sock"; argv[n++] = (char *)G.menu_sock; }
            if (!strcmp(cfg_str("pointer"), "relative")) { argv[n++] = "--pointer"; argv[n++] = "relative"; }
            argv[n++] = "--desktop-hotkey"; argv[n++] = (char *)cfg_str("desktop_hotkey");   /* the captured mouse */
            argv[n] = NULL;
            if (spawn(P_INPUT, argv)) { session_end(G.error, 1); return; }
        }
    } else if (cfg_bool("input")) {
        snprintf(bin, sizeof(bin), "%s/p4/rdx_input_tx", G.repo);
        argv[n++] = bin;
        argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
        argv[n++] = "--peer"; argv[n++] = peer;
        argv[n++] = "--port"; argv[n++] = port;
        argv[n++] = "--mtu";  argv[n++] = mtu;
        if (G.menu_sock) { argv[n++] = "--menu-sock"; argv[n++] = (char *)G.menu_sock; }
        const cJSON *sel = cJSON_GetObjectItemCaseSensitive(G.config, "input_devices"), *it;
        if (cJSON_GetArraySize(sel) > 0) {
            cJSON_ArrayForEach(it, sel)
                if (cJSON_IsString(it) && n < 90) { argv[n++] = "--input"; argv[n++] = it->valuestring; }
        } else {
            cJSON_ArrayForEach(it, G.inputs) {          /* default: every keyboard and mouse, and media keys */
                const char *k = cJSON_GetObjectItemCaseSensitive(it, "kind")->valuestring;
                if ((!strcmp(k, "kbd") || !strcmp(k, "mouse") || !strcmp(k, "keys")) && n < 90) {
                    argv[n++] = "--input";
                    argv[n++] = cJSON_GetObjectItemCaseSensitive(it, "path")->valuestring;
                }
            }
        }
        argv[n] = NULL;
        if (n <= (G.menu_sock ? 11 : 9)) { session_end("no input device to forward (uncheck input or plug one in)", 1); return; }
        if (G.lease_on) {         /* the desktop on the other monitors: keyboard and mouse there and back */
            argv[n++] = "--desktop-hotkey"; argv[n++] = (char *)cfg_str("desktop_hotkey");
            argv[n++] = "--desktop-hotkey"; argv[n++] = "ctrl+alt+shift+z";
            argv[n] = NULL;
        }
        if (spawn(P_INPUT, argv)) { session_end(G.error, 1); return; }
    }
    const cJSON *usel = cJSON_GetObjectItemCaseSensitive(G.config, "usb");
    if (cJSON_GetArraySize(usel) > 0) {
        char ubin[PATH_MAX], uport[16];
        snprintf(ubin, sizeof(ubin), "%s/usb/rdx_usb_client", G.repo);
        snprintf(uport, sizeof(uport), "%d", G.usb_port ? G.usb_port : G.port_base + 30);
        n = 0;
        argv[n++] = ubin;
        argv[n++] = "--peer"; argv[n++] = peer;
        argv[n++] = "--port"; argv[n++] = uport;
        argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
        const cJSON *it;
        cJSON_ArrayForEach(it, usel) {
            char kind[16];
            const cJSON *d = cJSON_IsString(it) ? usb_dev(it->valuestring, kind, sizeof(kind)) : NULL;
            if (!d || n >= 90) continue;
            /* always explicit "ID:kind", the client tool picks the path from it */
            static char use[8][160];
            int u = (n - 7) / 2;
            if (u >= 8) continue;
            snprintf(use[u], sizeof(use[u]), "%s:%s", cJSON_GetObjectItemCaseSensitive(d, "id")->valuestring, kind);
            argv[n++] = "--use"; argv[n++] = use[u];
        }
        argv[n] = NULL;
        if (n > 7 && !access(ubin, X_OK) && spawn(P_USB, argv))
            snprintf(G.error, sizeof(G.error), "USB client failed to start (session continues)");
    }
    if (cfg_bool("mic") && G.mic_port > 0) {
        /* connects now, opens the microphone only while an app on the host records */
        static char mbin[PATH_MAX], mport[16];
        snprintf(mbin, sizeof(mbin), "%s/p5/rdx_audio_tx", G.repo);
        snprintf(mport, sizeof(mport), "%d", G.mic_port);
        const char *src = cfg_str("mic_source");
        n = 0;
        argv[n++] = mbin;
        argv[n++] = "--dev";  argv[n++] = (char *)cfg_str("rdma_dev");
        argv[n++] = "--peer"; argv[n++] = peer;
        argv[n++] = "--port"; argv[n++] = mport;
        argv[n++] = "--mtu";  argv[n++] = mtu;
        argv[n++] = "--seconds"; argv[n++] = "0";
        argv[n++] = "--mic"; argv[n++] = (char *)(src[0] ? src : "default");
        if (G.mic_muted) argv[n++] = "--muted";
        argv[n] = NULL;
        if (spawn(P_MIC, argv)) snprintf(G.error, sizeof(G.error), "microphone sender failed to start (session continues)");
    }
    G.t_status_ns = now_ns() + 2000000000ull;
    set_state(S_RUNNING, NULL);
    G.clip_kick = 1;
    G.clip_menu = -1;
    clip_sync();
    if (G.menu_sock) {                  /* the menu offers the host's outputs */
        const char *names[8];
        int nn = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(G.host_info, "outputs")) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "name");
            if (cJSON_IsString(nm) && nn < 8) names[nn++] = nm->valuestring;
        }
        rdx_menu_session(names, nn, !strncmp(G.app, "mirror:", 7) ? G.app + 7 : "", cfg_bool("cursor"));
        const char *pt = cfg_str("pointer");
        rdx_menu_pointer(!strcmp(pt, "absolute") ? RDX_PTR_ABSOLUTE : !strcmp(pt, "relative") ? RDX_PTR_RELATIVE : RDX_PTR_AUTO,
                         !strncmp(G.app, "mirror:", 7));
        char hkt[48];
        rdx_hotkey_text(cfg_str("desktop_hotkey"), hkt, sizeof(hkt));
        rdx_menu_hotkey(hkt);
        rdx_menu_monitor(G.monitor);
        menu_bg_push();
        rdx_menu_mic(G.proc[P_MIC] && rdx_proc_running(G.proc[P_MIC]), G.mic_muted);
    }
}

/* timers of the start sequence and the session; returns ms until the next one (-1 none) */
static int session_tick(void)
{
    uint64_t now = now_ns();
    switch (G.st) {
    case S_VT: {
        int want = cfg_int("sink_vt");
        if (G.no_vt || window_sink() || vt_active() == want) { start_listeners(); return 0; }
        if (now >= G.t_retry_ns) {
            /* the desktop may still hold DRM master: ask again every 2 s, give up after 30 s */
            if (++G.vt_tries > 15) { session_end("could not switch to the stream VT", 0); return -1; }
            vt_switch(want);
            G.t_retry_ns = now + 2000000000ull;
        }
        return 100;
    }
    case S_LISTEN:
        if (listening(P_SINK) && (!G.proc[P_AUDIO] || !rdx_proc_running(G.proc[P_AUDIO]) || listening(P_AUDIO))) {
            send_start();
            return -1;
        }
        if (now - G.t_state_ns > 10000000000ull) { session_end("sink did not come up within 10 s", 0); return -1; }
        return 100;
    case S_REQUEST:
        if (now - G.t_state_ns > 20000000000ull) { session_end("host did not answer session.start within 20 s", 1); return -1; }
        return (int)((G.t_state_ns + 20000000000ull - now) / 1000000ull) + 1;
    case S_RUNNING:
        clip_sync();                            /* the switch, from the web page or the menu */
        if (G.t_notice_end_ns && now >= G.t_notice_end_ns) {
            G.t_notice_end_ns = 0;
            session_end(G.error, 1);            /* the notice was shown: end with it as the reason */
            return -1;
        }
        if (now >= G.t_status_ns) {
            G.id_status = host_request("status", NULL);
            G.t_status_ns = now + 2000000000ull;
        }
        if (G.t_notice_end_ns && G.t_notice_end_ns < G.t_status_ns)
            return (int)((G.t_notice_end_ns - now) / 1000000ull) + 1;
        return (int)((G.t_status_ns - now) / 1000000ull) + 1;
    case S_IDLE:
        /* the host's suspended app changes while we are idle (quit on the host, resumed elsewhere) */
        if (!G.connected) return -1;
        if (now >= G.t_idle_status_ns) {
            G.id_status = host_request("status", NULL);
            G.t_idle_status_ns = now + 2000000000ull;
        }
        return (int)((G.t_idle_status_ns - now) / 1000000ull) + 1;
    default:
        return -1;
    }
}

/* hello and status carry the host's suspended app (null = none) */
static void take_suspended(const cJSON *m)
{
    const cJSON *su = cJSON_GetObjectItemCaseSensitive(m, "suspended");
    const cJSON *sl = cJSON_GetObjectItemCaseSensitive(m, "suspended_apps");
    if (!su && !sl) return;               /* an older host: leave it */
    cJSON_Delete(G.suspended);
    G.suspended = cJSON_IsObject(su) ? cJSON_Duplicate(su, 1) : NULL;
    cJSON *nl = cJSON_IsArray(sl) ? cJSON_Duplicate(sl, 1) : cJSON_CreateArray();
    if (!cJSON_IsArray(sl) && G.suspended) cJSON_AddItemToArray(nl, cJSON_Duplicate(G.suspended, 1));   /* one, older host */
    const cJSON *mu = cJSON_GetObjectItemCaseSensitive(m, "suspend_multi");
    G.suspend_multi = cJSON_IsTrue(mu);
    /* the menu follows when the apps change (another started, one ended or was quit), not on since_s */
    int changed = !G.suspended_list || cJSON_GetArraySize(G.suspended_list) != cJSON_GetArraySize(nl);
    for (const cJSON *a = G.suspended_list ? G.suspended_list->child : NULL, *b = nl->child; !changed && a && b;
         a = a->next, b = b->next)
        changed = !cJSON_Compare(cJSON_GetObjectItemCaseSensitive(a, "app"), cJSON_GetObjectItemCaseSensitive(b, "app"), 1);
    cJSON_Delete(G.suspended_list);
    G.suspended_list = nl;
    if (changed) menu_bg_push();
}

/* ---------------- link health ---------------- */

/* the host's link object (hello "link", event link.health) */
static void peer_link_set(const cJSON *link)
{
    if (!cJSON_IsObject(link)) return;
    cJSON_Delete(G.peer_link);
    G.peer_link = cJSON_Duplicate(link, 1);
    snprintf(G.peer_addr, sizeof(G.peer_addr), "%s", active_host().addr);
}

/* the line of both links for the menu and the placeholder; called from every main loop pass */
static void link_update(void)
{
    if (G.peer_link && strcmp(G.peer_addr, active_host().addr)) {   /* another host is active now */
        cJSON_Delete(G.peer_link);
        G.peer_link = NULL;
    }
    struct rdx_link_info own, peer;
    rdx_link_get(&own);
    int have_peer = G.peer_link && !rdx_link_from_json(G.peer_link, &peer);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    char line[256];
    rdx_link_line(&own, have_peer ? &peer : NULL, (double)ts.tv_sec, line, sizeof(line));
    if (!strcmp(line, G.link_line)) return;
    snprintf(G.link_line, sizeof(G.link_line), "%s", line);
    fprintf(stderr, "[clientd] link: %s\n", line[0] ? line : "ok");
    if (G.menu_sock) rdx_menu_link(line);
}

/* ---------------- control messages from the host ---------------- */

static void post_cmd(int cmd, const char *app);

static void host_message(cJSON *m)
{
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(m, "event");
    if (cJSON_IsString(ev)) {
        /* only once the host has accepted THIS session: the control stream is ordered, so a
         * session.ended of the previous session (stopped just before) arrives before the reply
         * to our new session.start and must not end the new one */
        /* the app ended right after its start (rdx-hostd's app_ended_check, e.g. it is already open
         * on the host's desktop): the start placeholder shows why, then the session ends with it */
        /* the host says the picture pauses (it switches the mirrored output, the source reconfigures):
         * the placeholder over the last frame until the next one; never on a mere absence of frames */
        const cJSON *why = cJSON_GetObjectItemCaseSensitive(m, "why");
        if (!strcmp(ev->valuestring, "session.stall") && G.st == S_RUNNING) {
            fprintf(stderr, "[clientd] host: the picture pauses (%s)\n", cJSON_IsString(why) ? why->valuestring : "?");
            if (G.menu_sock) rdx_menu_stall(cJSON_IsString(why) ? why->valuestring : "");
            return;
        }
        const cJSON *nt = cJSON_GetObjectItemCaseSensitive(m, "text");
        if (!strcmp(ev->valuestring, "session.notice") && G.st == S_RUNNING && cJSON_IsString(nt)) {
            fprintf(stderr, "[clientd] host notice: %s\n", nt->valuestring);
            snprintf(G.error, sizeof(G.error), "%s", nt->valuestring);
            if (G.menu_sock) rdx_menu_notice(nt->valuestring);
            G.t_notice_end_ns = now_ns() + NOTICE_NS;
            return;
        }
        /* the host's link changed level (its module got hot, its port went down or came back) */
        if (!strcmp(ev->valuestring, "link.health")) {
            const cJSON *lk = cJSON_GetObjectItemCaseSensitive(m, "link");
            peer_link_set(lk);
            if (G.host_info && cJSON_IsObject(lk))
                cJSON_ReplaceItemInObjectCaseSensitive(G.host_info, "link", cJSON_Duplicate(lk, 1));
            link_update();
            return;
        }
        if (!strcmp(ev->valuestring, "session.ended") && G.st == S_RUNNING) {
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(m, "reason");
            char why[200];
            snprintf(why, sizeof(why), "host ended the session: %s", cJSON_IsString(r) ? r->valuestring : "?");
            session_end(why, 0);
        }
        /* "Send to the client" on the host: its app (paused there) starts here, i.e. resumes */
        const cJSON *oa = cJSON_GetObjectItemCaseSensitive(m, "app");
        if (!strcmp(ev->valuestring, "app.offer") && G.st == S_IDLE && cJSON_IsString(oa) && oa->valuestring[0]) {
            fprintf(stderr, "[clientd] the host sends %s here: starting it\n", oa->valuestring);
            post_cmd(CMD_START, oa->valuestring);
        }
        return;
    }
    const cJSON *idj = cJSON_GetObjectItemCaseSensitive(m, "id");
    int id = cJSON_IsNumber(idj) ? idj->valueint : -1;
    int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "ok"));
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(m, "error");
    if (id == G.id_hello) {
        cJSON_Delete(G.host_info);
        G.host_info = ok ? cJSON_Duplicate(m, 1) : NULL;
        if (ok) take_suspended(m);
        if (ok) peer_link_set(cJSON_GetObjectItemCaseSensitive(m, "link"));
        if (!ok) snprintf(G.host_error, sizeof(G.host_error), "hello refused: %s", cJSON_IsString(err) ? err->valuestring : "?");
        if (ok && G.pending_app[0]) {          /* an app of this host was started while switching to it */
            char app[128];
            snprintf(app, sizeof(app), "%s", strcmp(G.pending_app, " ") ? G.pending_app : "");
            G.pending_app[0] = 0;
            session_start(app);
        }
    } else if (id == G.id_start && G.st == S_REQUEST) {
        if (!ok) {
            char why[300];
            snprintf(why, sizeof(why), "host refused the session: %s", cJSON_IsString(err) ? err->valuestring : "?");
            session_end(why, 1);
            return;
        }
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(m, "input_port");
        const cJSON *up = cJSON_GetObjectItemCaseSensitive(m, "usb_port");
        G.input_port = cJSON_IsNumber(ip) ? ip->valueint : 0;
        G.usb_port = cJSON_IsNumber(up) ? up->valueint : 0;
        const cJSON *mp = cJSON_GetObjectItemCaseSensitive(m, "mic_port");
        G.mic_port = cJSON_IsNumber(mp) ? mp->valueint : 0;
        const cJSON *cp = cJSON_GetObjectItemCaseSensitive(m, "clip_port");
        G.clip_port = cJSON_IsNumber(cp) ? cp->valueint : 0;
        const cJSON *mon = cJSON_GetObjectItemCaseSensitive(m, "monitor");
        snprintf(G.monitor, sizeof(G.monitor), "%s", cJSON_IsString(mon) ? mon->valuestring : "");
        /* the host resumed the running instance of the same single-instance program under its own entry */
        const cJSON *ras = cJSON_GetObjectItemCaseSensitive(m, "resumed_as");
        if (cJSON_IsString(ras) && ras->valuestring[0] && strcmp(ras->valuestring, G.app)) {
            fprintf(stderr, "[clientd] the host resumes %s instead of %s (the same program, it runs already)\n",
                    ras->valuestring, G.app);
            snprintf(G.app, sizeof(G.app), "%s", ras->valuestring);
        }
        const cJSON *vw = cJSON_GetObjectItemCaseSensitive(m, "vram_warning");
        cJSON_Delete(G.vram_warning);
        G.vram_warning = cJSON_IsObject(vw) ? cJSON_Duplicate(vw, 1) : NULL;
        const cJSON *vt = G.vram_warning ? cJSON_GetObjectItemCaseSensitive(G.vram_warning, "text") : NULL;
        if (cJSON_IsString(vt)) {
            fprintf(stderr, "[clientd] host: %s\n", vt->valuestring);
            if (G.menu_sock) rdx_menu_warn(vt->valuestring);
        }
        start_senders();
    } else if (id == G.id_update && G.menu_sock && G.update_monitor >= 0) {
        rdx_menu_monitor_result(ok, cJSON_IsString(err) ? err->valuestring : NULL, G.update_monitor);
        G.update_monitor = -1;
    } else if (id == G.id_update && G.menu_sock) {
        const cJSON *cur = cJSON_GetObjectItemCaseSensitive(m, "cursor");
        const cJSON *out = cJSON_GetObjectItemCaseSensitive(m, "output");
        rdx_menu_result(ok, cJSON_IsString(err) ? err->valuestring : NULL, cJSON_IsBool(cur) ? cJSON_IsTrue(cur) : -1,
                        cJSON_IsString(out) ? out->valuestring : NULL);
    } else if (id == G.id_status) {
        take_suspended(m);
        if (G.st != S_IDLE) {
            cJSON_Delete(G.host_status);
            G.host_status = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(m, "session"), 1);
        }
    } else if (id == G.id_quit) {
        if (!ok) snprintf(G.error, sizeof(G.error), "could not quit the app: %s", cJSON_IsString(err) ? err->valuestring : "?");
        take_suspended(m);
        G.id_status = host_request("status", NULL);     /* refresh the suspended app now */
    }
}

/* ---------------- web API (civetweb threads) ---------------- */

static int send_json(struct mg_connection *c, int code, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    size_t n = strlen(s);
    if (code == 200) {
        mg_send_http_ok(c, "application/json", (long long)n);
    } else {
        mg_printf(c, "HTTP/1.1 %d Error\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, n);
    }
    mg_write(c, s, n);
    cJSON_free(s);
    cJSON_Delete(j);
    return code;
}

static int send_err(struct mg_connection *c, int code, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 0);
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(c, code, j);
}

static cJSON *read_body(struct mg_connection *c)
{
    const struct mg_request_info *ri = mg_get_request_info(c);
    long long len = ri->content_length;
    if (len <= 0) return cJSON_CreateObject();
    if (len > 65536) return NULL;
    char *buf = malloc((size_t)len + 1);
    long long got = 0;
    while (got < len) {
        int r = mg_read(c, buf + got, (size_t)(len - got));
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/* caller holds G.mu */
static cJSON *state_json(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON *d = cJSON_AddObjectToObject(s, "daemon");
    cJSON_AddStringToObject(d, "name", "rdx-clientd");
    cJSON_AddNumberToObject(d, "version", VERSION);
    cJSON_AddStringToObject(d, "release_combo", "LeftCtrl+LeftAlt+Esc");

    struct hostent hl[MAXH];
    int nh = hosts_list(hl), act = active_index();
    cJSON *ha = cJSON_AddArrayToObject(s, "hosts");
    for (int i = 0; i < nh; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", i);
        cJSON_AddStringToObject(o, "name", hl[i].name);
        cJSON_AddStringToObject(o, "address", hl[i].addr);
        cJSON_AddNumberToObject(o, "port", hl[i].port);
        cJSON_AddBoolToObject(o, "active", i == act);
        if (i == act) {
            cJSON_AddBoolToObject(o, "connected", G.connected);
            cJSON_AddItemToObject(o, "info", G.host_info ? cJSON_Duplicate(G.host_info, 1) : cJSON_CreateNull());
            if (G.host_error[0]) cJSON_AddStringToObject(o, "error", G.host_error);
        } else {
            cJSON_AddBoolToObject(o, "connected", P[i].ctl != NULL);
            cJSON_AddItemToObject(o, "info", P[i].info ? cJSON_Duplicate(P[i].info, 1) : cJSON_CreateNull());
            if (P[i].error[0]) cJSON_AddStringToObject(o, "error", P[i].error);
        }
        cJSON_AddItemToArray(ha, o);
    }
    cJSON *h = cJSON_AddObjectToObject(s, "host");
    cJSON_AddNumberToObject(h, "index", act);
    cJSON_AddStringToObject(h, "address", hl[act].addr);
    cJSON_AddNumberToObject(h, "port", hl[act].port);
    cJSON_AddBoolToObject(h, "connected", G.connected);
    cJSON_AddItemToObject(h, "info", G.host_info ? cJSON_Duplicate(G.host_info, 1) : cJSON_CreateNull());
    if (G.host_error[0]) cJSON_AddStringToObject(h, "error", G.host_error);
    cJSON_AddItemToObject(h, "suspended", G.suspended ? cJSON_Duplicate(G.suspended, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(h, "suspended_apps", G.suspended_list ? cJSON_Duplicate(G.suspended_list, 1) : cJSON_CreateArray());
    cJSON_AddBoolToObject(h, "suspend_multi", G.suspend_multi);

    cJSON *ss = cJSON_AddObjectToObject(s, "session");
    cJSON_AddStringToObject(ss, "state", sstate_names[G.st]);
    if (G.stage[0]) cJSON_AddStringToObject(ss, "stage", G.stage);
    if (G.st != S_IDLE) {
        cJSON_AddStringToObject(ss, "app", G.app);
        cJSON_AddItemToObject(ss, "mode", cJSON_Duplicate(cur_mode(), 1));     /* null = screen default */
        cJSON_AddBoolToObject(ss, "resumed", G.resume_mode != NULL);
        cJSON_AddNumberToObject(ss, "since_s", (now_ns() - G.t_since_ns) / 1e9);
        if (G.vram_warning) cJSON_AddItemToObject(ss, "vram_warning", cJSON_Duplicate(G.vram_warning, 1));
    }
    if (G.error[0]) cJSON_AddStringToObject(ss, "error", G.error);
    cJSON *pa = cJSON_AddArrayToObject(ss, "procs");
    for (int i = 0; i < NPROC; i++) {
        if (!G.proc[i]) continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", proc_names[i]);
        cJSON_AddNumberToObject(p, "pid", rdx_proc_pid(G.proc[i]));
        cJSON_AddBoolToObject(p, "running", rdx_proc_running(G.proc[i]));
        cJSON_AddNumberToObject(p, "cpu_pct", rdx_proc_cpu_pct(G.proc[i]));
        cJSON_AddItemToArray(pa, p);
    }
    cJSON_AddItemToObject(ss, "host", G.host_status ? cJSON_Duplicate(G.host_status, 1) : cJSON_CreateNull());
    cJSON *mic = cJSON_AddObjectToObject(ss, "mic");
    int mic_run = G.proc[P_MIC] && rdx_proc_running(G.proc[P_MIC]);
    cJSON_AddBoolToObject(mic, "connected", mic_run);
    cJSON_AddBoolToObject(mic, "open", mic_run && mic_is_open());
    cJSON_AddBoolToObject(mic, "muted", G.mic_muted);
    /* the clipboard: the host offers it in this session (available), our switch, our helper runs */
    cJSON *clip = cJSON_AddObjectToObject(ss, "clipboard");
    cJSON_AddBoolToObject(clip, "available", G.st == S_RUNNING && G.clip_port > 0);
    cJSON_AddBoolToObject(clip, "on", cfg_bool("clipboard"));
    cJSON_AddBoolToObject(clip, "running", G.proc[P_CLIP] && rdx_proc_running(G.proc[P_CLIP]));

    cJSON *l = cJSON_AddObjectToObject(s, "lists");
    cJSON_AddItemToObject(l, "modes", G.modes ? cJSON_Duplicate(G.modes, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(l, "inputs", G.inputs ? cJSON_Duplicate(G.inputs, 1) : cJSON_CreateArray());
    cJSON *u = cJSON_AddObjectToObject(l, "usb");
    cJSON_AddItemToObject(u, "devices", G.usb ? cJSON_Duplicate(G.usb, 1) : cJSON_CreateArray());
    if (G.usb_note[0]) cJSON_AddStringToObject(u, "note", G.usb_note);
    cJSON_AddItemToObject(l, "mic_sources", G.mic_sources ? cJSON_Duplicate(G.mic_sources, 1) : cJSON_CreateArray());

    struct rdx_link_info li;
    rdx_link_get(&li);
    cJSON_AddItemToObject(s, "link", rdx_link_json(&li));        /* this computer's port and module */
    cJSON_AddItemToObject(s, "peer_link", G.peer_link ? cJSON_Duplicate(G.peer_link, 1) : cJSON_CreateNull());
    cJSON_AddStringToObject(s, "link_warning", G.link_line);    /* the line of the in-stream menu */
    /* the game monitor: by lease in this session, what the X server can (rdx_lease --probe) */
    cJSON *gmj = cJSON_AddObjectToObject(s, "game_monitor");
    cJSON_AddBoolToObject(gmj, "lease", G.lease_on && G.st != S_IDLE);
    cJSON_AddItemToObject(gmj, "probe", G.lease_probe ? cJSON_Duplicate(G.lease_probe, 1) : cJSON_CreateNull());
    /* the session's display as sent to the host: {"vrr", "min_hz", "max_hz", "hz"}, null = no session */
    cJSON_AddItemToObject(s, "display", G.display && G.st != S_IDLE ? cJSON_Duplicate(G.display, 1) : cJSON_CreateNull());

    cJSON_AddItemToObject(s, "config", cJSON_Duplicate(G.config, 1));
    return s;
}

static void post_cmd_host(int cmd, const char *app, int host)
{
    /* caller holds G.mu */
    G.cmd = cmd;
    G.cmd_host = host;
    snprintf(G.cmd_app, sizeof(G.cmd_app), "%s", app ? app : "");
    uint64_t one = 1;
    if (write(G.efd, &one, sizeof(one)) < 0) perror("eventfd");
}

static void post_cmd(int cmd, const char *app) { post_cmd_host(cmd, app, -1); }

/* ---------------- app icons from the host ---------------- */

#define ICON_MAX  (512 * 1024)
#define ICON_SLOTS 32
static struct icon { char app[192]; char type[64]; char *data; size_t len; uint64_t t_ns; } g_icons[ICON_SLOTS];
static pthread_mutex_t g_icon_mu = PTHREAD_MUTEX_INITIALIZER;

static int fetch_icon(const char *host, int port, const char *path, struct icon *out)
{
    char ebuf[128];
    struct mg_connection *hc = mg_connect_client(host, port, 0, ebuf, sizeof(ebuf));
    if (!hc) return -1;
    mg_printf(hc, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    int rc = -1;
    if (mg_get_response(hc, ebuf, sizeof(ebuf), 2000) >= 0) {
        const struct mg_response_info *ri = mg_get_response_info(hc);
        const char *ct = NULL;
        for (int i = 0; ri && i < ri->num_headers; i++)
            if (!strcasecmp(ri->http_headers[i].name, "Content-Type")) ct = ri->http_headers[i].value;
        if (ri && ri->status_code == 200 && ct && !strncmp(ct, "image/", 6)) {
            out->data = malloc(ICON_MAX);
            out->len = 0;
            int r;
            while (out->len < ICON_MAX && (r = mg_read(hc, out->data + out->len, ICON_MAX - out->len)) > 0)
                out->len += (size_t)r;
            snprintf(out->type, sizeof(out->type), "%s", ct);
            rc = out->len > 0 && out->len < ICON_MAX ? 0 : -1;
            if (rc) { free(out->data); out->data = NULL; }
        }
    }
    mg_close_connection(hc);
    return rc;
}

/* the app's icon (cached 5 min, else fetched from the host's web UI) as a malloc'd copy:
 * 0 = ok, 404 = the app has none, 502 = the host did not deliver it */
static int icon_bytes(int hidx, const char *app_id, unsigned char **data, size_t *len, char *type, size_t tlen)
{
    /* the icon path comes from the host's hello, never from the request */
    char host[128] = "", path[256] = "", app[160];
    int port;
    pthread_mutex_lock(&G.mu);
    struct hostent hl[MAXH];
    int nh = hosts_list(hl), act = active_index();
    if (hidx < 0 || hidx >= nh) hidx = act;
    const cJSON *info = hidx == act ? G.host_info : P[hidx].info;
    snprintf(app, sizeof(app), "%d:%s", hidx, app_id);          /* cache key: apps of two hosts may share ids */
    const cJSON *a;
    if (info)
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(info, "apps")) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(a, "id"), *ic = cJSON_GetObjectItemCaseSensitive(a, "icon");
            if (cJSON_IsString(id) && !strcmp(id->valuestring, app_id) && cJSON_IsString(ic) && ic->valuestring[0] == '/')
                snprintf(path, sizeof(path), "%s", ic->valuestring);
        }
    snprintf(host, sizeof(host), "%s", hl[hidx].addr);
    /* a seat's web UI is on its own port (hello.web_port) */
    const cJSON *wp = info ? cJSON_GetObjectItemCaseSensitive(info, "web_port") : NULL;
    port = cJSON_IsNumber(wp) && wp->valueint > 0 && (hidx != act || nh > 1) ? wp->valueint : cfg_int("host_web_port");
    pthread_mutex_unlock(&G.mu);
    if (!path[0] || port <= 0) return 404;

    pthread_mutex_lock(&g_icon_mu);
    struct icon *hit = NULL, *oldest = &g_icons[0];
    for (int i = 0; i < ICON_SLOTS; i++) {
        if (g_icons[i].data && !strcmp(g_icons[i].app, app)) hit = &g_icons[i];
        if (g_icons[i].t_ns < oldest->t_ns) oldest = &g_icons[i];
    }
    if (hit && now_ns() - hit->t_ns > 300000000000ull) { free(hit->data); hit->data = NULL; oldest = hit; hit = NULL; }
    if (!hit) {
        struct icon fresh = {0};
        pthread_mutex_unlock(&g_icon_mu);
        int rc = fetch_icon(host, port, path, &fresh);    /* no lock held over the network */
        if (rc) return 502;
        pthread_mutex_lock(&g_icon_mu);
        free(oldest->data);
        *oldest = fresh;
        snprintf(oldest->app, sizeof(oldest->app), "%s", app);
        oldest->t_ns = now_ns();
        hit = oldest;
    }
    *data = malloc(hit->len);
    if (*data) memcpy(*data, hit->data, hit->len);
    *len = hit->len;
    snprintf(type, tlen, "%s", hit->type);
    pthread_mutex_unlock(&g_icon_mu);
    return *data ? 0 : 502;
}

static int h_icon(struct mg_connection *c, const char *app_id, int hidx)
{
    unsigned char *data = NULL;
    size_t len = 0;
    char type[64];
    int rc = icon_bytes(hidx, app_id, &data, &len, type, sizeof(type));
    if (rc == 404) return send_err(c, 404, "this app has no icon");
    if (rc) return send_err(c, 502, "could not fetch the icon from the host");
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                 "Cache-Control: max-age=300\r\nConnection: close\r\n\r\n", type, len);
    mg_write(c, data, len);
    free(data);
    return 200;
}

/* ---------------- card choice of a multiseat host (rdx-seatmgr, config "seatmgr") ----------------
 * GET /api/seats: seatmgr's /api/gpus (every card: free / seat N / llm / busy / booked, every seat and
 * its card) plus "hosts": [{index, seat}]. POST /api/seats/start {"host": index, "gpu": PCI, "force"}:
 * seatmgr starts that host's seat on the card (shut down first if it runs) and answers when `pct start`
 * is done; the page then waits for the seat's hello and starts the session. The browser only ever talks
 * to this daemon; seatmgr accepts starts only from its "control_from" (the RDMA link). */

/* the seat number of host index i (caller holds G.mu): configured "seat", else the one its hello said,
 * else the port convention (control 47989 + 100 * N) */
static int host_seat(const struct hostent *hl, int nh, int i)
{
    if (i < 0 || i >= nh) return 0;
    if (hl[i].seat > 0) return hl[i].seat;
    const cJSON *info = i == active_index() ? G.host_info : P[i].info;
    const cJSON *s = info ? cJSON_GetObjectItemCaseSensitive(info, "seat") : NULL;
    if (cJSON_IsNumber(s) && s->valueint > 0) return s->valueint;
    int d = hl[i].port - 47989;
    return d > 0 && d % 100 == 0 ? d / 100 : 0;
}

/* one request to seatmgr: its reply body (malloc'd, NUL-terminated) and HTTP status; NULL = unreachable */
static char *seatmgr_call(const char *url, const char *method, const char *path, const char *body, int timeout_ms,
                          int *status, char *err, size_t errn)
{
    char host[128] = "";
    int port = 80;
    if (sscanf(url, "http://%127[^:/]:%d", host, &port) < 1 || !host[0]) {
        snprintf(err, errn, "seatmgr URL %s is not http://HOST:PORT", url);
        return NULL;
    }
    char ebuf[160];
    struct mg_connection *hc = mg_connect_client(host, port, 0, ebuf, sizeof(ebuf));
    if (!hc) { snprintf(err, errn, "cannot reach the seat manager at %s:%d (%s)", host, port, ebuf); return NULL; }
    size_t bl = body ? strlen(body) : 0;
    mg_printf(hc, "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
              method, path, host, bl);
    if (bl) mg_write(hc, body, bl);
    char *out = NULL;
    if (mg_get_response(hc, ebuf, sizeof(ebuf), timeout_ms) >= 0) {
        const struct mg_response_info *ri = mg_get_response_info(hc);
        *status = ri ? ri->status_code : 502;
        size_t cap = 1 << 20, n = 0;
        out = malloc(cap + 1);
        int r;
        while (out && n < cap && (r = mg_read(hc, out + n, cap - n)) > 0) n += (size_t)r;
        if (out) out[n] = 0;
    } else {
        snprintf(err, errn, "no answer from the seat manager (%s)", ebuf);
    }
    mg_close_connection(hc);
    return out;
}

static int h_seats(struct mg_connection *c, int post)
{
    char url[256], err[300] = "";
    struct hostent hl[MAXH];
    pthread_mutex_lock(&G.mu);
    snprintf(url, sizeof(url), "%s", cfg_str("seatmgr"));
    int nh = hosts_list(hl), seats[MAXH], busy_host = G.st != S_IDLE ? active_index() : -1;
    for (int i = 0; i < nh; i++) seats[i] = host_seat(hl, nh, i);
    pthread_mutex_unlock(&G.mu);
    if (!post) {
        cJSON *j = NULL;
        if (url[0]) {
            int st = 0;
            char *b = seatmgr_call(url, "GET", "/api/gpus", NULL, 4000, &st, err, sizeof(err));
            j = b && st == 200 ? cJSON_Parse(b) : NULL;
            if (b && !j) snprintf(err, sizeof(err), "the seat manager answered %d", st);
            free(b);
        }
        if (!j) j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "enabled", url[0] != 0);
        if (err[0]) cJSON_AddStringToObject(j, "error", err);
        cJSON *ha = cJSON_AddArrayToObject(j, "hosts");
        for (int i = 0; i < nh; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "index", i);
            cJSON_AddNumberToObject(o, "seat", seats[i]);
            cJSON_AddItemToArray(ha, o);
        }
        return send_json(c, 200, j);
    }
    cJSON *body = read_body(c);
    const cJSON *hj = body ? cJSON_GetObjectItemCaseSensitive(body, "host") : NULL;
    const cJSON *gj = body ? cJSON_GetObjectItemCaseSensitive(body, "gpu") : NULL;
    int hidx = cJSON_IsNumber(hj) ? hj->valueint : -1, force = body && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "force"));
    char gpu[32];
    snprintf(gpu, sizeof(gpu), "%s", cJSON_IsString(gj) ? gj->valuestring : "");
    cJSON_Delete(body);
    if (!url[0]) return send_err(c, 400, "no seat manager configured (config \"seatmgr\")");
    if (hidx < 0 || hidx >= nh || !gpu[0]) return send_err(c, 400, "need {\"host\": index, \"gpu\": PCI address}");
    if (seats[hidx] <= 0) return send_err(c, 400, "this host is no seat of a multiseat host (config hosts[].seat)");
    if (hidx == busy_host) return send_err(c, 409, "a stream to this seat runs: disconnect first");
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "seat", seats[hidx]);
    cJSON_AddStringToObject(req, "gpu", gpu);
    cJSON_AddBoolToObject(req, "force", force);
    char *rs = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    fprintf(stderr, "[clientd] seat %d (host %d): start on %s%s\n", seats[hidx], hidx, gpu, force ? " (forced)" : "");
    int st = 0;
    char *b = seatmgr_call(url, "POST", "/api/seat/start", rs, 240000, &st, err, sizeof(err));
    cJSON_free(rs);
    cJSON *j = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!j) return send_err(c, 502, err[0] ? err : "the seat manager gave no JSON answer");
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    fprintf(stderr, "[clientd] seat %d: %s\n", seats[hidx], st == 200 ? "started" : cJSON_IsString(e) ? e->valuestring : "failed");
    if (st == 200) {
        pthread_mutex_lock(&G.mu);
        if (hidx != active_index()) P[hidx].next_ns = 0;      /* say hello to the restarted seat right away */
        pthread_mutex_unlock(&G.mu);
    }
    return send_json(c, st == 200 ? 200 : st ? st : 502, j);
}

static int h_api(struct mg_connection *c, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(c);
    const char *uri = ri->local_uri, *meth = ri->request_method;
    int post = !strcmp(meth, "POST"), get = !strcmp(meth, "GET");
    int rc;
    if (get && !strcmp(uri, "/api/state")) {
        pthread_mutex_lock(&G.mu);
        cJSON *s = state_json();
        pthread_mutex_unlock(&G.mu);
        return send_json(c, 200, s);
    }
    if (!strcmp(uri, "/api/config") && (get || post)) {
        cJSON *body = NULL;
        if (post && !(body = read_body(c))) return send_err(c, 400, "body is not JSON");
        char err[160] = "";
        pthread_mutex_lock(&G.mu);
        char *old_host = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(G.config, "hosts"));
        char old_act[200];
        snprintf(old_act, sizeof(old_act), "%s %d %d", cfg_str("host"), cfg_int("active_host"), G.host_port);
        rc = post ? config_merge(G.config, body, err, sizeof(err)) : 0;
        if (post && !rc) {
            config_save();
            G.clip_kick = 1;                    /* the clipboard switch acts at once (clip_sync) */
            uint64_t one = 1;
            if (G.st == S_RUNNING && write(G.efd, &one, sizeof(one)) < 0) perror("eventfd");
            char *new_host = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(G.config, "hosts"));
            char new_act[200];
            snprintf(new_act, sizeof(new_act), "%s %d %d", cfg_str("host"), cfg_int("active_host"), G.host_port);
            if ((strcmp(old_host, new_host) || strcmp(old_act, new_act)) && G.st == S_IDLE) post_cmd(CMD_RECONNECT, NULL);
            free(new_host);
        }
        cJSON *out = cJSON_Duplicate(G.config, 1);
        pthread_mutex_unlock(&G.mu);
        free(old_host);
        cJSON_Delete(body);
        if (rc) { cJSON_Delete(out); return send_err(c, 400, err); }
        return send_json(c, 200, out);
    }
    if (post && (!strcmp(uri, "/api/session/start") || !strcmp(uri, "/api/session/stop") ||
                 !strcmp(uri, "/api/refresh"))) {
        cJSON *body = read_body(c);
        const cJSON *app = body ? cJSON_GetObjectItemCaseSensitive(body, "app") : NULL;
        const cJSON *hj = body ? cJSON_GetObjectItemCaseSensitive(body, "host") : NULL;
        pthread_mutex_lock(&G.mu);
        post_cmd_host(!strcmp(uri, "/api/refresh") ? CMD_REFRESH : strstr(uri, "start") ? CMD_START : CMD_STOP,
                      cJSON_IsString(app) ? app->valuestring : NULL, cJSON_IsNumber(hj) ? hj->valueint : -1);
        pthread_mutex_unlock(&G.mu);
        cJSON_Delete(body);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", 1);
        cJSON_AddStringToObject(j, "note", "queued, follow /api/state");
        return send_json(c, 200, j);
    }
    if (post && !strcmp(uri, "/api/host/restart")) {
        pthread_mutex_lock(&G.mu);
        int conn = G.connected;
        if (conn) post_cmd(CMD_RESTART_HOST, "");
        pthread_mutex_unlock(&G.mu);
        if (!conn) return send_err(c, 503, "not connected to the host");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", 1);
        cJSON_AddStringToObject(j, "note", "the host program restarts; it reconnects in a few seconds");
        return send_json(c, 200, j);
    }
    if (post && !strcmp(uri, "/api/apps/quit")) {
        cJSON *body = read_body(c);
        const cJSON *app = body ? cJSON_GetObjectItemCaseSensitive(body, "app") : NULL;
        if (!cJSON_IsString(app) || !app->valuestring[0]) { cJSON_Delete(body); return send_err(c, 400, "need {\"app\": ID}"); }
        const cJSON *hj = cJSON_GetObjectItemCaseSensitive(body, "host");
        int hidx = cJSON_IsNumber(hj) ? hj->valueint : -1;
        pthread_mutex_lock(&G.mu);
        int conn = hidx < 0 || hidx == active_index() ? G.connected : hidx < MAXH && P[hidx].ctl != NULL;
        if (conn) post_cmd_host(CMD_QUIT_APP, app->valuestring, hidx);
        pthread_mutex_unlock(&G.mu);
        cJSON_Delete(body);
        if (!conn) return send_err(c, 503, "not connected to the host");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", 1);
        cJSON_AddStringToObject(j, "note", "queued, follow /api/state (host.suspended)");
        return send_json(c, 200, j);
    }
    if (post && !strcmp(uri, "/api/mic")) {
        cJSON *body = read_body(c);
        const cJSON *mu = body ? cJSON_GetObjectItemCaseSensitive(body, "muted") : NULL;
        if (!cJSON_IsBool(mu)) { cJSON_Delete(body); return send_err(c, 400, "need {\"muted\": true|false}"); }
        pthread_mutex_lock(&G.mu);
        mic_set_muted(cJSON_IsTrue(mu));
        if (G.menu_sock && G.st == S_RUNNING)
            rdx_menu_mic(G.proc[P_MIC] && rdx_proc_running(G.proc[P_MIC]), G.mic_muted);
        pthread_mutex_unlock(&G.mu);
        cJSON_Delete(body);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", 1);
        cJSON_AddBoolToObject(j, "muted", cJSON_IsTrue(mu));
        return send_json(c, 200, j);
    }
    if ((get && !strcmp(uri, "/api/seats")) || (post && !strcmp(uri, "/api/seats/start")))
        return h_seats(c, post);
    if (get && !strcmp(uri, "/api/icon")) {
        char app[128] = "", hs[8] = "";
        if (!ri->query_string || mg_get_var(ri->query_string, strlen(ri->query_string), "app", app, sizeof(app)) <= 0)
            return send_err(c, 400, "need ?app=ID");
        int hidx = mg_get_var(ri->query_string, strlen(ri->query_string), "host", hs, sizeof(hs)) > 0 ? atoi(hs) : -1;
        return h_icon(c, app, hidx);
    }
    if (get && !strcmp(uri, "/api/logs")) {
        char name[32] = "";
        if (!ri->query_string || mg_get_var(ri->query_string, strlen(ri->query_string), "proc", name, sizeof(name)) <= 0)
            return send_err(c, 400, "need ?proc=sink|audio|input|usb|mic|clip");
        static char buf[65536];
        size_t n = 0;
        int found = 0;
        pthread_mutex_lock(&G.mu);
        for (int i = 0; i < NPROC; i++)
            if (!strcmp(name, proc_names[i]) && G.proc[i]) { n = rdx_proc_log_tail(G.proc[i], buf, sizeof(buf)); found = 1; }
        if (found) {
            mg_send_http_ok(c, "text/plain; charset=utf-8", (long long)n);
            mg_write(c, buf, n);
        }
        pthread_mutex_unlock(&G.mu);
        return found ? 200 : send_err(c, 404, "no such part in the current session");
    }
    return send_err(c, 404, "unknown API call");
}

/* ---------------- main ---------------- */

static int web_log(const struct mg_connection *c, const char *msg)
{
    (void)c;
    fprintf(stderr, "[clientd] web: %s\n", msg);
    return 1;
}

/* ---------------- in-stream menu actions (main loop, lock held) ---------------- */

static void menu_quit(void)
{
    G.error[0] = 0;
    session_end("quit from the in-stream menu", 1);
}

/* the start placeholder waited long: restart the host program, as POST /api/host/restart does */
static void menu_restart_host(void)
{
    G.error[0] = 0;
    session_end("host program restarted from the in-stream menu", 0);
    if (G.connected) host_request("host.restart", NULL);
}

/* disconnect (the host suspends a gamescope app), then close it there: the control stream is
 * ordered, so app.quit arrives after session.stop */
static void menu_quit_app(void)
{
    char app[128];
    snprintf(app, sizeof(app), "%s", G.app);
    G.error[0] = 0;
    session_end("quit app from the in-stream menu", 1);
    if (app[0] && strncmp(app, "mirror:", 7) && G.connected) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "app", app);
        G.id_quit = host_request("app.quit", m);
    }
}

static void menu_monitor(int on)
{
    if (G.st != S_RUNNING) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddBoolToObject(m, "monitor", on);
    G.update_monitor = on;
    G.id_update = host_request("session.update", m);
}

static void menu_mic_mute(int on)
{
    mic_set_muted(on);
}

/* the menu's "Clipboard" entry: the same switch as the web page's (kept) */
/* "Switch to <App>": this session ends (the host keeps this app in the background), that app starts,
 * i.e. resumes; the control stream is ordered, so session.start arrives after session.stop */
static void menu_switch_app(const char *app)
{
    char to[128];
    snprintf(to, sizeof(to), "%s", app);
    fprintf(stderr, "[clientd] in-stream menu: switching from %s to %s\n", G.app, to);
    G.error[0] = 0;
    session_end("switched to another app from the in-stream menu", 1);
    G.error[0] = 0;
    session_start(to);
}

/* "Quit <App>": close that app in the host's background; this session goes on */
static void menu_quit_bg(const char *app)
{
    if (!G.connected) return;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "app", app);
    G.id_quit = host_request("app.quit", m);
}

static void menu_clipboard(int on)
{
    cJSON_ReplaceItemInObjectCaseSensitive(G.config, "clipboard", cJSON_CreateBool(on));
    config_save();
    G.clip_kick = 1;
    clip_sync();
}

static void menu_update(int cursor, const char *output)
{
    if (G.st != S_RUNNING) return;
    G.update_monitor = -1;
    cJSON *m = cJSON_CreateObject();
    if (cursor >= 0) cJSON_AddBoolToObject(m, "cursor", cursor);
    if (output) cJSON_AddStringToObject(m, "output", output);
    G.id_update = host_request("session.update", m);
}

static void handle_cmd(void)
{
    uint64_t v;
    if (read(G.efd, &v, sizeof(v)) < 0) return;
    int cmd = G.cmd;
    char app[128];
    snprintf(app, sizeof(app), "%s", G.cmd_app);
    int hidx = G.cmd_host;
    G.cmd = CMD_NONE;
    struct hostent hl[MAXH];
    int nh = hosts_list(hl), act = active_index();
    switch (cmd) {
    case CMD_START:
        if (hidx >= 0 && hidx < nh && hidx != act) {
            if (G.st != S_IDLE) { snprintf(G.error, sizeof(G.error), "a session is already %s", sstate_names[G.st]); break; }
            /* make that host the active one: full connection, then start once it said hello */
            fprintf(stderr, "[clientd] switching to host %d (%s:%d) to start %s\n", hidx, hl[hidx].addr, hl[hidx].port, app);
            cJSON_ReplaceItemInObjectCaseSensitive(G.config, "active_host", cJSON_CreateNumber(hidx));
            config_save();
            probe_close(hidx);
            host_drop(NULL);
            cJSON_Delete(G.host_info);
            G.host_info = NULL;
            cJSON_Delete(G.suspended);
            G.suspended = NULL;
            cJSON_Delete(G.suspended_list);
            G.suspended_list = NULL;
            G.suspend_multi = 0;
            G.next_connect_ns = 0;
            G.error[0] = 0;
            snprintf(G.pending_app, sizeof(G.pending_app), "%s", app[0] ? app : "");
            if (!app[0]) snprintf(G.pending_app, sizeof(G.pending_app), " ");
            break;
        }
        session_start(app);
        break;
    case CMD_STOP:      G.error[0] = 0; session_end("stopped", 1); break;
    case CMD_REFRESH:
        refresh_lists();
        if (G.connected) G.id_hello = host_request("hello", NULL);
        else G.next_connect_ns = 0;
        break;
    case CMD_RECONNECT:
        host_drop(NULL);
        G.next_connect_ns = 0;
        for (int i = 0; i < MAXH; i++) { probe_close(i); P[i].next_ns = 0; }
        break;
    case CMD_QUIT_APP: {
        G.error[0] = 0;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "app", app);
        if (hidx >= 0 && hidx < nh && hidx != act) {
            probe_request(hidx, "app.quit", m);
            P[hidx].hello_ns = now_ns() + 300000000ull;      /* its paused app: refresh soon */
        } else if (G.connected) G.id_quit = host_request("app.quit", m);
        else cJSON_Delete(m);
        break;
    }
    case CMD_RESTART_HOST:
        G.error[0] = 0;
        if (G.st != S_IDLE) session_end("host program restarted from this computer", 0);
        if (G.connected) host_request("host.restart", NULL);
        G.next_connect_ns = 0;            /* reconnect as soon as it listens again */
        break;
    }
}

int main(int argc, char **argv)
{
    G.config_path = "/etc/rdx/client.json";
    G.web_bind = "127.0.0.1";
    G.web_port = 47991;
    G.host_port = 47989;
    G.port_base = 18650;
    G.menu_font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char *menu_path = RDX_MENU_SOCK_PATH;
    static char repo[PATH_MAX], webroot[PATH_MAX + 16];
    /* default repo: the directory above our binary (client/rdx_clientd) */
    ssize_t rl = readlink("/proc/self/exe", repo, sizeof(repo) - 1);
    if (rl > 0) {
        repo[rl] = 0;
        for (int i = 0; i < 2; i++) { char *sl = strrchr(repo, '/'); if (sl) *sl = 0; }
    }
    G.repo = repo;
    static const struct option opts[] = {
        {"config", 1, 0, 'c'}, {"repo", 1, 0, 'r'}, {"web-root", 1, 0, 'w'}, {"web-port", 1, 0, 'W'},
        {"web-bind", 1, 0, 'b'}, {"host-port", 1, 0, 'H'}, {"data-port-base", 1, 0, 'D'},
        {"no-vt", 0, 0, 'n'}, {"menu-sock", 1, 0, 'S'}, {"menu-font", 1, 0, 'F'}, {"no-menu", 0, 0, 'N'},
        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'c': G.config_path = optarg; break;
        case 'r': G.repo = optarg; break;
        case 'w': G.web_root = optarg; break;
        case 'W': G.web_port = atoi(optarg); break;
        case 'b': G.web_bind = optarg; break;
        case 'H': G.host_port = atoi(optarg); break;
        case 'D': G.port_base = atoi(optarg); break;
        case 'n': G.no_vt = 1; break;
        case 'S': menu_path = optarg; break;
        case 'F': G.menu_font = optarg; break;
        case 'N': G.no_menu = 1; break;
        default:  fprintf(stderr, "see the header of rdx_clientd.c for options\n"); return 2;
        }
    }
    if (!G.web_root) { snprintf(webroot, sizeof(webroot), "%s/web/client", G.repo); G.web_root = webroot; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&G.mu, NULL);
    for (int i = 0; i < NPROC; i++) G.proc_status[i] = -1;

    config_load();
    /* the audio player talks to the desktop user's PipeWire: "audio_runtime_dir" of the config wins
     * over an inherited XDG_RUNTIME_DIR (started from a root login, /run/user/0 has no PipeWire and
     * the player failed, ending the session at once); without the setting the inherited one stays */
    if (*cfg_str("audio_runtime_dir")) setenv("XDG_RUNTIME_DIR", cfg_str("audio_runtime_dir"), 1);
    refresh_lists();

    G.efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event e = {.events = EPOLLIN, .data.u32 = 1};
    epoll_ctl(ep, EPOLL_CTL_ADD, G.efd, &e);
    e.data.u32 = 2;
    epoll_ctl(ep, EPOLL_CTL_ADD, rdx_proc_poll_fd(), &e);
    static const struct rdx_menu_actions menu_act = {menu_quit, menu_quit_app, menu_update, menu_monitor,
                                                     menu_restart_host, menu_mic_mute, menu_clipboard,
                                                     menu_switch_app, menu_quit_bg};
    G.update_monitor = -1;
    if (!G.no_menu && !rdx_menu_start(menu_path, G.menu_font, &menu_act)) {
        G.menu_sock = menu_path;
        e.data.u32 = 4;
        epoll_ctl(ep, EPOLL_CTL_ADD, rdx_menu_poll_fd(), &e);
    } else if (!G.no_menu) {
        fprintf(stderr, "[clientd] in-stream menu unavailable, sessions run without it\n");
    }
    snprintf(G.lease_state, sizeof(G.lease_state), "/run/rdx/lease-%d.state", G.web_port);
    if (!rdx_link_start(cfg_str("link_ip"), NULL)) {        /* link health, never fatal */
        e.data.u32 = 5;
        epoll_ctl(ep, EPOLL_CTL_ADD, rdx_link_event_fd(), &e);
    }

    char ports[96];
    snprintf(ports, sizeof(ports), "%s:%d", G.web_bind, G.web_port);
    const char *wopts[] = {"listening_ports", ports, "document_root", G.web_root, "num_threads", "4",
                           "enable_directory_listing", "no", NULL};
    mg_init_library(0);
    struct mg_callbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.log_message = web_log;
    struct mg_context *web = mg_start(&cbs, NULL, wopts);
    if (!web) { fprintf(stderr, "[clientd] cannot serve the web UI on %s\n", ports); return 1; }
    mg_set_request_handler(web, "/api/", h_api, NULL);
    struct hostent hl0[MAXH];
    int nh0 = hosts_list(hl0), a0 = active_index();
    fprintf(stderr, "[clientd] web UI on http://%s (files %s), host %s:%d (%d configured), data ports %d.., repo %s\n",
            ports, G.web_root, hl0[a0].addr, hl0[a0].port, nh0, G.port_base, G.repo);

    G.ctl_epfd = -1;
    while (!g_stop) {
        pthread_mutex_lock(&G.mu);
        if (!G.ctl && now_ns() >= G.next_connect_ns) host_connect();
        /* keep the epoll set in step with the control socket */
        int fd = G.ctl ? rdx_ctl_fd(G.ctl) : -1;
        if (fd != G.ctl_epfd) {
            if (G.ctl_epfd >= 0) epoll_ctl(ep, EPOLL_CTL_DEL, G.ctl_epfd, NULL);
            if (fd >= 0) { e.data.u32 = 3; epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e); }
            G.ctl_epfd = fd;
        }
        int t = session_tick();
        int tp = probe_tick();
        if (tp >= 0 && (t < 0 || tp < t)) t = tp;
        for (int i = 0; i < MAXH; i++) {          /* the other hosts' sockets in the epoll set */
            int pfd = P[i].ctl ? rdx_ctl_fd(P[i].ctl) : -1;
            if (pfd != P[i].epfd - 1) {
                if (P[i].epfd > 0) epoll_ctl(ep, EPOLL_CTL_DEL, P[i].epfd - 1, NULL);
                if (pfd >= 0) { struct epoll_event pe = {.events = EPOLLIN, .data.u32 = 100 + (uint32_t)i}; epoll_ctl(ep, EPOLL_CTL_ADD, pfd, &pe); }
                P[i].epfd = pfd + 1;              /* 0 = none */
            }
        }
        if (!G.ctl) {
            uint64_t now = now_ns();
            int tc = G.next_connect_ns > now ? (int)((G.next_connect_ns - now) / 1000000ull) + 1 : 0;
            if (t < 0 || tc < t) t = tc;
        }
        pthread_mutex_unlock(&G.mu);

        struct epoll_event ev[8];
        int n = epoll_wait(ep, ev, 8, t);
        if (n < 0 && errno != EINTR) { perror("epoll_wait"); break; }
        pthread_mutex_lock(&G.mu);
        for (int i = 0; i < n; i++) {
            if (ev[i].data.u32 == 1) handle_cmd();
            else if (ev[i].data.u32 == 2) rdx_proc_dispatch();
            else if (ev[i].data.u32 == 4) rdx_menu_dispatch();
            else if (ev[i].data.u32 == 5) rdx_link_take();     /* link_update below */
            else if (ev[i].data.u32 >= 100 && ev[i].data.u32 < 100 + MAXH) {
                int k = (int)ev[i].data.u32 - 100;
                cJSON *m;
                int r = 0;
                while (P[k].ctl && (r = rdx_ctl_recv(P[k].ctl, &m)) == 1) { probe_message(k, m); cJSON_Delete(m); }
                if (r < 0) {
                    probe_close(k);
                    snprintf(P[k].error, sizeof(P[k].error), "rdx-hostd closed the control connection");
                    P[k].next_ns = now_ns() + 5000000000ull;
                }
            }
            else if (ev[i].data.u32 == 3 && G.ctl) {
                cJSON *m;
                int r;
                while ((r = rdx_ctl_recv(G.ctl, &m)) == 1) { host_message(m); cJSON_Delete(m); }
                if (r < 0) {
                    host_drop("rdx-hostd closed the control connection");
                    cJSON_Delete(G.host_info);
                    G.host_info = NULL;
                    if (G.st != S_IDLE) session_end("control connection to the host lost", 0);
                }
            }
        }
        link_update();
        display_check();
        if (G.lease_retry && G.st == S_LISTEN) {
            G.lease_retry = 0;
            G.sink_only = 1;
            start_listeners();
        }
        if (G.st == S_IDLE) {
            G.lease_refused = 0;
            static uint64_t t_lease_chk;
            /* a lease that ended without its rdx_lease (killed): the monitor back from its state file */
            if (now_ns() > t_lease_chk && !access(G.lease_state, F_OK)) {
                char cmd[PATH_MAX + 200];
                snprintf(cmd, sizeof(cmd), "'%s/p0b/rdx_lease' --restore '%s' >&2", G.repo, G.lease_state);
                fprintf(stderr, "[clientd] a lease's monitor is still off: %s\n", cmd);
                if (system(cmd)) { /* reported by rdx_lease */ }
                t_lease_chk = now_ns() + 10000000000ull;
            }
        }
        /* free parts of a finished session that exited after it ended */
        if (G.st == S_IDLE)
            for (int i = 0; i < NPROC; i++)
                if (G.proc[i] && !rdx_proc_running(G.proc[i])) { rdx_proc_free(G.proc[i]); G.proc[i] = NULL; }
        pthread_mutex_unlock(&G.mu);
    }
    fprintf(stderr, "[clientd] stopping\n");
    pthread_mutex_lock(&G.mu);
    session_end("rdx-clientd stopped", 1);
    host_drop(NULL);
    pthread_mutex_unlock(&G.mu);
    mg_stop(web);
    return 0;
}
