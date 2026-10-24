/* SPDX-License-Identifier: MIT
 *
 * P0-B sink, window mode: the NIC writes via RDMA straight into udmabuf
 * buffers in host RAM; the very same pages are imported into the iGPU with
 * EGL_EXT_image_dma_buf_import, without a copy, and drawn into an X11 window.
 *
 * Two checks per frame against the expected test pattern:
 *   CPU: frame header and counter pixel through the memfd mapping,
 *   GPU: four probes sampled from the imported texture and read back
 *        (shows whether the iGPU sees what the NIC wrote).
 *
 *   rdx_sink_win [--dev auto] [--bind 127.0.0.1] [--port 18650]
 *                [--res 1080p|1440p|4k|WxH] [--slots 3] [--win 1280x720]
 *                [--no-gl] [--vsync] [--mtu 4096] [--frames-max 100000]
 *                [--busy-poll] [--pm-qos-us 50]
 *
 * Waiting is event driven (CQ completion channel); --busy-poll spins.
 *
 * As rdx-clientd's sink (config "sink": "window", a client in a window of this desktop): the
 * options of rdx_sink_kms that clientd passes are understood or ignored: --mode WxH[@Hz] sets the
 * frame size (= --res), --card, --mode-fallback, --log-dir, --comp-vector, --no-late-flip and
 * --tearing are ignored (no DRM master). --list-modes-json prints the window sizes clientd offers.
 * Once the bootstrap socket listens the sink prints "[sink] listening on IP:PORT". Started as root
 * without DISPLAY (rdx-clientd), it takes DISPLAY and XAUTHORITY from the desktop session
 * (gnome-shell & co.). The frames (live content: no pattern checks) are letterboxed into the window;
 * F11 = fullscreen, closing the window ends the sink.
 * --menu-sock PATH: clientd's in-stream menu (p0b/rdx_winmenu.h): the menu, the "<App> is
 * starting…" placeholder and the link card are clientd's ARGB buffers, imported into GL without a
 * copy and blended over the frame; before the first frame, while the frames pause and on a change
 * while none come the window is redrawn without a frame (black or the last one). The source's
 * pointer hint (frame header RDX_HDR_F_REL_MOUSE) goes to clientd for the window's input.
 * The host's cursor next to the picture (common/rdx_cursor.h; offered unless --cursor picture): the
 * source draws no cursor into the frames, the window's X cursor becomes the host's picture (Xcursor,
 * scaled like the frame is letterboxed) or an empty one while the host hides it. Its position is the
 * local pointer's (the window's input sends absolute positions), so a pointer that moves needs no draw
 * here and no frame from the host at all. clientd learns it (POINTER b = 1) and tells the window's
 * input to leave the window's cursor alone (b = 2). In relative mode the pointer grab's empty cursor
 * hides it, like the game does.
 * The monitor (p0b/rdx_xmon.h): the window opens on --monitor NAME (the one it was on last time,
 * remembered by clientd), else on the primary, else the one under the pointer; when the frame
 * has exactly that monitor's size it opens fullscreen there (native resolution, nothing scaled;
 * --no-auto-fullscreen: a normal window). "[sink] on monitor NAME WxH+X+Y" whenever the window
 * changes monitors. --list-modes-json lists every monitor's current mode first ("monitor": NAME,
 * "preferred" for the one the window would open on), then the usual window sizes.
 *   rdx_sink_win --dev auto --bind 10.0.1.2 --port 19650 --mode 1920x1080@60 [--slots 3]
 *                [--win 1280x720] [--title TEXT] [--menu-sock /run/rdx/menu-window.sock]
 *                [--monitor DisplayPort-0] [--no-auto-fullscreen] [--vrr]
 * --vrr (rdx-clientd: default on, config "vrr"): the window shows each frame the moment it arrives
 * anyway (no swap interval); with the window fullscreen on a monitor the compositor runs with a
 * variable refresh rate (GNOME/mutter: current mode "refresh-rate-mode" "variable", asked with
 * tools/rdx-mutter-display.py as the desktop user) the refresh follows the frames, so the host may
 * send at the app's pace: "[sink] display vrr=1 min=48.000 max=144.000 hz=144.000" (min from
 * mutter's "min-refresh-rate", else the EDID range limits), again whenever it changes (fullscreen
 * off, another monitor). Below the minimum the last frame is drawn again (low framerate
 * compensation).
 *
 * --local PATH: a window on this computer for rdx_gs_src --peer local:PATH (gs/rdx_local_proto.h),
 *   no NIC: rdx_gs_src hands over dma-bufs of its host slots, the window imports them into its GPU
 *   (the iGPU shares the RAM: no copy) and draws each new frame with vsync, so the desktop's
 *   refresh leads. Credit after the draw, "shown" after the swap, a frame replaced before it was
 *   drawn is reported dropped. F11 = fullscreen (GNOME then unredirects it), closing the window
 *   ends it.
 *   Frames have the window's size (fullscreen: the monitor's); a resize asks rdx_gs_src for frames of
 *   the new size ~300 ms after the last change (MODE -> RECONFIG). The window's own keyboard and
 *   mouse go to the app while it has the focus (INPUT -> uinput "rdx: local window ..."), F11 stays ours.
 *     rdx_sink_win --local PATH [--hz 143.998] [--slots 3] [--win 1280x720]
 *                  [--fullscreen] [--title TEXT] [--seconds 0]
 */
#define _GNU_SOURCE
#include "../common/rdx_common.h"
#include "../gs/rdx_local_proto.h"
#include "rdx_xwin.h"
#include "rdx_winmenu.h"
#include "rdx_xmon.h"
#include "../common/rdx_kms.h"
#include "../common/rdx_cursor.h"
#include <pwd.h>
#include <glob.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdalign.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#ifdef RDX_HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif
#include <linux/input-event-codes.h>
#include <drm/drm_fourcc.h>   /* kernel UAPI (linux-libc-dev), no libdrm-dev needed */

#define WR_RECV   0x200u
#define WR_CREDIT 0x400u
#define NRECV     32
#define NPROBE    4

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

/* ---------------- GL ---------------- */

