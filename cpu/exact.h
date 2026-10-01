// Exact table (Sec 4.1): the "why not just keep per-flow state" upper bound.
// Window-end scan is made O(appearances) by bucketing flows on their last window.
#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "hash.h"

class ExactTable {
public:
    ExactTable(int L, int g, int W) : L_(L), g_(g), W_(W), pend_(W + 2) {}

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        st_.reserve(1u << 21);
        int cur = 0; uint64_t bnd = N; int c = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == bnd) { c++; bnd += N; sweep(cur, emit); cur = c; }
            const uint32_t id = flow_id(pairs[2 * q], pairs[2 * q + 1]);
            S& s = st_[id];
            if (s.last != c || s.P == 0) {
                s.P++; s.last = c; s.reported = 0;
                if (c + g_ <= W_ - 1) pend_[c].push_back(id);
            }
            if ((uint32_t)st_.size() > peak_) peak_ = (uint32_t)st_.size();
        }
        for (int w = cur; w < W_; w++) sweep(w, emit);
    }
    uint32_t peak_entries() const { return peak_; }

private:
    struct S { uint32_t P = 0; int32_t last = -1; uint8_t reported = 0; };
    // At the end of window w, a flow last seen at w-g has been silent for the
    // g complete windows w-g+1 .. w.
    template <class Sink> void sweep(int w, Sink& emit) {
        const int x = w - g_;
        if (x < 0 || x >= (int)pend_.size()) return;
        for (uint32_t id : pend_[x]) {
            auto it = st_.find(id);
            if (it == st_.end()) continue;
            S& s = it->second;
            if (s.last == x && !s.reported && s.P >= (uint32_t)L_) { s.reported = 1; emit(id, w, 0, (int)s.P); }
        }
        std::vector<uint32_t>().swap(pend_[x]);
    }
    int L_, g_, W_;
    uint32_t peak_ = 0;
    std::unordered_map<uint32_t, S> st_;
    std::vector<std::vector<uint32_t>> pend_;
};
