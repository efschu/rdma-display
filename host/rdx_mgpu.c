/* SPDX-License-Identifier: MIT
 * rdx-hostd: several GPUs for one seat - frame generation and OptiScaler options of gamescope
 * apps, the host's GPU inventory, and the web API for them. See rdx_mgpu.h, docs/MULTI-GPU.md. */
#define _GNU_SOURCE
#include "rdx_mgpu.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static struct rdx_mgpu_host H;
static struct {
    pid_t pid;
    int status;                 /* waitpid status, -1 while running / never ran */
    char what[64];
} INST = {0, -1, ""};
static pthread_mutex_t inst_mu = PTHREAD_MUTEX_INITIALIZER;
#define INST_LOG "/tmp/rdx-mgpu-install.log"

void rdx_mgpu_init(const struct rdx_mgpu_host *h) { H = *h; }

/* ---- small helpers ---------------------------------------------------------------------------- */

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int one_of(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (!strcmp(s, *list)) return 1;
    return 0;
}

static const char *const ENGINES[] = {"auto", "helper", "afmf", "lsfg", "nvsm", "off", NULL};
static const char *const QUALITIES[] = {"auto", "quality", "performance", NULL};
static const char *const PROXIES[] = {"dxgi", "winmm", "version", "dbghelp", "d3d12", "wininet", "winhttp", NULL};

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static void lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* ---- per-app options ------------------------------------------------------------------------- */

const char *rdx_mgpu_validate_app(const cJSON *app)
{
    const cJSON *fg = cJSON_GetObjectItemCaseSensitive(app, "framegen");
    if (fg) {
        if (!cJSON_IsObject(fg)) return "framegen must be an object";
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(fg, "engine"), *q = cJSON_GetObjectItemCaseSensitive(fg, "quality");
        if (e && (!cJSON_IsString(e) || !one_of(e->valuestring, ENGINES)))
            return "framegen.engine: auto, helper, afmf, lsfg, nvsm or off";
        if (q && (!cJSON_IsString(q) || !one_of(q->valuestring, QUALITIES)))
            return "framegen.quality: auto, quality or performance";
        double m = jnum(fg, "multiplier", 2), fs = jnum(fg, "flow_scale", 1);
        if (m < 2 || m > 4 || m != (int)m) return "framegen.multiplier: 2, 3 or 4";
        if (fs < 0.25 || fs > 1.0) return "framegen.flow_scale: 0.25 .. 1.0";
    }
    const cJSON *os = cJSON_GetObjectItemCaseSensitive(app, "optiscaler");
    if (os && !cJSON_IsBool(os)) {
        if (!cJSON_IsObject(os)) return "optiscaler must be true/false or an object";
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(os, "enabled"), *px = cJSON_GetObjectItemCaseSensitive(os, "proxy");
        if (en && !cJSON_IsBool(en)) return "optiscaler.enabled must be true or false";
        if (px && (!cJSON_IsString(px) || !one_of(px->valuestring, PROXIES)))
            return "optiscaler.proxy: dxgi, winmm, version, dbghelp, d3d12, wininet or winhttp";
    }
    return NULL;
}

static int afmf_installed(const struct rdx_desktop_user *u)
{
    char p[512];
    if (!u || !*u->home) return 0;
    snprintf(p, sizeof(p), "%s/.local/share/vulkan/implicit_layer.d/afmf-linux.json", u->home);
    return file_exists(p);
}

/* the user's Lossless.dll outside the places lsfg-vk searches by itself, "" if none */
static void lossless_dll(const struct rdx_desktop_user *u, char *out, size_t n)
{
    out[0] = 0;
    if (!u || !*u->home) return;
    snprintf(out, n, "%s/.local/share/lsfg-vk/Lossless.dll", u->home);
    if (!file_exists(out)) out[0] = 0;
}

const char *rdx_mgpu_engine(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u)
{
    const cJSON *fg = cJSON_GetObjectItemCaseSensitive(app, "framegen");
    const char *e = jstr(fg, "engine", "auto");
    const char *helper = jstr(cfg, "helper_gpu", ""), *gpu = H.seat_gpu ? H.seat_gpu() : jstr(cfg, "gpu", "");
    int have_helper = *helper && strcasecmp(helper, gpu ? gpu : "");
    if (!strcmp(e, "auto")) return have_helper ? "helper" : afmf_installed(u) ? "afmf" : "off";
    if (!strcmp(e, "helper") && !have_helper) return "off";     /* no helper GPU on this seat */
    return e;
}