struct gl {
    Display *xdpy;
    Window win;
    EGLDisplay dpy;
    EGLSurface surf;
    EGLContext ctx;
    GLuint prog, vbo, probe_fbo, probe_tex;
    GLint a_pos, u_uvrect, u_tex;
    GLuint tex[RDX_MAX_SLOTS];
    EGLImageKHR img[RDX_MAX_SLOTS];
    GLuint ov_tex[RDX_MENU_NBUF];        /* clientd's menu buffers (ARGB8888 premultiplied) */
    EGLImageKHR ov_img[RDX_MENU_NBUF];
    int win_w, win_h;
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
    "void main() { gl_FragColor = texture2D(tex, uv); }\n";

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

/* where gl_init puts the window: root position, and fullscreen from the first map (RDMA path) */
static int g_place_x, g_place_y, g_place_set, g_place_fs;

static int gl_init(struct gl *g, int win_w, int win_h, int vsync)
{
    g->win_w = win_w; g->win_h = win_h;
    g->xdpy = XOpenDisplay(NULL);
    if (!g->xdpy) { fprintf(stderr, "XOpenDisplay failed (DISPLAY/XAUTHORITY?)\n"); return -1; }
    g->dpy = eglGetDisplay((EGLNativeDisplayType)g->xdpy);
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) {
        fprintf(stderr, "eglInitialize failed\n"); return -1;
    }
    const char *ext = eglQueryString(g->dpy, EGL_EXTENSIONS);
    if (!strstr(ext, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "EGL_EXT_image_dma_buf_import missing\n"); return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(g->dpy, cattr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return -1;
    }
    EGLint vid;
    eglGetConfigAttrib(g->dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl = {.visualid = (VisualID)vid}, *vi;
    int nvi;
    vi = XGetVisualInfo(g->xdpy, VisualIDMask, &tmpl, &nvi);
    Window root = DefaultRootWindow(g->xdpy);
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(g->xdpy, root, vi ? vi->visual : DefaultVisual(g->xdpy, 0), AllocNone);
    swa.event_mask = StructureNotifyMask;
    g->win = XCreateWindow(g->xdpy, root, g_place_x, g_place_y, win_w, win_h, 0,
                           vi ? vi->depth : CopyFromParent, InputOutput,
                           vi ? vi->visual : CopyFromParent, CWColormap | CWEventMask, &swa);
    XStoreName(g->xdpy, g->win, "rdma-display P0-B");
    if (g_place_set) {                   /* on the chosen monitor: a user-given position the WM keeps */
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = USPosition | PPosition;
        sh.x = g_place_x;
        sh.y = g_place_y;
        XSetWMNormalHints(g->xdpy, g->win, &sh);
    }
    if (g_place_fs) {                    /* fullscreen from the first map: no window, no scaling */
        Atom st = XInternAtom(g->xdpy, "_NET_WM_STATE", False), fs = XInternAtom(g->xdpy, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(g->xdpy, g->win, st, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);
    }
    XMapWindow(g->xdpy, g->win);
    XFlush(g->xdpy);
    if (vi) XFree(vi);

    g->surf = eglCreateWindowSurface(g->dpy, cfg, (EGLNativeWindowType)g->win, NULL);
    EGLint xattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, xattr);
    if (g->surf == EGL_NO_SURFACE || g->ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx)) {
        fprintf(stderr, "EGL surface/context failed 0x%x\n", eglGetError()); return -1;
    }
    eglSwapInterval(g->dpy, vsync ? 1 : 0);
    printf("[gl] EGL %s / GL_RENDERER=%s\n", eglQueryString(g->dpy, EGL_VENDOR),
           (const char *)glGetString(GL_RENDERER));

    g->prog = glCreateProgram();
    glAttachShader(g->prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g->prog, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(g->prog, 0, "pos");
    glLinkProgram(g->prog);
    glUseProgram(g->prog);
    g->a_pos = 0;
    g->u_uvrect = glGetUniformLocation(g->prog, "uvrect");
    g->u_tex = glGetUniformLocation(g->prog, "tex");
    glUniform1i(g->u_tex, 0);

    static const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    /* 4x1 target for the probes */
    glGenTextures(1, &g->probe_tex);
    glBindTexture(GL_TEXTURE_2D, g->probe_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NPROBE, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g->probe_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->probe_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->probe_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "probe FBO incomplete\n"); return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}

static int gl_import_fmt(struct gl *g, EGLImageKHR *img, GLuint *tex, int dmabuf_fd, uint32_t w, uint32_t h,
                         uint32_t stride, uint32_t fourcc)
{
    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    int with_mod = strstr(eglQueryString(g->dpy, EGL_EXTENSIONS),
                          "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
    EGLint a[32], n = 0;
    a[n++] = EGL_WIDTH;  a[n++] = (EGLint)w;
    a[n++] = EGL_HEIGHT; a[n++] = (EGLint)h;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT;      a[n++] = (EGLint)fourcc;
    a[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     a[n++] = dmabuf_fd;
    a[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; a[n++] = 0;
    a[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  a[n++] = (EGLint)stride;
    if (with_mod) {
        a[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; a[n++] = (EGLint)(DRM_FORMAT_MOD_LINEAR & 0xffffffff);
        a[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; a[n++] = (EGLint)(DRM_FORMAT_MOD_LINEAR >> 32);
    }
    a[n++] = EGL_NONE;
    *img = create_image(g->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (*img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "eglCreateImageKHR(%ux%u) failed: 0x%x\n", w, h, eglGetError());
        return -1;
    }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, *tex);
    /* the menu is drawn scaled when the window is smaller than the frame: linear */
    GLint filt = fourcc == DRM_FORMAT_ARGB8888 ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, filt);
    target_tex(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)*img);
    if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "EGLImage -> texture failed\n"); return -1; }
    return 0;
}

static int gl_import(struct gl *g, int slot, int dmabuf_fd, uint32_t w, uint32_t h, uint32_t stride)
{
    return gl_import_fmt(g, &g->img[slot], &g->tex[slot], dmabuf_fd, w, h, stride, DRM_FORMAT_XRGB8888);
}

/* ---------------- the in-stream menu over the frame (p0b/rdx_winmenu.h) ---------------- */

static struct rdx_winmenu g_menu = {.sock = -1, .shown = -1, .want = -1};
static int g_redraw;                     /* the window needs a draw without a new frame (expose, resize) */

/* clientd's memfd -> udmabuf -> EGLImage (the GPU reads the very pages clientd renders into) */
static int menu_import(void *user, int i, int memfd, uint32_t w, uint32_t h, uint32_t stride)
{
    struct gl *g = user;
    struct stat st;
    int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0 || fstat(memfd, &st)) { if (ufd >= 0) close(ufd); return -1; }
    struct udmabuf_create uc = {.memfd = (uint32_t)memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0,
                                .size = (uint64_t)st.st_size};
    int db = ioctl(ufd, UDMABUF_CREATE, &uc);
    close(ufd);
    if (db < 0) { perror("[menu] UDMABUF_CREATE"); return -1; }
    int r = gl_import_fmt(g, &g->ov_img[i], &g->ov_tex[i], db, w, h, stride, DRM_FORMAT_ARGB8888);
    close(db);                           /* the EGLImage holds the buffer */
    return r;
}

/* the frame (slot, -1 = black) letterboxed into the window and the menu buffer ov (-1 = none) over it */
static void win_compose(struct gl *g, int slot, uint32_t w, uint32_t h, int ov)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->win_w, g->win_h);
    glClear(GL_COLOR_BUFFER_BIT);
    int vx, vy, vw, vh;
    rdx_letterbox(g->win_w, g->win_h, w, h, &vx, &vy, &vw, &vh);
    glActiveTexture(GL_TEXTURE0);
    glUniform4f(g->u_uvrect, 0.0f, 1.0f, 1.0f, -1.0f);   /* row 0 at the top */
    if (slot >= 0) {
        glViewport(vx, g->win_h - vy - vh, vw, vh);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->tex[slot]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (ov >= 0 && g->ov_tex[ov]) {
        double sc = w ? (double)vw / w : 1;
        int ox = vx + (int)(g_menu.x * sc), oy = vy + (int)(g_menu.y * sc);
        int ow = (int)(g_menu.w * sc), oh = (int)(g_menu.h * sc);
        glViewport(ox, g->win_h - oy - oh, ow, oh);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);     /* premultiplied, like the KMS plane */
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->ov_tex[ov]);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisable(GL_BLEND);
    }
    g_menu.shown = ov;
}

static void x_pump(struct gl *g);

