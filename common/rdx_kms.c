/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "rdx_kms.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

int rdx_kms_set_master(int fd)
{
    return xioctl(fd, DRM_IOCTL_SET_MASTER, NULL);
}

int rdx_kms_monotonic_ts(int fd)
{
    struct drm_get_cap cap = {.capability = DRM_CAP_TIMESTAMP_MONOTONIC};
    return xioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value;
}

static int get_encoder_crtc(int fd, uint32_t enc_id, uint32_t *crtc_id, uint32_t *possible)
{
    struct drm_mode_get_encoder enc = {.encoder_id = enc_id};
    if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc)) return -1;
    *crtc_id = enc.crtc_id;
    *possible = enc.possible_crtcs;
    return 0;
}

int rdx_kms_list_modes(int fd)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return -1; }
    uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) { free(conns); return -1; }
    for (uint32_t i = 0; i < r2.count_connectors; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != RDX_CONN_CONNECTED || !c.count_modes)
            continue;
        struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
        struct drm_mode_get_connector c2 = {.connector_id = conns[i], .modes_ptr = (uintptr_t)modes,
                                            .count_modes = c.count_modes};
        if (!xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2)) {
            printf("connector %u (type %u-%u): %u modes\n", conns[i], c.connector_type, c.connector_type_id,
                   c2.count_modes);
            for (uint32_t m = 0; m < c2.count_modes; m++)
                printf("  %4ux%-4u @ %3u Hz  clock %6u kHz%s\n", modes[m].hdisplay, modes[m].vdisplay,
                       modes[m].vrefresh, modes[m].clock,
                       (modes[m].type & DRM_MODE_TYPE_PREFERRED) ? "  (preferred)" : "");
        }
        free(modes);
    }
    free(conns);
    return 0;
}

static const char *conn_type_name(uint32_t t);

/* refreshes worth offering above what the monitor lists (derived like rdx_kms_pick_output does) */
static const uint32_t derive_hz[] = {100, 120, 144, 165, 180, 200, 240};

int rdx_kms_list_modes_json(int fd, const char *card)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return -1; }
    uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) { free(conns); return -1; }
    int rc = -1;
    /* the first connected connector, the same one rdx_kms_pick_output drives */
    for (uint32_t i = 0; i < r2.count_connectors && rc; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != RDX_CONN_CONNECTED || !c.count_modes)
            continue;
        struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
        struct drm_mode_get_connector c2 = {.connector_id = conns[i], .modes_ptr = (uintptr_t)modes,
                                            .count_modes = c.count_modes};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2) || !c2.count_modes) { free(modes); continue; }
        uint32_t max_clock = 0;
        for (uint32_t m = 0; m < c2.count_modes; m++)
            if (modes[m].clock > max_clock) max_clock = modes[m].clock;
        printf("{\"card\":\"%s\",\"connector\":\"%s-%u\",\"modes\":[", card, conn_type_name(c.connector_type),
               c.connector_type_id);
        int first = 1;
        for (uint32_t m = 0; m < c2.count_modes; m++) {
            int dup = 0;                                   /* same size and refresh listed earlier */
            for (uint32_t p = 0; p < m && !dup; p++)
                dup = modes[p].hdisplay == modes[m].hdisplay && modes[p].vdisplay == modes[m].vdisplay &&
                      modes[p].vrefresh == modes[m].vrefresh;
            if (dup) continue;
            printf("%s{\"w\":%u,\"h\":%u,\"hz\":%u,\"derived\":false,\"preferred\":%s}", first ? "" : ",",
                   modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh,
                   (modes[m].type & DRM_MODE_TYPE_PREFERRED) ? "true" : "false");
            first = 0;
        }
        /* derived: a listed mode of the same size whose refresh divides the target, pixel clock x k
         * not above the highest clock the monitor lists anywhere (the link evidently carries it).
         * Only from real-rate bases (>= 50 Hz, k <= 4): rdx_kms_pick_output takes the smallest k,
         * so each listed entry is exactly what it would set; film/odd rates are not offered */
        for (uint32_t m = 0; m < c2.count_modes; m++) {
            uint32_t w = modes[m].hdisplay, h = modes[m].vdisplay, top = 0;
            int seen = 0;
            for (uint32_t p = 0; p < c2.count_modes; p++) {
                if (modes[p].hdisplay != w || modes[p].vdisplay != h) continue;
                if (p < m) seen = 1;
                if (modes[p].vrefresh > top) top = modes[p].vrefresh;
            }
            if (seen) continue;                            /* each size once */
            for (size_t d = 0; d < sizeof(derive_hz) / sizeof(derive_hz[0]); d++) {
                uint32_t hz = derive_hz[d];
                if (hz <= top) continue;
                int ok = 0;
                for (uint32_t k = 2; k <= 4 && !ok; k++)
                    for (uint32_t p = 0; p < c2.count_modes && !ok; p++)
                        ok = modes[p].hdisplay == w && modes[p].vdisplay == h && modes[p].vrefresh * k == hz &&
                             modes[p].vrefresh >= 50 && modes[p].clock * k <= max_clock;
                if (!ok) continue;
                printf("%s{\"w\":%u,\"h\":%u,\"hz\":%u,\"derived\":true,\"preferred\":false}", first ? "" : ",",
                       w, h, hz);
                first = 0;
            }
        }
        printf("]}\n");
        free(modes);
        rc = 0;
    }
    if (rc) printf("{\"card\":\"%s\",\"connector\":null,\"modes\":[]}\n", card);
    free(conns);
    return rc;
}

