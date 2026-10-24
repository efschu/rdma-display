/* SPDX-License-Identifier: MIT
 *
 * P1 mirror source: mirrors one real monitor of this host to a remote sink
 * (rdx_sink_kms or rdx_sink_win), with the credit / "shown" / vblank-lock
 * protocol of rdx_src.
 *
 * Per frame (capture path as in ffmpeg kmsgrab and Sunshine kmsgrab.cpp):
 *   primary plane of the chosen output -> current framebuffer (plane fb_id,
 *   GETFB2, PRIME dma-buf; EGLImage with the scanout modifier, cached per fb_id)
 *   -> GPU draw of the output's rectangle (crtc x/y + mode size) into a LINEAR
 *   GBM buffer of the sink's size (de-tiles, scales if sizes differ)
 *   -> glFinish -> RDMA_WRITE of the pixels straight from that dma-buf
 *   -> RDMA_WRITE_WITH_IMM of the frame header.
 * One GPU copy is unavoidable: scanout buffers are tiled (and on NVIDIA not
 * NIC-registrable, see p0a gate), a remote iGPU only scans out linear.
 * Without a linear GBM render target the NIC can register (NVIDIA: its GBM refuses
 * GBM_BO_USE_LINEAR for rendering, and its dma-bufs are not NIC-registrable), the draw goes
 * into a GL texture and glReadPixels copies it into host memory the NIC sends from
 * ("host read-back", printed at start).
 *
 * vblank lock as in rdx_src; the lead additionally covers capture + draw time.
 * The source must run as root (GETFB2 only returns handles to root/DRM master).
 *
 *   rdx_mirror_src --peer IP [--dev auto] [--port 18650] [--mtu 4096]
 *                  [--card /dev/dri/card1] [--render /dev/dri/renderD128]
 *                  [--output DP-1|<index>] [--list | --list-json] [--frames 600] [--fps 0=auto]
 *                  [--sync immediate|lock] [--lock-margin-ms 1.5] [--vblank-guard]
 *                  [--busy-poll] [--pm-qos-us 50] [--no-cursor] [--log-dir /var/log/rdx | -]
 *   rdx_mirror_src --probe N [--dump FILE.ppm] [--card ..] [--render ..] [--output ..]
 *                  (capture path alone, no sink, no NIC: N captures, timing, last frame as PPM)
 *
 * --sync immediate (default): capture as soon as the compositor COMMITS a new buffer to
 *   the mirrored output (its primary plane's FB_ID changes), i.e. when the frame is rendered,
 *   before the host's own monitor shows it at the next vblank (measured on the test host: commit ->
 *   vblank p50 3.6 ms of 6.9). There is no event for another client's commit, so FB_ID and the
 *   last vblank are read every --commit-poll-us (two ioctls, sleeping in between). A period
 *   without a commit (cursor-only change, static screen) is captured at its vblank. No free
 *   sink slot: a vblank capture is skipped, a commit capture takes the newest buffer as soon
 *   as a slot frees. No frame rate cap unless --fps. --commit-poll-us 0 = vblank only.
 * --phase-target-us T (with --commit-poll-us 0, i.e. a client that runs at the output's exact
 *   rate): capture D after each host vblank and steer D from the sink's "shown" reports so frames
 *   arrive T before the sink's vblank (fixed phase, no waiting a period on the sink).
 * --sync lock: schedule capture so frames arrive just before the sink's vblank (vblank
 *   lock, --lock-margin-ms); --vblank-guard additionally keeps a copy from spanning a
 *   vblank of the mirrored output. Both cost latency; off unless asked for.
 *
 * The cursor plane (separate from the primary plane) is composited into the frame
 * by default. Per-frame telemetry goes to --log-dir (see rdx_tlog, tools/rdx_hitch.py);
 * the journal only gets a line for seconds with anomalies.
 *
 * --list-json prints the active outputs as one JSON line
 * {"outputs":[{"name","w","h","hz","hz_exact","x","y"}]} and exits (for rdx-hostd); hz_exact is the
 * refresh from the mode timings (e.g. 143.998), which a client can match exactly.
 * --frames 0 runs until SIGINT/SIGTERM (statistics then come as rolling windows).
 * Waiting is event driven (CQ completion channel), only the last 50 us before a
 * post deadline spin; --busy-poll spins all the time.
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"
#include "../common/rdx_kms.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define WR_SRC_BASE 0x100u
#define WR_RECV     0x200u
#define WR_END      0x300u
#define NRECV       32
#define CRING       (RDX_MAX_SLOTS * 4)
#define FB_CACHE    8

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* Runtime control from rdx-hostd (in-stream menu, session.update), applied before the
 * next frame: sigqueue(SIGRTMIN, 0|1) = cursor off/on, sigqueue(SIGRTMIN+1, i) = mirror
 * output i of the --list order. Signals keep the stream connected (no restart). */
static volatile sig_atomic_t g_cursor_req = -1, g_output_req = -1;
static void on_control(int s, siginfo_t *si, void *uc)
{
    (void)uc;
    if (s == SIGRTMIN) g_cursor_req = si->si_value.sival_int ? 1 : 0;
    else if (s == SIGRTMIN + 1) g_output_req = si->si_value.sival_int;
}

static PFNEGLCREATEIMAGEKHRPROC p_create_image;
static PFNEGLDESTROYIMAGEKHRPROC p_destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_target_tex;

struct gl {
    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    GLuint prog, vbo;
    GLint u_uvrect, u_opaque;
};

struct cap_fb {                        /* cached scanout framebuffer */
    uint32_t fb_id;
    uint32_t w, h, fourcc;
    uint64_t modifier;
    EGLImageKHR img;
    GLuint tex;
    uint64_t last_use;
};

struct out_buf {                       /* linear render target, read by the NIC */
    struct gbm_bo *bo;
    int fd;
    EGLImageKHR img;
    GLuint tex, fbo;
    struct ibv_mr *mr;
    uint8_t *host;                     /* host read-back: the NIC sends from here */
    uint32_t stride, w, h;
};

static const char *VS =
    "attribute vec2 pos;\n"
    "uniform highp vec4 uvrect;\n"
    "varying highp vec2 uv;\n"
    "void main() {\n"
    "  uv = uvrect.xy + (pos * 0.5 + 0.5) * uvrect.zw;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";
static const char *FS =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES tex;\n"
    "varying highp vec2 uv;\n"
    "uniform float opaque;\n"
    "void main() { vec4 c = texture2D(tex, uv); gl_FragColor = vec4(c.rgb, opaque > 0.5 ? 1.0 : c.a); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

/* GPU queue priority for our short capture draw. The local desktop shares the iGPU;
 * at normal priority the draw queued behind its rendering (capture 4-13 ms instead of
 * 1.5 ms, measured). high needs root/CAP_SYS_NICE, which the source has. */
static EGLint g_gpu_prio = EGL_CONTEXT_PRIORITY_HIGH_IMG;

