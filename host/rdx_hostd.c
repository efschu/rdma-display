/* SPDX-License-Identifier: MIT
 *
 * rdx-hostd: control daemon on the sending host (Sunshine-like).
 *   - web UI (civetweb) on 127.0.0.1 and the link IP, port 47990
 *   - control channel (JSON lines, rdx_ctl) on the link IP only, port 47989
 *   - runs one session at a time for the client that started it: spawns and
 *     supervises p4/rdx_input_rx, p1/rdx_mirror_src, p5/rdx_audio_tx and,
 *     if built, usb/rdx_usb_host; any child ending ends the whole session
 * Session start (contract, client side first): the client already listens with
 * its sink and audio player, then sends session.start; we start rdx_input_rx,
 * wait until it listens ("waiting for source" / "listening"), then start the
 * senders (and rdx_usb_host, waiting for its "listening" line too) and reply
 * {ok, input_port, usb_port, mic_port}. The client then connects input_tx, usb_client and
 * its microphone sender.
 * Microphone (client -> host, on by default: "mic" in the client's session.start and in
 * "defaults"): p5/rdx_audio_rx --mic "<defaults.mic_name>" listens on "mic_port" (18690) and
 * is the virtual source apps record from, the default source while the session runs. It opens
 * the client's capture only while an app records (p5/rdx_audio_mic.h). Its ending does not end
 * the session (no microphone then).
 * Clipboard (both ways, on by default: "clipboard" in the client's session.start and in "defaults"):
 * clip/rdx_clip --listen on "clip_port" (18700, seat N +100*N), only for the
 * session's client, on the Xwayland below the app's rdx_gs_src (gamescope's; the seat's monitor
 * gamescope for a join, the desktop's display for a mirrored screen); reply "clip_port", the client
 * connects its rdx_clip from its desktop. Optional like the microphone: its ending ends nothing.
 * Event-driven: one epoll over signals, child output/exit, control sockets and
 * a start deadline; idle CPU is ~0.
 *
 *   rdx_hostd [--config /etc/rdx/hostd.json] [--repo DIR] [--web-root DIR]
 *             [--bind 10.0.0.2] [--ctl-port 47989] [--web-port 47990]
 *             [--dev auto] [--mtu 4096] [--seat N]
 *
 * Seats (several streamed desktops on one machine, e.g. one LXC per GPU sharing the host's
 * network namespace and RDMA NIC): --seat N (or config "seat") moves every default port by
 * 100*N (control 47989, web 47990, video 18650, input 18660, audio 18670, USB 18680, clipboard 18700) and
 * the default config to /etc/rdx/hostd-seatN.json. Config "gpu": PCI address of the GPU the
 * seat's gamescope apps run on, any vendor ("" = the first usable NVIDIA GPU, else the first
 * usable GPU; a non-NVIDIA GPU always uses host buffers); several seats may name the same
 * GPU (one gamescope each). On a multiseat host a seat starts on any free card (rdx-seatmgr writes "gpu"
 * before the start, docs/MULTISEAT.md "Card choice"): everything card-dependent follows "gpu" (the DRM
 * nodes by sysfs, the monitor on its connectors, gamescope's Vulkan device by UUID and ids).
 * "seat_name" ("" = "Seat N" for a seat, the GPU's short name, e.g. "5090", for a single host with
 * "gpu") shows as "RDMA Display - <name>" in hello.seat_name; hello.gpu_name is the card. "priority" ("high" | "normal" | "low") is the
 * seat's share when several seats use one GPU: rdx_gs_src --priority (gamescope's compositor
 * queue priority, nice and I/O priority of gamescope and the app) and the mirror source's GPU
 * context priority. "helper_gpu" (PCI, any vendor): frame generation on a second card, per app
 * option "framegen" (host/rdx_mgpu.h, docs/MULTI-GPU.md).
 * "seatmgr" (URL of rdx-seatmgr on the Proxmox host, seat/rdx_seatmgr.py) and "seatmgr_ui"
 * (a page where the seat's devices are assigned): the web UI lists the seat's local keyboards, mice and sound
 * outputs from <seatmgr>/api/seat/N and can ask for "the next key press is mine" (claim).
 *
 * Hook for per-seat additions to gamescope apps (frame generation, layers): seat_app_hook()
 * adds environment for the app; every app gets RDX_SEAT, RDX_GPU, RDX_HELPER_GPU and the
 * config object "seat_env" ({KEY: VALUE}, the app's own "env" wins). Config "pre_launch":
 * [argv...] runs before rdx_gs_src (same environment plus RDX_APP; at most 10 s, its failure
 * ends the start).
 * Command-line values override the config file; settings changed in the web
 * UI are written back to the config file.
 *
 * Apps (like Sunshine's apps.json): one built-in "mirror:<output>" app per
 * active output, plus the configured list "apps" in the config file:
 *   {"id":"game1","name":"My game","kind":"gamescope","cmd":["/usr/bin/wine","game.exe"],
 *    "env":{"DXVK_HUD":"fps"},"capture_output":""}
 * A gamescope app runs through gs/rdx_gs_src (the gamescope path, docs/GAMESCOPE.md): it connects to the
 * client's sink, owns the NIC-readable VRAM buffers and starts the patched gamescope
 * ("gamescope_bin", host/install-gamescope.sh) with the client's mode; gamescope and the
 * app run as the desktop user ("desktop_user", else the seat0 session's, else - no graphical
 * session, a seat container - the lingering user: rdx_desktop_user), Steam refuses root.
 * "gs_buffers" (default "auto") is rdx_gs_src's --buffers for every gamescope app: "host" pins
 * host buffers where the NIC must never peer-read the GPU's VRAM, whatever auto decides.
 * App fields: id, name, kind "gamescope", cmd [...], env {...}, gamescope_args [...]
 * (e.g. ["-e"] for Steam), icon (theme name or path), desktop_id, note, private_dbus.
 * The web UI finds installed programs (/api/apps/detect, presets for Steam & co.).
 *
 * Own session bus ("private_dbus", default on, per app or in "defaults"; app_bus_wrap): the
 * app runs under dbus-run-session, so a single-instance app (GApplication, Qt) does not find
 * its instance on the desktop and hand the start over (Lutris: "launch: Primary child
 * shut down!"), and a later start on the desktop does not open in the stream. On that bus
 * D-Bus services start on demand (dconf, portals: their dialogs then show in the stream); the
 * desktop's notification daemon and its unlocked keyring (org.freedesktop.secrets) are not
 * there - an app that keeps logins in the keyring (Electron safeStorage, e.g. Heroic) may ask
 * again or not find them: "private_dbus": false for it. A flatpak keeps the user's bus.
 * An app that still ends within APP_EARLY_END_S of the start gets a notice for the client
 * (app_ended_check): "<App> is already open on this PC - close it there and start again" when
 * a process of it runs on the desktop (outside rdx-hostd's cgroup), shown on the start
 * placeholder.
 *
 * Suspend / resume (Moonlight semantics): when a gamescope app's session ends because the
 * client stopped or went away, gs/rdx_gs_src is kept: it detaches from the sink (SIGUSR2)
 * and the app keeps running. session.start for that app resumes it (the new peer in
 * <socket>.peer, SIGUSR1), also at another client mode (rdx_gs_src re-creates its buffers and
 * gamescope its output); another tearing setting quits that one app and starts it fresh.
 * "tearing" in session.start (the client shows frames on arrival) starts rdx_gs_src --tearing.
 * Several apps in the background ("suspend_multi" in "defaults", on by default): switching to
 * another app keeps the previous one running (a Steam download goes on while Battle.net is
 * streamed), at most APPS_MAX apps on the seat at once, the one in the stream included; a start
 * beyond that is refused with the list (nothing is ever closed to make room). Each app has its
 * own rdx_gs_src socket (/run/rdx-gs-<seat>-<app>.sock) and gamescope; a detached gamescope lets
 * go of the client's input devices (RDX_GS_INPUT 0 from rdx_gs_src: libinput suspended, grabs
 * released), so the next app's gamescope gets them alone. A gamescope without that message (an
 * older /opt/rdx-gs: no marker string in the binary, gs_input_capable) keeps the old behaviour,
 * one app in the background. The same app twice is a resume, never a second start, also for an
 * app that runs the same single-instance program (Steam: "steam" as the program, or the app
 * field "single_instance": true) under another entry: that start resumes the running one
 * ("resumed_as" in the reply). hello, status and /api/state carry "suspended_apps"
 * [{app, name, mode, since_s, tearing, vram_mib}] (oldest first) and, for older clients,
 * "suspended" = the newest of them. Starting one more app while others run checks the free
 * memory of the seat's GPU (nvidia-smi, "nvidia_smi" in the config for tests): below
 * "suspend_vram_warn_mib" (defaults, 2048) the reply carries "vram_warning" {free_mib,
 * threshold_mib, apps: [{app, name, vram_mib}]} and the client shows it; the start goes on, nothing
 * is closed. An app's memory: every GPU process below its rdx_gs_src or with its RDX_GS_SOCKET.
 * Closing is explicit only: app.quit {app}, POST /api/apps/quit {app} (no app: the newest), the
 * host web UI's Stop (a running gamescope session: disconnect = suspend; idle: the newest one);
 * rdx-hostd's shutdown closes them all. An app that ends by itself in the background is dropped
 * from the list (session_tick).
 *
 * Variable refresh (FreeSync / VRR, host/rdx_vrr.h, on by default, "vrr" in "defaults"): the client
 * reports its screen in session.start as "display" {vrr, min_hz, max_hz, hz}. For a gamescope app with
 * vrr (not tearing) rdx_gs_src gets --vrr <cap>: gamescope runs at the app's pace up to the client's
 * maximum times "vrr_cap_frac" (0.97) instead of the client's vblank (gs/rdx_gs_vrr.h); a resume passes it as the third field of
 * <socket>.peer, a later session.update {"display"} (fullscreen off, another monitor on the client) goes
 * to the running rdx_gs_src by sigqueue(SIGRTMIN, cap mHz, 0 = fixed). Without "display" or with vrr
 * false nothing changes. /api/state session.vrr: {active, cap_hz, why, client, source} (source: the
 * last "[gs] vrr:" line of rdx_gs_src, what really runs); the session.start reply carries "vrr" too.
 * A join onto the seat's monitor gets it only when that monitor is vrr_capable (read at the join): the
 * seat's gamescope runs --adaptive-sync ("monitor_vrr", default on) and its monitor paces the app.
 *
 * Link health (common/rdx_link.h): the RDMA port carrying link_ip and its network module's
 * temperature, sampled every 2 s by a thread, are "link" in hello and /api/state; every change of
 * level (ok / warn: the module or the card's chip passed its warning threshold / alarm / down, with the
 * reason / failed: the card itself is gone, power-cycle needed) goes to
 * the connected clients as the event link.health {link}, so a client warns before the module's own
 * threshold takes the link down and says why it went down.
 */
#define _GNU_SOURCE
#include "../common/rdx_ctl.h"
#include "../common/rdx_link.h"
#include "../common/rdx_proc.h"
#include "../third_party/civetweb/civetweb.h"
#include "rdx_apps.h"
#include "rdx_mgpu.h"
#include "rdx_seatguard.h"
#include "rdx_vrr.h"

#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>

#define RDX_EXIT_RESTART 75            /* systemd restarts the unit (Restart=on-failure) */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS   8
#define MAX_PROCS     8
#define MAX_OUTPUTS   8
#define START_TIMEOUT_S 8.0
#define STOP_GRACE_MS 2000
#define RETRY_LISTEN_S 5.0
#define APPS_MAX      4                 /* gamescope apps of a seat at once: the stream's and the background ones */
#define VRAM_WARN_MIB 2048              /* "suspend_vram_warn_mib": free GPU memory below this warns */

enum sess_state { S_IDLE, S_STARTING, S_RUNNING, S_STOPPING };
static const char *state_name[] = {"idle", "starting", "running", "stopping"};

struct output { char name[32]; int w, h, hz, x, y; double hz_exact; };   /* hz_exact: from the timings */

struct session {
    enum sess_state state;
    struct rdx_ctl *owner;              /* client connection that started it */
    double reply_id;                    /* id of the pending session.start */
    double t_start, deadline;
    cJSON *params;                      /* effective parameters (for status) */
    char app[64], output[32], client_ip[64];
    cJSON *app_cfg;                     /* configured app (gamescope), NULL for mirror apps */
    int video_port, input_port, audio_port, usb_port, mic_port, clip_port;
    struct rdx_proc *procs[MAX_PROCS];
    int nprocs;
    struct rdx_proc *input_rx, *usb_host, *mic_rx, *clip;
    int senders_started;                /* phase 2 of the start: waiting for usb_host */
    int resume;                         /* session.start of an app in the background: re-attach its rdx_gs_src */
    char asked_app[64];                 /* the app the client asked for, when a resume of the same single-instance
                                         * program under another entry took its place ("resumed_as") */
    cJSON *vram_warn;                   /* one more app started with little free GPU memory: for the reply */
    char gs_sock[96];                   /* rdx_gs_src's socket (its .peer file is next to it) */
    int local_join;                     /* the client joined the app on the seat's own monitor */
    int input_private;                  /* the client's input for the app's gamescope only */
    int input_wait_grab;                /* a gamescope reads it and grabs it (rdx_input_rx --wait-grab) */
    int local_window;                   /* "Start here on this PC": a window on this computer's desktop */
    struct rdx_vrr vrr;                 /* variable refresh: the client's "display" and what the source does */
    /* a child ended: handled after dispatch, never inside the exit callback */
    int pending_end;
    char end_reason[128], end_proc[32];
    int end_status;
    /* the app (gamescope) ended by itself (rdx_gs_src "stopping: gamescope ended"): never a
     * suspend, never "stopped by the client"; right after the start the client shows app_reason
     * as a notice until notice_end (see app_ended) */
    int app_ended;
    char app_reason[256];
    double notice_end;
};

static struct {
    int restart;                        /* host.restart from a client: exit RDX_EXIT_RESTART */
    pthread_mutex_t mu;                 /* web threads vs. main loop */
    char cfg_path[PATH_MAX], repo[2048], web_root[PATH_MAX];   /* repo + relative binary path fits PATH_MAX */
    cJSON *cfg;
    int ep, sfd, lfd;
    double next_listen;
    struct rdx_ctl *clients[MAX_CLIENTS];
    struct session S;
    struct rdx_proc *last[MAX_PROCS];   /* children of the previous session, for logs */
    int nlast;
    char last_end[256];
    struct output outs[MAX_OUTPUTS];
    int nouts;
    double outs_time, t0;
    char host_name[64];
    int seat;
    char gpu_pci[32], gpu_name[96];     /* resolved "gpu" (normalized 0000:0d:00.0) and its name */
    struct { char pci[32], name[96], vendor[16]; int usable; } gpus[8];   /* every GPU, for the web UI's choice */
    int ngpus;
    struct mg_context *web;
    struct rdx_proc *installer;         /* "Install Steam" from the web UI */
    struct susp_app {                   /* gamescope apps kept running without a client, oldest first */
        struct rdx_proc *gs;            /* its rdx_gs_src, detached from any sink */
        char app[64], sock[96];
        cJSON *mode;                    /* the client mode it runs with {w,h,hz} */
        int tearing;
        double since;
        int vram_mib;                   /* its GPU memory at the last measurement, -1 = not measured */
    } susp[APPS_MAX];
    int nsusp;
    int gs_input;                       /* the installed gamescope lets go of the input when detached: 1 / 0, -1 = not checked */
    struct {                            /* the seat's own monitor: the home app in gamescope (DRM) */
        struct rdx_proc *gs;            /* runuser ... gamescope --backend drm -O <conn> -- <app> */
        char conn[32], monitor[16], app[64], sock[64];
        int connected, fails, asleep;
        int vrr;                        /* its vrr_capable at the last join: 1 / 0 / -1 unknown */
        int guard_busy, guard_logged;   /* a desktop / another DRM master owns the card: stream only */
        double next_check, retry_at, started, guard_next;
        char why[160];                  /* why it does not run (state for the web UI) */
    } local;
} G;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---------------- config ---------------- */