int rdx_mgpu_sink_slots(const cJSON *cfg, const cJSON *app)
{
    /* "auto" without a desktop user resolves to helper or off, which is all that matters here */
    return !strcmp(rdx_mgpu_engine(cfg, app, NULL), "helper") ? 4 : 0;
}

static int put(char env[][512], int n, int max, const char *k, const char *v)
{
    if (n >= max) return n;
    snprintf(env[n], 512, "%s=%s", k, v);
    return n + 1;
}

int rdx_mgpu_app_env(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u, char env[][512], int n,
                     int max)
{
    const cJSON *fg = cJSON_GetObjectItemCaseSensitive(app, "framegen");
    const char *e = rdx_mgpu_engine(cfg, app, u), *q = jstr(fg, "quality", "auto");
    char b[64];
    /* never two frame generators on one swapchain (afmf-linux README: they fight over presents) */
    if (!strcmp(e, "afmf")) {
        n = put(env, n, max, "AFMF_ENABLE", "1");
        n = put(env, n, max, "AFMF_GAMESCOPE", "1");     /* five extra images, gamescope itself passes through */
        n = put(env, n, max, "AFMF_PERFORMANCE_MODE", q);
        n = put(env, n, max, "DISABLE_LSFGVK", "1");
    } else if (!strcmp(e, "lsfg")) {
        n = put(env, n, max, "LSFGVK_ENV", "1");
        snprintf(b, sizeof(b), "%d", (int)jnum(fg, "multiplier", 2));
        n = put(env, n, max, "LSFGVK_MULTIPLIER", b);
        snprintf(b, sizeof(b), "%.2f", jnum(fg, "flow_scale", 1.0));
        n = put(env, n, max, "LSFGVK_FLOW_SCALE", b);
        n = put(env, n, max, "LSFGVK_PERFORMANCE_MODE", !strcmp(q, "performance") ? "1" : "0");
        char dll[512];
        lossless_dll(u, dll, sizeof(dll));
        if (*dll) n = put(env, n, max, "LSFGVK_DLL_PATH", dll);
        n = put(env, n, max, "DISABLE_AFMF", "1");
    } else if (!strcmp(e, "nvsm")) {
        n = put(env, n, max, "NVPRESENT_ENABLE_SMOOTH_MOTION", "1");
        n = put(env, n, max, "DISABLE_AFMF", "1");
        n = put(env, n, max, "DISABLE_LSFGVK", "1");
    } else {                                              /* helper (rdx_gs_src does it) or off */
        n = put(env, n, max, "DISABLE_AFMF", "1");
        n = put(env, n, max, "DISABLE_LSFGVK", "1");
    }
    n = put(env, n, max, "RDX_FRAMEGEN", e);

    const cJSON *os = cJSON_GetObjectItemCaseSensitive(app, "optiscaler");
    int on = cJSON_IsBool(os) ? cJSON_IsTrue(os) : !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(os, "enabled"));
    if (on) {
        /* the app's own WINEDLLOVERRIDES (its "env") is merged in front, so both apply */
        const char *own = jstr(cJSON_GetObjectItemCaseSensitive(app, "env"), "WINEDLLOVERRIDES", "");
        char v[400];
        snprintf(v, sizeof(v), "%s%s%s=n,b", own, *own ? ";" : "", jstr(os, "proxy", "dxgi"));
        n = put(env, n, max, "WINEDLLOVERRIDES", v);
    }
    return n;
}

int rdx_mgpu_gs_args(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u, char **av, int n, int max)
{
    static char gpu[32], q[16];
    if (strcmp(rdx_mgpu_engine(cfg, app, u), "helper") || n + 6 > max) return n;
    snprintf(gpu, sizeof(gpu), "%s", jstr(cfg, "helper_gpu", ""));
    snprintf(q, sizeof(q), "%s", jstr(cJSON_GetObjectItemCaseSensitive(app, "framegen"), "quality", "auto"));
    av[n++] = "--fg";
    av[n++] = "helper";
    av[n++] = "--fg-gpu";
    av[n++] = gpu;
    av[n++] = "--fg-quality";
    av[n++] = q;
    return n;
}

/* ---- GPU inventory --------------------------------------------------------------------------- */

