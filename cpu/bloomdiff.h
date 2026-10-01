#pragma once
#include <cstdint>
#include <vector>
#include "hash.h"
#include "rng.h"

class BloomDiff {
public:
    BloomDiff(uint64_t memory, int L, int g, uint32_t seed, double bf_split = 0.5,
              bool probe_first = true, double bytes_override = 0.0)
        : L_(L), g_(g), ring_(g + 1), pf_(probe_first) {
        bytes_ = bytes_override > 0 ? bytes_override : BYTES;
        const uint64_t bf_bytes = (uint64_t)(memory * bf_split);
        const uint64_t ll_bytes = memory - bf_bytes;
        width_ = (uint32_t)(ll_bytes / (bytes_ * ROWS)); if (width_ < 1) width_ = 1;
        t_.assign((size_t)width_ * ROWS, B{0, 0, 0, 0, 0});
        for (int i = 0; i < ROWS; i++) h_[i] = mix32(0x9E37u + i * 0x9E3779B9u, seed);
        rng_ = Rng(seed ^ 0xA1B2C3D4u);
        mbits_ = (uint32_t)((bf_bytes / ring_) * 8); if (mbits_ < 64) mbits_ = 64;
        bf_.assign(ring_, std::vector<uint64_t>((mbits_ + 63) / 64, 0));
        for (int k = 0; k < 3; k++) bh_[k] = mix32(0x1000u + k * 0x9E3779B9u, seed);
    }
    static constexpr double BYTES = 14;
    static constexpr int SILMAX = 255;
    uint32_t buckets() const { return width_ * ROWS; }
    uint32_t bits_per_filter() const { return mbits_; }
    double bucket_bytes() const { return bytes_; }

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        int cur = 0;
        std::fill(bf_[0].begin(), bf_[0].end(), 0ull);
        uint64_t bnd = N; int c = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == bnd) { c++; bnd += N; }
            if (c != cur) {
                for (int w = cur + 1; w <= c; w++) {
                    for (auto& b : t_) {                        // Pandora's Inactivity()
                        if (b.status == 0) {
                            if (b.cold < 65535) b.cold++;
                            if (b.sil < SILMAX) b.sil++;
                        } else if (b.cold) b.cold--;
                        b.status = 0;
                    }
                    declare(w - 1, emit);                       // close window w-1
                    auto& f = bf_[w % ring_]; std::fill(f.begin(), f.end(), 0ull);
                }
                cur = c;
            }
            const uint32_t id = flow_id(pairs[2 * q], pairs[2 * q + 1]);
            update(id);
            for (int k = 0; k < 3; k++) bset(bf_[c % ring_], mix32(id, bh_[k]) % mbits_);
        }
        for (auto& b : t_) if (b.status == 0 && b.sil < SILMAX) b.sil++;
        declare(cur, emit);
    }

private:
    struct B { uint32_t id; uint16_t P, cold; uint8_t status, sil; };
    static inline void bset(std::vector<uint64_t>& v, uint32_t i) { v[i >> 6] |= 1ull << (i & 63); }
    static inline bool bget(const std::vector<uint64_t>& v, uint32_t i) { return (v[i >> 6] >> (i & 63)) & 1ull; }
    inline bool seen(int w, uint32_t id) const {
        const auto& f = bf_[((w % ring_) + ring_) % ring_];
        for (int k = 0; k < 3; k++) if (!bget(f, mix32(id, bh_[k]) % mbits_)) return false;
        return true;
    }
    inline void hit(B& b) {
        if (!b.status) { if (b.P < 65535) b.P++; b.status = 1; }
        b.sil = 0;
    }
    inline void update(uint32_t id) {   // Pandora's policy, no scan
        uint32_t idx[ROWS];
        for (int i = 0; i < ROWS; i++) idx[i] = (uint32_t)i * width_ + mix32(id, h_[i]) % width_;
        if (pf_) {
            for (int i = 0; i < ROWS; i++) {
                B& b = t_[idx[i]];
                if (b.id == id && (b.P || b.status)) { hit(b); return; }
            }
        }
        int loc = -1; uint32_t mn = 0xFFFFFFFFu;
        for (int i = 0; i < ROWS; i++) {
            B& b = t_[idx[i]];
            if (b.P == 0 && b.status == 0) { b.id = id; b.P = 1; b.cold = 0; b.status = 1; b.sil = 0; return; }
            if (!pf_ && b.id == id) { hit(b); return; }
            if (b.P < mn) { mn = b.P; loc = (int)idx[i]; }
        }
        if (loc < 0) return;
        B& b = t_[loc];
        if (b.status) return;
        const int v = (DEFAULT_VALUE - (int)b.cold < 1) ? 1 : DEFAULT_VALUE - (int)b.cold;
        if ((uint32_t)(rng_.next() % ((uint32_t)b.P * (uint32_t)v + 1)) == 0) {
            b.id = id; b.P = 1; b.cold = 0; b.status = 1; b.sil = 0;
        }
    }
    // At the end of window c, an established resident whose silence run is exactly
    // g windows and that is absent from every filter of windows c-g+1..c is declared.
    template <class Sink> void declare(int c, Sink& emit) {
        if (c - g_ < 0) return;
        for (auto& b : t_) {
            if (b.P < L_ || (int)b.sil != g_) continue;
            bool alive = false;
            for (int w = c - g_ + 1; w <= c && !alive; w++) alive = seen(w, b.id);
            if (!alive) { emit(b.id, c, 0, (int)b.P); b.P = 0; b.id = 0; b.sil = 0; }
        }
    }
    int L_, g_, ring_; bool pf_ = true;
    double bytes_ = BYTES;
    static constexpr int ROWS = 4, DEFAULT_VALUE = 50;
    uint32_t width_, mbits_, h_[4], bh_[3];
    Rng rng_{1};
    std::vector<B> t_;
    std::vector<std::vector<uint64_t>> bf_;
};
