/* SPDX-License-Identifier: MIT
 *
 * Microphone direction of P5 (client B -> host A), the PipeWire side of both ends.
 *
 * rdx_pw_graph: a registry mirror of one PipeWire daemon (nodes, ports, links and the
 * "default" metadata), kept in the thread loop of the caller. Used by both ends.
 *
 * Host, rdx_audio_rx --mic NAME (struct rdx_vmic):
 *   - a virtual source NAME (support.null-audio-sink with media.class Audio/Source/Virtual,
 *     node.name rdx-mic-<port>) that apps record from; it is its own timer driver when no
 *     device drives the graph
 *   - the player's playback stream (no autoconnect) is linked into its input ports here, by
 *     channel: WirePlumber 0.4 refuses an Audio/Source/Virtual as a stream target
 *     (measured: "target not found")
 *   - "in use" = a link out of the virtual source into any node but a peak meter
 *     (stream.monitor = true: pavucontrol and pipewire-pulse level bars attach to every new
 *     source at once, measured); on_use(n) fires on every change
 *   - while it lives it is the default source (default.configured.audio.source); at the end
 *     the previous value comes back unless the user picked another source meanwhile. The
 *     previous value is also kept in <runtime dir>/rdx-mic-<port>.prev, so the next start
 *     restores it after a crash.
 * Client, rdx_audio_tx --mic SRC: the known sources (Audio/Source*, never a sink monitor), so a
 * named source that is not there is refused instead of falling back to another microphone.
 */
#pragma once
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#define RDX_PW_MAXNODES 512
#define RDX_PW_MAXPORTS 2048
#define RDX_PW_MAXLINKS 2048

struct rdx_pw_graph;
struct rdx_pw_node {
    struct rdx_pw_graph *g;
    uint32_t id;
    char name[128], desc[128], cls[48];
    int monitor;                        /* stream.monitor: -1 not known yet, 0, 1 */
    struct pw_proxy *proxy;             /* bound for Stream/Input nodes, for their props */
    struct spa_hook hook;
};
struct rdx_pw_port { uint32_t id, node; int out; char ch[16]; };
struct rdx_pw_link { uint32_t id, out_node, in_node; };

struct rdx_pw_graph {
    struct pw_thread_loop *loop;
    struct pw_core *core;
    struct pw_registry *reg;
    struct spa_hook reg_hook, core_hook, md_hook;
    struct rdx_pw_node *nodes[RDX_PW_MAXNODES];
    int nnodes;
    struct rdx_pw_port ports[RDX_PW_MAXPORTS];
    int nports;
    struct rdx_pw_link links[RDX_PW_MAXLINKS];
    int nlinks;
    struct pw_metadata *md;             /* "default" */
    char def_source[256];               /* default.audio.source: node name, "" unknown */
    char cfg_source[512];               /* default.configured.audio.source: raw JSON, "" none */
    int sync_seq, synced;
    void (*changed)(void *user);        /* any node/port/link change, loop thread */
    void *user;
};

/* {"name":"X"} -> X ("" if it is not that shape) */
static inline void rdx_json_name(const char *v, char *out, size_t n)
{
    out[0] = 0;
    const char *s = v ? strstr(v, "\"name\"") : NULL;
    if (!s || !(s = strchr(s + 6, '"'))) return;
    const char *e = strchr(++s, '"');
    if (e) snprintf(out, n, "%.*s", (int)(e - s), s);
}

static inline const char *rdx_dict_get(const struct spa_dict *d, const char *k)
{
    return d ? spa_dict_lookup(d, k) : NULL;
}

static void rdx_pw_node_info(void *data, const struct pw_node_info *info);
static const struct pw_node_events rdx_pw_node_events = {PW_VERSION_NODE_EVENTS, .info = rdx_pw_node_info};