static int obj_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint32_t *prop_id, uint64_t *value);

double rdx_kms_mode_hz(const struct drm_mode_modeinfo *m)
{
    if (!m->htotal || !m->vtotal || !m->clock) return m->vrefresh;
    double hz = (double)m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
    if (m->flags & DRM_MODE_FLAG_INTERLACE) hz *= 2;
    if (m->flags & DRM_MODE_FLAG_DBLSCAN) hz /= 2;
    if (m->vscan > 1) hz /= m->vscan;
    return hz;
}

int rdx_kms_synth_mode(const struct drm_mode_modeinfo *base, double hz, const struct rdx_kms_range *range,
                       struct drm_mode_modeinfo *out, char *why, size_t whyn)
{
    if (!base->htotal || !base->vtotal || !base->clock || hz <= 0) {
        snprintf(why, whyn, "base mode %s has no usable timings", base->name);
        return -1;
    }
    /* scale only the clock: same htotal/vtotal, so refresh is proportional to it */
    double clock = (double)base->clock * hz / rdx_kms_mode_hz(base);
    uint32_t khz = (uint32_t)(clock + 0.5);
    if (range && range->max_clock_khz && khz > range->max_clock_khz) {
        snprintf(why, whyn, "needs a pixel clock of %u kHz, the monitor allows at most %u kHz", khz,
                 range->max_clock_khz);
        return -1;
    }
    if (range && ((range->vmin_hz && hz < range->vmin_hz - 0.5) || (range->vmax_hz && hz > range->vmax_hz + 0.5))) {
        snprintf(why, whyn, "%.3f Hz is outside the monitor's vertical range %u-%u Hz", hz, range->vmin_hz,
                 range->vmax_hz);
        return -1;
    }
    *out = *base;
    out->clock = khz;
    out->vrefresh = (uint32_t)(rdx_kms_mode_hz(out) + 0.5);
    out->type &= ~DRM_MODE_TYPE_PREFERRED;
    out->type |= DRM_MODE_TYPE_USERDEF;
    snprintf(out->name, sizeof(out->name), "%ux%u@%.3f", base->hdisplay, base->vdisplay, rdx_kms_mode_hz(out));
    return 0;
}

int rdx_kms_edid_range(int fd, uint32_t conn_id, struct rdx_kms_range *r)
{
    uint64_t blob_id = 0;
    memset(r, 0, sizeof(*r));
    if (obj_prop(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", NULL, &blob_id) || !blob_id) return -1;
    struct drm_mode_get_blob gb = {.blob_id = (uint32_t)blob_id};
    if (xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb) || gb.length < 128) return -1;
    uint8_t *e = calloc(1, gb.length);
    gb.data = (uintptr_t)e;
    int rc = -1;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &gb)) rc = rdx_edid_range_bytes(e, gb.length, r);
    free(e);
    return rc;
}

