/* fenwickBench: real-world workloads over every Fenwick and segment tree.
 *
 *   datakit-test test fenwickBench      (build with the perf preset)
 *
 * Five workloads, each at a small (1K), medium (100K) and large (4M)
 * size, report nanoseconds per operation and bytes per element:
 *
 *   1. counter table   integer counts (+1 updates, prefix/range sums,
 *                      element reads, rank by LowerBound): every 1D sum
 *                      tree, and segment SUM trees (whose Update assigns)
 *   2. mixed values    the same with 1% of updates doubles: the adaptive
 *                      databox trees then carry kinds (fenwickDouble for
 *                      comparison)
 *   3. sparse growth   writes at random indexes of an empty tree, so every
 *                      tree pays its growth (ns per write, all included)
 *   4. range minimum   segment MIN trees (Fenwick trees cannot do this)
 *   5. 2D grid         heatmap counts: point updates, prefix and
 *                      rectangle sums, element reads, on a square grid of
 *                      about N cells
 *
 * Values are small, so every sum is exact in every type; indexes are
 * uniform random; results feed a printed checksum so nothing is
 * optimized away.  Each cell of a table runs for a fixed time budget (at
 * most BENCH_MAX_OPS operations), so slow trees take fewer operations,
 * not more time.  validate skips this test (timing only). */

#include "fenwickBench.h"

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "fenwick/fenwick2d.h"
#include "fenwick/fenwickDouble.h"
#include "fenwick/fenwickFloat.h"
#include "fenwick/fenwickI16.h"
#include "fenwick/fenwickI32.h"
#include "fenwick/fenwickI64.h"
#ifdef __SIZEOF_INT128__
#include "fenwick/fenwickI128.h"
#endif
#include "multiFenwick.h"
#include "multiFenwick2d.h"
#include "multiFenwickFlat.h"
#include "segment/segmentDouble.h"
#include "segment/segmentI64.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

#define BENCH_MAX_OPS 200000
#define BENCH_BUDGET_NS 40000000.0 /* 40 ms per table cell */

