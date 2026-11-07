/* Shared compute kernel — compiled natively (xtensa-gcc) AND to WASM (wasi-clang)
 * from the same source, so interpreter/AOT/native run byte-for-byte the same
 * algorithm. Pure 32-bit integer ALU work (xorshift + LCG), register-only, no
 * host calls in the loop: measures raw execution speed, not call dispatch. */
#pragma once
#include <stdint.h>

static inline uint32_t compute_kernel(uint32_t seed, uint32_t iters)
{
    uint32_t x = seed;
    for (uint32_t i = 0; i < iters; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x = x * 1664525u + 1013904223u;
        x += (x >> 7) ^ (i * 2654435761u);
    }
    return x;
}

#define EXEC_KERNEL_SEED  0x12345678u
#define EXEC_KERNEL_ITERS 2000000u