int rdx_edid_range_bytes(const uint8_t *e, size_t n, struct rdx_kms_range *r)
{
    static const uint8_t magic[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    memset(r, 0, sizeof(*r));
    if (!e || n < 128 || memcmp(e, magic, 8)) return -1;
    int rc = -1;
    for (int i = 54; i <= 108; i += 18) {            /* the four 18-byte descriptors of block 0 */
        const uint8_t *d = e + i;
        if (d[0] || d[1] || d[2] || d[3] != 0xFD) continue;
        /* byte 4: rate offsets (EDID 1.4): bit 0 / bit 1 = +255 Hz on the min / max vertical rate */
        r->vmin_hz = d[5] + ((d[4] & 0x03) == 0x03 ? 255 : 0);
        r->vmax_hz = d[6] + ((d[4] & 0x02) ? 255 : 0);
        r->max_clock_khz = (uint32_t)d[9] * 10000;
        rc = 0;
    }
    return rc;
}

int rdx_kms_driver_name(int fd, char *name, size_t n)
{
    struct drm_version v;
    memset(&v, 0, sizeof(v));
    v.name = name;
    v.name_len = n ? n - 1 : 0;
    if (!n || xioctl(fd, DRM_IOCTL_VERSION, &v)) return -1;
    name[v.name_len < n ? v.name_len : n - 1] = 0;
    return 0;
}

int rdx_kms_vrr_enable(int fd, uint32_t conn_id, uint32_t crtc_id, int on)
{
    uint64_t cap = 0, val = 0;
    uint32_t pid = 0;
    if (obj_prop(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "vrr_capable", NULL, &cap) || !cap) return 0;
    if (obj_prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", &pid, &val)) return -1;
    struct drm_mode_obj_set_property sp = {.value = on ? 1 : 0, .prop_id = pid, .obj_id = crtc_id,
                                           .obj_type = DRM_MODE_OBJECT_CRTC};
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp)) return -1;
    if (obj_prop(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", NULL, &val)) return -1;
    return (val != 0) == (on != 0) ? (on ? 1 : 0) : -1;
}

int rdx_kms_pick_output(int fd, struct rdx_kms_out *out, uint32_t want_w, uint32_t want_h, double want_hz)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return -1; }
    uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(uint32_t));
    uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.crtc_id_ptr = (uintptr_t)crtcs, .connector_id_ptr = (uintptr_t)conns,
                                   .count_crtcs = res.count_crtcs, .count_connectors = res.count_connectors};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) { perror("GETRESOURCES"); return -1; }

    int found = -1;
    for (uint32_t i = 0; i < r2.count_connectors && found < 0; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) continue;      /* counts, triggers probe */
        if (c.connection != RDX_CONN_CONNECTED || !c.count_modes) continue;
        struct drm_mode_modeinfo *modes = calloc(c.count_modes, sizeof(*modes));
        uint32_t *encs = calloc(c.count_encoders ? c.count_encoders : 1, sizeof(uint32_t));
        struct drm_mode_get_connector c2 = {.connector_id = conns[i],
            .modes_ptr = (uintptr_t)modes, .count_modes = c.count_modes,
            .encoders_ptr = (uintptr_t)encs, .count_encoders = c.count_encoders};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c2) || !c2.count_modes) { free(modes); free(encs); continue; }
        out->conn_id = conns[i];
        out->mode = modes[0];
        for (uint32_t m = 0; m < c2.count_modes; m++)
            if (modes[m].type & DRM_MODE_TYPE_PREFERRED) { out->mode = modes[m]; break; }
        if (want_w && want_h) {
            int have = 0;
            int whole = want_hz > 0 && want_hz == (double)(uint32_t)want_hz;
            for (uint32_t m = 0; m < c2.count_modes; m++) {
                if (modes[m].hdisplay != want_w || modes[m].vdisplay != want_h) continue;
                double mhz = rdx_kms_mode_hz(&modes[m]);
                if (want_hz > 0 ? ((whole && modes[m].vrefresh == (uint32_t)want_hz) ||
                                   (mhz > want_hz - 0.01 && mhz < want_hz + 0.01))
                                : (!have || modes[m].vrefresh > out->mode.vrefresh)) { out->mode = modes[m]; have = 1; }
            }
            /* not listed: derive from a listed refresh that divides want_hz (whole Hz only) */
            for (uint32_t k = 2; whole && !have && k <= 8; k++)
                for (uint32_t m = 0; m < c2.count_modes && !have; m++)
                    if (modes[m].hdisplay == want_w && modes[m].vdisplay == want_h &&
                        modes[m].vrefresh * k == (uint32_t)want_hz) {
                        out->mode = modes[m];
                        out->mode.clock *= k;
                        out->mode.vrefresh = (uint32_t)want_hz;
                        out->mode.type &= ~DRM_MODE_TYPE_PREFERRED;
                        snprintf(out->mode.name, sizeof(out->mode.name), "%ux%u@%u", want_w, want_h, (uint32_t)want_hz);
                        fprintf(stderr, "[kms] %ux%u@%u is not listed by the monitor: derived from @%u "
                                "(pixel clock %u kHz), the monitor may refuse it\n",
                                want_w, want_h, (uint32_t)want_hz, modes[m].vrefresh, out->mode.clock);
                        have = 1;
                    }
            /* still nothing: synthesize from the listed mode of that size with the closest refresh */
            char why[160] = "no listed mode of that size";
            if (want_hz > 0 && !have) {
                int best = -1;
                double bd = 0;
                for (uint32_t m = 0; m < c2.count_modes; m++) {
                    if (modes[m].hdisplay != want_w || modes[m].vdisplay != want_h) continue;
                    if (modes[m].flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) continue;
                    double d = rdx_kms_mode_hz(&modes[m]) - want_hz;
                    if (d < 0) d = -d;
                    if (best < 0 || d < bd) { best = (int)m; bd = d; }
                }
                struct rdx_kms_range rg;
                int has_range = !rdx_kms_edid_range(fd, conns[i], &rg);
                if (best >= 0 && !rdx_kms_synth_mode(&modes[best], want_hz, has_range ? &rg : NULL, &out->mode,
                                                     why, sizeof(why))) {
                    fprintf(stderr, "[kms] %ux%u@%.3f is not listed by the monitor: SYNTHESIZED from %s "
                            "(%.3f Hz, %u kHz) -> pixel clock %u kHz = %.4f Hz, same porches/sync; EDID range %s "
                            "(v %u-%u Hz, max %u kHz); the monitor may refuse it\n",
                            want_w, want_h, want_hz, modes[best].name, rdx_kms_mode_hz(&modes[best]),
                            modes[best].clock, out->mode.clock, rdx_kms_mode_hz(&out->mode),
                            has_range ? "checked" : "unknown", rg.vmin_hz, rg.vmax_hz, rg.max_clock_khz);
                    have = 1;
                }
            }
            if (!have) {
                if (want_hz > 0)
                    fprintf(stderr, "monitor offers no %ux%u mode at %.3f Hz: %s\n", want_w, want_h, want_hz, why);
                else
                    fprintf(stderr, "monitor offers no %ux%u mode\n", want_w, want_h);
                free(modes); free(encs); free(crtcs); free(conns);
                return -1;
            }
        }
        /* CRTC: the current encoder's, otherwise the first possible one */
        uint32_t crtc = 0, possible = 0;
        if (c2.encoder_id && !get_encoder_crtc(fd, c2.encoder_id, &crtc, &possible) && crtc) {
            out->crtc_id = crtc;
        } else {
            for (uint32_t e = 0; e < c2.count_encoders && !out->crtc_id; e++) {
                if (get_encoder_crtc(fd, encs[e], &crtc, &possible)) continue;
                for (uint32_t k = 0; k < r2.count_crtcs; k++)
                    if (possible & (1u << k)) { out->crtc_id = crtcs[k]; break; }
            }
        }
        free(modes); free(encs);
        if (out->crtc_id) found = 0;
    }
    free(crtcs); free(conns);
    if (found) fprintf(stderr, "no connected monitor with a usable CRTC\n");
    return found;
}