static int is_bdf(const char *s)
{
    unsigned a, b, c, d;
    char tail;
    return sscanf(s, "%4x:%2x:%2x.%1x%c", &a, &b, &c, &d, &tail) == 4;
}

static const char *vendor_name(const char *ven)
{
    return !strcmp(ven, "0x10de") ? "NVIDIA" : !strcmp(ven, "0x1002") ? "AMD" : !strcmp(ven, "0x8086") ? "Intel" : NULL;
}

static int gpu_cmp(const void *a, const void *b)
{
    return strcmp(((const struct rdx_gpu_info *)a)->pci, ((const struct rdx_gpu_info *)b)->pci);
}

/* the NVIDIA driver's device node of a GPU (minor from its procfs), "" if unknown */
static void nvidia_node(const char *bdf, char *node, size_t n)
{
    char info[96], l[128];
    node[0] = 0;
    snprintf(info, sizeof(info), "/proc/driver/nvidia/gpus/%s/information", bdf);
    FILE *f = fopen(info, "r");
    while (f && fgets(l, sizeof(l), f))
        if (!strncmp(l, "Device Minor:", 13)) snprintf(node, n, "/dev/nvidia%d", atoi(l + 13));
    if (f) fclose(f);
}

/* "Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]" from lspci -mm, "" without lspci */
static void lspci_name(const char *bdf, char *out, size_t n)
{
    char cmd[96], line[512];
    out[0] = 0;
    snprintf(cmd, sizeof(cmd), "lspci -mm -s %.15s 2>/dev/null", bdf);
    FILE *p = popen(cmd, "r");
    if (p && fgets(line, sizeof(line), p)) {
        /* slot "class" "vendor" "device" ...: the 4th quoted field is the device */
        char *q = line;
        for (int i = 0; i < 5 && q; i++) q = strchr(q + (i ? 1 : 0), '"');   /* the 5th: opens the device */
        char *e = q ? strchr(q + 1, '"') : NULL;
        if (q && e) snprintf(out, n, "%.*s", (int)(e - q - 1), q + 1);
    }
    if (p) pclose(p);
}

int rdx_mgpu_enumerate(struct rdx_gpu_info *g, int max)
{
    int n = 0;
    FILE *p = popen("timeout 5 nvidia-smi --query-gpu=pci.bus_id,name --format=csv,noheader 2>/dev/null", "r");
    char line[512];
    while (p && n < max && fgets(line, sizeof(line), p)) {
        char *c = strchr(line, ',');
        if (!c) continue;
        *c = 0;
        char *nm = c + 1;
        while (*nm == ' ') nm++;
        nm[strcspn(nm, "\r\n")] = 0;
        char *bdf = line + (strlen(line) > 12 ? strlen(line) - 12 : 0);   /* "00000000:0D:00.0" */
        lower(bdf);
        struct rdx_gpu_info *x = &g[n++];
        memset(x, 0, sizeof(*x));
        snprintf(x->pci, sizeof(x->pci), "%.15s", bdf);
        snprintf(x->vendor, sizeof(x->vendor), "NVIDIA");
        snprintf(x->name, sizeof(x->name), "%s", nm);
        x->nvidia = 1;
        char node[32];
        nvidia_node(bdf, node, sizeof(node));
        x->usable = node[0] && access(node, R_OK | W_OK) == 0;
    }
    if (p) pclose(p);
    DIR *d = opendir("/sys/class/drm");
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (strncmp(de->d_name, "renderD", 7)) continue;
        char path[300], real[PATH_MAX], ven[16] = "", dev[16] = "";
        snprintf(path, sizeof(path), "/sys/class/drm/%s/device", de->d_name);
        if (!realpath(path, real)) continue;
        const char *bdf = strrchr(real, '/');
        bdf = bdf ? bdf + 1 : real;
        if (!is_bdf(bdf)) continue;
        int k = 0;
        while (k < n && strcmp(g[k].pci, bdf)) k++;
        if (k == n) {
            if (n == max) continue;
            memset(&g[n], 0, sizeof(g[n]));
            snprintf(g[n].pci, sizeof(g[n].pci), "%.15s", bdf);
            snprintf(path, sizeof(path), "/sys/class/drm/%s/device/vendor", de->d_name);
            read_line(path, ven, sizeof(ven));
            snprintf(path, sizeof(path), "/sys/class/drm/%s/device/device", de->d_name);
            read_line(path, dev, sizeof(dev));
            const char *vn = vendor_name(ven);
            snprintf(g[n].vendor, sizeof(g[n].vendor), "%s", vn ? vn : ven);
            char dn[72];
            lspci_name(bdf, dn, sizeof(dn));
            if (dn[0]) snprintf(g[n].name, sizeof(g[n].name), "%s %s", vn ? vn : "", dn);
            else snprintf(g[n].name, sizeof(g[n].name), "%s GPU %.6s:%.6s", vn ? vn : "", ven + 2, dev + 2);
            g[n].nvidia = !strcmp(ven, "0x10de");
            n++;
        }
        snprintf(g[k].render, sizeof(g[k].render), "/dev/dri/%.16s", de->d_name);
        if (g[k].nvidia) {                       /* NVIDIA: its /dev/nvidiaN decides, as without DRM */
            char node[32];
            nvidia_node(g[k].pci, node, sizeof(node));
            if (node[0]) g[k].usable = access(node, R_OK | W_OK) == 0;
        } else {
            int fd = open(g[k].render, O_RDWR | O_CLOEXEC);
            g[k].usable = fd >= 0;
            if (fd >= 0) close(fd);
        }
    }
    if (d) closedir(d);
    qsort(g, (size_t)n, sizeof(*g), gpu_cmp);
    return n;
}