/* probes first (into the 4x1 FBO, read back), then the picture into the window */
static int gl_frame(struct gl *g, int slot, uint32_t w, uint32_t h,
                    const uint32_t px[NPROBE], const uint32_t py[NPROBE], uint8_t out[NPROBE][4], int probe)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->tex[slot]);

    if (probe) {                         /* the test pattern: what the GPU sees, read back */
        glBindFramebuffer(GL_FRAMEBUFFER, g->probe_fbo);
        for (int i = 0; i < NPROBE; i++) {
            glViewport(i, 0, 1, 1);
            glUniform4f(g->u_uvrect, (px[i] + 0.5f) / w, (py[i] + 0.5f) / h, 0.0f, 0.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glReadPixels(0, 0, NPROBE, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    }
    win_compose(g, slot, w, h, rdx_winmenu_current(&g_menu));
    if (!eglSwapBuffers(g->dpy, g->surf)) return -1;
    glFinish();   /* after this the GPU no longer reads the slot: the credit may go back */
    x_pump(g);
    return 0;
}

/* ---------------- local mode: a window on this computer for rdx_gs_src ---------------- */

static volatile sig_atomic_t g_lstop;
static void on_lstop(int s) { (void)s; g_lstop = 1; }
static uint64_t g_lgen;                 /* generation of the slots we have (BUFS) */

static void lsend(int fd, uint32_t type, uint32_t slot, int64_t seq, uint64_t t, uint64_t a)
{
    struct rdx_local_msg m = {.magic = RDX_LOCAL_MAGIC, .type = type, .slot = slot, .seq = seq, .t_ns = t, .a = a,
                              .b = g_lgen};
    send(fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* the window's own keyboard / mouse while it has the focus -> rdx_gs_src (uinput "rdx: ...") */
static void linput(int fd, int kind, int code, int value)
{
    if (fd < 0) return;
    struct rdx_local_msg m = {.magic = RDX_LOCAL_MAGIC, .type = RDX_LOCAL_INPUT, .slot = (uint32_t)kind,
                              .n = (uint32_t)code, .seq = value};
    send(fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void lin_out(void *user, int kind, int code, int value) { linput(*(int *)user, kind, code, value); }

static void set_fullscreen(struct gl *g, int on)
{
    Atom st = XInternAtom(g->xdpy, "_NET_WM_STATE", False), fs = XInternAtom(g->xdpy, "_NET_WM_STATE_FULLSCREEN", False);
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.window = g->win;
    e.xclient.message_type = st;
    e.xclient.format = 32;
    e.xclient.data.l[0] = on ? 1 : 0;
    e.xclient.data.l[1] = (long)fs;
    e.xclient.data.l[3] = 1;
    XSendEvent(g->xdpy, DefaultRootWindow(g->xdpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(g->xdpy);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* --local PATH: listen on PATH, take one rdx_gs_src (--peer local:PATH), draw its slots */
static int local_main(const char *path, uint32_t w, uint32_t h, double hz, uint32_t ns, uint32_t win_w, uint32_t win_h,
                      int fullscreen, const char *title, double seconds)
{
    struct gl g;
    memset(&g, 0, sizeof(g));
    if (gl_init(&g, (int)win_w, (int)win_h, 1)) return 1;     /* vsync: the desktop's refresh leads */
    char t[160];
    snprintf(t, sizeof(t), "%s", title);
    XStoreName(g.xdpy, g.win, t);
    Atom del = XInternAtom(g.xdpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g.xdpy, g.win, &del, 1);
    XSelectInput(g.xdpy, g.win, StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | FocusChangeMask | EnterWindowMask);
    XkbSetDetectableAutoRepeat(g.xdpy, True, NULL);  /* held keys: one press, no fake releases */
    if (fullscreen) set_fullscreen(&g, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(g.dpy, g.surf);

    int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(lfd, 1)) { perror(path); return 1; }
    signal(SIGINT, on_lstop);
    signal(SIGTERM, on_lstop);
    signal(SIGPIPE, SIG_IGN);
    printf("[local] window 0x%lx %ux%u, frames %ux%u @ %.3f Hz, %u slots; waiting for rdx_gs_src on %s\n",
           (unsigned long)g.win, win_w, win_h, w, h, hz, ns, path);
    fflush(stdout);

    int sfd = -1, have[RDX_MAX_SLOTS] = {0}, fs = fullscreen;
    int pending = -1;                    /* newest frame not drawn yet */
    int64_t pend_seq = 0;
    uint64_t pend_arr = 0, t0 = rdx_now_ns();
    const long cap = 200000;
    double *vis = calloc((size_t)cap, sizeof(double));
    long nshown = 0, ndrop = 0, nvis = 0, nmode = 0;
    uint64_t t_resize = 0;                /* debounce: MODE ~300 ms after the last resize */
    struct rdx_xin xi;                    /* the window's keyboard / mouse -> evdev (rdx_xwin.h) */
    memset(&xi, 0, sizeof(xi));
    uint64_t t_first = 0, t_last = 0;
    while (!g_lstop && (seconds <= 0 || rdx_now_ns() - t0 < (uint64_t)(seconds * 1e9))) {
        /* X events: resize, F11 fullscreen, close */
        while (XPending(g.xdpy)) {
            XEvent ev;
            XNextEvent(g.xdpy, &ev);
            if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width != g.win_w || ev.xconfigure.height != g.win_h) t_resize = rdx_now_ns();
                g.win_w = ev.xconfigure.width; g.win_h = ev.xconfigure.height;
            } else if (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_F11) {
                set_fullscreen(&g, fs = !fs);          /* ours, not the app's */
            } else if (rdx_xin_event(&xi, &ev, g.win_w, g.win_h, w, h, lin_out, &sfd)) {
                /* keyboard / mouse of the window went to rdx_gs_src */
            } else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == del) g_lstop = 1;
        }
        /* resized: ~300 ms after the last change, ask for frames of the window's size */
        if (t_resize && sfd >= 0 && rdx_now_ns() - t_resize > 300000000ull) {
            t_resize = 0;
            if (((uint32_t)g.win_w & ~1u) != w || ((uint32_t)g.win_h & ~1u) != h) {
                struct rdx_local_msg mm = {.magic = RDX_LOCAL_MAGIC, .type = RDX_LOCAL_MODE, .a = (uint64_t)g.win_w,
                                           .b = (uint64_t)g.win_h};
                send(sfd, &mm, sizeof(mm), MSG_NOSIGNAL | MSG_DONTWAIT);
                nmode++;
            }
        }
        struct pollfd p[3] = {{.fd = sfd >= 0 ? sfd : lfd, .events = POLLIN}, {.fd = ConnectionNumber(g.xdpy), .events = POLLIN}};
        if (pending < 0 && poll(p, 2, 100) <= 0) continue;
        if (sfd < 0 && (p[0].revents & POLLIN)) {
            sfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (sfd < 0) continue;
            /* frames of the window's own size (fullscreen: the monitor's); resizes follow (MODE) */
            XSync(g.xdpy, False);
            while (XPending(g.xdpy)) {
                XEvent ev;
                XNextEvent(g.xdpy, &ev);
                if (ev.type == ConfigureNotify) { g.win_w = ev.xconfigure.width; g.win_h = ev.xconfigure.height; }
            }
            if (g.win_w >= 320 && g.win_h >= 200) { w = (uint32_t)g.win_w & ~1u; h = (uint32_t)g.win_h & ~1u; }
            uint32_t stride = (w * 4 + 255) & ~255u;
            struct rdx_hello hello;
            memset(&hello, 0, sizeof(hello));
            hello.magic = RDX_MAGIC;
            hello.width = w; hello.height = h; hello.stride = stride; hello.fourcc = DRM_FORMAT_XRGB8888;
            hello.nslots = ns;
            hello.hdr_off = align_up((uint64_t)stride * h, 4096);
            hello.slot_bytes = align_up(hello.hdr_off + sizeof(struct rdx_frame_hdr), 4096);
            hello.vblank_ns = (uint64_t)(1e9 / hz + 0.5);
            send(sfd, &hello, sizeof(hello), MSG_NOSIGNAL);
            memset(have, 0, sizeof(have));
            printf("[local] rdx_gs_src connected\n");
            fflush(stdout);
            continue;
        }
        /* messages from rdx_gs_src */
        while (sfd >= 0) {
            struct rdx_local_msg m;
            alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * RDX_MAX_SLOTS)];
            struct iovec iov = {.iov_base = &m, .iov_len = sizeof(m)};
            struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
            ssize_t r = recvmsg(sfd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                printf("[local] rdx_gs_src went away\n");
                close(sfd);
                sfd = -1;
                pending = -1;
                break;
            }
            if (r != (ssize_t)sizeof(m) || m.magic != RDX_LOCAL_MAGIC) break;
            if (m.type == RDX_LOCAL_END) { g_lstop = 1; break; }
            if (m.type == RDX_LOCAL_BUFS) {
                struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
                int nf = c && c->cmsg_type == SCM_RIGHTS ? (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)) : 0;
                int fds[RDX_MAX_SLOTS];
                if (nf > RDX_MAX_SLOTS) nf = RDX_MAX_SLOTS;
                if (nf) memcpy(fds, CMSG_DATA(c), sizeof(int) * (size_t)nf);
                if (m.a && m.slot) { w = (uint32_t)m.a; h = m.slot; }   /* the slots' frame size */
                g_lgen = m.b;
                pending = -1;                    /* a frame of the old slots: never drawn */
                uint32_t stride = (w * 4 + 255) & ~255u;
                for (int i = 0; i < nf; i++) {
                    if (g.img[i]) { glDeleteTextures(1, &g.tex[i]); g.img[i] = EGL_NO_IMAGE_KHR; }
                    have[i] = !gl_import(&g, i, fds[i], w, h, stride);
                    close(fds[i]);
                }
                printf("[local] %d slots of %ux%u imported (no copy: the window samples rdx_gs_src's host RAM)\n", nf, w, h);
                fflush(stdout);
                continue;
            }
            if (m.type != RDX_LOCAL_FRAME || m.slot >= ns || m.b != g_lgen) continue;
            if (pending >= 0) {                  /* replaced before it was drawn */
                lsend(sfd, RDX_LOCAL_DROP, (uint32_t)pending, pend_seq, rdx_now_ns(), 0);
                lsend(sfd, RDX_LOCAL_CREDIT, (uint32_t)pending, pend_seq, rdx_now_ns(), 0);
                ndrop++;
            }
            pending = (int)m.slot;
            pend_seq = m.seq;
            pend_arr = rdx_now_ns();
        }
        if (pending < 0 || sfd < 0) continue;
        int slot = pending;
        pending = -1;
        if (have[slot]) {
            /* letterboxed into the window, row 0 at the top */
            double sx = (double)g.win_w / w, sy = (double)g.win_h / h, s = sx < sy ? sx : sy;
            int vw = (int)(w * s), vh = (int)(h * s);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, g.win_w, g.win_h);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport((g.win_w - vw) / 2, (g.win_h - vh) / 2, vw, vh);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, g.tex[slot]);
            glUniform4f(g.u_uvrect, 0.0f, 1.0f, 1.0f, -1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();                          /* the slot is in our back buffer: free it */
        }
        lsend(sfd, RDX_LOCAL_CREDIT, (uint32_t)slot, pend_seq, rdx_now_ns(), 0);
        if (have[slot]) eglSwapBuffers(g.dpy, g.surf);   /* vsync: returns at the desktop's refresh */
        uint64_t tv = rdx_now_ns();
        lsend(sfd, RDX_LOCAL_SHOWN, (uint32_t)slot, pend_seq, tv, tv - pend_arr);
        if (nvis < cap) vis[nvis++] = (tv - pend_arr) / 1e6;
        if (!t_first) t_first = tv;
        t_last = tv;
        nshown++;
    }
    double el = t_last > t_first ? (t_last - t_first) / 1e9 : 0;
    printf("[local] %ld frames shown in %.1f s (%.1f fps), %ld replaced before drawing, %ld resizes\n", nshown, el,
           el > 0 ? (nshown - 1) / el : 0, ndrop, nmode);
    if (nvis) {
        qsort(vis, (size_t)nvis, sizeof(double), cmp_dbl);
        printf("[local] arrival -> visible (after the vsync'ed swap) p50 %.2f p90 %.2f p99 %.2f ms\n", vis[nvis / 2],
               vis[nvis * 9 / 10], vis[nvis * 99 / 100]);
    }
    if (sfd >= 0) { lsend(sfd, RDX_LOCAL_END, 0, 0, rdx_now_ns(), 0); close(sfd); }
    unlink(path);
    return 0;
}

/* ---------------- as rdx-clientd's sink ---------------- */

/* the frame sizes rdx-clientd offers for a window (its mode list) */
static int list_modes_json(const char *want_mon)
{
    static const int sz[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
    static const int hz[] = {60, 120, 144};
    /* the monitors of this desktop: "match this monitor" (native size and refresh) */
    struct rdx_mon mon[RDX_MAX_MONS];
    int nmon = 0, pick = -1;
    rdx_x_desktop_env();
    Display *d = XOpenDisplay(NULL);
    if (d) {
        nmon = rdx_x_monitors(d, mon, RDX_MAX_MONS);
        pick = rdx_mon_pick(d, mon, nmon, want_mon);
        XCloseDisplay(d);
    }
    printf("{\"card\":\"window\",\"connector\":\"window\",\"modes\":[");
    int first = 1;
    for (int i = 0; i < nmon; i++) {
        int mhz = (int)(mon[i].hz + 0.5);
        if (mhz <= 0) mhz = 60;
        printf("%s{\"w\":%d,\"h\":%d,\"hz\":%d,\"hz_exact\":%.3f,\"derived\":false,\"preferred\":%s,\"monitor\":\"%s\","
               "\"monitor_primary\":%s}", first ? "" : ",", mon[i].w, mon[i].h, mhz, mon[i].hz, i == pick ? "true" : "false",
               mon[i].name, mon[i].primary ? "true" : "false");
        first = 0;
    }
    for (size_t i = 0; i < sizeof(sz) / sizeof(sz[0]); i++)
        for (size_t k = 0; k < sizeof(hz) / sizeof(hz[0]); k++) {
            int dup = 0;
            for (int m = 0; m < nmon; m++) dup |= mon[m].w == sz[i][0] && mon[m].h == sz[i][1] && (int)(mon[m].hz + 0.5) == hz[k];
            if (dup) continue;
            printf("%s{\"w\":%d,\"h\":%d,\"hz\":%d,\"derived\":false,\"preferred\":%s}", first ? "" : ",",
                   sz[i][0], sz[i][1], hz[k], !nmon && sz[i][0] == 1920 && hz[k] == 60 ? "true" : "false");
            first = 0;
        }
    printf("]}\n");
    return 0;
}

/* window events of the RDMA path: size, F11, close */
static volatile sig_atomic_t g_closed;
static Atom g_wm_del;
static int g_fs;
static struct rdx_mon g_mons[RDX_MAX_MONS];
static int g_nmons, g_cur_mon = -2;
/* ---------------- variable refresh (the compositor's view of the monitors) ---------------- */

struct mut_mon { char conn[32]; int x, y, w, h, vrr; double hz, min_hz; };
static struct mut_mon g_mut[RDX_MAX_MONS];
static int g_nmut = -1, g_want_vrr, g_vrr;      /* g_nmut -1 = not asked yet */
static double g_vrr_min, g_vrr_max, g_disp_hz;
static uint64_t g_t_swap;                       /* last swap (rdx_now_ns), for low framerate compensation */
static char g_disp_line[160];

/* tools/rdx-mutter-display.py next to this binary's tree, as the desktop user (owner of XAUTHORITY) */
static void mutter_query(void)
{
    g_nmut = 0;
    char exe[512], helper[600], cmd[1200];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) *sl = 0;                                /* .../p0b */
    snprintf(helper, sizeof(helper), "%s", getenv("RDX_MUTTER_HELPER") ? getenv("RDX_MUTTER_HELPER") : "");
    if (!helper[0]) snprintf(helper, sizeof(helper), "%s/../tools/rdx-mutter-display.py", exe);
    if (access(helper, R_OK)) return;
    struct stat st;
    const char *xa = getenv("XAUTHORITY");
    struct passwd *pw = getuid() == 0 && xa && !stat(xa, &st) && st.st_uid ? getpwuid(st.st_uid) : NULL;
    if (pw)
        snprintf(cmd, sizeof(cmd), "timeout 5 runuser -u '%s' -- env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus "
                 "python3 '%s' 2>/dev/null", pw->pw_name, (unsigned)pw->pw_uid, helper);
    else
        snprintf(cmd, sizeof(cmd), "timeout 5 python3 '%s' 2>/dev/null", helper);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    static char out[8192];
    size_t len = fread(out, 1, sizeof(out) - 1, f);
    pclose(f);
    out[len] = 0;
    /* {"monitors": [{"connector": "DP-1", "x": 2560, "y": 0, ..., "w": 2560, "h": 1440, "hz": 143.999,
     *  "vrr": true, "min_hz": 48}, ...]}: each object read field by field */
    for (char *o = strstr(out, "{\"connector\""); o && g_nmut < RDX_MAX_MONS; o = strstr(o + 1, "{\"connector\"")) {
        struct mut_mon *m = &g_mut[g_nmut];
        memset(m, 0, sizeof(*m));
        char *end = strchr(o, '}');
        if (!end) break;
        *end = 0;
        sscanf(o, "{\"connector\": \"%31[^\"]\"", m->conn);
        char *v;
        if ((v = strstr(o, "\"x\": "))) m->x = atoi(v + 5);
        if ((v = strstr(o, "\"y\": "))) m->y = atoi(v + 5);
        if ((v = strstr(o, "\"w\": "))) m->w = atoi(v + 5);
        if ((v = strstr(o, "\"h\": "))) m->h = atoi(v + 5);
        if ((v = strstr(o, "\"hz\": "))) m->hz = atof(v + 6);
        if ((v = strstr(o, "\"min_hz\": "))) m->min_hz = atof(v + 10);
        m->vrr = strstr(o, "\"vrr\": true") != NULL;
        *end = '}';
        g_nmut++;
    }
    for (int i = 0; i < g_nmut; i++)
        printf("[sink] compositor: %s %dx%d+%d+%d @ %.3f Hz, refresh %s%s\n", g_mut[i].conn, g_mut[i].w, g_mut[i].h,
               g_mut[i].x, g_mut[i].y, g_mut[i].hz, g_mut[i].vrr ? "variable" : "fixed",
               g_mut[i].min_hz > 0 ? " (min reported)" : "");
    fflush(stdout);
}

/* the monitor's lowest refresh from its EDID (/sys/class/drm/card*-CONNECTOR/edid), 0 = unknown */
static double edid_min_hz(const char *conn)
{
    char pat[96];
    snprintf(pat, sizeof(pat), "/sys/class/drm/card*-%s/edid", conn);
    glob_t gl;
    double r = 0;
    if (glob(pat, 0, NULL, &gl)) return 0;
    for (size_t i = 0; i < gl.gl_pathc && !r; i++) {
        FILE *f = fopen(gl.gl_pathv[i], "rb");
        if (!f) continue;
        uint8_t e[512];
        size_t n = fread(e, 1, sizeof(e), f);
        fclose(f);
        struct rdx_kms_range rg;
        if (!rdx_edid_range_bytes(e, n, &rg) && rg.vmin_hz) r = rg.vmin_hz;
    }
    globfree(&gl);
    return r;
}

/* VRR now: the window fullscreen on a monitor the compositor drives with a variable refresh; the
 * "[sink] display" line whenever the answer changes (rdx-clientd tells the host) */
static void display_update(struct gl *g)
{
    if (!g->xdpy || g_cur_mon < 0 || g_cur_mon >= g_nmons) return;
    const struct rdx_mon *mon = &g_mons[g_cur_mon];
    if (g_want_vrr && g_nmut < 0) mutter_query();
    const struct mut_mon *mm = NULL;
    for (int i = 0; i < g_nmut; i++)
        if (g_mut[i].x == mon->x && g_mut[i].y == mon->y) mm = &g_mut[i];
    int fs = g->win_w == mon->w && g->win_h == mon->h;
    g_disp_hz = mm ? mm->hz : mon->hz;
    g_vrr = g_want_vrr && fs && mm && mm->vrr;
    g_vrr_min = g_vrr ? (mm->min_hz > 0 ? mm->min_hz : edid_min_hz(mm->conn)) : 0;
    g_vrr_max = g_vrr ? mm->hz : g_disp_hz;
    char line[160];
    snprintf(line, sizeof(line), "[sink] display vrr=%d min=%.3f max=%.3f hz=%.3f", g_vrr, g_vrr_min, g_vrr_max, g_disp_hz);
    if (!strcmp(line, g_disp_line)) return;
    snprintf(g_disp_line, sizeof(g_disp_line), "%s", line);
    printf("%s\n", line);
    fflush(stdout);
}

/* the monitor the window's centre is on: logged when it changes (clientd remembers the last one) */
static void track_monitor(struct gl *g)
{
    if (!g_nmons) return;
    int rx, ry;
    Window child;
    if (!XTranslateCoordinates(g->xdpy, g->win, DefaultRootWindow(g->xdpy), g->win_w / 2, g->win_h / 2, &rx, &ry, &child))
        return;
    int k = rdx_mon_at(g_mons, g_nmons, rx, ry);
    if (k < 0 || k == g_cur_mon) return;
    g_cur_mon = k;
    g_nmut = -1;                         /* another monitor: ask the compositor again */
    printf("[sink] on monitor %s %dx%d+%d+%d @ %.2f Hz, window %dx%d%s\n", g_mons[k].name, g_mons[k].w, g_mons[k].h,
           g_mons[k].x, g_mons[k].y, g_mons[k].hz, g->win_w, g->win_h,
           g->win_w == g_mons[k].w && g->win_h == g_mons[k].h ? " (fullscreen)" : "");
    fflush(stdout);
}

static void x_pump(struct gl *g)
{
    while (g->xdpy && XPending(g->xdpy)) {
        XEvent ev;
        XNextEvent(g->xdpy, &ev);
        if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != g->win_w || ev.xconfigure.height != g->win_h) g_redraw = 1;
            g->win_w = ev.xconfigure.width; g->win_h = ev.xconfigure.height;
            track_monitor(g);
            display_update(g);
        }
        else if (ev.type == Expose && !ev.xexpose.count) g_redraw = 1;
        else if (ev.type == KeyPress && XLookupKeysym(&ev.xkey, 0) == XK_F11) set_fullscreen(g, g_fs = !g_fs);
        else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g_wm_del) g_closed = 1;
    }
}
static void on_stop(int s) { (void)s; g_closed = 1; }