static int rdx_pw_md_prop(void *data, uint32_t subject, const char *key, const char *type, const char *value)
{
    struct rdx_pw_graph *g = data;
    (void)type;
    if (subject != PW_ID_CORE || !key) return 0;
    if (!strcmp(key, "default.audio.source")) rdx_json_name(value, g->def_source, sizeof(g->def_source));
    else if (!strcmp(key, "default.configured.audio.source")) snprintf(g->cfg_source, sizeof(g->cfg_source), "%s", value ? value : "");
    return 0;
}
static const struct pw_metadata_events rdx_pw_md_events = {PW_VERSION_METADATA_EVENTS, .property = rdx_pw_md_prop};

static void rdx_pw_node_info(void *data, const struct pw_node_info *info)
{
    struct rdx_pw_node *nd = data;
    if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS)) return;
    const char *m = rdx_dict_get(info->props, "stream.monitor");
    int mon = m && (!strcmp(m, "true") || !strcmp(m, "1"));
    if (mon == nd->monitor) return;
    nd->monitor = mon;
    if (nd->g->changed) nd->g->changed(nd->g->user);
}

static struct rdx_pw_node *rdx_pw_node_by_id(struct rdx_pw_graph *g, uint32_t id)
{
    for (int i = 0; i < g->nnodes; i++) if (g->nodes[i]->id == id) return g->nodes[i];
    return NULL;
}

/* a node by node.name whose media.class starts with cls ("" = any) */
static inline struct rdx_pw_node *rdx_pw_node_by_name(struct rdx_pw_graph *g, const char *name, const char *cls)
{
    for (int i = 0; i < g->nnodes; i++)
        if (!strcmp(g->nodes[i]->name, name) && !strncmp(g->nodes[i]->cls, cls, strlen(cls))) return g->nodes[i];
    return NULL;
}

/* the port of node in direction out with channel ch (NULL = the only one) */
static inline const struct rdx_pw_port *rdx_pw_port_of(struct rdx_pw_graph *g, uint32_t node, int out, const char *ch, int *count)
{
    const struct rdx_pw_port *hit = NULL;
    int c = 0;
    for (int i = 0; i < g->nports; i++) {
        const struct rdx_pw_port *p = &g->ports[i];
        if (p->node != node || p->out != out) continue;
        c++;
        if (!ch || !strcmp(p->ch, ch)) hit = hit ? hit : p;
    }
    if (count) *count = c;
    return hit;
}

