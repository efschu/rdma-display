/* The gamescope path source: gamescope composites directly into NIC-readable buffers.
 *
 *   rdx_sink_kms (client)  <--RDMA--  rdx_gs_src  <--unix socket-->  gamescope --backend rdx -- <app>
 *
 * - connects to the sink like p1/rdx_mirror_src (same protocol, unchanged sink),
 * - allocates one output buffer per sink slot (linear XRGB8888, sink stride), registers
 *   it with the NIC and hands its fd to gamescope (gs/rdx_gsbuf.h): --buffers vram = CUDA
 *   VMM in VRAM (the NIC peer-reads the GPU), host = pinned host memory (the GPU writes
 *   over PCIe, the NIC reads host RAM), auto = vram only with GPU and NIC at CPU root ports
 *   and the barlink stack present, else host,
 * - starts gamescope with the sink's mode (-W/-H/-r), so the app sees a virtual
 *   monitor of exactly the client's resolution and refresh,
 * - frees a slot to gamescope when the sink returns its credit, posts the slot
 *   by RDMA as soon as gamescope reports it composited,
 * - paces gamescope from the client's vblank: every "shown" report re-anchors
 *   gamescope's vblank timer at (sink vblank grid - network time - margin), so
 *   gamescope (and an app in FIFO mode) finishes each frame just in time for the
 *   sink's next vblank. No copy, no PipeWire.
 * - the lead (how far before the sink's vblank gamescope must be done) is a closed
 *   loop on what the sink reports: every "shown" report carries how long the frame
 *   waited from arrival to its vblank (CQE -> vblank). Target: the sink's flip lead
 *   plus a margin. Too long a wait = too early, a wait just under one period more =
 *   just missed = too late (the error is taken modulo one period). No estimate of
 *   the transfer time is needed, so it holds for any PCIe path to the NIC.
 *
 *   rdx_gs_src --peer 10.0.0.1 [--dev auto] [--port 18650] [--mtu 4096]
 *              [--cuda 0] [--socket /run/rdx-gs.sock] [--gamescope PATH] [--gs-arg ARG ...]
 *              [--lead-margin-ms 1.5] [--sink-flip-lead-ms 2.0] [--log-dir DIR|-] [--comp-vector N]
 *              [--pm-qos-us 50] [--user NAME] [--env KEY=VALUE ...] [--buffers auto|host|vram] [--force-vram]
 *              [--tearing]      (client shows frames on arrival: no vblank pacing, gamescope --immediate-flips)
 *              [--vrr MAX_HZ]   (the client's screen has variable refresh up to MAX_HZ, FreeSync / VRR:
 *                    gamescope runs at the app's pace, capped at MAX_HZ (never above the sink's mode),
 *                    every frame is posted when it is composited, no vblank pacing and no lead loop;
 *                    gs/rdx_gs_vrr.h. Off with --tearing and with frame generation. A resume reads the
 *                    new client's value as the third field of <socket>.peer, "IP PORT MAX_HZ")
 * --peer local:PATH: a window on this computer instead of a client (p0b/rdx_sink_win --local PATH,
 *              gs/rdx_local_proto.h): host buffers, the window imports them, no RDMA transfer;
 *              suspend/resume between a local window and a client works like between two clients
 *              [--gpu 0000:0d:00.0]  (the GPU by PCI address, any vendor; default --cuda 0)
 *              [--fg off|helper] [--fg-gpu 0000:0e:00.0] [--fg-quality auto|quality|performance]
 *                    (frame generation on a helper GPU: gs/rdx_fg_stage.h, docs/MULTI-GPU.md; gamescope
 *                    keeps its full rate and all slots, the stage fills the vblanks the app leaves empty)
 *              [--lead-file PATH]  (the settled lead in ms, rewritten once per second of shown frames;
 *                    rdx-hostd hands it to the client in hello, which sizes the sink's slots with it:
 *                    rdx_pace_slots in common/rdx_ctl.h)
 *              [--cursor auto|picture]  (auto, the default: a sink that can show the cursor itself gets it
 *                    next to the picture, common/rdx_cursor.h: gamescope draws none into the frames, a
 *                    pointer that only moves sends no frame; picture: always drawn into the frames)
 *              [--priority high|normal|low]  (the seat's priority: gamescope's compositor queue
 *                    priority via GAMESCOPE_VK_QUEUE_PRIORITY (our patch), nice and I/O priority
 *                    of gamescope and the app: high = nice -5, ionice best-effort 0; low = nice 10,
 *                    best-effort 7; normal = unchanged)
 * The GPU (--gpu, any vendor): the Vulkan device at that PCI address (VK_EXT_pci_bus_info);
 * gamescope composites on the device with its UUID (GAMESCOPE_VK_DEVICE_UUID, our patch;
 * vendor:device alone cannot tell two identical cards apart). NVIDIA: CUDA ordinal from the same
 * address, --buffers auto looks at that GPU's own PCIe placement. AMD / Intel: host buffers, no
 * CUDA at all. The frame generation helper (--fg-gpu) may be any Vulkan GPU as well.
 *
 * Suspend / resume (Moonlight-like: ending a stream keeps the game running):
 * - the sink going away (its bootstrap connection closes, or the QP cannot reach it) or
 *   SIGUSR2 detaches: QP and socket go, gamescope, the app, the buffers and their MRs stay;
 *   gamescope keeps running cheaply, one slot is given back every 100 ms (10 fps), and lets go of
 *   every input device (RDX_GS_INPUT 0: several apps may wait in the background while another
 *   app's gamescope has the client's devices, rdx-hostd "suspend_multi"); RDX_GS_INPUT 1 after a resume;
 * - SIGUSR1 resumes: reads "IP PORT" from <socket>.peer and connects to that sink with the
 *   same MRs. A sink with another mode (size, stride, slots, refresh): new buffers and MRs
 *   for it, RECONFIG to gamescope (it re-creates its output at the new size, the app keeps
 *   running); only if that fails: exit 3, the supervisor then quits and starts fresh.
 *              [--verify N]      (host buffers: print a checksum of every Nth frame, see gs/rdx_hash.h)
 *              [-- app args...]      (no app: wait for an externally started gamescope)
 * --user: gamescope and the app run as that user (HOME, XDG_RUNTIME_DIR=/run/user/UID,
 * DBUS_SESSION_BUS_ADDRESS of their session); the socket is handed to that user. --env
 * adds environment for the app (repeatable). The source itself stays root (NIC, CUDA).
 */
#define _GNU_SOURCE
#include "rdx_gs_proto.h"
#include "rdx_gs_vrr.h"
#include "rdx_srcsess.h"
#include "rdx_gsbuf.h"
#include "rdx_hash.h"
#include "rdx_localinput.h"
#include "rdx_local_proto.h"
#include "../common/rdx_vmm.h"
#ifdef RDX_WITH_FG
#include "rdx_fg_stage.h"
#endif

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define MAX_GS_ARGS 32
#define MAX_ENVS 32

/* "vendor:device" of the Vulkan device with this UUID, for gamescope --prefer-vk-device
 * (on a host with several GPUs it would otherwise composite on the first one) */
static int vk_ids_for_uuid(const uint8_t uuid[16], char *out, size_t n)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return -1;
    uint32_t cnt = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &cnt, pds);
    int found = -1;
    for (uint32_t i = 0; i < cnt && found < 0; i++) {
        VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp};
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (!memcmp(idp.deviceUUID, uuid, 16)) {
            snprintf(out, n, "%04x:%04x", p2.properties.vendorID, p2.properties.deviceID);
            found = 0;
        }
    }
    vkDestroyInstance(inst, NULL);
    return found;
}

/* the Vulkan device at PCI address `pci` ("0000:09:00.0"): its deviceUUID (what gamescope's
 * GAMESCOPE_VK_DEVICE_UUID and the rdx backend match), PCI vendor id and name; 0 = found.
 * Any vendor: an AMD or Intel GPU has no CUDA, it is found here (VK_EXT_pci_bus_info). */
static int vk_find_pci(const char *pci, uint8_t uuid[16], uint32_t *vendor, char *name, size_t name_len)
{
    unsigned dom = 0, bus, dv, fn;
    if (sscanf(pci, "%x:%x:%x.%x", &dom, &bus, &dv, &fn) != 4 && (dom = 0, sscanf(pci, "%x:%x.%x", &bus, &dv, &fn) != 3))
        return -1;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return -1;
    uint32_t cnt = 16;
    VkPhysicalDevice pds[16];
    vkEnumeratePhysicalDevices(inst, &cnt, pds);
    int found = -1;
    for (uint32_t i = 0; i < cnt && found < 0; i++) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pds[i], NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
        vkEnumerateDeviceExtensionProperties(pds[i], NULL, &ne, ep);
        int has = 0;
        for (uint32_t k = 0; k < ne && !has; k++) has = !strcmp(ep[k].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME);
        free(ep);
        if (!has) continue;
        VkPhysicalDevicePCIBusInfoPropertiesEXT pb = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        VkPhysicalDeviceIDProperties idp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &pb};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp};
        vkGetPhysicalDeviceProperties2(pds[i], &p2);
        if (pb.pciDomain != dom || pb.pciBus != bus || pb.pciDevice != dv || pb.pciFunction != fn) continue;
        memcpy(uuid, idp.deviceUUID, 16);
        *vendor = p2.properties.vendorID;
        snprintf(name, name_len, "%s", p2.properties.deviceName);
        found = 0;
    }
    vkDestroyInstance(inst, NULL);
    return found;
}