int rdx_kms_prime_import(int fd, int dmabuf_fd, uint32_t *handle)
{
    struct drm_prime_handle p = {.fd = dmabuf_fd};
    if (xioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p)) return -1;
    *handle = p.handle;
    return 0;
}

int rdx_kms_add_fb_linear(int fd, uint32_t handle, uint32_t w, uint32_t h, uint32_t stride,
                          uint32_t fourcc, uint32_t *fb_id)
{
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof(f));
    f.width = w; f.height = h; f.pixel_format = fourcc;
    f.handles[0] = handle; f.pitches[0] = stride; f.offsets[0] = 0;
    f.flags = DRM_MODE_FB_MODIFIERS;
    f.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    if (xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f)) {
        f.flags = 0; f.modifier[0] = 0;            /* driver without modifier support */
        if (xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f)) return -1;
    }
    *fb_id = f.fb_id;
    return 0;
}

void *rdx_kms_dumb_create(int fd, uint32_t w, uint32_t h, uint32_t *handle, uint32_t *pitch, uint64_t *size)
{
    struct drm_mode_create_dumb c = {.width = w, .height = h, .bpp = 32};
    if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return NULL;
    struct drm_mode_map_dumb m = {.handle = c.handle};
    if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return NULL;
    void *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
    if (p == MAP_FAILED) return NULL;
    *handle = c.handle;
    *pitch = c.pitch;
    *size = c.size;
    return p;
}

