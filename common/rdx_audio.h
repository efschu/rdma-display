/* SPDX-License-Identifier: MIT
 *
 * rdma-display audio path (P5): wire format shared by rdx_audio_tx (sender)
 * and rdx_audio_rx (player). The same pair carries both directions: desktop
 * audio A -> B and microphone B -> A just swap the hosts.
 *
 * Transport: the player exposes a ring of RDX_AUDIO_RING_FRAMES interleaved
 * F32 frames in host RAM. The sender writes quantum k (RDX_AUDIO_QUANTUM
 * frames) with ONE RDMA_WRITE_WITH_IMM to ring position (k * quantum) mod ring,
 * imm = k. The completion at the player implies the data is placed, so the
 * write index can never run ahead of its data.
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include "rdx_common.h"

#define RDX_AUDIO_MAGIC       0x31445541u   /* "AUD1" */
#define RDX_AUDIO_RATE        48000u
#define RDX_AUDIO_CH          2u
#define RDX_AUDIO_QUANTUM     64u
#define RDX_AUDIO_RING_FRAMES 65536u        /* 1.37 s, multiple of the quantum */
#define RDX_AUDIO_FRAME_BYTES (RDX_AUDIO_CH * (uint32_t)sizeof(float))
#define RDX_AUDIO_MARK_EVERY  75u           /* one latency marker per 100 ms */
#define RDX_AUDIO_IMM_END     0xFFFFFFFFu

/* player -> sender during bootstrap */
struct rdx_audio_hello {
    uint32_t magic, rate, channels, quantum, ring_frames;
    uint64_t base_addr;
    uint32_t rkey;
    struct rdx_conn conn;
};

/* player -> sender, one SEND per marker quantum (every RDX_AUDIO_MARK_EVERY) */
struct rdx_audio_report {
    uint32_t quantum;        /* marker quantum index k */
    uint32_t residence_us;   /* player clock: CQE of quantum k -> handed to PipeWire */
    uint32_t delay_us;       /* PipeWire: handed over -> reaches the device (graph + resampler) */
    int32_t  fill_frames;    /* ring fill at that moment */
    int32_t  ppm_milli;      /* current rate correction, (1/corr - 1) * 1e9 */
    uint32_t underruns;      /* cumulative */
};

/* player -> sender control, as a report with one of these in .quantum (rest zero). Microphone
 * direction: the player's host opens the sender's capture only while an app records there. */
#define RDX_AUDIO_CTL_MIC_OPEN  0xFFFFFFF0u
#define RDX_AUDIO_CTL_MIC_CLOSE 0xFFFFFFF1u

/* deterministic test signal: 440 Hz left, 997 Hz right, exact integer phase so
 * both hosts compute bit-identical samples for any absolute frame index; the
 * signal repeats every second, so a 48000-frame table serves every lookup */
static inline float rdx_audio_test_sample_calc(uint64_t frame, unsigned ch)
{
    const uint64_t f = ch ? 997u : 440u;
    uint64_t ph = ((frame % RDX_AUDIO_RATE) * f) % RDX_AUDIO_RATE;
    return 0.25f * (float)sin(2.0 * M_PI * (double)ph / (double)RDX_AUDIO_RATE);
}

static float rdx_audio_tab[RDX_AUDIO_RATE * RDX_AUDIO_CH];

static inline void rdx_audio_test_init(void)
{
    for (uint32_t i = 0; i < RDX_AUDIO_RATE; i++)
        for (unsigned c = 0; c < RDX_AUDIO_CH; c++)
            rdx_audio_tab[i * RDX_AUDIO_CH + c] = rdx_audio_test_sample_calc(i, c);
}

static inline float rdx_audio_test_sample(uint64_t frame, unsigned ch)
{
    return rdx_audio_tab[(frame % RDX_AUDIO_RATE) * RDX_AUDIO_CH + ch];
}
