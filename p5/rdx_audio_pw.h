/* SPDX-License-Identifier: MIT
 *
 * PipeWire and process helpers shared by rdx_audio_tx and rdx_audio_rx.
 */
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

/* An inaudible null sink of our own: timer driven (its own driver, so the
 * user's audio graph keeps its quantum), removed when we disconnect.
 * Call with the thread loop locked; returns once the node is in the graph. */
struct rdx_null_sink {
    struct pw_proxy *proxy;
    struct spa_hook hook;
    struct pw_thread_loop *loop;
    char name[64];
    uint32_t id;
    int bound;
};

static void rdx_null_sink_bound(void *data, uint32_t global_id)
{
    struct rdx_null_sink *ns = data;
    ns->id = global_id;
    ns->bound = 1;
    pw_thread_loop_signal(ns->loop, false);
}

static const struct pw_proxy_events rdx_null_sink_events = {
    PW_VERSION_PROXY_EVENTS,
    .bound = rdx_null_sink_bound,
};

/* tests: > 0 forces this graph quantum on our own null sink (node.force-quantum), like a device whose
 * cycles are much larger than the network quantum (rdx_audio_rx --sink-quantum) */
static int rdx_null_sink_quantum;

static inline int rdx_null_sink_create(struct rdx_null_sink *ns, struct pw_core *core,
                                       struct pw_thread_loop *loop, const char *tag)
{
    ns->loop = loop;
    snprintf(ns->name, sizeof(ns->name), "rdx-null-%s-%d", tag, (int)getpid());
    struct pw_properties *np = pw_properties_new(
        "factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, ns->name,
        PW_KEY_NODE_DESCRIPTION, "rdma-display test sink (inaudible)", PW_KEY_MEDIA_CLASS, "Audio/Sink",
        "audio.channels", "2", "audio.position", "FL,FR", "audio.rate", "48000",
        "node.driver", "true", "priority.driver", "1", "object.linger", "false",
        "node.pause-on-idle", "false", NULL);
    if (rdx_null_sink_quantum > 0) {
        char q[16];
        snprintf(q, sizeof(q), "%d", rdx_null_sink_quantum);
        pw_properties_set(np, "node.force-quantum", q);
    }
    ns->proxy = pw_core_create_object(core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &np->dict, 0);
    pw_properties_free(np);
    if (!ns->proxy) return -1;
    pw_proxy_add_listener(ns->proxy, &ns->hook, &rdx_null_sink_events, ns);
    while (!ns->bound) pw_thread_loop_wait(loop);
    fprintf(stderr, "[audio] null sink %s bound as node %u\n", ns->name, ns->id);
    return 0;
}

/* process CPU (user+sys, all threads) in ns */
static inline uint64_t rdx_audio_cpu_ns(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec) * 1000000000ull +
           ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec) * 1000ull;
}

/* PM QoS: hold /dev/cpu_dma_latency at `us` while the fd stays open
 * (avoids deep C-state wake-ups); -1 if us <= 0 or on error */
static inline int rdx_audio_pm_qos(int us)
{
    if (us <= 0) return -1;
    int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (fd < 0) { perror("/dev/cpu_dma_latency"); return -1; }
    int32_t v = us;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) { perror("cpu_dma_latency"); close(fd); return -1; }
    return fd;
}
