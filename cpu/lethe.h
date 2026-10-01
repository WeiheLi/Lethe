#pragma once
#define LETHE_INLINE inline __attribute__((always_inline))
#include <cstdint>
#include <cstring>
#include <vector>
#include "hash.h"
#include "rng.h"

struct LetheCfg {
    int L = 10, g = 10;
    int ts_bits = 16;
    int p_bits = 16;
    int k_hand = 1;
    int hand_steps = 1;   // --hand-steps: buckets the hand advances per tick.
    double decay_k = 0.5;   // hand decay aggressiveness for candidates
    bool age_decay = false;           // --age-decay (refuted variant, kept for ablation)
    bool ripening_protection = true;  // --no-ripening-protection
    int  evict_coin = 0;              // --evict-coin: which sketch's coin condemns
    bool harvest_first = true;        // --no-harvest-first
    bool hand = true;                 // --no-hand
    bool scan = false;                // --scan: release at the window end
                                      // instead of with the hand
    int  W = 0;                       // windows in the stream, for the final sweep
    bool hand_decay = true;           // --hand-no-decay: release-only hand (diagnostic)
    bool single_choice = false;       // --single-choice
    bool ghost = true;                // --no-ghost : forget identity on report
    int  ways = 2;                    // --ways d
    double admit = 1.0;               // --admit p : probabilistic admission of new flows
    bool grouped = false;             // --grouped: one hash, d contiguous buckets
    uint32_t seed = 1;
};

struct Bucket { uint32_t id; uint16_t P; uint16_t lw; uint8_t rep; };

static inline int ceil_log2(uint64_t x) {         // smallest l with 2^l >= x
    int l = 0;
    while ((1ull << l) < x) l++;
    return l;
}

struct PlainStore {
    static constexpr bool packed = false;
    std::vector<Bucket> t;
    int width = 8;
    void setup(const LetheCfg& cfg) {
        if (cfg.p_bits >= 16) width = 8;
        else width = (cfg.ts_bits >= 16) ? 7 : 6;   // 32b id + 8b P + {4b,16b} last_win
    }
    void init(uint32_t M) { t.assign(M, Bucket{0, 0, 0, 0}); }
    // the logic works on the bucket where it lives; flush has nothing to do
    LETHE_INLINE Bucket& at(uint32_t i) { return t[i]; }
    LETHE_INLINE void flush() {}
    int bytes() const { return width; }
    int lP_bits() const { return 16; }
    int lT_bits() const { return 16; }
};

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "PackedStore lays its tail out in a little-endian word"
#endif
struct PackedStore {
    static constexpr bool packed = true;
    std::vector<uint8_t> m;
    int lP = 9, lT = 4, tail = 14;
    uint32_t stride = 10;
    void setup(const LetheCfg& cfg) {
        lP = cfg.W > 0 ? ceil_log2((uint64_t)cfg.W + 1) : 16;
        lT = cfg.g >= 0 ? ceil_log2((uint64_t)cfg.g + 2) : 16;
        if (lP > 16) lP = 16;                // the decoded count is a uint16
        if (lP < 1)  lP = 1;
        if (lT < 1)  lT = 1;
        tail = lP + lT + 1;
        stride = (uint32_t)((64 + tail + 7) / 8);
    }
    void init(uint32_t M) { m.assign((size_t)M * stride + 16, 0); }
    struct Slot { uint32_t i; Bucket b, orig; };
    Slot slots[16];
    int nslots = 0;
    LETHE_INLINE Bucket& at(uint32_t i) {
        for (int k = 0; k < nslots; k++) if (slots[k].i == i) return slots[k].b;
        Slot& s = slots[nslots++];
        s.i = i; s.b = get(i); s.orig = s.b;
        return s.b;
    }
    LETHE_INLINE void flush() {
        for (int k = 0; k < nslots; k++) {
            const Bucket &b = slots[k].b, &o = slots[k].orig;
            if (b.id != o.id || b.P != o.P || b.lw != o.lw || b.rep != o.rep) put(slots[k].i, b);
        }
        nslots = 0;
    }
    LETHE_INLINE Bucket get(uint32_t i) const {
        const uint8_t* p = m.data() + (size_t)i * stride;
        uint64_t lo, hi;
        memcpy(&lo, p, 8);                   // the key
        memcpy(&hi, p + 8, 8);               // the tail, plus whatever follows it
        Bucket b;
        b.id  = (uint32_t)lo;
        b.P   = (uint16_t)(hi & ((1ull << lP) - 1));
        b.lw  = (uint16_t)((hi >> lP) & ((1ull << lT) - 1));
        b.rep = (uint8_t)((hi >> (lP + lT)) & 1ull);
        return b;
    }
    LETHE_INLINE void put(uint32_t i, const Bucket& b) {
        uint8_t* p = m.data() + (size_t)i * stride;
        const uint64_t lo = (uint64_t)b.id;
        memcpy(p, &lo, 8);
        const uint64_t tmask = (1ull << tail) - 1;
        uint64_t hi;
        memcpy(&hi, p + 8, 8);
        hi = (hi & ~tmask)
           | ((uint64_t)b.P   & ((1ull << lP) - 1))
           | (((uint64_t)b.lw & ((1ull << lT) - 1)) << lP)
           | (((uint64_t)b.rep & 1ull) << (lP + lT));
        memcpy(p + 8, &hi, 8);
    }
    int bytes() const { return (int)stride; }
    int lP_bits() const { return lP; }
    int lT_bits() const { return lT; }
};

