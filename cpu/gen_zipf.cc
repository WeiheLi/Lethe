// gen_zipf: synthetic traces whose flow popularity follows a Zipf law.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <numeric>

static inline uint64_t xs(uint64_t& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage: gen_zipf <n_flows> <n_packets> <skew> <seed> <out>\n"); return 1; }
    const uint32_t n = (uint32_t)atoll(argv[1]);
    const uint64_t T = (uint64_t)atoll(argv[2]);
    const double sk = atof(argv[3]);
    uint64_t seed = (uint64_t)atoll(argv[4]) * 0x9E3779B97F4A7C15ull + 1;
    FILE* fo = fopen(argv[5], "wb");
    if (!fo) { fprintf(stderr, "cannot write %s\n", argv[5]); return 1; }

    // Zipf weights over ranks 1..n, normalised to n * p_i for the alias tables.
    std::vector<double> p(n);
    double Z = 0.0;
    for (uint32_t i = 0; i < n; i++) { p[i] = 1.0 / std::pow((double)(i + 1), sk); Z += p[i]; }
    for (uint32_t i = 0; i < n; i++) p[i] = p[i] * n / Z;

    std::vector<uint32_t> alias(n); std::vector<double> prob(n);
    std::vector<uint32_t> small, large; small.reserve(n); large.reserve(n);
    for (uint32_t i = 0; i < n; i++) (p[i] < 1.0 ? small : large).push_back(i);
    while (!small.empty() && !large.empty()) {
        const uint32_t s = small.back(); small.pop_back();
        const uint32_t l = large.back(); large.pop_back();
        prob[s] = p[s]; alias[s] = l;
        p[l] = (p[l] + p[s]) - 1.0;
        (p[l] < 1.0 ? small : large).push_back(l);
    }
    for (uint32_t i : large) prob[i] = 1.0;
    for (uint32_t i : small) prob[i] = 1.0;

    const size_t CH = 1 << 20;
    std::vector<uint32_t> out(2 * CH);
    uint64_t written = 0;
    while (written < T) {
        const size_t k = (size_t)std::min<uint64_t>(CH, T - written);
        for (size_t i = 0; i < k; i++) {
            const uint64_t r1 = xs(seed);
            uint32_t col = (uint32_t)(r1 % n);
            const double u = (double)((xs(seed) >> 11)) * (1.0 / 9007199254740992.0);
            const uint32_t rank = (u < prob[col]) ? col : alias[col];
            // spread ranks over the address space so hashing is not degenerate
            const uint32_t src = rank * 2654435761u + 0x9E3779B9u;
            const uint32_t dst = rank * 40503u + 0x85EBCA6Bu;
            out[2 * i] = src; out[2 * i + 1] = dst;
        }
        fwrite(out.data(), 8, k, fo);
        written += k;
    }
    fclose(fo);
    fprintf(stderr, "%s: n=%u T=%llu skew=%.2f\n", argv[5], n, (unsigned long long)T, sk);
    return 0;
}
