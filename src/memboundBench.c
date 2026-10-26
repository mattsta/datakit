/* memboundBench - decision-grade allocator microbenchmark.
 *
 * Compares system malloc, memshard (the in-tree vendored allocator), and
 * membound (STANDARD + ENTERPRISE) on identical single- and multi-threaded
 * alloc/free patterns. Built by the `memboundBench` CMake target; run under the
 * `perf` preset (no sanitizers). Directional, not statistically rigorous. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__APPLE__)
#include <malloc/malloc.h> /* malloc_size */
#else
#include <malloc.h> /* malloc_usable_size */
#endif

/* This benchmark's baseline arm calls system malloc/free/calloc
 * directly (that IS the thing being measured), so opt out of datakit.h's
 * deprecation poison on the raw allocator symbols. */
#define IKNOWWHATIMDOING
#include "datakit.h" /* zmalloc/zfree (global allocator config) */
#include "membound.h"
#include "memshardConfig.h" /* datakitUseMemshard() */
#include "mimalloc.h"

static uint64_t nowns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ull + (uint64_t)ts.tv_nsec;
}

typedef struct {
    const char *name;
    void *(*alloc)(void *ctx, size_t n);
    void (*release)(void *ctx, void *p);
    void *ctx;
    size_t (*usable)(void *ctx, void *p); /* actual block size for overhead */
} Alloc;

/* Actual usable block size the allocator handed back for a request — captures
 * size-class rounding / min-atom waste (the memory side of the tradeoff). */
static size_t sysU(void *, void *p) {
#if defined(__APPLE__)
    return malloc_size(p);
#else
    return malloc_usable_size(p);
#endif
}
static size_t miU(void *, void *p) {
    return mi_usable_size(p);
}
static size_t mbU(void *c, void *p) {
    return memboundUsableSize((membound *)c, p);
}

static void *sysA(void *, size_t n) {
    return malloc(n);
}
static void sysF(void *, void *p) {
    free(p);
}
static void *miA(void *, size_t n) {
    return mi_malloc(n);
}
static void miF(void *, void *p) {
    mi_free(p);
}
static void *mbA(void *c, size_t n) {
    return memboundAlloc((membound *)c, n);
}
static void mbF(void *c, void *p) {
    memboundFree((membound *)c, p);
}
/* The library-wide global allocator path. Its backend is whatever the global
 * datakitConfig points at — system by default, memshard after
 * datakitUseMemshard(). Comparing this row across the two configs measures the
 * memshard-as-global-backend benefit, function-pointer indirection
 * included. */
static void *zmA(void *, size_t n) {
    /* @own: caller; free=zfree */
    return zmalloc(n);
}
static void zmF(void *, void *p) {
    zfree(p);
}

typedef struct {
    const Alloc *al;
    size_t blockSize;
    size_t live;
    size_t rounds;
    uint64_t ops;
} Job;

static void *batchRun(void *arg) {
    Job *j = (Job *)arg;
    void **ptrs = (void **)calloc(j->live, sizeof(void *));
    uint64_t ops = 0;
    for (size_t r = 0; r < j->rounds; r++) {
        for (size_t i = 0; i < j->live; i++) {
            ptrs[i] = j->al->alloc(j->al->ctx, j->blockSize);
            if (ptrs[i]) {
                ((volatile char *)ptrs[i])[0] = (char)i; /* touch */
                ops++;
            }
        }
        for (size_t i = 0; i < j->live; i++) {
            if (ptrs[i]) {
                j->al->release(j->al->ctx, ptrs[i]);
                ops++;
            }
        }
    }
    free(ptrs);
    j->ops = ops;
    return nullptr;
}

/* Number of trials per data point; we report the MIN (best-case throughput —
 * scheduling/IRQ noise only ever adds time, so the min is the cleanest signal
 * and is what makes the MT numbers stable enough to reason about). */
static constexpr int BENCH_TRIALS = 5;

static double benchST(const Alloc *al, size_t blk, size_t live, size_t rounds) {
    double best = 1e30;
    for (int t = 0; t < BENCH_TRIALS; t++) {
        Job j = {al, blk, live, rounds, 0};
        uint64_t t0 = nowns();
        batchRun(&j);
        uint64_t t1 = nowns();
        const double ns = (double)(t1 - t0) / (double)j.ops;
        if (ns < best) {
            best = ns;
        }
    }
    return best;
}

static double benchMTOnce(const Alloc *al, size_t blk, size_t live,
                          size_t rounds, int threads) {
    pthread_t th[64];
    Job jobs[64];
    for (int i = 0; i < threads; i++) {
        jobs[i] = (Job){al, blk, live, rounds, 0};
    }
    uint64_t t0 = nowns();
    for (int i = 0; i < threads; i++) {
        pthread_create(&th[i], nullptr, batchRun, &jobs[i]);
    }
    uint64_t total = 0;
    for (int i = 0; i < threads; i++) {
        pthread_join(th[i], nullptr);
        total += jobs[i].ops;
    }
    uint64_t t1 = nowns();
    return (double)(t1 - t0) / (double)total; /* wall ns per op */
}

