/* fenwickBench: a speed and size matrix of every Fenwick tree.
 *
 *   datakit-test test fenwickBench      (build with the perf preset)
 *
 * Rows: the typed trees (fenwick<Type>), multiFenwick, multiFenwickFlat,
 * and the 2D trees (as a square grid of about N cells).  Columns:
 * nanoseconds per build element, update, prefix query, element get,
 * range query and lower bound (1D), and bytes per element.  Values are
 * small non-negative integers (exact in every type), indexes uniform
 * random; every operation's results feed a printed checksum so nothing
 * is optimized away.  Each cell of the matrix runs for a fixed time
 * budget (at most BENCH_MAX_OPS operations), so slow trees take fewer
 * operations, not more time.  validate skips this test (timing only). */

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
#include "fenwick/fenwickU16.h"
#include "fenwick/fenwickU32.h"
#include "fenwick/fenwickU64.h"
#ifdef __SIZEOF_INT128__
#include "fenwick/fenwickI128.h"
#include "fenwick/fenwickU128.h"
#endif
#include "multiFenwick.h"
#include "multiFenwick2d.h"
#include "multiFenwickFlat.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

#define BENCH_MAX_OPS 200000
#define BENCH_BUDGET_NS 50000000.0 /* 50 ms per matrix cell */

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

/* Run 'body' (using 'i' and 'seed') until the budget or the op cap; set
 * 'nsPerOp' */
#define BENCH_TIMED(nsPerOp, body)                                             \
    do {                                                                       \
        uint64_t seed = 99;                                                    \
        size_t ops = 0;                                                        \
        const double start = benchNow_();                                      \
        double elapsed = 0;                                                    \
        while (ops < BENCH_MAX_OPS) {                                          \
            for (size_t i = 0; i < 256; i++, ops++) {                          \
                body;                                                          \
            }                                                                  \
            elapsed = benchNow_() - start;                                     \
            if (elapsed > BENCH_BUDGET_NS) {                                   \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        (nsPerOp) = elapsed / (double)ops;                                     \
    } while (0)

typedef struct benchRow {
    double build, update, query, get, range, lower;
    double bytesPerElem;
} benchRow;

static void benchPrint_(const char *name, size_t n, const benchRow *r) {
    printf("%-18s %8zu %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %7.2f\n", name, n,
           r->build, r->update, r->query, r->get, r->range, r->lower,
           r->bytesPerElem);
}

/* Typed 1D trees: built by n Updates (they have no public array build) */
#define BENCH_TYPED(T, VT)                                                     \
    static benchRow benchTyped##T(size_t n, uint64_t *check) {                 \
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
        BENCH_TIMED(r.lower, *check += fenwick##T##LowerBound(                 \
                                 fw, (VT)(benchRand_(&seed) % (3 * n))));      \
        r.bytesPerElem = (double)fenwick##T##Bytes(fw) / (double)n;            \
        *check += (uint64_t)acc;                                               \
        fenwick##T##Free(fw);                                                  \
        return r;                                                              \
    }

BENCH_TYPED(I16, int16_t)
BENCH_TYPED(I32, int32_t)
BENCH_TYPED(I64, int64_t)
BENCH_TYPED(U16, uint16_t)
BENCH_TYPED(U32, uint32_t)
BENCH_TYPED(U64, uint64_t)
BENCH_TYPED(Float, float)
BENCH_TYPED(Double, double)
#ifdef __SIZEOF_INT128__
BENCH_TYPED(I128, __int128_t)
BENCH_TYPED(U128, __uint128_t)
#endif