static const char *cfg_str(const char *k, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(G.cfg, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static int cfg_int(const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(G.cfg, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static cJSON *cfg_defaults(void) { return cJSON_GetObjectItem(G.cfg, "defaults"); }

static void cfg_set(cJSON *obj, const char *k, cJSON *v)
{
    if (cJSON_GetObjectItem(obj, k)) cJSON_ReplaceItemInObject(obj, k, v);
    else cJSON_AddItemToObject(obj, k, v);
}

static void cfg_fill_defaults(void)
{
    char hn[64] = "host";
    gethostname(hn, sizeof(hn));
    const int o = 100 * G.seat;                  /* seat N: every port +100*N */
    const struct { const char *k; const char *s; int n; } top[] = {
        {"name", hn, 0}, {"link_ip", "10.0.0.2", 0},
        {"rdma_dev", "auto", 0},     /* auto: the RDMA device whose GID table holds link_ip (common/rdx_common.h) */
        {"card", "/dev/dri/card1", 0}, {"render", "/dev/dri/renderD128", 0},
        {"log_dir", "/var/log/rdx", 0}, {"pipewire_runtime_dir", "", 0},
        {"mtu", NULL, 4096}, {"ctl_port", NULL, RDX_CTL_PORT + o}, {"web_port", NULL, 47990 + o},
        {"video_port", NULL, 18650 + o}, {"input_port", NULL, 18660 + o}, {"audio_port", NULL, 18670 + o},
        {"usb_port", NULL, 18680 + o}, {"mic_port", NULL, 18690 + o}, {"clip_port", NULL, 18700 + o}, {"gpu", "", 0}, {"seat_name", "", 0}, {"helper_gpu", "", 0},
        {"priority", "normal", 0}, {"seat", NULL, G.seat}, {"seatmgr", "", 0}, {"seatmgr_ui", "", 0},
        /* the seat's own monitor (docs/SEAT-MONITOR.md): "monitor+stream" | "monitor" | "stream" */
        {"picture", "monitor+stream", 0}, {"monitor_connector", "", 0}, {"home_app", "steam", 0},
        {"stream_via", "gamescope", 0}, {"local_libseat", "noop", 0}};
    for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++)
        if (!cJSON_GetObjectItem(G.cfg, top[i].k))
            cJSON_AddItemToObject(G.cfg, top[i].k,
                                  top[i].s ? cJSON_CreateString(top[i].s) : cJSON_CreateNumber(top[i].n));
    if (!cJSON_IsArray(cJSON_GetObjectItem(G.cfg, "apps"))) {
        cJSON_DeleteItemFromObject(G.cfg, "apps");
        cJSON_AddArrayToObject(G.cfg, "apps");
    }
    if (!cJSON_GetObjectItem(G.cfg, "gamescope_bin")) cJSON_AddStringToObject(G.cfg, "gamescope_bin", "/opt/rdx-gs/bin/gamescope");
    if (!cJSON_GetObjectItem(G.cfg, "gs_src_bin")) cJSON_AddStringToObject(G.cfg, "gs_src_bin", "");   /* "" = <repo>/gs/rdx_gs_src */
    /* rdx_gs_src --buffers: "auto" (its PCIe rule, docs/GAMESCOPE.md), "host" pins host buffers (e.g. a GeForce
     * behind the chipset whose VRAM the NIC must never peer-read), "vram" only where that rule allows it */
    if (!cJSON_GetObjectItem(G.cfg, "gs_buffers")) cJSON_AddStringToObject(G.cfg, "gs_buffers", "auto");
    if (!cJSON_GetObjectItem(G.cfg, "desktop_user")) cJSON_AddStringToObject(G.cfg, "desktop_user", "");  /* "" = seat0 session, else the lingering user */
    if (!cJSON_IsObject(cJSON_GetObjectItem(G.cfg, "seat_env"))) {
        cJSON_DeleteItemFromObject(G.cfg, "seat_env");
        cJSON_AddObjectToObject(G.cfg, "seat_env");
    }
    if (!cJSON_IsArray(cJSON_GetObjectItem(G.cfg, "pre_launch"))) {
        cJSON_DeleteItemFromObject(G.cfg, "pre_launch");
        cJSON_AddArrayToObject(G.cfg, "pre_launch");
    }
    cJSON *d = cfg_defaults();
    if (!cJSON_IsObject(d)) {
        if (d) cJSON_DeleteItemFromObject(G.cfg, "defaults");
        d = cJSON_AddObjectToObject(G.cfg, "defaults");
    }
    if (!cJSON_GetObjectItem(d, "output")) cJSON_AddStringToObject(d, "output", "");
    if (!cJSON_GetObjectItem(d, "fps")) cJSON_AddNumberToObject(d, "fps", 0);
    if (!cJSON_GetObjectItem(d, "cursor")) cJSON_AddBoolToObject(d, "cursor", 1);
    if (!cJSON_GetObjectItem(d, "gpu_prio")) cJSON_AddStringToObject(d, "gpu_prio", "high");
    if (!cJSON_GetObjectItem(d, "audio")) cJSON_AddBoolToObject(d, "audio", 1);
    if (!cJSON_GetObjectItem(d, "audio_source")) cJSON_AddStringToObject(d, "audio_source", "");
    if (!cJSON_GetObjectItem(d, "input")) cJSON_AddBoolToObject(d, "input", 1);
    if (!cJSON_GetObjectItem(d, "usb")) cJSON_AddBoolToObject(d, "usb", 1);
    if (!cJSON_GetObjectItem(d, "mic")) cJSON_AddBoolToObject(d, "mic", 1);
    if (!cJSON_GetObjectItem(d, "mic_name")) cJSON_AddStringToObject(d, "mic_name", "RDMA Display Mic");
    /* the clipboard between the client's desktop and the app, both ways (clip/rdx_clip) */
    if (!cJSON_GetObjectItem(d, "clipboard")) cJSON_AddBoolToObject(d, "clipboard", 1);
    /* the client sizes its sink's slots from the refresh and this seat's measured lead (hello) */
    if (!cJSON_GetObjectItem(d, "sink_slots_auto")) cJSON_AddBoolToObject(d, "sink_slots_auto", 1);
    /* gamescope apps on their own session D-Bus (app_bus_wrap) */
    if (!cJSON_GetObjectItem(d, "private_dbus")) cJSON_AddBoolToObject(d, "private_dbus", 1);
    /* several apps in the background (switching keeps the previous one running), up to APPS_MAX */
    if (!cJSON_GetObjectItem(d, "suspend_multi")) cJSON_AddBoolToObject(d, "suspend_multi", 1);
    /* one more app while others run: warn below this much free memory on the seat's GPU */
    if (!cJSON_GetObjectItem(d, "suspend_vram_warn_mib")) cJSON_AddNumberToObject(d, "suspend_vram_warn_mib", VRAM_WARN_MIB);
    /* variable refresh (FreeSync / VRR) where the client's screen has it (host/rdx_vrr.h) */
    if (!cJSON_GetObjectItem(d, "vrr")) cJSON_AddBoolToObject(d, "vrr", 1);
    /* its cap as a fraction of the client's maximum: a little below it stays inside the VRR range */
    if (!cJSON_GetObjectItem(d, "vrr_cap_frac")) cJSON_AddNumberToObject(d, "vrr_cap_frac", RDX_VRR_CAP_FRAC_DEFAULT);
}

static int jbool(cJSON *o, const char *k, int def);

/* what a session is for variable refresh (host/rdx_vrr.h) */
static enum rdx_vrr_kind session_vrr_kind(const struct session *s)
{
    return s->local_window ? RDX_VRR_LOCAL_WINDOW
         : s->local_join ? (G.local.vrr == 1 && jbool(G.cfg, "monitor_vrr", 1) ? RDX_VRR_JOIN_VRR : RDX_VRR_JOIN)
         : s->app_cfg ? RDX_VRR_GAMESCOPE : RDX_VRR_MIRROR;
}

/* the source runs at the app's pace (rdx_gs_src --vrr, SIGRTMIN on a change): a gamescope app of its own,
 * or the app on the seat's monitor when that monitor has variable refresh */
static int vrr_kind_paced(enum rdx_vrr_kind k) { return k == RDX_VRR_GAMESCOPE || k == RDX_VRR_JOIN_VRR; }

static void local_card(char *card, size_t n);

/* the seat monitor's vrr_capable, read while its gamescope holds the card (the join): the open never
 * makes us DRM master then. -1 = cannot tell */
static int local_monitor_vrr(void)
{
    if (!G.local.gs || !rdx_proc_running(G.local.gs) || rdx_proc_uptime_s(G.local.gs) < 2 || !G.local.conn[0])
        return -1;
    char card[300];
    local_card(card, sizeof(card));
    return rdx_vrr_monitor_capable(card, G.local.conn);
}

/* rdx_gs_src --lead-file of this seat: its settled lead of the last stream, for hello */
static void lead_path(char *out, size_t n)
{
    snprintf(out, n, "/run/rdx-gs-lead-seat%d", G.seat);
}

static void cfg_load(void)
{
    FILE *f = fopen(G.cfg_path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *b = malloc((size_t)n + 1);
        size_t got = fread(b, 1, (size_t)n, f);
        b[got] = 0;
        fclose(f);
        G.cfg = cJSON_Parse(b);
        free(b);
        if (!cJSON_IsObject(G.cfg)) {
            fprintf(stderr, "[hostd] %s is not a JSON object, using defaults\n", G.cfg_path);
            cJSON_Delete(G.cfg);
            G.cfg = NULL;
        }
    }
    if (!G.cfg) G.cfg = cJSON_CreateObject();
    if (G.seat < 0) {                           /* no --seat: the config's, else seat 0 */
        cJSON *se = cJSON_GetObjectItem(G.cfg, "seat");
        G.seat = cJSON_IsNumber(se) && se->valueint >= 0 && se->valueint < 100 ? se->valueint : 0;
    }
    cfg_fill_defaults();
}

static int cfg_save(void)
{
    char dir[PATH_MAX], tmp[PATH_MAX + 8];
    snprintf(dir, sizeof(dir), "%s", G.cfg_path);
    mkdir(dirname(dir), 0755);
    snprintf(tmp, sizeof(tmp), "%s.tmp", G.cfg_path);
    char *s = cJSON_Print(G.cfg);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(s); return -1; }
    fprintf(f, "%s\n", s);
    free(s);
    if (fclose(f) || rename(tmp, G.cfg_path)) return -1;
    rdx_link_target(cfg_str("link_ip", ""));            /* the link IP may have changed */
    return 0;
}

/* ---------------- environment probes ---------------- */

/* outputs of this host via rdx_mirror_src --list-json, fallback: its "[cap] output" lines */
static void refresh_outputs(int force)
{
    if (!force && G.nouts && now_s() - G.outs_time < 5) return;
    char cmd[PATH_MAX * 2];
    struct output o[MAX_OUTPUTS];
    int n = 0;
    for (int pass = 0; pass < 2 && !n; pass++) {
        snprintf(cmd, sizeof(cmd), "'%s/p1/rdx_mirror_src' %s --card '%s' 2>/dev/null", G.repo,
                 pass ? "--list" : "--list-json", cfg_str("card", "/dev/dri/card1"));
        FILE *p = popen(cmd, "r");
        if (!p) continue;
        char *all = NULL;
        size_t cap = 0, len = 0;
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            size_t l = strlen(line);
            if (len + l + 1 > cap) { cap = (len + l + 1) * 2; all = realloc(all, cap); }
            memcpy(all + len, line, l + 1);
            len += l;
            if (pass && n < MAX_OUTPUTS) {
                struct output t = {0};
                int idx;
                if (sscanf(line, "[cap] output %d: %31s %dx%d@%d at +%d+%d", &idx, t.name, &t.w, &t.h, &t.hz, &t.x,
                           &t.y) == 7)
                    o[n++] = t;
            }
        }
        pclose(p);
        if (!pass && all) {
            const char *js = strchr(all, '{');
            cJSON *j = js ? cJSON_Parse(js) : NULL;
            cJSON *arr = cJSON_GetObjectItem(j, "outputs"), *it;
            cJSON_ArrayForEach(it, arr) {
                if (n == MAX_OUTPUTS) break;
                struct output t = {0};
                cJSON *nm = cJSON_GetObjectItem(it, "name");
                if (!cJSON_IsString(nm)) continue;
                snprintf(t.name, sizeof(t.name), "%s", nm->valuestring);
                t.w = cJSON_GetObjectItem(it, "w") ? cJSON_GetObjectItem(it, "w")->valueint : 0;
                t.h = cJSON_GetObjectItem(it, "h") ? cJSON_GetObjectItem(it, "h")->valueint : 0;
                t.hz = cJSON_GetObjectItem(it, "hz") ? cJSON_GetObjectItem(it, "hz")->valueint : 0;
                t.hz_exact = cJSON_IsNumber(cJSON_GetObjectItem(it, "hz_exact"))
                             ? cJSON_GetObjectItem(it, "hz_exact")->valuedouble : t.hz;
                t.x = cJSON_GetObjectItem(it, "x") ? cJSON_GetObjectItem(it, "x")->valueint : 0;
                t.y = cJSON_GetObjectItem(it, "y") ? cJSON_GetObjectItem(it, "y")->valueint : 0;
                o[n++] = t;
            }
            cJSON_Delete(j);
        }
        free(all);
    }
    memcpy(G.outs, o, sizeof(o));
    G.nouts = n;
    G.outs_time = now_s();
}

/* PipeWire of the desktop user: config value, else the desktop user's (rdx_desktop_user: the session's,
 * or a seat container's lingering user manager), else the first /run/user/<uid>/pipewire-0 with uid >= 1000 */
static void setup_pipewire_env(void)
{
    const char *d = cfg_str("pipewire_runtime_dir", "");
    char found[PATH_MAX] = "";
    struct rdx_desktop_user du;
    if (*d) snprintf(found, sizeof(found), "%s", d);
    else if (!rdx_desktop_user(cfg_str("desktop_user", ""), &du)) {
        char sock[PATH_MAX];
        snprintf(sock, sizeof(sock), "/run/user/%u/pipewire-0", (unsigned)du.uid);
        if (!access(sock, F_OK)) snprintf(found, sizeof(found), "/run/user/%u", (unsigned)du.uid);
    }
    if (!*found && !*d) {
        DIR *ru = opendir("/run/user");
        struct dirent *e;
        while (ru && (e = readdir(ru))) {
            if (atoi(e->d_name) < 1000) continue;
            char sock[PATH_MAX];
            snprintf(sock, sizeof(sock), "/run/user/%s/pipewire-0", e->d_name);
            if (!access(sock, F_OK)) { snprintf(found, sizeof(found), "/run/user/%s", e->d_name); break; }
        }
        if (ru) closedir(ru);
    }
    if (*found) {
        setenv("PIPEWIRE_RUNTIME_DIR", found, 1);
        fprintf(stderr, "[hostd] audio: PipeWire of %s\n", found);
    } else fprintf(stderr, "[hostd] audio: no desktop PipeWire found, audio_tx may fail\n");
}

/* name of the default sink (its monitor is captured), "" if unknown */
static void default_sink(char *out, size_t n)
{
    out[0] = 0;
    FILE *p = popen("pw-metadata -n default 0 default.audio.sink 2>/dev/null", "r");
    if (!p) return;
    char line[1024];
    while (fgets(line, sizeof(line), p)) {
        char *s = strstr(line, "{\"name\":\"");
        if (!s) continue;
        s += 9;
        char *e = strchr(s, '"');
        if (e) snprintf(out, n, "%.*s", (int)(e - s), s);
    }
    pclose(p);
}

/* ---------------- session ---------------- */

static void ctl_reply(struct rdx_ctl *c, double id, int ok, const char *err, cJSON *extra)
{
    cJSON *r = extra ? extra : cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", ok);
    if (err) cJSON_AddStringToObject(r, "error", err);
    if (c) rdx_ctl_send(c, r);
    cJSON_Delete(r);
}

/* ---------------- apps in the background (suspended) ---------------- */

static cJSON *find_app(const char *id);
static const char *jstr(cJSON *o, const char *k, const char *def);
static double jnum(cJSON *o, const char *k, double def);

static cJSON *suspended_list_json(void);

static int susp_find(const char *app)
{
    for (int i = 0; i < G.nsusp; i++)
        if (!strcmp(G.susp[i].app, app)) return i;
    return -1;
}

static int susp_owns(const struct rdx_proc *p)
{
    for (int i = 0; i < G.nsusp; i++)
        if (G.susp[i].gs == p) return 1;
    return 0;
}

/* forget entry i: its rdx_gs_src belongs to the caller now (resumed) or is gone */
static void susp_remove(int i)
{
    cJSON_Delete(G.susp[i].mode);
    memmove(&G.susp[i], &G.susp[i + 1], (size_t)(G.nsusp - i - 1) * sizeof(G.susp[0]));
    G.nsusp--;
    memset(&G.susp[G.nsusp], 0, sizeof(G.susp[0]));
}

static void quit_suspended_at(int i, const char *why)
{
    fprintf(stderr, "[hostd] quitting the suspended app %s (%s)\n", G.susp[i].app, why);
    rdx_proc_stop(G.susp[i].gs, STOP_GRACE_MS);
    rdx_proc_free(G.susp[i].gs);
    susp_remove(i);
}

/* every app in the background: SIGTERM to all first, so their grace periods run in parallel */
static void quit_suspended_all(const char *why)
{
    for (int i = 0; i < G.nsusp; i++)
        if (rdx_proc_running(G.susp[i].gs)) kill(rdx_proc_pid(G.susp[i].gs), SIGTERM);
    while (G.nsusp) quit_suspended_at(G.nsusp - 1, why);
}

/* the newest app in the background, -1 = none (the one "suspended" names for older clients) */
static int susp_newest(void) { return G.nsusp - 1; }

/* The installed gamescope lets go of the client's input devices while its stream is detached
 * (RDX_GS_INPUT, gs/rdx_gs_proto.h): its binary carries this marker. Without it two gamescopes would
 * both grab the next session's devices, so only one app may wait in the background. Read again when
 * the binary changes. */
#define GS_INPUT_MARKER "RDX_GS_INPUT v1"
static int gs_input_capable(void)
{
    static dev_t dev;
    static ino_t ino;
    static off_t size;
    static time_t mtime;
    char gs[PATH_MAX];
    snprintf(gs, sizeof(gs), "%s", cfg_str("gamescope_bin", "/opt/rdx-gs/bin/gamescope"));
    if (access(gs, X_OK)) snprintf(gs, sizeof(gs), "/opt/rdx-gs/bin/gamescope");
    struct stat st;
    if (stat(gs, &st)) return G.gs_input = 0;
    if (G.gs_input >= 0 && st.st_dev == dev && st.st_ino == ino && st.st_size == size && st.st_mtime == mtime)
        return G.gs_input;
    dev = st.st_dev; ino = st.st_ino; size = st.st_size; mtime = st.st_mtime;
    FILE *f = fopen(gs, "rb");
    int found = 0;
    static char buf[1 << 16];
    size_t keep = 0, n, ml = strlen(GS_INPUT_MARKER);
    while (f && !found && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        n += keep;
        found = memmem(buf, n, GS_INPUT_MARKER, ml) != NULL;
        keep = n < ml ? n : ml - 1;
        memmove(buf, buf + n - keep, keep);
    }
    if (f) fclose(f);
    fprintf(stderr, "[hostd] %s %s: %s\n", gs, found ? "lets go of the input in the background" : "is older (no " GS_INPUT_MARKER ")",
            found ? "several apps may run in the background" : "one app at a time in the background");
    return G.gs_input = found;
}

/* several apps in the background: the setting, and a gamescope that can */
static int multi_on(void) { return jbool(cfg_defaults(), "suspend_multi", 1) && gs_input_capable(); }

/* the program of a single-instance app ("steam", or any with "single_instance": true): a second
 * entry running it only hands over to the running instance. "" = not single-instance */
static void single_prog(cJSON *app_cfg, char *out, size_t n)
{
    out[0] = 0;
    if (!app_cfg) return;
    rdx_app_program(cJSON_GetObjectItem(app_cfg, "cmd"), out, n);
    if (strcmp(out, "steam") && !jbool(app_cfg, "single_instance", 0)) out[0] = 0;
}

/* the app in the background that is the same single-instance program as app_cfg, -1 = none */
static int susp_same_instance(cJSON *app_cfg)
{
    char want[256], have[256];
    single_prog(app_cfg, want, sizeof(want));
    for (int i = 0; *want && i < G.nsusp; i++) {
        single_prog(find_app(G.susp[i].app), have, sizeof(have));
        if (!strcmp(want, have)) return i;
    }
    return -1;
}

/* rdx_gs_src's socket of an app: one per app, the app id made safe for a path */
static void gs_sock_path(const char *app, char *out, size_t n)
{
    char id[64];
    size_t k = 0;
    for (const char *c = app; *c && k < sizeof(id) - 1; c++)
        id[k++] = isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_' ? *c : '_';
    id[k] = 0;
    snprintf(out, n, "/run/rdx-gs-%d-%s.sock", G.seat, id);
    for (int i = 0, d = 2; i < G.nsusp; i++)           /* two ids that read the same: never one socket */
        if (!strcmp(G.susp[i].sock, out) && strcmp(G.susp[i].app, app)) {
            snprintf(out, n, "/run/rdx-gs-%d-%s-%d.sock", G.seat, id, d++);
            i = -1;
        }
}

/* ---- the seat GPU's memory (nvidia-smi; config "nvidia_smi" for tests) ---- */
#define GPU_PROCS_MAX 128
struct gpu_mem { int free_mib, total_mib, nproc; int pid[GPU_PROCS_MAX], mib[GPU_PROCS_MAX]; };

/* 0 = measured: the free and total MiB of the seat's GPU and the memory of every process on it (the
 * per-process view lists graphics processes too; in a container the driver gives the container's pids) */
static int gpu_mem_measure(struct gpu_mem *g)
{
    char cmd[PATH_MAX + 160], id[48] = "", line[512];
    memset(g, 0, sizeof(*g));
    g->free_mib = -1;
    if (G.gpu_pci[0]) snprintf(id, sizeof(id), " -i %s", G.gpu_pci);
    snprintf(cmd, sizeof(cmd), "timeout 5 %s --query-gpu=memory.free,memory.total --format=csv,noheader,nounits%s 2>/dev/null",
             cfg_str("nvidia_smi", "nvidia-smi"), id);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(line, sizeof(line), p) || sscanf(line, "%d , %d", &g->free_mib, &g->total_mib) != 2) g->free_mib = -1;
    pclose(p);
    if (g->free_mib < 0) return -1;
    snprintf(cmd, sizeof(cmd), "timeout 5 %s -q -d PIDS%s 2>/dev/null", cfg_str("nvidia_smi", "nvidia-smi"), id);
    if (!(p = popen(cmd, "r"))) return 0;
    int pid = 0, v;
    while (fgets(line, sizeof(line), p)) {
        const char *c = strchr(line, ':');
        if (!c) continue;
        if (strstr(line, "Process ID") && sscanf(c + 1, "%d", &v) == 1) pid = v;
        else if (strstr(line, "Used GPU Memory") && pid > 0 && sscanf(c + 1, "%d", &v) == 1 && g->nproc < GPU_PROCS_MAX) {
            g->pid[g->nproc] = pid;
            g->mib[g->nproc++] = v;
            pid = 0;
        }
    }
    pclose(p);
    return 0;
}

/* pid is part of the app whose rdx_gs_src is gs_pid: below it, or (re-parented, e.g. a game Steam
 * started) carrying its RDX_GS_SOCKET */
static int pid_in_app(int pid, pid_t gs_pid, const char *sock)
{
    char path[64], buf[512];
    for (int cur = pid, hops = 0; cur > 1 && hops < 64; hops++) {
        if (cur == gs_pid) return 1;
        snprintf(path, sizeof(path), "/proc/%d/stat", cur);
        FILE *f = fopen(path, "r");
        if (!f) break;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *rp = strrchr(buf, ')');
        int ppid = 0;
        if (!rp || sscanf(rp + 1, " %*c %d", &ppid) != 1) break;
        cur = ppid;
    }
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char env[65536];
    size_t n = fread(env, 1, sizeof(env) - 1, f);
    fclose(f);
    env[n] = 0;
    char want[128];
    snprintf(want, sizeof(want), "RDX_GS_SOCKET=%s", sock);
    for (size_t i = 0; i < n; i += strlen(env + i) + 1)
        if (!strcmp(env + i, want)) return 1;
    return 0;
}

/* every background app's GPU memory from a measurement (vram_mib) */
static void susp_vram_update(const struct gpu_mem *g)
{
    for (int i = 0; i < G.nsusp; i++) {
        int sum = 0;
        pid_t gp = rdx_proc_pid(G.susp[i].gs);
        for (int k = 0; k < g->nproc; k++)
            if (pid_in_app(g->pid[k], gp, G.susp[i].sock)) sum += g->mib[k];
        G.susp[i].vram_mib = sum;
    }
}

/* one more app starts while others run: little free memory on the seat's GPU warns (NULL = enough or
 * not measurable); nothing is closed, the client names the apps that hold it */
static cJSON *vram_warning(void)
{
    struct gpu_mem g;
    if (!G.nsusp || gpu_mem_measure(&g)) return NULL;
    susp_vram_update(&g);
    int thr = (int)jnum(cfg_defaults(), "suspend_vram_warn_mib", VRAM_WARN_MIB);
    fprintf(stderr, "[hostd] seat GPU: %d of %d MiB free with %d app(s) in the background (warning below %d MiB)\n",
            g.free_mib, g.total_mib, G.nsusp, thr);
    if (g.free_mib >= thr) return NULL;
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, "free_mib", g.free_mib);
    cJSON_AddNumberToObject(w, "total_mib", g.total_mib);
    cJSON_AddNumberToObject(w, "threshold_mib", thr);
    cJSON *a = cJSON_AddArrayToObject(w, "apps");
    char text[512];
    int tl = snprintf(text, sizeof(text), "Only %d MiB free on the graphics card; still running:", g.free_mib);
    for (int i = 0; i < G.nsusp; i++) {
        cJSON *o = cJSON_CreateObject();
        const char *name = jstr(find_app(G.susp[i].app), "name", G.susp[i].app);
        cJSON_AddStringToObject(o, "app", G.susp[i].app);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "vram_mib", G.susp[i].vram_mib);
        cJSON_AddItemToArray(a, o);
        if (tl > 0 && (size_t)tl < sizeof(text))
            tl += snprintf(text + tl, sizeof(text) - (size_t)tl, "%s %s (%d MiB)", i ? "," : "", name, G.susp[i].vram_mib);
    }
    cJSON_AddStringToObject(w, "text", text);
    fprintf(stderr, "[hostd] VRAM warning: %s\n", text);
    return w;
}