static int gl_init(struct gl *g, int render_fd)
{
    g->gbm = gbm_create_device(render_fd);
    if (!g->gbm) { fprintf(stderr, "gbm_create_device failed\n"); return -1; }
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_pd =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g->dpy = get_pd ? get_pd(EGL_PLATFORM_GBM_KHR, g->gbm, NULL) : EGL_NO_DISPLAY;
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) {
        fprintf(stderr, "EGL on GBM failed\n"); return -1;
    }
    const char *ext = eglQueryString(g->dpy, EGL_EXTENSIONS);
    if (!strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") || !strstr(ext, "EGL_KHR_surfaceless_context")) {
        fprintf(stderr, "EGL lacks dma_buf_import_modifiers or surfaceless_context\n"); return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    int have_prio = strstr(ext, "EGL_IMG_context_priority") != NULL;
    EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                      have_prio ? EGL_CONTEXT_PRIORITY_LEVEL_IMG : EGL_NONE, g_gpu_prio, EGL_NONE};
    EGLConfig cfg = NULL;
    if (!strstr(ext, "EGL_KHR_no_config_context")) {
        EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE}, n = 0;
        eglChooseConfig(g->dpy, ca, &cfg, 1, &n);
    }
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, cattr);
    if (g->ctx == EGL_NO_CONTEXT || !eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g->ctx)) {
        fprintf(stderr, "EGL context failed 0x%x\n", eglGetError()); return -1;
    }
    p_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_target_tex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    EGLint got = 0;
    if (have_prio) eglQueryContext(g->dpy, g->ctx, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &got);
    printf("[gl] GBM %s, GL_RENDERER=%s, GPU priority %s\n", gbm_device_get_backend_name(g->gbm),
           (const char *)glGetString(GL_RENDERER),
           !have_prio ? "unsupported" : got == EGL_CONTEXT_PRIORITY_HIGH_IMG ? "high"
           : got == EGL_CONTEXT_PRIORITY_LOW_IMG ? "low" : "medium");

    g->prog = glCreateProgram();
    glAttachShader(g->prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g->prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(g->prog, 0, "pos");
    glLinkProgram(g->prog);
    glUseProgram(g->prog);
    g->u_uvrect = glGetUniformLocation(g->prog, "uvrect");
    g->u_opaque = glGetUniformLocation(g->prog, "opaque");
    glUniform1f(g->u_opaque, 1.0f);
    glUniform1i(glGetUniformLocation(g->prog, "tex"), 0);
    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    return 0;
}

/* EGLImage from dma-buf planes; modifier only if valid */
static EGLImageKHR import_dmabuf(EGLDisplay dpy, uint32_t w, uint32_t h, uint32_t fourcc, int nplanes,
                                 const int *fds, const uint32_t *offsets, const uint32_t *pitches,
                                 int with_mod, uint64_t modifier)
{
    static const EGLint pa[4][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
         EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
         EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
         EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}};
    EGLint a[64], n = 0;
    a[n++] = EGL_WIDTH;  a[n++] = (EGLint)w;
    a[n++] = EGL_HEIGHT; a[n++] = (EGLint)h;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT; a[n++] = (EGLint)fourcc;
    for (int p = 0; p < nplanes; p++) {
        a[n++] = pa[p][0]; a[n++] = fds[p];
        a[n++] = pa[p][1]; a[n++] = (EGLint)offsets[p];
        a[n++] = pa[p][2]; a[n++] = (EGLint)pitches[p];
        if (with_mod) {
            a[n++] = pa[p][3]; a[n++] = (EGLint)(modifier & 0xffffffff);
            a[n++] = pa[p][4]; a[n++] = (EGLint)(modifier >> 32);
        }
    }
    a[n++] = EGL_NONE;
    return p_create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
}

/* look up or import the scanout framebuffer fb_id */
static struct cap_fb *cap_get(struct gl *g, int card_fd, struct cap_fb *cache, uint32_t fb_id, uint64_t now)
{
    struct cap_fb *lru = &cache[0];
    for (int i = 0; i < FB_CACHE; i++) {
        if (cache[i].fb_id == fb_id && cache[i].img) { cache[i].last_use = now; return &cache[i]; }
        if (cache[i].last_use < lru->last_use) lru = &cache[i];
    }
    struct drm_mode_fb_cmd2 fb;
    if (rdx_kms_getfb2(card_fd, fb_id, &fb)) { perror("GETFB2 (root needed)"); return NULL; }
    int fds[4] = {-1, -1, -1, -1}, np = 0;
    for (int p = 0; p < 4 && fb.handles[p]; p++, np++) {
        int dup = -1;
        for (int q = 0; q < p; q++) if (fb.handles[q] == fb.handles[p]) dup = q;
        if (dup >= 0) fds[p] = fds[dup];
        else if (rdx_kms_handle_to_fd(card_fd, fb.handles[p], &fds[p])) { perror("PRIME_HANDLE_TO_FD"); return NULL; }
    }
    int with_mod = (fb.flags & DRM_MODE_FB_MODIFIERS) && fb.modifier[0] != DRM_FORMAT_MOD_INVALID;
    EGLImageKHR img = import_dmabuf(g->dpy, fb.width, fb.height, fb.pixel_format, np, fds, fb.offsets,
                                    fb.pitches, with_mod, fb.modifier[0]);
    for (int p = 0; p < np; p++) {
        int dup = 0;
        for (int q = 0; q < p; q++) if (fds[q] == fds[p]) dup = 1;
        if (!dup) close(fds[p]);                         /* the EGLImage keeps its own reference */
    }
    for (int p = 0; p < np; p++) {
        int dup = 0;
        for (int q = 0; q < p; q++) if (fb.handles[q] == fb.handles[p]) dup = 1;
        if (!dup) rdx_kms_gem_close(card_fd, fb.handles[p]);
    }
    if (img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "import of scanout fb %u (fourcc 0x%08x, modifier 0x%016llx, %d planes) failed: 0x%x\n",
                fb_id, fb.pixel_format, (unsigned long long)fb.modifier[0], np, eglGetError());
        return NULL;
    }
    if (lru->img) { glDeleteTextures(1, &lru->tex); p_destroy_image(g->dpy, lru->img); }
    memset(lru, 0, sizeof(*lru));
    lru->fb_id = fb_id; lru->w = fb.width; lru->h = fb.height; lru->fourcc = fb.pixel_format;
    lru->modifier = with_mod ? fb.modifier[0] : DRM_FORMAT_MOD_INVALID;
    lru->img = img;
    lru->last_use = now;
    glGenTextures(1, &lru->tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, lru->tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_target_tex(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    printf("[cap] scanout fb %u: %ux%u fourcc %.4s modifier 0x%016llx, %d plane(s)\n", fb_id, fb.width,
           fb.height, (const char *)&fb.pixel_format, (unsigned long long)lru->modifier, np);
    return lru;
}

/* a plain GL texture as render target (host read-back, and the probe without linear GBM) */
static int plain_fbo(uint32_t w, uint32_t h, GLuint *tex, GLuint *fbo)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "plain target FBO incomplete 0x%x\n", st); return -1; }
    return 0;
}

/* host read-back of the drawn frame as XRGB8888 rows of o->stride bytes (row 0 = top). Waits for
 * the GPU like glFinish. GL_PACK_ROW_LENGTH needs GLES 3; BGRA read-back GL_EXT_read_format_bgra
 * (both on NVIDIA), else row by row / swapped on the CPU. */
static void out_readback(struct out_buf *o)
{
    static int bgra = -1, es3 = -1;
    if (bgra < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS), *ver = (const char *)glGetString(GL_VERSION);
        bgra = ext && strstr(ext, "GL_EXT_read_format_bgra") != NULL;
        es3 = ver && strncmp(ver, "OpenGL ES ", 10) == 0 && ver[10] >= '3';
    }
    GLenum fmt = bgra ? GL_BGRA_EXT : GL_RGBA;
    glBindFramebuffer(GL_FRAMEBUFFER, o->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (o->stride == o->w * 4) {
        glReadPixels(0, 0, (GLsizei)o->w, (GLsizei)o->h, fmt, GL_UNSIGNED_BYTE, o->host);
    } else if (es3) {
        glPixelStorei(0x0D02 /* GL_PACK_ROW_LENGTH */, (GLint)(o->stride / 4));
        glReadPixels(0, 0, (GLsizei)o->w, (GLsizei)o->h, fmt, GL_UNSIGNED_BYTE, o->host);
        glPixelStorei(0x0D02, 0);
    } else {
        for (uint32_t y = 0; y < o->h; y++)
            glReadPixels(0, (GLint)y, (GLsizei)o->w, 1, fmt, GL_UNSIGNED_BYTE, o->host + (size_t)y * o->stride);
    }
    if (!bgra)
        for (uint32_t y = 0; y < o->h; y++) {
            uint8_t *r = o->host + (size_t)y * o->stride;
            for (uint32_t x = 0; x < o->w; x++) { uint8_t t = r[x * 4]; r[x * 4] = r[x * 4 + 2]; r[x * 4 + 2] = t; }
        }
}

