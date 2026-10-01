// On-Off Sketch (VLDB'20) FPI core, replacement policy untouched, plus a
// per-slot last-window field and a reporting scan hooked into NewWindow().
#pragma once
#include <cstdint>
#include <vector>
#include "hash.h"

class OnOffScan {
public:
    static constexpr int SLOTS = 8;
    static constexpr double BYTES_PER_BUCKET = SLOTS * (4 + 2 + 2) + 2 + (SLOTS + 1) / 8.0;

    OnOffScan(uint64_t memory, int L, int g, int W, uint32_t seed) : L_(L), g_(g), W_(W) {
        len_ = (uint32_t)(memory / BYTES_PER_BUCKET);
        if (len_ < 1) len_ = 1;
        item_.assign((size_t)len_ * SLOTS, 0);
        cnt_.assign((size_t)len_ * SLOTS, 0);
        lw_.assign((size_t)len_ * SLOTS, 0);
        sk_.assign(len_, 0);
        bbit_.assign(((size_t)len_ * SLOTS + 63) / 64, 0);
        sbit_.assign((len_ + 63) / 64, 0);
        h_ = mix32(0xA5A5u, seed);
    }

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        int cur = 0; uint64_t bnd = N; int c = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == bnd) { c++; bnd += N; new_window(cur, emit); cur = c; }
            insert(flow_id(pairs[2 * q], pairs[2 * q + 1]), c);
        }
        for (int w = cur; w < W_; w++) new_window(w, emit);
    }
    uint32_t length() const { return len_; }

private:
    static inline bool get(std::vector<uint64_t>& v, size_t i) { return (v[i >> 6] >> (i & 63)) & 1ull; }
    static inline void set(std::vector<uint64_t>& v, size_t i) { v[i >> 6] |= 1ull << (i & 63); }
    static inline bool setnget(std::vector<uint64_t>& v, size_t i) {
        bool b = get(v, i); set(v, i); return b;
    }

    inline void insert(uint32_t id, int c) {
        const uint32_t pos = mix32(id, h_) % len_;
        const size_t base = (size_t)pos * SLOTS;
        for (int i = 0; i < SLOTS; i++)
            if (item_[base + i] == id) {
                cnt_[base + i] += (uint16_t)(!setnget(bbit_, base + i));
                lw_[base + i] = (uint16_t)c;
                return;
            }
        if (!get(sbit_, pos)) {
            for (int i = 0; i < SLOTS; i++)
                if (cnt_[base + i] == sk_[pos]) {
                    item_[base + i] = id; cnt_[base + i]++;
                    lw_[base + i] = (uint16_t)c; set(bbit_, base + i);
                    return;
                }
            sk_[pos]++; set(sbit_, pos);
        }
    }
    // The per-window on/off reset already walks the table; the report rides on it.
    template <class Sink> void new_window(int w, Sink& emit) {
        for (uint32_t p = 0; p < len_; p++) {
            const size_t base = (size_t)p * SLOTS;
            for (int i = 0; i < SLOTS; i++)
                if (item_[base + i] && cnt_[base + i] >= L_ && w - (int)lw_[base + i] >= g_) {
                    emit(item_[base + i], w, 0, (int)cnt_[base + i]);
                    item_[base + i] = 0; cnt_[base + i] = sk_[p];   // slot becomes replaceable
                }
        }
        std::fill(bbit_.begin(), bbit_.end(), 0ull);
        std::fill(sbit_.begin(), sbit_.end(), 0ull);
    }
    int L_, g_, W_; uint32_t len_, h_;
    std::vector<uint32_t> item_;
    std::vector<uint16_t> cnt_, lw_, sk_;
    std::vector<uint64_t> bbit_, sbit_;
};
