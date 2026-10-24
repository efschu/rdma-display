/* SPDX-License-Identifier: MIT
 * In-stream menu, sink side: overlay plane, see rdx_overlay.h. Raw kernel UAPI, no libdrm.
 */
#define _GNU_SOURCE
#include "rdx_overlay.h"
#include "../common/rdx_kms.h"
#include "../common/rdx_menu_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* id (and value) of property `name` on a KMS object; -1 if absent */
static int prop_of(int fd, uint32_t obj, uint32_t type, const char *name, uint32_t *id, uint64_t *val)
{
    struct drm_mode_obj_get_properties op = {.obj_id = obj, .obj_type = type};
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props) return -1;
    uint32_t *ids = calloc(op.count_props, sizeof(uint32_t));
    uint64_t *vals = calloc(op.count_props, sizeof(uint64_t));
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    int found = -1;
    if (!xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
        for (uint32_t i = 0; i < op.count_props && found < 0; i++) {
            struct drm_mode_get_property gp = {.prop_id = ids[i]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp)) continue;
            if (strncmp(gp.name, name, DRM_PROP_NAME_LEN)) continue;
            if (id) *id = ids[i];
            if (val) *val = vals[i];
            found = 0;
        }
    free(ids);
    free(vals);
    return found;
}

static int plane_has_format(int fd, uint32_t plane_id, uint32_t fourcc, uint32_t *possible, uint32_t *cur_crtc)
{
    struct drm_mode_get_plane p = {.plane_id = plane_id};
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p)) return 0;
    *possible = p.possible_crtcs;
    *cur_crtc = p.crtc_id;
    if (!p.count_format_types) return 0;
    uint32_t *f = calloc(p.count_format_types, sizeof(uint32_t));
    struct drm_mode_get_plane p2 = {.plane_id = plane_id, .format_type_ptr = (uintptr_t)f,
                                    .count_format_types = p.count_format_types};
    int ok = 0;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p2))
        for (uint32_t i = 0; i < p2.count_format_types; i++) ok |= f[i] == fourcc;
    free(f);
    return ok;
}

static int find_planes(struct rdx_overlay *o)
{
    int idx = rdx_kms_crtc_index(o->drm_fd, o->crtc_id);
    if (idx < 0) return -1;
    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof(pr));
    if (xioctl(o->drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes) return -1;
    uint32_t *ids = calloc(pr.count_planes, sizeof(uint32_t));
    struct drm_mode_get_plane_res pr2 = {.plane_id_ptr = (uintptr_t)ids, .count_planes = pr.count_planes};
    if (xioctl(o->drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr2)) { free(ids); return -1; }
    for (uint32_t i = 0; i < pr2.count_planes; i++) {
        uint64_t type = 0;
        uint32_t possible = 0, cur = 0;
        if (prop_of(o->drm_fd, ids[i], DRM_MODE_OBJECT_PLANE, "type", NULL, &type)) continue;
        int argb = plane_has_format(o->drm_fd, ids[i], DRM_FORMAT_ARGB8888, &possible, &cur);
        if (!(possible & (1u << idx))) continue;
        if (type == DRM_PLANE_TYPE_PRIMARY && cur == o->crtc_id) o->primary_id = ids[i];
        if (type == DRM_PLANE_TYPE_OVERLAY && argb && !o->plane_id && (cur == 0 || cur == o->crtc_id))
            o->plane_id = ids[i];
    }
    free(ids);
    if (!o->plane_id || !o->primary_id) return -1;
    struct { const char *n; uint32_t *id; } props[] = {
        {"FB_ID", &o->p_fb}, {"CRTC_ID", &o->p_crtc}, {"SRC_X", &o->p_sx}, {"SRC_Y", &o->p_sy},
        {"SRC_W", &o->p_sw}, {"SRC_H", &o->p_sh}, {"CRTC_X", &o->p_cx}, {"CRTC_Y", &o->p_cy},
        {"CRTC_W", &o->p_cw}, {"CRTC_H", &o->p_ch}};
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
        if (prop_of(o->drm_fd, o->plane_id, DRM_MODE_OBJECT_PLANE, props[i].n, props[i].id, NULL)) return -1;
    return prop_of(o->drm_fd, o->primary_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &o->pp_fb, NULL);
}