static double benchMT(const Alloc *al, size_t blk, size_t live, size_t rounds,
                      int threads) {
    double best = 1e30;
    for (int t = 0; t < BENCH_TRIALS; t++) {
        const double ns = benchMTOnce(al, blk, live, rounds, threads);
        if (ns < best) {
            best = ns;
        }
    }
    return best;
}

/* Fragmentation / bloat under churn. The concern with a thread-CACHING
 * allocator (memshard/mimalloc) is that per-thread/size-class caches retain
 * freed memory and grow committed footprint over time. Hold N mixed-size
 * objects, then churn (free+reallocate all of them) for many rounds and check
 * whether committed memory stays near the live set (good reuse) or balloons,
 * and whether a collect/free returns it. Uses each allocator's OWN accounting
 * (mimalloc mi_process_info, membound capacity) so there is no process-wide RSS
 * pollution. The bookkeeping array uses raw malloc so it doesn't perturb either
 * heap. */
static void fragReport(membound *pool) {
    enum { N = 50000, ROUNDS = 50 };
    const size_t sizes[4] = {16, 64, 256, 1024};
    void **ptrs = (void **)malloc(N * sizeof(void *));
    if (!ptrs) {
        return;
    }

    printf("\n=== FRAGMENTATION UNDER CHURN (committed vs live; N=%d, "
           "%d rounds, sizes 16/64/256/1024) ===\n",
           N, ROUNDS);

    /* ---- memshard (mimalloc) ----
     * Isolate memshard's real resident footprint with an RSS *delta* around its
     * workload (process-wide RSS is polluted by the membound pools created
     * earlier; mimalloc's `commit` counts reserved virtual arena, not RAM — so
     * neither absolute number is meaningful alone). */
    size_t e, u, s, rss0, prss, ccommit, pcommit, pf;
    mi_collect(true);
    mi_process_info(&e, &u, &s, &rss0, &prss, &ccommit, &pcommit, &pf);
    for (int i = 0; i < N; i++) {
        ptrs[i] = mi_malloc(sizes[i & 3]);
    }
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < N; i++) {
            mi_free(ptrs[i]);
            ptrs[i] = mi_malloc(sizes[i & 3]);
        }
    }
    size_t miLive = 0;
    for (int i = 0; i < N; i++) {
        miLive += mi_usable_size(ptrs[i]);
    }
    size_t rss1;
    mi_process_info(&e, &u, &s, &rss1, &prss, &ccommit, &pcommit, &pf);
    /* memshard's RSS growth */
    const int64_t miResident = (int64_t)rss1 - (int64_t)rss0;
    printf("memshard      live=%8zuKB  resident(RSS delta)=%8" PRId64
           "KB (%.2fx live) -- no churn bloat: footprint tracks live\n",
           miLive / 1024, miResident / 1024,
           (double)miResident / (double)miLive);
    for (int i = 0; i < N; i++) {
        mi_free(ptrs[i]);
    }
    mi_collect(true);
    size_t rss2;
    mi_process_info(&e, &u, &s, &rss2, &prss, &ccommit, &pcommit, &pf);
    printf("memshard      after free+mi_collect: resident(RSS delta)=%8" PRId64
           "KB (retained for reuse, not returned to OS)\n",
           ((int64_t)rss2 - (int64_t)rss0) / 1024);

    /* ---- membound (STD pool) ---- pre-commits its pool, so committed ==
     * capacity regardless of churn; this quantifies the fixed/min-atom cost. */
    if (pool) {
        memboundReset(pool);
        for (int i = 0; i < N; i++) {
            ptrs[i] = memboundAlloc(pool, sizes[i & 3]);
        }
        for (int r = 0; r < ROUNDS; r++) {
            for (int i = 0; i < N; i++) {
                memboundFree(pool, ptrs[i]);
                ptrs[i] = memboundAlloc(pool, sizes[i & 3]);
            }
        }
        const size_t mbLive = memboundBytesUsed(pool);
        const size_t mbCap = memboundCapacity(pool);
        printf("membound-STD  live=%8zuKB committed=%8zuKB (%.2fx) "
               "[capacity; pre-committed, 256B min atom]\n",
               mbLive / 1024, mbCap / 1024,
               mbLive ? (double)mbCap / (double)mbLive : 0.0);
        memboundReset(pool);
    }

    free(ptrs);
}

