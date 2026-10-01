#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "hash.h"
#include "rng.h"

class HyperScan {
public:
    static constexpr double BYTES = 12;
    static constexpr int SLOT = 3, D1 = 2, D2 = 2, DMAX = 4, SILMAX = 255;
    static constexpr int L2_THRES = 25, L1_GAP = 45, L1_RATIO = 85;
    static constexpr double FILTER_RATIO = 0.6;
    static constexpr int CACHE_KB = 1, CACHE_WAYS = 16;

    HyperScan(uint64_t memory, int L, int g, int W, uint32_t seed,
              bool ghost = true, double bytes_override = 0.0,
              double filt_ratio = 0.0, double l1_ratio = 0.0, int cache_kb = -1,
              int d1 = 0, int d2 = 0)
        : L_(L), g_(g), W_(W), rng_(seed), ghost_(ghost) {
        bytes_ = bytes_override > 0 ? bytes_override : BYTES;
        d1_ = d1 > 0 ? d1 : D1; d2_ = d2 > 0 ? d2 : D2;
        if (d1_ > DMAX) d1_ = DMAX; if (d2_ > DMAX) d2_ = DMAX;

        l1_thres_ = L_ - L1_GAP - L2_THRES > 0 ? L_ - L1_GAP : 1;   // HIT - 45
        if (l1_thres_ > 255) l1_thres_ = 255;
        l2_thres_ = L2_THRES;
        base_ = l1_thres_ + l2_thres_;

        const double fr = filt_ratio > 0 ? filt_ratio : FILTER_RATIO;
        const double l1r = l1_ratio > 0 ? l1_ratio : (L1_RATIO / 100.0);
        const double ckb = cache_kb >= 0 ? (double)cache_kb : (double)CACHE_KB;
        const double filt = (double)memory * fr;
        const double cache_b = ckb * 1024.0;
        double rem = filt - cache_b; if (rem < 1024.0) rem = 1024.0;
        const double m1 = rem * l1r, m2 = rem * (1.0 - l1r);
        const double per = 1.0 + 1.0 / 8.0;
        len1_ = (uint32_t)(m1 / d1_ / per); if (len1_ < 1) len1_ = 1;
        len2_ = (uint32_t)(m2 / d2_ / per); if (len2_ < 1) len2_ = 1;

        const double l3b = (double)memory * (1.0 - fr);
        len3_ = (uint32_t)(l3b / 2.0 / (bytes_ * SLOT)); if (len3_ < 2) len3_ = 2;

        for (int i = 0; i < d1_; i++) h1_[i] = mix32(0x9E3779B9u + i * 0x85EBCA6Bu, seed);
        for (int i = 0; i < d2_; i++) h2_[i] = mix32(0xC2B2AE35u + i * 0x27D4EB2Fu, seed);
        h3_ = mix32(0x165667B1u, seed);
        hc_ = mix32(0x9E3779B1u, seed);

        c1_.assign((size_t)len1_ * d1_, 0); b1_.assign((size_t)len1_ * d1_, 0);
        c2_.assign((size_t)len2_ * d2_, 0); b2_.assign((size_t)len2_ * d2_, 0);
        hot_.assign((size_t)len3_ * 2 * SLOT, E{0, 0, 0, 0, 0});

        cache_sets_ = (uint32_t)(cache_b / 4.0 / CACHE_WAYS); if (cache_sets_ < 1) cache_sets_ = 1;
        cache_.assign((size_t)cache_sets_ * CACHE_WAYS, 0);
    }

    uint32_t buckets() const { return len3_ * 2 * SLOT; }
    double bucket_bytes() const { return bytes_; }

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        int cur = 0; uint64_t bnd = N; int c = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == bnd) { c++; bnd += N; boundary(cur, emit); cur = c; }
            cache_insert(flow_id(pairs[2 * q], pairs[2 * q + 1]));
        }
        for (int w = cur; w < W_; w++) boundary(w, emit);
    }