int rdx_overlay_probe(int drm_fd, uint32_t crtc_id)
{
    struct rdx_overlay o;
    memset(&o, 0, sizeof(o));
    o.drm_fd = drm_fd;
    o.crtc_id = crtc_id;
    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
    if (xioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap)) { perror("DRM_CLIENT_CAP_ATOMIC"); return 1; }
    int r = find_planes(&o);
    printf("{\"crtc\":%u,\"overlay_plane\":%u,\"primary_plane\":%u,\"argb8888\":%s,\"usable\":%s}\n",
           crtc_id, o.plane_id, o.primary_id, o.plane_id ? "true" : "false", r ? "false" : "true");
    return r ? 1 : 0;
}

static void send_msg(struct rdx_overlay *o, uint32_t type, int a, int b, int c, int d, int e)
{
    if (o->sock < 0) return;
    struct rdx_menu_msg m = {type, a, b, c, d, e};
    if (send(o->sock, &m, sizeof(m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN) {
        close(o->sock);
        o->sock = -1;
    }
}

int rdx_overlay_init(struct rdx_overlay *o, int drm_fd, uint32_t crtc_id, const char *sock_path,
                     uint32_t mode_w, uint32_t mode_h, uint32_t refresh_mhz)
{
    memset(o, 0, sizeof(*o));
    o->drm_fd = drm_fd;
    o->crtc_id = crtc_id;
    o->shown = o->want = -1;
    o->sock = -1;
    o->waiting = 1;                       /* until the first stream frame */
    if (!sock_path || !*sock_path) return -1;
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "[overlay] no menu: %s: %s\n", sock_path, strerror(errno));
        if (s >= 0) close(s);
        return -1;
    }
    o->sock = s;
    struct drm_set_client_cap cap = {.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1};
    int found = !xioctl(drm_fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) && !find_planes(o);
    send_msg(o, RDX_MENU_HELLO, RDX_MENU_ROLE_SINK, (int)mode_w, (int)mode_h, (int)refresh_mhz, 0);
    if (!found) {                         /* the sink blends the menu into its frames (soft overlay) */
        o->soft = 1;
        printf("[overlay] no ARGB8888 overlay plane for CRTC %u: menu blended into the frames, socket %s\n", crtc_id,
               sock_path);
        return 0;
    }
    printf("[overlay] menu on overlay plane %u (primary %u, CRTC %u), socket %s\n", o->plane_id,
           o->primary_id, crtc_id, sock_path);
    return 0;
}

struct atomic_req {
    uint32_t objs[2], counts[2], props[16];
    uint64_t vals[16];
    int nobj, nprop;
};

static void add(struct atomic_req *r, uint32_t obj, uint32_t prop, uint64_t val)
{
    if (!r->nobj || r->objs[r->nobj - 1] != obj) { r->objs[r->nobj] = obj; r->counts[r->nobj++] = 0; }
    r->counts[r->nobj - 1]++;
    r->props[r->nprop] = prop;
    r->vals[r->nprop++] = val;
}

static void add_overlay(struct atomic_req *r, const struct rdx_overlay *o, int idx)
{
    uint32_t p = o->plane_id;
    if (idx < 0) {           /* hidden: no framebuffer, no CRTC */
        add(r, p, o->p_fb, 0);
        add(r, p, o->p_crtc, 0);
        return;
    }
    add(r, p, o->p_fb, o->fb[idx]);
    add(r, p, o->p_crtc, o->crtc_id);
    add(r, p, o->p_sx, 0);
    add(r, p, o->p_sy, 0);
    add(r, p, o->p_sw, (uint64_t)o->w << 16);
    add(r, p, o->p_sh, (uint64_t)o->h << 16);
    add(r, p, o->p_cx, (uint64_t)(uint32_t)o->x);
    add(r, p, o->p_cy, (uint64_t)(uint32_t)o->y);
    add(r, p, o->p_cw, o->w);
    add(r, p, o->p_ch, o->h);
}

