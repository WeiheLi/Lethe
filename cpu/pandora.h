#pragma once
#include <cstdint>
#include <vector>
#include "hash.h"
#include "rng.h"

class PandoraScan {
public:
    static constexpr double BYTES = 14;
    static constexpr int ROWS = 4, DEFAULT_VALUE = 50, SILMAX = 255;

    PandoraScan(uint64_t memory, int L, int g, int W, uint32_t seed, bool ghost = false,
                bool probe_first = true, double bytes_override = 0.0)
        : L_(L), g_(g), W_(W), rng_(seed), ghost_(ghost), pf_(probe_first) {
        bytes_ = bytes_override > 0 ? bytes_override : BYTES;
        width_ = (uint32_t)(memory / (bytes_ * ROWS)); if (width_ < 1) width_ = 1;
        t_.assign((size_t)width_ * ROWS, B{0, 0, 0, 0, 0, 0});
        for (int i = 0; i < ROWS; i++) h_[i] = mix32(0x2545F49u + i * 0x9E3779B9u, seed);
    }
    uint32_t buckets() const { return width_ * ROWS; }

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        int cur = 0; uint64_t bnd = N; int c = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == bnd) { c++; bnd += N; boundary(cur, emit); cur = c; }
            update(flow_id(pairs[2 * q], pairs[2 * q + 1]));
        }
        for (int w = cur; w < W_; w++) boundary(w, emit);
    }

private:
    struct B { uint32_t id; uint16_t count, cold; uint8_t status, rep, sil; };

    inline void install(B& b, uint32_t id) {
        b.id = id; b.count = 1; b.cold = 0; b.status = 1; b.rep = 0; b.sil = 0;
    }
    inline void hit(B& b) {
        if (b.rep) b.rep = 0;
        if (!b.status) { if (b.count < 65535) b.count++; b.status = 1; }
        b.sil = 0;
    }
    inline void update(uint32_t id) {
        uint32_t idx[ROWS];
        for (int i = 0; i < ROWS; i++) idx[i] = (uint32_t)i * width_ + mix32(id, h_[i]) % width_;
        if (pf_) {
            for (int i = 0; i < ROWS; i++) {
                B& b = t_[idx[i]];
                if (b.id == id && (b.count || b.status || b.rep)) { hit(b); return; }
            }
        }
        int loc = -1; uint32_t mn = 0xFFFFFFFFu;
        for (int i = 0; i < ROWS; i++) {
            B& b = t_[idx[i]];
            if ((b.count == 0 && b.status == 0) || b.rep) { install(b, id); return; }
            if (!pf_ && b.id == id) { hit(b); return; }
            if (b.count < mn) { mn = b.count; loc = (int)idx[i]; }
        }
        if (loc < 0) return;
        B& b = t_[loc];
        if (b.status) return;
        const int value = (DEFAULT_VALUE - (int)b.cold < 1) ? 1 : DEFAULT_VALUE - (int)b.cold;
        const uint32_t m = (uint32_t)b.count * (uint32_t)value;
        if ((uint32_t)(rng_.next() % (m + 1)) == 0) install(b, id);   // prob 1/(count*value+1)
    }
    // Inactivity() and the silence counter, then the report scan, then the reset.
    template <class Sink> void boundary(int w, Sink& emit) {
        for (auto& b : t_) {
            if (b.status == 0) {
                if (b.cold < 65535) b.cold++;
                if (b.sil < SILMAX) b.sil++;
            } else if (b.cold) b.cold--;
            if (b.count >= L_ && !b.rep && (int)b.sil >= g_) {
                emit(b.id, w, 0, (int)b.count);
                if (ghost_) b.rep = 1; else { b.id = 0; b.count = 0; b.cold = 0; b.sil = 0; }
            }
            b.status = 0;
        }
    }
    int L_, g_, W_; bool ghost_ = false, pf_ = true;
public:
    double bucket_bytes() const { return bytes_; }
private:
    double bytes_ = BYTES;
    uint32_t width_, h_[ROWS];
    std::vector<B> t_;
    Rng rng_;
};