int rdx_kms_set_crtc(int fd, const struct rdx_kms_out *out, uint32_t fb_id)
{
    uint32_t conn = out->conn_id;
    struct drm_mode_crtc c;
    memset(&c, 0, sizeof(c));
    c.crtc_id = out->crtc_id;
    c.fb_id = fb_id;
    c.set_connectors_ptr = (uintptr_t)&conn;
    c.count_connectors = 1;
    c.mode = out->mode;
    c.mode_valid = 1;
    return xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &c);
}

int rdx_kms_page_flip(int fd, uint32_t crtc_id, uint32_t fb_id, int async, uint64_t user_data)
{
    struct drm_mode_crtc_page_flip f;
    memset(&f, 0, sizeof(f));
    f.crtc_id = crtc_id;
    f.fb_id = fb_id;
    f.flags = DRM_MODE_PAGE_FLIP_EVENT | (async ? DRM_MODE_PAGE_FLIP_ASYNC : 0);
    f.user_data = user_data;
    return ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &f);
}

int rdx_kms_read_flips(int fd, uint64_t *tv_ns)
{
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    int flips = 0;
    for (ssize_t off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
        struct drm_event *e = (struct drm_event *)(buf + off);
        if (e->type == DRM_EVENT_FLIP_COMPLETE && e->length >= sizeof(struct drm_event_vblank)) {
            struct drm_event_vblank *v = (struct drm_event_vblank *)e;
            *tv_ns = (uint64_t)v->tv_sec * 1000000000ull + (uint64_t)v->tv_usec * 1000ull;
            flips++;
        }
        if (!e->length) break;
        off += e->length;
    }
    return flips;
}

/* ---------------- capture side ---------------- */

int rdx_kms_universal_planes(int fd)
{
    struct drm_set_client_cap c = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    return xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c);
}

static const char *conn_type_name(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
    case DRM_MODE_CONNECTOR_eDP:         return "eDP";
    case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
    case DRM_MODE_CONNECTOR_VGA:         return "VGA";
    case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
    default:                             return "Unknown";
    }
}

uint32_t rdx_kms_plane_fb(int fd, uint32_t plane_id)
{
    struct drm_mode_get_plane p = {.plane_id = plane_id};
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p)) return 0;
    return p.fb_id;
}

int rdx_kms_getfb2(int fd, uint32_t fb_id, struct drm_mode_fb_cmd2 *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->fb_id = fb_id;
    if (xioctl(fd, DRM_IOCTL_MODE_GETFB2, fb)) return -1;
    if (!fb->handles[0]) { errno = EPERM; return -1; }   /* kernel hides handles from non-root */
    return 0;
}

/* value of property `name` on object `obj` (also returns its id); -1 if absent */
static int obj_prop(int fd, uint32_t obj, uint32_t type, const char *name, uint32_t *prop_id, uint64_t *value)
{
    struct drm_mode_obj_get_properties op = {.obj_id = obj, .obj_type = type};
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props) return -1;
    uint32_t *ids = calloc(op.count_props, sizeof(uint32_t));
    uint64_t *vals = calloc(op.count_props, sizeof(uint64_t));
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    int found = -1;
    if (!xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op)) {
        for (uint32_t i = 0; i < op.count_props && found < 0; i++) {
            struct drm_mode_get_property gp = {.prop_id = ids[i]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp)) continue;
            if (!strncmp(gp.name, name, DRM_PROP_NAME_LEN)) {
                if (prop_id) *prop_id = ids[i];
                if (value) *value = vals[i];
                found = 0;
            }
        }
    }
    free(ids);
    free(vals);
    return found;
}