static int commit(struct rdx_overlay *o, struct atomic_req *r, uint32_t flags, uint64_t user_data)
{
    struct drm_mode_atomic a;
    memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.count_objs = (uint32_t)r->nobj;
    a.objs_ptr = (uintptr_t)r->objs;
    a.count_props_ptr = (uintptr_t)r->counts;
    a.props_ptr = (uintptr_t)r->props;
    a.prop_values_ptr = (uintptr_t)r->vals;
    a.user_data = user_data;
    return xioctl(o->drm_fd, DRM_IOCTL_MODE_ATOMIC, &a);
}

static int import_buffers(struct rdx_overlay *o, const int *fds, int nfd, const struct rdx_menu_msg *m)
{
    int ufd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ufd < 0) return -1;
    o->w = (uint32_t)m->a; o->h = (uint32_t)m->b; o->x = m->d; o->y = m->e;
    uint32_t stride = (uint32_t)m->c;
    int ok = nfd == RDX_MENU_NBUF;
    for (int i = 0; ok && i < RDX_MENU_NBUF; i++) {
        struct stat st;
        uint32_t handle;
        ok = !fstat(fds[i], &st);
        struct udmabuf_create uc = {.memfd = (uint32_t)fds[i], .flags = UDMABUF_FLAGS_CLOEXEC,
                                    .offset = 0, .size = (uint64_t)st.st_size};
        int db = ok ? ioctl(ufd, UDMABUF_CREATE, &uc) : -1;
        ok = db >= 0 && !rdx_kms_prime_import(o->drm_fd, db, &handle) &&
             !rdx_kms_add_fb_linear(o->drm_fd, handle, o->w, o->h, stride, DRM_FORMAT_ARGB8888, &o->fb[i]);
        if (db >= 0) close(db);
    }
    close(ufd);
    if (!ok) return -1;
    /* test the placement once, so a later stream flip never fails because of the overlay */
    struct atomic_req r = {0};
    add_overlay(&r, o, 0);
    return commit(o, &r, DRM_MODE_ATOMIC_TEST_ONLY, 0);
}

int rdx_overlay_handle(struct rdx_overlay *o)
{
    while (o->sock >= 0) {
        struct rdx_menu_msg m;
        int fds[RDX_MENU_NBUF] = {-1, -1};
        char cbuf[CMSG_SPACE(sizeof(fds))];
        struct iovec iov = {.iov_base = &m, .iov_len = sizeof(m)};
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
        ssize_t n = recvmsg(o->sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        if (n <= 0) {                     /* clientd went away: take the menu down */
            close(o->sock);
            o->sock = -1;
            o->want = -1;
            return -1;
        }
        int nfd = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                nfd = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                if (nfd > RDX_MENU_NBUF) nfd = RDX_MENU_NBUF;
                memcpy(fds, CMSG_DATA(c), (size_t)nfd * sizeof(int));
            }
        if (n != (ssize_t)sizeof(m)) goto next;
        switch (m.type) {
        case RDX_MENU_BUFFERS:
            if (o->soft && !o->smap[0] && nfd == RDX_MENU_NBUF) {   /* map them, the sink blends */
                struct stat st;
                int ok = !fstat(fds[0], &st);
                o->w = (uint32_t)m.a; o->h = (uint32_t)m.b; o->x = m.d; o->y = m.e;
                o->sstride = (uint32_t)m.c;
                o->slen = ok ? (size_t)st.st_size : 0;
                for (int i = 0; ok && i < RDX_MENU_NBUF; i++) {
                    void *p = mmap(NULL, o->slen, PROT_READ, MAP_SHARED, fds[i], 0);
                    ok = p != MAP_FAILED;
                    o->smap[i] = ok ? p : NULL;
                }
                o->usable = ok;
                printf("[overlay] menu buffers %ux%u at %d,%d %s\n", o->w, o->h, o->x, o->y,
                       ok ? "mapped (blended by the sink)" : "NOT mapped");
                send_msg(o, RDX_MENU_OVERLAY, ok, ok ? 0 : errno, 0, 0, 0);
                fflush(stdout);
                break;
            }
            if (!o->plane_id || o->fb[0]) break;          /* one buffer set per session */
            if (import_buffers(o, fds, nfd, &m)) {
                int err = errno;
                fprintf(stderr, "[overlay] overlay %ux%u at %d,%d not usable: %s\n", o->w, o->h, o->x, o->y,
                        strerror(err));
                send_msg(o, RDX_MENU_OVERLAY, 0, err, 0, 0, 0);
            } else {
                o->usable = 1;
                printf("[overlay] menu buffers %ux%u at %d,%d, test commit ok\n", o->w, o->h, o->x, o->y);
                send_msg(o, RDX_MENU_OVERLAY, 1, 0, 0, 0, 0);
            }
            fflush(stdout);
            break;
        case RDX_MENU_SHOW:
            if (m.a >= 0 && m.a < RDX_MENU_NBUF) {
                int fresh = o->shown < 0 && !o->show_at;   /* nothing on the overlay yet */
                o->want = m.a;
                o->until_frame = m.b == 1 && o->waiting;   /* a frame that is already flowing ends it at once */
                if (m.b == 1 && !o->waiting) o->want = -1;
                if (m.b != 2) o->show_at = 0;
                if (m.b == 2) {
                    /* the host said the picture goes (session.stall, a notice): over the last frame,
                     * however long it has been static, until the next frame comes; after a grace
                     * period, so a switch that is over at once shows nothing at all (its redraws
                     * every second keep the first deadline) */
                    if (fresh) {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        o->show_at = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec +
                                     RDX_MENU_STALL_GRACE_MS * 1000000ull;
                    }
                    o->until_frame = 1;
                    o->waiting = 1;
                }
            }
            break;
        case RDX_MENU_HIDE: o->want = -1; o->until_frame = 0; o->show_at = 0; break;
        default: break;
        }
    next:
        for (int i = 0; i < nfd; i++) close(fds[i]);
    }
    return -1;
}