static void on_child_exit(struct rdx_proc *p, int status, void *user)
{
    (void)user;
    struct session *s = &G.S;
    if (susp_owns(p)) return;                           /* an app in the background: session_tick tidies up */
    if (s->state != S_STARTING && s->state != S_RUNNING) return;   /* we are stopping it ourselves */
    if (s->pending_end) return;
    if (p == s->clip) {
        fprintf(stderr, "[hostd] clipboard helper ended (status %d): the session continues without the clipboard\n", status);
        return;
    }
    if (p == s->mic_rx) {
        /* the microphone is optional: the session goes on without it */
        fprintf(stderr, "[hostd] mic_rx ended (status %d): the session continues without the client's microphone\n", status);
        return;
    }
    s->pending_end = 1;
    snprintf(s->end_proc, sizeof(s->end_proc), "%s", rdx_proc_name(p));
    s->end_status = status;
    if (WIFEXITED(status))
        snprintf(s->end_reason, sizeof(s->end_reason), "%s exited with %d", rdx_proc_name(p), WEXITSTATUS(status));
    else
        snprintf(s->end_reason, sizeof(s->end_reason), "%s killed by signal %d", rdx_proc_name(p),
                 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

static struct rdx_proc *spawn(const char *name, char **argv)
{
    struct session *s = &G.S;
    if (s->nprocs == MAX_PROCS) return NULL;
    fprintf(stderr, "[hostd] start %s:", name);
    for (char **a = argv; *a; a++) fprintf(stderr, " %s", *a);
    fprintf(stderr, "\n");
    struct rdx_proc *p = rdx_proc_spawn(name, argv, on_child_exit, NULL);
    if (p) s->procs[s->nprocs++] = p;
    /* rdx_gs_src's variable refresh state for /api/state, kept apart from the app's own output */
    if (p && !strcmp(name, "gs_src")) rdx_proc_keep_last(p, "[gs] vrr: ");
    return p;
}

/* end the session: stop all children, tell the owner, keep the children for the log view.
 * suspend: a gamescope app's rdx_gs_src stays (detached), the app keeps running.
 * Never called from inside an rdx_proc exit callback. */
static void end_session_ex(const char *reason, const char *proc, int status, int suspend)
{
    struct session *s = &G.S;
    if (s->state == S_IDLE) return;
    int was_starting = s->state == S_STARTING;
    s->state = S_STOPPING;
    fprintf(stderr, "[hostd] session ends: %s\n", reason);
    for (int i = 0; suspend && s->app_cfg && !s->local_join && i < s->nprocs; i++) {
        struct rdx_proc *p = s->procs[i];
        if (strcmp(rdx_proc_name(p), "gs_src") || !rdx_proc_running(p)) continue;
        int k = susp_find(s->app);
        if (k >= 0) quit_suspended_at(k, "replaced");      /* never twice (cannot happen: a resume takes it out) */
        if (G.nsusp == APPS_MAX)                           /* cannot happen either: begin_session keeps the limit */
            quit_suspended_at(0, "more apps in the background than APPS_MAX");
        kill(rdx_proc_pid(p), SIGUSR2);                     /* detach from the sink, keep the app */
        struct susp_app *a = &G.susp[G.nsusp++];
        memset(a, 0, sizeof(*a));
        a->gs = p;
        a->vram_mib = -1;
        snprintf(a->app, sizeof(a->app), "%s", s->app);
        snprintf(a->sock, sizeof(a->sock), "%s", s->gs_sock);
        cJSON *m = cJSON_GetObjectItem(s->params, "client_mode");
        a->mode = m ? cJSON_Duplicate(m, 1) : cJSON_CreateObject();
        if (s->local_window) {
            /* a window renders at its own size and follows its resizes (rdx_gs_src logs each):
             * the paused app has the size of the last one */
            static char tail[16384];
            size_t tn = rdx_proc_log_tail(p, tail, sizeof(tail) - 1);
            tail[tn] = 0;
            unsigned rw, rh;
            for (const char *q = tail; (q = strstr(q, "local window resized: ")); q++) {
                const char *aw = strstr(q, "-> ");
                if (aw && sscanf(aw, "-> %ux%u", &rw, &rh) == 2) {
                    cJSON_ReplaceItemInObject(a->mode, "w", cJSON_CreateNumber(rw));
                    cJSON_ReplaceItemInObject(a->mode, "h", cJSON_CreateNumber(rh));
                }
            }
        }
        a->tearing = cJSON_IsTrue(cJSON_GetObjectItem(s->params, "tearing"));
        a->since = now_s();
        s->procs[i] = s->procs[--s->nprocs];
        fprintf(stderr, "[hostd] %s suspended: keeps running, the next start of it resumes (%d app(s) in the background)\n",
                s->app, G.nsusp);
        break;
    }
    /* SIGTERM to all first, so the grace periods run in parallel, then wait for each */
    for (int i = 0; i < s->nprocs; i++)
        if (rdx_proc_running(s->procs[i])) kill(rdx_proc_pid(s->procs[i]), SIGTERM);
    for (int i = 0; i < s->nprocs; i++) rdx_proc_stop(s->procs[i], STOP_GRACE_MS);
    if (s->owner) {
        if (was_starting) ctl_reply(s->owner, s->reply_id, 0, reason, NULL);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "event", "session.ended");
        cJSON_AddStringToObject(e, "reason", reason);
        cJSON_AddStringToObject(e, "proc", proc ? proc : "");
        cJSON_AddNumberToObject(e, "status", status);
        rdx_ctl_send(s->owner, e);
        cJSON_Delete(e);
    }
    for (int i = 0; i < G.nlast; i++) rdx_proc_free(G.last[i]);
    memcpy(G.last, s->procs, sizeof(s->procs));
    G.nlast = s->nprocs;
    snprintf(G.last_end, sizeof(G.last_end), "%s", reason);
    cJSON_Delete(s->params);
    cJSON_Delete(s->app_cfg);
    cJSON_Delete(s->vram_warn);
    memset(s, 0, sizeof(*s));
    s->state = S_IDLE;
}

static void end_session(const char *reason, const char *proc, int status)
{
    end_session_ex(reason, proc, status, 0);
}



static const char *jstr(cJSON *o, const char *k, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static int jbool(cJSON *o, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static double jnum(cJSON *o, const char *k, double def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int have_bin(const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", G.repo, rel);
    return !access(p, X_OK);
}

static cJSON *find_app(const char *id)
{
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(G.cfg, "apps")) {
        cJSON *i = cJSON_GetObjectItem(a, "id");
        if (cJSON_IsString(i) && !strcmp(i->valuestring, id)) return a;
    }
    return NULL;
}

/* configured app list from the web UI: NULL if valid, else the reason */
static const char *validate_apps(cJSON *apps)
{
    if (!cJSON_IsArray(apps)) return "apps must be an array";
    cJSON *a;
    int i = 0;
    cJSON_ArrayForEach(a, apps) {
        cJSON *id = cJSON_GetObjectItem(a, "id"), *nm = cJSON_GetObjectItem(a, "name");
        cJSON *kind = cJSON_GetObjectItem(a, "kind"), *cmd = cJSON_GetObjectItem(a, "cmd");
        cJSON *env = cJSON_GetObjectItem(a, "env"), *cap = cJSON_GetObjectItem(a, "capture_output"), *x;
        if (!cJSON_IsString(id) || !*id->valuestring || strlen(id->valuestring) > 48) return "every app needs an id (1..48 chars)";
        if (!strncmp(id->valuestring, "mirror:", 7)) return "app ids starting with mirror: are reserved";
        if (!cJSON_IsString(nm) || !*nm->valuestring) return "every app needs a name";
        if (!cJSON_IsString(kind) || strcmp(kind->valuestring, "gamescope")) return "kind must be gamescope";
        const char *mgpu_err = rdx_mgpu_validate_app(a);   /* "framegen", "optiscaler" (host/rdx_mgpu.h) */
        if (mgpu_err) return mgpu_err;
        if (!cJSON_IsArray(cmd) || !cJSON_GetArraySize(cmd)) return "cmd must be a non-empty array";
        cJSON_ArrayForEach(x, cmd) if (!cJSON_IsString(x) || !*x->valuestring) return "cmd entries must be non-empty strings";
        if (env && !cJSON_IsObject(env)) return "env must be an object";
        cJSON_ArrayForEach(x, env) if (!cJSON_IsString(x) || !*x->string || strchr(x->string, '=')) return "env: KEY must be non-empty without '=', values strings";
        if (cap && !cJSON_IsString(cap)) return "capture_output must be a string";
        cJSON *ga = cJSON_GetObjectItem(a, "gamescope_args");
        if (ga && !cJSON_IsArray(ga)) return "gamescope_args must be an array";
        cJSON_ArrayForEach(x, ga) if (!cJSON_IsString(x) || !*x->valuestring) return "gamescope_args entries must be non-empty strings";
        const char *strs[] = {"icon", "desktop_id", "note"};
        for (size_t k = 0; k < 3; k++)
            if (cJSON_GetObjectItem(a, strs[k]) && !cJSON_IsString(cJSON_GetObjectItem(a, strs[k]))) return "icon, desktop_id, note must be strings";
        cJSON *b;
        int j = 0;
        cJSON_ArrayForEach(b, apps) {
            if (j++ >= i) break;
            if (!strcmp(cJSON_GetObjectItem(b, "id")->valuestring, id->valuestring)) return "app ids must be unique";
        }
        i++;
    }
    return NULL;
}

static int local_running(void);
static int local_sleep(int sleep);
static int spawn_local_stream(const char *fps, const char *mtu);

/* A gamescope app gets the client's keyboard, mouse and USB HID for itself: rdx_input_rx and
 * rdx_usb_host --private put their devices on the seat "seat-rdx" (common/rdx_input.h), which no
 * desktop reads (desktop/72-rdma-display-input.rules), so playing never also drives this computer's
 * desktop. Only a mirrored desktop, and a client joining the app on a seat's own monitor (gamescope's
 * DRM backend reads seat0 there), get ordinary seat0 devices. The rule gives the private devices to
 * the group "rdx-input"; the app user becomes a member here (rdx_gs_src and runuser start the app
 * with initgroups, so it applies to this very start). */
static void input_group_member(void)
{
    struct group *g = getgrnam("rdx-input");
    struct rdx_desktop_user du;
    if (!g) {
        fprintf(stderr, "[hostd] group rdx-input missing (desktop/install.sh host creates it): the app may not "
                        "read the client's input\n");
        return;
    }
    if (rdx_desktop_user(cfg_str("desktop_user", ""), &du) || du.gid == g->gr_gid) return;
    for (char **m = g->gr_mem; m && *m; m++)
        if (!strcmp(*m, du.name)) return;
    pid_t p = fork();
    if (p == 0) {
        execlp("gpasswd", "gpasswd", "-a", du.name, "rdx-input", (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st))
        fprintf(stderr, "[hostd] could not add %s to the group rdx-input\n", du.name);
    else fprintf(stderr, "[hostd] %s added to the group rdx-input (the client's input devices of gamescope apps)\n",
                 du.name);
}

static void begin_session(struct rdx_ctl *c, double id, cJSON *req)
{
    struct session *s = &G.S;
    /* the app runs in a window on this computer: the client takes it over (the window closes,
     * rdx_gs_src detaches and resumes to the client below) */
    if (s->state != S_IDLE && s->local_window && !strcmp(jstr(req, "app", ""), s->app))
        end_session_ex("taken over by the client", "", -1, 1);
    if (s->state != S_IDLE) { ctl_reply(c, id, 0, "a session is already active", NULL); return; }
    cJSON *d = cfg_defaults(), *client = cJSON_GetObjectItem(req, "client"), *ports = cJSON_GetObjectItem(req, "ports");
    refresh_outputs(0);

    /* app "mirror:<output>" or a configured app id; empty = default output, else the first one */
    const char *app = jstr(req, "app", "");
    int is_mirror = !*app || !strncmp(app, "mirror:", 7);
    cJSON *app_cfg = is_mirror ? NULL : find_app(app);
    if (!is_mirror && !app_cfg) { ctl_reply(c, id, 0, "unknown app (see hello.apps)", NULL); return; }
    const char *out = app_cfg ? "" : *app ? app + 7 : jstr(d, "output", "");
    if (is_mirror && !*out && G.nouts) out = G.outs[0].name;
    int known = 0;
    for (int i = 0; i < G.nouts; i++) known |= !strcmp(G.outs[i].name, out);
    if ((is_mirror || *out) && !known) { ctl_reply(c, id, 0, "unknown output (see hello.outputs)", NULL); return; }
    /* the seat's own monitor: "monitor" refuses every client; the home app running there is
     * joined (same gamescope, the stream is added to it), anything else runs as before */
    if (!strcmp(cfg_str("picture", "monitor+stream"), "monitor") && !G.local.guard_busy) {
        ctl_reply(c, id, 0, "This seat shows its picture only on its own monitor (setting \"Picture: Monitor only\" "
                            "in the host's web UI).", NULL);
        return;
    }
    int local_join = app_cfg && local_running() && !strcmp(app, G.local.app);

    /* apps in the background: this one resumes (also as the running instance of the same single-instance
     * program), another tearing setting restarts only this one; a new app keeps the others running
     * (suspend_multi), up to APPS_MAX, or, without it, closes the one in the background */
    int resume = 0;
    char asked[64] = "", appbuf[64];
    cJSON *vram_warn = NULL;
    if (!local_join) {
        int k = app_cfg ? susp_find(app) : -1;
        if (k < 0 && app_cfg && (k = susp_same_instance(app_cfg)) >= 0) {
            fprintf(stderr, "[hostd] %s is the same single-instance program as %s in the background: resuming that one\n",
                    app, G.susp[k].app);
            snprintf(asked, sizeof(asked), "%s", app);
            snprintf(appbuf, sizeof(appbuf), "%s", G.susp[k].app);
            app = appbuf;
            app_cfg = find_app(app);
        }
        if (k >= 0) {
            int tearing = jbool(req, "tearing", 0);
            if (rdx_proc_running(G.susp[k].gs) && tearing == G.susp[k].tearing) resume = 1;
            else quit_suspended_at(k, rdx_proc_running(G.susp[k].gs) ? "the client's tearing setting changed" : "it ended");
        }
        if (!resume && !multi_on()) {
            if (G.nsusp) quit_suspended_all("another app starts (one app in the background: suspend_multi off or an older gamescope)");
        } else if (!resume && app_cfg && G.nsusp + 1 > APPS_MAX) {
            char why[400];
            int wl = snprintf(why, sizeof(why), "%d apps run on this seat already (", G.nsusp);
            for (int i = 0; i < G.nsusp && wl > 0 && (size_t)wl < sizeof(why); i++)
                wl += snprintf(why + wl, sizeof(why) - (size_t)wl, "%s%s", i ? ", " : "", jstr(find_app(G.susp[i].app), "name", G.susp[i].app));
            if (wl > 0 && (size_t)wl < sizeof(why)) snprintf(why + wl, sizeof(why) - (size_t)wl, "): quit one of them first");
            fprintf(stderr, "[hostd] refused %s: %s\n", app, why);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddItemToObject(e, "suspended_apps", suspended_list_json());
            ctl_reply(c, id, 0, why, e);
            return;
        } else if (!resume && app_cfg) vram_warn = vram_warning();
    }
    memset(s, 0, sizeof(*s));
    s->resume = resume;
    s->vram_warn = vram_warn;
    snprintf(s->asked_app, sizeof(s->asked_app), "%s", asked);
    s->local_join = local_join;
    s->input_private = app_cfg && strcmp(jstr(app_cfg, "kind", "gamescope"), "mirror") && !local_join;
    /* gamescope grabs the devices it reads (EVIOCGRAB, both backends: the host's console keyboard
     * handler never sees them); until it has, rdx_input_rx / rdx_usb_host hold keys back.
     * A mirrored desktop does not grab: no waiting there. */
    s->input_wait_grab = app_cfg && strcmp(jstr(app_cfg, "kind", "gamescope"), "mirror");
    s->app_cfg = app_cfg ? cJSON_Duplicate(app_cfg, 1) : NULL;
    s->owner = c;
    s->reply_id = id;
    s->t_start = now_s();
    s->deadline = s->t_start + START_TIMEOUT_S;
    snprintf(s->output, sizeof(s->output), "%s", out);
    if (app_cfg) snprintf(s->app, sizeof(s->app), "%s", app);
    else snprintf(s->app, sizeof(s->app), "mirror:%s", out);
    snprintf(s->client_ip, sizeof(s->client_ip), "%s", jstr(client, "link_ip", rdx_ctl_peer(c)));
    s->video_port = (int)jnum(ports, "video", cfg_int("video_port", 18650));
    /* listeners on this host (input, USB): always this seat's ports, so two seats never collide;
     * the client learns them from the reply. Video and audio listen on the client: its choice. */
    s->input_port = cfg_int("input_port", 18660);
    s->audio_port = (int)jnum(ports, "audio", cfg_int("audio_port", 18670));
    s->usb_port = cfg_int("usb_port", 18680);
    s->mic_port = cfg_int("mic_port", 18690);
    s->clip_port = cfg_int("clip_port", 18700);

    /* effective parameters: request, else host defaults */
    cJSON *p = s->params = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "app", s->app);
    cJSON_AddStringToObject(p, "output", s->output);
    cJSON_AddStringToObject(p, "client_ip", s->client_ip);
    cJSON_AddNumberToObject(p, "fps", jnum(req, "fps", jnum(d, "fps", 0)));
    /* "latency" (default): capture/compose as soon as a frame exists, no cap;
     * "smooth": even cadence locked to the client's vblank at "fps" (the client picks a divisor) */
    cJSON_AddStringToObject(p, "pacing", !strcmp(jstr(req, "pacing", "latency"), "smooth") ? "smooth" : "latency");
    /* the client drives its screen at exactly this output's refresh ("match the host"):
     * then capture at the output's vblank (fixed phase, one frame per client vblank)
     * instead of on commit (random phase within the period, which doubles/skips frames) */
    cJSON_AddNumberToObject(p, "match_hz", jnum(req, "match_hz", 0));
    cJSON_AddBoolToObject(p, "cursor", jbool(req, "cursor", jbool(d, "cursor", 1)));
    cJSON_AddBoolToObject(p, "tearing", jbool(req, "tearing", 0));
    /* variable refresh: the client's screen ("display"), decided once here, changed by session.update */
    cJSON *disp = cJSON_GetObjectItem(req, "display");
    if (local_join) {
        G.local.vrr = local_monitor_vrr();
        fprintf(stderr, "[hostd] the seat's monitor %s: vrr_capable %s\n", G.local.conn,
                G.local.vrr < 0 ? "unknown" : G.local.vrr ? "1" : "0");
    }
    rdx_vrr_decide(disp, jbool(d, "vrr", 1), session_vrr_kind(s), jbool(req, "tearing", 0),
                   jnum(d, "vrr_cap_frac", RDX_VRR_CAP_FRAC_DEFAULT), &s->vrr);
    if (cJSON_IsObject(disp)) cJSON_AddItemToObject(p, "display", cJSON_Duplicate(disp, 1));
    cJSON_AddBoolToObject(p, "vrr", s->vrr.active);
    fprintf(stderr, "[hostd] %s\n", s->vrr.why);
    cJSON_AddStringToObject(p, "gpu_prio", jstr(req, "gpu_prio", jstr(d, "gpu_prio", "high")));
    cJSON_AddBoolToObject(p, "audio", jbool(req, "audio", jbool(d, "audio", 1)));
    cJSON_AddBoolToObject(p, "input", jbool(req, "input", jbool(d, "input", 1)));
    cJSON *usb = cJSON_GetObjectItem(req, "usb");
    int want_usb = jbool(d, "usb", 1) && cJSON_IsArray(usb) && cJSON_GetArraySize(usb) > 0;
    cJSON_AddBoolToObject(p, "usb", want_usb);
    /* the client's microphone: it asks for it ("mic"), the host may refuse it (defaults.mic) */
    cJSON_AddBoolToObject(p, "mic", jbool(req, "mic", 0) && jbool(d, "mic", 1) && have_bin("p5/rdx_audio_rx"));
    cJSON_AddBoolToObject(p, "clipboard", jbool(req, "clipboard", 0) && jbool(d, "clipboard", 1) && have_bin("clip/rdx_clip"));
    cJSON *mode = cJSON_GetObjectItem(client, "mode");
    if (mode) cJSON_AddItemToObject(p, "client_mode", cJSON_Duplicate(mode, 1));
    cJSON *jports = cJSON_AddObjectToObject(p, "ports");
    cJSON_AddNumberToObject(jports, "video", s->video_port);
    cJSON_AddNumberToObject(jports, "input", s->input_port);
    cJSON_AddNumberToObject(jports, "audio", s->audio_port);
    cJSON_AddNumberToObject(jports, "usb", s->usb_port);
    cJSON_AddNumberToObject(jports, "mic", s->mic_port);
    s->state = S_STARTING;
    if (resume) cJSON_AddBoolToObject(p, "resumed", 1);
    if (asked[0]) cJSON_AddStringToObject(p, "asked_app", asked);
    if (local_join) cJSON_AddStringToObject(p, "joined", G.local.conn);
    fprintf(stderr, "[hostd] session.start from %s: %s%s\n", rdx_ctl_peer(c), s->app,
            resume ? " (resume)" : local_join ? " (joins the seat's monitor)" : "");

    char exe[PATH_MAX], port[16], mtu[16];
    snprintf(mtu, sizeof(mtu), "%d", cfg_int("mtu", 4096));
    if (cJSON_IsTrue(cJSON_GetObjectItem(p, "input"))) {
        snprintf(exe, sizeof(exe), "%s/p4/rdx_input_rx", G.repo);
        snprintf(port, sizeof(port), "%d", s->input_port);
        char *argv[] = {exe, "--dev", (char *)cfg_str("rdma_dev", ""), "--bind", (char *)cfg_str("link_ip", ""),
                        "--port", port, "--mtu", mtu,
                        /* --private implies a gamescope, i.e. --wait-grab: never a NULL before it */
                        s->input_wait_grab ? "--wait-grab" : NULL, s->input_private ? "--private" : NULL, NULL};
        if (s->input_private) input_group_member();
        s->input_rx = spawn("input_rx", argv);
        if (!s->input_rx) end_session("cannot start rdx_input_rx", "input_rx", -1);
    }
    /* without input, or once input_rx listens: start_senders() via the main loop */
}

/* ---- per-seat hook for gamescope apps (frame generation on "helper_gpu", layers, ...) ----
 * Adds "KEY=VALUE" strings to env[] (at most max), returns the new count. The app's own "env"
 * comes later and wins. Extend here; keep it free of blocking work. */
static int seat_app_hook(cJSON *app_cfg, char env[][512], int n, int max)
{
    (void)app_cfg;
    char sn[16];
    snprintf(sn, sizeof(sn), "%d", G.seat);
    const char *fixed[][2] = {{"RDX_SEAT", sn}, {"RDX_GPU", G.gpu_pci}, {"RDX_HELPER_GPU", cfg_str("helper_gpu", "")}};
    for (size_t i = 0; i < 3 && n < max; i++)
        snprintf(env[n++], 512, "%s=%s", fixed[i][0], fixed[i][1]);
    cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(G.cfg, "seat_env")) {
        if (n == max) break;
        if (cJSON_IsString(x) && *x->string && !strchr(x->string, '=')) snprintf(env[n++], 512, "%s=%s", x->string, x->valuestring);
    }
    return n;
}

/* config "pre_launch": run it (seat env + RDX_APP), wait at most 10 s; 0 = ok or none */
static int run_pre_launch(const char *app, char env[][512], int nenv)
{
    cJSON *pl = cJSON_GetObjectItem(G.cfg, "pre_launch");
    int argc = cJSON_GetArraySize(pl);
    if (argc <= 0) return 0;
    char *argv[33];
    int k = 0;
    cJSON *x;
    cJSON_ArrayForEach(x, pl) if (k < 32 && cJSON_IsString(x)) argv[k++] = x->valuestring;
    argv[k] = NULL;
    if (!k) return 0;
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < nenv; i++) putenv(env[i]);
        setenv("RDX_APP", app, 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    for (int i = 0; i < 200; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

#define DBUS_RUN_SESSION "/usr/bin/dbus-run-session"

/* the app's own session D-Bus: av gets "dbus-run-session --" in front of the app's cmd. A
 * single-instance app (GApplication, Qt/KDBusService) otherwise finds its instance on the
 * desktop through the user's bus, hands the start over and quits at once (Lutris), and
 * the other way round a later start on the desktop would open in the stream. On by default
 * ("private_dbus" in the app, else in "defaults"); a flatpak keeps the user's bus (its
 * portals and document store are reached through it). What the own bus changes: the header
 * ("Own session bus"). Steam's single instance is ~/.steam/steam.pipe, not D-Bus. */
static int app_bus_wrap(cJSON *app_cfg, char **av, int n)
{
    const cJSON *c0 = cJSON_GetArrayItem(cJSON_GetObjectItem(app_cfg, "cmd"), 0);
    const char *b = cJSON_IsString(c0) ? strrchr(c0->valuestring, '/') : NULL;
    b = b ? b + 1 : cJSON_IsString(c0) ? c0->valuestring : "";
    if (!jbool(app_cfg, "private_dbus", jbool(cfg_defaults(), "private_dbus", 1)) || !strcmp(b, "flatpak") ||
        access(DBUS_RUN_SESSION, X_OK))
        return n;
    av[n++] = DBUS_RUN_SESSION;
    av[n++] = "--";
    return n;
}

/* gamescope app: gs/rdx_gs_src connects to the client's sink, owns the NIC-readable VRAM
 * buffers and starts gamescope (and the app) as the desktop user; 0 = ok */
static int spawn_gamescope(const char *fps, const char *mtu)
{
    struct session *s = &G.S;
    struct rdx_desktop_user du;
    if (rdx_desktop_user(cfg_str("desktop_user", ""), &du)) {
        end_session("no desktop user to run the app: nobody is logged in on this computer's desktop, there is no "
                    "single lingering user and no \"desktop_user\" in the config", "gs_src", -1);
        return -1;
    }
    static char exe[PATH_MAX], gs[PATH_MAX], port[16], sock[96], lim[16];
    static char envbuf[32][512];
    const char *src = cfg_str("gs_src_bin", "");
    if (*src) snprintf(exe, sizeof(exe), "%s", src);
    else snprintf(exe, sizeof(exe), "%s/gs/rdx_gs_src", G.repo);
    snprintf(gs, sizeof(gs), "%s", cfg_str("gamescope_bin", "/opt/rdx-gs/bin/gamescope"));
    if (access(gs, X_OK)) snprintf(gs, sizeof(gs), "/opt/rdx-gs/bin/gamescope");
    if (access(exe, X_OK)) { end_session("gs/rdx_gs_src is not built on this host", "gs_src", -1); return -1; }
    if (access(gs, X_OK)) { end_session("gamescope is not installed (host/install-gamescope.sh)", "gs_src", -1); return -1; }
    snprintf(port, sizeof(port), "%d", s->video_port);
    int k = s->resume ? susp_find(s->app) : -1;
    if (s->resume && (k < 0 || !rdx_proc_running(G.susp[k].gs))) {
        fprintf(stderr, "[hostd] %s ended in the background before its resume: starting it fresh\n", s->app);
        if (k >= 0) quit_suspended_at(k, "it ended");
        s->resume = 0;
        k = -1;
    }
    if (k >= 0) {
        /* the app still runs: point its rdx_gs_src at the new sink (same buffers and MRs) */
        struct susp_app *a = &G.susp[k];
        char peer[128];
        snprintf(peer, sizeof(peer), "%s.peer", a->sock);
        FILE *f = fopen(peer, "w");
        /* third field: the new client's variable refresh cap, 0 = fixed (rdx_gs_src gs_resume) */
        if (!f || fprintf(f, "%s %d %.3f\n", s->client_ip, s->video_port, s->vrr.active ? s->vrr.cap_hz : 0.0) < 0 ||
            fclose(f)) {
            if (f) fclose(f);
            quit_suspended_at(k, "cannot write the resume request");
            s->resume = 0;
        } else {
            snprintf(s->gs_sock, sizeof(s->gs_sock), "%s", a->sock);
            s->procs[s->nprocs++] = a->gs;
            kill(rdx_proc_pid(a->gs), SIGUSR1);
            fprintf(stderr, "[hostd] resume %s: rdx_gs_src -> %s:%d (%d other app(s) in the background)\n", a->app,
                    s->client_ip, s->video_port, G.nsusp - 1);
            susp_remove(k);
            return 0;
        }
    }
    gs_sock_path(s->app, sock, sizeof(sock));
    snprintf(s->gs_sock, sizeof(s->gs_sock), "%s", sock);
    char *av[160];
    int n = 0;
    av[n++] = exe;
    av[n++] = "--dev"; av[n++] = (char *)cfg_str("rdma_dev", "");
    av[n++] = "--peer"; av[n++] = s->client_ip;
    av[n++] = "--port"; av[n++] = port;
    av[n++] = "--mtu"; av[n++] = (char *)mtu;
    av[n++] = "--log-dir"; av[n++] = (char *)cfg_str("log_dir", "/var/log/rdx");
    av[n++] = "--gamescope"; av[n++] = gs;
    av[n++] = "--socket"; av[n++] = sock;
    av[n++] = "--user"; av[n++] = du.name;
    static char leadf[64];
    lead_path(leadf, sizeof(leadf));
    av[n++] = "--lead-file"; av[n++] = leadf;
    if (strcmp(cfg_str("gs_buffers", "auto"), "auto")) { av[n++] = "--buffers"; av[n++] = (char *)cfg_str("gs_buffers", "auto"); }
    if (jbool(s->params, "tearing", 0)) av[n++] = "--tearing";
    static char vrr_cap[32];
    if (s->vrr.active) {                          /* the app's pace up to the client's maximum */
        snprintf(vrr_cap, sizeof(vrr_cap), "%.3f", s->vrr.cap_hz);
        av[n++] = "--vrr"; av[n++] = vrr_cap;
    }
    if (G.gpu_pci[0]) { av[n++] = "--gpu"; av[n++] = G.gpu_pci; }
    static char prio[16];
    snprintf(prio, sizeof(prio), "%s", cfg_str("priority", "normal"));
    if (strcmp(prio, "normal")) { av[n++] = "--priority"; av[n++] = prio; }
    cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(s->app_cfg, "gamescope_args")) {
        if (n > 120) break;
        av[n++] = "--gs-arg"; av[n++] = x->valuestring;
    }
    if (jnum(s->params, "fps", 0) > 0) {
        snprintf(lim, sizeof(lim), "%s", fps);
        av[n++] = "--gs-arg"; av[n++] = "--framerate-limit";
        av[n++] = "--gs-arg"; av[n++] = lim;
    }
    int ne = seat_app_hook(s->app_cfg, envbuf, 0, 16);
    ne = rdx_mgpu_app_env(G.cfg, s->app_cfg, &du, envbuf, ne, 26);   /* frame generation, OptiScaler */
    n = rdx_mgpu_gs_args(G.cfg, s->app_cfg, &du, av, n, 128);          /* --fg helper on the helper GPU */
    for (int i = 0; i < ne && n < 140; i++) { av[n++] = "--env"; av[n++] = envbuf[i]; }
    if (run_pre_launch(s->app, envbuf, ne)) {
        end_session("the seat's pre_launch command failed", "gs_src", -1);
        return -1;
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(s->app_cfg, "env")) {
        /* a key the hooks set already: WINEDLLOVERRIDES was merged there (app value first, then
         * OptiScaler's "<proxy>=n,b"), any other key takes the app's value in place, so the
         * result never depends on the order rdx_gs_src applies its --env arguments */
        size_t kl = strlen(x->string);
        int dup = -1;
        for (int i = 0; i < ne && dup < 0; i++)
            if (!strncmp(envbuf[i], x->string, kl) && envbuf[i][kl] == '=') dup = i;
        if (dup >= 0) {
            if (strcmp(x->string, "WINEDLLOVERRIDES"))
                snprintf(envbuf[dup], sizeof(envbuf[dup]), "%s=%s", x->string, x->valuestring);
            continue;
        }
        if (ne == 32 || n > 140) break;
        snprintf(envbuf[ne], sizeof(envbuf[ne]), "%s=%s", x->string, x->valuestring);
        av[n++] = "--env"; av[n++] = envbuf[ne++];
    }
    av[n++] = "--";
    n = app_bus_wrap(s->app_cfg, av, n);
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(s->app_cfg, "cmd")) {
        if (n == 158) break;
        av[n++] = x->valuestring;
    }
    av[n] = NULL;
    cJSON_AddStringToObject(s->params, "run_as", du.name);
    if (!spawn("gs_src", av)) { end_session("cannot start gs/rdx_gs_src", "gs_src", -1); return -1; }
    return 0;
}

/* the DRM node of one kind ("card", "renderD") of GPU bdf among the nodes in /dev/dri. A
 * container has no /dev/dri/by-path; /sys/dev/char/226:N/device names the PCI device. */
static int drm_node_for(const char *bdf, const char *kind, char *out, size_t n)
{
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    size_t kl = strlen(kind);
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        if (strncmp(e->d_name, kind, kl) || !isdigit((unsigned char)e->d_name[kl])) continue;
        char dev[300], sys[64], link[256];
        struct stat st;
        snprintf(dev, sizeof(dev), "/dev/dri/%s", e->d_name);
        if (stat(dev, &st) || !S_ISCHR(st.st_mode)) continue;
        snprintf(sys, sizeof(sys), "/sys/dev/char/%u:%u/device", major(st.st_rdev), minor(st.st_rdev));
        ssize_t l = readlink(sys, link, sizeof(link) - 1);
        if (l <= 0) continue;
        link[l] = 0;
        const char *b = strrchr(link, '/');
        if (!strcasecmp(b ? b + 1 : link, bdf)) { snprintf(out, n, "%s", dev); found = 1; }
    }
    if (d) closedir(d);
    return found;
}

/* rdx_mirror_src for the session's output; 0 = ok */
static int spawn_mirror(const char *fps, const char *mtu)
{
    struct session *s = &G.S;
    cJSON *p = s->params;
    char exe[PATH_MAX], port[16];
    snprintf(exe, sizeof(exe), "%s/p1/rdx_mirror_src", G.repo);
    snprintf(port, sizeof(port), "%d", s->video_port);
    char *av[32];
    int n = 0;
    av[n++] = exe;
    av[n++] = "--dev"; av[n++] = (char *)cfg_str("rdma_dev", "");
    av[n++] = "--peer"; av[n++] = s->client_ip;
    av[n++] = "--port"; av[n++] = port;
    av[n++] = "--mtu"; av[n++] = (char *)mtu;
    /* a seat with its own GPU: that GPU's nodes by PCI path (or, in a container without
     * by-path, by sysfs), else the configured ones */
    static char bycard[300], byrender[300];
    int bypath = 0;
    if (*cfg_str("gpu", "")) {
        snprintf(bycard, sizeof(bycard), "/dev/dri/by-path/pci-%s-card", G.gpu_pci);
        snprintf(byrender, sizeof(byrender), "/dev/dri/by-path/pci-%s-render", G.gpu_pci);
        bypath = (!access(bycard, F_OK) && !access(byrender, F_OK)) ||
                 (drm_node_for(G.gpu_pci, "card", bycard, sizeof(bycard)) &&
                  drm_node_for(G.gpu_pci, "renderD", byrender, sizeof(byrender)));
    }
    av[n++] = "--card"; av[n++] = bypath ? bycard : (char *)cfg_str("card", "/dev/dri/card1");
    av[n++] = "--render"; av[n++] = bypath ? byrender : (char *)cfg_str("render", "/dev/dri/renderD128");
    av[n++] = "--output"; av[n++] = s->output;
    av[n++] = "--frames"; av[n++] = "0";
    av[n++] = "--log-dir"; av[n++] = (char *)cfg_str("log_dir", "/var/log/rdx");
    const char *sp = cfg_str("priority", "normal");
    av[n++] = "--gpu-prio"; av[n++] = (char *)(strcmp(sp, "normal") ? sp : jstr(p, "gpu_prio", "high"));
    if (jnum(p, "fps", 0) > 0) { av[n++] = "--fps"; av[n++] = (char *)fps; }
    if (!strcmp(jstr(p, "pacing", "latency"), "smooth")) { av[n++] = "--sync"; av[n++] = "lock"; }
    else if (jnum(p, "match_hz", 0) > 0) {
        /* capture at the output's vblank + a delay steered from the sink's "shown" reports,
         * so every frame arrives ~1 ms before the client's vblank (phase alignment) */
        av[n++] = "--commit-poll-us"; av[n++] = "0";
        av[n++] = "--phase-target-us"; av[n++] = "1000";
    }
    if (!jbool(p, "cursor", 1)) av[n++] = "--no-cursor";
    av[n] = NULL;
    if (!spawn("mirror_src", av)) { end_session("cannot start rdx_mirror_src", "mirror_src", -1); return -1; }
    return 0;
}

/* ---------------- the seat's own monitor (docs/SEAT-MONITOR.md) ----------------
 * With a monitor on the seat GPU's own connector and "picture" not "stream", the home app
 * ("home_app", default steam) runs in a gamescope with the DRM backend on that connector, as
 * the desktop user, without any client: direct scanout, VRR, HDR as gamescope does them. A
 * client starting the home app joins it: rdx_gs_src attaches to that gamescope's stream add-on
 * (RDX_GS_STREAM_SOCKET, "stream_via": "gamescope") or rdx_mirror_src captures the monitor
 * ("mirror", gamescope then runs with --disable-layers so the primary plane holds the whole
 * picture). Ending the stream leaves the monitor running. The monitor unplugged, the setting
 * changed to "stream" or the home app removed: the local gamescope ends and the console gets the
 * connector back. Input: local evdev devices and rdx_input_rx's mirrors both reach it; the side of
 * the last input leads the clock (gamescope patch). Sound: the seat's default sink (HDMI of the
 * GPU when it is the default) plays locally and rdx_audio_tx captures its monitor for the stream. */
static int local_running(void) { return G.local.gs && rdx_proc_running(G.local.gs); }

/* the seat GPU's PCI "vendor:device" for gamescope --prefer-vk-device ("10de:2206"), read for the bound
 * card (a seat may start on any card, docs/MULTISEAT.md "Card choice"; sysfs in a seat container is the
 * host's); 0 = found */
static int gpu_vk_ids(const char *pci, char *out, size_t n)
{
    char p[160], v[2][16];
    const char *f[2] = {"vendor", "device"};
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof(p), "/sys/bus/pci/devices/%s/%s", pci, f[i]);
        FILE *fh = fopen(p, "r");
        int ok = fh && fscanf(fh, "0x%15[0-9a-fA-F]", v[i]) == 1;
        if (fh) fclose(fh);
        if (!ok) return -1;
    }
    snprintf(out, n, "%s:%s", v[0], v[1]);
    return 0;
}

