/* SPDX-License-Identifier: MIT
 * rdma-display: variable refresh of a session (see rdx_vrr.h) */
#include "rdx_vrr.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static double num(const cJSON *o, const char *k)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(x) && x->valuedouble > 0 && x->valuedouble < 100000 ? x->valuedouble : 0;
}

void rdx_vrr_decide(const cJSON *display, int host_allows, enum rdx_vrr_kind kind, int tearing, double cap_frac,
                    struct rdx_vrr *v)
{
    memset(v, 0, sizeof(*v));
    v->reported = cJSON_IsObject(display);
    if (v->reported) {
        v->client_vrr = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "vrr"));
        v->min_hz = num(display, "min_hz");
        v->max_hz = num(display, "max_hz");
        v->hz = num(display, "hz");
    }
    double cap = v->max_hz > 0 ? v->max_hz : v->hz;
    if (v->hz > 0 && cap > v->hz) cap = v->hz;
    const char *off = NULL;
    if (kind == RDX_VRR_LOCAL_WINDOW) off = "a window on this computer: its desktop sets the refresh";
    else if (!v->reported) off = "the client did not report its screen (older client)";
    else if (!v->client_vrr) off = "the client's screen has no variable refresh, or it is off there";
    else if (!host_allows) off = "variable refresh is off on this host (\"vrr\" in the defaults)";
    else if (kind == RDX_VRR_MIRROR) off = "a whole screen: frames follow this computer's monitor";
    else if (kind == RDX_VRR_JOIN) off = "joined the app on the seat's own monitor, which has no variable refresh "
                                         "(vrr_capable 0): its refresh paces the app";
    else if (tearing) off = "tearing mode: the client shows frames on arrival and never flips";
    else if (cap < 1) off = "the client reported no refresh range";
    else if (v->min_hz > 0 && v->min_hz >= cap) off = "the client's variable refresh range is empty";
    if (off) {
        snprintf(v->why, sizeof(v->why), "fixed refresh: %s", off);
        return;
    }
    if (!(cap_frac >= 0.5 && cap_frac <= 1)) cap_frac = RDX_VRR_CAP_FRAC_DEFAULT;
    double c = cap * cap_frac;
    if (v->min_hz > 0 && c < v->min_hz) c = v->min_hz;
    cap = (double)(long)(c * 1000.0 + 0.5) / 1000.0;     /* mHz, what rdx_gs_src gets (--vrr %.3f) */
    v->active = 1;
    v->cap_hz = cap;
    const char *who = kind == RDX_VRR_JOIN_VRR ? "the app sets the pace on the seat's monitor and the client"
                                               : "the app sets the pace";
    if (v->min_hz > 0)
        snprintf(v->why, sizeof(v->why), "variable refresh: %s, up to %.3f Hz; below %.3f Hz the client repeats frames",
                 who, cap, v->min_hz);
    else
        snprintf(v->why, sizeof(v->why), "variable refresh: %s, up to %.3f Hz", who, cap);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static const char *conn_type(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
    case DRM_MODE_CONNECTOR_eDP: return "eDP";
    case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
    default: return "Unknown";
    }
}

/* the value of a connector property by name; -1 if it has none */
static int conn_prop(int fd, uint32_t id, const char *name, uint64_t *value)
{
    struct drm_mode_obj_get_properties op = {.obj_id = id, .obj_type = DRM_MODE_OBJECT_CONNECTOR};
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props) return -1;
    uint32_t *ids = calloc(op.count_props, sizeof(uint32_t));
    uint64_t *vals = calloc(op.count_props, sizeof(uint64_t));
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    int rc = -1;
    if (ids && vals && !xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
        for (uint32_t i = 0; i < op.count_props && rc < 0; i++) {
            struct drm_mode_get_property gp = {.prop_id = ids[i]};
            if (!xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) && !strncmp(gp.name, name, DRM_PROP_NAME_LEN)) {
                *value = vals[i];
                rc = 0;
            }
        }
    free(ids);
    free(vals);
    return rc;
}

int rdx_vrr_monitor_capable(const char *card, const char *conn)
{
    int fd = card && *card && conn && *conn ? open(card, O_RDWR | O_CLOEXEC) : -1;
    if (fd < 0) return -1;
    int rc = -1;
    struct drm_mode_card_res res = {0};
    uint32_t *conns = NULL;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) && res.count_connectors &&
        (conns = calloc(res.count_connectors, sizeof(uint32_t)))) {
        struct drm_mode_card_res r2 = {.connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors};
        if (!xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2))
            for (uint32_t i = 0; i < r2.count_connectors && rc < 0; i++) {
                struct drm_mode_get_connector c = {.connector_id = conns[i]};
                if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;
                char name[48];
                snprintf(name, sizeof(name), "%s-%u", conn_type(c.connector_type), c.connector_type_id);
                uint64_t cap = 0;
                if (!strcmp(name, conn)) rc = conn_prop(fd, conns[i], "vrr_capable", &cap) ? 0 : cap != 0;
            }
    }
    free(conns);
    close(fd);
    return rc;
}

cJSON *rdx_vrr_json(const struct rdx_vrr *v)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "active", v->active);
    cJSON_AddNumberToObject(o, "cap_hz", v->active ? v->cap_hz : 0);
    cJSON_AddStringToObject(o, "why", v->why);
    if (v->reported) {
        cJSON *c = cJSON_AddObjectToObject(o, "client");
        cJSON_AddBoolToObject(c, "vrr", v->client_vrr);
        cJSON_AddNumberToObject(c, "min_hz", v->min_hz);
        cJSON_AddNumberToObject(c, "max_hz", v->max_hz);
        cJSON_AddNumberToObject(c, "hz", v->hz);
    } else cJSON_AddNullToObject(o, "client");
    return o;
}