/* a draw without a new frame: before the first one, while they pause, after an expose; slot -1 = black.
 * Not while frames flow (the next frame carries the change within a period) */
static uint64_t g_t_frame;               /* last frame drawn (rdx_now_ns), 0 = none yet */
/* VRR below the monitor's minimum: the last frame once more before the panel would have to */
static uint64_t lfc_due(void)
{
    return g_vrr && g_vrr_min > 0 && g_t_swap && g_t_frame ? g_t_swap + (uint64_t)(0.9e9 / g_vrr_min) : 0;
}
static long g_lfc_draws;
static void idle_draw(struct gl *g, int slot, uint32_t w, uint32_t h)
{
    uint64_t due = lfc_due();
    int lfc = due && slot >= 0 && rdx_now_ns() >= due;
    if (!g->xdpy || (!g_redraw && !rdx_winmenu_pending(&g_menu) && !lfc)) return;
    if (!lfc && g_t_frame && rdx_now_ns() - g_t_frame < 50000000ull) return;
    g_redraw = 0;
    win_compose(g, slot, w, h, rdx_winmenu_current(&g_menu));
    eglSwapBuffers(g->dpy, g->surf);
    g_t_swap = rdx_now_ns();
    if (lfc) g_lfc_draws++;
}

/* wait for the source like rdx_tcp_listen_accept_as(), but serve the menu and the window meanwhile:
 * clientd's "<App> is starting…" shows while the host still starts the app */
