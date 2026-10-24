/* SPDX-License-Identifier: MIT
 * rdx-hostd: several GPUs for one seat (docs/MULTI-GPU.md).
 *
 * Per app (config "apps"[i]):
 *   "framegen":   {"engine": "auto" | "helper" | "afmf" | "lsfg" | "nvsm" | "off",
 *                  "quality": "auto" | "quality" | "performance", "multiplier": 2..4 (lsfg),
 *                  "flow_scale": 0.25..1.0 (lsfg)}
 *       helper: rdx_gs_src generates frames on the seat's "helper_gpu" (gs/rdx_fg_stage.h);
 *       afmf:   afmf-linux layer in the app, on the seat's GPU (FidelityFX optical flow, MIT);
 *       lsfg:   lsfg-vk layer in the app, on the seat's GPU (needs the user's Lossless.dll);
 *       nvsm:   NVIDIA Smooth Motion (driver layer VK_LAYER_NV_present, RTX 40/50);
 *       auto (default): helper when the seat has a helper GPU, else afmf when it is installed,
 *                       else off.
 *   "optiscaler": {"enabled": true (default), "proxy": "dxgi"}: WINEDLLOVERRIDES=<proxy>=n,b for
 *       the app, so OptiScaler loads in every game of it that has it in its folder
 *       (tools/rdx-mgpu-optiscaler.py installs it per game); games without it are unaffected.
 *
 * Web API (rdx_mgpu_http on "/api/mgpu"):
 *   GET  /api/mgpu              GPUs (PCI placement, link), the seat's GPU / helper, installed engines
 *   GET  /api/mgpu/games        Steam games: exe folder, upscaler DLLs, OptiScaler state
 *   POST /api/mgpu/optiscaler   {"action":"install"|"remove"|"fetch","appid","proxy","fg"}
 *   POST /api/mgpu/install      {"component":"afmf"|"lsfg"|"7z","remove":false}  (background)
 *   GET  /api/mgpu/install      {"running","exit","log"} of the last one
 *   POST /api/mgpu/app          {"id","framegen":{...},"optiscaler":{...}}: an app's options */
#pragma once
#include "rdx_apps.h"
#include "../third_party/civetweb/civetweb.h"
#include "../third_party/cjson/cJSON.h"

#include <pthread.h>

/* what rdx-hostd lends the module */
struct rdx_mgpu_host {
    const char *repo;                  /* tools/ are found here */
    pthread_mutex_t *mu;               /* guards the config */
    cJSON *(*cfg)(void);               /* the live config object */
    const char *(*seat_gpu)(void);     /* resolved PCI address of the seat's GPU ("" = first NVIDIA) */
    int (*save)(void);                 /* write the config back; called with mu held; 0 = ok */
};

void rdx_mgpu_init(const struct rdx_mgpu_host *h);

/* validate_apps(): NULL if the app's "framegen" / "optiscaler" are well-formed */
const char *rdx_mgpu_validate_app(const cJSON *app);

/* the engine "auto" resolves to for this app on this seat ("helper", "afmf", "off", ...) */
const char *rdx_mgpu_engine(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u);

/* hello's app list: the sink slots the client should give this app, 0 = its own choice. Frame
 * generation on the helper GPU keeps one sink slot for the generated frames, so gamescope gets
 * 3 of 4 and can run ahead while a real frame waits for the helper (measured on
 * the test host: 100 -> 124 frames/s shown at 1080p@240). The client asks rdx_sink_kms --slots N. */
int rdx_mgpu_sink_slots(const cJSON *cfg, const cJSON *app);

/* seat_app_hook(): "KEY=VALUE" environment for the app; returns the new count */
int rdx_mgpu_app_env(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u, char env[][512], int n,
                     int max);

/* spawn_gamescope(): rdx_gs_src arguments (--fg helper --fg-gpu PCI --fg-quality Q); new count.
 * The strings stay valid until the next call. */
int rdx_mgpu_gs_args(const cJSON *cfg, const cJSON *app, const struct rdx_desktop_user *u, char **av, int n, int max);

/* every graphics card of this host, any vendor: NVIDIA from nvidia-smi (also without a DRM
 * driver, e.g. nvidia_drm not loaded), everything with a DRM render node from /sys/class/drm.
 * usable: NVIDIA = its /dev/nvidiaN is accessible (a seat container has only its own), others =
 * the render node opens read-write. Sorted by PCI address; returns the count. */
struct rdx_gpu_info {
    char pci[16];        /* "0000:09:00.0" */
    char vendor[16];     /* "NVIDIA", "AMD", "Intel" or the PCI vendor id */
    char name[96];       /* "NVIDIA GeForce RTX 2080 Ti", "AMD Cezanne [Radeon Vega Series ...]" */
    char render[32];     /* "/dev/dri/renderD129" or "" */
    int nvidia, usable;
};
int rdx_mgpu_enumerate(struct rdx_gpu_info *g, int max);

/* GPUs of this host as JSON (PCI address, vendor, name, VRAM, link, CPU root port or not) */
cJSON *rdx_mgpu_gpus(void);

/* civetweb handler for /api/mgpu and below (cbdata unused) */
int rdx_mgpu_http(struct mg_connection *conn, void *cbdata);