static int send_hello(int fd, const struct rdx_gs_hello *h, const int *fds, uint32_t n)
{
    struct iovec iov = {.iov_base = (void *)h, .iov_len = sizeof(*h)};
    char cbuf[CMSG_SPACE(sizeof(int) * RDX_GS_MAX_SLOTS)];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
                       .msg_controllen = CMSG_SPACE(sizeof(int) * n)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n);
    return sendmsg(fd, &m, MSG_NOSIGNAL) == (ssize_t)sizeof(*h) ? 0 : -1;
}

static int send_msg(int fd, uint32_t type, uint32_t slot, uint64_t t_ns)
{
    struct rdx_gs_msg m = {.magic = RDX_GS_MAGIC, .type = type, .slot = slot, .t_ns = t_ns};
    return send(fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof(m) ? 0 : -1;
}

/* The app's pointer hint (RDX_GS_HINT from gamescope): kept in the session, so it goes into the header of
 * every frame posted from now on (RDX_HDR_F_REL_MOUSE) and survives a resume at another sink. */
static void gs_hint(struct rdx_srcsess *S, const struct rdx_gs_msg *m)
{
    if (!rdx_srcsess_gs_hint(S, m)) return;
    printf("[gs] pointer: %s\n", S->hint_rel_mouse ? "relative (the app hides the cursor and locks the pointer)" : "absolute");
    fflush(stdout);
}

/* --- the cursor next to the picture (common/rdx_cursor.h) ------------------------------------- */

/* what gamescope reported last (rdx_srcsess.h): a resumed client gets it at once, a picture that could not
 * go out yet (chunk buffers in flight) goes with the next completions */
static struct rdx_gs_cursor G_cur;

/* gamescope's packets: the fixed ones (struct rdx_gs_msg) and a picture chunk */
static union {
    struct rdx_gs_msg m;
    struct rdx_gs_ack a;
    struct rdx_gs_curimg ci;
    uint8_t b[sizeof(struct rdx_gs_curimg) + RDX_GS_CUR_CHUNK];
} g_gsbuf;

static void cur_push(struct rdx_srcsess *S) { rdx_gs_cursor_push(&G_cur, S); }

/* gamescope draws the cursor unless the current sink shows it (after a connect, a resume, a failure) */
static void cur_sync(struct rdx_srcsess *S, int gfd)
{
    int want = rdx_srcsess_cursor_up(S);
    if (want != G_cur.told) {
        if (send_msg(gfd, RDX_GS_CURSOR, (uint32_t)want, 0) == 0) G_cur.told = want;
        printf("[gs] cursor: %s\n", want ? "next to the picture: the client shows it, gamescope draws none"
                                         : "in the picture (gamescope draws it)");
        fflush(stdout);
    }
    if (want) {                        /* a new sink has nothing yet; gamescope sends its whole state as well */
        G_cur.img_dirty = G_cur.serial != 0;
        G_cur.pos_dirty = G_cur.have_pos;
        cur_push(S);
    }
}

/* a cursor packet of gamescope (n bytes in g_gsbuf): 1 = it was one; what changed goes to the sink */
static int cur_msg(struct rdx_srcsess *S, ssize_t n, struct rdx_tlog *tl)
{
    int ch = 0;
    if (n <= 0 || !rdx_gs_cursor_take(&G_cur, &g_gsbuf, (size_t)n, &ch)) return 0;
    if (ch) cur_push(S);
    if (tl && (ch & RDX_CUR_CH_POS))
        rdx_tlog_line(tl, "C,%llu,%llu,%d,%d,%d,%d", (unsigned long long)G_cur.t_pos, (unsigned long long)rdx_now_ns(),
                      G_cur.x, G_cur.y, G_cur.visible, !G_cur.pos_dirty);
    return 1;
}

/* --user: gamescope and the app run as the desktop user (Steam refuses root) with that
 * user's session environment, so the app finds its PipeWire, D-Bus and home */
struct run_user {
    const char *name;
    uid_t uid;
    gid_t gid;
    char home[256];
};

static int resolve_user(const char *name, struct run_user *u)
{
    struct passwd *pw = getpwnam(name);
    if (!pw) { fprintf(stderr, "unknown user %s\n", name); return -1; }
    u->name = name;
    u->uid = pw->pw_uid;
    u->gid = pw->pw_gid;
    snprintf(u->home, sizeof(u->home), "%s", pw->pw_dir);
    return 0;
}

static void become_user(const struct run_user *u)
{
    char rt[64], bus[128];
    if (initgroups(u->name, u->gid) || setgid(u->gid) || setuid(u->uid)) { perror("drop to user"); _exit(126); }
    snprintf(rt, sizeof(rt), "/run/user/%u", (unsigned)u->uid);
    snprintf(bus, sizeof(bus), "unix:path=%s/bus", rt);
    setenv("HOME", u->home, 1);
    setenv("USER", u->name, 1);
    setenv("LOGNAME", u->name, 1);
    setenv("XDG_RUNTIME_DIR", rt, 1);
    setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
    unsetenv("DISPLAY");                 /* gamescope brings its own Xwayland */
    unsetenv("WAYLAND_DISPLAY");
    /* the desktop's search path, not the daemon's: launchers such as /usr/games/lutris,
     * ~/.local/bin tools, snaps and flatpak exports must be found by name */
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/.local/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games:/usr/local/games:"
             "/snap/bin:%s/.local/share/flatpak/exports/bin:/var/lib/flatpak/exports/bin", u->home, u->home);
    setenv("PATH", path, 1);
    setenv("XDG_DATA_DIRS", "/usr/share/ubuntu:/usr/local/share:/usr/share:/var/lib/snapd/desktop", 0);
    if (chdir(u->home)) { /* keep the cwd */ }
}

static int g_priority;       /* -1 low, 0 normal, 1 high */

/* the seat's CPU and I/O priority, inherited by gamescope and the app; called while still
 * root (a negative nice needs it) */
static void apply_priority(void)
{
    if (!g_priority) return;
    if (setpriority(PRIO_PROCESS, 0, g_priority > 0 ? -5 : 10)) perror("setpriority");
    /* ioprio_set(IOPRIO_WHO_PROCESS, 0, class BE (2) << 13 | level) */
    if (syscall(SYS_ioprio_set, 1, 0, (2 << 13) | (g_priority > 0 ? 0 : 7))) perror("ioprio_set");
}