/* the NVIDIA GPU's UUID as 32 hex digits (/proc/driver/nvidia/gpus/<pci>/information "GPU UUID", the
 * same bytes as its Vulkan deviceUUID) for the gamescope patch's GAMESCOPE_VK_DEVICE_UUID, which picks
 * exactly that device (two cards of one model have the same --prefer-vk-device ids); 0 = found */
static int gpu_uuid_hex(const char *pci, char *out, size_t n)
{
    char p[160], line[256];
    snprintf(p, sizeof(p), "/proc/driver/nvidia/gpus/%s/information", pci);
    FILE *fh = fopen(p, "r");
    if (!fh || n < 33) { if (fh) fclose(fh); return -1; }
    int found = -1;
    while (found && fgets(line, sizeof(line), fh)) {
        const char *u = strstr(line, "GPU-");
        if (strncmp(line, "GPU UUID", 8) || !u) continue;
        size_t k = 0;
        for (u += 4; *u && k < 32; u++)
            if (isxdigit((unsigned char)*u)) out[k++] = (char)tolower((unsigned char)*u);
            else if (*u != '-') break;
        out[k] = 0;
        found = k == 32 ? 0 : -1;
        if (found) break;
    }
    fclose(fh);
    return found;
}

/* the seat GPU's card node (by PCI address, else the configured "card") */
static void local_card(char *card, size_t n)
{
    if (!G.gpu_pci[0] || !drm_node_for(G.gpu_pci, "card", card, n)) {
        char bp[300];
        snprintf(bp, sizeof(bp), "/dev/dri/by-path/pci-%s-card", G.gpu_pci);
        snprintf(card, n, "%s", G.gpu_pci[0] && !access(bp, F_OK) ? bp : cfg_str("card", "/dev/dri/card1"));
    }
}

/* the monitor's name from an EDID (display product name descriptor 0xFC), "" if none */
static void edid_name(const char *path, char *out, size_t n)
{
    unsigned char e[256];
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t len = fread(e, 1, sizeof(e), f);
    fclose(f);
    for (size_t d = 54; len >= 128 && d <= 108; d += 18) {
        if (e[d] || e[d + 1] || e[d + 2] || e[d + 3] != 0xFC) continue;
        size_t k = 0;
        for (; k < 13 && k + 1 < n && e[d + 5 + k] != 0x0A && e[d + 5 + k]; k++)
            out[k] = (char)(e[d + 5 + k] >= 0x20 && e[d + 5 + k] < 0x7F ? e[d + 5 + k] : '?');
        out[k] = 0;
        while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
    }
}

/* the connector to drive: "monitor_connector", else the first connected one; 1 = connected.
 * Read from sysfs (/sys/class/drm/cardN-<connector>/{status,edid,modes}): opening the card node
 * while no DRM master exists would make us its master for a moment, and gamescope could lose
 * that race when it starts. */
static int local_connector(char *name, size_t n, char *monitor, size_t mn, cJSON *list)
{
    char card[300];
    local_card(card, sizeof(card));
    name[0] = monitor[0] = 0;
    char real[PATH_MAX];
    const char *base = realpath(card, real) ? strrchr(real, '/') : strrchr(card, '/');
    base = base ? base + 1 : card;                       /* "card2" */
    size_t bl = strlen(base);
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    const char *want = cfg_str("monitor_connector", "");
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, base, bl) || de->d_name[bl] != '-') continue;
        const char *conn = de->d_name + bl + 1;           /* "HDMI-A-2", as gamescope -O names it */
        char path[PATH_MAX], st[32] = "", mode[32] = "", mon[16];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; fclose(f); }
        int connected = !strncmp(st, "connected", 9);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", de->d_name);
        if ((f = fopen(path, "r"))) { if (!fgets(mode, sizeof(mode), f)) mode[0] = 0; fclose(f); }
        mode[strcspn(mode, "\n")] = 0;                    /* the first listed = preferred */
        snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", de->d_name);
        edid_name(path, mon, sizeof(mon));
        if (list) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", conn);
            cJSON_AddStringToObject(o, "monitor", connected ? mon : "");
            cJSON_AddBoolToObject(o, "connected", connected);
            cJSON_AddStringToObject(o, "mode", connected ? mode : "");
            cJSON_AddItemToArray(list, o);
        }
        if (found || !connected || (*want && strcmp(want, conn))) continue;
        snprintf(name, n, "%s", conn);
        snprintf(monitor, mn, "%s", mon);
        found = 1;
    }
    closedir(d);
    return found;
}

static void local_on_exit(struct rdx_proc *p, int status, void *user)
{
    (void)p; (void)status; (void)user;                  /* local_tick notices and restarts */
}

static void local_stop(const char *why)
{
    if (!G.local.gs) return;
    fprintf(stderr, "[hostd] monitor: stopping %s on %s (%s)\n", G.local.app, G.local.conn, why);
    if (G.S.state != S_IDLE && G.S.local_join) end_session("the app on the seat's monitor ends", "", -1);
    rdx_proc_stop(G.local.gs, 4000);                    /* gamescope, Xwayland and Steam take a moment */
    rdx_proc_free(G.local.gs);
    G.local.gs = NULL;
    G.local.asleep = 0;
    unlink(G.local.sock);
}