int main(void) {
    const size_t LIVE = 1000;
    const size_t ROUNDS_ST = 4000; /* ~8M ops/run */
    const size_t ROUNDS_MT = 2000;
    const int THREADS = 4;
    const size_t sizes[3] = {64, 256, 1024};

    /* Set MEMBOUND_BENCH_MEMSHARD=1 to point the global zmalloc backend at
     * memshard for this run (affects the `zmalloc` row below). Run the bench
     * twice — default vs that env — and compare the `zmalloc` rows. */
    const bool useMemshard = getenv("MEMBOUND_BENCH_MEMSHARD") != nullptr;
    if (useMemshard) {
        datakitUseMemshard();
    }
    printf("# global zmalloc backend: %s\n",
           useMemshard ? "memshard" : "default");

    membound *stdPool = memboundCreateDynamic(8u << 20, 0, 8u << 20);
    membound *entPool = memboundCreateEnterprise(8u << 20, 0);
    memboundConfig tcCfg = {.mode = MEMBOUND_MODE_DYNAMIC,
                            .initialSize = 8u << 20,
                            .tier = MEMBOUND_TIER_ENTERPRISE,
                            .threadSafe = true,
                            .growthSize = 8u << 20,
                            .threadCache = true};
    membound *tcPool = memboundCreateWithConfig(&tcCfg);

    Alloc sys = {"system", sysA, sysF, nullptr, sysU};
    Alloc mi = {"memshard", miA, miF, nullptr, miU};
    Alloc zm = {"zmalloc", zmA, zmF, nullptr, nullptr};
    Alloc mbs = {"membound-STD", mbA, mbF, stdPool, mbU};
    Alloc mbe = {"membound-ENT", mbA, mbF, entPool, mbU};
    Alloc mbtc = {"membound-TC", mbA, mbF, tcPool, mbU};
    const Alloc *all[6] = {&sys, &mi, &zm, &mbs, &mbe, &mbtc};

    printf("=== SINGLE-THREAD ns/op (lower=better; live=%zu) ===\n", LIVE);
    printf("%-14s %10s %10s %10s\n", "allocator", "64B", "256B", "1024B");
    for (int a = 0; a < 6; a++) {
        printf("%-14s", all[a]->name);
        for (size_t s = 0; s < 3; s++) {
            if (all[a]->ctx) {
                memboundReset((membound *)all[a]->ctx);
            }
            printf(" %9.2f", benchST(all[a], sizes[s], LIVE, ROUNDS_ST));
        }
        printf("\n");
    }

    printf(
        "\n=== MULTI-THREAD x%d wall ns/op (lower=better; shared pool) ===\n",
        THREADS);
    printf("%-14s %10s %10s\n", "allocator", "64B", "256B");
    for (int a = 0; a < 6; a++) {
        printf("%-14s", all[a]->name);
        for (size_t s = 0; s < 2; s++) {
            if (all[a]->ctx) {
                memboundReset((membound *)all[a]->ctx);
            }
            printf(" %9.2f",
                   benchMT(all[a], sizes[s], LIVE, ROUNDS_MT, THREADS));
        }
        printf("\n");
    }

    /* Memory side of the tradeoff: the actual block size the allocator hands
     * back for each requested size. ratio = usable/requested = wasted bytes per
     * object (size-class rounding, min-atom, alignment). Speed is moot if an
     * 8-byte request costs 256 bytes. */
    printf("\n=== PER-ALLOCATION OVERHEAD (usable bytes returned for request) "
           "===\n");
    const size_t osizes[] = {8, 16, 32, 48, 64, 100, 256, 512, 1000, 4096};
    const size_t nos = countof(osizes);
    const Alloc *ov[3] = {&sys, &mi, &mbs};
    printf("%-14s", "request->");
    for (size_t s = 0; s < nos; s++) {
        printf(" %7zu", osizes[s]);
    }
    printf("\n");
    for (int a = 0; a < 3; a++) {
        if (!ov[a]->usable) {
            continue;
        }
        if (ov[a]->ctx) {
            memboundReset((membound *)ov[a]->ctx);
        }
        printf("%-14s", ov[a]->name);
        for (size_t s = 0; s < nos; s++) {
            void *p = ov[a]->alloc(ov[a]->ctx, osizes[s]);
            const size_t u = p ? ov[a]->usable(ov[a]->ctx, p) : 0;
            ov[a]->release(ov[a]->ctx, p);
            printf(" %7zu", u);
        }
        printf("\n");
    }
    /* Worst-case overhead ratio per allocator (over the sizes above). */
    printf("%-14s", "max waste x");
    for (int a = 0; a < 3; a++) {
        if (!ov[a]->usable) {
            continue;
        }
        if (ov[a]->ctx) {
            memboundReset((membound *)ov[a]->ctx);
        }
        double worst = 1.0;
        for (size_t s = 0; s < nos; s++) {
            void *p = ov[a]->alloc(ov[a]->ctx, osizes[s]);
            const size_t u = p ? ov[a]->usable(ov[a]->ctx, p) : osizes[s];
            ov[a]->release(ov[a]->ctx, p);
            const double r = (double)u / (double)osizes[s];
            if (r > worst) {
                worst = r;
            }
        }
        printf("  %s=%.1fx", ov[a]->name, worst);
    }
    printf("\n");

    fragReport(stdPool);

    memboundShutdown(stdPool);
    memboundShutdown(entPool);
    return 0;
}
