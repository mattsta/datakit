/* Segment Tree - I64 Comprehensive Tests (2-TIER SYSTEM)
 * FULLY MIGRATED from original segmentTest.c with ALL tests and benchmarks
 */

#include "segmentI64.h"
#include "../ctest.h"
#include "../datakit.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DATAKIT_TEST

#define DOUBLE_NEWLINE 1
#include "../perf.h"

/* Naive implementations for benchmark comparison */
typedef struct {
    int64_t *values;
    size_t count;
} naiveArray;

static naiveArray *naiveNew(size_t n) {
    naiveArray *arr = zmalloc(sizeof(*arr));
    arr->count = n;
    arr->values = zcalloc(n, sizeof(int64_t));
    return arr;
}

static void naiveFree(naiveArray *arr) {
    if (arr) {
        if (arr->values) {
            zfree(arr->values);
        }
        zfree(arr);
    }
}

static void naiveUpdate(naiveArray *arr, size_t idx, int64_t value) {
    if (idx < arr->count) {
        arr->values[idx] = value;
    }
}

static int64_t naiveRangeSum(const naiveArray *arr, size_t left, size_t right) {
    int64_t sum = 0;
    if (right >= arr->count) {
        right = arr->count - 1;
    }
    for (size_t i = left; i <= right; i++) {
        sum += arr->values[i];
    }
    return sum;
}

static int64_t naiveRangeMin(const naiveArray *arr, size_t left, size_t right) {
    if (right >= arr->count) {
        right = arr->count - 1;
    }
    int64_t minVal = arr->values[left];
    for (size_t i = left + 1; i <= right; i++) {
        if (arr->values[i] < minVal) {
            minVal = arr->values[i];
        }
    }
    return minVal;
}

static int64_t naiveRangeMax(const naiveArray *arr, size_t left, size_t right) {
    if (right >= arr->count) {
        right = arr->count - 1;
    }
    int64_t maxVal = arr->values[left];
    for (size_t i = left + 1; i <= right; i++) {
        if (arr->values[i] > maxVal) {
            maxVal = arr->values[i];
        }
    }
    return maxVal;
}