cJSON *rdx_mgpu_gpus(void)
{
    cJSON *arr = cJSON_CreateArray();
    /* VRAM of NVIDIA GPUs */
    char vram[16][2][32];
    int nv = 0;
    FILE *p = popen("timeout 5 nvidia-smi --query-gpu=pci.bus_id,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    char line[256];
    while (p && nv < 16 && fgets(line, sizeof(line), p)) {
        char *c = strchr(line, ',');
        if (!c) continue;
        *c = 0;
        char *bdf = line + (strlen(line) > 12 ? strlen(line) - 12 : 0);
        lower(bdf);
        snprintf(vram[nv][0], sizeof(vram[nv][0]), "%.15s", bdf);
        snprintf(vram[nv][1], sizeof(vram[nv][1]), "%.31s", c + 1);
        nv++;
    }
    if (p) pclose(p);
    const char *seat = H.seat_gpu ? H.seat_gpu() : "";
    cJSON *cfg = H.cfg ? H.cfg() : NULL;
    const char *helper = jstr(cfg, "helper_gpu", "");
    struct rdx_gpu_info gl[16];
    int ng = rdx_mgpu_enumerate(gl, 16);
    for (int k = 0; k < ng; k++) {
        const struct rdx_gpu_info *x = &gl[k];
        char path[PATH_MAX], v[128];
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "pci", x->pci);
        cJSON_AddStringToObject(g, "vendor", x->vendor);
        cJSON_AddStringToObject(g, "name", x->name);
        cJSON_AddBoolToObject(g, "usable", x->usable);
        cJSON_AddStringToObject(g, "render", x->render);
        double mib = 0;
        for (int i = 0; i < nv; i++)
            if (!strcmp(vram[i][0], x->pci)) mib = atof(vram[i][1]);
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/mem_info_vram_total", x->pci);   /* amdgpu */
        if (!mib && !read_line(path, v, sizeof(v))) mib = atof(v) / 1048576.0;
        cJSON_AddNumberToObject(g, "vram_mib", mib);
        static const char *const lk[] = {"current_link_speed", "current_link_width", "max_link_speed", "max_link_width"};
        cJSON *link = cJSON_AddObjectToObject(g, "link");
        for (int i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", x->pci, lk[i]);
            if (read_line(path, v, sizeof(v))) v[0] = 0;
            cJSON_AddStringToObject(link, lk[i], v);
        }
        /* bridges between the root complex and the GPU: 1 = directly below a CPU root port
         * (the rule rdx_gsbuf's --buffers auto uses); a PCIe switch or the chipset adds more.
         * An integrated GPU sits behind an internal bridge of the CPU and also counts 1. */
        char real[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s", x->pci);
        int bridges = -1;
        if (realpath(path, real)) {
            char *save = NULL;
            bridges = 0;
            for (char *t = strtok_r(real, "/", &save); t; t = strtok_r(NULL, "/", &save))
                if (is_bdf(t)) bridges++;
            bridges--;                                     /* the GPU itself */
        }
        cJSON_AddNumberToObject(g, "bridges", bridges);
        cJSON_AddBoolToObject(g, "root_port", bridges == 1);
        cJSON_AddBoolToObject(g, "seat_gpu", seat && !strcasecmp(seat, x->pci));
        cJSON_AddBoolToObject(g, "helper_gpu", *helper && !strcasecmp(helper, x->pci));
        cJSON_AddItemToArray(arr, g);
    }
    return arr;
}

/* ---- running the tools ----------------------------------------------------------------------- */

/* argv with stdout captured into out (NUL-terminated), as `u` if given; exit status or -1 */
static int run_capture(char *const argv[], const struct rdx_desktop_user *u, char *out, size_t n, int timeout_s)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); }
        if (u && u->uid) {
            if (initgroups(u->name, u->gid) || setgid(u->gid) || setuid(u->uid)) _exit(126);
            setenv("HOME", u->home, 1);
            setenv("USER", u->name, 1);
            setenv("LOGNAME", u->name, 1);
            unsetenv("XDG_CACHE_HOME");
            if (chdir(u->home)) {}
        }
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t got = 0;
    time_t t_end = time(NULL) + timeout_s;
    for (;;) {
        int left = (int)(t_end - time(NULL));
        if (left <= 0) { kill(pid, SIGKILL); break; }
        struct pollfd pp = {.fd = pfd[0], .events = POLLIN};
        if (poll(&pp, 1, left * 1000) <= 0) continue;
        ssize_t r = read(pfd[0], out + got, n - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= n - 1) break;
    }
    out[got] = 0;
    close(pfd[0]);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int desktop_user(struct rdx_desktop_user *u)
{
    char want[64] = "";
    if (H.mu) pthread_mutex_lock(H.mu);
    snprintf(want, sizeof(want), "%s", jstr(H.cfg ? H.cfg() : NULL, "desktop_user", ""));
    if (H.mu) pthread_mutex_unlock(H.mu);
    return rdx_desktop_user(want, u);
}

