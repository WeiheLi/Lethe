// Hash utilities. One library (MurmurHash3), explicit seeds everywhere.
#pragma once
#include <cstdint>
#include <cstring>

static inline uint32_t rotl32(uint32_t x, int8_t r) { return (x << r) | (x >> (32 - r)); }

// MurmurHash3 x86_32
static inline uint32_t mm3_32(const void* key, int len, uint32_t seed) {
    const uint8_t* data = (const uint8_t*)key;
    const int nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    const uint32_t* blocks = (const uint32_t*)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1; memcpy(&k1, blocks + i, 4);
        k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2;
        h1 ^= k1; h1 = rotl32(h1, 13); h1 = h1 * 5 + 0xe6546b64;
    }
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= tail[2] << 16; [[fallthrough]];
        case 2: k1 ^= tail[1] << 8;  [[fallthrough]];
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = rotl32(k1, 15); k1 *= c2; h1 ^= k1;
    }
    h1 ^= len;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6b; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35; h1 ^= h1 >> 16;
    return h1;
}

// The single flow-ID hash shared by every method (fixed seed, never varied).
static constexpr uint32_t HID_SEED = 0x9E3779B9u;
static inline uint32_t flow_id(uint32_t src, uint32_t dst) {
    uint32_t k[2] = {src, dst};
    return mm3_32(k, 8, HID_SEED);
}

// Cheap strong mixer used for the per-structure hashes H1/H2 (seeded per run).
static inline uint32_t mix32(uint32_t x, uint32_t seed) {
    x ^= seed;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
