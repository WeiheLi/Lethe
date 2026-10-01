#pragma once
#include <cstdint>
// Deterministic given a seed; used only for probabilistic replacement.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0xDEADBEEFull) { if (!s) s = 1; }
    inline uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    inline double unit() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
};