static cJSON *tool_json(char *const argv[], const struct rdx_desktop_user *u, int timeout_s)
{
    static char buf[1 << 20];       /* one request at a time per tool call: guarded by tool_mu */
    static pthread_mutex_t tool_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&tool_mu);
    int rc = run_capture(argv, u, buf, sizeof(buf), timeout_s);
    cJSON *j = cJSON_Parse(buf);
    pthread_mutex_unlock(&tool_mu);
    if (!j) {
        j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", 0);
        cJSON_AddStringToObject(j, "error", rc == 127 ? "tool not found" : rc < 0 ? "tool timed out" : "tool printed no JSON");
    }
    return j;
}

static void tool_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/tools/%s", H.repo ? H.repo : ".", name);
}

/* background installer (afmf build takes a minute): one at a time */
static int install_start(const char *what, char *const argv[])
{
    pthread_mutex_lock(&inst_mu);
    if (INST.pid > 0 && waitpid(INST.pid, &INST.status, WNOHANG) == 0) { pthread_mutex_unlock(&inst_mu); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(INST_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        setsid();
        execvp(argv[0], argv);
        _exit(127);
    }
    INST.pid = pid;
    INST.status = -1;
    snprintf(INST.what, sizeof(INST.what), "%s", what);
    pthread_mutex_unlock(&inst_mu);
    return pid > 0 ? 0 : -1;
}

static cJSON *install_state(void)
{
    pthread_mutex_lock(&inst_mu);
    int running = INST.pid > 0 && waitpid(INST.pid, &INST.status, WNOHANG) == 0;
    if (INST.pid > 0 && !running) INST.pid = -INST.pid;          /* reaped; keep the status */
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "component", INST.what);
    cJSON_AddBoolToObject(j, "running", running);
    if (!running && INST.status >= 0) cJSON_AddNumberToObject(j, "exit", WIFEXITED(INST.status) ? WEXITSTATUS(INST.status) : -1);
    pthread_mutex_unlock(&inst_mu);
    char log[4096] = "";
    FILE *f = fopen(INST_LOG, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, sz > (long)sizeof(log) - 1 ? sz - (long)sizeof(log) + 1 : 0, SEEK_SET);
        size_t r = fread(log, 1, sizeof(log) - 1, f);
        log[r] = 0;
        fclose(f);
    }
    cJSON_AddStringToObject(j, "log", log);
    return j;
}

/* ---- web API --------------------------------------------------------------------------------- */

