/* ====================================================================
 * varintCompeteBench — performance validation for the compete layer
 * ====================================================================
 * Not a correctness test (see varintCompeteTest). Two sections:
 *
 * 1. Primitive scans — the SIMD-sensitive kernels the chunked encoder
 *    leans on every block: the matching-prefix progression scan
 *    (varintStrideMatchingPrefixUnsigned), the sortedness checks
 *    (varintBP128IsSorted64/32), and the pruning probe
 *    (varintCompetePruneMask). Each library kernel is measured against
 *    a scalar reference compiled here that replicates the pre-SIMD
 *    code, with compiler barriers so neither side is hoisted — the only
 *    honest way to see what the vector paths contribute on the current
 *    machine. Both regimes are reported: a cache-hot block (the
 *    per-block cost the chunked encoder actually pays) and a streaming
 *    pass (bandwidth-bound; SIMD gains shrink here by design).
 *
 * 2. Chunked codec throughput — encode/decode MB/s, compressed size,
 *    and block counts for chunked streams vs the single-frame encoder
 *    across dataset shapes that exercise different winners. Add a row
 *    to datasets_[] to grow coverage.
 *
 * Used to gate optimizations on these paths: keep only measured wins.
 * Usage: varintCompeteBench [values-per-run] [repeats] [block-values]
 *   defaults: 1 << 20 values, 15 repeats (median), block-values 0 (the
 *   library default of 4096). */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, hidden by glibc under strict
 * -std=c11 unless requested. (Apple exposes them unconditionally.) */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "varintBP128.h"
#include "varintCompete.h"
#include "varintStride.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rngState_ = UINT64_C(88172645463325252);
static uint64_t rng_(void) {
    rngState_ = rngState_ * UINT64_C(6364136223846793005) +
                UINT64_C(1442695040888963407);
    return rngState_ >> 16;
}

static double now_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmpDouble_(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Prevents call hoisting/reordering around timed regions. */
#define BENCH_BARRIER() __asm__ volatile("" ::: "memory")

/* ====================================================================
 * Section 1: primitive scans
 * ====================================================================
 * Scalar references replicating the pre-SIMD library code. Kept
 * noinline so both sides pay one call like the real call sites do. */

__attribute__((noinline)) static size_t prefixScalarRef_(const uint64_t *values,
                                                         size_t count) {
    if (count <= 2) {
        return count;
    }
    const uint64_t stride = values[1] - values[0];
    size_t k = 2;
    while (k < count && values[k] - values[k - 1] == stride) {
        k++;
    }
    return k;
}

__attribute__((noinline)) static bool
isSorted64ScalarRef_(const uint64_t *values, size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (values[i] < values[i - 1]) {
            return false;
        }
    }
    return true;
}

__attribute__((noinline)) static bool
isSorted32ScalarRef_(const uint32_t *values, size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (values[i] < values[i - 1]) {
            return false;
        }
    }
    return true;
}

/* One timed kernel: repeatedly run fn over the same buffer and report
 * the median throughput in Gval/s. The full-match datasets below make
 * every scan walk the entire buffer, so throughput is comparable. */
typedef size_t (*scanFn_)(const void *values, size_t count);

static double benchScan_(scanFn_ fn, const void *values, size_t count,
                         size_t reps, int repeats, double *times) {
    volatile size_t sink = 0;
    for (int r = 0; r < repeats; r++) {
        const double t0 = now_();
        for (size_t k = 0; k < reps; k++) {
            BENCH_BARRIER();
            sink += fn(values, count);
        }
        times[r] = now_() - t0;
    }
    (void)sink;
    qsort(times, (size_t)repeats, sizeof(double), cmpDouble_);
    return (double)count * (double)reps / times[repeats / 2] / 1e9;
}

static size_t scanPrefixLib_(const void *values, size_t count) {
    return varintStrideMatchingPrefixUnsigned(values, count);
}
static size_t scanPrefixRef_(const void *values, size_t count) {
    return prefixScalarRef_(values, count);
}
static size_t scanSorted64Lib_(const void *values, size_t count) {
    return (size_t)varintBP128IsSorted64(values, count);
}
static size_t scanSorted64Ref_(const void *values, size_t count) {
    return (size_t)isSorted64ScalarRef_(values, count);
}
static size_t scanSorted32Lib_(const void *values, size_t count) {
    return (size_t)varintBP128IsSorted32(values, count);
}
static size_t scanSorted32Ref_(const void *values, size_t count) {
    return (size_t)isSorted32ScalarRef_(values, count);
}
static size_t scanPrune_(const void *values, size_t count) {
    return (size_t)varintCompetePruneMask(values, count,
                                          VARINT_COMPETE_DEFAULT_MASK);
}

