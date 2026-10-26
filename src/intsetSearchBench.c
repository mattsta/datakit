/* intsetSearchBench - decision bench for branchless intset search.
 *
 * Question: is a branchless (+prefetch) binary search meaningfully faster than
 * the current branchy binary search used by intsetMedium/intsetFull Find, on
 * large sorted arrays? This is a SELF-CONTAINED microbench on a plain sorted
 * uint32_t array (the intset element type for the medium/full-32 tier) — it
 * does NOT touch intset internals, so there is zero correctness risk in
 * measuring. Only if branchless wins here is the risky 3-width x 2-tier
 * integration (which must preserve the exact insertion-position contract) worth
 * attempting.
 *
 * Built by the `intsetSearchBench` CMake target; run under the `perf` preset.
 */

/* A standalone program without datakit: it allocates with libc */
#define IKNOWWHATIMDOING
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t nowns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ull + (uint64_t)ts.tv_nsec;
}

/* Branchy binary search, mirroring intsetFullFind32's structure: returns the
 * matching index, or (on miss) encodes the insertion point. */
static int64_t branchyFind(const uint32_t *a, int64_t n, uint32_t v) {
    int64_t lo = 0;
    int64_t hi = n - 1;
    while (lo <= hi) {
        const int64_t mid = lo + ((hi - lo) >> 1);
        const uint32_t m = a[mid];
        if (m == v) {
            return mid;
        }
        if (m < v) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -(lo + 1); /* not found; lo = insertion position */
}

/* Branchless lower-bound with prefetch of both candidate children (the
 * Khuong-style layout). Returns the lower-bound index; caller would check
 * a[idx]==v for membership and use idx as the insertion position. */
static int64_t branchlessFind(const uint32_t *a, int64_t n, uint32_t v) {
    const uint32_t *base = a;
    int64_t len = n;
    while (len > 1) {
        const int64_t half = len >> 1;
        __builtin_prefetch(base + (half >> 1));
        __builtin_prefetch(base + half + (half >> 1));
        base += (base[half - 1] < v) ? half : 0; /* compiles to cmov */
        len -= half;
    }
    int64_t idx = (int64_t)(base - a);
    if (idx < n && a[idx] < v) {
        idx++;
    }
    return idx;
}

static void run(const char *name, int64_t n) {
    uint32_t *a = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    /* Sorted even values 0,2,4,... so ~half of random odd probes miss. */
    for (int64_t i = 0; i < n; i++) {
        a[i] = (uint32_t)(i * 2);
    }

    const int64_t M = 10'000'000; /* probes */
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    [[maybe_unused]] volatile int64_t sink = 0;

    uint64_t t0 = nowns();
    for (int64_t i = 0; i < M; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t v = (uint32_t)((seed >> 33) % (uint64_t)(n * 2));
        sink += branchyFind(a, n, v);
    }
    const double branchy = (double)(nowns() - t0) / (double)M;

    t0 = nowns();
    for (int64_t i = 0; i < M; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t v = (uint32_t)((seed >> 33) % (uint64_t)(n * 2));
        sink += branchlessFind(a, n, v);
    }
    const double branchless = (double)(nowns() - t0) / (double)M;

    printf("%-12s n=%9" PRId64 "  branchy=%7.2f ns  branchless+pf=%7.2f ns  "
           "speedup=%.2fx\n",
           name, n, branchy, branchless, branchy / branchless);
    free(a);
}

int main(void) {
    printf("=== intset binary-search: branchy vs branchless+prefetch "
           "(ns/lookup, 10M random probes) ===\n");
    run("tiny", 64);
    run("small", 1024);
    run("medium", 65536);
    run("large", 1u << 20);  /* 1M -> 4MB array */
    run("xlarge", 1u << 21); /* 2M -> 8MB array (medium-tier cap) */
    return 0;
}
