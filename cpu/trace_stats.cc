#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "hash.h"

static const uint64_t EMPTY64 = ~0ull;

struct Ent { uint32_t id; uint16_t P; uint16_t last; uint8_t used; uint8_t alive; };

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: trace_stats <pairs> <W> <L> <g> <G> <outprefix> [logbits]\n");
        return 1;
    }
    const char* path = argv[1];
    const int W = atoi(argv[2]), L = atoi(argv[3]), g = atoi(argv[4]), G = atoi(argv[5]);
    const char* pre = argv[6];
    const int LOGB = (argc > 7) ? atoi(argv[7]) : 25;

    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); uint64_t bytes = ftello(f); fseek(f, 0, SEEK_SET);
    const uint64_t total = bytes / 8;
    const uint64_t N = (total + W - 1) / W;          // packets per window
    const int Tend = W - 1;

    const size_t SLOTS = 1ull << LOGB, MASK = SLOTS - 1;
    std::vector<Ent> tab(SLOTS);
    memset(tab.data(), 0, SLOTS * sizeof(Ent));
    std::vector<uint64_t> ptab(SLOTS, EMPTY64);       // distinct (src,dst) pairs

    std::vector<int64_t> delta(W + 2, 0);
    std::vector<uint64_t> expo(W + 2, 0), stops(W + 2, 0);
    uint64_t n_pairs = 0, n_ids = 0, n_events = 0, n_over255 = 0;

    char buf[512];
    snprintf(buf, sizeof buf, "%s_gt.csv", pre);
    FILE* fgt = fopen(buf, "w"); fprintf(fgt, "id,t_last,P_at_tlast\n");

    const size_t CH = 1 << 20;
    std::vector<uint32_t> in(2 * CH);
    uint64_t q = 0; size_t got;
    while ((got = fread(in.data(), 8, CH, f)) > 0) {
        for (size_t i = 0; i < got; i++, q++) {
            const uint32_t src = in[2 * i], dst = in[2 * i + 1];
            const int t = (int)(q / N);

            uint64_t pk = ((uint64_t)src << 32) | dst;
            size_t j = (size_t)(mm3_32(&pk, 8, 0x51ed270b) & MASK);
            while (ptab[j] != EMPTY64 && ptab[j] != pk) j = (j + 1) & MASK;
            if (ptab[j] == EMPTY64) { ptab[j] = pk; n_pairs++; }

            const uint32_t id = flow_id(src, dst);
            size_t k = (size_t)(mix32(id, 0xB5297A4Du) & MASK);
            while (tab[k].used && tab[k].id != id) k = (k + 1) & MASK;
            Ent& e = tab[k];
            if (!e.used) {
                e.used = 1; e.id = id; e.P = 1; e.last = (uint16_t)t; e.alive = 0;
                n_ids++;
                if (L <= 1) { delta[t]++; e.alive = 1; }
                continue;
            }
            if (e.last == (uint16_t)t) continue;      // same window, nothing changes

            const int last = e.last, gap = t - last, a = e.P;
            if (a >= L) {
                if (last + g <= Tend) { expo[a]++; if (gap > g) stops[a]++; }
                if (gap > G && last <= Tend - G) {
                    n_events++;
                    fprintf(fgt, "%u,%d,%d\n", e.id, last, a);
                    if (e.alive) { int c = std::min(last + G + 1, W); delta[c]--; e.alive = 0; }
                }
            }
            e.P = (uint16_t)(a + 1); e.last = (uint16_t)t;
            if (e.P == L) { delta[t]++; e.alive = 1; }
            else if (e.P > L && !e.alive) { delta[t]++; e.alive = 1; }
        }
    }
    fclose(f);

    // Flush: flows silent at the end of the trace.
    std::vector<uint64_t> pdist(W + 2, 0);
    for (size_t k = 0; k < SLOTS; k++) {
        Ent& e = tab[k];
        if (!e.used) continue;
        pdist[e.P]++;
        if (e.P > 255) n_over255++;
        const int last = e.last, a = e.P;
        if (a >= L) {
            if (last + g <= Tend) { expo[a]++; stops[a]++; }
            if (last <= Tend - G) {
                n_events++;
                fprintf(fgt, "%u,%d,%d\n", e.id, last, a);
                if (e.alive) { int c = std::min(last + G + 1, W); delta[c]--; e.alive = 0; }
            }
        }
    }
    fclose(fgt);

    // Curves.
    snprintf(buf, sizeof buf, "%s_conc.csv", pre);
    FILE* fc = fopen(buf, "w"); fprintf(fc, "window,established_alive\n");
    int64_t cur = 0, peak = 0; double mean = 0;
    for (int t = 0; t < W; t++) {
        cur += delta[t];
        fprintf(fc, "%d,%lld\n", t, (long long)cur);
        peak = std::max(peak, cur); mean += (double)cur;
    }
    fclose(fc); mean /= W;

    snprintf(buf, sizeof buf, "%s_hazard.csv", pre);
    FILE* fh = fopen(buf, "w"); fprintf(fh, "age,exposures,stops,hazard\n");
    for (int a = L; a <= W; a++)
        if (expo[a]) fprintf(fh, "%d,%llu,%llu,%.6f\n", a,
                             (unsigned long long)expo[a], (unsigned long long)stops[a],
                             (double)stops[a] / (double)expo[a]);
    fclose(fh);

    snprintf(buf, sizeof buf, "%s_pdist.csv", pre);
    FILE* fp = fopen(buf, "w"); fprintf(fp, "P,flows\n");
    for (int p = 1; p <= W; p++) if (pdist[p]) fprintf(fp, "%d,%llu\n", p, (unsigned long long)pdist[p]);
    fclose(fp);

    uint64_t n_est = 0; for (int p = L; p <= W; p++) n_est += pdist[p];
    printf("trace=%s packets=%llu W=%d N=%llu L=%d g=%d G=%d\n",
           path, (unsigned long long)total, W, (unsigned long long)N, L, g, G);
    printf("distinct_pairs=%llu distinct_ids=%llu id_collision_rate=%.4f%%\n",
           (unsigned long long)n_pairs, (unsigned long long)n_ids,
           100.0 * (double)(n_pairs - n_ids) / (double)n_pairs);
    printf("established_flows_total=%llu peak_concurrent=%lld mean_concurrent=%.0f\n",
           (unsigned long long)n_est, (long long)peak, mean);
    printf("vanish_events=%llu  flows_with_P_gt_255=%llu (%.3f%%)\n",
           (unsigned long long)n_events, (unsigned long long)n_over255,
           100.0 * (double)n_over255 / (double)n_ids);
    return 0;
}