int rdx_kms_find_cursor_plane(int fd, uint32_t crtc_id, struct rdx_kms_cursor *cur)
{
    memset(cur, 0, sizeof(*cur));
    struct drm_set_client_cap c = {.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1};
    xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c);
    c.capability = DRM_CLIENT_CAP_ATOMIC;                 /* exposes CRTC_X/Y/W/H on planes */
    if (xioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c)) return -1;

    /* index of our CRTC in the resource list (possible_crtcs is a bitmask over it) */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs) return -1;
    uint32_t *crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs};
    int idx = -1;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2))
        for (uint32_t i = 0; i < r2.count_crtcs; i++) if (crtcs[i] == crtc_id) idx = (int)i;
    free(crtcs);
    if (idx < 0) return -1;

    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof(pr));
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes) return -1;
    uint32_t *ids = calloc(pr.count_planes, sizeof(uint32_t));
    pr.plane_id_ptr = (uintptr_t)ids;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) {
        for (uint32_t i = 0; i < pr.count_planes && !cur->plane_id; i++) {
            struct drm_mode_get_plane p = {.plane_id = ids[i]};
            uint64_t type = 0;
            if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) || !(p.possible_crtcs & (1u << idx))) continue;
            if (obj_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "type", NULL, &type) || type != DRM_PLANE_TYPE_CURSOR)
                continue;
            if (obj_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "CRTC_X", &cur->prop_crtc_x, NULL) ||
                obj_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "CRTC_Y", &cur->prop_crtc_y, NULL) ||
                obj_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "CRTC_W", &cur->prop_crtc_w, NULL) ||
                obj_prop(fd, ids[i], DRM_MODE_OBJECT_PLANE, "CRTC_H", &cur->prop_crtc_h, NULL))
                continue;
            cur->plane_id = ids[i];
        }
    }
    free(ids);
    return cur->plane_id ? 0 : -1;
}

int rdx_kms_cursor_state(int fd, const struct rdx_kms_cursor *cur, uint32_t crtc_id, uint32_t *fb_id,
                         int32_t *x, int32_t *y, uint32_t *w, uint32_t *h)
{
    *fb_id = 0;
    if (!cur->plane_id) return -1;
    struct drm_mode_get_plane p = {.plane_id = cur->plane_id};
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p)) return -1;
    if (!p.fb_id || p.crtc_id != crtc_id) return 0;        /* hidden */
    struct drm_mode_obj_get_properties op = {.obj_id = cur->plane_id, .obj_type = DRM_MODE_OBJECT_PLANE};
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || !op.count_props) return -1;
    uint32_t ids[64];
    uint64_t vals[64];
    if (op.count_props > 64) op.count_props = 64;
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    if (xioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op)) return -1;
    for (uint32_t i = 0; i < op.count_props; i++) {
        if (ids[i] == cur->prop_crtc_x) *x = (int32_t)(int64_t)vals[i];   /* signed range property */
        else if (ids[i] == cur->prop_crtc_y) *y = (int32_t)(int64_t)vals[i];
        else if (ids[i] == cur->prop_crtc_w) *w = (uint32_t)vals[i];
        else if (ids[i] == cur->prop_crtc_h) *h = (uint32_t)vals[i];
    }
    *fb_id = p.fb_id;
    return 0;
}

int rdx_kms_handle_to_fd(int fd, uint32_t handle, int *dmabuf_fd)
{
    struct drm_prime_handle p = {.handle = handle, .flags = DRM_CLOEXEC | DRM_RDWR};
    if (xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p)) return -1;
    *dmabuf_fd = p.fd;
    return 0;
}

void rdx_kms_gem_close(int fd, uint32_t handle)
{
    struct drm_gem_close c = {.handle = handle};
    xioctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

static void close_fb_handles(int fd, const struct drm_mode_fb_cmd2 *fb)
{
    for (int k = 0; k < 4; k++) {
        if (!fb->handles[k]) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) if (fb->handles[j] == fb->handles[k]) dup = 1;
        if (!dup) rdx_kms_gem_close(fd, fb->handles[k]);
    }
}

/* the primary plane of a CRTC: the plane on it whose framebuffer covers the mode
 * (cursor planes carry small framebuffers) */