typedef struct scanRow {
    const char *name;
    scanFn_ lib;
    scanFn_ ref; /* NULL = no scalar twin (report absolute only) */
} scanRow;

static void benchScans_(size_t blockValues, size_t streamCount, int repeats) {
    /* Full-length arithmetic progression: prefix scan matches to the
     * end, sortedness holds, so every kernel walks the whole buffer. */
    uint64_t *v64 = malloc(streamCount * sizeof(*v64));
    uint32_t *v32 = malloc(streamCount * sizeof(*v32));
    double *times = malloc((size_t)repeats * sizeof(*times));
    if (!v64 || !v32 || !times) {
        fprintf(stderr, "bench: allocation failed\n");
        exit(2);
    }
    for (size_t i = 0; i < streamCount; i++) {
        v64[i] = 1000 + (uint64_t)i * 7;
        v32[i] = (uint32_t)(1000 + i * 3);
    }

    const scanRow rows[] = {
        {"matchingPrefix64", scanPrefixLib_, scanPrefixRef_},
        {"isSorted64", scanSorted64Lib_, scanSorted64Ref_},
        {"isSorted32", scanSorted32Lib_, scanSorted32Ref_},
        {"pruneMask probe", scanPrune_, NULL},
    };

    printf("\n== primitive scans (median of %d, Gval/s) ==\n", repeats);
    printf("%-18s %14s %14s %14s %14s\n", "", "hot scalar", "hot SIMD",
           "stream scalar", "stream SIMD");
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        const void *hot =
            (rows[r].lib == scanSorted32Lib_) ? (void *)v32 : (void *)v64;
        /* Enough repetitions over the hot block to time reliably. */
        const size_t hotReps = (streamCount / blockValues) + 1;

        double hotRef = 0.0;
        double streamRef = 0.0;
        if (rows[r].ref) {
            hotRef = benchScan_(rows[r].ref, hot, blockValues, hotReps, repeats,
                                times);
            streamRef =
                benchScan_(rows[r].ref, hot, streamCount, 1, repeats, times);
        }
        const double hotLib =
            benchScan_(rows[r].lib, hot, blockValues, hotReps, repeats, times);
        const double streamLib =
            benchScan_(rows[r].lib, hot, streamCount, 1, repeats, times);

        if (rows[r].ref) {
            printf("%-18s %14.2f %7.2f (%.2fx) %14.2f %7.2f (%.2fx)\n",
                   rows[r].name, hotRef, hotLib, hotLib / hotRef, streamRef,
                   streamLib, streamLib / streamRef);
        } else {
            printf("%-18s %14s %14.2f %14s %14.2f\n", rows[r].name, "-", hotLib,
                   "-", streamLib);
        }
    }

    free(v64);
    free(v32);
    free(times);
}

/* ====================================================================
 * Section 2: chunked codec throughput
 * ==================================================================== */

typedef void (*fillFn_)(uint64_t *values, size_t count);

static void fillRamp_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = 5000 + (uint64_t)i * 7;
    }
}

static void fillRunsThenNoise_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = (i < count / 2) ? 42 : rng_();
    }
}

static void fillSortedJitter_(uint64_t *values, size_t count) {
    uint64_t v = UINT64_C(1) << 40;
    for (size_t i = 0; i < count; i++) {
        v += 1 + (rng_() & 31);
        values[i] = v;
    }
}

static void fillSkewedSmall_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const uint64_t r = rng_() & 0xFF;
        uint64_t sym = 0;
        for (uint64_t bit = 128; bit > 1 && (r & bit); bit >>= 1) {
            sym++;
        }
        values[i] = sym;
    }
}

static void fillUniqueNoise_(uint64_t *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        values[i] = rng_() ^ ((uint64_t)i << 40);
    }
}