private:
    struct E { uint32_t id; uint16_t count; uint8_t seen, sil, rep; };

    // --- the software cache: dedup inside a window, flushed at its end ---
    inline void cache_insert(uint32_t id) {
        const uint32_t s = (mix32(id, hc_) % cache_sets_) * CACHE_WAYS;
        for (int i = 0; i < CACHE_WAYS; i++) {
            uint32_t& e = cache_[s + i];
            if (e == 0) { e = id ? id : 1u; return; }
            if (e == id) return;
        }
        insert(id);
    }
    inline void flush_cache() {
        for (auto& e : cache_) if (e) { insert(e); e = 0; }
    }

    // --- Filter::Insert, verbatim in structure ---
    // returns true when the item is absorbed here, false to promote it
    inline bool filter_insert(std::vector<uint8_t>& cnt, std::vector<uint8_t>& bit,
                              uint32_t len, int rows, const uint32_t* hs,
                              uint32_t id, int threshold) {
        uint32_t pos[DMAX];
        int mn = 0x7FFFFFFF; bool any_unset = false;
        for (int i = 0; i < rows; i++) {
            pos[i] = (uint32_t)i * len + mix32(id, hs[i]) % len;
            const int cur = cnt[pos[i]];
            if (cur < mn) mn = cur;
            if (!bit[pos[i]]) any_unset = true;
        }
        if (!any_unset) return true;                  // already counted this window
        if (mn >= threshold) {                        // saturated: promote
            for (int i = 0; i < rows; i++) bit[pos[i]] = 1;
            return false;
        }
        for (int i = 0; i < rows; i++)
            if (cnt[pos[i]] == mn) { if (!bit[pos[i]]) { bit[pos[i]] = 1; if (cnt[pos[i]] < 255) cnt[pos[i]]++; } }
        return true;
    }

    inline void insert(uint32_t id) {
        if (filter_insert(c1_, b1_, len1_, d1_, h1_, id, l1_thres_)) return;
        if (filter_insert(c2_, b2_, len2_, d2_, h2_, id, l2_thres_)) return;
        hot_insert(id);
    }

    // --- HotStorage::Insert, verbatim in structure ---
    inline void hot_insert(uint32_t id) {
        const uint32_t hh = mix32(id, h3_) % (len3_ * (len3_ - 1));
        const uint32_t h0 = hh % len3_, h1 = hh / len3_;
        int mn = 0x7FFFFFFF; long minIdx = -1;
        for (int t = 0; t < 2; t++) {
            const uint32_t base = ((uint32_t)t * len3_ + (t ? h1 : h0)) * SLOT;
            for (int i = 0; i < SLOT; i++) {
                E& e = hot_[base + i];
                if (e.id == id && e.count) {
                    if (!e.seen) { e.seen = 1; if (e.count < 65535) e.count++; }
                    e.sil = 0; e.rep = 0; return;
                }
                if (e.count < mn) { mn = e.count; minIdx = (long)(base + i); }
            }
        }
        if (minIdx < 0) return;
        if (hh % (uint32_t)(mn + 1) == 0) {           // replace with prob 1/(min+1)
            E& e = hot_[minIdx];
            e.id = id; e.rep = 0; e.sil = 0;
            if (!e.seen) { e.seen = 1; if (e.count < 65535) e.count++; }
        }
    }

    // --- NewWindow: flush the cache, score the hot storage, clear presence ---
    template <class Sink> void boundary(int w, Sink& emit) {
        flush_cache();
        for (auto& e : hot_) {
            if (e.count) {
                if (!e.seen) { if (e.sil < SILMAX) e.sil++; } else e.sil = 0;
                const int phat = base_ + (int)e.count;
                if (phat >= L_ && !e.rep && (int)e.sil >= g_) {
                    emit(e.id, w, 0, phat);
                    if (ghost_) e.rep = 1; else { e.id = 0; e.count = 0; e.sil = 0; }
                }
            }
            e.seen = 0;
        }
        memset(b1_.data(), 0, b1_.size());
        memset(b2_.data(), 0, b2_.size());
    }

    int L_, g_, W_;
    int l1_thres_, l2_thres_, base_;
    double bytes_ = BYTES;
    uint32_t len1_, len2_, len3_, cache_sets_;
    int d1_ = D1, d2_ = D2;
    uint32_t h1_[DMAX], h2_[DMAX], h3_, hc_;
    std::vector<uint8_t> c1_, b1_, c2_, b2_;
    std::vector<uint32_t> cache_;
    std::vector<E> hot_;
    Rng rng_;
    bool ghost_ = true;
};
