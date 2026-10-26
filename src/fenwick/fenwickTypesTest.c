/* Differential tests for every typed Fenwick tree against a naive array.
 *
 * Contract checked (fenwickCore.h, "VALUES"):
 *   - Elements are stored values: an Update whose new element would not fit
 *     the value type is refused (returns false) and changes nothing; float
 *     trees also refuse non-finite deltas, values and elements.
 *   - Get returns the element exactly (float trees: while every partial
 *     sum is exact, which the float cases below keep to).
 *   - Query and RangeQuery return the exact sum when it fits the type; an
 *     integer sum that does not fit comes back modulo 2^bits (never UB).
 *   - Indexes past the count extend it; the tier change at the Small
 *     tier's limit keeps every value. */

#include "../cleaner.h"
#include "../ctest.h"
#include "../datakit.h"
#include "fenwickDouble.h"
#include "fenwickFloat.h"
#include "fenwickI16.h"
#include "fenwickI32.h"
#include "fenwickI64.h"
#include "fenwickU16.h"
#include "fenwickU32.h"
#include "fenwickU64.h"
#ifdef __SIZEOF_INT128__
#include "fenwickI128.h"
#include "fenwickU128.h"
#endif

#include <float.h>
#include <inttypes.h>
#include <math.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

/* splitmix64 */
static uint64_t fwtRand_(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* An index: mostly small, sometimes far past the end (sparse growth), and
 * now and then across the Small tier's limit. */
static size_t fwtIndex_(uint64_t *s) {
    const uint64_t r = fwtRand_(s) % 64;
    if (r == 0) {
        return 40000 + (size_t)(fwtRand_(s) % 64);
    }

    if (r < 4) {
        return 1000 + (size_t)(fwtRand_(s) % 2000);
    }

    return (size_t)(fwtRand_(s) % 300);
}

static constexpr int FWT_ELEMS = 41000;
static constexpr int FWT_OPS = 2500;

/* ====================================================================
 * Integer types
 * ====================================================================
 * The oracle keeps elements exactly; prefix sums are taken in the
 * unsigned type of the same width, which is the sum modulo 2^bits. */
#define FWT_INT_DIFF(T, VT, UT, VMIN, VMAX)                                    \
    static int fwtDiff##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        cleaner(zfree) void *elemMem = zcalloc(FWT_ELEMS, sizeof(VT));         \
        VT *elem = elemMem;                                                    \
        size_t count = 0;                                                      \
        void *fw = NULL;                                                       \
        for (size_t op = 0; op < FWT_OPS && !err; op++) {                      \
            const size_t idx = fwtIndex_(&seed);                               \
            const uint64_t pick = fwtRand_(&seed) % 10;                        \
            VT v;                                                              \
            if (pick < 6) {                                                    \
                v = (VT)((int64_t)(fwtRand_(&seed) % 101) - 50);               \
                if ((VT) - 1 > 0) {                                            \
                    v = (VT)(fwtRand_(&seed) % 51);                            \
                }                                                              \
            } else if (pick == 6) {                                            \
                v = (VT)(VMAX - (VT)(fwtRand_(&seed) % 3));                    \
            } else if (pick == 7) {                                            \
                v = (VT)(VMIN + (VT)(fwtRand_(&seed) % 3));                    \
            } else if (pick == 8) {                                            \
                v = (VT)fwtRand_(&seed);                                       \
            } else {                                                           \
                v = 0;                                                         \
            }                                                                  \
                                                                               \
            if (fwtRand_(&seed) % 4 == 0) {                                    \
                if (!fenwick##T##Set(&fw, idx, v)) {                           \
                    ERR(#T " op %zu: Set(%zu) refused", op, idx);              \
                }                                                              \
                elem[idx] = v;                                                 \
                count = idx + 1 > count ? idx + 1 : count;                     \
            } else {                                                           \
                VT want;                                                       \
                const bool fits = !ckd_add(&want, elem[idx], v);               \
                const size_t countBefore = fenwick##T##Count(fw);              \
                if (fenwick##T##Update(&fw, idx, v) != fits) {                 \
                    ERR(#T " op %zu: Update(%zu) %s", op, idx,                 \
                        fits ? "refused an in-range element"                   \
                             : "accepted an out-of-range element");            \
                }                                                              \
                if (fits) {                                                    \
                    elem[idx] = want;                                          \
                    count = idx + 1 > count ? idx + 1 : count;                 \
                } else if (fenwick##T##Count(fw) != countBefore) {             \
                    ERR(#T " op %zu: refused Update changed the count", op);   \
                }                                                              \
            }                                                                  \
                                                                               \
            if (fenwick##T##Count(fw) != count) {                              \
                ERR(#T " op %zu: count %zu, want %zu", op,                     \
                    fenwick##T##Count(fw), count);                             \
            }                                                                  \
                                                                               \
            /* Spot checks: element, prefix and range (the oracle's sums       \
             * are O(q), so they stay low except every 256th op) */            \
            for (int k = 0; k < 4 && count > 0 && !err; k++) {                 \
                const size_t span = count < 1024 ? count : 1024;               \
                const size_t q = k == 0 && op % 256 == 255                     \
                                     ? count - 1                               \
                                     : (size_t)(fwtRand_(&seed) % span);       \
                const size_t l = (size_t)(fwtRand_(&seed) % (q + 1));          \
                UT pre = 0;                                                    \
                UT preL = 0;                                                   \
                for (size_t i = 0; i <= q; i++) {                              \
                    pre += (UT)elem[i];                                        \
                    if (i + 1 == l) {                                          \
                        preL = pre;                                            \
                    }                                                          \
                }                                                              \
                if (fenwick##T##Get(fw, q) != elem[q]) {                       \
                    ERR(#T " op %zu: Get(%zu) wrong", op, q);                  \
                }                                                              \
                if (fenwick##T##Query(fw, q) != (VT)pre) {                     \
                    ERR(#T " op %zu: Query(%zu) wrong", op, q);                \
                }                                                              \
                if (fenwick##T##RangeQuery(fw, l, q) != (VT)(pre - preL)) {    \
                    ERR(#T " op %zu: RangeQuery(%zu, %zu) wrong", op, l, q);   \
                }                                                              \
            }                                                                  \
        }                                                                      \
                                                                               \
        /* Full sweep, then Clear keeps the count and zeroes every element */  \
        for (size_t i = 0; i < count && !err; i++) {                           \
            if (fenwick##T##Get(fw, i) != elem[i]) {                           \
                ERR(#T " sweep: Get(%zu) wrong", i);                           \
            }                                                                  \
        }                                                                      \
        /* The tree holds at least one cell per element */                     \
        if (count && fenwick##T##Bytes(fw) < count * sizeof(VT)) {             \
            ERR(#T ": Bytes %zu below %zu elements", fenwick##T##Bytes(fw),    \
                count);                                                        \
        }                                                                      \
        fenwick##T##Clear(fw);                                                 \
        if (count && (fenwick##T##Count(fw) != count ||                        \
                      fenwick##T##Query(fw, count - 1) != 0 ||                 \
                      fenwick##T##Get(fw, count / 2) != 0)) {                  \
            ERR(#T ": Clear did not zero the tree and keep the count");        \
        }                                                                      \
        fenwick##T##Free(fw);                                                  \
        return err;                                                            \
    }

/* LowerBound over non-negative elements: the smallest index whose prefix
 * sum reaches the target, or SIZE_MAX when none does. */
#define FWT_LOWER_BOUND(T, VT)                                                 \
    static int fwtLowerBound##T(uint64_t seed) {                               \
        int err = 0;                                                           \
        constexpr int N = 200;                                                 \
        VT elem[N];                                                            \
        void *fw = NULL;                                                       \
        for (size_t i = 0; i < N; i++) {                                       \
            elem[i] = (VT)(fwtRand_(&seed) % 4);                               \
            fenwick##T##Update(&fw, i, elem[i]);                               \
        }                                                                      \
        for (int t = 0; t < 200 && !err; t++) {                                \
            const VT target = (VT)(fwtRand_(&seed) % 340);                     \
            size_t want = SIZE_MAX;                                            \
            VT pre = 0;                                                        \
            for (size_t i = 0; i < N; i++) {                                   \
                pre += elem[i];                                                \
                if (pre >= target) {                                           \
                    want = i;                                                  \
                    break;                                                     \
                }                                                              \
            }                                                                  \
            const size_t got = fenwick##T##LowerBound(fw, target);             \
            if (got != want) {                                                 \
                ERR(#T " LowerBound: got %zu, want %zu", got, want);           \
            }                                                                  \
        }                                                                      \
        fenwick##T##Free(fw);                                                  \
        return err;                                                            \
    }

FWT_INT_DIFF(I16, int16_t, uint16_t, INT16_MIN, INT16_MAX)
FWT_INT_DIFF(I32, int32_t, uint32_t, INT32_MIN, INT32_MAX)
FWT_INT_DIFF(I64, int64_t, uint64_t, INT64_MIN, INT64_MAX)
FWT_INT_DIFF(U16, uint16_t, uint16_t, 0, UINT16_MAX)
FWT_INT_DIFF(U32, uint32_t, uint32_t, 0, UINT32_MAX)
FWT_INT_DIFF(U64, uint64_t, uint64_t, 0, UINT64_MAX)
FWT_LOWER_BOUND(I16, int16_t)
FWT_LOWER_BOUND(I32, int32_t)
FWT_LOWER_BOUND(I64, int64_t)
FWT_LOWER_BOUND(U16, uint16_t)
FWT_LOWER_BOUND(U32, uint32_t)
FWT_LOWER_BOUND(U64, uint64_t)
#ifdef __SIZEOF_INT128__
#define FWT_I128_MAX ((__int128_t)(~(__uint128_t)0 >> 1))
#define FWT_I128_MIN (-FWT_I128_MAX - 1)
FWT_INT_DIFF(I128, __int128_t, __uint128_t, FWT_I128_MIN, FWT_I128_MAX)
FWT_INT_DIFF(U128, __uint128_t, __uint128_t, 0, ~(__uint128_t)0)
FWT_LOWER_BOUND(I128, __int128_t)
FWT_LOWER_BOUND(U128, __uint128_t)
#endif

/* ====================================================================
 * Float types
 * ====================================================================
 * Values are small integers, so every sum is exact in the type and the
 * oracle can compare exactly. */
#define FWT_FLOAT_DIFF(T, VT, VMAX)                                            \
    static int fwtDiff##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        cleaner(zfree) void *elemMem = zcalloc(FWT_ELEMS, sizeof(VT));         \
        VT *elem = elemMem;                                                    \
        size_t count = 0;                                                      \
        void *fw = NULL;                                                       \
        for (size_t op = 0; op < FWT_OPS && !err; op++) {                      \
            const size_t idx = fwtIndex_(&seed) % 300;                         \
            const VT v = (VT)((int64_t)(fwtRand_(&seed) % 129) - 64);          \
            if (fwtRand_(&seed) % 4 == 0) {                                    \
                if (!fenwick##T##Set(&fw, idx, v)) {                           \
                    ERR(#T " op %zu: Set refused", op);                        \
                }                                                              \
                elem[idx] = v;                                                 \
            } else {                                                           \
                if (!fenwick##T##Update(&fw, idx, v)) {                        \
                    ERR(#T " op %zu: Update refused", op);                     \
                }                                                              \
                elem[idx] += v;                                                \
            }                                                                  \
            count = idx + 1 > count ? idx + 1 : count;                         \
                                                                               \
            const size_t q = (size_t)(fwtRand_(&seed) % count);                \
            const size_t l = (size_t)(fwtRand_(&seed) % (q + 1));              \
            VT pre = 0;                                                        \
            VT preL = 0;                                                       \
            for (size_t i = 0; i <= q; i++) {                                  \
                pre += elem[i];                                                \
                if (i + 1 == l) {                                              \
                    preL = pre;                                                \
                }                                                              \
            }                                                                  \
            /* Small integer elements: every sum is exact in VT */             \
            if (fenwick##T##Get(fw, q) != elem[q] ||                           \
                fenwick##T##Query(fw, q) != pre ||                             \
                fenwick##T##RangeQuery(fw, l, q) != pre - preL) {              \
                ERR(#T " op %zu: Get/Query/RangeQuery(%zu) wrong", op, q);     \
            }                                                                  \
        }                                                                      \
        if (fenwick##T##Bytes(fw) < count * sizeof(VT)) {                      \
            ERR(#T ": Bytes %zu below %zu elements", fenwick##T##Bytes(fw),    \
                count);                                                        \
        }                                                                      \
                                                                               \
        /* Non-finite deltas, values and elements are refused */               \
        const size_t before = fenwick##T##Count(fw);                           \
        const VT big = VMAX;                                                   \
        if (fenwick##T##Update(&fw, 0, (VT)NAN) ||                             \
            fenwick##T##Update(&fw, 0, (VT)INFINITY) ||                        \
            fenwick##T##Set(&fw, 0, (VT) - INFINITY) ||                        \
            !fenwick##T##Set(&fw, 1, big) ||                                   \
            fenwick##T##Update(&fw, 1, big) ||                                 \
            fenwick##T##Get(fw, 0) != elem[0] ||                               \
            fenwick##T##Get(fw, 1) != big ||                                   \
            fenwick##T##Count(fw) != before) {                                 \
            ERR(#T ": a non-finite delta, value or element was accepted");     \
        }                                                                      \
                                                                               \
        /* Growth across the Small tier's limit keeps every element */         \
        if (!fenwick##T##Update(&fw, 40000, (VT)3) ||                          \
            fenwick##T##Get(fw, 40000) != (VT)3 ||                             \
            fenwick##T##Get(fw, 0) != elem[0] ||                               \
            fenwick##T##Get(fw, 1) != big) {                                   \
            ERR(#T ": growth lost a value");                                   \
        }                                                                      \
                                                                               \
        /* Clear keeps the count and zeroes every element */                   \
        const size_t grown = fenwick##T##Count(fw);                            \
        fenwick##T##Clear(fw);                                                 \
        if (fenwick##T##Count(fw) != grown ||                                  \
            fenwick##T##Get(fw, 1) != (VT)0 ||                                 \
            fenwick##T##Query(fw, grown - 1) != (VT)0) {                       \
            ERR(#T ": Clear did not zero the tree and keep the count");        \
        }                                                                      \
                                                                               \
        fenwick##T##Free(fw);                                                  \
        return err;                                                            \
    }

FWT_FLOAT_DIFF(Float, float, FLT_MAX)
FWT_FLOAT_DIFF(Double, double, DBL_MAX)
FWT_LOWER_BOUND(Float, float)
FWT_LOWER_BOUND(Double, double)

/* NewFromArray and CopyTo against the array they came from and a tree
 * built with one Set per element.  Sizes cover empty, tiny and past the
 * Small tier; float values are small integers, so every sum is exact. */
#define FWT_BULK(T, VT, FLOATING)                                              \
    static int fwtBulk##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        static constexpr size_t sizes[] = {0, 1, 5, 1000, 40000};              \
        for (size_t s = 0; s < countof(sizes); s++) {                          \
            const size_t n = sizes[s];                                         \
            cleaner(zfree) void *bufA = zcalloc(n + 1, sizeof(VT));            \
            cleaner(zfree) void *bufB = zcalloc(n + 1, sizeof(VT));            \
            VT *values = bufA;                                                 \
            VT *copied = bufB;                                                 \
            void *bySet = NULL;                                                \
            for (size_t i = 0; i < n; i++) {                                   \
                const uint64_t r = fwtRand_(&seed);                            \
                /* floats: |sum| <= 400000, well inside float's 2^24 */        \
                values[i] = FLOATING ? (VT)((int64_t)(r % 21) - 10) : (VT)r;   \
                (void)fenwick##T##Set(&bySet, i, values[i]);                   \
            }                                                                  \
                                                                               \
            void *fw = fenwick##T##NewFromArray(n ? values : NULL, n);         \
            fenwick##T##CopyTo(fw, copied);                                    \
            if (!fw || fenwick##T##Count(fw) != n ||                           \
                memcmp(copied, values, n * sizeof(VT)) != 0) {                 \
                ERR(#T ": NewFromArray/CopyTo of %zu elements", n);            \
            }                                                                  \
                                                                               \
            for (size_t i = 0; i < n; i += 1 + n / 50) {                       \
                if (fenwick##T##Get(fw, i) != values[i] ||                     \
                    fenwick##T##Query(fw, i) != fenwick##T##Query(bySet, i)) { \
                    ERR(#T ": index %zu of %zu differs from the Set tree", i,  \
                        n);                                                    \
                    break;                                                     \
                }                                                              \
            }                                                                  \
                                                                               \
            fenwick##T##Free(fw);                                              \
            fenwick##T##Free(bySet);                                           \
        }                                                                      \
                                                                               \
        return err;                                                            \
    }

FWT_BULK(I16, int16_t, 0)
FWT_BULK(I32, int32_t, 0)
FWT_BULK(I64, int64_t, 0)
FWT_BULK(U16, uint16_t, 0)
FWT_BULK(U32, uint32_t, 0)
FWT_BULK(U64, uint64_t, 0)
#ifdef __SIZEOF_INT128__
FWT_BULK(I128, __int128_t, 0)
FWT_BULK(U128, __uint128_t, 0)
#endif
FWT_BULK(Float, float, 1)
FWT_BULK(Double, double, 1)

int fenwickTypesTest(int, char *[]) {
    int err = 0;

#define FWT_RUN(T)                                                             \
    TEST("differential: fenwick" #T " against a naive array") {                \
        int typeErr = 0; /* every type runs, whatever failed before */         \
        for (uint64_t seed = 1; seed <= 3 && !typeErr; seed++) {               \
            typeErr += fwtDiff##T(seed);                                       \
        }                                                                      \
        err += typeErr;                                                        \
    }                                                                          \
    TEST("LowerBound: fenwick" #T " over non-negative elements") {             \
        err += fwtLowerBound##T(7);                                            \
    }                                                                          \
    TEST("NewFromArray and CopyTo: fenwick" #T) {                              \
        err += fwtBulk##T(11);                                                 \
    }                                                                          \
    TEST("Repr: fenwick" #T " nil, Small and Full") {                          \
        fenwick##T##Repr(NULL);                                                \
        void *fw = NULL;                                                       \
        fenwick##T##Update(&fw, 3, 1);                                         \
        fenwick##T##Repr(fw);                                                  \
        fenwick##T##Update(&fw, 1 << 17, 1);                                   \
        fenwick##T##Repr(fw);                                                  \
        fenwick##T##Free(fw);                                                  \
    }

    FWT_RUN(I16)
    FWT_RUN(I32)
    FWT_RUN(I64)
    FWT_RUN(U16)
    FWT_RUN(U32)
    FWT_RUN(U64)
#ifdef __SIZEOF_INT128__
    FWT_RUN(I128)
    FWT_RUN(U128)
#endif
    FWT_RUN(Float)
    FWT_RUN(Double)
#undef FWT_RUN

    TEST_FINAL_RESULT;
}