static int send_json(struct mg_connection *conn, int code, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    size_t n = strlen(s);
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
              "Content-Length: %zu\r\nConnection: close\r\n\r\n",
              code, code == 200 ? "OK" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Bad Request", n);
    mg_write(conn, s, n);
    free(s);
    cJSON_Delete(j);
    return code;
}

static cJSON *read_body(struct mg_connection *conn)
{
    char buf[16384];
    int n = 0, r;
    while (n < (int)sizeof(buf) - 1 && (r = mg_read(conn, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0) n += r;
    buf[n] = 0;
    return n ? cJSON_Parse(buf) : cJSON_CreateObject();
}

static cJSON *err_json(const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 0);
    cJSON_AddStringToObject(j, "error", msg);
    return j;
}

static cJSON *overview(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", 1);
    cJSON_AddItemToObject(j, "gpus", rdx_mgpu_gpus());
    struct rdx_desktop_user u;
    int have = !desktop_user(&u);
    cJSON *seat = cJSON_AddObjectToObject(j, "seat");
    if (H.mu) pthread_mutex_lock(H.mu);
    cJSON *cfg = H.cfg ? H.cfg() : NULL;
    cJSON_AddStringToObject(seat, "gpu", H.seat_gpu ? H.seat_gpu() : jstr(cfg, "gpu", ""));
    cJSON_AddStringToObject(seat, "helper_gpu", jstr(cfg, "helper_gpu", ""));
    cJSON *apps = cJSON_AddArrayToObject(j, "apps"), *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(cfg, "apps")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", jstr(a, "id", ""));
        cJSON_AddStringToObject(o, "name", jstr(a, "name", ""));
        cJSON *fg = cJSON_GetObjectItemCaseSensitive(a, "framegen"), *os = cJSON_GetObjectItemCaseSensitive(a, "optiscaler");
        cJSON_AddItemToObject(o, "framegen", fg ? cJSON_Duplicate(fg, 1) : cJSON_CreateObject());
        cJSON_AddItemToObject(o, "optiscaler", os ? cJSON_Duplicate(os, 1) : cJSON_CreateObject());
        cJSON_AddStringToObject(o, "engine", rdx_mgpu_engine(cfg, a, have ? &u : NULL));
        cJSON_AddItemToArray(apps, o);
    }
    if (H.mu) pthread_mutex_unlock(H.mu);
    char tool[512];
    tool_path(tool, sizeof(tool), "rdx-mgpu-install.sh");
    char *argv[] = {"bash", tool, "status", have ? u.name : "", NULL};
    cJSON_AddItemToObject(j, "engines", tool_json(argv, NULL, 10));
    cJSON_AddItemToObject(j, "install", install_state());
    return j;
}

static int set_app_options(cJSON *b, char *err, size_t errn)
{
    const char *id = jstr(b, "id", "");
    cJSON *tmp = cJSON_CreateObject();
    cJSON *fg = cJSON_GetObjectItemCaseSensitive(b, "framegen"), *os = cJSON_GetObjectItemCaseSensitive(b, "optiscaler");
    if (fg) cJSON_AddItemToObject(tmp, "framegen", cJSON_Duplicate(fg, 1));
    if (os) cJSON_AddItemToObject(tmp, "optiscaler", cJSON_Duplicate(os, 1));
    const char *bad = rdx_mgpu_validate_app(tmp);
    if (bad) { snprintf(err, errn, "%s", bad); cJSON_Delete(tmp); return 400; }
    int code = 404;
    pthread_mutex_lock(H.mu);
    cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(H.cfg(), "apps")) {
        if (strcmp(jstr(a, "id", ""), id)) continue;
        if (fg) {
            cJSON_DeleteItemFromObjectCaseSensitive(a, "framegen");
            cJSON_AddItemToObject(a, "framegen", cJSON_Duplicate(fg, 1));
        }
        if (os) {
            cJSON_DeleteItemFromObjectCaseSensitive(a, "optiscaler");
            cJSON_AddItemToObject(a, "optiscaler", cJSON_Duplicate(os, 1));
        }
        code = H.save && H.save() ? 500 : 200;
        if (code == 500) snprintf(err, errn, "could not write the config");
        break;
    }
    pthread_mutex_unlock(H.mu);
    if (code == 404) snprintf(err, errn, "no app \"%s\"", id);
    cJSON_Delete(tmp);
    return code;
}