static int local_start(cJSON *app_cfg, const char *app)
{
    struct rdx_desktop_user du;
    if (rdx_desktop_user(cfg_str("desktop_user", ""), &du)) {
        snprintf(G.local.why, sizeof(G.local.why), "no desktop user (config \"desktop_user\")");
        return -1;
    }
    static char gs[PATH_MAX], envbuf[40][512], ubuf[8][300];
    snprintf(gs, sizeof(gs), "%s", cfg_str("gamescope_bin", "/opt/rdx-gs/bin/gamescope"));
    if (access(gs, X_OK)) { snprintf(G.local.why, sizeof(G.local.why), "gamescope is not installed"); return -1; }
    int stream = !strcmp(cfg_str("picture", "monitor+stream"), "monitor+stream");
    int via_mirror = !strcmp(cfg_str("stream_via", "gamescope"), "mirror");
    snprintf(G.local.sock, sizeof(G.local.sock), "/run/rdx-gs-local-%d.sock", G.seat);
    snprintf(ubuf[0], sizeof(ubuf[0]), "HOME=%s", du.home);
    snprintf(ubuf[1], sizeof(ubuf[1]), "USER=%s", du.name);
    snprintf(ubuf[2], sizeof(ubuf[2]), "XDG_RUNTIME_DIR=/run/user/%u", (unsigned)du.uid);
    snprintf(ubuf[3], sizeof(ubuf[3]), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus", (unsigned)du.uid);
    snprintf(ubuf[4], sizeof(ubuf[4]), "LIBSEAT_BACKEND=%s", cfg_str("local_libseat", "noop"));
    snprintf(ubuf[5], sizeof(ubuf[5]), "RDX_GS_STREAM_SOCKET=%s", G.local.sock);
    char *av[200];
    int n = 0;
    av[n++] = "runuser"; av[n++] = "-u"; av[n++] = du.name; av[n++] = "--"; av[n++] = "env";
    /* gamescope starts its helpers (gamescopereaper) from PATH: its own directory first */
    static char pathenv[PATH_MAX + 64];
    snprintf(pathenv, sizeof(pathenv), "PATH=%.*s:/usr/local/bin:/usr/bin:/bin:/usr/games",
             (int)(strrchr(gs, '/') ? strrchr(gs, '/') - gs : 0), gs);
    av[n++] = pathenv;
    for (int i = 0; i < 5; i++) av[n++] = ubuf[i];
    if (stream && !via_mirror) av[n++] = ubuf[5];
    /* gamescope composites on the seat's card, whichever it was started on: by UUID (patch), and the
     * ids as the hint for a build without it (rdx_gs_src does the same for a client's gamescope) */
    static char vk_ids[40], vk_uuid[40];
    int have_ids = G.gpu_pci[0] && !gpu_vk_ids(G.gpu_pci, vk_ids, sizeof(vk_ids));
    if (G.gpu_pci[0] && !gpu_uuid_hex(G.gpu_pci, vk_uuid, sizeof(vk_uuid))) {
        snprintf(ubuf[6], sizeof(ubuf[6]), "GAMESCOPE_VK_DEVICE_UUID=%s", vk_uuid);
        av[n++] = ubuf[6];
    }
    int ne = seat_app_hook(app_cfg, envbuf, 0, 16);
    ne = rdx_mgpu_app_env(G.cfg, app_cfg, &du, envbuf, ne, 30);
    cJSON *x;
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(app_cfg, "env")) {
        if (ne == 40) break;
        snprintf(envbuf[ne++], 512, "%s=%s", x->string, x->valuestring);
    }
    for (int i = 0; i < ne && n < 120; i++) av[n++] = envbuf[i];
    if (run_pre_launch(app, envbuf, ne)) {
        snprintf(G.local.why, sizeof(G.local.why), "the seat's pre_launch command failed");
        return -1;
    }
    av[n++] = gs;
    av[n++] = "--backend"; av[n++] = "drm";
    if (have_ids) { av[n++] = "--prefer-vk-device"; av[n++] = vk_ids; }
    av[n++] = "-O"; av[n++] = G.local.conn;
    /* FreeSync / VRR on the seat's monitor (gamescope applies it only where the connector is vrr_capable);
     * a joined stream to a client with variable refresh then runs at the app's pace too */
    if (jbool(G.cfg, "monitor_vrr", 1)) av[n++] = "--adaptive-sync";
    if (stream && via_mirror) av[n++] = "--disable-layers";   /* the mirror reads the primary plane */
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(app_cfg, "gamescope_args")) {
        if (n > 170) break;
        av[n++] = x->valuestring;
    }
    av[n++] = "--";
    n = app_bus_wrap(app_cfg, av, n);
    cJSON_ArrayForEach(x, cJSON_GetObjectItem(app_cfg, "cmd")) {
        if (n == 198) break;
        av[n++] = x->valuestring;
    }
    av[n] = NULL;
    fprintf(stderr, "[hostd] monitor: start %s on %s%s%s:", app, G.local.conn, G.local.monitor[0] ? " " : "",
            G.local.monitor);
    for (char **a = av; *a; a++) fprintf(stderr, " %s", *a);
    fprintf(stderr, "\n");
    G.local.gs = rdx_proc_spawn("local_gs", av, local_on_exit, NULL);
    if (!G.local.gs) { snprintf(G.local.why, sizeof(G.local.why), "cannot start gamescope"); return -1; }
    snprintf(G.local.app, sizeof(G.local.app), "%s", app);
    G.local.started = now_s();
    G.local.why[0] = 0;
    return 0;
}

/* every 2 s and after every wake-up: start, keep or stop the home app on the monitor */
static void local_tick(void)
{
    double t = now_s();
    if (t < G.local.next_check) return;
    G.local.next_check = t + 2.0;
    if (G.local.gs && !rdx_proc_running(G.local.gs)) {
        int st = rdx_proc_exit_status(G.local.gs);
        int quick = t - G.local.started < 30;
        G.local.fails = quick ? G.local.fails + 1 : 0;
        fprintf(stderr, "[hostd] monitor: %s ended (status %d)%s\n", G.local.app, st,
                G.local.fails >= 3 ? ", 3 quick ends in a row: not restarted until the setting changes" : "");
        if (G.S.state != S_IDLE && G.S.local_join) end_session("the app on the seat's monitor ended", "", -1);
        rdx_proc_free(G.local.gs);
        G.local.gs = NULL;
        G.local.asleep = 0;
        G.local.retry_at = t + 5.0;
        if (G.local.fails >= 3) snprintf(G.local.why, sizeof(G.local.why), "gamescope ended 3 times in a row (see the logs)");
    }
    char conn[32], mon[16];
    G.local.connected = local_connector(conn, sizeof(conn), mon, sizeof(mon), NULL);
    const char *home = cfg_str("home_app", "steam");
    cJSON *app_cfg = *home ? find_app(home) : NULL;
    int want = strcmp(cfg_str("picture", "monitor+stream"), "stream") && G.local.connected && app_cfg &&
               strcmp(jstr(app_cfg, "kind", "gamescope"), "mirror");
    if (!want) {
        snprintf(G.local.why, sizeof(G.local.why), "%s",
                 !strcmp(cfg_str("picture", "monitor+stream"), "stream") ? "picture is \"stream only\""
                 : !G.local.connected ? "no monitor on the seat GPU" : "no home app (config \"home_app\")");
        local_stop(G.local.why);
        G.local.fails = 0;
        return;
    }
    if (G.local.gs && strcmp(G.local.conn, conn)) { local_stop("the monitor moved to another connector"); return; }
    snprintf(G.local.conn, sizeof(G.local.conn), "%s", conn);
    snprintf(G.local.monitor, sizeof(G.local.monitor), "%s", mon);
    if (G.local.gs || G.local.fails >= 3 || t < G.local.retry_at) return;
    /* never on a monitor that this computer's desktop (a graphical logind session on the card's
     * seat) or another DRM master shows a picture on, e.g. a desktop machine whose render GPU the
     * user picked: the picture falls back to "stream only"; checked every 15 s, logged once */
    if (t >= G.local.guard_next) {
        char card[300], gwhy[200];
        local_card(card, sizeof(card));
        G.local.guard_busy = rdx_card_free_for_local(card, gwhy, sizeof(gwhy)) != 0;
        G.local.guard_next = t + 15.0;
        if (G.local.guard_busy) {
            snprintf(G.local.why, sizeof(G.local.why), "the monitor belongs to this computer's desktop (%.90s): stream only", gwhy);
            if (!G.local.guard_logged) fprintf(stderr, "[hostd] monitor: not taken, %s; the picture falls back to stream only\n", gwhy);
        }
        G.local.guard_logged = G.local.guard_busy;
    }
    if (G.local.guard_busy) return;
    /* the home app running classic (a client session or suspended) owns it: not twice */
    if ((G.S.state != S_IDLE && !G.S.local_join && !strcmp(G.S.app, home)) || susp_find(home) >= 0) {
        snprintf(G.local.why, sizeof(G.local.why), "%s runs in a stream session", home);
        return;
    }
    local_start(app_cfg, home);
}

/* a client joins the home app on the monitor: its stream ("stream_via") */
static int spawn_local_stream(const char *fps, const char *mtu)
{
    struct session *s = &G.S;
    if (!strcmp(cfg_str("stream_via", "gamescope"), "mirror")) {
        snprintf(s->output, sizeof(s->output), "%s", G.local.conn);
        return spawn_mirror(fps, mtu);
    }
    struct rdx_desktop_user du;
    static char exe[PATH_MAX], port[16];
    const char *src = cfg_str("gs_src_bin", "");
    if (*src) snprintf(exe, sizeof(exe), "%s", src);
    else snprintf(exe, sizeof(exe), "%s/gs/rdx_gs_src", G.repo);
    if (rdx_desktop_user(cfg_str("desktop_user", ""), &du)) {
        end_session("no desktop user for the stream socket", "gs_src", -1);
        return -1;
    }
    snprintf(port, sizeof(port), "%d", s->video_port);
    snprintf(s->gs_sock, sizeof(s->gs_sock), "%s", G.local.sock);
    /* no app: rdx_gs_src connects to the client's sink, listens on the local gamescope's
     * stream socket (handed to the desktop user) and waits for its stream add-on to attach */
    char *av[40];
    int n = 0;
    av[n++] = exe;
    av[n++] = "--dev"; av[n++] = (char *)cfg_str("rdma_dev", "");
    av[n++] = "--peer"; av[n++] = s->client_ip;
    av[n++] = "--port"; av[n++] = port;
    av[n++] = "--mtu"; av[n++] = (char *)mtu;
    av[n++] = "--log-dir"; av[n++] = (char *)cfg_str("log_dir", "/var/log/rdx");
    av[n++] = "--socket"; av[n++] = G.local.sock;
    av[n++] = "--user"; av[n++] = du.name;
    if (strcmp(cfg_str("gs_buffers", "auto"), "auto")) { av[n++] = "--buffers"; av[n++] = (char *)cfg_str("gs_buffers", "auto"); }
    if (G.gpu_pci[0]) { av[n++] = "--gpu"; av[n++] = G.gpu_pci; }
    static char vrr_cap[32];
    if (s->vrr.active) {                          /* the seat's monitor has variable refresh (RDX_VRR_JOIN_VRR) */
        snprintf(vrr_cap, sizeof(vrr_cap), "%.3f", s->vrr.cap_hz);
        av[n++] = "--vrr"; av[n++] = vrr_cap;
    }
    av[n] = NULL;
    (void)fps;
    cJSON_AddStringToObject(s->params, "run_as", du.name);
    if (!spawn("gs_src", av)) { end_session("cannot start gs/rdx_gs_src", "gs_src", -1); return -1; }
    return 0;
}

/* "Monitor off/on" from the in-stream menu: gamescope's DRM sleep, the stream goes on */
static int local_sleep(int sleep)
{
    struct rdx_desktop_user du;
    if (!local_running() || rdx_desktop_user(cfg_str("desktop_user", ""), &du)) return -1;
    char cmd[PATH_MAX + 256], ctl[PATH_MAX];
    snprintf(ctl, sizeof(ctl), "%s", cfg_str("gamescope_bin", "/opt/rdx-gs/bin/gamescope"));
    char *sl = strrchr(ctl, '/');
    if (sl) snprintf(sl + 1, sizeof(ctl) - (size_t)(sl + 1 - ctl), "gamescopectl");
    /* HDMI/DP count as external screens for gamescope */
    snprintf(cmd, sizeof(cmd), "runuser -u '%s' -- env XDG_RUNTIME_DIR=/run/user/%u '%s' drm_sleep_external_screen %d "
             ">/dev/null 2>&1", du.name, (unsigned)du.uid, ctl, sleep ? 1 : 0);
    if (system(cmd) != 0) return -1;
    G.local.asleep = sleep;
    fprintf(stderr, "[hostd] monitor: %s\n", sleep ? "off (asleep), the stream goes on" : "on");
    return 0;
}

static cJSON *local_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "picture", cfg_str("picture", "monitor+stream"));
    /* what applies: a desktop owning the monitor makes it "stream" (see local_tick) */
    cJSON_AddStringToObject(o, "effective_picture", G.local.guard_busy ? "stream" : cfg_str("picture", "monitor+stream"));
    cJSON_AddStringToObject(o, "connector", G.local.conn);
    cJSON_AddStringToObject(o, "monitor", G.local.monitor);
    cJSON_AddBoolToObject(o, "connected", G.local.connected);
    cJSON_AddBoolToObject(o, "running", local_running());
    cJSON_AddStringToObject(o, "app", local_running() ? G.local.app : "");
    cJSON_AddBoolToObject(o, "asleep", G.local.asleep);
    cJSON_AddStringToObject(o, "stream_via", cfg_str("stream_via", "gamescope"));
    cJSON_AddStringToObject(o, "why_not", local_running() ? "" : G.local.why);
    char c[32], m[16];
    local_connector(c, sizeof(c), m, sizeof(m), cJSON_AddArrayToObject(o, "connectors"));
    return o;
}

/* ---------------- "Start here on this PC": a window on this computer's own desktop ----------------
 * The app runs as for a client (rdx_gs_src + gamescope rdx backend on the seat GPU), but the
 * sink is p0b/rdx_sink_win --local in a window on the desktop of the desktop user: it imports
 * rdx_gs_src's host slots (no copy, the iGPU shares the RAM) and its vsync leads the clock
 * (gs/rdx_local_proto.h). The window closing, "Send to the client" or a client starting the same
 * app suspend rdx_gs_src (the app keeps running); "Back to this PC" resumes it into a new window,
 * with RECONFIG when the window's mode differs from the client's. The window's size is the
 * desktop monitor's mode (config "local_output", default the first output); F11 = fullscreen. */

/* Xwayland's display of user uid's Wayland session: Xwayland's own command line (":1 ... -auth FILE")
 * when it runs, else the DISPLAY most of the user's processes with the newest .mutter-Xwaylandauth
 * carry (mutter-x11-frames and ibus-x11 use a second display, :2; processes of an older X11 session
 * still have :0 with gdm's Xauthority). 0 = found */
static int rdx_xwayland_env(uid_t uid, char *disp, size_t dn, char *auth, size_t an)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    char cand[16][64], cauth[256] = "";
    int cnt[16], nc = 0, xw = 0;
    time_t best_t = 0;
    while (!xw && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], comm[64] = "";
        struct stat pst, ast;
        snprintf(path, sizeof(path), "/proc/%.200s", de->d_name);
        if (stat(path, &pst) || pst.st_uid != uid) continue;
        snprintf(path, sizeof(path), "/proc/%.200s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(comm, sizeof(comm), f)) comm[0] = 0; fclose(f); }
        comm[strcspn(comm, "\n")] = 0;
        static char buf[65536];
        if (!strcmp(comm, "Xwayland")) {          /* argv: Xwayland :1 -rootless ... -auth FILE ... */
            snprintf(path, sizeof(path), "/proc/%.200s/cmdline", de->d_name);
            if (!(f = fopen(path, "r"))) continue;
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            const char *dsp = NULL, *au = NULL, *prev = "";
            for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
                if (buf[i] == ':' && !dsp) dsp = buf + i;
                if (!strcmp(prev, "-auth")) au = buf + i;
                prev = buf + i;
            }
            if (dsp && au && !stat(au, &ast)) {
                snprintf(disp, dn, "%s", dsp);
                snprintf(auth, an, "%s", au);
                xw = 1;
            }
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%.200s/environ", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        const char *dp = NULL, *xa = NULL;
        for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
            if (!strncmp(buf + i, "DISPLAY=", 8)) dp = buf + i + 8;
            else if (!strncmp(buf + i, "XAUTHORITY=", 11)) xa = buf + i + 11;
        }
        if (!dp || !xa || !strstr(xa, ".mutter-Xwaylandauth") || stat(xa, &ast) || ast.st_mtime < best_t) continue;
        if (ast.st_mtime > best_t) { best_t = ast.st_mtime; nc = 0; snprintf(cauth, sizeof(cauth), "%s", xa); }
        int k = 0;
        while (k < nc && strcmp(cand[k], dp)) k++;
        if (k == nc && nc < 16) { snprintf(cand[nc], sizeof(cand[0]), "%s", dp); cnt[nc++] = 0; }
        if (k < nc) cnt[k]++;
    }
    closedir(d);
    if (xw) return 0;
    int b = -1;
    for (int k = 0; k < nc; k++) if (b < 0 || cnt[k] > cnt[b]) b = k;
    if (b < 0) return -1;
    snprintf(disp, dn, "%s", cand[b]);
    snprintf(auth, an, "%s", cauth);
    return 0;
}

/* DISPLAY and XAUTHORITY of the desktop user's graphical session, from one of its processes (on
 * Wayland Xwayland's, see p0b/rdx_xwin.h rdx_x_desktop_env) */
static int desktop_x_env(uid_t uid, char *display, size_t dn, char *xauth, size_t xn)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    int found = 0;
    display[0] = xauth[0] = 0;
    while (!found && (de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300], comm[64] = "";
        struct stat st;
        snprintf(path, sizeof(path), "/proc/%.200s", de->d_name);
        if (stat(path, &st) || st.st_uid != uid) continue;
        snprintf(path, sizeof(path), "/proc/%.200s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof(comm), f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        /* the shell of the desktop carries the session's DISPLAY / XAUTHORITY */
        if (strcmp(comm, "gnome-shell") && strcmp(comm, "plasmashell") && strcmp(comm, "xfce4-session") &&
            strcmp(comm, "cinnamon") && strcmp(comm, "mate-session")) continue;
        snprintf(path, sizeof(path), "/proc/%.200s/environ", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        char env[65536];
        size_t n = fread(env, 1, sizeof(env) - 1, f);
        fclose(f);
        env[n] = 0;
        for (size_t i = 0; i < n; i += strlen(env + i) + 1) {
            if (!strncmp(env + i, "DISPLAY=", 8)) snprintf(display, dn, "%s", env + i + 8);
            else if (!strncmp(env + i, "XAUTHORITY=", 11)) snprintf(xauth, xn, "%s", env + i + 11);
        }
        found = display[0] != 0;
    }
    closedir(d);
    if (!found && !rdx_xwayland_env(uid, display, dn, xauth, xn)) found = 1;   /* Wayland: Xwayland's */
    return found ? 0 : -1;
}

static const char *local_win_sock(void) { return G.S.client_ip + 6; }   /* after "local:" */

/* "Start here on this PC" / "Back to this PC" for app; 0 = starting, else why in err */
static int begin_local(const char *app, char *err, size_t en)
{
    struct session *s = &G.S;
    cJSON *app_cfg = find_app(app);
    if (!app_cfg || !strcmp(jstr(app_cfg, "kind", "gamescope"), "mirror")) {
        snprintf(err, en, "unknown app or not a gamescope app");
        return -1;
    }
    if (s->state != S_IDLE && !strcmp(s->app, app) && !s->local_window)
        end_session_ex("back to this PC", "", -1, 1);           /* the client's stream: suspend, resume here */
    if (s->state != S_IDLE) { snprintf(err, en, "%s runs already", s->app); return -1; }
    struct rdx_desktop_user du;
    char display[64], xauth[PATH_MAX];
    if (rdx_desktop_user(cfg_str("desktop_user", ""), &du) ||
        desktop_x_env(du.uid, display, sizeof(display), xauth, sizeof(xauth))) {
        snprintf(err, en, "no graphical desktop session of the desktop user on this computer");
        return -1;
    }
    refresh_outputs(0);
    const char *want = cfg_str("local_output", "");
    int oi = -1;
    for (int i = 0; i < G.nouts && oi < 0; i++) if (!*want || !strcmp(G.outs[i].name, want)) oi = i;
    if (oi < 0) { snprintf(err, en, "no monitor of the desktop found (config \"local_output\")"); return -1; }
    /* the app in the background resumes here (a window never tears); another app keeps the others running
     * (suspend_multi, up to APPS_MAX), or, without it, closes the one in the background */
    int k = susp_find(app);
    int resume = k >= 0 && rdx_proc_running(G.susp[k].gs) && !G.susp[k].tearing;
    if (k >= 0 && !resume) quit_suspended_at(k, G.susp[k].tearing ? "it ran with tearing" : "it ended");
    if (!resume && !multi_on() && G.nsusp) quit_suspended_all("another app starts here (one app in the background)");
    if (!resume && G.nsusp + 1 > APPS_MAX) {
        snprintf(err, en, "%d apps run on this seat already: quit one of them first", G.nsusp);
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->resume = resume;
    s->local_window = 1;
    rdx_vrr_decide(NULL, 0, RDX_VRR_LOCAL_WINDOW, 0, RDX_VRR_CAP_FRAC_DEFAULT, &s->vrr);
    s->input_private = 1;                             /* the window's devices: gs/rdx_localinput.c */
    input_group_member();
    s->app_cfg = cJSON_Duplicate(app_cfg, 1);
    s->t_start = now_s();
    s->deadline = s->t_start + START_TIMEOUT_S;
    snprintf(s->app, sizeof(s->app), "%s", app);
    snprintf(s->client_ip, sizeof(s->client_ip), "local:/run/user/%u/rdx-local-%d.sock", (unsigned)du.uid, G.seat);
    s->video_port = cfg_int("video_port", 18650);
    cJSON *d = cfg_defaults(), *p = s->params = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "app", app);
    cJSON_AddStringToObject(p, "output", "");
    cJSON_AddStringToObject(p, "client_ip", s->client_ip);
    cJSON_AddNumberToObject(p, "fps", jnum(d, "fps", 0));
    cJSON_AddStringToObject(p, "pacing", "latency");
    cJSON_AddNumberToObject(p, "match_hz", 0);
    cJSON_AddBoolToObject(p, "cursor", 1);
    cJSON_AddBoolToObject(p, "tearing", 0);
    cJSON_AddStringToObject(p, "gpu_prio", jstr(d, "gpu_prio", "high"));
    cJSON_AddBoolToObject(p, "audio", 0);             /* the app's sound plays here already */
    cJSON_AddBoolToObject(p, "input", 0);             /* gamescope reads this computer's devices */
    cJSON_AddBoolToObject(p, "usb", 0);
    cJSON_AddBoolToObject(p, "local_window", 1);
    /* frames have the window's size (it starts at --win, resizes follow); the refresh is the monitor's */
    cJSON *cm = cJSON_AddObjectToObject(p, "client_mode");
    cJSON_AddNumberToObject(cm, "w", 1280);
    cJSON_AddNumberToObject(cm, "h", 720);
    cJSON_AddNumberToObject(cm, "hz", G.outs[oi].hz_exact > 0 ? G.outs[oi].hz_exact : G.outs[oi].hz);
    if (resume) cJSON_AddBoolToObject(p, "resumed", 1);
    s->state = S_STARTING;

    static char exe[PATH_MAX], sock[sizeof(G.S.client_ip)], res[32], hz[32], title[160], denv[80], xenv[PATH_MAX + 16];
    snprintf(exe, sizeof(exe), "%s/p0b/rdx_sink_win", G.repo);
    snprintf(sock, sizeof(sock), "%.63s", local_win_sock());
    snprintf(res, sizeof(res), "%dx%d", G.outs[oi].w, G.outs[oi].h);
    snprintf(hz, sizeof(hz), "%.4f", G.outs[oi].hz_exact > 0 ? G.outs[oi].hz_exact : (double)G.outs[oi].hz);
    snprintf(title, sizeof(title), "%s - rdma-display (F11: full screen)", jstr(app_cfg, "name", app));
    snprintf(denv, sizeof(denv), "DISPLAY=%s", display);
    snprintf(xenv, sizeof(xenv), "XAUTHORITY=%s", xauth);
    unlink(sock);
    char *av[] = {"runuser", "-u", du.name, "--", "env", denv, xenv, exe, "--local", sock, "--res", res, "--hz", hz,
                  "--win", "1280x720", "--title", title, NULL};
    if (!xauth[0]) av[6] = "XAUTHORITY=";
    fprintf(stderr, "[hostd] %s here on this PC (%s, %s @ %s Hz)%s\n", app, G.outs[oi].name, res, hz,
            resume ? ", resuming the paused app" : "");
    if (!spawn("local_win", av)) { end_session("cannot start the local window", "local_win", -1); snprintf(err, en, "cannot start p0b/rdx_sink_win"); return -1; }
    return 0;
}

/* "Send to the client": the window closes, the app keeps running (suspended) and every connected
 * client is offered it (app.offer: a client resumes it) */
static int send_to_client(char *err, size_t en)
{
    struct session *s = &G.S;
    if (s->state == S_IDLE || !s->local_window) { snprintf(err, en, "nothing runs here on this PC"); return -1; }
    char app[64];
    snprintf(app, sizeof(app), "%s", s->app);
    end_session_ex("sent to the client", "", -1, 1);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "event", "app.offer");
    cJSON_AddStringToObject(e, "app", app);
    for (int i = 0; i < MAX_CLIENTS; i++) if (G.clients[i]) rdx_ctl_send(G.clients[i], e);
    cJSON_Delete(e);
    return 0;
}