static int accept_source_win(struct gl *g, const char *bind_ip, int port, uint32_t w, uint32_t h, int use_gl)
{
    if (!use_gl || g_menu.sock < 0) return rdx_tcp_listen_accept_as(bind_ip, port, "sink");
    struct sockaddr_in a;
    int one = 1, lst = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(bind_ip);
    setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lst, (struct sockaddr *)&a, sizeof(a))) { perror("bind"); return -1; }
    if (listen(lst, 1)) { perror("listen"); return -1; }
    printf("[sink] listening on %s:%d\n", bind_ip, port);    /* clientd starts the session after this */
    printf("[oob] waiting for source on %s:%d ...\n", bind_ip, port);
    fflush(stdout);
    while (!g_closed) {
        struct pollfd p[3] = {{lst, POLLIN, 0}, {g_menu.sock, POLLIN, 0}, {ConnectionNumber(g->xdpy), POLLIN, 0}};
        int tmo = g_menu.show_at ? 100 : 1000;
        if (poll(p, 3, tmo) < 0 && errno != EINTR) { perror("poll"); close(lst); return -1; }
        if (p[0].revents) break;
        if (p[1].revents) rdx_winmenu_handle(&g_menu, menu_import, g);
        x_pump(g);
        idle_draw(g, -1, w, h);
    }
    if (g_closed) { close(lst); return -1; }
    int sk = accept(lst, NULL, NULL);
    close(lst);
    if (sk < 0) { perror("accept"); return -1; }
    setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sk;
}

/* ---------------- the host's cursor as the window's X cursor (common/rdx_cursor.h) ---------------- */

static struct rdx_cur_rx g_cr;
static Cursor g_xcur, g_xblank;
static uint32_t g_xcur_serial;
static int g_xcur_vis = -1, g_xcur_w, g_xcur_h;    /* what the window shows, -1 = nothing of ours yet */
static uint64_t g_xcur_t;                          /* last XDefineCursor */
static long g_xcur_sets;

/* the window's cursor from the host's state; again after a resize (the scale) and once a second (the window's
 * input may have set its empty cursor at the very moment we took over) */
