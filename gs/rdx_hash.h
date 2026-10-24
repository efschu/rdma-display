/* Frame checksum for the gamescope path tests: FNV-1a over 64-bit words of the visible pixels (rows of
 * width*4 bytes at the given stride). rdx_gs_src --verify hashes what the CPU sees in the
 * host buffer right after READY; rdx_sink_null --hash hashes what the NIC delivered. Equal
 * hashes per frame number = the frame was complete and coherent when it was sent. */
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint64_t rdx_frame_hash(const uint8_t *p, uint32_t w, uint32_t h, uint32_t stride)
{
    uint64_t x = 0xcbf29ce484222325ull;
    size_t row = (size_t)w * 4;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = p + (size_t)y * stride;
        size_t i = 0;
        for (; i + 8 <= row; i += 8) {
            uint64_t v;
            memcpy(&v, r + i, 8);
            x = (x ^ v) * 0x100000001b3ull;
        }
        for (; i < row; i++) x = (x ^ r[i]) * 0x100000001b3ull;
    }
    return x;
}
