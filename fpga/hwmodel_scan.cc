#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: hwmodel_scan pairs addr_w W L g stim expect [npkts]\n");
        return 1;
    }
    const char* trace = argv[1];
    const int ADDR_W = atoi(argv[2]);
    const int W = atoi(argv[3]);
    const int L = atoi(argv[4]);
    const int g = atoi(argv[5]);
    const int GR = g + 1;
    const int TS_W = 16, P_W = 16;
    const uint32_t TSMOD = 1u << TS_W;
    const uint32_t PMAX = (1u << P_W) - 1;
    const int KNUM = 9, KDEN = 2;              // decay_k*(g+1) = 4.5
    const uint64_t LIMIT = (argc > 8) ? strtoull(argv[8], nullptr, 10) : 0;

    FILE* f = fopen(trace, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", trace); return 1; }
    fseek(f, 0, SEEK_END); long bytes = ftell(f); fseek(f, 0, SEEK_SET);
    uint64_t total = (uint64_t)bytes / 8;
    if (LIMIT && total > LIMIT) total = LIMIT;
    std::vector<uint32_t> pairs(total * 2);
    if (fread(pairs.data(), 8, total, f) != total) {
        fprintf(stderr, "short read\n"); return 1;
    }
    fclose(f);

    const uint32_t DEPTH = 1u << ADDR_W;
    struct B { uint64_t id; uint32_t P, lw, rep; };
    std::vector<B> m0(DEPTH, B{0, 0, 0, 0}), m1(DEPTH, B{0, 0, 0, 0});

    FILE* fs = fopen(argv[6], "w");
    FILE* fe = fopen(argv[7], "w");

    const uint64_t N = (total + W - 1) / W;    // packets per window
    uint32_t hptr = 0, last_cw = 0, lfsr = 0xACE12345u;
    bool sweeping = false;
    int c = 0; uint32_t cw = 0; uint64_t boundary = N;
    long n_rep = 0;

    for (uint64_t q = 0; q < total; q++) {
        if (q == boundary) { c++; boundary += N; cw = (uint32_t)c & (TSMOD - 1); }

        // the flow key the core carries whole: the pair, as it arrives
        const uint64_t id = ((uint64_t)pairs[2 * q] << 32) | pairs[2 * q + 1];
        fprintf(fs, "%016llx %04x\n", (unsigned long long)id, cw);

        // win_edge: the first packet of a new window starts the sweep
        if (cw != last_cw) { last_cw = cw; sweeping = true; hptr = 0; }

        // ---- hashes: the key is folded to a word, then two multiply-shifts
        const uint32_t idf = (uint32_t)(id >> 32) ^ (uint32_t)id;
        const uint32_t ha0 = (uint32_t)(idf * 0x2545F491u) >> (32 - ADDR_W);
        const uint32_t ha1 = (uint32_t)(idf * 0x9E3779B1u) >> (32 - ADDR_W);

        // ---- the scan reads its bucket from the same pre-packet state
        const uint32_t hway = (hptr >> ADDR_W) & 1u;
        const uint32_t haddr = hptr & (DEPTH - 1);
        const B hqb = hway ? m1[haddr] : m0[haddr];

        // ---- arrival decode
        B& b0 = m0[ha0];
        B& b1 = m1[ha1];
        const uint32_t s0 = (cw - b0.lw) & (TSMOD - 1);
        const uint32_t s1 = (cw - b1.lw) & (TSMOD - 1);
        const bool nz0 = b0.P != 0, nz1 = b1.P != 0;
        const bool es0 = b0.P >= (uint32_t)L, es1 = b1.P >= (uint32_t)L;
        const bool rp0 = nz0 && !b0.rep && es0 && s0 >= (uint32_t)GR;
        const bool rp1 = nz1 && !b1.rep && es1 && s1 >= (uint32_t)GR;
        const uint32_t RMAX = (1u << (P_W + 1)) - 1;
        const uint32_t r0 = (!nz0 || b0.rep || rp0) ? 0u : (es0 ? RMAX : b0.P);
        const uint32_t r1 = (!nz1 || b1.rep || rp1) ? 0u : (es1 ? RMAX : b1.P);

        const bool hit0 = nz0 && b0.id == id;
        const bool hit1 = nz1 && b1.id == id && !hit0;
        const bool hit = hit0 || hit1;
        const bool pick1 = (r1 < r0) || (r1 == r0 && s1 > s0);

        B& hb = hit0 ? b0 : b1;
        const uint32_t hs = hit0 ? s0 : s1;
        const bool hrp = hit0 ? rp0 : rp1;

        B& vb = pick1 ? b1 : b0;
        const bool vrp = pick1 ? rp1 : rp0;
        const bool vfr = pick1 ? !nz1 : !nz0;
        const bool vre = pick1 ? (b1.rep != 0) : (b0.rep != 0);
        const bool ves = pick1 ? es1 : es0;
        const uint32_t vs = pick1 ? s1 : s0;

        const bool vdec = !vfr && !vre && !ves && vs != 0 && vs > vb.P;
        const bool vlast = vdec && vb.P == 1;
        const bool do_inst = !hit && (vfr || vre || vrp);
        const bool do_dec = !hit && !do_inst && vdec && !vlast;
        const bool do_di = !hit && !do_inst && vlast;

        // a returning packet that finds its own bucket due, or a miss that
        // takes a due one, delivers that bucket's report
        if (hit && hrp)        { fprintf(fe, "%016llx %u\n", (unsigned long long)hb.id, hb.P); n_rep++; }
        else if (!hit && vrp)  { fprintf(fe, "%016llx %u\n", (unsigned long long)vb.id, vb.P); n_rep++; }

        if (hit) {
            const uint32_t pn = (hs != 0 && hb.P != PMAX) ? hb.P + 1 : hb.P;
            hb.P = pn; hb.lw = cw; hb.rep = 0;
        } else if (do_inst || do_di) {
            vb.id = id; vb.P = 1; vb.lw = cw; vb.rep = 0;
        } else if (do_dec) {
            vb.P -= 1;                          // the last-seen field is left alone
        }

        // ---- scan step, on the bucket read above
        const bool coin = ((uint64_t)lfsr * hqb.P * KDEN) < ((uint64_t)KNUM << 32);
        lfsr = (lfsr << 1) | (((lfsr >> 31) ^ (lfsr >> 21) ^ (lfsr >> 1) ^ lfsr) & 1u);
        if (sweeping) {
            const uint32_t hsil = (cw - hqb.lw) & (TSMOD - 1);
            const bool hdue = hqb.P != 0 && !hqb.rep && hsil >= (uint32_t)GR;
            if (hdue) {
                B w = hqb;
                if (hqb.P >= (uint32_t)L) {     // release = report
                    fprintf(fe, "%016llx %u\n", (unsigned long long)hqb.id, hqb.P); n_rep++;
                    w.rep = 1;
                } else if (coin) {
                    if (hqb.P == 1) w = B{0, 0, 0, 0};
                    else { w.P = hqb.P - 1; w.lw = cw; }
                } else {
                    w.lw = cw;
                }
                if (hway) m1[haddr] = w; else m0[haddr] = w;
            }
            if (hptr == (DEPTH * 2 - 1)) sweeping = false;
            hptr = (hptr + 1) & (DEPTH * 2 - 1);
        }
    }
    fclose(fs); fclose(fe);

    // the whole table at the end of the replay, for a state comparison
    FILE* fd2 = fopen("model_state.txt", "w");
    for (uint32_t w = 0; w < 2; w++)
        for (uint32_t a = 0; a < DEPTH; a++) {
            const B& b = w ? m1[a] : m0[a];
            fprintf(fd2, "%u %u %016llx %u %u %u\n", w, a,
                    (unsigned long long)b.id, b.P, b.lw, b.rep);
        }
    fclose(fd2);
    fprintf(stderr, "packets %llu  windows %d  reports %ld\n",
            (unsigned long long)total, c + 1, n_rep);
    return 0;
}