static void rdx_pw_global(void *data, uint32_t id, uint32_t perm, const char *type, uint32_t version,
                          const struct spa_dict *props)
{
    struct rdx_pw_graph *g = data;
    (void)perm; (void)version;
    if (!strcmp(type, PW_TYPE_INTERFACE_Node)) {
        if (g->nnodes == RDX_PW_MAXNODES) return;
        struct rdx_pw_node *nd = calloc(1, sizeof(*nd));
        if (!nd) return;
        nd->g = g;
        nd->id = id;
        const char *v;
        snprintf(nd->name, sizeof(nd->name), "%s", (v = rdx_dict_get(props, PW_KEY_NODE_NAME)) ? v : "");
        snprintf(nd->desc, sizeof(nd->desc), "%s", (v = rdx_dict_get(props, PW_KEY_NODE_DESCRIPTION)) ? v : nd->name);
        snprintf(nd->cls, sizeof(nd->cls), "%s", (v = rdx_dict_get(props, PW_KEY_MEDIA_CLASS)) ? v : "");
        nd->monitor = 0;
        if (!strncmp(nd->cls, "Stream/Input", 12)) {
            /* stream.monitor is not among the registry props: ask the node */
            nd->monitor = -1;
            nd->proxy = pw_registry_bind(g->reg, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
            if (nd->proxy) pw_node_add_listener((struct pw_node *)nd->proxy, &nd->hook, &rdx_pw_node_events, nd);
            else nd->monitor = 0;
        }
        g->nodes[g->nnodes++] = nd;
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Port)) {
        if (g->nports == RDX_PW_MAXPORTS) return;
        const char *n = rdx_dict_get(props, PW_KEY_NODE_ID), *d = rdx_dict_get(props, PW_KEY_PORT_DIRECTION);
        const char *ch = rdx_dict_get(props, PW_KEY_AUDIO_CHANNEL);
        if (!n || !d) return;
        struct rdx_pw_port *p = &g->ports[g->nports++];
        p->id = id;
        p->node = (uint32_t)strtoul(n, NULL, 10);
        p->out = !strcmp(d, "out");
        snprintf(p->ch, sizeof(p->ch), "%s", ch ? ch : "");
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Link)) {
        if (g->nlinks == RDX_PW_MAXLINKS) return;
        const char *o = rdx_dict_get(props, PW_KEY_LINK_OUTPUT_NODE), *i = rdx_dict_get(props, PW_KEY_LINK_INPUT_NODE);
        if (!o || !i) return;
        struct rdx_pw_link *l = &g->links[g->nlinks++];
        l->id = id;
        l->out_node = (uint32_t)strtoul(o, NULL, 10);
        l->in_node = (uint32_t)strtoul(i, NULL, 10);
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata)) {
        const char *n = rdx_dict_get(props, PW_KEY_METADATA_NAME);
        if (g->md || !n || strcmp(n, "default")) return;
        g->md = pw_registry_bind(g->reg, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
        if (g->md) pw_metadata_add_listener(g->md, &g->md_hook, &rdx_pw_md_events, g);
        return;
    } else return;
    if (g->changed) g->changed(g->user);
}

static void rdx_pw_global_remove(void *data, uint32_t id)
{
    struct rdx_pw_graph *g = data;
    int hit = 0;
    for (int i = 0; i < g->nnodes; i++)
        if (g->nodes[i]->id == id) {
            if (g->nodes[i]->proxy) { spa_hook_remove(&g->nodes[i]->hook); pw_proxy_destroy(g->nodes[i]->proxy); }
            free(g->nodes[i]);
            g->nodes[i] = g->nodes[--g->nnodes];
            hit = 1;
            break;
        }
    for (int i = 0; !hit && i < g->nports; i++)
        if (g->ports[i].id == id) { g->ports[i] = g->ports[--g->nports]; hit = 1; }
    for (int i = 0; !hit && i < g->nlinks; i++)
        if (g->links[i].id == id) { g->links[i] = g->links[--g->nlinks]; hit = 1; }
    if (hit && g->changed) g->changed(g->user);
}

static const struct pw_registry_events rdx_pw_reg_events = {
    PW_VERSION_REGISTRY_EVENTS, .global = rdx_pw_global, .global_remove = rdx_pw_global_remove};

static void rdx_pw_core_done(void *data, uint32_t id, int seq)
{
    struct rdx_pw_graph *g = data;
    if (id == PW_ID_CORE && seq == g->sync_seq) { g->synced = 1; pw_thread_loop_signal(g->loop, false); }
}
static const struct pw_core_events rdx_pw_core_events = {PW_VERSION_CORE_EVENTS, .done = rdx_pw_core_done};

/* round trip to the daemon: every event sent before this call has been handled. Loop locked;
 * from the loop thread itself it must not be called (it would wait for itself). */
static inline void rdx_pw_graph_sync(struct rdx_pw_graph *g)
{
    g->synced = 0;
    g->sync_seq = pw_core_sync(g->core, PW_ID_CORE, g->sync_seq);
    for (int i = 0; !g->synced && i < 50; i++) pw_thread_loop_timed_wait(g->loop, 1);
}

/* loop locked; returns after the first full registry round (two syncs: node infos and the
 * metadata's properties come in answer to the binds of the first one) */
