// One method, one configuration, one pass. Scores against the ground truth
// in process and prints a single CSV row.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include "hash.h"
#include "lethe.h"
#include "exact.h"
#include "onoff.h"
#include "bloomdiff.h"
#include "pandora.h"
#include "stable.h"
#include "hypersistent.h"

struct GT {                                   // ground-truth vanish events, per flow
    std::unordered_map<uint32_t, std::vector<int>> ev;
    std::unordered_map<uint32_t, std::vector<int>> pv;   // true persistence at t_last
    std::unordered_map<uint32_t, std::vector<uint8_t>> used;
    size_t n = 0;
    void load(const char* p) {
        FILE* f = fopen(p, "r"); if (!f) { fprintf(stderr, "no gt %s\n", p); exit(1); }
        char line[128]; fgets(line, sizeof line, f);
        uint32_t id; int tl, P;
        std::unordered_map<uint32_t, std::vector<std::pair<int,int>>> tmp;
        while (fscanf(f, "%u,%d,%d\n", &id, &tl, &P) == 3) { tmp[id].push_back({tl, P}); n++; }
        fclose(f);
        for (auto& kv : tmp) {
            std::sort(kv.second.begin(), kv.second.end());
            auto& e = ev[kv.first]; auto& q = pv[kv.first];
            for (auto& z : kv.second) { e.push_back(z.first); q.push_back(z.second); }
            used[kv.first].assign(kv.second.size(), 0);
        }
    }
    // A declaration at t_d matches the most recent unmatched event in [t_d-G, t_d-1].
    // Returns the latency and, through truep, the true persistence of the matched event.
    int match(uint32_t id, int td, int G, int* truep) {
        auto it = ev.find(id); if (it == ev.end()) return -1;
        auto& v = it->second; auto& u = used[id];
        for (int i = (int)v.size() - 1; i >= 0; i--) {
            if (v[i] >= td) continue;
            if (v[i] < td - G) break;
            if (!u[i]) { u[i] = 1; *truep = pv[id][i]; return td - v[i]; }
        }
        return -1;
    }
};

static std::vector<uint32_t> load_pairs(const char* p, uint64_t& total) {
    FILE* f = fopen(p, "rb"); if (!f) { fprintf(stderr, "no trace %s\n", p); exit(1); }
    fseeko(f, 0, SEEK_END); uint64_t b = ftello(f); fseeko(f, 0, SEEK_SET);
    total = b / 8;
    std::vector<uint32_t> v(total * 2);
    if (fread(v.data(), 8, total, f) != total) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f); return v;
}

static const char* arg(int c, char** v, const char* k, const char* d) {
    for (int i = 1; i + 1 < c; i++) if (!strcmp(v[i], k)) return v[i + 1];
    return d;
}
static bool flag(int c, char** v, const char* k) {
    for (int i = 1; i < c; i++) if (!strcmp(v[i], k)) return true;
    return false;
}