/* the link's level changed (common/rdx_link.h): every connected client hears it */
static void link_broadcast(void)
{
    struct rdx_link_info li;
    rdx_link_get(&li);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "event", "link.health");
    cJSON_AddItemToObject(e, "link", rdx_link_json(&li));
    for (int i = 0; i < MAX_CLIENTS; i++) if (G.clients[i]) rdx_ctl_send(G.clients[i], e);
    cJSON_Delete(e);
}

/* phase 2: input_rx listens (or no input wanted) -> start the senders and confirm */
static void start_senders(void)
{
    struct session *s = &G.S;
    cJSON *p = s->params;
    char mtu[16], fps[16];
    snprintf(mtu, sizeof(mtu), "%d", cfg_int("mtu", 4096));

    snprintf(fps, sizeof(fps), "%g", jnum(p, "fps", 0));
    char *av[32];
    int n = 0;
    if (s->local_join) { if (spawn_local_stream(fps, mtu)) return; }
    else if (s->app_cfg) { if (spawn_gamescope(fps, mtu)) return; }
    else if (spawn_mirror(fps, mtu)) return;

    char node[256] = "";
    if (jbool(p, "audio", 0)) {
        snprintf(node, sizeof(node), "%s", jstr(cfg_defaults(), "audio_source", ""));
        if (!*node) {
            /* the sink name only proves that a default exists now; rdx_audio_tx gets "default" and follows
             * the default sink from then on (a fixed name stayed silent when that sink disappeared) */
            default_sink(node, sizeof(node));
            if (*node) snprintf(node, sizeof(node), "default");
        }
        /* never fall back to rdx_audio_tx's test signal in a real session: it could be audible */
        if (!*node) fprintf(stderr, "[hostd] audio: no default sink found (PipeWire of the desktop user?), no audio\n");
    }
    if (*node) {
        char aexe[PATH_MAX], aport[16];
        snprintf(aexe, sizeof(aexe), "%s/p5/rdx_audio_tx", G.repo);
        snprintf(aport, sizeof(aport), "%d", s->audio_port);
        n = 0;
        av[n++] = aexe;
        av[n++] = "--dev"; av[n++] = (char *)cfg_str("rdma_dev", "");
        av[n++] = "--peer"; av[n++] = s->client_ip;
        av[n++] = "--port"; av[n++] = aport;
        av[n++] = "--mtu"; av[n++] = mtu;
        av[n++] = "--seconds"; av[n++] = "0";
        av[n++] = "--capture"; av[n++] = node;
        av[n] = NULL;
        if (!spawn("audio_tx", av)) { end_session("cannot start rdx_audio_tx", "audio_tx", -1); return; }
    }
    if (jbool(p, "usb", 0)) {
        if (have_bin("usb/rdx_usb_host")) {
            char uexe[PATH_MAX], uport[16];
            snprintf(uexe, sizeof(uexe), "%s/usb/rdx_usb_host", G.repo);
            snprintf(uport, sizeof(uport), "%d", s->usb_port);
            char *uv[] = {uexe, "--bind", (char *)cfg_str("link_ip", ""), "--port", uport,
                          "--dev", (char *)cfg_str("rdma_dev", ""), "--mtu", mtu,
                          s->input_wait_grab ? "--wait-grab" : NULL, s->input_private ? "--private" : NULL, NULL};
            if (s->input_private) input_group_member();
            s->usb_host = spawn("usb_host", uv);
            if (!s->usb_host) { end_session("cannot start rdx_usb_host", "usb_host", -1); return; }
        } else fprintf(stderr, "[hostd] usb requested, but usb/rdx_usb_host is not built\n");
    }
    if (jbool(p, "mic", 0)) {
        /* the virtual microphone; it runs as root on the desktop user's PipeWire, like audio_tx */
        static char mexe[PATH_MAX], mport[16], mname[128];
        snprintf(mexe, sizeof(mexe), "%s/p5/rdx_audio_rx", G.repo);
        snprintf(mport, sizeof(mport), "%d", s->mic_port);
        snprintf(mname, sizeof(mname), "%s", jstr(cfg_defaults(), "mic_name", "RDMA Display Mic"));
        char *mv[] = {mexe, "--dev", (char *)cfg_str("rdma_dev", ""), "--bind", (char *)cfg_str("link_ip", ""),
                      "--port", mport, "--mtu", mtu, "--mic", mname, NULL};
        s->mic_rx = spawn("mic_rx", mv);
        if (!s->mic_rx) fprintf(stderr, "[hostd] cannot start the microphone player, no microphone\n");
    }
    if (jbool(p, "clipboard", 0)) {
        /* the clipboard on the app's Xwayland (below its rdx_gs_src / the seat's gamescope), for this client only */
        static char cexe[PATH_MAX], cport[16], cpid[16];
        snprintf(cexe, sizeof(cexe), "%s/clip/rdx_clip", G.repo);
        snprintf(cport, sizeof(cport), "%d", s->clip_port);
        struct rdx_proc *gs = NULL;
        for (int i = 0; i < s->nprocs && !s->local_join; i++)
            if (!strcmp(rdx_proc_name(s->procs[i]), "gs_src")) gs = s->procs[i];
        if (s->local_join) gs = G.local.gs;
        snprintf(cpid, sizeof(cpid), "%d", gs ? rdx_proc_pid(gs) : 0);
        char *cv[] = {cexe, "--listen", (char *)cfg_str("link_ip", ""), "--port", cport, "--peer", s->client_ip,
                      "--primary", gs ? "--x-of-pid" : "--desktop", gs ? cpid : NULL, NULL};
        s->clip = spawn("clip", cv);
        if (!s->clip) fprintf(stderr, "[hostd] cannot start the clipboard helper, no clipboard\n");
    }
    s->senders_started = 1;
}

/* everything listens: the session runs, tell the client where to connect */
static void confirm_start(void)
{
    struct session *s = &G.S;
    s->state = S_RUNNING;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "input_port", jbool(s->params, "input", 1) ? s->input_port : 0);
    cJSON_AddNumberToObject(r, "usb_port", s->usb_host ? s->usb_port : 0);
    int mic = s->mic_rx && rdx_proc_running(s->mic_rx) && rdx_proc_output_has(s->mic_rx, "listening");
    cJSON_AddNumberToObject(r, "mic_port", mic ? s->mic_port : 0);
    cJSON_AddNumberToObject(r, "clip_port", s->clip && rdx_proc_running(s->clip) ? s->clip_port : 0);
    /* joined the app on the seat's own monitor: the client's menu offers "Monitor off / on" */
    if (s->local_join) cJSON_AddStringToObject(r, "monitor", G.local.monitor[0] ? G.local.monitor : G.local.conn);
    cJSON_AddItemToObject(r, "vrr", rdx_vrr_json(&s->vrr));      /* what the host does with the client's display */
    cJSON_AddStringToObject(r, "app", s->app);
    cJSON_AddBoolToObject(r, "resumed", s->resume);
    /* the client asked for another entry of the same single-instance program: this one runs */
    if (s->asked_app[0]) cJSON_AddStringToObject(r, "resumed_as", s->app);
    if (s->vram_warn) cJSON_AddItemToObject(r, "vram_warning", cJSON_Duplicate(s->vram_warn, 1));
    ctl_reply(s->owner, s->reply_id, 1, NULL, r);
    fprintf(stderr, "[hostd] session running (%.3f s to start)\n", now_s() - s->t_start);
}

#define APP_EARLY_END_S 10.0            /* an app that ends this soon after the start gets a notice */
#define APP_NOTICE_S    6.0             /* how long the client shows it (rdx_gs_src keeps the sink) */

static struct rdx_proc *sess_proc(const char *name)
{
    for (int i = 0; i < G.S.nprocs; i++)
        if (!strcmp(rdx_proc_name(G.S.procs[i]), name)) return G.S.procs[i];
    return NULL;
}

/* rdx_gs_src logged "stopping: gamescope ended": the app quit by itself. Right after the start
 * that is mostly a single-instance app that found its own instance on the desktop and handed the
 * start over (Lutris on the session D-Bus: "launch: Primary child shut down!"):
 * look for that instance (only reading /proc, nothing on the desktop is touched) and tell the
 * client why, as a notice its start placeholder shows. Never a suspend, never "stopped by the
 * client": the client's session.stop that follows is the consequence, not the cause. */
static void app_ended_check(void)
{
    struct session *s = &G.S;
    if ((s->state != S_RUNNING && s->state != S_STARTING) || s->app_ended || !s->app_cfg || s->local_join) return;
    struct rdx_proc *gs = sess_proc("gs_src");
    if (!gs || !rdx_proc_output_has(gs, "[gs] stopping: gamescope ended")) return;
    s->app_ended = 1;
    double lived = now_s() - s->t_start;
    const char *name = jstr(s->app_cfg, "name", s->app);
    if (lived >= APP_EARLY_END_S || s->state != S_RUNNING) {
        snprintf(s->app_reason, sizeof(s->app_reason), "%s ended (after %.0f s)", name, lived);
        fprintf(stderr, "[hostd] %s ended by itself after %.1f s\n", s->app, lived);
        end_session_ex(s->app_reason, "gs_src", 0, 0);
        return;
    }
    char prog[256], what[256] = "";
    struct rdx_desktop_user du;
    int pid = 0;
    rdx_app_program(cJSON_GetObjectItem(s->app_cfg, "cmd"), prog, sizeof(prog));
    if (*prog && !rdx_desktop_user(cfg_str("desktop_user", ""), &du))
        pid = rdx_desktop_instance("/proc", du.uid, prog, what, sizeof(what));
    if (pid)
        snprintf(s->app_reason, sizeof(s->app_reason),
                 "%s is already open on this PC \xe2\x80\x93 close it there and start again", name);
    else
        snprintf(s->app_reason, sizeof(s->app_reason),
                 "%s ended right after its start (%.1f s) \xe2\x80\x93 see the host's log", name, lived);
    fprintf(stderr, "[hostd] %s ended by itself %.1f s after the start%s%s\n", s->app, lived,
            pid ? "; already open on the desktop: " : "; no instance of it on the desktop", what);
    if (s->owner) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "event", "session.notice");
        cJSON_AddStringToObject(e, "text", s->app_reason);
        cJSON_AddStringToObject(e, "app", name);
        cJSON_AddBoolToObject(e, "already_open", pid > 0);
        rdx_ctl_send(s->owner, e);
        cJSON_Delete(e);
    }
    s->notice_end = now_s() + APP_NOTICE_S;
}

/* advance the start state machine; called after every wake-up */
static void session_tick(void)
{
    struct session *s = &G.S;
    for (int i = G.nsusp - 1; i >= 0; i--) {
        if (rdx_proc_running(G.susp[i].gs)) continue;
        fprintf(stderr, "[hostd] the suspended app %s ended by itself (%d app(s) left in the background)\n",
                G.susp[i].app, G.nsusp - 1);
        rdx_proc_free(G.susp[i].gs);
        susp_remove(i);
    }
    app_ended_check();
    if (s->state == S_IDLE) return;
    if (s->app_ended && (s->pending_end || (s->notice_end > 0 && now_s() >= s->notice_end))) {
        end_session_ex(s->app_reason, "gs_src", 0, 0);
        return;
    }
    if (s->pending_end) {
        char reason[128], proc[32];
        snprintf(reason, sizeof(reason), "%s", s->end_reason);
        snprintf(proc, sizeof(proc), "%s", s->end_proc);
        /* input/audio/usb ending = the client side went away: keep the game; gs_src ending = not */
        end_session_ex(reason, proc, s->end_status, strcmp(proc, "gs_src") != 0);
        return;
    }
    if (s->state != S_STARTING) return;
    if (!s->senders_started && s->local_window && access(local_win_sock(), F_OK)) {
        if (now_s() > s->deadline) end_session("start timeout: the local window did not open", "local_win", -1);
        return;
    }
    if (!s->senders_started) {
        if (!s->input_rx || rdx_proc_output_has(s->input_rx, "waiting for source") ||
            rdx_proc_output_has(s->input_rx, "listening"))
            start_senders();
        else if (now_s() > s->deadline) {
            end_session("start timeout: rdx_input_rx did not listen", "input_rx", -1);
            return;
        }
    }
    if (s->state != S_STARTING || !s->senders_started) return;
    int usb_ok = !s->usb_host || rdx_proc_output_has(s->usb_host, "listening");
    int mic_ok = !s->mic_rx || !rdx_proc_running(s->mic_rx) || rdx_proc_output_has(s->mic_rx, "listening");
    if (usb_ok && mic_ok)
        confirm_start();
    else if (now_s() > s->deadline && !usb_ok)
        end_session("start timeout: rdx_usb_host did not listen", "usb_host", -1);
    else if (now_s() > s->deadline) {
        fprintf(stderr, "[hostd] start timeout: the microphone player did not listen, no microphone\n");
        rdx_proc_stop(s->mic_rx, STOP_GRACE_MS);
        confirm_start();
    }
}

/* ---------------- JSON views ---------------- */