static pid_t spawn_gamescope(const char *gs, char **gs_args, int ngs, char **app, int napp, const char *sock_path,
                             uint32_t w, uint32_t h, int hz, const struct run_user *ru, char **envs, int nenv)
{
    char sw[16], sh[16], sr[16];
    snprintf(sw, sizeof(sw), "%u", w);
    snprintf(sh, sizeof(sh), "%u", h);
    snprintf(sr, sizeof(sr), "%d", hz);
    char *argv[MAX_GS_ARGS + 64];
    int a = 0;
    argv[a++] = (char *)gs;
    argv[a++] = "--backend"; argv[a++] = "rdx";
    argv[a++] = "-W"; argv[a++] = sw; argv[a++] = "-H"; argv[a++] = sh;
    argv[a++] = "-w"; argv[a++] = sw; argv[a++] = "-h"; argv[a++] = sh;    /* the app renders at the client mode */
    argv[a++] = "-r"; argv[a++] = sr;
    for (int i = 0; i < ngs; i++) argv[a++] = gs_args[i];
    argv[a++] = "--";
    for (int i = 0; i < napp && a < (int)(sizeof(argv) / sizeof(argv[0])) - 1; i++) argv[a++] = app[i];
    argv[a] = NULL;
    printf("[gs] starting:");
    for (int i = 0; argv[i]; i++) printf(" %s", argv[i]);
    printf("\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        apply_priority();
        if (g_priority) setenv("GAMESCOPE_VK_QUEUE_PRIORITY", g_priority > 0 ? "high" : "low", 1);
        if (ru && ru->name) become_user(ru);
        for (int i = 0; i < nenv; i++) putenv(envs[i]);     /* app env from rdx-hostd, KEY=VALUE */
        setenv("RDX_GS_SOCKET", sock_path, 1);
        /* gamescope starts its helpers (gamescopereaper) from PATH: run from a build tree too */
        const char *slash = strrchr(gs, '/');
        if (slash) {
            char path[4096];
            snprintf(path, sizeof(path), "%.*s:%s", (int)(slash - gs), gs, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
            setenv("PATH", path, 1);
        }
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execv(gs, argv);
        perror(gs);
        _exit(127);
    }
    return pid;
}

/* --- suspend / resume ---------------------------------------------------------------- */

struct gs_mode { uint32_t w, h, stride, ns; uint64_t vblank_ns; uint32_t vrr_mhz; };   /* vrr_mhz: the cap, 0 = fixed */

#ifdef RDX_WITH_FG
static struct rdx_fgs *g_fgs;          /* frame generation on a helper GPU, NULL = off */
static int g_fgs_active(void) { return g_fgs != NULL; }
#else
static int g_fgs_active(void) { return 0; }
#endif

/* gamescope's socket, for RDX_GS_INPUT around a detach (-1 before gamescope connected) */
static int g_gfd = -1;

/* a detached app waits in the background, maybe next to another app's gamescope in the stream: its
 * gamescope lets go of every input device until we attach again (RDX_GS_INPUT, gs/rdx_gs_proto.h) */
static void gs_input(int on)
{
    if (g_gfd < 0) return;
    if (send_msg(g_gfd, RDX_GS_INPUT, on ? 1 : 0, 0) == 0) printf("[gs] input: %s\n", on ? "gamescope reads the input again"
                                                           : "gamescope lets go of the input (in the background)");
    fflush(stdout);
}

static void gs_detach(struct rdx_srcsess *S, int *attached, uint32_t gs_mask, uint32_t *held_mask, uint32_t all,
                      int tfd, const char *why)
{
    if (!*attached) return;
    rdx_srcsess_disconnect(S);
    *attached = 0;
    gs_input(0);
#ifdef RDX_WITH_FG
    rdx_fgs_detach(g_fgs);
#endif
    *held_mask = all & ~gs_mask;           /* in flight or at the sink: ours again */
    struct itimerspec its = {.it_interval = {0, 100000000}, .it_value = {0, 100000000}};
    timerfd_settime(tfd, 0, &its, NULL);
    printf("[gs] DETACHED (%s): gamescope and the app keep running at 10 fps, buffers and MRs kept\n", why);
    fflush(stdout);
}

/* 0 = attached to the new sink, -1 = could not connect (stay detached),
 * 2 = attached, but its mode differs (the caller reconfigures buffers and gamescope).
 * *vrr_hz: the new client's variable refresh maximum, the optional third field ("IP PORT MAX_HZ"; an
 * older rdx-hostd writes two: 0, fixed refresh). The caller compares the cap it gives. */
static int gs_resume(struct rdx_srcsess *S, const char *sock_path, const struct gs_mode *m, double *vrr_hz)
{
    char path[512], ip[64] = "";
    int port = 0;
    snprintf(path, sizeof(path), "%s.peer", sock_path);
    FILE *f = fopen(path, "r");
    *vrr_hz = 0;
    if (!f || fscanf(f, "%63s %d", ip, &port) != 2) {
        fprintf(stderr, "[gs] RESUME: no \"IP PORT\" in %s\n", path);
        if (f) fclose(f);
        return -1;
    }
    if (fscanf(f, "%lf", vrr_hz) != 1 || !(*vrr_hz > 0)) *vrr_hz = 0;
    fclose(f);
    printf("[gs] RESUME: connecting to the sink at %s:%d\n", ip, port);
    fflush(stdout);
    if (rdx_srcsess_connect(S, ip, port)) {
        rdx_srcsess_disconnect(S);
        fprintf(stderr, "[gs] RESUME FAILED: no sink at %s:%d, still detached\n", ip, port);
        return -1;
    }
    const struct rdx_hello *h = &S->hello;
    double d = (double)h->vblank_ns - (double)m->vblank_ns;
    if (h->width != m->w || h->height != m->h || h->stride != m->stride || h->nslots != m->ns ||
        (m->vblank_ns && (d < 0 ? -d : d) > 0.001 * (double)m->vblank_ns) || (uint64_t)m->stride * m->h > h->hdr_off) {
        printf("[gs] RESUME at another mode: %ux%u (stride %u, %u slots, %.3f Hz), running %ux%u (stride %u, "
               "%u slots, %.3f Hz)\n", h->width, h->height, h->stride, h->nslots,
               h->vblank_ns ? 1e9 / (double)h->vblank_ns : 0, m->w, m->h, m->stride, m->ns,
               m->vblank_ns ? 1e9 / (double)m->vblank_ns : 0);
        fflush(stdout);
        return 2;
    }
    return 0;
}

/* new buffers for the sink in S->hello, handed to gamescope by RECONFIG; on success the old
 * buffers are freed (gamescope has released them before its ACK). 0 = ok */
/* host slots go to gamescope as dma-bufs (udmabuf) instead of memfds: a GPU other than NVIDIA,
 * whose driver cannot import a host pointer of a shared mapping (RDX_GS_FLAG_DMABUF) */
static int g_dmabuf_slots;

/* the fd gamescope gets for a slot: its memfd / opaque fd (owned by the buffer), or a new
 * dma-buf (*owned = 1: close it after sending) */
static int slot_fd(const struct rdx_gsbuf *b, int *owned)
{
    *owned = g_dmabuf_slots && b->kind == RDX_GSBUF_HOST;
    return *owned ? rdx_gsbuf_dmabuf(b) : rdx_gsbuf_fd(b);
}

/* a local window (--peer local:PATH, gs/rdx_local_proto.h) draws straight from our host slots:
 * hand it a dma-buf of each (udmabuf of the sealed memfd). 0 = ok or not local */
static int local_buffers(struct rdx_srcsess *S, const struct rdx_gsbuf *b, uint32_t n)
{
    if (!S->local) return 0;
    int fds[RDX_MAX_SLOTS] = {0};
    uint32_t k = 0;
    for (; k < n; k++)
        if ((fds[k] = rdx_gsbuf_dmabuf(&b[k])) < 0) break;
    int rc = k == n ? rdx_srcsess_local_buffers(S, fds, n) : -1;
    for (uint32_t i = 0; i < k; i++) close(fds[i]);
    if (rc) fprintf(stderr, "[gs] local window: cannot hand it the slots (host buffers and /dev/udmabuf needed)\n");
    else printf("[gs] local window: %u slots handed over (no copy, the window draws from them)\n", n);
    return rc;
}

/* the sink's refresh in mHz, from its vblank period (60 Hz if it does not say) */
static uint32_t sink_mhz(const struct rdx_hello *h)
{
    return h->vblank_ns ? (uint32_t)(1e12 / (double)h->vblank_ns + 0.5) : 60000;
}

/* the "[gs] vrr:" line; rdx-hostd shows the last one in /api/state */
static void vrr_report(uint32_t vrr_mhz, uint32_t sink, double vrr_hz, int tearing, int fg)
{
    if (vrr_mhz)
        printf("[gs] vrr: on, gamescope runs at the app's pace up to %.3f Hz (the client's screen: variable refresh "
               "up to %.3f Hz, mode %.3f Hz), no vblank pacing\n", vrr_mhz / 1000.0, vrr_hz, sink / 1000.0);
    else
        printf("[gs] vrr: off, fixed refresh %.3f Hz paced by the client's vblank: %s\n", sink / 1000.0,
               rdx_gs_vrr_off_reason(vrr_hz, tearing, fg));
    fflush(stdout);
}

/* vrr_mhz: the variable refresh cap for the new sink (rdx_gs_vrr_cap_mhz), 0 = fixed refresh */
static int gs_reconfigure(struct rdx_srcsess *S, int gfd, int kind, int cuda_ord, struct rdx_gsbuf *buf,
                          struct rdx_gs_hello *gh, uint32_t vrr_mhz)
{
    const struct rdx_hello *h = &S->hello;
    uint32_t pix = h->stride * h->height;
    if (pix > h->hdr_off) { fprintf(stderr, "[gs] RECONFIG: frame larger than the sink slot\n"); return -1; }
    struct rdx_gsbuf nb[RDX_MAX_SLOTS];
    int fds[RDX_MAX_SLOTS], own[RDX_MAX_SLOTS] = {0};
    memset(nb, 0, sizeof(nb));
    for (uint32_t i = 0; i < h->nslots; i++) {
        if (rdx_gsbuf_alloc(&nb[i], kind, cuda_ord, pix, S->pd) || (fds[i] = slot_fd(&nb[i], &own[i])) < 0) {
            for (uint32_t k = 0; k <= i; k++) {
                if (k < i && own[k]) close(fds[k]);
                rdx_gsbuf_free(&nb[k]);
            }
            return -1;
        }
    }
    struct rdx_gs_hello r = *gh;
    r.type = RDX_GS_RECONFIG;
    r.width = h->width; r.height = h->height; r.stride = h->stride; r.nslots = h->nslots;
    r.drm_fourcc = h->fourcc;
    r.refresh_mhz = vrr_mhz ? vrr_mhz : sink_mhz(h);
    r.flags = (gh->flags & ~RDX_GS_FLAG_VRR) | (vrr_mhz ? RDX_GS_FLAG_VRR : 0);
    r.slot_size = nb[0].size;
    uint64_t t0 = rdx_now_ns();
    struct rdx_gs_ack ack = {0};
    int got = 0;
    int sent = send_hello(gfd, &r, fds, r.nslots);
    for (uint32_t i = 0; i < h->nslots; i++)
        if (own[i]) close(fds[i]);                  /* gamescope has its own reference now */
    if (sent == 0) {
        /* gamescope applies it in its paint thread; READYs of old slots may come first */
        while (!got && rdx_now_ns() - t0 < 5000000000ull) {
            struct pollfd p = {.fd = gfd, .events = POLLIN};
            if (poll(&p, 1, 200) <= 0) continue;
            ssize_t n = recv(gfd, &g_gsbuf, sizeof(g_gsbuf), MSG_DONTWAIT);
            if (n == (ssize_t)sizeof(struct rdx_gs_ack) && g_gsbuf.a.magic == RDX_GS_MAGIC && g_gsbuf.a.type == RDX_GS_ACK) {
                ack = g_gsbuf.a;
                got = 1;
            } else if (n == (ssize_t)sizeof(struct rdx_gs_msg) && g_gsbuf.m.magic == RDX_GS_MAGIC && g_gsbuf.m.type == RDX_GS_HINT) {
                gs_hint(S, &g_gsbuf.m);                /* not lost while we wait for the ACK */
            } else if (n > 0 && cur_msg(S, n, NULL)) {
                /* the cursor neither */
            } else if (n <= 0 && errno != EAGAIN) break;
        }
    }
    if (!got || ack.error || ack.row_pitch != h->stride) {
        fprintf(stderr, "[gs] RECONFIG failed (%s, error %d, row pitch %u, stride %u)\n", got ? "ACK" : "no ACK in 5 s",
                ack.error, ack.row_pitch, h->stride);
        for (uint32_t k = 0; k < h->nslots; k++) rdx_gsbuf_free(&nb[k]);
        return -1;
    }
    for (uint32_t i = 0; i < RDX_MAX_SLOTS; i++)
        if (buf[i].size) rdx_gsbuf_free(&buf[i]);
    memcpy(buf, nb, sizeof(nb));
    *gh = r;
    gh->type = RDX_GS_HELLO;
    printf("[gs] RECONFIG: gamescope now %ux%u @ %.3f Hz%s, %u slots of %zu B (%s), %.1f ms\n", h->width, h->height,
           r.refresh_mhz / 1000.0, vrr_mhz ? " (variable refresh cap)" : "", h->nslots, buf[0].size,
           rdx_gsbuf_describe(&buf[0]), (rdx_now_ns() - t0) / 1e6);
    fflush(stdout);
    return 0;
}

/* --lead-file: "<ms>\n", replaced atomically (rdx-hostd reads it for hello's "stream_lead_ms") */
static void save_lead(const char *path, double ms)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%.3f\n", ms);
    if (fclose(f) == 0) rename(tmp, path);
    else unlink(tmp);
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer_ip = NULL, *sock_path = "/run/rdx-gs.sock";
    const char *gs_bin = "/opt/rdx-gs/bin/gamescope", *log_dir = RDX_TLOG_DIR;   /* host/install-gamescope.sh */
    int port = 18650, mtu = 4096, cuda_ord = 0, comp_vector = -1, pm_qos_us = 50, ngs = 0;
    double margin_ms = 1.5, sink_lead_ms = 2.0;
    char *gs_args[MAX_GS_ARGS], *envs[MAX_ENVS];
    int nenv = 0;
    const char *buffers = "auto";
    long verify = 0, nverified = 0;
    int tearing = 0;
    double vrr_hz = 0;                              /* --vrr: the client's variable refresh maximum, 0 = fixed */
    struct run_user ru = {0};
    const char *fg_mode = "off", *fg_gpu = "", *fg_quality = "auto";
    const char *gpu_pci = NULL;                     /* --gpu: any vendor, found through Vulkan */
    const char *lead_file = NULL;                   /* --lead-file: the settled lead for rdx-hostd's hello */
    int cursor_sep = 1;                             /* --cursor auto: next to the picture where the sink can */
    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"cuda", 1, 0, 'c'}, {"socket", 1, 0, 's'}, {"gamescope", 1, 0, 'g'}, {"gs-arg", 1, 0, 'a'},
        {"lead-margin-ms", 1, 0, 'L'}, {"sink-flip-lead-ms", 1, 0, 'F'}, {"log-dir", 1, 0, 'T'}, {"comp-vector", 1, 0, 'V'},
        {"pm-qos-us", 1, 0, 'Q'}, {"user", 1, 0, 'u'}, {"env", 1, 0, 'e'}, {"buffers", 1, 0, 'b'},
        {"verify", 1, 0, 'v'}, {"force-vram", 0, 0, 'f'}, {"tearing", 0, 0, 't'}, {"gpu", 1, 0, 'G'}, {"priority", 1, 0, 'R'},
        {"fg", 1, 0, 'X'}, {"fg-gpu", 1, 0, 'Y'}, {"fg-quality", 1, 0, 'Z'}, {"lead-file", 1, 0, 'D'},
        {"vrr", 1, 0, 'r'}, {"cursor", 1, 0, 'C'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'p': peer_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'c': cuda_ord = atoi(optarg); break;
        case 's': sock_path = optarg; break;
        case 'g': gs_bin = optarg; break;
        case 'a': if (ngs < MAX_GS_ARGS) gs_args[ngs++] = optarg; break;
        case 'L': margin_ms = atof(optarg); break;
        case 'F': sink_lead_ms = atof(optarg); break;
        case 'T': log_dir = optarg; break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'Q': pm_qos_us = atoi(optarg); break;
        case 'b': buffers = optarg; break;
        case 'v': verify = atol(optarg); break;
        case 'f': buffers = "vram-force"; break;
        case 't': tearing = 1; break;
        case 'r': vrr_hz = atof(optarg); break;
        case 'C':
            if (!strcmp(optarg, "picture")) cursor_sep = 0;
            else if (strcmp(optarg, "auto")) { fprintf(stderr, "--cursor auto|picture\n"); return 2; }
            break;
        case 'X': fg_mode = optarg; break;
        case 'Y': fg_gpu = optarg; break;
        case 'Z': fg_quality = optarg; break;
        case 'D': lead_file = optarg; break;
        case 'R':
            if (!strcmp(optarg, "high")) g_priority = 1;
            else if (!strcmp(optarg, "low")) g_priority = -1;
            else if (strcmp(optarg, "normal")) { fprintf(stderr, "--priority high|normal|low\n"); return 2; }
            break;
        case 'G': gpu_pci = optarg; break;
        case 'u': if (resolve_user(optarg, &ru)) return 2; break;
        case 'e':
            if (!strchr(optarg, '=') || nenv >= MAX_ENVS) { fprintf(stderr, "--env KEY=VALUE (max %d)\n", MAX_ENVS); return 2; }
            envs[nenv++] = optarg;
            break;
        default:  fprintf(stderr, "see the header of rdx_gs_src.c for options\n"); return 2;
        }
    }
    if (!peer_ip) { fprintf(stderr, "need --peer\n"); return 2; }
    if (rdx_srcsess_is_local_peer(peer_ip)) {
        /* a window on this computer (p0b/rdx_sink_win --local) draws from our slots: host RAM */
        if (strcmp(buffers, "auto") && strcmp(buffers, "host")) fprintf(stderr, "[gs] local window: host buffers\n");
        buffers = "host";
        if (strcmp(fg_mode, "off")) { printf("[fg] frame generation: not for a local window yet, off\n"); fg_mode = "off"; }
    }
    char **app = argv + optind;
    int napp = argc - optind;

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGPIPE);
    sigaddset(&sigs, SIGUSR1);                     /* resume: connect to the sink in <socket>.peer */
    sigaddset(&sigs, SIGUSR2);                     /* detach from the sink, keep gamescope running */
    sigaddset(&sigs, SIGRTMIN);                    /* sigqueue value: the client's new variable refresh max in mHz */
    sigprocmask(SIG_BLOCK, &sigs, NULL);
    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC | SFD_NONBLOCK);

    /* 1. the sink decides size, stride, slots and refresh */
    struct rdx_srcsess S;
    {                                               /* = rdx_srcsess_open, with the cursor setting before the connect */
        char lip[64] = "";
        if (!rdx_srcsess_is_local_peer(peer_ip)) rdx_local_ip_for(peer_ip, lip, sizeof(lip));
        if (rdx_srcsess_init(&S, dev, lip[0] ? lip : NULL, mtu, comp_vector)) return 1;
        S.want_cursor = S.want_cursor && cursor_sep;
        if (rdx_srcsess_connect(&S, peer_ip, port)) return 1;
    }
    dev = ibv_get_device_name(S.ctx->device);      /* "auto" resolved: the NIC placement check needs the name */
    uint32_t w = S.hello.width, h = S.hello.height, ns = S.hello.nslots, stride = S.hello.stride;
    uint32_t pix_bytes = stride * h;
    if (pix_bytes > S.hello.hdr_off) { fprintf(stderr, "frame larger than sink slot\n"); return 1; }
    uint32_t refresh_mhz = S.hello.vblank_ns ? (uint32_t)(1e12 / (double)S.hello.vblank_ns + 0.5) : 60000;

    /* 2. the GPU. --gpu PCI: the Vulkan device at that address, any vendor; CUDA (VRAM buffers,
     *    the frame generation's staging copy) only for NVIDIA. Any other GPU composites into host
     *    buffers through gamescope's Vulkan path (VK_EXT_external_memory_host) and needs no CUDA. */
    uint8_t gpu_uuid[16];
    int nvidia = 1;
    if (gpu_pci) {
        uint32_t vendor = 0;
        char gname[256];
        if (vk_find_pci(gpu_pci, gpu_uuid, &vendor, gname, sizeof(gname))) {
            fprintf(stderr, "[gs] no Vulkan GPU at %s\n", gpu_pci);
            return 2;
        }
        nvidia = vendor == 0x10de;
        printf("[gs] GPU %s: %s%s\n", gpu_pci, gname, nvidia ? "" : " (not NVIDIA: host buffers, no CUDA)");
        if (nvidia && (cuda_ord = rdx_vmm_ordinal_for_pci(gpu_pci)) < 0) return 2;
        if (!nvidia) {
            if (strcmp(buffers, "auto") && strcmp(buffers, "host"))
                fprintf(stderr, "[gs] --buffers %s needs CUDA: host buffers on this GPU\n", buffers);
            buffers = "host";
            cuda_ord = -1;
        }
    }
    /* one output buffer per sink slot: the NIC's MR + the fd gamescope imports (rdx_gsbuf.h) */
    int kind = rdx_gsbuf_choose(buffers, cuda_ord, dev);
    if (kind == -1) { fprintf(stderr, "--buffers auto|host|vram (--force-vram skips the safety rule)\n"); return 2; }
    if (kind < 0) return 2;
    if (nvidia) {
        char bdf[32];
        int dmabuf = 0;
        if (rdx_vmm_probe(cuda_ord, NULL, 0, gpu_uuid, &dmabuf, bdf, sizeof(bdf))) return 1;
    }
    struct rdx_gsbuf buf[RDX_MAX_SLOTS];
    int fds[RDX_MAX_SLOTS], own[RDX_MAX_SLOTS] = {0};
    memset(buf, 0, sizeof(buf));
    g_dmabuf_slots = !nvidia && kind == RDX_GSBUF_HOST;
    for (uint32_t i = 0; i < ns; i++) {
        if (rdx_gsbuf_alloc(&buf[i], kind, cuda_ord, pix_bytes, S.pd)) return 1;
        if ((fds[i] = slot_fd(&buf[i], &own[i])) < 0) {
            fprintf(stderr, "[gs] no dma-buf of the host slots for this GPU (/dev/udmabuf, modprobe udmabuf)\n");
            return 1;
        }
    }
    printf("[gs] %u output buffers of %zu B: %s\n", ns, buf[0].size, rdx_gsbuf_describe(&buf[0]));
    if (local_buffers(&S, buf, ns)) return 1;
    if (verify && kind != RDX_GSBUF_HOST) { fprintf(stderr, "--verify needs host buffers\n"); verify = 0; }
    uint32_t gs_ns = ns, gs_refresh_mhz = refresh_mhz;    /* what gamescope gets */