static uint64_t benchRand_(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static double benchNow_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* Run 'body' (using 'seed') until the budget or the op cap; set 'nsPerOp' */
#define BENCH_TIMED(nsPerOp, body)                                             \
    do {                                                                       \
        uint64_t seed = 99;                                                    \
        size_t ops = 0;                                                        \
        const double start = benchNow_();                                      \
        double elapsed = 0;                                                    \
        while (ops < BENCH_MAX_OPS) {                                          \
            for (size_t rep_ = 0; rep_ < 256; rep_++, ops++) {                 \
                body;                                                          \
            }                                                                  \
            elapsed = benchNow_() - start;                                     \
            if (elapsed > BENCH_BUDGET_NS) {                                   \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        (nsPerOp) = elapsed / (double)ops;                                     \
    } while (0)

/* One row: ns per operation (NAN: not supported) and bytes per element */
typedef struct benchRow {
    double build, update, query, get, range, lower;
    double bytesPerElem;
} benchRow;

static uint64_t benchCheck_; /* checksum every operation feeds */

static void benchHeader_(const char *title) {
    printf("\n### %s\n\n", title);
    printf("| tree | N | build | update | query | get | range | lower | "
           "B/elem |\n");
    printf("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
}

static void benchCell_(double v) {
    if (isnan(v)) {
        printf(" - |");
    } else {
        printf(" %.1f |", v);
    }
}

static void benchPrint_(const char *name, size_t n, const benchRow *r) {
    printf("| %s | %zu |", name, n);
    benchCell_(r->build);
    benchCell_(r->update);
    benchCell_(r->query);
    benchCell_(r->get);
    benchCell_(r->range);
    benchCell_(r->lower);
    printf(" %.2f |\n", r->bytesPerElem);
}

/* ====================================================================
 * Typed 1D trees (built by n Updates: they have no public array build)
 * ==================================================================== */
#define BENCH_TYPED(T, VT)                                                     \
    static benchRow benchTyped##T(size_t n) {                                  \
        benchRow r = {0};                                                      \
        void *fw = NULL;                                                       \
        const double t0 = benchNow_();                                         \
        for (size_t i = 0; i < n; i++) {                                       \
            fenwick##T##Update(&fw, i, (VT)(i % 7));                           \
        }                                                                      \
        r.build = (benchNow_() - t0) / (double)n;                              \
        VT acc = 0;                                                            \
        BENCH_TIMED(r.update,                                                  \
                    fenwick##T##Update(&fw, benchRand_(&seed) % n, (VT)1));    \
        BENCH_TIMED(r.query,                                                   \
                    acc += fenwick##T##Query(fw, benchRand_(&seed) % n));      \
        BENCH_TIMED(r.get, acc += fenwick##T##Get(fw, benchRand_(&seed) % n)); \
        BENCH_TIMED(r.range, {                                                 \
            const size_t a = benchRand_(&seed) % n;                            \
            acc += fenwick##T##RangeQuery(fw, a / 2, a);                       \
        });                                                                    \
        BENCH_TIMED(r.lower, benchCheck_ += fenwick##T##LowerBound(            \
                                 fw, (VT)(benchRand_(&seed) % (3 * n))));      \
        r.bytesPerElem = (double)fenwick##T##Bytes(fw) / (double)n;            \
        benchCheck_ += (uint64_t)acc;                                          \
        fenwick##T##Free(fw);                                                  \
        return r;                                                              \
    }

BENCH_TYPED(I16, int16_t)
BENCH_TYPED(I32, int32_t)
BENCH_TYPED(I64, int64_t)
BENCH_TYPED(Float, float)
BENCH_TYPED(Double, double)
#ifdef __SIZEOF_INT128__
BENCH_TYPED(I128, __int128_t)
#endif

/* ====================================================================
 * Databox 1D trees: built from an array (O(n)); 'doubleEvery' > 0 makes
 * every that-many-th value (and update) a double
 * ==================================================================== */
#define BENCH_MULTI(NAME, T)                                                   \
    static benchRow benchMulti##NAME(size_t n, size_t doubleEvery) {           \
        benchRow r = {0};                                                      \
        cleaner(zfree) void *valsMem = zcalloc(n, sizeof(databox));            \
        databox *vals = valsMem;                                               \
        for (size_t i = 0; i < n; i++) {                                       \
            vals[i] = doubleEvery && i % doubleEvery == 0                      \
                          ? DATABOX_DOUBLE(0.5)                                \
                          : DATABOX_UNSIGNED(i % 7);                           \
        }                                                                      \
        const double t0 = benchNow_();                                         \
        T *fw = T##NewFromArray(vals, n);                                      \
        r.build = (benchNow_() - t0) / (double)n;                              \
        const databox one = DATABOX_UNSIGNED(1);                               \
        const databox half = DATABOX_DOUBLE(0.5);                              \
        databox out;                                                           \
        uint64_t acc = 0;                                                      \
        BENCH_TIMED(r.update, {                                                \
            const uint64_t k = benchRand_(&seed);                              \
            T##Update(&fw, k % n,                                              \
                      doubleEvery && k % doubleEvery == 0 ? &half : &one);     \
        });                                                                    \
        BENCH_TIMED(r.query, {                                                 \
            T##Query(fw, benchRand_(&seed) % n, &out);                         \
            acc += out.data.u64;                                               \
        });                                                                    \
        BENCH_TIMED(r.get, {                                                   \
            T##Get(fw, benchRand_(&seed) % n, &out);                           \
            acc += out.data.u64;                                               \
        });                                                                    \
        BENCH_TIMED(r.range, {                                                 \
            const size_t a = benchRand_(&seed) % n;                            \
            T##RangeQuery(fw, a / 2, a, &out);                                 \
            acc += out.data.u64;                                               \
        });                                                                    \
        BENCH_TIMED(r.lower, {                                                 \
            const databox target =                                             \
                DATABOX_UNSIGNED(benchRand_(&seed) % (3 * n));                 \
            acc += T##LowerBound(fw, &target);                                 \
        });                                                                    \
        r.bytesPerElem = (double)T##Bytes(fw) / (double)n;                     \
        benchCheck_ += acc;                                                    \
        T##Free(fw);                                                           \
        return r;                                                              \
    }

BENCH_MULTI(Fenwick, multiFenwick)
BENCH_MULTI(Flat, multiFenwickFlat)

/* ====================================================================
 * Segment trees: Update assigns (counter +1 is a Get and an Update);
 * 'query' is the prefix fold [0, idx]
 * ==================================================================== */
#define BENCH_SEGMENT(T, VT)                                                   \
    static benchRow benchSegment##T(size_t n, segmentOp op) {                  \
        benchRow r = {0};                                                      \
        void *seg = segment##T##New(op);                                       \
        const double t0 = benchNow_();                                         \
        for (size_t i = 0; i < n; i++) {                                       \
            segment##T##Update(&seg, i, (VT)(i % 7));                          \
        }                                                                      \
        r.build = (benchNow_() - t0) / (double)n;                              \
        VT acc = 0;                                                            \
        BENCH_TIMED(r.update, {                                                \
            const size_t i = benchRand_(&seed) % n;                            \
            segment##T##Update(&seg, i, segment##T##Get(seg, i) + (VT)1);      \
        });                                                                    \
        BENCH_TIMED(r.query,                                                   \
                    acc += segment##T##Query(seg, 0, benchRand_(&seed) % n));  \
        BENCH_TIMED(r.get,                                                     \
                    acc += segment##T##Get(seg, benchRand_(&seed) % n));       \
        BENCH_TIMED(r.range, {                                                 \
            const size_t a = benchRand_(&seed) % n;                            \
            acc += segment##T##Query(seg, a / 2, a);                           \
        });                                                                    \
        r.lower = NAN;                                                         \
        r.bytesPerElem = (double)segment##T##Bytes(seg) / (double)n;           \
        benchCheck_ += (uint64_t)acc;                                          \
        segment##T##Free(seg);                                                 \
        return r;                                                              \
    }

BENCH_SEGMENT(I64, int64_t)
BENCH_SEGMENT(Double, double)

/* ====================================================================
 * Sparse growth: n writes at random indexes in [0, n) of an empty tree;
 * 'build' is ns per write, every growth included
 * ==================================================================== */
static benchRow benchGrowRow_(double nsPerWrite, size_t bytes, size_t n) {
    return (benchRow){.build = nsPerWrite,
                      .update = NAN,
                      .query = NAN,
                      .get = NAN,
                      .range = NAN,
                      .lower = NAN,
                      .bytesPerElem = (double)bytes / (double)n};
}

static benchRow benchGrowTypedI64(size_t n) {
    uint64_t seed = 5;
    void *fw = NULL;
    const double t0 = benchNow_();
    for (size_t i = 0; i < n; i++) {
        fenwickI64Update(&fw, benchRand_(&seed) % n, 1);
    }
    const benchRow r =
        benchGrowRow_((benchNow_() - t0) / (double)n, fenwickI64Bytes(fw), n);
    fenwickI64Free(fw);
    return r;
}

#define BENCH_GROW_MULTI(NAME, T)                                              \
    static benchRow benchGrow##NAME(size_t n) {                                \
        uint64_t seed = 5;                                                     \
        T *fw = NULL;                                                          \
        const databox one = DATABOX_UNSIGNED(1);                               \
        const double t0 = benchNow_();                                         \
        for (size_t i = 0; i < n; i++) {                                       \
            T##Update(&fw, benchRand_(&seed) % n, &one);                       \
        }                                                                      \
        const benchRow r =                                                     \
            benchGrowRow_((benchNow_() - t0) / (double)n, T##Bytes(fw), n);    \
        T##Free(fw);                                                           \
        return r;                                                              \
    }

BENCH_GROW_MULTI(Fenwick, multiFenwick)
BENCH_GROW_MULTI(Flat, multiFenwickFlat)

static benchRow benchGrowSegmentI64(size_t n) {
    uint64_t seed = 5;
    void *seg = NULL; /* a NULL tree becomes a SUM tree */
    const double t0 = benchNow_();
    for (size_t i = 0; i < n; i++) {
        segmentI64Update(&seg, benchRand_(&seed) % n, 1);
    }
    const benchRow r =
        benchGrowRow_((benchNow_() - t0) / (double)n, segmentI64Bytes(seg), n);
    segmentI64Free(seg);
    return r;
}

/* ====================================================================
 * 2D grids: side x side, side = sqrt(n)
 * ==================================================================== */
#define BENCH_2D_TYPED(T, VT)                                                  \
    static benchRow bench2d##T(size_t n) {                                     \
        benchRow r = {0};                                                      \
        const size_t side = (size_t)sqrt((double)n);                           \
        fenwick2d##T *t = NULL;                                                \
        const double t0 = benchNow_();                                         \
        for (size_t i = 0; i < side; i++) {                                    \
            for (size_t j = 0; j < side; j++) {                                \
                fenwick2d##T##Update(&t, i, j, (VT)((i + j) % 7));             \
            }                                                                  \
        }                                                                      \
        r.build = (benchNow_() - t0) / (double)(side * side);                  \
        VT acc = 0;                                                            \
        BENCH_TIMED(r.update,                                                  \
                    fenwick2d##T##Update(&t, benchRand_(&seed) % side,         \
                                         benchRand_(&seed) % side, (VT)1));    \
        BENCH_TIMED(r.query,                                                   \
                    acc += fenwick2d##T##Query(t, benchRand_(&seed) % side,    \
                                               benchRand_(&seed) % side));     \
        BENCH_TIMED(r.get,                                                     \
                    acc += fenwick2d##T##Get(t, benchRand_(&seed) % side,      \
                                             benchRand_(&seed) % side));       \
        BENCH_TIMED(r.range, {                                                 \
            const size_t a = benchRand_(&seed) % side;                         \
            const size_t b = benchRand_(&seed) % side;                         \
            acc += fenwick2d##T##RangeQuery(t, a / 2, b / 2, a, b);            \
        });                                                                    \
        r.lower = NAN;                                                         \
        r.bytesPerElem =                                                       \
            (double)fenwick2d##T##Bytes(t) / (double)(side * side);            \
        benchCheck_ += (uint64_t)acc;                                          \
        fenwick2d##T##Free(t);                                                 \
        return r;                                                              \
    }