template <class Store>
class LetheT {
public:
    enum St { FREE, GHOST, RIPE, STALE, RIPENING, ACTIVE, CAND };

    LetheT(uint64_t memory_bytes, const LetheCfg& c) : cfg(c), rng(c.seed) {
        st.setup(cfg);
        if (Store::packed) {
            cfg.p_bits  = st.lP_bits();
            cfg.ts_bits = st.lT_bits();
        }
        tsmod = (cfg.ts_bits >= 16) ? 65536 : (1u << cfg.ts_bits);
        pmax  = (cfg.p_bits  >= 16) ? 65535 : ((1u << cfg.p_bits) - 1);
        gr    = cfg.g + 1;                     // mid-window detector: see report()
        d = cfg.single_choice ? 1 : cfg.ways;
        M = (uint32_t)(memory_bytes / bucket_bytes());
        if (M < (uint32_t)d) M = d;
        M -= M % d; half = M / d;          // half = buckets per sub-array = number of groups
        st.init(M);
        for (int i = 0; i < d; i++) hs.push_back(mix32(0x1234567u + i * 0x9E3779B9u, cfg.seed));
    }

    int bucket_bytes() const { return st.bytes(); }
    uint32_t buckets() const { return M; }
    int hand_steps(uint64_t N) const {
        if (cfg.hand_steps > 0) return cfg.hand_steps;
        const uint64_t s = ((uint64_t)M * cfg.k_hand + N - 1) / N;
        return (int)(s < 1 ? 1 : s);
    }
    double sweep_windows(uint64_t N) const {
        return (double)M * cfg.k_hand / ((double)N * hand_steps(N));
    }
    uint64_t promotions = 0, installs = 0, resurrections = 0;
    int ts_span() const { return gr + (int)(M * cfg.k_hand) ; }   // packets; checked by caller

    template <class Sink>
    void run(const uint32_t* pairs, uint64_t total, uint64_t N, Sink&& emit) {
        const int L = cfg.L;
        const int nsteps = hand_steps(N);
        uint32_t hand_ctr = 0;
        uint32_t c = 0; uint64_t boundary = N;          // window index without a divide
        uint16_t cw = 0;
        for (uint64_t q = 0; q < total; q++) {
            if (q == boundary) {
                if (cfg.scan) sweep(c, cw, L, emit);   // window c has just closed
                c++; boundary += N; cw = (uint16_t)(c & (tsmod - 1));
            }
            const uint32_t id = flow_id(pairs[2 * q], pairs[2 * q + 1]);

            uint32_t idx[8];
            if (cfg.grouped) {
                const uint32_t base = (mix32(id, hs[0]) % half) * (uint32_t)d;
                for (int i = 0; i < d; i++) idx[i] = base + (uint32_t)i;
            } else {
                for (int i = 0; i < d; i++) idx[i] = (uint32_t)i * half + mix32(id, hs[i]) % half;
            }

            Bucket* x = nullptr;
            for (int i = 0; i < d && !x; i++) { Bucket& b = st.at(idx[i]); if (b.P && b.id == id) x = &b; }

            if (x) {
                if (!x->rep && x->P >= (uint16_t)L && sil(*x, cw) >= gr) emit(x->id, c, 0, x->P);
                if (x->rep) { x->rep = 0; resurrections++; }   // vanished flow came back
                if (x->lw != cw) {
                    if (x->P < pmax) x->P++;
                    if (x->P == (uint16_t)L) promotions++;
                    x->lw = cw;
                }
            } else {
                uint32_t best = idx[0];
                if (cfg.harvest_first) {               // lowest retention value wins
                    double rb = score(st.at(best), cw);
                    for (int i = 1; i < d; i++) {
                        const double r = score(st.at(idx[i]), cw);
                        if (r < rb || (r == rb && sil(st.at(idx[i]), cw) > sil(st.at(best), cw))) { rb = r; best = idx[i]; }
                    }
                } else {                               // Lasso-LL's rule: lowest persistence
                    for (int i = 1; i < d; i++) if (st.at(idx[i]).P < st.at(best).P) best = idx[i];
                }
                place(st.at(best), id, cw, c, L, emit);
            }

            if (cfg.hand && !cfg.scan && ++hand_ctr >= (uint32_t)cfg.k_hand) {
                hand_ctr = 0;
                int hstep = nsteps;
                do {
                Bucket& b = st.at(ptr);
                if (b.P && !b.rep && sil(b, cw) >= gr) {
                    if (b.P >= L) {                      // vanished: the release is the report
                        emit(b.id, c, 1, b.P);
                        if (cfg.ghost) b.rep = 1; else { b.P = 0; b.id = 0; b.lw = 0; }
                    } else {
                        if (cfg.hand_decay &&
                            rng.unit() * (double)b.P < cfg.decay_k * (double)gr) {
                            if (--b.P == 0) { b.id = 0; b.lw = 0; }
                            else b.lw = cw;
                        } else b.lw = cw;
                    }
                }
                ptr = (ptr + 1) % M;
                st.flush();
                } while (--hstep);
            }
            st.flush();
        }
        if (cfg.scan)
            for (uint32_t w = c; w < (uint32_t)cfg.W; w++)
                sweep(w, (uint16_t)(w & (tsmod - 1)), L, emit);
    }

