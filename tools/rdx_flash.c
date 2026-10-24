/* Test app for the end-to-end latency (tools/rdx_perf.py --latency): a fullscreen X11/GLX window that
 * is black and turns white (and back) on every key press. Run inside gamescope (Xwayland) like a game;
 * the sink watches a pixel (rdx_sink_null --watch X,Y) and the client times key press -> changed frame.
 *
 *   rdx_flash [--idle] [--swap-interval 1] [--fps 0] [--cursor] [--seconds 0] [--log FILE]
 *
 * Default: renders continuously (glClear + glXSwapBuffers, swap interval 1 = paced by the compositor),
 * input is read once per frame like a game's main loop. --idle: draws only after a key press (an
 * editor or a menu: the frame starts at the key). --swap-interval 0: no vsync in the app (the
 * compositor takes the newest frame, like a game with vsync off). --fps N: at most N frames per second (a
 * still app with a frame limiter, e.g. a launcher or vkcube under MangoHud's fps_limit). --cursor: the window's
 * X cursor is a white 16x16 block with its hotspot top left (gamescope's cursor over the black picture, for the
 * pointer latency probe: rdx_sink_null --watch-cursor). At the end one line with frames, fps and keys
 * (to --log, else stdout). Exits on SIGTERM / SIGINT or after --seconds.
 */
#define _GNU_SOURCE
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int idle = 0, swap_interval = 1, block_cursor = 0;
    double seconds = 0, fps = 0;
    const char *log_path = NULL;
    static const struct option opts[] = {{"idle", 0, 0, 'i'}, {"seconds", 1, 0, 's'}, {"log", 1, 0, 'l'},
                                        {"swap-interval", 1, 0, 'w'}, {"fps", 1, 0, 'f'}, {"cursor", 0, 0, 'c'},
                                        {0, 0, 0, 0}};
    for (int c; (c = getopt_long(argc, argv, "", opts, NULL)) != -1;) {
        switch (c) {
        case 'i': idle = 1; break;
        case 's': seconds = atof(optarg); break;
        case 'l': log_path = optarg; break;
        case 'w': swap_interval = atoi(optarg); break;
        case 'f': fps = atof(optarg); break;
        case 'c': block_cursor = 1; break;
        default:
            fprintf(stderr, "rdx_flash [--idle] [--swap-interval N] [--fps N] [--cursor] [--seconds S] [--log FILE]\n");
            return 2;
        }
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "rdx_flash: no X display\n"); return 1; }
    int scr = DefaultScreen(dpy);
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *vi = glXChooseVisual(dpy, scr, attrs);
    if (!vi) { fprintf(stderr, "rdx_flash: no GLX visual\n"); return 1; }
    Window root = RootWindow(dpy, scr);
    XSetWindowAttributes swa = {.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone),
                                .event_mask = KeyPressMask | StructureNotifyMask | ExposureMask,
                                .background_pixel = 0};
    unsigned ww = DisplayWidth(dpy, scr), wh = DisplayHeight(dpy, scr);
    Window win = XCreateWindow(dpy, root, 0, 0, ww, wh, 0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWEventMask | CWBackPixel, &swa);
    XStoreName(dpy, win, "rdx_flash");
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False), fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(dpy, win, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);
    if (block_cursor) {                  /* a core cursor from a bitmap: no Xcursor library, no cursor theme */
        static char bits[32];
        memset(bits, 0xFF, sizeof(bits));
        Pixmap pm = XCreateBitmapFromData(dpy, win, bits, 16, 16);
        XColor white = {.red = 0xFFFF, .green = 0xFFFF, .blue = 0xFFFF, .flags = DoRed | DoGreen | DoBlue};
        XColor black = {.flags = DoRed | DoGreen | DoBlue};
        XDefineCursor(dpy, win, XCreatePixmapCursor(dpy, pm, pm, &white, &black, 0, 0));
        XFreePixmap(dpy, pm);
    }
    XMapWindow(dpy, win);
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) { fprintf(stderr, "rdx_flash: no GL context\n"); return 1; }
    typedef void (*swap_fn)(Display *, GLXDrawable, int);
    swap_fn swap_ext = (swap_fn)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    if (swap_ext) swap_ext(dpy, win, swap_interval);

    int white = 0, dirty = 2;
    long frames = 0, keys = 0;
    double t0 = now_s(), t_next = t0;
    while (!g_stop && (seconds <= 0 || now_s() - t0 < seconds)) {
        if (fps > 0 && !idle) {                          /* the frame limiter: sleep until the next frame is due */
            double t = now_s();
            if (t < t_next) {
                struct timespec d = {.tv_sec = (time_t)(t_next - t), .tv_nsec = (long)((t_next - t - (time_t)(t_next - t)) * 1e9)};
                nanosleep(&d, NULL);
            }
            t_next = (t_next + 1.0 / fps > now_s()) ? t_next + 1.0 / fps : now_s() + 1.0 / fps;
        }
        if (idle && !dirty && !XPending(dpy)) {         /* wait for input (or a signal) */
            struct pollfd p = {.fd = ConnectionNumber(dpy), .events = POLLIN};
            poll(&p, 1, 200);
        }
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == KeyPress) { white = !white; keys++; dirty = 2; }
            else if (ev.type == Expose || ev.type == ConfigureNotify) dirty = 2;
        }
        if (idle && !dirty) continue;
        glClearColor(white ? 1.f : 0.f, white ? 1.f : 0.f, white ? 1.f : 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
        frames++;
        if (dirty) dirty--;                              /* --idle: two frames per change (both buffers) */
    }
    double el = now_s() - t0;
    FILE *out = log_path ? fopen(log_path, "w") : stdout;
    if (!out) out = stdout;
    fprintf(out, "[flash] %ld frames in %.1f s = %.1f fps, %ld keys, mode %s, swap interval %d%s%s\n", frames, el,
            el > 0 ? frames / el : 0, keys, idle ? "idle" : "continuous", swap_interval, swap_ext ? "" : " (not settable)",
            fps > 0 && !idle ? ", limited" : "");
    if (out != stdout) fclose(out);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