static inline int rdx_pw_graph_start(struct rdx_pw_graph *g, struct pw_thread_loop *loop, struct pw_core *core)
{
    g->loop = loop;
    g->core = core;
    pw_core_add_listener(core, &g->core_hook, &rdx_pw_core_events, g);
    g->reg = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!g->reg) return -1;
    pw_registry_add_listener(g->reg, &g->reg_hook, &rdx_pw_reg_events, g);
    rdx_pw_graph_sync(g);
    rdx_pw_graph_sync(g);
    return 0;
}

static inline void rdx_pw_graph_stop(struct rdx_pw_graph *g)
{
    g->changed = NULL;
    for (int i = 0; i < g->nnodes; i++) {
        if (g->nodes[i]->proxy) { spa_hook_remove(&g->nodes[i]->hook); pw_proxy_destroy(g->nodes[i]->proxy); }
        free(g->nodes[i]);
    }
    g->nnodes = g->nports = g->nlinks = 0;
    if (g->md) { spa_hook_remove(&g->md_hook); pw_proxy_destroy((struct pw_proxy *)g->md); g->md = NULL; }
    if (g->reg) { spa_hook_remove(&g->reg_hook); pw_proxy_destroy((struct pw_proxy *)g->reg); g->reg = NULL; }
}

/* the sources a user may pick (Audio/Source and Audio/Source/Virtual) as a JSON array on f:
 * [{"name","description","default"}] */