static uint32_t primary_plane_of(int fd, uint32_t crtc_id, uint32_t mode_w)
{
    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof(pr));
    if (xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) || !pr.count_planes) return 0;
    uint32_t *ids = calloc(pr.count_planes, sizeof(uint32_t));
    pr.plane_id_ptr = (uintptr_t)ids;
    uint32_t found = 0;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) {
        for (uint32_t i = 0; i < pr.count_planes && !found; i++) {
            struct drm_mode_get_plane p = {.plane_id = ids[i]};
            if (xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) || p.crtc_id != crtc_id || !p.fb_id) continue;
            struct drm_mode_fb_cmd2 fb;
            if (rdx_kms_getfb2(fd, p.fb_id, &fb)) continue;
            close_fb_handles(fd, &fb);
            if (fb.width >= mode_w) found = ids[i];
        }
    }
    free(ids);
    return found;
}

int rdx_kms_list_cap_outputs(int fd, struct rdx_kms_cap_out *out, int max)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("GETRESOURCES"); return -1; }
    uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.connector_id_ptr = (uintptr_t)conns, .count_connectors = res.count_connectors};
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2)) { free(conns); return -1; }
    int n = 0;
    for (uint32_t i = 0; i < r2.count_connectors && n < max; i++) {
        struct drm_mode_get_connector c = {.connector_id = conns[i]};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connection != RDX_CONN_CONNECTED || !c.encoder_id) continue;
        struct drm_mode_get_encoder enc = {.encoder_id = c.encoder_id};
        if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) || !enc.crtc_id) continue;
        struct drm_mode_crtc crtc = {.crtc_id = enc.crtc_id};
        if (xioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) || !crtc.mode_valid) continue;
        struct rdx_kms_cap_out *o = &out[n];
        memset(o, 0, sizeof(*o));
        o->conn_id = conns[i];
        o->crtc_id = enc.crtc_id;
        snprintf(o->name, sizeof(o->name), "%s-%u", conn_type_name(c.connector_type), c.connector_type_id);
        o->x = crtc.x; o->y = crtc.y;
        o->w = crtc.mode.hdisplay; o->h = crtc.mode.vdisplay; o->vrefresh = crtc.mode.vrefresh;
        o->hz_exact = rdx_kms_mode_hz(&crtc.mode);
        o->plane_id = primary_plane_of(fd, enc.crtc_id, o->w);
        n++;
    }
    free(conns);
    return n;
}

int rdx_kms_crtc_index(int fd, uint32_t crtc_id)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs) return -1;
    uint32_t *crtcs = calloc(res.count_crtcs, sizeof(uint32_t));
    struct drm_mode_card_res r2 = {.crtc_id_ptr = (uintptr_t)crtcs, .count_crtcs = res.count_crtcs};
    int idx = -1;
    if (!xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r2))
        for (uint32_t i = 0; i < r2.count_crtcs && i < res.count_crtcs; i++)
            if (crtcs[i] == crtc_id) idx = (int)i;
    free(crtcs);
    return idx;
}

/* WAIT_VBLANK needs no DRM master. relative 0 returns at once with the last vblank,
 * relative 1 sleeps until the next one. Timestamps are CLOCK_MONOTONIC. */
static int wait_vblank(int fd, int crtc_index, uint32_t relative, uint64_t *t_ns)
{
    union drm_wait_vblank vb;
    memset(&vb, 0, sizeof(vb));
    vb.request.type = (enum drm_vblank_seq_type)(_DRM_VBLANK_RELATIVE |
                      (((uint32_t)crtc_index << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK));
    vb.request.sequence = relative;
    if (xioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vb)) return -1;
    *t_ns = (uint64_t)vb.reply.tval_sec * 1000000000ull + (uint64_t)vb.reply.tval_usec * 1000ull;
    return 0;
}

int rdx_kms_last_vblank(int fd, int crtc_index, uint64_t *t_ns) { return wait_vblank(fd, crtc_index, 0, t_ns); }
int rdx_kms_next_vblank(int fd, int crtc_index, uint64_t *t_ns) { return wait_vblank(fd, crtc_index, 1, t_ns); }

int rdx_kms_dirty_fb(int fd, uint32_t fb_id)
{
    struct drm_mode_fb_dirty_cmd d;
    memset(&d, 0, sizeof(d));
    d.fb_id = fb_id;                       /* no clips: the whole framebuffer */
    if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &d) == 0) return 0;
    return errno == ENOSYS ? 0 : -1;       /* drivers without dirty tracking need nothing */
}