    template <class Sink>
    void sweep(uint32_t c, uint16_t cw, int L, Sink& emit) {
        for (uint32_t i = 0; i < M; i++) {
            Bucket& b = st.at(i);
            if (b.P && !b.rep && sil(b, cw) >= cfg.g) {
                if (b.P >= L) {
                    emit(b.id, c, 1, b.P);
                    if (cfg.ghost) b.rep = 1; else { b.P = 0; b.id = 0; b.lw = 0; }
                } else {
                    if (cfg.hand_decay &&
                        rng.unit() * (double)b.P < cfg.decay_k * (double)gr) {
                        if (--b.P == 0) { b.id = 0; b.lw = 0; }
                        else b.lw = cw;
                    } else b.lw = cw;
                }
            }
            st.flush();
        }
    }

private:
    LETHE_INLINE int sil(const Bucket& b, uint16_t cw) const { return (int)((cw - b.lw) & (tsmod - 1)); }

    LETHE_INLINE St state(const Bucket& b, uint16_t cw, int& s) const {
        if (b.P == 0) { s = 0; return FREE; }
        s = sil(b, cw);
        if (b.rep) return GHOST;          // already reported: replaceable, identity kept
        if (b.P >= cfg.L) {
            if (s >= gr) return RIPE;                              // owes a report
            if (s >= 1) return cfg.ripening_protection ? RIPENING : STALE;
            return ACTIVE;
        }
        return CAND;
    }
    LETHE_INLINE double score(const Bucket& b, uint16_t cw) const {
        int s; const St st_ = state(b, cw, s);
        switch (st_) {
            case FREE: case GHOST: case RIPE: return 0.0;
            case STALE:           return (double)cfg.L;   // protection falls to a candidate's
            case RIPENING:        return 1e300;
            case ACTIVE:          return cfg.age_decay ? (double)cfg.L * cfg.L / (double)b.P : 1e300;
            default:              return (double)b.P;          // CAND
        }
    }

    template <class Sink>
    LETHE_INLINE void place(Bucket& y, uint32_t id, uint16_t cw, uint32_t c, int L, Sink& emit) {
        int s; const St st = state(y, cw, s);
        switch (st) {
            case FREE: case GHOST:
                install(y, id, cw); return;
            case RIPE:                                   // eviction is the report
                if (y.P >= L) emit(y.id, c, 0, y.P);
                install(y, id, cw); return;
            case STALE: {                                // --no-ripening-protection:
                double p;
                switch (cfg.evict_coin) {
                    case 1: {                            // Pandora: 1/(count*(50-cold)+1)
                        const int v = (50 - s < 1) ? 1 : 50 - s;
                        p = 1.0 / (1.0 + (double)y.P * (double)v); break;
                    }
                    case 2:                              // Stable: 1/(count*stable+1)
                        p = 1.0 / (1.0 + (double)y.P * (double)y.P); break;
                    case 3: p = 0.0; break;              // never condemns above threshold
                    default: p = 1.0 / (1.0 + (double)L); break;
                }
                if (p > 0.0 && rng.unit() < p) install(y, id, cw);
                return;
            }
            case RIPENING:                               // untouchable: it owes a detection
                return;
            case ACTIVE: {
                if (!cfg.age_decay) return;              // established and active: protected
                const double R = (double)L * L / (double)y.P;   // --age-decay ablation
                if (rng.unit() < 1.0 / (1.0 + R)) install(y, id, cw);
                return;
            }
            default:                                     // CAND: persistent-flow admission
                if (s == 0) return;                      // already seen this window
                if (s > (int)y.P) { if (--y.P == 0) install(y, id, cw); }
                return;
        }
    }
    LETHE_INLINE void install(Bucket& b, uint32_t id, uint16_t cw) {
        b.rep = 0;
        if (cfg.admit < 1.0 && rng.unit() >= cfg.admit) { b.P = 0; b.id = 0; b.lw = 0; return; }
        b.id = id; b.P = 1; b.lw = cw; installs++;
    }

    LetheCfg cfg;
    Rng rng;
    Store st;
    uint32_t M = 0, half = 0, ptr = 0, tsmod = 16, pmax = 255;
    int d = 2;
    std::vector<uint32_t> hs;
    int gr = 11;
};

using Lethe  = LetheT<PlainStore>;
using LetheB = LetheT<PackedStore>;