static inline void rdx_pw_print_sources(struct rdx_pw_graph *g, FILE *f, const char *skip_prefix)
{
    fprintf(f, "[");
    int first = 1;
    for (int i = 0; i < g->nnodes; i++) {
        const struct rdx_pw_node *nd = g->nodes[i];
        if (strncmp(nd->cls, "Audio/Source", 12) || (skip_prefix && !strncmp(nd->name, skip_prefix, strlen(skip_prefix))))
            continue;
        fprintf(f, "%s{\"name\":\"", first ? "" : ",");
        for (const char *c = nd->name; *c; c++) fprintf(f, *c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        fprintf(f, "\",\"description\":\"");
        for (const char *c = nd->desc; *c; c++)
            if ((unsigned char)*c >= 0x20) fprintf(f, *c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        fprintf(f, "\",\"default\":%s}", !strcmp(nd->name, g->def_source) ? "true" : "false");
        first = 0;
    }
    fprintf(f, "]\n");
}

/* ---------------- host: the virtual microphone ---------------- */

struct rdx_vmic {
    struct rdx_pw_graph *g;
    char node_name[64], desc[128], state_path[512];
    struct pw_proxy *node;
    struct spa_hook node_hook;
    uint32_t node_id;
    int bound;
    uint32_t stream_node;               /* the player's playback stream, SPA_ID_INVALID until known */
    struct pw_proxy *link[2];
    int nlink, link_err;
    int consumers;                      /* last reported */
    char prev[512];                     /* default.configured.audio.source before us, "" none */
    int def_set;
    void (*on_use)(int consumers, void *user);   /* loop thread */
    void *user;
};

static void rdx_vmic_bound(void *data, uint32_t global_id)
{
    struct rdx_vmic *v = data;
    v->node_id = global_id;
    v->bound = 1;
    pw_thread_loop_signal(v->g->loop, false);
}
static const struct pw_proxy_events rdx_vmic_node_events = {PW_VERSION_PROXY_EVENTS, .bound = rdx_vmic_bound};

/* nodes recording from the virtual source (one per app stream, however many channel links) */
static inline int rdx_vmic_count(struct rdx_vmic *v)
{
    uint32_t seen[64];
    int n = 0;
    for (int i = 0; i < v->g->nlinks; i++) {
        const struct rdx_pw_link *l = &v->g->links[i];
        if (l->out_node != v->node_id || l->in_node == v->stream_node) continue;
        const struct rdx_pw_node *nd = rdx_pw_node_by_id(v->g, l->in_node);
        if (nd && nd->monitor) continue;            /* peak meter, or its props not known yet */
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= seen[k] == l->in_node;
        if (!dup && n < 64) seen[n++] = l->in_node;
    }
    return n;
}

static inline struct pw_proxy *rdx_vmic_link(struct rdx_vmic *v, uint32_t op, uint32_t ip)
{
    char on[16], oport[16], in[16], iport[16];
    snprintf(on, sizeof(on), "%u", v->stream_node);
    snprintf(oport, sizeof(oport), "%u", op);
    snprintf(in, sizeof(in), "%u", v->node_id);
    snprintf(iport, sizeof(iport), "%u", ip);
    struct pw_properties *p = pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, on, PW_KEY_LINK_OUTPUT_PORT, oport,
                                                PW_KEY_LINK_INPUT_NODE, in, PW_KEY_LINK_INPUT_PORT, iport,
                                                PW_KEY_OBJECT_LINGER, "false", NULL);
    struct pw_proxy *l = pw_core_create_object(v->g->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                                               &p->dict, 0);
    pw_properties_free(p);
    return l;
}

/* after every graph change (loop thread): link the stream in once its ports exist, recount */
static inline void rdx_vmic_update(struct rdx_vmic *v)
{
    if (!v->bound) return;
    if (!v->nlink && !v->link_err && v->stream_node != SPA_ID_INVALID) {
        static const char *const ch[2] = {"FL", "FR"};
        int nout = 0, nin = 0;
        rdx_pw_port_of(v->g, v->stream_node, 1, NULL, &nout);
        rdx_pw_port_of(v->g, v->node_id, 0, NULL, &nin);
        if (nout >= 1 && nin >= 2) {
            const struct rdx_pw_port *o[2], *in[2];
            for (int c = 0; c < 2; c++) {
                o[c] = nout == 1 ? rdx_pw_port_of(v->g, v->stream_node, 1, NULL, NULL)
                                 : rdx_pw_port_of(v->g, v->stream_node, 1, ch[c], NULL);
                in[c] = rdx_pw_port_of(v->g, v->node_id, 0, ch[c], NULL);
            }
            if (o[0] && o[1] && in[0] && in[1]) {
                for (int c = 0; c < 2; c++) v->link[v->nlink++] = rdx_vmic_link(v, o[c]->id, in[c]->id);
                fprintf(stderr, "[mic] player stream (node %u) linked into %s (node %u)%s\n", v->stream_node,
                        v->node_name, v->node_id, v->link[0] && v->link[1] ? "" : ": link-factory FAILED");
                if (!v->link[0] || !v->link[1]) v->link_err = 1;
            } else if (nout >= 2) {
                fprintf(stderr, "[mic] no FL/FR ports to link (stream %d out, source %d in)\n", nout, nin);
                v->link_err = 1;
            }
        }
    }
    int n = rdx_vmic_count(v);
    if (n != v->consumers && v->on_use) {
        v->consumers = n;
        if (v->on_use) v->on_use(n, v->user);
    }
}

/* loop locked: the virtual source exists in the graph when this returns 0 */
static inline int rdx_vmic_create(struct rdx_vmic *v, struct rdx_pw_graph *g, const char *desc, int port)
{
    v->g = g;
    v->stream_node = SPA_ID_INVALID;
    snprintf(v->node_name, sizeof(v->node_name), "rdx-mic-%d", port);
    snprintf(v->desc, sizeof(v->desc), "%s", desc);
    const char *rt = getenv("PIPEWIRE_RUNTIME_DIR");
    if (!rt || !*rt) rt = getenv("XDG_RUNTIME_DIR");
    snprintf(v->state_path, sizeof(v->state_path), "%s/%s.prev", rt && *rt ? rt : "/tmp", v->node_name);
    struct pw_properties *np = pw_properties_new(
        "factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, v->node_name, PW_KEY_NODE_DESCRIPTION, v->desc,
        PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual", "audio.channels", "2", "audio.position", "FL,FR",
        "audio.rate", "48000", "node.driver", "true", "priority.driver", "1", PW_KEY_OBJECT_LINGER, "false",
        "device.icon-name", "audio-input-microphone", NULL);
    v->node = pw_core_create_object(g->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &np->dict, 0);
    pw_properties_free(np);
    if (!v->node) return -1;
    pw_proxy_add_listener(v->node, &v->node_hook, &rdx_vmic_node_events, v);
    for (int i = 0; !v->bound && i < 50; i++) pw_thread_loop_timed_wait(g->loop, 1);
    if (!v->bound) { fprintf(stderr, "[mic] the virtual source did not appear\n"); return -1; }
    fprintf(stderr, "[mic] virtual source \"%s\" (%s) is node %u\n", v->desc, v->node_name, v->node_id);
    return 0;
}

/* loop locked, after rdx_pw_graph_start: our source becomes the default one */
static inline int rdx_vmic_take_default(struct rdx_vmic *v)
{
    struct rdx_pw_graph *g = v->g;
    if (!g->md) { fprintf(stderr, "[mic] no \"default\" metadata: the default source stays as it is\n"); return -1; }
    char cur[256];
    rdx_json_name(g->cfg_source, cur, sizeof(cur));
    snprintf(v->prev, sizeof(v->prev), "%s", g->cfg_source);
    FILE *f;
    if (!strcmp(cur, v->node_name) && (f = fopen(v->state_path, "r"))) {
        /* an earlier run ended without restoring: its saved value is the user's */
        if (!fgets(v->prev, sizeof(v->prev), f)) v->prev[0] = 0;
        v->prev[strcspn(v->prev, "\n")] = 0;
        fclose(f);
        fprintf(stderr, "[mic] previous default source from %s (an earlier run did not restore it)\n", v->state_path);
    } else if (!strcmp(cur, v->node_name)) {
        v->prev[0] = 0;
    }
    if ((f = fopen(v->state_path, "w"))) { fprintf(f, "%s\n", v->prev); fclose(f); }
    char val[160];
    snprintf(val, sizeof(val), "{\"name\":\"%s\"}", v->node_name);
    pw_metadata_set_property(g->md, PW_ID_CORE, "default.configured.audio.source", "Spa:String:JSON", val);
    v->def_set = 1;
    rdx_pw_graph_sync(g);
    fprintf(stderr, "[mic] default source is now %s (before: %s)\n", v->node_name, v->prev[0] ? v->prev : "none configured");
    return 0;
}

/* loop locked: give the default back, unless the user chose another source meanwhile */
static inline void rdx_vmic_restore_default(struct rdx_vmic *v)
{
    struct rdx_pw_graph *g = v->g;
    if (!v->def_set || !g->md) return;
    v->def_set = 0;
    char cur[256];
    rdx_json_name(g->cfg_source, cur, sizeof(cur));
    if (!strcmp(cur, v->node_name)) {
        pw_metadata_set_property(g->md, PW_ID_CORE, "default.configured.audio.source",
                                 v->prev[0] ? "Spa:String:JSON" : NULL, v->prev[0] ? v->prev : NULL);
        rdx_pw_graph_sync(g);
        fprintf(stderr, "[mic] default source restored: %s\n", v->prev[0] ? v->prev : "none configured");
    } else {
        fprintf(stderr, "[mic] default source was changed to %s meanwhile: left as it is\n", cur[0] ? cur : "none");
    }
    unlink(v->state_path);
}

static inline void rdx_vmic_destroy(struct rdx_vmic *v)
{
    for (int i = 0; i < v->nlink; i++) if (v->link[i]) pw_proxy_destroy(v->link[i]);
    v->nlink = 0;
    if (v->node) { spa_hook_remove(&v->node_hook); pw_proxy_destroy(v->node); v->node = NULL; }
}