int main(int argc, char** argv) {
    const char* trace  = arg(argc, argv, "--trace", "");
    const char* gtpath = arg(argc, argv, "--gt", "");
    const char* method = arg(argc, argv, "--method", "lethe");
    {   static const char* known[] = { "lethe", "letheb", "pandora", "stable",
                                       "hyper", "bloom", "exact", "onoff", 0 };
        bool ok = false;
        for (int i = 0; known[i]; i++) if (!strcmp(method, known[i])) ok = true;
        if (!ok) { fprintf(stderr, "unknown --method %s\n", method); return 1; } }
    const char* tag    = arg(argc, argv, "--tag", "");
    const char* declout= arg(argc, argv, "--decl-out", "");
    const uint64_t mem = strtoull(arg(argc, argv, "--mem", "65536"), nullptr, 10);
    const int W = atoi(arg(argc, argv, "--W", "1600"));
    const int L = atoi(arg(argc, argv, "--L", "10"));
    const int g = atoi(arg(argc, argv, "--g", "10"));
    const int G = atoi(arg(argc, argv, "--G", "10"));
    const int MATCH = atoi(arg(argc, argv, "--match", "30"));
    const uint32_t seed = (uint32_t)atoi(arg(argc, argv, "--seed", "1"));
    const double bfsplit = atof(arg(argc, argv, "--bf-split", "0.5"));
    const double bkt_override = atof(arg(argc, argv, "--bkt-bytes", "0"));
    const bool probe_first = !flag(argc, argv, "--no-probe-first");

    uint64_t total; std::vector<uint32_t> pairs = load_pairs(trace, total);
    const uint64_t N = (total + W - 1) / W;
    GT gt; gt.load(gtpath);

    size_t declared = 0, matched = 0;
    std::vector<int> lat; lat.reserve(1 << 20);
    uint64_t src_cnt[2] = {0, 0};
    FILE* fd = declout[0] ? fopen(declout, "w") : nullptr;
    if (fd) fprintf(fd, "id,t_declare,source,p_hat\n");

    double sum_abs = 0.0, sum_rel = 0.0;
    auto emit = [&](uint32_t id, int td, int src, int phat) {
        declared++; src_cnt[src & 1]++;
        if (fd) fprintf(fd, "%u,%d,%d,%d\n", id, td, src, phat);
        int tp = 0;
        int d = gt.match(id, td, MATCH, &tp);
        if (d >= 0) {
            matched++; lat.push_back(d);
            const double e = fabs((double)phat - (double)tp);   // persistence estimation error
            sum_abs += e; if (tp > 0) sum_rel += e / (double)tp;
        }
    };

    uint32_t nbuckets = 0; double bbytes = 0; uint32_t peak = 0; double sweep = 0;
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!strcmp(method, "exact")) {
        ExactTable e(L, g, W); e.run(pairs.data(), total, N, emit); peak = e.peak_entries();
    } else if (!strcmp(method, "onoff")) {
        OnOffScan s(mem, L, g, W, seed); nbuckets = s.length(); bbytes = OnOffScan::BYTES_PER_BUCKET;
        s.run(pairs.data(), total, N, emit);
    } else if (!strcmp(method, "pandora")) {
        PandoraScan s(mem, L, g, W, seed, flag(argc, argv, "--ghost"), probe_first, bkt_override);
        nbuckets = s.buckets(); bbytes = s.bucket_bytes();
        s.run(pairs.data(), total, N, emit);
    } else if (!strcmp(method, "stable")) {
        StableScan s(mem, L, g, W, seed, probe_first, bkt_override);
        nbuckets = s.buckets(); bbytes = s.bucket_bytes();
        s.run(pairs.data(), total, N, emit);
    } else if (!strcmp(method, "hyper")) {
        // Every baseline frees the bucket when it reports; keeping the key
        // through one (the ghost) is opt-in, as it is for Pandora.
        const bool ghost = flag(argc, argv, "--ghost") && !flag(argc, argv, "--no-ghost");
        HyperScan s(mem, L, g, W, seed, ghost, bkt_override,
                    atof(arg(argc, argv, "--hy-ratio", "0")), atof(arg(argc, argv, "--hy-l1", "0")),
                    atoi(arg(argc, argv, "--hy-cache", "-1")),
                    atoi(arg(argc, argv, "--hy-d1", "0")), atoi(arg(argc, argv, "--hy-d2", "0")));
        nbuckets = s.buckets(); bbytes = s.bucket_bytes();
        s.run(pairs.data(), total, N, emit);
    } else if (!strcmp(method, "bloom")) {
        BloomDiff s(mem, L, g, seed, bfsplit, probe_first, bkt_override);
        nbuckets = s.buckets(); bbytes = s.bucket_bytes();
        s.run(pairs.data(), total, N, emit);
    } else {
        LetheCfg c; c.L = L; c.g = g; c.seed = seed;
        c.ts_bits = atoi(arg(argc, argv, "--ts-bits", "9"));   // a window index at W=400
        c.p_bits  = atoi(arg(argc, argv, "--p-bits", "9"));    // counts to W without saturating
        c.k_hand  = atoi(arg(argc, argv, "--k-hand", "1"));
        c.hand_steps = atoi(arg(argc, argv, "--hand-steps", "1"));
        c.decay_k = atof(arg(argc, argv, "--decay-k", "0.5"));
        c.ways    = atoi(arg(argc, argv, "--ways", "2"));
        c.grouped =  flag(argc, argv, "--grouped");
        c.admit   = atof(arg(argc, argv, "--admit", "1"));
        c.age_decay           =  flag(argc, argv, "--age-decay");
        c.ripening_protection = !flag(argc, argv, "--no-ripening-protection");
        { const char* ec = arg(argc, argv, "--evict-coin", "lethe");
          c.evict_coin = !strcmp(ec, "pandora") ? 1 : !strcmp(ec, "stable") ? 2
                       : !strcmp(ec, "none")    ? 3 : 0; }
        c.harvest_first       = !flag(argc, argv, "--no-harvest-first");
        c.hand                = !flag(argc, argv, "--no-hand");
        c.scan                =  flag(argc, argv, "--scan");
        c.W                   =  W;
        c.hand_decay          = !flag(argc, argv, "--hand-no-decay");
        c.single_choice       =  flag(argc, argv, "--single-choice");
        c.ghost               = !flag(argc, argv, "--no-ghost");
        if (!strcmp(method, "letheb")) {
#ifndef LETHE_NO_PACKED
            LetheB s(mem, c); nbuckets = s.buckets(); bbytes = s.bucket_bytes();
            sweep = s.sweep_windows(N);
            s.run(pairs.data(), total, N, emit);
            fprintf(stderr, "promotions=%llu installs=%llu\n", (unsigned long long)s.promotions, (unsigned long long)s.installs);
#else
            fprintf(stderr, "this build has no Lethe-B\n"); return 2;
#endif
        } else {
#ifndef LETHE_NO_PLAIN
            Lethe s(mem, c); nbuckets = s.buckets(); bbytes = s.bucket_bytes();
            sweep = s.sweep_windows(N);
            s.run(pairs.data(), total, N, emit);
            fprintf(stderr, "promotions=%llu installs=%llu\n", (unsigned long long)s.promotions, (unsigned long long)s.installs);
#else
            fprintf(stderr, "this build has no default Lethe\n"); return 2;
#endif
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    if (fd) fclose(fd);

    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::sort(lat.begin(), lat.end());
    const double prec = declared ? (double)matched / declared : 0.0;
    const double rec  = gt.n ? (double)matched / gt.n : 0.0;
    const double f1   = (prec + rec) ? 2 * prec * rec / (prec + rec) : 0.0;
    double lmean = 0; for (int x : lat) lmean += x; if (!lat.empty()) lmean /= lat.size();
    const int p99 = lat.empty() ? 0 : lat[(size_t)(0.99 * (lat.size() - 1))];
    const int p50 = lat.empty() ? 0 : lat[lat.size() / 2];

    const double aae = matched ? sum_abs / (double)matched : 0.0;
    const double are = matched ? sum_rel / (double)matched : 0.0;
    printf("%s,%s,%llu,%d,%d,%d,%u,%zu,%zu,%zu,%.6f,%.6f,%.6f,%.4f,%d,%d,%.3f,%u,%.2f,%llu,%llu,%.4f,%.4f,%.6f\n",
           tag, method, (unsigned long long)mem, L, g, G, seed,
           gt.n, declared, matched, prec, rec, f1, lmean, p50, p99,
           total / sec / 1e6, nbuckets, bbytes,
           (unsigned long long)src_cnt[0], (unsigned long long)src_cnt[1], sweep, aae, are);
    if (peak) fprintf(stderr, "peak_entries=%u bytes=%.0f\n", peak, peak * 12.0);
    return 0;
}