#ifdef RDX_WITH_FG
    if (!strcmp(fg_mode, "helper") && tearing) printf("[fg] frame generation needs vblank pacing: off in tearing mode\n");
    else if (!strcmp(fg_mode, "helper")) {
        struct rdx_fgs_cfg fc = {.helper_pci = fg_gpu, .quality = fg_quality, .width = w, .height = h, .stride = stride,
                                 .sink_slots = ns, .buf_kind = kind, .cuda_ord = cuda_ord};
        g_fgs = rdx_fgs_open(&fc, &S, buf);
    }
#else
    if (strcmp(fg_mode, "off")) printf("[fg] built without frame generation (gs/fg.mk): --fg %s ignored\n", fg_mode);
    (void)fg_gpu; (void)fg_quality;
#endif
    /* variable refresh (gs/rdx_gs_vrr.h): gamescope's refresh is then the cap, 0 = fixed refresh */
    uint32_t vrr_mhz = rdx_gs_vrr_cap_mhz(vrr_hz, refresh_mhz, tearing, g_fgs_active());
    if (vrr_mhz) gs_refresh_mhz = vrr_mhz;
    vrr_report(vrr_mhz, refresh_mhz, vrr_hz, tearing, g_fgs_active());

    /* 3. gamescope connects to our socket */
    int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    unlink(sock_path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(lfd, 1)) { perror(sock_path); return 1; }
    if (ru.name && (chown(sock_path, ru.uid, ru.gid) || chmod(sock_path, 0600))) { perror("chown socket"); return 1; }
    pid_t child = -1;
    int pidfd = -1;
    char vk_ids[16];
    /* gamescope picks exactly this GPU by UUID (patch); --prefer-vk-device as the hint for
     * builds without it */
    char uuid_hex[33];
    for (int i = 0; i < 16; i++) snprintf(uuid_hex + 2 * i, 3, "%02x", gpu_uuid[i]);
    setenv("GAMESCOPE_VK_DEVICE_UUID", uuid_hex, 1);
    if (vk_ids_for_uuid(gpu_uuid, vk_ids, sizeof(vk_ids)) == 0 && ngs + 2 <= MAX_GS_ARGS) {
        gs_args[ngs++] = "--prefer-vk-device";
        gs_args[ngs++] = vk_ids;
    }
    if (tearing && ngs < MAX_GS_ARGS)
        gs_args[ngs++] = "--immediate-flips";       /* present as soon as the app does, tearing allowed */
    if (napp > 0) {
        child = spawn_gamescope(gs_bin, gs_args, ngs, app, napp, sock_path, w, h, (int)((gs_refresh_mhz + 500) / 1000),
                                &ru, envs, nenv);
        if (child < 0) { perror("fork"); return 1; }
        pidfd = (int)syscall(SYS_pidfd_open, child, 0);
    } else {
        printf("[gs] waiting for gamescope --backend rdx on %s (RDX_GS_SOCKET)\n", sock_path);
    }
    struct pollfd ap[3] = {{.fd = lfd, .events = POLLIN}, {.fd = pidfd, .events = POLLIN}, {.fd = sfd, .events = POLLIN}};
    if (poll(ap, 3, 60000) <= 0 || !(ap[0].revents & POLLIN)) {
        fprintf(stderr, "gamescope did not connect\n"); return 1;
    }
    int gfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (gfd < 0) { perror("accept"); return 1; }
    g_gfd = gfd;

    struct rdx_gs_hello gh = {.magic = RDX_GS_MAGIC, .type = RDX_GS_HELLO, .width = w, .height = h, .stride = stride,
                              .drm_fourcc = S.hello.fourcc, .nslots = gs_ns, .refresh_mhz = gs_refresh_mhz,
                              .slot_size = buf[0].size,
                              .buf_kind = kind == RDX_GSBUF_HOST ? RDX_GS_BUF_HOST : RDX_GS_BUF_VRAM,
                              .flags = (tearing ? RDX_GS_FLAG_TEARING : 0) | (g_dmabuf_slots ? RDX_GS_FLAG_DMABUF : 0) |
                                       (vrr_mhz ? RDX_GS_FLAG_VRR : 0)};
    memcpy(gh.gpu_uuid, gpu_uuid, 16);
    struct rdx_gs_ack ack;
    int sent = send_hello(gfd, &gh, fds, gs_ns);
    for (uint32_t i = 0; i < ns; i++)
        if (own[i]) close(fds[i]);                  /* gamescope has its own reference now */
    if (sent || recv(gfd, &ack, sizeof(ack), 0) != (ssize_t)sizeof(ack) ||
        ack.magic != RDX_GS_MAGIC || ack.type != RDX_GS_ACK) {
        fprintf(stderr, "gamescope handshake failed\n"); return 1;
    }
    if (ack.error || ack.row_pitch != stride) {
        fprintf(stderr, "gamescope could not import the buffers (error %d, row pitch %u, sink stride %u)\n",
                ack.error, ack.row_pitch, stride);
        return 1;
    }
    printf("[gs] gamescope imported %u slots, row pitch %u = sink stride, refresh %.3f Hz%s%s\n", gs_ns, ack.row_pitch,
           gs_refresh_mhz / 1000.0, vrr_mhz ? " (variable refresh cap)" : "", kind != RDX_GSBUF_HOST ? ""
           : ack.path ? ", host buffers: composite in VRAM + one GPU copy"
                      : ", host buffers: composite straight into them");
    /* slot ownership: gs_mask = given to gamescope (FREE sent, no READY yet); held_mask = ours
     * while detached (neither at gamescope nor at a sink) */
    uint32_t gs_mask = 0, held_mask = 0, all_mask = gs_ns >= 32 ? 0xFFFFFFFFu : (1u << gs_ns) - 1;
    for (uint32_t i = 0; i < gs_ns; i++) { send_msg(gfd, RDX_GS_FREE, i, 0); gs_mask |= 1u << i; }
    cur_sync(&S, gfd);                             /* the cursor next to the picture, if the sink took the channel */
    struct gs_mode run_mode = {w, h, stride, ns, S.hello.vblank_ns, vrr_mhz};
    uint32_t gen = 0;                              /* slot generation, READY carries it */
    int attached = 1, tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    long detaches = 0, resumes = 0;

    /* 4. event loop: sink completions, gamescope messages, child exit, signals */
    struct rdx_tlog tl;
    rdx_tlog_open(&tl, log_dir, "gs-src",
                  "F,seq,slot,t_begin,t_ready,t_post\n"
                  "S,seq,t_report,round_us,sink_us,shown\n"
                  "D,seq,t_report\n"
                  "X,t,status,frames_lost,recovery_ms,dmabuf_via\n"
                  "P,seq,slot,sge_addr,len,lkey,mr_addr,mr_len,psn\n"
                  "C,t_move,t_recv,x,y,visible,sent");
    S.tl = &tl;
    int qos_fd = rdx_pm_qos(pm_qos_us);
    long cap = 36000, nround = 0, ngpu = 0, nq = 0, seq = 0, vblanks = 0, nwait = 0;
    double *waitv = calloc((size_t)cap, sizeof(double));
    /* start with half a period plus the target; the loop below settles it */
    double target_wait_ns = (sink_lead_ms + margin_ms) * 1e6, lead_ns = target_wait_ns + 0.5 * S.period_ns;
    double *round = calloc((size_t)cap, sizeof(double)), *gpu = calloc((size_t)cap, sizeof(double));
    double *queue = calloc((size_t)cap, sizeof(double));
    uint64_t t0 = rdx_now_ns(), t0_cpu = rdx_cpu_ns(), t_sec = t0;
    long lead_shown = 0;                          /* S.shown at the last --lead-file write */
    int stop = 0, reason = 0;
    struct rdx_localinput LI;                     /* a local window's keyboard / mouse (uinput) */
    rdx_localinput_init(&LI);
    uint32_t mode_w = 0, mode_h = 0;               /* a local window's new size, 0 = unchanged */
    uint64_t t_recover[5] = {0};                       /* last recoveries, for the give-up rule */
    /* variable refresh: a READY that comes sooner than one cap period after the previous post waits for
     * that moment (rdx_gs_vrr_hold_until, vtfd); a newer READY meanwhile replaces it (its slot goes back) */
    uint64_t vrr_period = rdx_gs_vrr_period_ns(vrr_mhz), last_post = 0;
    int vtfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK), have_pend = 0, take_pend = 0;
    struct rdx_gs_msg pend = {0};
    long vrr_held = 0, vrr_replaced = 0, nivl = 0;
    double *ivl = calloc((size_t)cap, sizeof(double));  /* post -> next post (ms), variable refresh */
    while (!stop) {
        struct rdx_ev ev[16];
        int n = 0;
        if (attached) {
            n = rdx_srcsess_poll(&S, ev, 16);
            if (n < 0) { reason = 1; break; }
            if (n == 0) {
                if (rdx_srcsess_arm(&S)) { perror("req_notify_cq"); reason = 1; break; }
                n = rdx_srcsess_poll(&S, ev, 16);          /* closes the arm race */
                if (n < 0) { reason = 1; break; }
            }
        }
        if (attached && n == 0 && S.need_recover && S.err_status != IBV_WC_LOC_PROT_ERR) {
            /* retry exceeded, RNR, remote errors: the sink is gone or unreachable */
            char why[96];
            snprintf(why, sizeof(why), "sink unreachable: %s", ibv_wc_status_str(S.err_status));
            gs_detach(&S, &attached, gs_mask, &held_mask, all_mask, tfd, why);
            detaches++;
            continue;
        }
        if (attached && n == 0 && S.need_recover) {
            /* A work request failed locally (seen: local protection error on the pixel write
             * from VRAM while the GPU was under memory pressure). Every in-flight frame is lost,
             * the session is not: fresh dma-bufs and MRs for all slots, the QP back to RTS on
             * the same sink QP, the lost slots back to gamescope. The sink misses those frames. */
            if (kind == RDX_GSBUF_VRAM) {
                /* A failed read of VRAM by the NIC is a stalled peer read (seen: vendor_err 0x51 after
                 * ~60 ms). Posting again keeps a stalled GPU under peer reads; on the test host that ended in
                 * Xid 62 / "GPU requires reset". End the session instead. */
                fprintf(stderr, "[gs] %s on a VRAM buffer: no re-post, ending the session\n",
                        ibv_wc_status_str(S.err_status));
                reason = 1; break;
            }
            uint64_t tr0 = rdx_now_ns();
            if (t_recover[0] && tr0 - t_recover[0] < 2000000000ull) {
                fprintf(stderr, "[gs] 5 recoveries within 2 s, giving up\n");
                reason = 1; break;
            }
            memmove(t_recover, t_recover + 1, sizeof(t_recover) - sizeof(t_recover[0]));
            t_recover[4] = tr0;
            enum ibv_wc_status st = S.err_status;
            int ok = 1;
            for (uint32_t i = 0; i < ns && ok; i++)
                if (rdx_gsbuf_reregister(&buf[i], S.pd)) ok = 0;
            uint32_t lost[RDX_MAX_SLOTS * 2];
            int nl = ok ? rdx_srcsess_recover(&S, lost, RDX_MAX_SLOTS * 2) : -1;
            if (nl < 0) { fprintf(stderr, "[gs] recovery failed\n"); reason = 1; break; }
            for (int i = 0; i < nl && !g_fgs_active(); i++) {
                send_msg(gfd, RDX_GS_FREE, lost[i], 0);
                gs_mask |= 1u << lost[i];
            }
#ifdef RDX_WITH_FG
            if (g_fgs) {                                /* sink slots are not gamescope's slots here */
                rdx_fgs_detach(g_fgs);                  /* its queued posts were for the lost frames */
                for (uint32_t sl = 0; sl < gs_ns; sl++)
                    if (!(gs_mask & (1u << sl))) { send_msg(gfd, RDX_GS_FREE, sl, 0); gs_mask |= 1u << sl; }
            }
#endif
            double ms = (rdx_now_ns() - tr0) / 1e6;
            printf("[gs] RECOVERY %ld after \"%s\": %d frames lost, %u MRs registered again (%s), %.2f ms\n",
                   S.recoveries, ibv_wc_status_str(st), nl, ns, rdx_gsbuf_describe(&buf[0]), ms);
            fflush(stdout);
            rdx_tlog_line(&tl, "X,%llu,%d,%d,%.3f,%d", (unsigned long long)tr0, (int)st, nl, ms,
                          kind == RDX_GSBUF_HOST ? -1 : (int)buf[0].vm.via);
            continue;
        }
        for (int k = 0; k < n; k++) {
            struct rdx_ev *e = &ev[k];
            if (e->type == RDX_EV_CREDIT) {
#ifdef RDX_WITH_FG
                if (g_fgs) {                           /* a pool: the sink slot carried some gamescope slot or a generated frame */
                    int gsl = rdx_fgs_credit(g_fgs, e->slot, &seq);
                    if (gsl == -2) { stop = 1; reason = 1; continue; }
                    if (gsl < 0) continue;
                    e->slot = (uint32_t)gsl;
                }
#endif
                gs_mask |= 1u << e->slot;
                /* tearing: the credit comes right after arrival, its round trip is post -> on screen */
                if (tearing && e->round_us > 0 && nround < cap) round[nround++] = e->round_us / 1e3;
                if (send_msg(gfd, RDX_GS_FREE, e->slot, 0)) { stop = 1; reason = 2; }
            } else if (e->type == RDX_EV_SHOWN) {
                rdx_tlog_line(&tl, "S,%ld,%llu,%.1f,%.1f,1", e->seq, (unsigned long long)e->t_ns, e->round_us, e->sink_us);
                if (e->round_us > 0 && nround < cap) round[nround++] = e->round_us / 1e3;
                int lead_frame = 1;
#ifdef RDX_WITH_FG
                lead_frame = rdx_fgs_lead_frame(g_fgs, e->seq);   /* a picture held behind its generated frame: no */
#endif
                /* variable refresh: the client shows each frame on arrival, there is no grid to aim at and
                 * gamescope gets no VBLANK; the wait is then arrival -> on screen */
                if (vrr_mhz) {
                    if (e->round_us > 0 && nwait < cap) waitv[nwait++] = e->sink_us / 1e3;
                    continue;
                }
                /* gamescope's vblank: the sink's grid, earlier by network time and margin */
                if (e->round_us > 0 && lead_frame) {
                    /* closed loop on the sink's arrival -> vblank wait, error modulo one period */
                    double err = e->sink_us * 1e3 - target_wait_ns;
                    err = fmod(err, S.period_ns);
                    if (err > S.period_ns / 2) err -= S.period_ns;
                    else if (err < -S.period_ns / 2) err += S.period_ns;
                    lead_ns -= 0.2 * err;
                    if (lead_ns < 0) lead_ns += S.period_ns;
                    if (lead_ns > 4 * S.period_ns) lead_ns -= S.period_ns;
                    if (nwait < cap) waitv[nwait++] = e->sink_us / 1e3;
                }
                uint64_t v = rdx_srcsess_vblank_before(&S, rdx_now_ns(), lead_ns);
                /* tearing: the client shows on arrival, gamescope must not wait for its vblank */
                if (v && !tearing && send_msg(gfd, RDX_GS_VBLANK, 0, v) == 0) vblanks++;
            } else if (e->type == RDX_EV_DROP) {
                rdx_tlog_line(&tl, "D,%ld,%llu", e->seq, (unsigned long long)e->t_ns);
            } else if (e->type == RDX_EV_INPUT) {          /* the local window's keyboard / mouse */
                if (e->in_kind == 0) rdx_localinput_release(&LI);
                else rdx_localinput_event(&LI, e->in_kind, e->in_code, e->in_value);
            } else if (e->type == RDX_EV_MODE) {           /* the local window was resized */
                mode_w = e->w & ~1u;
                mode_h = e->h & ~1u;
            }
        }
        /* local window resized: gamescope renders at the window's size (RECONFIG, new slots,
         * the window imports them), instead of the window scaling a frame of another size */
        if (mode_w && attached && S.local && !tearing && (mode_w != w || mode_h != h) &&
            mode_w >= 320 && mode_h >= 200 && mode_w <= 7680 && mode_h <= 4320) {
            struct rdx_hello old = S.hello;
            S.hello.width = mode_w;
            S.hello.height = mode_h;
            S.hello.stride = (mode_w * 4 + 255) & ~255u;
            S.hello.hdr_off = ((uint64_t)S.hello.stride * mode_h + 4095) & ~4095ull;
            S.hello.slot_bytes = (S.hello.hdr_off + sizeof(struct rdx_frame_hdr) + 4095) & ~4095ull;
            printf("[gs] local window resized: %ux%u -> %ux%u\n", w, h, mode_w, mode_h);
            fflush(stdout);
            if (gs_reconfigure(&S, gfd, kind, cuda_ord, buf, &gh, vrr_mhz)) {
                S.hello = old;                             /* keep the old buffers and size */
            } else {
                w = S.hello.width; h = S.hello.height; stride = S.hello.stride;
                pix_bytes = stride * h;
                run_mode = (struct gs_mode){w, h, stride, ns, S.hello.vblank_ns, vrr_mhz};
                gs_mask = all_mask;                        /* gamescope has the new slots, all free */
                held_mask = 0;
                gen++;
                for (uint32_t sl = 0; sl < ns; sl++) send_msg(gfd, RDX_GS_FREE, sl, 0);
                local_buffers(&S, buf, ns);
            }
        }
        if (mode_w) mode_w = mode_h = 0;
        if (n > 0) continue;                               /* drain everything before sleeping */

        int fg_fd = -1;
#ifdef RDX_WITH_FG
        fg_fd = rdx_fgs_fd(g_fgs);
#endif
        if (!attached) have_pend = 0;                    /* a held frame's slot is ours again (held_mask) */
        if (attached && G_cur.told && !rdx_srcsess_cursor_up(&S)) cur_sync(&S, gfd);   /* the channel failed */
        if (attached) cur_push(&S);                      /* a picture held back for buffers in flight */
        struct pollfd p[8] = {{.fd = attached ? rdx_srcsess_fd(&S) : -1, .events = POLLIN}, {.fd = gfd, .events = POLLIN},
                              {.fd = sfd, .events = POLLIN}, {.fd = pidfd, .events = POLLIN},
                              {.fd = attached ? S.sock : -1, .events = POLLIN}, {.fd = tfd, .events = POLLIN},
                              {.fd = fg_fd, .events = POLLIN}, {.fd = have_pend ? vtfd : -1, .events = POLLIN}};
        int r = poll(p, 8, 1000);
#ifdef RDX_WITH_FG
        if ((p[6].revents & POLLIN) && attached && rdx_fgs_service(g_fgs, &seq)) { reason = 1; break; }
#endif
        if (r < 0 && errno != EINTR) { perror("poll"); reason = 1; break; }
        if (p[0].revents & POLLIN) rdx_srcsess_ack(&S);
        int want_resume = 0, want_vrr = 0;
        double new_vrr = vrr_hz;
        if (p[2].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGUSR1) want_resume = 1;
                else if (si.ssi_signo == (uint32_t)SIGRTMIN) {
                    /* rdx-hostd: the client's screen changed during the session (session.update "display") */
                    want_vrr = 1;
                    new_vrr = si.ssi_int > 0 ? si.ssi_int / 1000.0 : 0;
                }
                else if (si.ssi_signo == SIGUSR2) {
                    gs_detach(&S, &attached, gs_mask, &held_mask, all_mask, tfd, "requested (SIGUSR2)");
                    detaches++;
                } else if (si.ssi_signo != SIGPIPE) { stop = 1; reason = 3; }
            }
        }
        if (attached && (p[4].revents & (POLLIN | POLLHUP | POLLERR))) {
            char b;
            ssize_t k = recv(S.sock, &b, 1, MSG_DONTWAIT | MSG_PEEK);
            if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                gs_detach(&S, &attached, gs_mask, &held_mask, all_mask, tfd, "the sink closed its connection");
                detaches++;
            }
        }
        if (p[5].revents & POLLIN) {
            uint64_t exp;
            if (read(tfd, &exp, sizeof(exp)) > 0 && !attached && held_mask) {
                uint32_t sl = (uint32_t)__builtin_ctz(held_mask);   /* 10 fps while nobody watches */
                held_mask &= ~(1u << sl);
                gs_mask |= 1u << sl;
                send_msg(gfd, RDX_GS_FREE, sl, 0);
            }
        }
        if (want_resume) {
            if (attached) {
                gs_detach(&S, &attached, gs_mask, &held_mask, all_mask, tfd, "resume to a new sink");
                detaches++;
            }
            int rc = gs_resume(&S, sock_path, &run_mode, &new_vrr);
            /* the new client's variable refresh (third field of <socket>.peer): a new mode carries it in its
             * RECONFIG, at the same mode only VRR changes (want_vrr below, the same buffers) */
            uint32_t new_vrr_mhz = rdx_gs_vrr_cap_mhz(new_vrr, sink_mhz(&S.hello), tearing, g_fgs_active());
            if (rc == 2) {
#ifdef RDX_WITH_FG
                int fg_again = g_fgs != NULL;              /* rebuilt below for the new mode */
                if (g_fgs) {
                    rdx_fgs_report(g_fgs);
                    rdx_fgs_close(g_fgs);
                    g_fgs = NULL;
                }
#endif
                if (gs_reconfigure(&S, gfd, kind, cuda_ord, buf, &gh, new_vrr_mhz)) {
                    rdx_srcsess_disconnect(&S);
                    stop = 1; reason = 5;
                } else {
                    w = S.hello.width; h = S.hello.height; stride = S.hello.stride; ns = S.hello.nslots;
                    pix_bytes = stride * h;
                    refresh_mhz = sink_mhz(&S.hello);
                    vrr_mhz = new_vrr_mhz;
                    run_mode = (struct gs_mode){w, h, stride, ns, S.hello.vblank_ns, vrr_mhz};
                    gs_ns = ns;
#ifdef RDX_WITH_FG
                    if (fg_again) {                        /* frame generation for the new size */
                        struct rdx_fgs_cfg fc = {.helper_pci = fg_gpu, .quality = fg_quality, .width = w, .height = h,
                                                 .stride = stride, .sink_slots = ns, .buf_kind = kind, .cuda_ord = cuda_ord};
                        g_fgs = rdx_fgs_open(&fc, &S, buf);
                    }
#endif
                    all_mask = gs_ns >= 32 ? 0xFFFFFFFFu : (1u << gs_ns) - 1;
                    gs_mask = 0;
                    held_mask = all_mask;                /* gamescope starts with no free slot */
                    gen++;
                    rc = 0;
                }
            }
            if (rc == 0) {
                want_vrr = 1;                              /* the line for rdx-hostd, and a changed VRR */
                vrr_period = rdx_gs_vrr_period_ns(vrr_mhz);
                last_post = 0;
                attached = 1;
                local_buffers(&S, buf, ns);                /* a local window imports the slots (again) */
                resumes++;
                struct itimerspec off = {{0, 0}, {0, 0}};
                timerfd_settime(tfd, 0, &off, NULL);
                lead_ns = target_wait_ns + 0.5 * S.period_ns;
                for (uint32_t sl = 0; sl < gs_ns; sl++)       /* the new sink has every slot free */
                    if (held_mask & (1u << sl)) { gs_mask |= 1u << sl; send_msg(gfd, RDX_GS_FREE, sl, 0); }
                held_mask = 0;
                printf("[gs] RESUMED (resume %ld): streaming to the new sink at %ux%u\n", resumes, w, h);
                fflush(stdout);
                gs_input(1);
                cur_sync(&S, gfd);                         /* this client may show the cursor itself, or not */
            }
        }
        if (want_vrr && !stop) {
            /* variable refresh on / off / another cap with the same buffers: RDX_GS_VRR to gamescope, which
             * switches its refresh and adaptive sync in its paint thread; our pacing follows at once */
            uint32_t nm = rdx_gs_vrr_cap_mhz(new_vrr, sink_mhz(&S.hello), tearing, g_fgs_active());
            if (nm != vrr_mhz) {
                uint32_t gs_mhz = nm ? nm : sink_mhz(&S.hello);
                if (send_msg(gfd, RDX_GS_VRR, nm ? 1 : 0, gs_mhz)) { stop = 1; reason = 2; }
                printf("[gs] variable refresh changes: cap %.3f -> %.3f Hz (0 = fixed), gamescope at %.3f Hz\n",
                       vrr_mhz / 1000.0, nm / 1000.0, gs_mhz / 1000.0);
                vrr_mhz = nm;
                run_mode.vrr_mhz = nm;
                gh.refresh_mhz = gs_mhz;                   /* a later RECONFIG keeps it */
                gh.flags = (gh.flags & ~RDX_GS_FLAG_VRR) | (nm ? RDX_GS_FLAG_VRR : 0);
                vrr_period = rdx_gs_vrr_period_ns(nm);
                last_post = 0;
                lead_ns = target_wait_ns + 0.5 * S.period_ns;   /* back on the client's grid: settle again */
            }
            vrr_hz = new_vrr;
            vrr_report(vrr_mhz, sink_mhz(&S.hello), vrr_hz, tearing, g_fgs_active());
        }
        if (pidfd >= 0 && (p[3].revents & POLLIN)) { stop = 1; reason = 4; }
        if (p[1].revents & (POLLHUP | POLLERR)) { stop = 1; reason = 4; }
        if (p[7].revents & POLLIN) {                       /* a held frame (variable refresh) is due */
            uint64_t exp;
            if (read(vtfd, &exp, sizeof(exp)) < 0) { /* not armed any more */ }
        }
        if (have_pend && !rdx_gs_vrr_hold_until(last_post, rdx_now_ns(), vrr_period)) take_pend = 1;
        if ((p[1].revents & POLLIN) || take_pend) {
            struct rdx_gs_msg m;
            for (;;) {
                int from_pend = take_pend;                 /* the held frame first: it is older */
                if (take_pend) { m = pend; have_pend = take_pend = 0; }
                else {
                    ssize_t gn = recv(gfd, &g_gsbuf, sizeof(g_gsbuf), MSG_DONTWAIT);
                    if (gn > 0 && cur_msg(&S, gn, &tl)) continue;     /* the cursor, next to the picture */
                    if (gn != (ssize_t)sizeof(m)) break;
                    m = g_gsbuf.m;
                }
                if (m.magic != RDX_GS_MAGIC) continue;
                if (m.type == RDX_GS_BYE) { stop = 1; reason = 4; break; }
                if (m.type == RDX_GS_HINT) { gs_hint(&S, &m); continue; }
                if (m.type != RDX_GS_READY) continue;
                if (m.pad != gen) continue;                /* a slot of the buffers before a RECONFIG */
                if (m.slot >= ns) {                        /* never post what is not one of our slots */
                    fprintf(stderr, "[gs] READY for slot %u (only %u slots): dropped\n", m.slot, ns);
                    continue;
                }
                gs_mask &= ~(1u << m.slot);
                if (!attached) {                           /* nobody watches: keep the slot for the 10 fps timer */
                    held_mask |= 1u << m.slot;
                    continue;
                }
                if (S.need_recover) {                      /* QP in error: skip, slot back to gamescope */
                    gs_mask |= 1u << m.slot;
                    send_msg(gfd, RDX_GS_FREE, m.slot, 0);
                    continue;
                }
                if (vrr_period && !from_pend) {
                    /* variable refresh: never sooner than one cap period after the previous post (the client's
                     * screen cannot refresh faster); wait for it, a newer frame meanwhile replaces this one */
                    uint64_t due = rdx_gs_vrr_hold_until(last_post, rdx_now_ns(), vrr_period);
                    if (due) {
                        if (have_pend) {
                            gs_mask |= 1u << pend.slot;
                            send_msg(gfd, RDX_GS_FREE, pend.slot, 0);
                            vrr_replaced++;
                        }
                        pend = m;
                        have_pend = 1;
                        vrr_held++;
                        struct itimerspec its = {{0, 0}, {(time_t)(due / 1000000000ull), (long)(due % 1000000000ull)}};
                        timerfd_settime(vtfd, TFD_TIMER_ABSTIME, &its, NULL);
                        continue;
                    }
                }
                uint64_t t_post = rdx_now_ns();
                if (verify && seq % verify == 0) {
                    /* what the CPU sees in the host buffer right after READY; the sink hashes what
                     * the NIC delivered (rdx_sink_null --hash): equal = complete and coherent */
                    uint64_t hsh = rdx_frame_hash(buf[m.slot].host, w, h, stride);
                    printf("[hash] %ld %016llx\n", seq, (unsigned long long)hsh);
                    nverified++;
                }
                int pr;
#ifdef RDX_WITH_FG
                if (g_fgs) {                               /* the stage posts; the same picture again (1): slot back */
                    int fr = rdx_fgs_ready(g_fgs, m.slot, &seq);
                    pr = fr < 0 ? -1 : fr == 1 ? -2 : 0;
                } else
#endif
                pr = rdx_srcsess_post(&S, m.slot, seq, buf[m.slot].mr, rdx_gsbuf_addr(&buf[m.slot]), pix_bytes);
                if (pr == -2) {                            /* refused by the bounds check: drop, slot back */
                    gs_mask |= 1u << m.slot;
                    send_msg(gfd, RDX_GS_FREE, m.slot, 0);
                    continue;
                }
                if (pr) { stop = 1; reason = 1; break; }
                rdx_tlog_line(&tl, "F,%ld,%u,%llu,%llu,%llu", seq, m.slot, (unsigned long long)m.t_begin_ns,
                              (unsigned long long)m.t_ns, (unsigned long long)t_post);
                /* a paint outside a vblank wake-up carries an older wake-up time: count only real ones */
                if (m.t_begin_ns && m.t_begin_ns <= m.t_ns && m.t_ns - m.t_begin_ns < 1000000000ull && ngpu < cap)
                    gpu[ngpu++] = (double)(m.t_ns - m.t_begin_ns) / 1e6;
                if (nq < cap) queue[nq++] = (double)(t_post - m.t_ns) / 1e6;
                if (vrr_period) {
                    if (last_post && nivl < cap) ivl[nivl++] = (double)(t_post - last_post) / 1e6;
                    last_post = t_post;
                }
#ifdef RDX_WITH_FG
                if (g_fgs) continue;                       /* the stage counts its own posts */
#endif
                seq++;
            }
        }
        uint64_t tnow = rdx_now_ns();
        rdx_tlog_tick(&tl, tnow);
        if (tnow - t_sec >= 1000000000ull) {
            rdx_cq_clock_sync(&S.rcq);
            t_sec = tnow;
            /* the settled lead, once per second of shown frames: the next session's sink slots
             * (rdx_pace_slots in common/rdx_ctl.h, through rdx-hostd's hello) */
            if (lead_file && attached && !tearing && !vrr_mhz && S.period_ns > 0 && S.shown - lead_shown >= (long)(1e9 / S.period_ns)) {
                save_lead(lead_file, lead_ns / 1e6);
                lead_shown = S.shown;
            }
        }
        if (nround >= cap || ngpu >= cap || nivl >= cap) {                /* endless: rolling summary */
            printf("[gs] window: %ld frames posted\n", seq);
            rdx_report("gs wake -> GPU done (ms)", gpu, (size_t)ngpu);
            rdx_report("post -> shown report (ms)", round, (size_t)nround);
            rdx_cpu_report("gs-src", t0, t0_cpu);
            fflush(stdout);
            if (vrr_mhz) rdx_report("post -> next post (ms)", ivl, (size_t)nivl);
            nround = ngpu = nq = nwait = nivl = 0;
        }
    }
    static const char *why[] = {"", "error", "gamescope socket", "signal", "gamescope ended",
                                "resume failed (reconfiguration)"};
    printf("[gs] stopping: %s\n", why[reason]);
    fflush(stdout);
    /* the app ended right after the start, mostly a single-instance app that handed the start to
     * its instance on the desktop: keep the sink connected, so the client's start placeholder can
     * show rdx-hostd's notice (app_ended_check) instead of the stream just vanishing; rdx-hostd
     * ends the session (SIGTERM) after it, 8 s at most here */
    if (reason == 4 && attached && rdx_now_ns() - t0 < 10000000000ull) {
        printf("[gs] the app ended %.1f s after the start: the sink stays connected for rdx-hostd's notice\n",
               (rdx_now_ns() - t0) / 1e9);
        fflush(stdout);
        struct pollfd sp = {.fd = sfd, .events = POLLIN};
        for (uint64_t until = rdx_now_ns() + 8000000000ull; rdx_now_ns() < until;) {
            struct signalfd_siginfo si;
            if (poll(&sp, 1, 200) > 0 && read(sfd, &si, sizeof(si)) == (ssize_t)sizeof(si) && si.ssi_signo != SIGPIPE &&
                si.ssi_signo != SIGUSR1 && si.ssi_signo != SIGUSR2 && si.ssi_signo != (uint32_t)SIGRTMIN)
                break;
        }
    }