static void xcursor_update(struct gl *g, uint32_t fw, uint32_t fh)
{
    const struct rdx_cur_state *s = g_cr.st;
    if (!g->xdpy || !g_cr.up || !s) return;
    if (g_cr.failed) {                             /* gone: the window's input hides the local cursor again */
        if (g_xcur_vis >= 0) {
            XUndefineCursor(g->xdpy, g->win);
            XFlush(g->xdpy);
            g_xcur_vis = -1;
            rdx_winmenu_cursor(&g_menu, 0);
        }
        return;
    }
    if (!s->have_pos) return;                      /* nothing from the host yet: the stream's cursor rules */
    int vis = s->visible && s->serial;
    int vx, vy, vw, vh;
    rdx_letterbox(g->win_w, g->win_h, fw, fh, &vx, &vy, &vw, &vh);
    double sc = fw ? (double)vw / fw : 1.0;
    int want_w = vis ? (int)(s->w * sc + 0.5) : 0, want_h = vis ? (int)(s->h * sc + 0.5) : 0;
    uint64_t now = rdx_now_ns();
    int same = vis == g_xcur_vis && (!vis || (s->serial == g_xcur_serial && want_w == g_xcur_w && want_h == g_xcur_h));
    if (same && now - g_xcur_t < 1000000000ull) return;
#ifdef RDX_HAVE_XCURSOR
    if (!same && vis) {
        static uint32_t px[RDX_CUR_MAX * RDX_CUR_MAX];
        int ow, oh;
        rdx_cur_scale(s->px, s->w, s->h, sc, px, &ow, &oh);
        XcursorImage *img = XcursorImageCreate(ow, oh);
        if (!img) return;
        int hx = (int)(s->hot_x * sc), hy = (int)(s->hot_y * sc);
        img->xhot = (XcursorDim)(hx < ow ? hx : ow - 1);
        img->yhot = (XcursorDim)(hy < oh ? hy : oh - 1);
        for (int i = 0; i < ow * oh; i++) img->pixels[i] = px[i];     /* ARGB premultiplied, like ours */
        Cursor c = XcursorImageLoadCursor(g->xdpy, img);
        XcursorImageDestroy(img);
        if (g_xcur) XFreeCursor(g->xdpy, g_xcur);
        g_xcur = c;
    }
#endif
    if (vis) XDefineCursor(g->xdpy, g->win, g_xcur);
    else {
        if (!g_xblank) {
            static char z[1];
            Pixmap pm = XCreateBitmapFromData(g->xdpy, g->win, z, 1, 1);
            XColor black = {0};
            g_xblank = XCreatePixmapCursor(g->xdpy, pm, pm, &black, &black, 0, 0);
            XFreePixmap(g->xdpy, pm);
        }
        XDefineCursor(g->xdpy, g->win, g_xblank);
    }
    XFlush(g->xdpy);
    if (!same) g_xcur_sets++;
    if (g_xcur_vis < 0) {
        printf("[sink] cursor: the host's, as this window's cursor (next to the picture)\n");
        fflush(stdout);
    }
    g_xcur_vis = vis;
    g_xcur_serial = s->serial;
    g_xcur_w = want_w;
    g_xcur_h = want_h;
    g_xcur_t = now;
    rdx_winmenu_cursor(&g_menu, 1);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *dev = "auto", *bind_ip = "127.0.0.1", *res = "1080p", *win = "1280x720";
    int port = 18650, mtu = RDX_DEFAULT_MTU, use_gl = 1, vsync = 0, busy = 0, pm_qos_us = 50;
    uint32_t ns = 3;
    long frames_max = 100000;

    static const struct option opts[] = {
        {"dev", 1, 0, 'd'}, {"bind", 1, 0, 'b'}, {"port", 1, 0, 'P'}, {"res", 1, 0, 'r'},
        {"slots", 1, 0, 's'}, {"win", 1, 0, 'w'}, {"no-gl", 0, 0, 'G'}, {"vsync", 0, 0, 'v'},
        {"mtu", 1, 0, 'm'}, {"frames-max", 1, 0, 'n'}, {"busy-poll", 0, 0, 'B'},
        {"pm-qos-us", 1, 0, 'Q'}, {"local", 1, 0, 'L'}, {"hz", 1, 0, 'H'}, {"fullscreen", 0, 0, 'F'},
        {"title", 1, 0, 'T'}, {"seconds", 1, 0, 'S'},
        /* rdx_sink_kms options rdx-clientd passes: understood or ignored */
        {"mode", 1, 0, 'M'}, {"card", 1, 0, 'x'}, {"mode-fallback", 1, 0, 'x'}, {"menu-sock", 1, 0, 'U'},
        {"log-dir", 1, 0, 'x'}, {"comp-vector", 1, 0, 'x'}, {"no-late-flip", 0, 0, 'y'}, {"tearing", 0, 0, 'y'},
        {"list-modes-json", 0, 0, 'J'}, {"monitor", 1, 0, 'O'}, {"no-auto-fullscreen", 0, 0, 'A'}, {"vrr", 0, 0, 'R'},
        {"cursor", 1, 0, 'C'}, {0, 0, 0, 0}};
    const char *want_mon = NULL;
    int auto_fs = 1, list_modes = 0, want_cursor = 1;
    const char *local = NULL, *title = "rdma-display", *menu_sock = NULL;
    static char mres[32];
    double hz = 60, seconds = 0;
    int fullscreen = 0;
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'd': dev = optarg; break;
        case 'b': bind_ip = optarg; break;
        case 'P': port = atoi(optarg); break;
        case 'r': res = optarg; break;
        case 's': ns = (uint32_t)atoi(optarg); break;
        case 'w': win = optarg; break;
        case 'G': use_gl = 0; break;
        case 'v': vsync = 1; break;
        case 'L': local = optarg; break;
        case 'H': hz = atof(optarg); break;
        case 'F': fullscreen = 1; break;
        case 'T': title = optarg; break;
        case 'S': seconds = atof(optarg); break;
        case 'm': mtu = atoi(optarg); break;
        case 'n': frames_max = atol(optarg); break;
        case 'B': busy = 1; break;
        case 'Q': pm_qos_us = atoi(optarg); break;
        case 'M': {                                  /* WxH[@Hz] */
            snprintf(mres, sizeof(mres), "%s", optarg);
            char *at = strchr(mres, '@');
            if (at) { *at = 0; hz = atof(at + 1); }
            res = mres;
            break;
        }
        case 'U': menu_sock = optarg; break;
        case 'x': case 'y': break;
        case 'J': list_modes = 1; break;
        case 'O': want_mon = optarg; break;
        case 'A': auto_fs = 0; break;
        case 'R': g_want_vrr = 1; break;
        case 'C':
            if (!strcmp(optarg, "picture")) want_cursor = 0;
            else if (strcmp(optarg, "auto")) { fprintf(stderr, "--cursor auto|picture\n"); return 2; }
            break;
        default:  fprintf(stderr, "see the header of rdx_sink_win.c for options\n"); return 2;
        }
    }
    if (list_modes) return list_modes_json(want_mon);
    uint32_t w, h, ww, wh;
    if (rdx_parse_res(res, &w, &h) || rdx_parse_res(win, &ww, &wh) || !ns || ns > RDX_MAX_SLOTS) {
        fprintf(stderr, "invalid --res/--win/--slots\n"); return 2;
    }
    if (local) return local_main(local, w, h, hz > 0 ? hz : 60, ns, ww, wh, fullscreen, title, seconds);
    uint32_t stride = (uint32_t)align_up((uint64_t)w * 4, 256);   /* amdgpu: 256-byte pitch alignment */
    uint64_t hdr_off = align_up((uint64_t)stride * h, 4096);
    uint64_t slot_bytes = align_up(hdr_off + sizeof(struct rdx_frame_hdr), 4096);
    uint64_t total = slot_bytes * ns;

    /* host RAM: memfd -> mmap (for NIC and CPU) and one udmabuf per slot (for the iGPU) */
    int mfd = memfd_create("rdx-slots", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, (off_t)total) || fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
        perror("memfd"); return 1;
    }
    uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }
    memset(map, 0, total);
    int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0) { perror("/dev/udmabuf"); return 1; }
    int dbuf[RDX_MAX_SLOTS];
    for (uint32_t i = 0; i < ns; i++) {
        struct udmabuf_create uc = {.memfd = (uint32_t)mfd, .flags = UDMABUF_FLAGS_CLOEXEC,
                                    .offset = i * slot_bytes, .size = slot_bytes};
        dbuf[i] = ioctl(ufd, UDMABUF_CREATE, &uc);
        if (dbuf[i] < 0) { perror("UDMABUF_CREATE"); return 1; }
    }
    printf("[i] %ux%u stride=%u slots=%u slot_bytes=%llu (memfd + udmabuf, host RAM)\n",
           w, h, stride, ns, (unsigned long long)slot_bytes);

    struct gl g;
    memset(&g, 0, sizeof(g));
    if (use_gl) {
        rdx_x_desktop_env();
        {                                /* which monitor, and fullscreen when the frame is its size */
            Display *d = XOpenDisplay(NULL);
            if (d) {
                g_nmons = rdx_x_monitors(d, g_mons, RDX_MAX_MONS);
                int k = rdx_mon_pick(d, g_mons, g_nmons, want_mon);
                if (k >= 0) {
                    const struct rdx_mon *m = &g_mons[k];
                    g_place_fs = auto_fs && m->w == (int)w && m->h == (int)h;
                    g_place_set = 1;
                    g_place_x = m->x + (g_place_fs ? 0 : (m->w - (int)ww) / 2 > 0 ? (m->w - (int)ww) / 2 : 0);
                    g_place_y = m->y + (g_place_fs ? 0 : (m->h - (int)wh) / 2 > 0 ? (m->h - (int)wh) / 2 : 0);
                    if (g_place_fs) { ww = (uint32_t)m->w; wh = (uint32_t)m->h; g_fs = 1; }
                    printf("[sink] opens on monitor %s %dx%d @ %.2f Hz%s%s\n", m->name, m->w, m->h, m->hz,
                           g_place_fs ? ": fullscreen, native (the frame has its size)" : ": window",
                           want_mon && strcmp(want_mon, m->name) ? " (the remembered one is gone)" : "");
                }
                XCloseDisplay(d);
            }
        }
        if (gl_init(&g, (int)ww, (int)wh, vsync)) return 1;
        char t[200];
        snprintf(t, sizeof(t), "%s (%ux%u)", title, w, h);
        XStoreName(g.xdpy, g.win, t);
        /* rdx-clientd hands this id to the window's input sender (p4/rdx_input_win) */
        printf("[sink] window 0x%lx %ux%u, frames %ux%u\n", (unsigned long)g.win, ww, wh, w, h);
        fflush(stdout);
        g_wm_del = XInternAtom(g.xdpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(g.xdpy, g.win, &g_wm_del, 1);
        XSelectInput(g.xdpy, g.win, StructureNotifyMask | KeyPressMask | ExposureMask);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(g.dpy, g.surf);
        for (uint32_t i = 0; i < ns; i++)
            if (gl_import(&g, (int)i, dbuf[i], w, h, stride)) return 1;
        printf("[gl] %u slots imported without a copy (dma-buf, linear XRGB8888)\n", ns);
        if (menu_sock && !rdx_winmenu_init(&g_menu, menu_sock, w, h, (uint32_t)(hz * 1000 + 0.5)))
            printf("[menu] in-stream menu over the window (%s)\n", menu_sock);
        /* the display line before "listening": rdx-clientd sends it with session.start. A window that
         * opens fullscreen gets its monitor's size from the WM within a moment: wait up to 600 ms */
        for (uint64_t te = rdx_now_ns() + 600000000ull; rdx_now_ns() < te;) {
            x_pump(&g);
            if (g_cur_mon >= 0 && (!g_place_fs || (g.win_w == g_mons[g_cur_mon].w && g.win_h == g_mons[g_cur_mon].h))) break;
            struct pollfd px = {ConnectionNumber(g.xdpy), POLLIN, 0};
            poll(&px, 1, 20);
        }
        if (g_cur_mon < 0) track_monitor(&g);
        display_update(&g);
        if (!g_disp_line[0])            /* no monitor known (no XRandR): fixed refresh */
            printf("[sink] display vrr=0 min=0.000 max=%.3f hz=%.3f\n", hz, hz);
        fflush(stdout);
    }

    struct ibv_context *ctx = rdx_open_dev_ip(dev, bind_ip);   /* dev "auto": the device carrying bind_ip */
    if (!ctx) return 1;
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_mr *mr = ibv_reg_mr(pd, map, total, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!pd || !mr) { perror("pd/reg_mr"); return 1; }
    struct ibv_comp_channel *ch = NULL;
    struct ibv_cq *cq = rdx_create_cq_ev(ctx, 256, &ch);
    struct ibv_qp *qp = cq ? rdx_create_qp(pd, cq, 64) : NULL;
    if (!qp || rdx_qp_to_init(qp)) return 1;
    for (int i = 0; i < NRECV; i++)
        if (rdx_post_zero_recv(qp, WR_RECV)) { perror("post_recv"); return 1; }
    /* the host's cursor next to the picture: the window's X cursor shows it */
#ifdef RDX_HAVE_XCURSOR
    int cur_offer = want_cursor && use_gl && !rdx_cur_rx_init(&g_cr, pd, cq);
#else
    int cur_offer = 0;                   /* built without libXcursor: the cursor stays in the picture */
    (void)want_cursor;
#endif

    struct rdx_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = RDX_MAGIC;
    hello.width = w; hello.height = h; hello.stride = stride; hello.fourcc = DRM_FORMAT_XRGB8888;
    hello.nslots = ns; hello.slot_bytes = slot_bytes; hello.hdr_off = hdr_off;
    hello.base_addr = (uintptr_t)map; hello.rkey = mr->rkey;
    if (cur_offer) hello.caps |= RDX_CAP_CURSOR;
    int gidx = rdx_find_gid_index_ip(ctx, bind_ip, hello.conn.gid);   /* the GID of the seat's link IP */
    if (gidx < 0) return 1;                           /* printed by rdx_find_gid_index_ip */
    hello.conn.qpn = qp->qp_num;
    hello.conn.psn = 0x1234;
    dev = ibv_get_device_name(ctx->device);              /* "auto" resolved: the real name for the label */
    printf("[i] %s sgid_index=%d (%s) ::ffff:%u.%u.%u.%u rkey=0x%x\n", dev, gidx,
           rdx_gid_is_v2(dev, gidx) ? "RoCE v2" : "RoCE v1",
           hello.conn.gid[12], hello.conn.gid[13], hello.conn.gid[14], hello.conn.gid[15], mr->rkey);

    int sock = accept_source_win(&g, bind_ip, port, w, h, use_gl);
    if (sock < 0) return g_closed ? 0 : 1;
    struct rdx_conn peer;
    if (rdx_io_all(sock, &hello, sizeof(hello), 1) || rdx_io_all(sock, &peer, sizeof(peer), 0)) return 1;
    int cur_up = rdx_cur_rx_connect(&g_cr, sock, &peer, gidx, hello.conn.gid, rdx_parse_mtu(mtu));
    if (cur_up < 0) return 1;
    if (rdx_qp_to_rtr_rts(qp, &peer, gidx, hello.conn.psn, rdx_parse_mtu(mtu))) return 1;
    if (rdx_sync(sock)) { fprintf(stderr, "sync failed\n"); return 1; }
    printf("[i] connected, waiting for frames; cursor %s\n", cur_up ? "next to the picture (the window's X cursor)"
           : "in the picture"); fflush(stdout);
    signal(SIGTERM, on_stop);                  /* from here on: end cleanly, with the summary */
    signal(SIGINT, on_stop);

    double *lat_xfer = calloc((size_t)frames_max, sizeof(double));
    double *lat_gl   = calloc((size_t)frames_max, sizeof(double));
    double *lat_all  = calloc((size_t)frames_max, sizeof(double));
    long nf = 0, bad_hdr = 0, bad_cpu = 0, bad_gpu = 0, bad_seq_order = 0, first_bad_gpu = -1;
    long nxf = 0, foreign_clock = 0;
    uint32_t last_seq = 0xFFFFFFFFu;
    uint64_t t_first = 0, t_last = 0, t_print = rdx_now_ns();
    long nf_print = 0;
    struct ibv_wc wc[16];
    int running = 1;

    int qos_fd = rdx_pm_qos(pm_qos_us);
    int last_slot = -1;                  /* the frame on screen, for draws without a new one */
    long sec_shown = 0, sec_n = 0;       /* this second, for the menu's statistics */
    double sec_lat[1024];
    uint64_t t_stat = rdx_now_ns();
    uint64_t t0_wall = rdx_now_ns(), t0_cpu = rdx_cpu_ns();
    while (running && !g_closed) {
        int nwc = ibv_poll_cq(cq, 16, wc);
        if (nwc == 0 && !busy) {
            ibv_req_notify_cq(cq, 0);
            nwc = ibv_poll_cq(cq, 16, wc);                 /* closes the arm race */
            if (nwc == 0) {
                /* the window's events and the menu wake us too (close, F11, resize, menu changes) */
                struct pollfd pw[3] = {{ch->fd, POLLIN, 0}, {use_gl ? ConnectionNumber(g.xdpy) : -1, POLLIN, 0},
                                       {g_menu.sock, POLLIN, 0}};
                int tmo = g_menu.show_at || g_redraw ? 60 : 500;
                uint64_t ld = lfc_due(), tl = rdx_now_ns();
                if (ld) { int t = ld > tl ? (int)((ld - tl) / 1000000ull) + 1 : 0; if (t < tmo) tmo = t; }
                if (poll(pw, 3, tmo) > 0 && (pw[0].revents & POLLIN)) {
                    struct ibv_cq *ecq;
                    void *ectx;
                    while (ibv_get_cq_event(ch, &ecq, &ectx) == 0) ibv_ack_cq_events(ecq, 1);
                }
                if (pw[2].revents) rdx_winmenu_handle(&g_menu, menu_import, &g);
                if (use_gl) {
                    x_pump(&g);
                    xcursor_update(&g, w, h);
                    uint64_t tn = rdx_now_ns();
                    if (g_t_frame && tn - g_t_frame > RDX_MENU_STALL_MS * 1000000ull) rdx_winmenu_stalled(&g_menu);
                    idle_draw(&g, last_slot, w, h);
                    if (tn - t_stat >= 1000000000ull) {           /* the menu's statistics line, also when idle */
                        rdx_winmenu_stats(&g_menu, sec_shown, 0, 0, (uint32_t)(hz * 1000 + 0.5));
                        sec_shown = 0; sec_n = 0; t_stat = tn;
                    }
                }
                continue;
            }
        }
        if (nwc < 0) { fprintf(stderr, "poll_cq failed\n"); return 1; }
        for (int k = 0; k < nwc && running; k++) {
            if (rdx_cur_wr(wc[k].wr_id)) {               /* the cursor channel: never fatal for the frames */
                if (rdx_cur_rx_complete(&g_cr, wc[k].wr_id, wc[k].status == IBV_WC_SUCCESS) && use_gl) xcursor_update(&g, w, h);
                continue;
            }
            if (wc[k].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s (wr_id=0x%llx)\n", ibv_wc_status_str(wc[k].status),
                        (unsigned long long)wc[k].wr_id);
                return 1;
            }
            if (wc[k].opcode == IBV_WC_RECV) {                 /* SEND_WITH_IMM: only END */
                if (ntohl(wc[k].imm_data) == RDX_IMM_END) running = 0;
                continue;
            }
            if (wc[k].opcode != IBV_WC_RECV_RDMA_WITH_IMM) continue;   /* credit send completed */

            uint64_t t_cqe = rdx_now_ns();
            uint32_t imm = ntohl(wc[k].imm_data);
            uint32_t slot = rdx_imm_slot(imm), seq28 = rdx_imm_seq(imm);
            if (rdx_post_zero_recv(qp, WR_RECV)) { perror("repost"); return 1; }
            if (slot >= ns) { fprintf(stderr, "slot %u out of range\n", slot); return 1; }

            uint8_t *sp = map + (size_t)slot * slot_bytes;
            struct rdx_frame_hdr hdr;
            memcpy(&hdr, sp + hdr_off, sizeof(hdr));
            int live = hdr.magic == RDX_MAGIC_LIVE;          /* live content: no pattern checks */
            if (live && (hdr.flags & RDX_HDR_F_HINTS))         /* the app locks the pointer or shows it */
                rdx_winmenu_hint(&g_menu, (hdr.flags & RDX_HDR_F_REL_MOUSE) ? 1 : 0);
            if ((hdr.magic != RDX_MAGIC && !live) || rdx_seq28(hdr.seq) != seq28) bad_hdr++;
            uint32_t seq = hdr.seq;
            if (last_seq != 0xFFFFFFFFu && seq != last_seq + 1) bad_seq_order++;
            last_seq = seq;

            /* probes: counter block, bar centre, gradient, last row */
            uint32_t bx = rdx_bar_x(seq, w);
            uint32_t px[NPROBE] = {RDX_SEQ_BLK / 2, bx + RDX_BAR_W / 2, w / 2 + 7, w - 1};
            uint32_t py[NPROBE] = {RDX_SEQ_BLK / 2, h / 2, h / 3, h - 1};
            for (int i = 0; !live && i < NPROBE; i++) {
                uint32_t v = *(uint32_t *)(sp + (size_t)py[i] * stride + (size_t)px[i] * 4) & 0x00FFFFFFu;
                if (v != rdx_expected_pixel(seq, w, h, px[i], py[i])) { bad_cpu++; break; }
            }
            uint64_t t_done = t_cqe;
            if (use_gl) {
                uint8_t out[NPROBE][4];
                rdx_winmenu_frame(&g_menu);                /* the first frame takes the placeholder off */
                if (gl_frame(&g, (int)slot, w, h, px, py, out, !live)) { fprintf(stderr, "eglSwapBuffers\n"); return 1; }
                xcursor_update(&g, w, h);                  /* a resize while frames flow, the re-assert */
                t_done = rdx_now_ns();
                last_slot = (int)slot;
                g_t_frame = t_done;
                g_t_swap = t_done;
                g_redraw = 0;
                if (sec_n < 1024) sec_lat[sec_n++] = (double)(t_done - t_cqe) / 1e3;
                sec_shown++;
                if (t_done - t_stat >= 1000000000ull) {
                    qsort(sec_lat, (size_t)sec_n, sizeof(double), cmp_dbl);
                    rdx_winmenu_stats(&g_menu, sec_shown, 0, sec_n ? sec_lat[sec_n / 2] : 0, (uint32_t)(hz * 1000 + 0.5));
                    sec_shown = 0; sec_n = 0; t_stat = t_done;
                }
                for (int i = 0; !live && i < NPROBE; i++) {
                    uint32_t e = rdx_expected_pixel(seq, w, h, px[i], py[i]);
                    uint32_t v = ((uint32_t)out[i][0] << 16) | ((uint32_t)out[i][1] << 8) | out[i][2];
                    if (v != e) {
                        if (first_bad_gpu < 0) {
                            first_bad_gpu = seq;
                            fprintf(stderr, "[gpu] first mismatch frame %u probe %d (%u,%u): 0x%06x instead of 0x%06x\n",
                                    seq, i, px[i], py[i], v, e);
                        }
                        bad_gpu++;
                        break;
                    }
                }
            }
            if (rdx_post_imm_send(qp, rdx_credit_imm(slot, rdx_now_ns() - t_cqe), WR_CREDIT)) {
                perror("credit"); return 1;
            }

            if (nf < frames_max) {
                /* the post timestamp is only comparable with the same clock (one host) */
                int same_clock = t_cqe >= hdr.t_post_ns && t_cqe - hdr.t_post_ns < 10000000000ull;
                if (!same_clock) foreign_clock++;
                lat_xfer[nxf] = (double)(t_cqe - hdr.t_post_ns) / 1e6;
                lat_all[nxf]  = (double)(t_done - hdr.t_post_ns) / 1e6;
                if (same_clock) nxf++;
                lat_gl[nf]    = (double)(t_done - t_cqe) / 1e6;
            }
            if (!t_first) t_first = t_cqe;
            t_last = t_done;
            nf++;
            if (t_done - t_print >= 1000000000ull) {
                double dt = (t_done - t_print) / 1e9;
                printf("[sink] %6ld frames  %7.1f fps  %6.2f GB/s  errors hdr=%ld cpu=%ld gpu=%ld\n", nf,
                       (nf - nf_print) / dt, (nf - nf_print) * (double)(hdr_off + sizeof(hdr)) / dt / 1e9,
                       bad_hdr, bad_cpu, bad_gpu);
                fflush(stdout);
                t_print = t_done; nf_print = nf;
            }
        }
    }

    size_t nrep = (size_t)(nf < frames_max ? nf : frames_max);
    double el = (t_last - t_first) / 1e9;
    if (g_closed) printf("\n[sink] window closed / stopped\n");
    printf("\n[sink] result %ux%u, %u slots, MTU %d, GL %s, vsync %s\n", w, h, ns, mtu,
           use_gl ? "on" : "off", vsync ? "on" : "off");
    printf("  frames %ld in %.3f s = %.1f fps, %.3f GB/s payload\n", nf, el, el > 0 ? (nf - 1) / el : 0,
           el > 0 ? (nf - 1) * (double)(hdr_off + sizeof(struct rdx_frame_hdr)) / el / 1e9 : 0);
    printf("  errors: header=%ld  CPU probes=%ld  GPU probes=%ld  order=%ld\n",
           bad_hdr, bad_cpu, bad_gpu, bad_seq_order);
    if (foreign_clock)
        printf("  %ld frames with a foreign clock (second host): the source measures transfer latency\n",
               foreign_clock);
    rdx_report("post -> CQE (transfer)", lat_xfer, (size_t)nxf);
    if (use_gl) {
        rdx_report("CQE -> GPU done", lat_gl, nrep);
        rdx_report("post -> GPU done", lat_all, (size_t)nxf);
    }
    rdx_cpu_report("sink", t0_wall, t0_cpu);
    if (qos_fd >= 0) close(qos_fd);
    if (g_menu.sock >= 0) close(g_menu.sock);
    if (g_want_vrr) printf("[sink] VRR: %ld frames drawn again below %.0f Hz (LFC)\n", g_lfc_draws, g_vrr_min);
    if (g_cr.up)
        printf("[sink] cursor next to the picture: %ld positions, %ld pictures (%ld invalid), the window's cursor set %ld "
               "times%s\n", g_cr.st->n_pos, g_cr.st->n_img, g_cr.st->n_bad, g_xcur_sets, g_cr.failed ? ", the channel failed" : "");
    close(sock);
    return (bad_hdr || bad_cpu || bad_gpu) ? 3 : 0;
}