int rdx_overlay_flip(struct rdx_overlay *o, uint32_t primary_fb, uint64_t user_data)
{
    struct atomic_req r = {0};
    add(&r, o->primary_id, o->pp_fb, primary_fb);
    add_overlay(&r, o, o->want);
    if (commit(o, &r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, user_data)) {
        o->failed++;
        fprintf(stderr, "[overlay] atomic flip failed (%s): menu disabled for this session\n", strerror(errno));
        o->usable = 0;
        send_msg(o, RDX_MENU_OVERLAY, 0, errno, 0, 0, 0);
        return -1;
    }
    o->commits++;
    o->shown = o->want;
    return 0;
}

void rdx_overlay_frame(struct rdx_overlay *o)
{
    if (!o->waiting) return;
    o->waiting = 0;
    if (o->until_frame) {
        o->want = -1;                     /* off in the commit with this frame (or never shown) */
        o->until_frame = 0;
        o->show_at = 0;
    }
    send_msg(o, RDX_MENU_FRAME, 1, 0, 0, 0, 0);
}

void rdx_overlay_stalled(struct rdx_overlay *o)
{
    /* no frames for a while is no event: a loading or still screen of the app sends none for
     * seconds (gaps up to 2.6 s in Lutris), and a placeholder over it was seen as
     * flicker. Only the overlay's own pace changes (menu commits without stream flips);
     * clientd is told nothing, the placeholder comes only when the host says so (SHOW b = 2). */
    o->waiting = 1;
}

void rdx_overlay_fini(struct rdx_overlay *o)
{
    if (o->plane_id && o->shown >= 0) {
        struct atomic_req r = {0};
        add_overlay(&r, o, -1);
        if (!commit(o, &r, 0, 0)) o->shown = -1;
    }
    if (o->sock >= 0) close(o->sock);
    o->sock = -1;
}

void rdx_overlay_stats(struct rdx_overlay *o, long shown_s, long hitches_s, long dropped_s,
                       double p50_us, uint32_t refresh_mhz)
{
    send_msg(o, RDX_MENU_STATS, (int)shown_s, (int)hitches_s, (int)dropped_s, (int)(p50_us + 0.5),
             (int)refresh_mhz);
}