/* The databox 1D trees: built from an array (O(n)) */
#define BENCH_MULTI(NAME, T)                                                   \
    static benchRow benchMulti##NAME(size_t n, uint64_t *check) {              \
        benchRow r = {0};                                                      \
        cleaner(zfree) void *valsMem = zcalloc(n, sizeof(databox));            \
        databox *vals = valsMem;                                               \
        for (size_t i = 0; i < n; i++) {                                       \
            vals[i] = DATABOX_UNSIGNED(i % 7);                                 \
        }                                                                      \
        const double t0 = benchNow_();                                         \
        T *fw = T##NewFromArray(vals, n);                                      \
        r.build = (benchNow_() - t0) / (double)n;                              \
        const databox one = DATABOX_UNSIGNED(1);                               \
        databox out;                                                           \
        uint64_t acc = 0;                                                      \
        BENCH_TIMED(r.update, T##Update(&fw, benchRand_(&seed) % n, &one));    \
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
        *check += acc;                                                         \
        T##Free(fw);                                                           \
        return r;                                                              \
    }

BENCH_MULTI(Fenwick, multiFenwick)
BENCH_MULTI(Flat, multiFenwickFlat)

/* 2D trees: a side x side grid, side = sqrt(n) */
#define BENCH_2D_TYPED(T, VT)                                                  \
    static benchRow bench2d##T(size_t n, uint64_t *check) {                    \
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
        *check += (uint64_t)acc;                                               \
        fenwick2d##T##Free(t);                                                 \
        return r;                                                              \
    }

BENCH_2D_TYPED(I64, int64_t)
BENCH_2D_TYPED(Double, double)

static benchRow bench2dMulti(size_t n, uint64_t *check) {
    benchRow r = {0};
    const size_t side = (size_t)sqrt((double)n);
    multiFenwick2d *t = NULL;
    const double t0 = benchNow_();
    for (size_t i = 0; i < side; i++) {
        for (size_t j = 0; j < side; j++) {
            const databox v = DATABOX_UNSIGNED((i + j) % 7);
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
    *check += acc;
    multiFenwick2dFree(t);
    return r;
}

int fenwickBenchTest(int argc, char *argv[]) {
    int err = 0;
    (void)argc;
    (void)argv;

    TEST("Fenwick tree matrix: ns per operation and bytes per element") {
        const size_t sizes[] = {1000, 16 * 1024, 100000, 1000000};
        uint64_t check = 0;
        printf("\n%-18s %8s %8s %8s %8s %8s %8s %8s %7s\n", "tree", "N",
               "build", "update", "query", "get", "range", "lower", "B/elem");
        for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
            const size_t n = sizes[s];
            benchRow r;
#define BENCH_ROW(label, call)                                                 \
    do {                                                                       \
        r = call;                                                              \
        benchPrint_(label, n, &r);                                             \
    } while (0)
            BENCH_ROW("fenwickI16", benchTypedI16(n, &check));
            BENCH_ROW("fenwickI32", benchTypedI32(n, &check));
            BENCH_ROW("fenwickI64", benchTypedI64(n, &check));
            BENCH_ROW("fenwickU16", benchTypedU16(n, &check));
            BENCH_ROW("fenwickU32", benchTypedU32(n, &check));
            BENCH_ROW("fenwickU64", benchTypedU64(n, &check));
#ifdef __SIZEOF_INT128__
            BENCH_ROW("fenwickI128", benchTypedI128(n, &check));
            BENCH_ROW("fenwickU128", benchTypedU128(n, &check));
#endif
            BENCH_ROW("fenwickFloat", benchTypedFloat(n, &check));
            BENCH_ROW("fenwickDouble", benchTypedDouble(n, &check));
            BENCH_ROW("multiFenwickFlat", benchMultiFlat(n, &check));
            BENCH_ROW("multiFenwick", benchMultiFenwick(n, &check));
            BENCH_ROW("fenwick2dI64", bench2dI64(n, &check));
            BENCH_ROW("fenwick2dDouble", bench2dDouble(n, &check));
            BENCH_ROW("multiFenwick2d", bench2dMulti(n, &check));
#undef BENCH_ROW
            printf("\n");
        }

        printf("checksum %" PRIu64 "\n", check);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