BENCH_2D_TYPED(I64, int64_t)
BENCH_2D_TYPED(Double, double)

/* 'doubleEvery' > 0 makes every that-many-th value a double */
static benchRow bench2dMulti(size_t n, size_t doubleEvery) {
    benchRow r = {0};
    const size_t side = (size_t)sqrt((double)n);
    multiFenwick2d *t = NULL;
    const double t0 = benchNow_();
    for (size_t i = 0; i < side; i++) {
        for (size_t j = 0; j < side; j++) {
            const size_t k = i * side + j;
            const databox v = doubleEvery && k % doubleEvery == 0
                                  ? DATABOX_DOUBLE(0.5)
                                  : DATABOX_UNSIGNED((i + j) % 7);
            multiFenwick2dUpdate(&t, i, j, &v);
        }
    }

    r.build = (benchNow_() - t0) / (double)(side * side);
    const databox one = DATABOX_UNSIGNED(1);
    databox out;
    uint64_t acc = 0;
    BENCH_TIMED(r.update, multiFenwick2dUpdate(&t, benchRand_(&seed) % side,
                                               benchRand_(&seed) % side, &one));
    BENCH_TIMED(r.query, {
        multiFenwick2dQuery(t, benchRand_(&seed) % side,
                            benchRand_(&seed) % side, &out);
        acc += out.data.u64;
    });
    BENCH_TIMED(r.get, {
        multiFenwick2dGet(t, benchRand_(&seed) % side, benchRand_(&seed) % side,
                          &out);
        acc += out.data.u64;
    });
    BENCH_TIMED(r.range, {
        const size_t a = benchRand_(&seed) % side;
        const size_t b = benchRand_(&seed) % side;
        multiFenwick2dRangeQuery(t, a / 2, b / 2, a, b, &out);
        acc += out.data.u64;
    });
    r.lower = NAN;
    r.bytesPerElem = (double)multiFenwick2dBytes(t) / (double)(side * side);
    benchCheck_ += acc;
    multiFenwick2dFree(t);
    return r;
}