int rdx_mgpu_http(struct mg_connection *conn, void *cbdata)
{
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : "/api/mgpu";
    int get = !strcmp(ri->request_method, "GET");
    char tool[512];
    if (get && (!strcmp(uri, "/api/mgpu") || !strcmp(uri, "/api/mgpu/"))) return send_json(conn, 200, overview());
    if (!strcmp(uri, "/api/mgpu/install")) {
        if (get) return send_json(conn, 200, install_state());
        cJSON *b = read_body(conn);
        const char *c = jstr(b, "component", "");
        int rm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "remove"));
        struct rdx_desktop_user u;
        int have = !desktop_user(&u);
        if (!one_of(c, (const char *const[]){"afmf", "lsfg", "7z", NULL}) || (strcmp(c, "7z") && !have)) {
            cJSON_Delete(b);
            return send_json(conn, 400, err_json("component: afmf, lsfg or 7z (and a desktop user)"));
        }
        tool_path(tool, sizeof(tool), "rdx-mgpu-install.sh");
        char comp[16], name[64];
        snprintf(comp, sizeof(comp), "%s", c);
        snprintf(name, sizeof(name), "%s", have ? u.name : "");
        char *argv_i[] = {"bash", tool, comp, name, NULL};
        char *argv_r[] = {"bash", tool, "remove", comp, name, NULL};
        int r = install_start(comp, rm ? argv_r : argv_i);
        cJSON_Delete(b);
        if (r) return send_json(conn, 409, err_json("an installation is still running"));
        cJSON *j = install_state();
        cJSON_AddBoolToObject(j, "ok", 1);
        return send_json(conn, 200, j);
    }
    if (!strcmp(uri, "/api/mgpu/games") || !strcmp(uri, "/api/mgpu/optiscaler")) {
        struct rdx_desktop_user u;
        if (desktop_user(&u)) return send_json(conn, 409, err_json("no desktop user (config \"desktop_user\")"));
        tool_path(tool, sizeof(tool), "rdx-mgpu-optiscaler.py");
        if (get && !strcmp(uri, "/api/mgpu/games")) {
            char *argv[] = {"python3", tool, "list", NULL};
            return send_json(conn, 200, tool_json(argv, &u, 60));
        }
        if (get) {
            char *argv[] = {"python3", tool, "status", NULL};
            return send_json(conn, 200, tool_json(argv, &u, 20));
        }
        cJSON *b = read_body(conn);
        const char *act = jstr(b, "action", "");
        char appid[32], proxy[16], fgm[16], dir[1024];
        snprintf(appid, sizeof(appid), "%s", jstr(b, "appid", ""));
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(b, "appid")))
            snprintf(appid, sizeof(appid), "%.0f", jnum(b, "appid", 0));
        snprintf(proxy, sizeof(proxy), "%s", jstr(b, "proxy", "dxgi"));
        snprintf(fgm, sizeof(fgm), "%s", jstr(b, "fg", "native"));
        snprintf(dir, sizeof(dir), "%s", jstr(b, "dir", ""));
        cJSON_Delete(b);
        char *argv[16];
        int k = 0;
        argv[k++] = "python3";
        argv[k++] = tool;
        if (!strcmp(act, "fetch")) argv[k++] = "fetch";
        else if (!strcmp(act, "install") || !strcmp(act, "remove")) {
            argv[k++] = (char *)(!strcmp(act, "install") ? "install" : "remove");
            if (*dir) { argv[k++] = "--dir"; argv[k++] = dir; }
            else if (*appid) { argv[k++] = "--appid"; argv[k++] = appid; }
            else return send_json(conn, 400, err_json("appid or dir"));
            if (!strcmp(act, "install")) {
                argv[k++] = "--proxy"; argv[k++] = proxy;
                argv[k++] = "--fg"; argv[k++] = fgm;
            }
        } else return send_json(conn, 400, err_json("action: install, remove or fetch"));
        argv[k] = NULL;
        cJSON *j = tool_json(argv, &u, 300);
        return send_json(conn, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")) ? 200 : 400, j);
    }
    if (!get && !strcmp(uri, "/api/mgpu/app")) {
        cJSON *b = read_body(conn);
        if (!b) return send_json(conn, 400, err_json("bad JSON"));
        char err[256] = "";
        int code = set_app_options(b, err, sizeof(err));
        cJSON_Delete(b);
        if (code != 200) return send_json(conn, code, err_json(err));
        return send_json(conn, 200, overview());
    }
    return send_json(conn, 404, err_json("unknown /api/mgpu call"));
}