typedef struct datasetRow {
    const char *name;
    fillFn_ fill;
} datasetRow;

static const datasetRow datasets_[] = {
    {"ramp", fillRamp_},
    {"runs+noise", fillRunsThenNoise_},
    {"sorted-jit", fillSortedJitter_},
    {"skewed-16", fillSkewedSmall_},
    {"unique", fillUniqueNoise_},
};

static void benchChunkedOne_(const datasetRow *ds, size_t count,
                             size_t blockValues, int repeats) {
    uint64_t *values = malloc(count * sizeof(*values));
    uint64_t *dec = malloc(count * sizeof(*dec));
    uint8_t *single = malloc(varintCompeteMaxEncodedSize(count));
    uint8_t *chunked =
        malloc(varintCompeteMaxEncodedSizeChunked(count, blockValues));
    double *encS = malloc((size_t)repeats * sizeof(*encS));
    double *encC = malloc((size_t)repeats * sizeof(*encC));
    double *decC = malloc((size_t)repeats * sizeof(*decC));
    if (!values || !dec || !single || !chunked || !encS || !encC || !decC) {
        fprintf(stderr, "bench: allocation failed\n");
        exit(2);
    }
    ds->fill(values, count);

    size_t singleBytes = 0;
    size_t chunkedBytes = 0;
    size_t blocks = 0;
    for (int r = 0; r < repeats; r++) {
        double t0 = now_();
        singleBytes = varintCompeteEncodeUnsigned(
            single, values, count, VARINT_COMPETE_DEFAULT_MASK, NULL);
        encS[r] = now_() - t0;

        t0 = now_();
        chunkedBytes = varintCompeteEncodeChunkedUnsigned(
            chunked, values, count, VARINT_COMPETE_DEFAULT_MASK, blockValues,
            &blocks);
        encC[r] = now_() - t0;
    }
    size_t decodedCount = 0;
    for (int r = 0; r < repeats; r++) {
        const double t0 = now_();
        const size_t got = varintCompeteDecodeChunkedUnsigned(
            chunked, chunkedBytes, dec, count, &decodedCount);
        decC[r] = now_() - t0;
        if (got != chunkedBytes || decodedCount != count) {
            fprintf(stderr, "bench: chunked decode failed on %s\n", ds->name);
            exit(2);
        }
    }
    if (memcmp(dec, values, count * sizeof(*dec)) != 0) {
        fprintf(stderr, "bench: round-trip mismatch on %s\n", ds->name);
        exit(2);
    }

    qsort(encS, (size_t)repeats, sizeof(double), cmpDouble_);
    qsort(encC, (size_t)repeats, sizeof(double), cmpDouble_);
    qsort(decC, (size_t)repeats, sizeof(double), cmpDouble_);
    const double mb = (double)(count * sizeof(uint64_t)) / (1024.0 * 1024.0);

    printf("%-10s  single %9zu B %7.1f MB/s | chunked %9zu B (%5.1f%%, "
           "%5zu blk)  enc %7.1f MB/s  dec %7.1f MB/s\n",
           ds->name, singleBytes, mb / encS[repeats / 2], chunkedBytes,
           100.0 * (double)chunkedBytes / (double)singleBytes, blocks,
           mb / encC[repeats / 2], mb / decC[repeats / 2]);

    free(values);
    free(dec);
    free(single);
    free(chunked);
    free(encS);
    free(encC);
    free(decC);
}

int main(int argc, char *argv[]) {
    const size_t count =
        (argc > 1) ? strtoull(argv[1], NULL, 0) : ((size_t)1 << 20);
    const int repeats = (argc > 2) ? atoi(argv[2]) : 15;
    const size_t blockValues = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;
    const size_t effectiveBlock =
        blockValues ? blockValues : VARINT_COMPETE_CHUNK_DEFAULT_VALUES;

    printf("varintCompeteBench: %zu values, median of %d runs, block %zu\n",
           count, repeats, effectiveBlock);

    benchScans_(effectiveBlock, count, repeats);

    printf("\n== chunked vs single-frame (chunked %% = of single size) ==\n");
    for (size_t d = 0; d < sizeof(datasets_) / sizeof(datasets_[0]); d++) {
        benchChunkedOne_(&datasets_[d], count, blockValues, repeats);
    }
    return 0;
}