static int out_init_host(struct ibv_pd *pd, struct out_buf *o, uint32_t w, uint32_t h, uint32_t want_stride,
                         size_t *bytes)
{
    static int said;
    if (!said++)
        printf("[cap] output buffers: host read-back (no linear GBM render target the NIC can register; "
               "the draw goes into a GL texture, glReadPixels into host memory)\n");
    o->w = w; o->h = h; o->stride = want_stride;
    *bytes = (size_t)want_stride * h;
    size_t len = (*bytes + 4095) & ~(size_t)4095;
    o->host = aligned_alloc(4096, len);
    if (!o->host) { perror("aligned_alloc"); return -1; }
    memset(o->host, 0, len);
    o->mr = ibv_reg_mr(pd, o->host, len, IBV_ACCESS_LOCAL_WRITE);
    if (!o->mr) { perror("ibv_reg_mr(host read-back)"); return -1; }
    return plain_fbo(w, h, &o->tex, &o->fbo);
}

static int out_init(struct gl *g, struct ibv_pd *pd, struct out_buf *o, uint32_t w, uint32_t h,
                    uint32_t want_stride, size_t *bytes)
{
    o->bo = gbm_bo_create(g->gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!o->bo) { perror("gbm_bo_create(LINEAR)"); return out_init_host(pd, o, w, h, want_stride, bytes); }
    uint32_t stride = gbm_bo_get_stride(o->bo);
    if (stride != want_stride) {
        fprintf(stderr, "linear stride %u != sink stride %u\n", stride, want_stride);
        return -1;
    }
    o->fd = gbm_bo_get_fd(o->bo);
    off_t end = lseek(o->fd, 0, SEEK_END);
    *bytes = (size_t)stride * h;
    o->mr = ibv_reg_dmabuf_mr(pd, 0, (size_t)end, 0, o->fd, IBV_ACCESS_LOCAL_WRITE);
    if (!o->mr) {
        perror("ibv_reg_dmabuf_mr(linear GBM)");
        close(o->fd);
        gbm_bo_destroy(o->bo);
        o->bo = NULL;
        o->fd = -1;
        return out_init_host(pd, o, w, h, want_stride, bytes);
    }
    int fd = o->fd;
    uint32_t off = 0;
    o->img = import_dmabuf(g->dpy, w, h, DRM_FORMAT_XRGB8888, 1, &fd, &off, &stride, 1, DRM_FORMAT_MOD_LINEAR);
    if (o->img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "target EGLImage failed 0x%x\n", eglGetError()); return -1; }
    glGenTextures(1, &o->tex);
    glBindTexture(GL_TEXTURE_2D, o->tex);
    p_target_tex(GL_TEXTURE_2D, (GLeglImageOES)o->img);
    glGenFramebuffers(1, &o->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "target FBO incomplete\n"); return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

/* --probe N [--dump FILE.ppm]: the capture path alone, without a sink or an RDMA NIC (seat
 * bring-up, e.g. gamescope's own DRM output in a seat container): N captures of the output at
 * its refresh into a linear buffer of the output's size, counting new scanout buffers and the
 * capture time; the last frame (cursor included) goes to a PPM file. */