static uint64_t randSeed(uint64_t *seed) {
    *seed = (*seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *seed;
}

int segmentI64Test(int argc, char *argv[]) {
    int err = 0;

    (void)argc;
    (void)argv;

    /* =================================================================
     * BASIC OPERATIONS
     * ================================================================= */

    TEST("basic: empty segment tree") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        if (segmentI64Count(seg) != 0) {
            ERRR("Empty tree should have count 0");
        }

        if (SEGMENT_I64_TYPE(seg) != SEGMENT_I64_TYPE_SMALL) {
            ERRR("New tree should start as SMALL tier");
        }

        segmentI64Free(seg);
    }

    TEST("basic: single element operations") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        segmentI64Update(&seg, 0, 10);

        if (segmentI64Get(seg, 0) != 10) {
            ERR("Element at 0 should be 10, got %" PRId64,
                segmentI64Get(seg, 0));
        }

        if (segmentI64Query(seg, 0, 0) != 10) {
            ERR("Query[0,0] should be 10, got %" PRId64,
                segmentI64Query(seg, 0, 0));
        }

        segmentI64Free(seg);
    }

    TEST("basic: range sum queries") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        /* Build: [1, 2, 3, 4, 5] */
        for (size_t i = 0; i < 5; i++) {
            segmentI64Update(&seg, i, (int64_t)(i + 1));
        }

        /* Verify elements */
        for (size_t i = 0; i < 5; i++) {
            if (segmentI64Get(seg, i) != (int64_t)(i + 1)) {
                ERR("Element at %zu should be %zu, got %" PRId64, i, i + 1,
                    segmentI64Get(seg, i));
            }
        }

        /* Test range queries */
        if (segmentI64Query(seg, 0, 4) != 15) {
            ERR("Sum[0,4] should be 15, got %" PRId64,
                segmentI64Query(seg, 0, 4));
        }

        if (segmentI64Query(seg, 1, 3) != 9) {
            ERR("Sum[1,3] should be 9, got %" PRId64,
                segmentI64Query(seg, 1, 3));
        }

        if (segmentI64Query(seg, 2, 2) != 3) {
            ERR("Sum[2,2] should be 3, got %" PRId64,
                segmentI64Query(seg, 2, 2));
        }

        segmentI64Free(seg);
    }

    TEST("basic: range min queries") {
        void *seg = segmentI64New(SEGMENT_OP_MIN);

        /* Build: [5, 2, 8, 1, 9] */
        int64_t values[] = {5, 2, 8, 1, 9};
        for (size_t i = 0; i < 5; i++) {
            segmentI64Update(&seg, i, values[i]);
        }

        if (segmentI64Query(seg, 0, 4) != 1) {
            ERR("Min[0,4] should be 1, got %" PRId64,
                segmentI64Query(seg, 0, 4));
        }

        if (segmentI64Query(seg, 0, 2) != 2) {
            ERR("Min[0,2] should be 2, got %" PRId64,
                segmentI64Query(seg, 0, 2));
        }

        if (segmentI64Query(seg, 2, 4) != 1) {
            ERR("Min[2,4] should be 1, got %" PRId64,
                segmentI64Query(seg, 2, 4));
        }

        segmentI64Free(seg);
    }

    TEST("basic: range max queries") {
        void *seg = segmentI64New(SEGMENT_OP_MAX);

        /* Build: [5, 2, 8, 1, 9] */
        int64_t values[] = {5, 2, 8, 1, 9};
        for (size_t i = 0; i < 5; i++) {
            segmentI64Update(&seg, i, values[i]);
        }

        if (segmentI64Query(seg, 0, 4) != 9) {
            ERR("Max[0,4] should be 9, got %" PRId64,
                segmentI64Query(seg, 0, 4));
        }

        if (segmentI64Query(seg, 0, 2) != 8) {
            ERR("Max[0,2] should be 8, got %" PRId64,
                segmentI64Query(seg, 0, 2));
        }

        if (segmentI64Query(seg, 3, 4) != 9) {
            ERR("Max[3,4] should be 9, got %" PRId64,
                segmentI64Query(seg, 3, 4));
        }

        segmentI64Free(seg);
    }

    TEST("edge case: sparse updates") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        segmentI64Update(&seg, 0, 1);
        segmentI64Update(&seg, 100, 2);
        segmentI64Update(&seg, 1000, 3);

        if (segmentI64Get(seg, 0) != 1) {
            ERRR("Element at 0 incorrect");
        }

        if (segmentI64Get(seg, 100) != 2) {
            ERRR("Element at 100 incorrect");
        }

        if (segmentI64Get(seg, 1000) != 3) {
            ERRR("Element at 1000 incorrect");
        }

        int64_t sum = segmentI64Query(seg, 0, 1000);
        if (sum != 6) {
            ERR("Sum[0,1000] should be 6, got %" PRId64, sum);
        }

        segmentI64Free(seg);
    }

    TEST("stress: 1K element operations") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        for (size_t i = 0; i < 1000; i++) {
            segmentI64Update(&seg, i, (int64_t)i);
        }

        if (segmentI64Count(seg) != 1000) {
            ERR("Count should be 1000, got %zu", segmentI64Count(seg));
        }

        /* Sum: 0+1+...+999 = 499500 */
        int64_t expected = (999 * 1000) / 2;
        int64_t actual = segmentI64Query(seg, 0, 999);
        if (actual != expected) {
            ERR("Sum[0,999] should be %" PRId64 ", got %" PRId64, expected,
                actual);
        }

        if (segmentI64Query(seg, 0, 99) != (99 * 100) / 2) {
            ERRR("Sum[0,99] incorrect");
        }

        segmentI64Free(seg);
    }

    TEST("tier upgrade: small to full (2-TIER)") {
        void *seg = segmentI64New(SEGMENT_OP_SUM);

        /* Build beyond small tier */
        for (size_t i = 0; i < 10000; i++) {
            segmentI64Update(&seg, i, (int64_t)(i % 100));
        }

        /* Should be in Full tier */
        if (SEGMENT_I64_TYPE(seg) != SEGMENT_I64_TYPE_FULL) {
            ERR("Should be FULL tier, got %d", SEGMENT_I64_TYPE(seg));
        }

        /* Verify all values */
        for (size_t i = 0; i < 10000; i++) {
            if (segmentI64Get(seg, i) != (int64_t)(i % 100)) {
                ERR("Value at %zu incorrect", i);
            }
        }

        segmentI64Free(seg);
    }

    TEST("advanced: min/max with negative values") {
        void *segMin = segmentI64New(SEGMENT_OP_MIN);
        void *segMax = segmentI64New(SEGMENT_OP_MAX);

        /* Build: [-50, -40, ..., 0, ..., 50] */
        for (int i = -50; i <= 50; i++) {
            segmentI64Update(&segMin, i + 50, i);
            segmentI64Update(&segMax, i + 50, i);
        }

        if (segmentI64Query(segMin, 0, 100) != -50) {
            ERR("Min should be -50, got %" PRId64,
                segmentI64Query(segMin, 0, 100));
        }

        if (segmentI64Query(segMax, 0, 100) != 50) {
            ERR("Max should be 50, got %" PRId64,
                segmentI64Query(segMax, 0, 100));
        }

        segmentI64Free(segMin);
        segmentI64Free(segMax);
    }

    /* =================================================================
     * PERFORMANCE BENCHMARKS
     * ================================================================= */

    TEST("BENCH: Segment vs Naive - SUM queries (1K)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 10000000;
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *seg = segmentI64New(SEGMENT_OP_SUM);
        for (size_t i = 0; i < N; i++) {
            segmentI64Update(&seg, i, init[i]);
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Segment queries */
        volatile int64_t segSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            segSum += segmentI64Query(seg, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "segmentI64 SUM queries (1K)");

        /* Naive queries */
        volatile int64_t naiveSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            naiveSum += naiveRangeSum(naive, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive SUM queries (1K)");

        if (segSum != naiveSum) {
            ERR("Checksum mismatch! Segment: %" PRId64 ", Naive: %" PRId64,
                segSum, naiveSum);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", segSum);

        segmentI64Free(seg);
        naiveFree(naive);
        zfree(init);
    }

    TEST("BENCH: Segment vs Naive - MIN queries (1K)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 10000000;
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *seg = segmentI64New(SEGMENT_OP_MIN);
        for (size_t i = 0; i < N; i++) {
            segmentI64Update(&seg, i, init[i]);
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Segment queries */
        volatile int64_t segMin = 0;
        seed = 99999;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            segMin += segmentI64Query(seg, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "segmentI64 MIN queries (1K)");

        /* Naive queries */
        volatile int64_t naiveMin = 0;
        seed = 99999;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            naiveMin += naiveRangeMin(naive, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive MIN queries (1K)");

        if (segMin != naiveMin) {
            ERR("Checksum mismatch! Segment: %" PRId64 ", Naive: %" PRId64,
                segMin, naiveMin);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", segMin);

        segmentI64Free(seg);
        naiveFree(naive);
        zfree(init);
    }

    TEST("BENCH: Segment vs Naive - MAX queries (1K)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 10000000;
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *seg = segmentI64New(SEGMENT_OP_MAX);
        for (size_t i = 0; i < N; i++) {
            segmentI64Update(&seg, i, init[i]);
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Segment queries */
        volatile int64_t segMax = 0;
        seed = 77777;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            segMax += segmentI64Query(seg, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "segmentI64 MAX queries (1K)");

        /* Naive queries */
        volatile int64_t naiveMax = 0;
        seed = 77777;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t left = randSeed(&seed) % N;
            size_t right = left + (randSeed(&seed) % (N - left));
            naiveMax += naiveRangeMax(naive, left, right);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive MAX queries (1K)");

        if (segMax != naiveMax) {
            ERR("Checksum mismatch! Segment: %" PRId64 ", Naive: %" PRId64,
                segMax, naiveMax);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", segMax);

        segmentI64Free(seg);
        naiveFree(naive);
        zfree(init);
    }

    TEST("BENCH: Update performance (1K)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 10000000;
        uint64_t seed = 12345;

        void *seg = segmentI64New(SEGMENT_OP_SUM);
        naiveArray *naive = naiveNew(N);

        /* Segment updates */
        seed = 11111;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            segmentI64Update(&seg, idx, (int64_t)(randSeed(&seed) % 100));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "segmentI64 updates (1K)");

        /* Naive updates */
        seed = 11111;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            naiveUpdate(naive, randSeed(&seed) % N,
                        (int64_t)(randSeed(&seed) % 100));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive updates (1K)");

        segmentI64Free(seg);
        naiveFree(naive);
    }

    /* =================================================================
     * SCALING BENCHMARKS - Show crossover points
     * ================================================================= */

    TEST("BENCH SCALING: SUM queries across sizes") {
        printf("\n=== segmentI64 vs Naive SUM - Scaling Benchmark ===\n");
        printf("Size    | Ops/sec (segI64) | Ops/sec (naive) | Speedup  | Avg "
               "Range\n");
        printf("--------|------------------|-----------------|----------|------"
               "----"
               "\n");

        /* Stop at 10K for naive comparison - larger sizes take too long */
        size_t sizes[] = {100, 500, 1000, 5000, 10000, 100000};
        const size_t NUM_OPS = 1000000;

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            size_t N = sizes[s];
            uint64_t seed = 12345;

            /* Build test data */
            int64_t *init = zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                init[i] = (randSeed(&seed) % 1000) - 500;
            }

            void *seg = segmentI64New(SEGMENT_OP_SUM);
            for (size_t i = 0; i < N; i++) {
                segmentI64Update(&seg, i, init[i]);
            }

            naiveArray *naive = naiveNew(N);
            memcpy(naive->values, init, N * sizeof(int64_t));

            /* Measure average range size */
            size_t totalRange = 0;
            seed = 54321;
            for (size_t i = 0; i < 10000; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                totalRange += (right - left + 1);
            }
            size_t avgRange = totalRange / 10000;

            /* Benchmark segment */
            volatile int64_t segSum = 0;
            seed = 54321;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                segSum += segmentI64Query(seg, left, right);
            }
            PERF_TIMERS_FINISH;
            double segOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Benchmark naive */
            volatile int64_t naiveSum = 0;
            seed = 54321;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                naiveSum += naiveRangeSum(naive, left, right);
            }
            PERF_TIMERS_FINISH;
            double naiveOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Verify */
            if (segSum != naiveSum) {
                ERR("Checksum mismatch at N=%zu!", N);
            }

            double speedup = segOpsPerSec / naiveOpsPerSec;
            printf("%-7zu | %13.0f | %15.0f | %7.2fx | %zu\n", N, segOpsPerSec,
                   naiveOpsPerSec, speedup, avgRange);

            segmentI64Free(seg);
            naiveFree(naive);
            zfree(init);
        }
        printf("\n");
    }

    TEST("BENCH SCALING: MIN queries across sizes") {
        printf("\n=== segmentI64 vs Naive MIN - Scaling Benchmark ===\n");
        printf("Size    | Ops/sec (segI64) | Ops/sec (naive) | Speedup\n");
        printf("--------|------------------|-----------------|----------\n");

        /* Stop at 10K for naive comparison - larger sizes take too long */
        size_t sizes[] = {100, 500, 1000, 5000, 10000, 100000};
        const size_t NUM_OPS = 1000000;

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            size_t N = sizes[s];
            uint64_t seed = 99999;

            /* Build test data */
            int64_t *init = zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                init[i] = (randSeed(&seed) % 1000) - 500;
            }

            void *seg = segmentI64New(SEGMENT_OP_MIN);
            for (size_t i = 0; i < N; i++) {
                segmentI64Update(&seg, i, init[i]);
            }

            naiveArray *naive = naiveNew(N);
            memcpy(naive->values, init, N * sizeof(int64_t));

            /* Benchmark segment */
            volatile int64_t segMin = 0;
            seed = 77777;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                segMin += segmentI64Query(seg, left, right);
            }
            PERF_TIMERS_FINISH;
            double segOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Benchmark naive */
            volatile int64_t naiveMin = 0;
            seed = 77777;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                naiveMin += naiveRangeMin(naive, left, right);
            }
            PERF_TIMERS_FINISH;
            double naiveOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Verify */
            if (segMin != naiveMin) {
                ERR("Checksum mismatch at N=%zu!", N);
            }

            double speedup = segOpsPerSec / naiveOpsPerSec;
            printf("%-7zu | %13.0f | %15.0f | %7.2fx\n", N, segOpsPerSec,
                   naiveOpsPerSec, speedup);

            segmentI64Free(seg);
            naiveFree(naive);
            zfree(init);
        }
        printf("\n");
    }

    TEST("BENCH SCALING: MAX queries across sizes") {
        printf("\n=== segmentI64 vs Naive MAX - Scaling Benchmark ===\n");
        printf("Size    | Ops/sec (segI64) | Ops/sec (naive) | Speedup\n");
        printf("--------|------------------|-----------------|----------\n");

        /* Stop at 10K for naive comparison - larger sizes take too long */
        size_t sizes[] = {100, 500, 1000, 5000, 10000, 100000};
        const size_t NUM_OPS = 1000000;

        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            size_t N = sizes[s];
            uint64_t seed = 11111;

            /* Build test data */
            int64_t *init = zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                init[i] = (randSeed(&seed) % 1000) - 500;
            }

            void *seg = segmentI64New(SEGMENT_OP_MAX);
            for (size_t i = 0; i < N; i++) {
                segmentI64Update(&seg, i, init[i]);
            }

            naiveArray *naive = naiveNew(N);
            memcpy(naive->values, init, N * sizeof(int64_t));

            /* Benchmark segment */
            volatile int64_t segMax = 0;
            seed = 88888;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                segMax += segmentI64Query(seg, left, right);
            }
            PERF_TIMERS_FINISH;
            double segOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Benchmark naive */
            volatile int64_t naiveMax = 0;
            seed = 88888;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t left = randSeed(&seed) % N;
                size_t right = left + (randSeed(&seed) % (N - left));
                naiveMax += naiveRangeMax(naive, left, right);
            }
            PERF_TIMERS_FINISH;
            double naiveOpsPerSec =
                NUM_OPS / ((double)lps.global.us.duration / 1e6);

            /* Verify */
            if (segMax != naiveMax) {
                ERR("Checksum mismatch at N=%zu!", N);
            }

            double speedup = segOpsPerSec / naiveOpsPerSec;
            printf("%-7zu | %13.0f | %15.0f | %7.2fx\n", N, segOpsPerSec,
                   naiveOpsPerSec, speedup);

            segmentI64Free(seg);
            naiveFree(naive);
            zfree(init);
        }
        printf("\n");
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