static cJSON *procs_json(struct rdx_proc **pv, int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", rdx_proc_name(pv[i]));
        cJSON_AddNumberToObject(o, "pid", rdx_proc_pid(pv[i]));
        cJSON_AddBoolToObject(o, "running", rdx_proc_running(pv[i]));
        cJSON_AddNumberToObject(o, "cpu_pct", (int)(rdx_proc_cpu_pct(pv[i]) * 10) / 10.0);
        cJSON_AddNumberToObject(o, "uptime_s", (int)rdx_proc_uptime_s(pv[i]));
        int st = rdx_proc_exit_status(pv[i]);
        if (st >= 0) cJSON_AddNumberToObject(o, "exit", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

static cJSON *outputs_json(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < G.nouts; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", G.outs[i].name);
        cJSON_AddNumberToObject(o, "w", G.outs[i].w);
        cJSON_AddNumberToObject(o, "h", G.outs[i].h);
        cJSON_AddNumberToObject(o, "hz", G.outs[i].hz);
        cJSON_AddNumberToObject(o, "hz_exact", G.outs[i].hz_exact);
        cJSON_AddNumberToObject(o, "x", G.outs[i].x);
        cJSON_AddNumberToObject(o, "y", G.outs[i].y);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

static cJSON *apps_json(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < G.nouts; i++) {
        char id[64], name[96];
        snprintf(id, sizeof(id), "mirror:%s", G.outs[i].name);
        char detail[64];
        snprintf(name, sizeof(name), "Whole screen %s", G.outs[i].name);
        snprintf(detail, sizeof(detail), "%dx%d, %d Hz", G.outs[i].w, G.outs[i].h, G.outs[i].hz);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", id);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddStringToObject(o, "detail", detail);
        cJSON_AddStringToObject(o, "kind", "mirror");
        cJSON_AddStringToObject(o, "output", G.outs[i].name);
        cJSON_AddStringToObject(o, "icon", "/api/icon?name=rdx-screen");
        cJSON_AddItemToArray(a, o);
    }
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(G.cfg, "apps")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", jstr(c, "id", ""));
        cJSON_AddStringToObject(o, "name", jstr(c, "name", ""));
        cJSON_AddStringToObject(o, "kind", jstr(c, "kind", "gamescope"));
        cJSON_AddStringToObject(o, "detail", "Program, runs in its own screen at your client's resolution");
        char icon[128];
        snprintf(icon, sizeof(icon), "/api/apps/%s/icon", jstr(c, "id", ""));
        cJSON_AddStringToObject(o, "icon", icon);
        if (*jstr(c, "note", "")) cJSON_AddStringToObject(o, "note", jstr(c, "note", ""));
        int ss = rdx_mgpu_sink_slots(G.cfg, c);   /* frame generation: the client starts its sink with these */
        if (ss) cJSON_AddNumberToObject(o, "sink_slots", ss);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

static int gs_vrr_line(char *out, size_t n);

static cJSON *session_json(void)
{
    struct session *s = &G.S;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_name[s->state]);
    if (s->state != S_IDLE) {
        cJSON_AddStringToObject(o, "app", s->app);
        cJSON_AddStringToObject(o, "client", s->client_ip);
        cJSON_AddBoolToObject(o, "local_window", s->local_window);
        cJSON_AddNumberToObject(o, "since_s", (int)(now_s() - s->t_start));
        if (s->params) cJSON_AddItemToObject(o, "params", cJSON_Duplicate(s->params, 1));
        cJSON_AddItemToObject(o, "procs", procs_json(s->procs, s->nprocs));
        cJSON *v = rdx_vrr_json(&s->vrr);
        char line[256];
        if (gs_vrr_line(line, sizeof(line))) cJSON_AddStringToObject(v, "source", line);
        else cJSON_AddNullToObject(v, "source");
        cJSON_AddItemToObject(o, "vrr", v);
    } else cJSON_AddItemToObject(o, "procs", cJSON_CreateArray());
    cJSON *last = cJSON_AddObjectToObject(o, "last");
    cJSON_AddStringToObject(last, "reason", G.last_end);
    cJSON_AddItemToObject(last, "procs", procs_json(G.last, G.nlast));
    return o;
}

/* host/rdx_mgpu.c borrows the config, its lock and the seat's GPU */
static cJSON *mgpu_cfg(void) { return G.cfg; }
static const char *mgpu_seat_gpu(void) { return G.gpu_pci; }

/* the seat's GPU: config "gpu" (PCI address), else the first usable NVIDIA GPU, else the first
 * usable GPU of any vendor. The list holds every graphics card of the host (rdx_mgpu_enumerate:
 * nvidia-smi plus every DRM render node). In a seat container nvidia-smi lists every GPU of the
 * machine but only the seat's /dev/nvidiaN is there: "usable" skips the others; for AMD / Intel
 * the render node must open. */
static void resolve_gpu(void)
{
    const char *want = cfg_str("gpu", "");
    struct rdx_gpu_info all[16];
    int na = rdx_mgpu_enumerate(all, 16), ng = 0, pick = -1;
    __typeof__(G.gpus[0]) gl[8];
    for (int i = 0; i < na && ng < 8; i++, ng++) {
        snprintf(gl[ng].pci, sizeof(gl[ng].pci), "%.15s", all[i].pci);
        snprintf(gl[ng].name, sizeof(gl[ng].name), "%s", all[i].name);
        snprintf(gl[ng].vendor, sizeof(gl[ng].vendor), "%s", all[i].vendor);
        gl[ng].usable = all[i].usable;
        if (*want && !strcasecmp(want, all[i].pci)) pick = ng;
    }
    for (int i = 0; !*want && pick < 0 && i < ng; i++)
        if (gl[i].usable && !strcmp(gl[i].vendor, "NVIDIA")) pick = i;
    for (int i = 0; !*want && pick < 0 && i < ng; i++)
        if (gl[i].usable) pick = i;
    char pci[32] = "", gname[96] = "";
    if (pick >= 0) {
        snprintf(pci, sizeof(pci), "%s", gl[pick].pci);
        snprintf(gname, sizeof(gname), "%s", gl[pick].name);
    } else if (*want) {
        fprintf(stderr, "[hostd] gpu %s not found, passing it on as given\n", want);
        snprintf(pci, sizeof(pci), "%s", want);
    }
    /* the web UI may change "gpu" while other threads read these */
    pthread_mutex_lock(&G.mu);
    snprintf(G.gpu_pci, sizeof(G.gpu_pci), "%s", pci);
    snprintf(G.gpu_name, sizeof(G.gpu_name), "%s", gname);
    memcpy(G.gpus, gl, sizeof(gl[0]) * (size_t)ng);
    G.ngpus = ng;
    pthread_mutex_unlock(&G.mu);
    if (G.gpu_pci[0]) fprintf(stderr, "[hostd] seat %d: GPU %s (%s)\n", G.seat, G.gpu_pci, G.gpu_name[0] ? G.gpu_name : "?");
}

/* "RDMA Display - Seat 1" for a seat (it may start on any card: the card is no name, hello "gpu_name"
 * says which one it has), "RDMA Display - 5090" for a single host with its own "gpu", its "seat_name"
 * when set, "" for the classic single host */
static void seat_name(char *out, size_t n)
{
    const char *nm = cfg_str("seat_name", "");
    char shortn[96] = "";
    if (!*nm && G.seat > 0) { snprintf(shortn, sizeof(shortn), "Seat %d", G.seat); nm = shortn; }
    if (!*nm && *cfg_str("gpu", "") && G.gpu_name[0]) {
        const char *g = G.gpu_name, *sp;
        while ((sp = strchr(g, ' ')) && strpbrk(sp + 1, "0123456789")) g = sp + 1;   /* "NVIDIA GeForce RTX 5090" -> "5090" */
        snprintf(shortn, sizeof(shortn), "%s", g);
        nm = shortn;
    }
    if (*nm) snprintf(out, n, "RDMA Display - %s", nm);
    else out[0] = 0;
}

static cJSON *susp_entry_json(int i)
{
    struct susp_app *a = &G.susp[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "app", a->app);
    cJSON_AddStringToObject(o, "name", jstr(find_app(a->app), "name", a->app));
    cJSON_AddItemToObject(o, "mode", cJSON_Duplicate(a->mode, 1));
    cJSON_AddNumberToObject(o, "since_s", (int)(now_s() - a->since));
    cJSON_AddBoolToObject(o, "tearing", a->tearing);
    if (a->vram_mib >= 0) cJSON_AddNumberToObject(o, "vram_mib", a->vram_mib);
    return o;
}

/* for older clients: the newest app in the background, null = none */
static cJSON *suspended_json(void)
{
    int i = susp_newest();
    return i < 0 ? cJSON_CreateNull() : susp_entry_json(i);
}

/* every app in the background, oldest first */
static cJSON *suspended_list_json(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < G.nsusp; i++) cJSON_AddItemToArray(a, susp_entry_json(i));
    return a;
}

/* hello / status / state: the list, the newest one for older clients, and what the seat allows */
static void add_suspended(cJSON *o)
{
    cJSON_AddItemToObject(o, "suspended", suspended_json());
    cJSON_AddItemToObject(o, "suspended_apps", suspended_list_json());
    cJSON_AddBoolToObject(o, "suspend_multi", multi_on());
    cJSON_AddNumberToObject(o, "apps_max", multi_on() ? APPS_MAX : 1);
}

static cJSON *hello_json(void)
{
    refresh_outputs(0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host_name", cfg_str("name", G.host_name));
    cJSON_AddNumberToObject(o, "version", RDX_CTL_VERSION);
    cJSON_AddStringToObject(o, "link_ip", cfg_str("link_ip", ""));
    cJSON_AddItemToObject(o, "outputs", outputs_json());
    cJSON_AddItemToObject(o, "apps", apps_json());
    cJSON_AddBoolToObject(o, "audio", 1);
    cJSON_AddBoolToObject(o, "usb", have_bin("usb/rdx_usb_host"));
    cJSON_AddBoolToObject(o, "mic", jbool(cfg_defaults(), "mic", 1) && have_bin("p5/rdx_audio_rx"));
    cJSON_AddBoolToObject(o, "clipboard", jbool(cfg_defaults(), "clipboard", 1) && have_bin("clip/rdx_clip"));
    cJSON_AddNumberToObject(o, "web_port", cfg_int("web_port", 47990));   /* icons: http://<link_ip>:<web_port><icon> */
    /* sink slots (rdx_pace_slots, common/rdx_ctl.h): the client applies the rule at its refresh with
     * this seat's last settled lead; without one yet it takes RDX_LEAD_DEFAULT_MS */
    cJSON_AddBoolToObject(o, "sink_slots_auto", jbool(cfg_defaults(), "sink_slots_auto", 1));
    char lp[64];
    lead_path(lp, sizeof(lp));
    FILE *lf = fopen(lp, "r");
    double lead = 0;
    if (lf) {
        if (fscanf(lf, "%lf", &lead) != 1) lead = 0;
        fclose(lf);
    }
    if (lead > 0) cJSON_AddNumberToObject(o, "stream_lead_ms", lead);
    add_suspended(o);
    cJSON_AddItemToObject(o, "local", local_json());
    struct rdx_link_info li;
    rdx_link_get(&li);
    cJSON_AddItemToObject(o, "link", rdx_link_json(&li));      /* port state and module temperature */
    char sn[160];
    seat_name(sn, sizeof(sn));
    cJSON_AddNumberToObject(o, "seat", G.seat);
    cJSON_AddStringToObject(o, "seat_name", sn);
    cJSON_AddStringToObject(o, "gpu", G.gpu_pci);
    cJSON_AddStringToObject(o, "gpu_name", G.gpu_name);
    cJSON *ga = cJSON_AddArrayToObject(o, "gpus");
    for (int i = 0; i < G.ngpus; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "pci", G.gpus[i].pci);
        cJSON_AddStringToObject(g, "name", G.gpus[i].name);
        cJSON_AddStringToObject(g, "vendor", G.gpus[i].vendor);
        cJSON_AddBoolToObject(g, "usable", G.gpus[i].usable);
        cJSON_AddItemToArray(ga, g);
    }
    return o;
}

static cJSON *state_json(void)
{
    cJSON *o = hello_json();
    cJSON_AddNumberToObject(o, "uptime_s", (int)(now_s() - G.t0));
    cJSON *cl = cJSON_AddArrayToObject(o, "clients");
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (G.clients[i]) cJSON_AddItemToArray(cl, cJSON_CreateString(rdx_ctl_peer(G.clients[i])));
    cJSON_AddBoolToObject(o, "ctl_listening", G.lfd >= 0);
    cJSON_AddItemToObject(o, "session", session_json());
    cJSON_AddItemToObject(o, "config", cJSON_Duplicate(G.cfg, 1));
    return o;
}

/* ---------------- control channel ---------------- */

static void client_gone(int i)
{
    struct rdx_ctl *c = G.clients[i];
    fprintf(stderr, "[hostd] client %s disconnected\n", rdx_ctl_peer(c));
    epoll_ctl(G.ep, EPOLL_CTL_DEL, rdx_ctl_fd(c), NULL);
    G.clients[i] = NULL;
    if (G.S.owner == c) {
        G.S.owner = NULL;
        end_session_ex("client disconnected", "", -1, 1);
    }
    rdx_ctl_close(c);
}

/* the last "[gs] vrr: ..." line of the session's rdx_gs_src (what it really runs), without the prefix;
 * 0 = none */
static int gs_vrr_line(char *out, size_t n)
{
    struct rdx_proc *gp = sess_proc("gs_src");
    static char tail[16384];
    if (!gp) return 0;
    if (rdx_proc_kept(gp, out, n)) return 1;     /* survives an app that floods the output (Wine, DXVK) */
    size_t tn = rdx_proc_log_tail(gp, tail, sizeof(tail) - 1);
    tail[tn] = 0;
    const char *last = NULL;
    for (const char *q = tail; (q = strstr(q, "[gs] vrr: ")); q++) last = q;
    if (!last) return 0;
    last += strlen("[gs] vrr: ");
    snprintf(out, n, "%.*s", (int)strcspn(last, "\n"), last);
    return 1;
}

/* session.update {"display"}: the client's screen changed during the session (its window went out of
 * full screen, another monitor). A gamescope app's rdx_gs_src switches variable refresh with the same
 * buffers: sigqueue(SIGRTMIN, cap in mHz, 0 = fixed refresh) */
static void update_display(cJSON *disp)
{
    struct session *s = &G.S;
    struct rdx_vrr nv;
    rdx_vrr_decide(disp, jbool(cfg_defaults(), "vrr", 1), session_vrr_kind(s), jbool(s->params, "tearing", 0),
                   jnum(cfg_defaults(), "vrr_cap_frac", RDX_VRR_CAP_FRAC_DEFAULT), &nv);
    cfg_set(s->params, "display", cJSON_Duplicate(disp, 1));
    cfg_set(s->params, "vrr", cJSON_CreateBool(nv.active));
    double d = nv.cap_hz - s->vrr.cap_hz;
    int changed = nv.active != s->vrr.active || (nv.active && (d < 0 ? -d : d) > 0.0005);
    s->vrr = nv;
    fprintf(stderr, "[hostd] session.update display: %s%s\n", nv.why, changed ? "" : " (unchanged)");
    struct rdx_proc *gp = sess_proc("gs_src");
    if (!changed || !vrr_kind_paced(session_vrr_kind(s)) || !gp || !rdx_proc_running(gp)) return;
    /* its signalfd stands right after the start; a signal before that would end it */
    if (rdx_proc_uptime_s(gp) < 0.5) {
        fprintf(stderr, "[hostd] rdx_gs_src is still starting: the variable refresh change waits for the next start\n");
        return;
    }
    union sigval v;
    v.sival_int = nv.active ? (int)(nv.cap_hz * 1000.0 + 0.5) : 0;
    if (sigqueue(rdx_proc_pid(gp), SIGRTMIN, v)) fprintf(stderr, "[hostd] variable refresh to rdx_gs_src: %s\n", strerror(errno));
}

/* session.update {cursor?, output?} (in-stream menu of the client): rdx_mirror_src takes
 * both at runtime by sigqueue (see p1/rdx_mirror_src.c), so the stream stays connected */
static void update_session(struct rdx_ctl *c, double id, cJSON *req)
{
    struct session *s = &G.S;
    if (s->state != S_RUNNING) { ctl_reply(c, id, 0, "no running session", NULL); return; }
    cJSON *mon = cJSON_GetObjectItem(req, "monitor");
    if (cJSON_IsBool(mon)) {                            /* in-stream menu: "Monitor off / on" */
        if (!s->local_join || local_sleep(!cJSON_IsTrue(mon))) {
            ctl_reply(c, id, 0, s->local_join ? "the seat's monitor cannot be switched" : "this stream has no local monitor", NULL);
            return;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "monitor", !G.local.asleep);
        ctl_reply(c, id, 1, NULL, r);
        return;
    }
    cJSON *disp = cJSON_GetObjectItem(req, "display");
    if (cJSON_IsObject(disp)) {
        update_display(disp);
        if (!cJSON_GetObjectItem(req, "cursor") && !cJSON_GetObjectItem(req, "output")) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddItemToObject(r, "vrr", rdx_vrr_json(&s->vrr));
            ctl_reply(c, id, 1, NULL, r);
            return;
        }
    }
    struct rdx_proc *mp = NULL;
    for (int i = 0; i < s->nprocs; i++)
        if (!strcmp(rdx_proc_name(s->procs[i]), "mirror_src") && rdx_proc_running(s->procs[i])) mp = s->procs[i];
    if (!mp) { ctl_reply(c, id, 0, "this app has no mirror source", NULL); return; }
    /* its signal handlers stand right after exec; a signal before that would kill it */
    if (rdx_proc_uptime_s(mp) < 0.5) { ctl_reply(c, id, 0, "the mirror source is still starting", NULL); return; }
    cJSON *cur = cJSON_GetObjectItem(req, "cursor");
    const char *out = jstr(req, "output", NULL);
    int idx = -1;
    if (out) {
        refresh_outputs(0);
        for (int i = 0; i < G.nouts; i++) if (!strcmp(G.outs[i].name, out)) idx = i;
        if (idx < 0) { ctl_reply(c, id, 0, "unknown output (see hello.outputs)", NULL); return; }
    }
    union sigval v;
    if (cJSON_IsBool(cur)) {
        v.sival_int = cJSON_IsTrue(cur);
        if (sigqueue(rdx_proc_pid(mp), SIGRTMIN, v)) { ctl_reply(c, id, 0, strerror(errno), NULL); return; }
        cJSON_ReplaceItemInObject(s->params, "cursor", cJSON_CreateBool(v.sival_int));
    }
    if (idx >= 0) {
        v.sival_int = idx;
        if (sigqueue(rdx_proc_pid(mp), SIGRTMIN + 1, v)) { ctl_reply(c, id, 0, strerror(errno), NULL); return; }
        /* the picture really changes: the client may show its placeholder until the next frame
         * (only if that takes longer than a moment; never just because no frames come) */
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "event", "session.stall");
        cJSON_AddStringToObject(e, "why", "switching to another output");
        if (s->owner) rdx_ctl_send(s->owner, e);
        cJSON_Delete(e);
        snprintf(s->output, sizeof(s->output), "%s", out);
        if (!s->app_cfg) snprintf(s->app, sizeof(s->app), "mirror:%s", out);
        cJSON_ReplaceItemInObject(s->params, "output", cJSON_CreateString(s->output));
        cJSON_ReplaceItemInObject(s->params, "app", cJSON_CreateString(s->app));
    }
    fprintf(stderr, "[hostd] session.update: cursor %s, output %s\n",
            cJSON_IsTrue(cJSON_GetObjectItem(s->params, "cursor")) ? "on" : "off", s->output);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "cursor", cJSON_IsTrue(cJSON_GetObjectItem(s->params, "cursor")));
    cJSON_AddStringToObject(r, "output", s->output);
    ctl_reply(c, id, 1, NULL, r);
}

static void handle_msg(struct rdx_ctl *c, cJSON *m)
{
    double id = jnum(m, "id", -1);
    const char *cmd = jstr(m, "cmd", "");
    if (!strcmp(cmd, "hello")) {
        fprintf(stderr, "[hostd] hello from %s (%s)\n", rdx_ctl_peer(c), jstr(m, "client_name", "?"));
        ctl_reply(c, id, 1, NULL, hello_json());
    } else if (!strcmp(cmd, "session.start")) {
        begin_session(c, id, m);
    } else if (!strcmp(cmd, "session.update")) {
        update_session(c, id, m);
    } else if (!strcmp(cmd, "session.stop")) {
        /* the app ended first: the stop is its consequence. Its "stopping" line may wait in the pipe
         * behind this message (same wake-up): read the children's output first (exit callbacks
         * only mark pending_end) */
        rdx_proc_dispatch();
        app_ended_check();
        if (G.S.state != S_IDLE && G.S.app_ended) end_session_ex(G.S.app_reason, "gs_src", 0, 0);
        else if (G.S.state != S_IDLE) end_session_ex("stopped by the client", "", -1, 1);
        ctl_reply(c, id, 1, NULL, NULL);
    } else if (!strcmp(cmd, "host.restart")) {
        /* the client restarts us without walking over: reply, shut down cleanly, exit with
         * RDX_EXIT_RESTART; the unit restarts on that code (Restart=on-failure) */
        fprintf(stderr, "[hostd] restart requested by the client %s\n", rdx_ctl_peer(c));
        ctl_reply(c, id, 1, NULL, NULL);
        G.restart = 1;
    } else if (!strcmp(cmd, "app.quit")) {
        const char *app = jstr(m, "app", "");
        int found = 0;
        if (G.S.state != S_IDLE && !strcmp(G.S.app, app)) { end_session("app quit by the client", "", -1); found = 1; }
        int k = susp_find(app);
        if (k >= 0) { quit_suspended_at(k, "app.quit from the client"); found = 1; }
        cJSON *r = cJSON_CreateObject();
        add_suspended(r);
        ctl_reply(c, id, found, found ? NULL : "app is neither running nor suspended", r);
    } else if (!strcmp(cmd, "status")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "session", session_json());
        /* the client polls status, also while idle: it learns about a suspend here, not only
         * from the hello of a new connection */
        add_suspended(r);
        ctl_reply(c, id, 1, NULL, r);
    } else ctl_reply(c, id, 0, "unknown cmd", NULL);
}

static void try_listen(void)
{
    if (G.lfd >= 0 || now_s() < G.next_listen) return;
    G.lfd = rdx_ctl_listen(cfg_str("link_ip", "10.0.0.2"), cfg_int("ctl_port", RDX_CTL_PORT));
    if (G.lfd < 0) { G.next_listen = now_s() + RETRY_LISTEN_S; return; }
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 1};
    epoll_ctl(G.ep, EPOLL_CTL_ADD, G.lfd, &ev);
    fprintf(stderr, "[hostd] control channel on %s:%d\n", cfg_str("link_ip", ""), cfg_int("ctl_port", RDX_CTL_PORT));
}

/* ---------------- web UI ---------------- */

static int send_json(struct mg_connection *conn, int code, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    size_t n = strlen(s);
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
              "Content-Length: %zu\r\nConnection: close\r\n\r\n",
              code, code == 200 ? "OK" : "Error", n);
    mg_write(conn, s, n);
    free(s);
    cJSON_Delete(j);
    return code;
}

static int send_err(struct mg_connection *conn, int code, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 0);
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(conn, code, j);
}