static int capture_probe(struct gl *g, int card_fd, const struct rdx_kms_cap_out *O, long n, const char *dump,
                         int with_cursor, const struct rdx_kms_cursor *cur, int cur_ok)
{
    uint32_t w = O->w, h = O->h;
    struct gbm_bo *bo = gbm_bo_create(g->gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    GLuint tex, fbo;
    if (!bo) {                                           /* NVIDIA: a plain texture, as the host read-back */
        perror("gbm_bo_create(LINEAR)");
        printf("[probe] target: a GL texture (the stream would use host read-back)\n");
        if (plain_fbo(w, h, &tex, &fbo)) return 1;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    } else {
        int fd = gbm_bo_get_fd(bo);
        uint32_t stride = gbm_bo_get_stride(bo), off = 0;
        EGLImageKHR img = import_dmabuf(g->dpy, w, h, DRM_FORMAT_XRGB8888, 1, &fd, &off, &stride, 1, DRM_FORMAT_MOD_LINEAR);
        if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "[probe] target EGLImage failed 0x%x\n", eglGetError()); return 1; }
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        p_target_tex(GL_TEXTURE_2D, (GLeglImageOES)img);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[probe] FBO incomplete\n"); return 1; }
    }
    struct cap_fb cache[FB_CACHE];
    memset(cache, 0, sizeof(cache));
    uint64_t period = O->vrefresh ? 1000000000ull / O->vrefresh : 16666667ull;
    uint32_t last_fb = 0;
    long fb_changes = 0, fails = 0;
    double *ms = calloc((size_t)n, sizeof(double));
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    for (long i = 0; i < n && !g_stop; i++) {
        uint64_t t0 = rdx_now_ns();
        uint32_t fb_id = rdx_kms_plane_fb(card_fd, O->plane_id);
        struct cap_fb *cf = fb_id ? cap_get(g, card_fd, cache, fb_id, t0) : NULL;
        if (!cf) { fails++; usleep((useconds_t)(period / 1000)); continue; }
        if (fb_id != last_fb) { fb_changes++; last_fb = fb_id; }
        fourcc = cf->fourcc;
        modifier = cf->modifier;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, cf->tex);
        glUniform4f(g->u_uvrect, (float)O->x / cf->w, (float)O->y / cf->h, (float)O->w / cf->w, (float)O->h / cf->h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        uint32_t cfb = 0;
        int32_t cx, cy;
        uint32_t cw, chh;
        if (with_cursor && cur_ok &&
            !rdx_kms_cursor_state(card_fd, cur, O->crtc_id, &cfb, &cx, &cy, &cw, &chh) &&
            cfb && cw && chh) {
            struct cap_fb *cc = cap_get(g, card_fd, cache, cfb, t0);
            if (cc) {
                glViewport(cx, cy, (GLsizei)cw, (GLsizei)chh);   /* rows top-down, as in the main loop */
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, cc->tex);
                glUniform4f(g->u_uvrect, 0.0f, 0.0f, (float)cw / cc->w, (float)chh / cc->h);
                glUniform1f(g->u_opaque, 0.0f);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glDisable(GL_BLEND);
                glUniform1f(g->u_opaque, 1.0f);
            }
        }
        glFinish();
        ms[i] = (rdx_now_ns() - t0) / 1e6;
        uint64_t used = rdx_now_ns() - t0;
        if (used < period) usleep((useconds_t)((period - used) / 1000));
    }
    printf("[probe] %s %ux%u@%u: %ld captures, %ld scanout buffer changes, %ld failed, scanout format %.4s modifier 0x%llx\n",
           O->name, w, h, O->vrefresh, n, fb_changes, fails, (const char *)&fourcc, (unsigned long long)modifier);
    rdx_report("capture (import + draw + glFinish)", ms, (size_t)n);
    if (dump) {
        uint8_t *px = malloc((size_t)w * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(dump, "wb");
        if (!f) { perror(dump); return 1; }
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        long nonblack = 0;
        for (uint32_t y = 0; y < h; y++) {               /* row 0 is the top (see the vertex shader) */
            const uint8_t *r = px + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                fwrite(r + x * 4, 1, 3, f);
                nonblack += (r[x * 4] | r[x * 4 + 1] | r[x * 4 + 2]) != 0;
            }
        }
        fclose(f);
        printf("[probe] last frame -> %s (%.1f %% non-black pixels)\n", dump, 100.0 * nonblack / ((double)w * h));
        free(px);
    }
    free(ms);
    return fails == n ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *dev = "auto", *peer_ip = NULL, *card = "/dev/dri/card1", *dump = NULL;
    long probe = 0;
    const char *render = "/dev/dri/renderD128", *output = NULL;
    int port = 18650, mtu = 4096, lock = 1, list_only = 0;
    long frames = 600;
    double fps = 0, lock_margin_ms = 1.5;              /* fps 0 = auto */
    int busy = 0, pm_qos_us = 50, with_cursor = 1, comp_vector = -1, vblank_guard = 0, immediate = 1, commit_poll_us = 250;
    /* --phase-target-us: steer the capture phase (see ph_* below); 0 = capture at the vblank */
    int phase_target_us = 0;
    const char *log_dir = RDX_TLOG_DIR;
    struct sigaction sa_ctl;
    memset(&sa_ctl, 0, sizeof(sa_ctl));
    sa_ctl.sa_sigaction = on_control;
    sa_ctl.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &sa_ctl, NULL);
    sigaction(SIGRTMIN + 1, &sa_ctl, NULL);

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"peer", 1, 0, 'p'}, {"port", 1, 0, 'P'}, {"mtu", 1, 0, 'm'},
        {"card", 1, 0, 'c'}, {"render", 1, 0, 'R'}, {"output", 1, 0, 'o'}, {"list", 0, 0, 'l'}, {"list-json", 0, 0, 'j'},
        {"frames", 1, 0, 'n'}, {"fps", 1, 0, 'f'}, {"no-lock", 0, 0, 'L'},
        {"lock-margin-ms", 1, 0, 'g'}, {"busy-poll", 0, 0, 'B'}, {"pm-qos-us", 1, 0, 'Q'},
        {"no-cursor", 0, 0, 'C'}, {"log-dir", 1, 0, 'T'}, {"comp-vector", 1, 0, 'V'},
        {"gpu-prio", 1, 0, 'G'}, {"no-vblank-guard", 0, 0, 'W'}, {"vblank-guard", 0, 0, 'w'},
        {"sync", 1, 0, 'S'}, {"commit-poll-us", 1, 0, 'K'}, {"phase-target-us", 1, 0, 'H'},
        {"probe", 1, 0, 'X'}, {"dump", 1, 0, 'U'}, {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'p': peer_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'c': card = optarg; break;
        case 'R': render = optarg; break;
        case 'o': output = optarg; break;
        case 'l': list_only = 1; break;
        case 'j': list_only = 2; break;
        case 'n': frames = atol(optarg); break;
        case 'f': fps = atof(optarg); break;
        case 'L': lock = 0; immediate = 0; break;
        case 'g': lock_margin_ms = atof(optarg); break;
        case 'B': busy = 1; break;
        case 'Q': pm_qos_us = atoi(optarg); break;
        case 'C': with_cursor = 0; break;
        case 'X': probe = atol(optarg); break;
        case 'U': dump = optarg; break;
        case 'W': vblank_guard = 0; break;
        case 'w': vblank_guard = 1; break;
        case 'K': commit_poll_us = atoi(optarg); break;
        case 'H': phase_target_us = atoi(optarg); break;
        case 'S':
            if (!strcmp(optarg, "immediate")) immediate = 1;
            else if (!strcmp(optarg, "lock")) immediate = 0;
            else { fprintf(stderr, "--sync immediate|lock\n"); return 2; }
            break;
        case 'T': log_dir = optarg; break;
        case 'V': comp_vector = atoi(optarg); break;
        case 'G':
            g_gpu_prio = !strcmp(optarg, "low") ? EGL_CONTEXT_PRIORITY_LOW_IMG
                       : !strcmp(optarg, "medium") ? EGL_CONTEXT_PRIORITY_MEDIUM_IMG : EGL_CONTEXT_PRIORITY_HIGH_IMG;
            break;
        default:  fprintf(stderr, "see the header of rdx_mirror_src.c for options\n"); return 2;
        }
    }

    /* outputs of this host */
    int card_fd = open(card, O_RDWR | O_CLOEXEC);
    if (card_fd < 0) { perror(card); return 1; }
    if (rdx_kms_universal_planes(card_fd)) perror("UNIVERSAL_PLANES");
    struct rdx_kms_cap_out outs[8];
    int nout = rdx_kms_list_cap_outputs(card_fd, outs, 8);
    if (list_only == 2) {                 /* machine-readable, nothing else on stdout */
        printf("{\"outputs\":[");
        for (int i = 0; i < (nout > 0 ? nout : 0); i++)
            printf("%s{\"name\":\"%s\",\"w\":%u,\"h\":%u,\"hz\":%u,\"hz_exact\":%.4f,\"x\":%u,\"y\":%u}",
                   i ? "," : "", outs[i].name, outs[i].w, outs[i].h, outs[i].vrefresh, outs[i].hz_exact,
                   outs[i].x, outs[i].y);
        printf("]}\n");
        return nout > 0 ? 0 : 1;
    }
    if (nout <= 0) { fprintf(stderr, "no active outputs on %s\n", card); return 1; }
    int sel = -1;
    for (int i = 0; i < nout; i++) {
        printf("[cap] output %d: %s %ux%u@%u at +%u+%u, CRTC %u, primary plane %u\n", i, outs[i].name,
               outs[i].w, outs[i].h, outs[i].vrefresh, outs[i].x, outs[i].y, outs[i].crtc_id, outs[i].plane_id);
        if (output && (!strcmp(output, outs[i].name) || (output[0] >= '0' && output[0] <= '9' && atoi(output) == i)))
            sel = i;
    }
    if (list_only) return 0;
    rdx_stop_signals(on_signal);
    if (!output) sel = 0;
    if (sel < 0 || !outs[sel].plane_id) { fprintf(stderr, "output %s not found or no primary plane\n", output); return 1; }
    struct rdx_kms_cap_out *O = &outs[sel];
    /* Compositors reuse their scanout buffers and repaint only the damage (buffer age):
     * once the next flip on this output completes, the buffer we are copying may get the
     * frame after next painted into it, and the copy shows a mix of old and new content.
     * The vblank guard starts a copy only if it ends before the next vblank of this CRTC,
     * otherwise it waits for that vblank first (then the buffer is stable for a period). */
    int crtc_idx = (vblank_guard || immediate) ? rdx_kms_crtc_index(card_fd, O->crtc_id) : -1;
    double src_period = O->vrefresh ? 1e9 / O->vrefresh : 0;
    uint64_t tv_probe;
    if ((vblank_guard || immediate) &&
        (crtc_idx < 0 || !src_period || rdx_kms_last_vblank(card_fd, crtc_idx, &tv_probe))) {
        fprintf(stderr, "[cap] no vblank events on CRTC %u: %s\n", O->crtc_id,
                immediate ? "immediate sync paced by the output refresh timer" : "copying without the guard");
        crtc_idx = -1;
    }
    if (!peer_ip && probe <= 0) { fprintf(stderr, "need --peer (or --probe N)\n"); return 2; }
    /* the cursor lives in its own plane and is not part of the scanout framebuffer */
    struct rdx_kms_cursor cur;
    memset(&cur, 0, sizeof(cur));
    int cur_ok = !rdx_kms_find_cursor_plane(card_fd, O->crtc_id, &cur);   /* looked up even when off */
    if (with_cursor && !cur_ok)
        fprintf(stderr, "[cap] no cursor plane on CRTC %u, mirroring without cursor\n", O->crtc_id);
    printf("[cap] cursor %s\n", with_cursor && cur_ok ? "composited from its plane" : "off");

    int render_fd = open(render, O_RDWR | O_CLOEXEC);
    if (render_fd < 0) { perror(render); return 1; }
    struct gl g;
    memset(&g, 0, sizeof(g));
    if (gl_init(&g, render_fd)) return 1;
    if (probe > 0) return capture_probe(&g, card_fd, O, probe, dump, with_cursor, &cur, cur_ok);

    /* sink tells us size and slots */
    int sock = rdx_tcp_connect(peer_ip, port);
    if (sock < 0) return 1;
    struct rdx_hello hello;
    if (rdx_io_all(sock, &hello, sizeof(hello), 0) || hello.magic != RDX_MAGIC) {
        fprintf(stderr, "bootstrap: invalid hello\n"); return 1;
    }
    uint32_t w = hello.width, h = hello.height, ns = hello.nslots;
    if (!ns || ns > RDX_MAX_SLOTS) { fprintf(stderr, "invalid nslots=%u\n", ns); return 1; }
    printf("[i] mirroring %s (%ux%u at +%u+%u) -> sink %ux%u%s, %u slots\n", O->name, O->w, O->h, O->x, O->y,
           w, h, (w == O->w && h == O->h) ? " (1:1)" : " (scaled)", ns);

    char lip[64] = "";
    if (peer_ip) rdx_local_ip_for(peer_ip, lip, sizeof(lip));   /* the seat's link IP towards the peer */
    struct ibv_context *ctx = rdx_open_dev_ip(dev, lip);
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd"); return 1; }
    struct out_buf ob[RDX_MAX_SLOTS];
    memset(ob, 0, sizeof(ob));
    size_t pix_bytes = 0;
    for (uint32_t i = 0; i < ns; i++)
        if (out_init(&g, pd, &ob[i], w, h, hello.stride, &pix_bytes)) return 1;
    if (pix_bytes > hello.hdr_off) { fprintf(stderr, "frame larger than sink slot\n"); return 1; }
    struct rdx_frame_hdr *hdrs = calloc(ns, sizeof(*hdrs));
    struct ibv_mr *hmr = ibv_reg_mr(pd, hdrs, ns * sizeof(*hdrs), IBV_ACCESS_LOCAL_WRITE);
    /* NIC completion timestamps: the vblank anchor is when the sink's "shown" report
     * reached our NIC, not when this process got around to look (that can be ms later
     * while it captures or wakes up late, and skewed the lock by a vblank) */
    struct rdx_cq rcq;
    if (!hmr || rdx_cq_create_ts(ctx, 256, comp_vector, &rcq)) return 1;
    struct ibv_comp_channel *ch = rcq.ch;
    struct ibv_cq *cq = rcq.cq;
    printf("[i] NIC completion timestamps %s, completion vector %d of %d\n", rcq.cqx ? "on" : "unavailable",
           rdx_pick_comp_vector(ctx, comp_vector), ctx->num_comp_vectors);
    struct ibv_qp *qp = cq ? rdx_create_qp(pd, cq, 64) : NULL;
    if (!qp || rdx_qp_to_init(qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(qp, WR_RECV)) { perror("post_recv"); return 1; }
    struct rdx_conn mine;
    memset(&mine, 0, sizeof(mine));
    int gidx = rdx_find_gid_index_ip(ctx, lip, mine.gid);
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    mine.qpn = qp->qp_num;
    mine.psn = 0x3456;
    if (rdx_io_all(sock, &mine, sizeof(mine), 1)) return 1;
    if (rdx_qp_to_rtr_rts(qp, &hello.conn, gidx, mine.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }

    uint32_t credit[CRING], c_head = 0, c_tail = 0;
    for (uint32_t i = 0; i < ns; i++) credit[c_tail++ % CRING] = i;
    int src_busy[RDX_MAX_SLOTS] = {0}, unshown[RDX_MAX_SLOTS] = {0};
    uint64_t slot_t_post[RDX_MAX_SLOTS] = {0};
    long slot_seq[RDX_MAX_SLOTS] = {0};
    long inflight = 0, credits_back = 0, done = 0, nrtt = 0, nshown = 0, nlocked = 0, fb_changes = 0, ncap = 0;
    /* telemetry: F = frame posted, S = sink report (shown / slot free), D = dropped by the sink */
    struct rdx_tlog tl;
    rdx_tlog_open(&tl, log_dir, "mirror-src",
                  "F,seq,slot,t_due,t_wake,t_credit,t_cap0,t_cap1,t_post,ahead,lead_us,fb_id,fb_new,cursor_fb\n"
                  "S,seq,t_report,round_us,sink_us,shown\n"
                  "D,seq,t_report");
    long anomalies = 0;
    long posted = 0;
    /* statistics: whole run, or (endless, --frames 0) a rolling window printed when full */
    long cap = frames > 0 ? frames : 36000;
    double *rtt = calloc((size_t)cap, sizeof(double)), *proc = calloc((size_t)cap, sizeof(double));
    double *xfer = calloc((size_t)cap, sizeof(double)), *capt = calloc((size_t)cap, sizeof(double));
    int qos_fd = rdx_pm_qos(pm_qos_us);
    uint64_t t0_cpu = rdx_cpu_ns();
    printf("[i] %s, PM QoS %s\n", busy ? "busy poll" : "event driven (CQ channel)", qos_fd >= 0 ? "on" : "off");
    uint64_t t_shown_last = 0;
    struct rdx_lock vlock = {0, 0};
    /* frame rate: --fps, never above the sink's refresh. Auto = the highest EVEN cadence the
     * mirrored output can feed: sink_hz / k for the smallest whole k with sink_hz / k <= output
     * refresh. 144 Hz on a 240 Hz sink gives 120 fps (every frame 2 vblanks), not 144 fps
     * alternating 1 and 2 vblanks, which judders visibly on motion. */
    double sink_hz = hello.vblank_ns ? 1e9 / (double)hello.vblank_ns : 0;
    if (fps <= 0 && immediate) {
        fps = 0;                                 /* no cap: every new frame of the output */
    } else if (fps <= 0) {
        fps = O->vrefresh;
        if (sink_hz > 0 && fps > 0) {
            int k = 1;
            while (sink_hz / k > fps + 0.5) k++;
            fps = sink_hz / k;
        }
    }
    if (sink_hz > 0 && fps > sink_hz) fps = sink_hz;
    /* the KMS sink reports its exact refresh period; estimate only without it */
    int fixed_period = hello.vblank_ns != 0;
    double period_est = fixed_period ? (double)hello.vblank_ns : (fps > 0 ? 1e9 / fps : 0);
    /* capture time shares the iGPU with the local desktop and jitters: the lead covers
     * mean + 3 x mean absolute deviation, not just the mean */
    double net_ewma = 0, cap_ewma = 0, cap_dev = 0, copy_ewma = 0, copy_dev = 0;
    long vbl_waits = 0, vbl_early = 0;
    uint64_t period_ns = fps > 0 ? (uint64_t)(1e9 / fps) : 0;   /* wanted frame interval */
    if (immediate)
        printf("[i] immediate sync: capture on each commit to %s%s, else at its vblank (%u Hz%s), post at once, %s\n",
               O->name, commit_poll_us > 0 ? "" : " DISABLED", O->vrefresh, crtc_idx >= 0 ? "" : ", by timer",
               fps > 0 ? "frame rate capped by --fps" : "no frame rate cap");
    else
        printf("[i] %.2f fps, sink refresh %s\n", fps,
               fixed_period ? "reported by sink" : "estimated from shown reports");
    uint64_t t0 = rdx_now_ns(), t_last_done = t0;
    uint32_t last_fb = 0;
    struct cap_fb cache[FB_CACHE];
    memset(cache, 0, sizeof(cache));
    /* warm-up: first import and first draw compile shaders and map buffers (hundreds of ms);
     * do it once before the clock starts so it never lands in the vblank lock */
    {
        uint32_t fb0 = rdx_kms_plane_fb(card_fd, O->plane_id);
        struct cap_fb *cf0 = fb0 ? cap_get(&g, card_fd, cache, fb0, rdx_now_ns()) : NULL;
        if (!cf0) { fprintf(stderr, "warm-up capture failed\n"); return 1; }
        for (uint32_t i = 0; i < ns; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, ob[i].fbo);
            glViewport(0, 0, (GLsizei)w, (GLsizei)h);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, cf0->tex);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glFinish();
    }
    struct rdx_wc wc[16];

/* x = arrival -> the vblank that showed the frame. With the sink flipping at once (match mode) a
 * frame is visible at the next vblank, x < one period. x above a period means the sink sits one
 * frame behind: every frame arrives while the previous flip is still pending and waits for it,
 * and at exactly equal rates that state holds itself. Steering cannot leave it (moving the
 * arrival moves x by the same amount, modulo a period it even looks on target), so the source
 * skips one capture: the pending flip completes, the next frame flips at once, x drops by a
 * period. Below a period x - target is the error, not wrapped; late reports are handled in
 * POLL_ONCE (three in a row: skip one capture and move by the lateness). */
#define ph_steer() do {                                                           \
        double per_ = src_period, e_[PH_N];                                       \
        for (int i_ = 0; i_ < PH_N; i_++) e_[i_] = ph_x[i_];                      \
        for (int i_ = 1; i_ < PH_N; i_++)                                         \
            for (int j_ = i_; j_ > 0 && e_[j_ - 1] > e_[j_]; j_--) {              \
                double t_ = e_[j_]; e_[j_] = e_[j_ - 1]; e_[j_ - 1] = t_;         \
            }                                                                     \
        double xm_ = e_[PH_N / 2];                                                \
        ph_last_err = xm_ - phase_target_us * 1e3;                                \
        /* positive: arrives too early -> capture later; full step while far, half when near */ \
        ph_d += (ph_last_err > 500e3 || ph_last_err < -500e3 ? 1.0 : 0.5) * ph_last_err; \
        ph_d -= per_ * (double)(long)(ph_d / per_);                               \
        if (ph_d < 0) ph_d += per_;                                               \
        ph_ignore = 4;                    /* frames in flight still show the old phase */ \
        ph_steps++;                                                               \
    } while (0)