#ifdef RDX_WITH_FG
    rdx_fgs_report(g_fgs);
#endif
    send_msg(gfd, RDX_GS_BYE, 0, 0);
    unlink(sock_path);
    double el = (rdx_now_ns() - t0) / 1e9;
    rdx_localinput_close(&LI);
    rdx_srcsess_close(&S);
#ifdef RDX_WITH_FG
    rdx_fgs_close(g_fgs);
#endif
    if (child > 0) {
        /* bounded: a supervisor (rdx-hostd) SIGKILLs us after its grace period, and gamescope
         * with Xwayland and the app can take seconds to go down */
        kill(child, SIGTERM);
        int done = 0;
        for (int i = 0; i < 60 && !done; i++) {
            done = waitpid(child, NULL, WNOHANG) == child;
            if (!done) usleep(20000);
        }
        if (!done) { kill(child, SIGKILL); waitpid(child, NULL, 0); printf("[gs] gamescope killed after 1.2 s\n"); }
    }
    printf("[gs] done: %ld frames in %.1f s = %.1f fps, shown %ld, dropped %ld, vblank anchors %ld, "
           "lead %.3f ms (target wait %.2f ms), recoveries %ld (flushed WRs %ld), refused posts %ld\n", seq, el,
           seq / el, S.shown, S.dropped, vblanks, lead_ns / 1e6, target_wait_ns / 1e6, S.recoveries, S.flushed,
           S.refused);
    printf("[gs] detached %ld times, resumed %ld times\n", detaches, resumes);
    printf("[gs] cursor next to the picture: %ld positions and %ld pictures from gamescope, %ld positions and %ld "
           "pictures sent, %ld positions dropped (send queue full)\n", G_cur.pos_in, G_cur.img_in, S.cur_pos_sent,
           S.cur_img_sent, S.cur_dropped);
    if (vrr_mhz || vrr_held) {
        printf("[gs] variable refresh (cap %.3f Hz): %ld frames held back to the cap, %ld replaced by a newer one\n",
               vrr_mhz / 1000.0, vrr_held, vrr_replaced);
        rdx_report("post -> next post (ms)", ivl, (size_t)nivl);
    }
    rdx_report(vrr_mhz ? "sink arrival -> on screen (ms)" : "sink arrival -> vblank (ms)", waitv, (size_t)nwait);
    rdx_report("gs wake -> GPU done (ms)", gpu, (size_t)ngpu);
    rdx_report("GPU done -> post (ms)", queue, (size_t)nq);
    rdx_report("post -> shown report (ms)", round, (size_t)nround);
    rdx_cpu_report("gs-src", t0, t0_cpu);
    rdx_tlog_close(&tl);
    if (qos_fd >= 0) close(qos_fd);
    if (verify) printf("[gs] verify: %ld frames hashed (compare with rdx_sink_null --hash)\n", nverified);
    for (uint32_t i = 0; i < ns; i++) rdx_gsbuf_free(&buf[i]);
    unlink(sock_path);
    return reason == 5 ? 3 : reason == 1 ? 1 : 0;      /* 3: resume refused, start fresh */
}