static cJSON *read_body(struct mg_connection *conn)
{
    char buf[65536];
    int n = 0, r;
    while (n < (int)sizeof(buf) - 1 && (r = mg_read(conn, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0) n += r;
    buf[n] = 0;
    return n ? cJSON_Parse(buf) : cJSON_CreateObject();
}

static int h_state(struct mg_connection *conn, void *cb)
{
    (void)cb;
    pthread_mutex_lock(&G.mu);
    cJSON *j = state_json();
    pthread_mutex_unlock(&G.mu);
    return send_json(conn, 200, j);
}

/* settings the web UI may change: any top-level scalar, and keys inside "defaults" (no auth by design: 127.0.0.1 and the link only) */
static int h_config(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!strcmp(ri->request_method, "GET")) {
        pthread_mutex_lock(&G.mu);
        cJSON *j = cJSON_Duplicate(G.cfg, 1);
        pthread_mutex_unlock(&G.mu);
        return send_json(conn, 200, j);
    }
    cJSON *in = read_body(conn);
    if (!cJSON_IsObject(in)) { cJSON_Delete(in); return send_err(conn, 400, "body must be a JSON object"); }
    cJSON *pr = cJSON_GetObjectItem(in, "priority");
    if (pr && (!cJSON_IsString(pr) || (strcmp(pr->valuestring, "high") && strcmp(pr->valuestring, "normal") &&
                                       strcmp(pr->valuestring, "low")))) {
        cJSON_Delete(in);
        return send_err(conn, 400, "priority is high, normal or low");
    }
    /* "gpu" / "helper_gpu": "" (automatic / none) or a usable GPU of this host, any vendor */
    int gpu_changed = 0;
    const char *gkeys[] = {"gpu", "helper_gpu"};
    for (int k = 0; k < 2; k++) {
        cJSON *g = cJSON_GetObjectItem(in, gkeys[k]);
        if (!g) continue;
        int ok = cJSON_IsString(g) && !g->valuestring[0];
        pthread_mutex_lock(&G.mu);
        for (int i = 0; !ok && cJSON_IsString(g) && i < G.ngpus; i++)
            ok = G.gpus[i].usable && !strcasecmp(G.gpus[i].pci, g->valuestring);
        pthread_mutex_unlock(&G.mu);
        if (!ok) { cJSON_Delete(in); return send_err(conn, 400, "not a GPU this seat can use"); }
        gpu_changed |= !k;
    }
    pthread_mutex_lock(&G.mu);
    cJSON *it;
    cJSON_ArrayForEach(it, in) {
        if (!strcmp(it->string, "defaults") && cJSON_IsObject(it)) {
            cJSON *d = cfg_defaults(), *x;
            cJSON_ArrayForEach(x, it) cfg_set(d, x->string, cJSON_Duplicate(x, 1));
        } else if (cJSON_IsString(it) || cJSON_IsNumber(it) || cJSON_IsBool(it)) {
            cfg_set(G.cfg, it->string, cJSON_Duplicate(it, 1));
        }
    }
    int rc = cfg_save();
    cJSON *j = cJSON_Duplicate(G.cfg, 1);
    pthread_mutex_unlock(&G.mu);
    cJSON_Delete(in);
    if (rc) { cJSON_Delete(j); return send_err(conn, 500, "cannot write the config file"); }
    fprintf(stderr, "[hostd] config updated from the web UI\n");
    if (gpu_changed) resolve_gpu();     /* applies to the next app start */
    return send_json(conn, 200, j);
}

static int send_file(struct mg_connection *conn, const char *path, const char *mime)
{
    FILE *f = fopen(path, "rb");
    if (!f) return send_err(conn, 404, "no icon");
    static char buf[4 * 1024 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nCache-Control: max-age=3600\r\n"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n", mime, n);
    mg_write(conn, buf, n);
    return 200;
}

static int send_icon(struct mg_connection *conn, const char *icon)
{
    if (!strcmp(icon, "rdx-screen") || !*icon) {
        size_t n = strlen(rdx_screen_svg);
        mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Type: image/svg+xml\r\nCache-Control: max-age=3600\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
        mg_write(conn, rdx_screen_svg, n);
        return 200;
    }
    struct rdx_desktop_user du;
    int have_user = !rdx_desktop_user(cfg_str("desktop_user", ""), &du);
    char path[1024];
    const char *mime = NULL;
    if (rdx_find_icon(have_user ? &du : NULL, icon, path, sizeof(path), &mime)) return send_icon(conn, "rdx-screen");
    return send_file(conn, path, mime);
}

/* GET /api/icon?name=<icon theme name or absolute path> */
static int h_icon(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char name[512] = "";
    if (ri->query_string) mg_get_var(ri->query_string, strlen(ri->query_string), "name", name, sizeof(name));
    return send_icon(conn, name);
}

static void save_apps_locked(cJSON *apps)
{
    cfg_set(G.cfg, "apps", apps);
    cfg_save();
}

/* POST /api/local {"app", "action": "here" | "to_client"}: "Start here on this PC" / "Back to this
 * PC" (here), "Send to the client" (to_client) */
static int h_local(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (strcmp(ri->request_method, "POST")) return send_err(conn, 405, "POST only");
    cJSON *b = read_body(conn);
    char e[200] = "", app[64];
    pthread_mutex_lock(&G.mu);
    /* no app given: the one running (a client's stream) or paused */
    snprintf(app, sizeof(app), "%s", *jstr(b, "app", "") ? jstr(b, "app", "") : G.S.state != S_IDLE ? G.S.app : susp_newest() >= 0 ? G.susp[susp_newest()].app : "");
    int rc = !strcmp(jstr(b, "action", "here"), "to_client") ? send_to_client(e, sizeof(e)) : begin_local(app, e, sizeof(e));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", rc == 0);
    if (rc) cJSON_AddStringToObject(j, "error", e);
    cJSON_AddItemToObject(j, "session", session_json());
    add_suspended(j);
    pthread_mutex_unlock(&G.mu);
    cJSON_Delete(b);
    return send_json(conn, rc ? 409 : 200, j);
}

/* /api/apps            GET: configured apps; POST: replace the whole list (validated)
 * /api/apps/detect     GET: installed programs with ready presets, plus Steam's state
 * /api/apps/add        POST {"desktop_id"} or {"preset":{...}}: add one app
 * /api/apps/remove     POST {"id"}
 * /api/apps/<id>/icon  GET: the app's icon */
static int h_apps(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : "/api/apps";
    int get = !strcmp(ri->request_method, "GET");
    if (!strcmp(uri, "/api/apps/detect")) {
        struct rdx_desktop_user du;
        int have_user = !rdx_desktop_user(cfg_str("desktop_user", ""), &du);
        cJSON *j = cJSON_CreateObject();
        cJSON *list = rdx_detect_apps(have_user ? &du : NULL);
        cJSON_AddStringToObject(j, "desktop_user", have_user ? du.name : "");
        int steam = 0;
        cJSON *e;
        cJSON_ArrayForEach(e, list) {
            char icon[700];
            snprintf(icon, sizeof(icon), "/api/icon?name=%s", jstr(e, "icon", ""));
            cJSON_AddStringToObject(e, "icon_url", icon);
            steam |= !strcmp(jstr(e, "id", ""), "steam") || !strcmp(jstr(e, "id", ""), "com.valvesoftware.Steam");
            cJSON *pre = cJSON_GetObjectItem(e, "preset");
            pthread_mutex_lock(&G.mu);
            cJSON_AddBoolToObject(e, "added", find_app(jstr(pre, "id", "")) != NULL);
            pthread_mutex_unlock(&G.mu);
        }
        cJSON_AddBoolToObject(j, "steam_installed", steam || !access("/usr/bin/steam", X_OK));
        cJSON_AddItemToObject(j, "programs", list);
        return send_json(conn, 200, j);
    }
    if (!get && !strcmp(uri, "/api/apps/quit")) {
        cJSON *b = read_body(conn);
        const char *app = jstr(b, "app", "");
        pthread_mutex_lock(&G.mu);
        int found = 0;
        if (G.S.state != S_IDLE && !strcmp(G.S.app, app)) { end_session("app quit from the host web UI", "", -1); found = 1; }
        int k = *app ? susp_find(app) : susp_newest();
        if (k >= 0) { quit_suspended_at(k, "quit from the host web UI"); found = 1; }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", found);
        add_suspended(j);
        pthread_mutex_unlock(&G.mu);
        cJSON_Delete(b);
        return send_json(conn, found ? 200 : 404, j);
    }
    size_t ul = strlen(uri);
    if (get && !strncmp(uri, "/api/apps/", 10) && ul > 15 && !strcmp(uri + ul - 5, "/icon")) {
        char id[64];
        snprintf(id, sizeof(id), "%.*s", (int)(ul - 15), uri + 10);
        pthread_mutex_lock(&G.mu);
        cJSON *a = find_app(id);
        char icon[512];
        snprintf(icon, sizeof(icon), "%s", a ? jstr(a, "icon", "") : "");
        pthread_mutex_unlock(&G.mu);
        return send_icon(conn, icon);
    }
    if (!strcmp(uri, "/api/apps/add") || !strcmp(uri, "/api/apps/remove")) {
        if (get) return send_err(conn, 405, "POST");
        cJSON *in = read_body(conn);
        int add = !strcmp(uri, "/api/apps/add");
        cJSON *app = NULL;
        if (add) {
            cJSON *pre = cJSON_GetObjectItem(in, "preset");
            if (cJSON_IsObject(pre)) app = cJSON_Duplicate(pre, 1);
            else {
                struct rdx_desktop_user du;
                int have_user = !rdx_desktop_user(cfg_str("desktop_user", ""), &du);
                app = rdx_preset_for(have_user ? &du : NULL, jstr(in, "desktop_id", ""));
            }
            if (!app) { cJSON_Delete(in); return send_err(conn, 404, "program not found on this computer"); }
        }
        pthread_mutex_lock(&G.mu);
        cJSON *apps = cJSON_Duplicate(cJSON_GetObjectItem(G.cfg, "apps"), 1);
        if (add) {
            /* same id again: keep a unique id */
            char base[48], id[56];
            snprintf(base, sizeof(base), "%s", jstr(app, "id", "app"));
            snprintf(id, sizeof(id), "%s", base);
            for (int k = 2; find_app(id) && k < 100; k++) snprintf(id, sizeof(id), "%s-%d", base, k);
            cfg_set(app, "id", cJSON_CreateString(id));
            cJSON_AddItemToArray(apps, app);
        } else {
            int i = 0, del = -1;
            cJSON *x;
            cJSON_ArrayForEach(x, apps) { if (!strcmp(jstr(x, "id", ""), jstr(in, "id", ""))) del = i; i++; }
            if (del >= 0) cJSON_DeleteItemFromArray(apps, del);
        }
        const char *bad = validate_apps(apps);
        if (bad) { pthread_mutex_unlock(&G.mu); cJSON_Delete(apps); cJSON_Delete(in); return send_err(conn, 400, bad); }
        save_apps_locked(apps);
        cJSON *j = cJSON_Duplicate(cJSON_GetObjectItem(G.cfg, "apps"), 1);
        pthread_mutex_unlock(&G.mu);
        cJSON_Delete(in);
        fprintf(stderr, "[hostd] app %s from the web UI\n", add ? "added" : "removed");
        return send_json(conn, 200, j);
    }
    if (get) {
        pthread_mutex_lock(&G.mu);
        cJSON *j = cJSON_Duplicate(cJSON_GetObjectItem(G.cfg, "apps"), 1);
        pthread_mutex_unlock(&G.mu);
        return send_json(conn, 200, j);
    }
    cJSON *in = read_body(conn);
    cJSON *apps = cJSON_IsObject(in) ? cJSON_GetObjectItem(in, "apps") : in;
    const char *bad = validate_apps(apps);
    if (bad) { cJSON_Delete(in); return send_err(conn, 400, bad); }
    pthread_mutex_lock(&G.mu);
    cfg_set(G.cfg, "apps", cJSON_Duplicate(apps, 1));
    int rc = cfg_save();
    cJSON *j = cJSON_Duplicate(cJSON_GetObjectItem(G.cfg, "apps"), 1);
    pthread_mutex_unlock(&G.mu);
    cJSON_Delete(in);
    if (rc) { cJSON_Delete(j); return send_err(conn, 500, "cannot write the config file"); }
    fprintf(stderr, "[hostd] app list updated from the web UI (%d app(s))\n", cJSON_GetArraySize(j));
    return send_json(conn, 200, j);
}

/* /api/steam  GET: {installed, installing, exit, log}; POST: install Steam in the background
 * (Ubuntu: multiverse + i386 + steam-installer, license accepted via debconf) */
static const char steam_install_sh[] =
    "set -x; export DEBIAN_FRONTEND=noninteractive; "
    "grep -rhq multiverse /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null || add-apt-repository -y -n multiverse; "
    "dpkg --print-foreign-architectures | grep -q i386 || dpkg --add-architecture i386; "
    "echo 'steam steam/question select I AGREE' | debconf-set-selections; "
    "echo 'steam steam/license note ' | debconf-set-selections; "
    "apt-get -o DPkg::Lock::Timeout=600 update -q && apt-get -o DPkg::Lock::Timeout=600 install -y -q steam-installer";

static void on_installer_exit(struct rdx_proc *p, int status, void *user)
{
    (void)p; (void)user;
    fprintf(stderr, "[hostd] Steam install finished, status %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static int h_steam(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    pthread_mutex_lock(&G.mu);
    if (strcmp(ri->request_method, "GET") && access("/usr/bin/steam", X_OK) &&
        !(G.installer && rdx_proc_running(G.installer))) {
        if (G.installer) rdx_proc_free(G.installer);
        char *av[] = {"/bin/sh", "-c", (char *)steam_install_sh, NULL};
        G.installer = rdx_proc_spawn("steam_install", av, on_installer_exit, NULL);
        fprintf(stderr, "[hostd] installing Steam (from the web UI)\n");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "installed", !access("/usr/bin/steam", X_OK));
    cJSON_AddBoolToObject(j, "installing", G.installer && rdx_proc_running(G.installer));
    if (G.installer) {
        static char tail[16384];
        size_t n = rdx_proc_log_tail(G.installer, tail, sizeof(tail) - 1);
        tail[n] = 0;
        cJSON_AddStringToObject(j, "log", tail);
        int st = rdx_proc_exit_status(G.installer);
        if (st >= 0) cJSON_AddNumberToObject(j, "exit", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    }
    pthread_mutex_unlock(&G.mu);
    return send_json(conn, 200, j);
}

static int h_stop(struct mg_connection *conn, void *cb)
{
    (void)cb;
    pthread_mutex_lock(&G.mu);
    if (G.S.state != S_IDLE) end_session_ex("disconnected from the host web UI", "", -1, 1);
    else if (susp_newest() >= 0) quit_suspended_at(susp_newest(), "stopped from the host web UI");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 1);
    cJSON_AddItemToObject(j, "session", session_json());
    pthread_mutex_unlock(&G.mu);
    return send_json(conn, 200, j);
}

static int h_start(struct mg_connection *conn, void *cb)
{
    (void)cb;
    return send_err(conn, 400, "sessions are started from the client (it must listen first)");
}

static int h_logs(struct mg_connection *conn, void *cb)
{
    (void)cb;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char name[32] = "";
    if (ri->query_string) mg_get_var(ri->query_string, strlen(ri->query_string), "proc", name, sizeof(name));
    static char tail[64 * 1024];
    size_t n = 0;
    int found = 0;
    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < G.S.nprocs && !found; i++)
        if (!strcmp(rdx_proc_name(G.S.procs[i]), name)) { n = rdx_proc_log_tail(G.S.procs[i], tail, sizeof(tail)); found = 1; }
    for (int i = 0; i < G.nlast && !found; i++)
        if (!strcmp(rdx_proc_name(G.last[i]), name)) { n = rdx_proc_log_tail(G.last[i], tail, sizeof(tail)); found = 1; }
    if (found) {
        mg_printf(conn,
                  "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n"
                  "Content-Length: %zu\r\nConnection: close\r\n\r\n", n);
        mg_write(conn, tail, n);
    }
    pthread_mutex_unlock(&G.mu);
    return found ? 200 : send_err(conn, 404, "no such process in the current or last session");
}

static int start_web(void)
{
    char ports[256];
    int wp = cfg_int("web_port", 47990);
    const char *link = cfg_str("link_ip", "");
    /* right after a restart the previous instance may still hold the port for a moment: keep
     * trying the link IP for ~10 s before falling back to localhost only (then the client's
     * icon proxy, which fetches from the link IP, cannot reach us) */
    const int link_tries = 20;
    for (int attempt = 0; attempt <= link_tries; attempt++) {
        if (attempt < link_tries && *link && strcmp(link, "127.0.0.1")) snprintf(ports, sizeof(ports), "127.0.0.1:%d,%s:%d", wp, link, wp);
        else if (attempt == link_tries) snprintf(ports, sizeof(ports), "127.0.0.1:%d", wp);
        else continue;
        if (attempt) usleep(500000);
        const char *opts[] = {"listening_ports", ports, "document_root", G.web_root, "num_threads", "4",
                              "enable_directory_listing", "no", "access_control_allow_origin", "*", NULL};
        struct mg_callbacks cbs;
        memset(&cbs, 0, sizeof(cbs));
        G.web = mg_start(&cbs, NULL, opts);
        if (G.web) {
            fprintf(stderr, "[hostd] web UI on %s (root %s)\n", ports, G.web_root);
            break;
        }
        fprintf(stderr, "[hostd] web UI could not bind %s\n", ports);
    }
    if (!G.web) return -1;
    mg_set_request_handler(G.web, "/api/state", h_state, NULL);
    mg_set_request_handler(G.web, "/api/config", h_config, NULL);
    mg_set_request_handler(G.web, "/api/session/stop", h_stop, NULL);
    mg_set_request_handler(G.web, "/api/session/start", h_start, NULL);
    mg_set_request_handler(G.web, "/api/logs", h_logs, NULL);
    mg_set_request_handler(G.web, "/api/apps", h_apps, NULL);
    mg_set_request_handler(G.web, "/api/local", h_local, NULL);      /* also /api/apps/... */
    mg_set_request_handler(G.web, "/api/icon", h_icon, NULL);
    mg_set_request_handler(G.web, "/api/steam", h_steam, NULL);
    struct rdx_mgpu_host mh = {.repo = G.repo, .mu = &G.mu, .cfg = mgpu_cfg, .seat_gpu = mgpu_seat_gpu, .save = cfg_save};
    rdx_mgpu_init(&mh);
    mg_set_request_handler(G.web, "/api/mgpu", rdx_mgpu_http, NULL);   /* also /api/mgpu/... */
    return 0;
}

/* ---------------- main ---------------- */

static void default_repo(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { snprintf(G.repo, sizeof(G.repo), "."); return; }
    exe[n] = 0;
    snprintf(G.repo, sizeof(G.repo), "%s", dirname(dirname(exe)));   /* <repo>/host/rdx_hostd */
}

int main(int argc, char **argv)
{
    const char *bind_ip = NULL, *dev = NULL;
    int ctl_port = 0, web_port = 0, mtu = 0, have_cfg = 0;
    G.seat = -1;
    snprintf(G.cfg_path, sizeof(G.cfg_path), "/etc/rdx/hostd.json");
    default_repo();
    static const struct option opts[] = {
        {"config", 1, 0, 'c'}, {"repo", 1, 0, 'r'}, {"web-root", 1, 0, 'w'}, {"bind", 1, 0, 'b'},
        {"ctl-port", 1, 0, 'C'}, {"web-port", 1, 0, 'W'}, {"dev", 1, 0, 'd'}, {"mtu", 1, 0, 'm'},
        {"seat", 1, 0, 's'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'c': snprintf(G.cfg_path, sizeof(G.cfg_path), "%s", optarg); have_cfg = 1; break;
        case 's': G.seat = atoi(optarg); if (G.seat < 0 || G.seat > 99) { fprintf(stderr, "--seat 0..99\n"); return 2; } break;
        case 'r': snprintf(G.repo, sizeof(G.repo), "%s", optarg); break;
        case 'w': snprintf(G.web_root, sizeof(G.web_root), "%s", optarg); break;
        case 'b': bind_ip = optarg; break;
        case 'C': ctl_port = atoi(optarg); break;
        case 'W': web_port = atoi(optarg); break;
        case 'd': dev = optarg; break;
        case 'm': mtu = atoi(optarg); break;
        default:  fprintf(stderr, "see the header of host/rdx_hostd.c for options\n"); return 2;
        }
    }
    if (G.seat > 0 && !have_cfg) snprintf(G.cfg_path, sizeof(G.cfg_path), "/etc/rdx/hostd-seat%d.json", G.seat);
    if (!G.web_root[0]) snprintf(G.web_root, sizeof(G.web_root), "%s/web/host", G.repo);
    gethostname(G.host_name, sizeof(G.host_name));
    pthread_mutex_init(&G.mu, NULL);
    G.t0 = now_s();
    G.lfd = -1;
    G.gs_input = -1;                    /* gs_input_capable reads the gamescope binary once */
    cfg_load();
    if (bind_ip) cfg_set(G.cfg, "link_ip", cJSON_CreateString(bind_ip));
    if (ctl_port) cfg_set(G.cfg, "ctl_port", cJSON_CreateNumber(ctl_port));
    if (web_port) cfg_set(G.cfg, "web_port", cJSON_CreateNumber(web_port));
    if (dev) cfg_set(G.cfg, "rdma_dev", cJSON_CreateString(dev));
    if (mtu) cfg_set(G.cfg, "mtu", cJSON_CreateNumber(mtu));
    cfg_set(G.cfg, "seat", cJSON_CreateNumber(G.seat));
    fprintf(stderr, "[hostd] repo %s, config %s, seat %d\n", G.repo, G.cfg_path, G.seat);
    resolve_gpu();
    setup_pipewire_env();
    signal(SIGPIPE, SIG_IGN);

    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    G.sfd = signalfd(-1, &ss, SFD_CLOEXEC);
    G.ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0};
    epoll_ctl(G.ep, EPOLL_CTL_ADD, G.sfd, &ev);
    ev.data.u64 = 2;
    epoll_ctl(G.ep, EPOLL_CTL_ADD, rdx_proc_poll_fd(), &ev);
    if (!rdx_link_start(cfg_str("link_ip", ""), NULL)) {         /* link health, never fatal */
        ev.data.u64 = 3;
        epoll_ctl(G.ep, EPOLL_CTL_ADD, rdx_link_event_fd(), &ev);
    }

    pthread_mutex_lock(&G.mu);
    refresh_outputs(1);
    for (int i = 0; i < G.nouts; i++)
        fprintf(stderr, "[hostd] output %s %dx%d@%d\n", G.outs[i].name, G.outs[i].w, G.outs[i].h, G.outs[i].hz);
    try_listen();
    pthread_mutex_unlock(&G.mu);
    /* civetweb threads are created after the signal mask is set, so they never take SIGTERM */
    if (start_web()) return 1;

    for (int running = 1; running;) {
        pthread_mutex_lock(&G.mu);
        double wake = 1e18;
        if (G.lfd < 0) wake = G.next_listen;
        if (G.S.state == S_STARTING && G.S.deadline < wake) wake = G.S.deadline;
        if (G.S.notice_end > 0 && G.S.notice_end < wake) wake = G.S.notice_end;
        if (G.local.next_check < wake) wake = G.local.next_check;
        pthread_mutex_unlock(&G.mu);
        int tmo = wake > 1e17 ? -1 : (int)((wake - now_s()) * 1000) + 1;
        if (tmo < 0 && wake < 1e17) tmo = 0;

        struct epoll_event evs[16];
        int n = epoll_wait(G.ep, evs, 16, tmo);
        if (n < 0 && errno != EINTR) { perror("epoll_wait"); break; }
        pthread_mutex_lock(&G.mu);
        for (int i = 0; i < n; i++) {
            uint64_t tag = evs[i].data.u64;
            if (tag == 0) {
                struct signalfd_siginfo si;
                if (read(G.sfd, &si, sizeof(si)) == sizeof(si)) running = 0;
            } else if (tag == 1) {
                struct rdx_ctl *c = rdx_ctl_accept(G.lfd);
                int slot = -1;
                for (int k = 0; k < MAX_CLIENTS && c; k++) if (!G.clients[k]) { slot = k; break; }
                if (c && slot < 0) { rdx_ctl_close(c); c = NULL; }
                if (c) {
                    G.clients[slot] = c;
                    struct epoll_event cev = {.events = EPOLLIN, .data.u64 = 16 + (uint64_t)slot};
                    epoll_ctl(G.ep, EPOLL_CTL_ADD, rdx_ctl_fd(c), &cev);
                    fprintf(stderr, "[hostd] client %s connected\n", rdx_ctl_peer(c));
                }
            } else if (tag == 2) {
                rdx_proc_dispatch();
            } else if (tag == 3) {
                if (rdx_link_take()) link_broadcast();
            } else {
                int slot = (int)(tag - 16);
                struct rdx_ctl *c = G.clients[slot];
                if (!c) continue;
                cJSON *m;
                int r;
                while ((r = rdx_ctl_recv(c, &m)) == 1) {
                    handle_msg(c, m);
                    cJSON_Delete(m);
                    if (G.clients[slot] != c) break;
                }
                if (r < 0 && G.clients[slot] == c) client_gone(slot);
            }
            session_tick();
        }
        session_tick();
        local_tick();
        try_listen();
        if (G.restart) running = 0;
        pthread_mutex_unlock(&G.mu);
    }

    fprintf(stderr, "[hostd] %s\n", G.restart ? "restarting (requested by the client)" : "shutting down");
    mg_stop(G.web);
    pthread_mutex_lock(&G.mu);
    end_session("rdx-hostd shutting down", "", -1);
    quit_suspended_all("rdx-hostd shutting down");
    local_stop("rdx-hostd shutting down");
    for (int i = 0; i < MAX_CLIENTS; i++) if (G.clients[i]) rdx_ctl_close(G.clients[i]);
    pthread_mutex_unlock(&G.mu);
    return G.restart ? RDX_EXIT_RESTART : 0;
}