int fenwickBenchTest(int argc, char *argv[]) {
    int err = 0;
    (void)argc;
    (void)argv;

    const size_t sizes[] = {1000, 100000, 4000000};
    const size_t nSizes = sizeof(sizes) / sizeof(*sizes);
    benchRow r;
#define BENCH_ROW(label, call)                                                 \
    do {                                                                       \
        r = call;                                                              \
        benchPrint_(label, n, &r);                                             \
    } while (0)

    TEST("workload 1: counter table (ns per op, bytes per element)") {
        benchHeader_("Counter table: integer counts, +1 updates");
        for (size_t s = 0; s < nSizes; s++) {
            const size_t n = sizes[s];
            BENCH_ROW("fenwickI16", benchTypedI16(n));
            BENCH_ROW("fenwickI32", benchTypedI32(n));
            BENCH_ROW("fenwickI64", benchTypedI64(n));
#ifdef __SIZEOF_INT128__
            BENCH_ROW("fenwickI128", benchTypedI128(n));
#endif
            BENCH_ROW("fenwickFloat", benchTypedFloat(n));
            BENCH_ROW("fenwickDouble", benchTypedDouble(n));
            BENCH_ROW("multiFenwickFlat", benchMultiFlat(n, 0));
            BENCH_ROW("multiFenwick", benchMultiFenwick(n, 0));
            BENCH_ROW("segmentI64 SUM", benchSegmentI64(n, SEGMENT_OP_SUM));
        }
    }

    TEST("workload 2: mixed values, 1% doubles") {
        benchHeader_("Mixed values: 1% of values and updates are doubles");
        for (size_t s = 0; s < nSizes; s++) {
            const size_t n = sizes[s];
            BENCH_ROW("fenwickDouble", benchTypedDouble(n));
            BENCH_ROW("multiFenwickFlat", benchMultiFlat(n, 100));
            BENCH_ROW("multiFenwick", benchMultiFenwick(n, 100));
            BENCH_ROW("segmentDouble SUM",
                      benchSegmentDouble(n, SEGMENT_OP_SUM));
        }
    }

    TEST("workload 3: sparse growth from empty") {
        benchHeader_("Sparse growth: n writes at random indexes of an empty "
                     "tree ('build' = ns per write)");
        for (size_t s = 0; s < nSizes; s++) {
            const size_t n = sizes[s];
            BENCH_ROW("fenwickI64", benchGrowTypedI64(n));
            BENCH_ROW("multiFenwickFlat", benchGrowFlat(n));
            if (n <= 100000) {
                /* 4M writes at tens of microseconds each would take
                 * minutes; the 100K row already shows the trend */
                BENCH_ROW("multiFenwick", benchGrowFenwick(n));
            }

            BENCH_ROW("segmentI64 SUM", benchGrowSegmentI64(n));
        }
    }

    TEST("workload 4: range minimum") {
        benchHeader_("Range minimum ('query' = min of [0, i], 'range' = min "
                     "of [i/2, i])");
        for (size_t s = 0; s < nSizes; s++) {
            const size_t n = sizes[s];
            BENCH_ROW("segmentI64 MIN", benchSegmentI64(n, SEGMENT_OP_MIN));
            BENCH_ROW("segmentDouble MIN",
                      benchSegmentDouble(n, SEGMENT_OP_MIN));
        }
    }

    TEST("workload 5: 2D grid (about N cells)") {
        benchHeader_("2D grid: heatmap counts on a square grid of about N "
                     "cells ('range' = rectangle sum)");
        for (size_t s = 0; s < nSizes; s++) {
            const size_t n = sizes[s];
            BENCH_ROW("fenwick2dI64", bench2dI64(n));
            BENCH_ROW("fenwick2dDouble", bench2dDouble(n));
            BENCH_ROW("multiFenwick2d", bench2dMulti(n, 0));
            BENCH_ROW("multiFenwick2d, 1% doubles", bench2dMulti(n, 100));
        }
    }
#undef BENCH_ROW

    printf("\nchecksum %" PRIu64 "\n", benchCheck_);
    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