#define POLL_ONCE() do {                                                          \
        int nwc = rdx_cq_poll(&rcq, wc, 16);                                      \
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }           \
        for (int k = 0; k < nwc; k++) {                                           \
            if (wc[k].status != IBV_WC_SUCCESS) {                                 \
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n",                  \
                        ibv_wc_status_str(wc[k].status), (unsigned long long)wc[k].wr_id); \
                return 1;                                                         \
            }                                                                     \
            if (wc[k].opcode == IBV_WC_RECV) {                                    \
                uint64_t t_cr = wc[k].hw_ns ? wc[k].hw_ns : rdx_now_ns();         \
                uint32_t cimm = ntohl(wc[k].imm_data), cslot = rdx_imm_slot(cimm); \
                int shown = (cimm & RDX_IMM_SHOWN) != 0;                          \
                if (!shown) { credit[c_tail++ % CRING] = cslot; credits_back++; } \
                if (cslot < ns && (shown || rdx_credit_us(cimm) == RDX_US_DROP)) unshown[cslot] = 0; \
                if (cslot < ns && rdx_credit_us(cimm) == RDX_US_DROP)                \
                    rdx_tlog_line(&tl, "D,%ld,%llu", slot_seq[cslot], (unsigned long long)t_cr); \
                if (cslot < ns && rdx_credit_us(cimm) < RDX_US_DROP && slot_t_post[cslot]) \
                    rdx_tlog_line(&tl, "S,%ld,%llu,%.1f,%.1f,%d", slot_seq[cslot], (unsigned long long)t_cr, \
                                  (t_cr - slot_t_post[cslot]) / 1e3, rdx_credit_us(cimm) * 1.0, shown); \
                if (rdx_credit_us(cimm) < RDX_US_DROP && nrtt < cap && cslot < ns && slot_t_post[cslot]) { \
                    if (shown) nshown++;                                          \
                    rtt[nrtt] = (t_cr - slot_t_post[cslot]) / 1e6;                \
                    proc[nrtt] = rdx_credit_us(cimm) / 1e3;                       \
                    xfer[nrtt] = rtt[nrtt] - proc[nrtt];                          \
                    if (shown) net_ewma = net_ewma > 0 ? 0.9 * net_ewma + 0.1 * xfer[nrtt] * 1e6 : xfer[nrtt] * 1e6; \
                    nrtt++;                                                       \
                }                                                                 \
                if (shown && phase_target_us > 0 && rdx_credit_us(cimm) < RDX_US_DROP) { \
                    double x_ = rdx_credit_us(cimm) * 1e3;                        \
                    if (ph_ignore > 0) ph_ignore--;                               \
                    else if (x_ > src_period) {                                   \
                        /* shown a period late: missed the latch (too late), or the sink sits \
                         * one frame behind (each frame waits for the previous flip, a state \
                         * that holds itself at equal rates). Three in a row: skip one capture \
                         * (ends "behind") and move the capture by the lateness. */ \
                        if (++ph_late >= 3) {                                     \
                            ph_d += x_ - src_period - phase_target_us * 1e3;      \
                            ph_d -= src_period * (double)(long)(ph_d / src_period); \
                            if (ph_d < 0) ph_d += src_period;                     \
                            ph_skip = 1; ph_escapes++; ph_ignore = 4; ph_nx = 0; ph_late = 0; \
                        }                                                         \
                    } else {                                                      \
                        ph_late = 0;                                              \
                        ph_x[ph_nx++] = x_;                                       \
                        if (ph_nx == PH_N) { ph_steer(); ph_nx = 0; }             \
                    }                                                             \
                }                                                                 \
                if (shown) {                                                      \
                    if (t_shown_last && !fixed_period) {                          \
                        double gap = (double)(t_cr - t_shown_last);               \
                        if (period_est <= 0 && gap > 4e6 && gap < 50e6) period_est = gap; \
                        else if (gap > 0.5 * period_est && gap < 1.5 * period_est) \
                            period_est = 0.95 * period_est + 0.05 * gap;          \
                    }                                                             \
                    t_shown_last = t_cr;                                          \
                }                                                                 \
                if (rdx_post_zero_recv(qp, WR_RECV)) { perror("repost"); return 1; } \
            } else if (wc[k].wr_id >= WR_SRC_BASE && wc[k].wr_id < WR_RECV) {     \
                src_busy[wc[k].wr_id - WR_SRC_BASE] = 0;                          \
                inflight--; done++; t_last_done = rdx_now_ns();                   \
            }                                                                     \
        }                                                                         \
    } while (0)

#define WAIT_UNTIL(cond, deadline) RDX_WAIT_UNTIL(cond, deadline, POLL_ONCE(), cq, ch, busy, g_stop)

    uint64_t t_sec = rdx_now_ns();
    long sec_frames = 0, sec_late = 0, sec_slow = 0, sec_wait = 0;
    double sec_late_max = 0, sec_cap_max = 0, sec_wait_max = 0;
    /* phase steering: D = capture delay after the host vblank. The sink and the host run at the
     * same rate (match_host) but with a random phase; the sink's "shown" report carries arrival ->
     * its vblank (x). Every PH_N reports the median of x (see ph_steer) moves D so the frame lands
     * target before the sink's vblank. Capturing later is no loss: the newest committed buffer is
     * read at capture time. */
    double ph_d = 0;
#define PH_N 16
    double ph_x[PH_N];
    int ph_nx = 0;
    long ph_steps = 0, ph_escapes = 0;
    int ph_skip = 0, ph_ignore = 0, ph_late = 0;
    double ph_last_err = 0;
    long imm_skipped = 0, imm_commits = 0, imm_vblanks = 0;
    uint64_t imm_last = 0, imm_tick = 0, imm_vbl = 0;
    uint32_t imm_fb = 0;          /* buffer of the last capture */
    int imm_since_vbl = 0;        /* captured on a commit since the last vblank of the output */
    int imm_trig = 0;             /* 1 = this capture follows a commit, 2 = a vblank */
    for (long seq = 0; (frames == 0 || seq < frames) && !g_stop; seq++) {
        uint64_t due = 0;
        int ahead = 0;
        double lead = 0;
        if (immediate) {
            /* wait for a commit (new buffer) or, failing that, the next vblank of the output */
            uint64_t poll_ns = commit_poll_us > 0 ? (uint64_t)commit_poll_us * 1000ull : 0;
            imm_trig = 0;
            while (!g_stop && !imm_trig) {
                POLL_ONCE();
                uint64_t now = rdx_now_ns(), tv = 0;
                if (crtc_idx < 0 || rdx_kms_last_vblank(card_fd, crtc_idx, &tv)) {
                    if (!imm_tick) imm_tick = now;            /* no vblank ioctl: output refresh timer */
                    while (imm_tick + (uint64_t)src_period <= now) imm_tick += (uint64_t)src_period;
                    tv = imm_tick;
                }
                uint32_t fbn = poll_ns ? rdx_kms_plane_fb(card_fd, O->plane_id) : 0;
                int slot_free = !(c_head == c_tail || src_busy[(uint32_t)(seq % ns)]);
                if (fbn && fbn != imm_fb && slot_free) { imm_trig = 1; due = now; imm_commits++; break; }
                if (tv != imm_vbl) {                          /* a vblank of the output passed */
                    imm_vbl = tv;
                    if (!imm_since_vbl) {
                        if (slot_free) { imm_trig = 2; due = tv; imm_vblanks++; break; }
                        imm_skipped++;
                    }
                    imm_since_vbl = 0;
                }
                /* sleep until the next poll (or the next vblank without polling), credits wake us */
                uint64_t next_v = imm_vbl + (uint64_t)src_period;
                uint64_t wake = poll_ns ? now + poll_ns : (next_v > now ? next_v : now + 100000);
                ibv_req_notify_cq(cq, 0);
                POLL_ONCE();
                if (rdx_now_ns() < wake) rdx_sleep_on(ch, -1, wake - rdx_now_ns());
            }
            if (g_stop) break;
            /* phase steering: capture D after the host vblank (vblank-triggered captures only) */
            if (imm_trig == 2 && ph_skip) {      /* leave "one frame behind" on the sink */
                ph_skip = 0;
                imm_skipped++;
                seq--;
                continue;
            }
            if (imm_trig == 2 && phase_target_us > 0 && ph_d > 0) {
                due = imm_vbl + (uint64_t)ph_d;
                WAIT_UNTIL(rdx_now_ns() >= due, due);
                if (g_stop) break;
            }
            /* --fps cap: drop captures that come too soon after the previous one */
            if (period_ns && imm_last && due < imm_last + period_ns - (uint64_t)(src_period / 2)) {
                imm_fb = rdx_kms_plane_fb(card_fd, O->plane_id);
                imm_since_vbl = 1;
                imm_skipped++;
                seq--;
                continue;
            }
            imm_last = due;
        } else if (lock && t_shown_last && period_est > 0 && net_ewma > 0) {
            for (uint32_t i = 0; i < ns; i++) ahead += unshown[i];
            double interval = period_ns > period_est ? (double)period_ns : period_est;
            lead = net_ewma + cap_ewma + 3 * cap_dev + lock_margin_ms * 1e6;
            due = rdx_lock_due(t_shown_last, period_est, interval, ahead, &vlock, lead, rdx_now_ns());
            /* vblank guard, planned: if the copy would span a vblank of the captured CRTC,
             * start it early enough to end before that vblank (content up to one copy time
             * older, but on time) instead of waiting for the vblank and posting late */
            if (vblank_guard && crtc_idx >= 0 && copy_ewma > 0) {
                uint64_t tv;
                if (!rdx_kms_last_vblank(card_fd, crtc_idx, &tv)) {
                    double budget = copy_ewma + 3 * copy_dev + 300000.0;
                    double v = (double)tv + src_period;
                    while (v <= (double)due) v += src_period;
                    if (v - (double)due < budget) {
                        due = (uint64_t)(v - budget - 100000.0);   /* 0.1 ms for the wake-up */
                        vbl_early++;
                    }
                }
            }
            WAIT_UNTIL(rdx_now_ns() >= due, due);
            nlocked++;
        } else if (period_ns) {
            due = t0 + (uint64_t)seq * period_ns;
            WAIT_UNTIL(rdx_now_ns() >= due, due);
        }
        uint64_t t_wake = rdx_now_ns();
        uint32_t sb = (uint32_t)(seq % ns);
        WAIT_UNTIL(!(c_head == c_tail || src_busy[sb]), 0);
        if (g_stop) break;
        uint32_t slot = credit[c_head++ % CRING];
        uint64_t t_credit = rdx_now_ns();

        /* runtime control (rdx-hostd): cursor on/off, another output into the same sink */
        if (g_cursor_req >= 0) {
            with_cursor = g_cursor_req;
            g_cursor_req = -1;
            printf("[cap] cursor %s (runtime)\n", with_cursor && cur_ok ? "on" : "off");
            fflush(stdout);
        }
        if (g_output_req >= 0) {
            int want = g_output_req;
            g_output_req = -1;
            int n2 = rdx_kms_list_cap_outputs(card_fd, outs, 8);
            if (want < n2 && outs[want].plane_id) {
                O = &outs[want];
                crtc_idx = (vblank_guard || immediate) ? rdx_kms_crtc_index(card_fd, O->crtc_id) : -1;
                src_period = O->vrefresh ? 1e9 / O->vrefresh : 0;
                imm_vbl = imm_tick = 0;          /* immediate sync follows the new output's vblanks */
                imm_fb = 0;
                imm_since_vbl = 0;
                memset(&cur, 0, sizeof(cur));
                cur_ok = !rdx_kms_find_cursor_plane(card_fd, O->crtc_id, &cur);
                last_fb = 0;
                printf("[cap] switched to output %d: %s %ux%u@%u at +%u+%u (runtime)\n", want, O->name, O->w, O->h,
                       O->vrefresh, O->x, O->y);
            } else {
                for (int i = 0; i < n2; i++) if (O->crtc_id == outs[i].crtc_id) O = &outs[i];  /* list re-read */
                fprintf(stderr, "[cap] output %d not available (%d outputs), staying on %s\n", want, n2, O->name);
            }
            fflush(stdout);
        }

        /* capture: what does the chosen output scan out right now? */
        uint64_t t_cap0 = rdx_now_ns();
        if (vblank_guard && !immediate && crtc_idx >= 0 && copy_ewma > 0) {
            uint64_t tv;
            if (!rdx_kms_last_vblank(card_fd, crtc_idx, &tv)) {
                double next = (double)tv + src_period;
                while (next <= (double)t_cap0) next += src_period;
                if (next - (double)t_cap0 < copy_ewma + 3 * copy_dev + 300000.0) {
                    rdx_kms_next_vblank(card_fd, crtc_idx, &tv);
                    vbl_waits++;
                }
            }
        }
        uint64_t t_copy0 = rdx_now_ns();
        uint32_t fb_id = rdx_kms_plane_fb(card_fd, O->plane_id);
        if (!fb_id) { fprintf(stderr, "plane %u has no framebuffer\n", O->plane_id); return 1; }
        int fb_new = fb_id != last_fb;
        imm_fb = fb_id;
        if (imm_trig == 1) imm_since_vbl = 1;   /* its vblank shows this buffer: no second capture */
        if (fb_new) { fb_changes++; last_fb = fb_id; }
        struct cap_fb *cf = cap_get(&g, card_fd, cache, fb_id, t_cap0);
        if (!cf) return 1;
        glBindFramebuffer(GL_FRAMEBUFFER, ob[sb].fbo);
        glViewport(0, 0, (GLsizei)w, (GLsizei)h);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, cf->tex);
        glUniform4f(g.u_uvrect, (float)O->x / cf->w, (float)O->y / cf->h, (float)O->w / cf->w,
                    (float)O->h / cf->h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        /* cursor plane on top, premultiplied alpha (KMS default blend mode) */
        uint32_t cfb = 0;
        if (with_cursor && cur_ok) {
            int32_t cx = 0, cy = 0;
            uint32_t cw = 0, chh = 0;
            if (!rdx_kms_cursor_state(card_fd, &cur, O->crtc_id, &cfb, &cx, &cy, &cw, &chh) && cfb && cw && chh) {
                struct cap_fb *cc = cap_get(&g, card_fd, cache, cfb, t_cap0);
                if (cc) {
                    float sx = (float)w / O->w, sy = (float)h / O->h;
                    glViewport((GLint)(cx * sx), (GLint)(cy * sy), (GLsizei)(cw * sx), (GLsizei)(chh * sy));
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, cc->tex);
                    glUniform4f(g.u_uvrect, 0.0f, 0.0f, (float)cw / cc->w, (float)chh / cc->h);
                    glUniform1f(g.u_opaque, 0.0f);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glDisable(GL_BLEND);
                    glUniform1f(g.u_opaque, 1.0f);
                }
            }
        }
        if (ob[sb].host) out_readback(&ob[sb]);   /* host read-back (waits for the GPU) */
        else glFinish();                  /* the NIC reads the buffer next */
        uint64_t t_cap1 = rdx_now_ns();
        double cap_ns = (double)(t_cap1 - t_cap0);      /* incl. a vblank-guard wait: feeds the lead */
        double copy_ns = (double)(t_cap1 - t_copy0);    /* the copy alone: sizes the guard window */
        if (copy_ewma > 0) {
            double d = copy_ns > copy_ewma ? copy_ns - copy_ewma : copy_ewma - copy_ns;
            copy_dev = 0.9 * copy_dev + 0.1 * d;
            copy_ewma = 0.9 * copy_ewma + 0.1 * copy_ns;
        } else {
            copy_ewma = copy_ns;
        }
        if (cap_ewma > 0) {
            double d = cap_ns > cap_ewma ? cap_ns - cap_ewma : cap_ewma - cap_ns;
            cap_dev = 0.9 * cap_dev + 0.1 * d;
            cap_ewma = 0.9 * cap_ewma + 0.1 * cap_ns;
        } else {
            cap_ewma = cap_ns;
        }
        if (ncap < cap) capt[ncap++] = cap_ns / 1e6;

        struct rdx_frame_hdr *hdr = &hdrs[sb];
        hdr->magic = RDX_MAGIC_LIVE;
        hdr->seq = (uint32_t)seq;
        hdr->t_post_ns = rdx_now_ns();
        slot_t_post[slot] = hdr->t_post_ns;
        uint64_t raddr = hello.base_addr + (uint64_t)slot * hello.slot_bytes;
        struct ibv_sge sp = {.addr = (uintptr_t)ob[sb].host, .length = (uint32_t)pix_bytes, .lkey = ob[sb].mr->lkey};
        struct ibv_sge sh = {.addr = (uintptr_t)hdr, .length = sizeof(*hdr), .lkey = hmr->lkey};
        struct ibv_send_wr wp, wh, *bad;
        memset(&wp, 0, sizeof(wp));
        memset(&wh, 0, sizeof(wh));
        wp.opcode = IBV_WR_RDMA_WRITE;                 /* pixels, unsignaled */
        wp.sg_list = &sp; wp.num_sge = 1;
        wp.wr.rdma.remote_addr = raddr;
        wp.wr.rdma.rkey = hello.rkey;
        wp.next = &wh;
        wh.wr_id = WR_SRC_BASE + sb;                   /* header + immediate; RC keeps the order */
        wh.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        wh.sg_list = &sh; wh.num_sge = 1;
        wh.send_flags = IBV_SEND_SIGNALED;
        wh.imm_data = htonl(rdx_imm((uint32_t)seq, slot));
        wh.wr.rdma.remote_addr = raddr + hello.hdr_off;
        wh.wr.rdma.rkey = hello.rkey;
        if (ibv_post_send(qp, &wp, &bad)) { perror("post_send"); return 1; }
        src_busy[sb] = 1;
        unshown[slot] = 1;
        slot_seq[slot] = seq;
        inflight++;
        posted++;
        rdx_tlog_line(&tl, "F,%ld,%u,%llu,%llu,%llu,%llu,%llu,%llu,%d,%.1f,%u,%d,%u", seq, slot,
                      (unsigned long long)due, (unsigned long long)t_wake, (unsigned long long)t_credit,
                      (unsigned long long)t_cap0, (unsigned long long)t_cap1,
                      (unsigned long long)hdr->t_post_ns, ahead, lead / 1e3, fb_id, fb_new, cfb);

        /* anomaly summary for the journal, once per second, only if something stood out */
        double late_ms = due && t_wake > due ? (t_wake - due) / 1e6 : 0;
        double wait_ms = (t_credit - t_wake) / 1e6, capms = cap_ns / 1e6;
        sec_frames++;
        if (late_ms > 0.5) { sec_late++; if (late_ms > sec_late_max) sec_late_max = late_ms; }
        if (capms > 4.0) sec_slow++;
        if (capms > sec_cap_max) sec_cap_max = capms;
        if (wait_ms > 1.0) { sec_wait++; if (wait_ms > sec_wait_max) sec_wait_max = wait_ms; }
        uint64_t tnow = rdx_now_ns();
        rdx_tlog_tick(&tl, tnow);
        if (tnow - t_sec >= 1000000000ull) rdx_cq_clock_sync(&rcq);
        if (tnow - t_sec >= 1000000000ull) {
            if (sec_late || sec_slow || sec_wait) {
                anomalies++;
                printf("[src] ANOMALY %ld frames/s: woke late %ld (max %.2f ms), capture > 4 ms %ld (max %.2f ms), "
                       "slot wait > 1 ms %ld (max %.2f ms)\n", sec_frames, sec_late, sec_late_max, sec_slow,
                       sec_cap_max, sec_wait, sec_wait_max);
                fflush(stdout);
            }
            t_sec = tnow;
            sec_frames = sec_late = sec_slow = sec_wait = 0;
            sec_late_max = sec_cap_max = sec_wait_max = 0;
        }
        if (frames == 0 && (nrtt >= cap || ncap >= cap)) {     /* endless: rolling summary */
            printf("[src] window of %ld frames:\n", ncap);
            rdx_report("capture+draw (GPU)", capt, (size_t)ncap);
            rdx_report("post -> report (round)", rtt, (size_t)nrtt);
            rdx_cpu_report("src", t0, t0_cpu);
            if (phase_target_us > 0)
                printf("[src] phase steering: capture %.3f ms after the host vblank, arrival -> sink vblank "
                       "off target by %+.3f ms (median of the last %d), %ld steps, %ld one-frame-behind escapes\n",
                       ph_d / 1e6, ph_last_err / 1e6, PH_N, ph_steps, ph_escapes);
            printf("[src] immediate: %ld captures on commit, %ld at vblank, %ld skipped (no free slot / --fps); "
                   "vblank guard: %ld early, %ld waited; framebuffer changes: %ld\n",
                   imm_commits, imm_vblanks, imm_skipped, vbl_early, vbl_waits, fb_changes);
            fflush(stdout);
            nrtt = ncap = 0;
        }
    }
    WAIT_UNTIL(inflight == 0, 0);
    double el = (t_last_done - t0) / 1e9;
    uint64_t t_end = rdx_now_ns() + 300000000ull;     /* a KMS sink keeps the slot on screen */
    WAIT_UNTIL(credits_back >= posted, t_end);
    if (rdx_post_imm_send(qp, RDX_IMM_END, WR_END)) { perror("send END"); return 1; }
    for (int got = 0; !got;) {
        int nwc = rdx_cq_poll(&rcq, wc, 16);
        for (int k = 0; k < nwc; k++) if (wc[k].wr_id == WR_END) got = 1;
    }

    printf("[src] done: %ld frames of %s in %.3f s = %.1f fps, %.3f GB/s, scanout fb changes %ld\n", done,
           O->name, el, done / el, done * (double)pix_bytes / el / 1e9, fb_changes);
    rdx_report("capture+draw (GPU)", capt, (size_t)ncap);
    rdx_report("post -> report (round)", rtt, (size_t)nrtt);
    rdx_report(nshown ? "sink CQE -> vblank" : "sink CQE -> free", proc, (size_t)nrtt);
    rdx_report("round - sink = network", xfer, (size_t)nrtt);
    if (nlocked)
        printf("[src] vblank lock: %ld of %ld frames, period %.3f ms, network EWMA %.3f ms, capture EWMA %.3f ms "
               "(+3 x dev %.3f ms), margin %.2f ms\n", nlocked, done, period_est / 1e6, net_ewma / 1e6,
               cap_ewma / 1e6, cap_dev / 1e6, lock_margin_ms);
    rdx_cpu_report("src", t0, t0_cpu);
    printf("[src] anomaly seconds: %ld\n", anomalies);
    rdx_tlog_close(&tl);
    if (qos_fd >= 0) close(qos_fd);
    close(sock);
    return 0;
}
