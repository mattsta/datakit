#include "multiFenwick.h"
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "fenwick/fenwickI64.h" /* Updated for new 2-tier system */
#include "multiFenwickCommon.h"
#include "multiFenwickFlat.h"
#include "multilist.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(databox *, zfree, DataboxArray);
defcleaner(multiFenwick *, multiFenwickFree);
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);
defcleanerName(int64_t *, zfree, I64Array);

/* Naive implementation for benchmark comparison */
typedef struct {
    databox *values;
    size_t count;
} naiveDataboxArray;

static naiveDataboxArray *naiveNewDatabox(size_t n, databox zero) {
    /* @own: caller; free=naiveFreeDatabox */
    naiveDataboxArray *arr = zmalloc(sizeof(*arr));
    arr->count = n;
    /* @own: container; free=naiveFreeDatabox */
    arr->values = zcalloc(n, sizeof(databox));
    for (size_t i = 0; i < n; i++) {
        arr->values[i] = zero;
    }
    return arr;
}

static void naiveFreeDatabox(naiveDataboxArray *arr) {
    if (arr) {
        if (arr->values) {
            zfree(arr->values);
        }
        zfree(arr);
    }
}

static bool naiveQueryDatabox(const naiveDataboxArray *arr, size_t idx,
                              databox *result) {
    if (!result) {
        return false;
    }

    *result = databoxZeroLike(&arr->values[0]);

    size_t limit = (idx >= arr->count) ? arr->count - 1 : idx;
    for (size_t i = 0; i <= limit; i++) {
        databox temp;
        if (!databoxAdd(result, &arr->values[i], &temp)) {
            return false;
        }
        *result = temp;
    }
    return true;
}

static uint64_t randSeed(uint64_t *seed) {
    *seed = (*seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *seed;
}

/* 'box' is the 64-bit integer 'want'.  Flex stores integers canonically,
 * so a non-negative value may come back as either SIGNED_64 or
 * UNSIGNED_64; any other type, or a wrapped value, is a mismatch. */
static bool mfwIntIs(const databox *box, int64_t want) {
    if (box->type == DATABOX_SIGNED_64) {
        return box->data.i64 == want;
    }

    if (box->type == DATABOX_UNSIGNED_64) {
        return want >= 0 && box->data.u64 == (uint64_t)want;
    }

    return false;
}

/* 'box' is the integer 'want' (in either 64-bit integer type) */
static bool mfwBoxIs128(const databox *box, __int128_t want) {
    if (box->type == DATABOX_SIGNED_64) {
        return box->data.i64 == want;
    }

    if (box->type == DATABOX_UNSIGNED_64) {
        return want >= 0 && box->data.u64 == (uint64_t)want;
    }

    return false;
}

static bool mfwVoid(databox box) {
    return DATABOX_IS_VOID(&box);
}

static bool mfwInt(databox box, __int128_t want) {
    return mfwBoxIs128(&box, want);
}

/* One call site each for the header's inline helpers, which the helper
 * test calls dozens of times (gcc -Winline warns on every call it will
 * not inline); noinline keeps gcc from folding these back in */
[[gnu::noinline]] static bool mfwAdd(const databox *a, const databox *b,
                                     databox *r) {
    return databoxAdd(a, b, r);
}

[[gnu::noinline]] static bool mfwSub(const databox *a, const databox *b,
                                     databox *r) {
    return databoxSubtract(a, b, r);
}

[[gnu::noinline]] static int mfwCmp(const databox *a, const databox *b) {
    return databoxCompareNumeric(a, b);
}

[[gnu::noinline]] static databox mfwConvert(const databox *src,
                                            databoxType type) {
    return databoxConvertToType(src, type);
}

[[gnu::noinline]] static databoxType mfwResultType(const databox *a,
                                                   const databox *b) {
    return databoxResultType(a, b);
}

int multiFenwickTest(int, char *[]) {
    int err = 0;

    TEST("helpers: databox arithmetic and conversion against exact oracles") {
        const databox s = DATABOX_SIGNED(-7);
        const databox u = DATABOX_UNSIGNED(UINT64_MAX);
        const databox big = DATABOX_SIGNED((INT64_C(1) << 53) + 1);
        const databox d = DATABOX_DOUBLE(0x1p53);
        const databox half = DATABOX_DOUBLE(2.5);
        const databox v = DATABOX_BOX_VOID;
        const databox bytes = DATABOX_WITH_BYTES((void *)"xy", 2);
        databox f;
        DATABOX_SET_FLOAT(&f, 1.5f);
        databox r;

        /* Integers add and subtract exactly; the result's type follows
         * its value */
        static constexpr int64_t ints[] = {INT64_MIN, -3, 0, 5, INT64_MAX};
        for (size_t i = 0; i < 5; i++) {
            for (size_t j = 0; j < 5; j++) {
                const databox a = DATABOX_SIGNED(ints[i]);
                const databox b = DATABOX_SIGNED(ints[j]);
                __int128_t sum = (__int128_t)ints[i] + ints[j];
                __int128_t diff = (__int128_t)ints[i] - ints[j];
                /* Past the 64-bit range the helpers wrap modulo 2^64 */
                if (sum > (__int128_t)UINT64_MAX || sum < INT64_MIN) {
                    continue;
                }
                if (!mfwAdd(&a, &b, &r) || !mfwBoxIs128(&r, sum)) {
                    ERR("add %" PRId64 " + %" PRId64, ints[i], ints[j]);
                }
                if (diff <= (__int128_t)UINT64_MAX && diff >= INT64_MIN &&
                    (!mfwSub(&a, &b, &r) || !mfwBoxIs128(&r, diff))) {
                    ERR("subtract %" PRId64 " - %" PRId64, ints[i], ints[j]);
                }
                const int cmp = mfwCmp(&a, &b);
                if ((cmp > 0) != (ints[i] > ints[j]) ||
                    (cmp < 0) != (ints[i] < ints[j])) {
                    ERR("compare %" PRId64 " %" PRId64, ints[i], ints[j]);
                }
            }
        }
        if (!mfwAdd(&s, &u, &r) ||
            !mfwBoxIs128(&r, (__int128_t)UINT64_MAX - 7) ||
            !mfwSub(&s, &u, &r) ||
            !mfwBoxIs128(&r,
                         (__int128_t)-7 - UINT64_MAX + ((__int128_t)1 << 64))) {
            ERR("mixed signed/unsigned add or subtract");
        }

        /* VOID is zero; anything not a number is refused */
        if (!mfwAdd(&v, &v, &r) || !DATABOX_IS_VOID(&r) ||
            !mfwAdd(&v, &s, &r) || !mfwBoxIs128(&r, -7) ||
            !mfwAdd(&s, &v, &r) || !mfwBoxIs128(&r, -7) ||
            !mfwSub(&v, &v, &r) || !DATABOX_IS_VOID(&r) ||
            !mfwSub(&v, &s, &r) || !mfwBoxIs128(&r, 7) ||
            !mfwSub(&v, &half, &r) || r.data.d64 != -2.5 ||
            !mfwSub(&s, &v, &r) || !mfwBoxIs128(&r, -7) ||
            mfwSub(&v, &bytes, &r) || mfwAdd(&s, &bytes, &r) ||
            mfwSub(&bytes, &s, &r) || mfwAdd(NULL, &s, &r) ||
            mfwSub(&s, NULL, &r)) {
            ERR("VOID / non-numeric operands");
        }

        /* Floats: the wider float type wins */
        if (!mfwAdd(&s, &half, &r) || r.type != DATABOX_DOUBLE_64 ||
            r.data.d64 != -4.5 || !mfwAdd(&f, &s, &r) ||
            r.type != DATABOX_FLOAT_32 || r.data.f32 != -5.5f ||
            !mfwSub(&half, &f, &r) || r.type != DATABOX_DOUBLE_64 ||
            r.data.d64 != 1.0 ||
            mfwResultType(&f, &half) != DATABOX_DOUBLE_64 ||
            mfwResultType(&u, &u) != DATABOX_UNSIGNED_64 ||
            mfwResultType(&s, &u) != DATABOX_SIGNED_64 ||
            mfwResultType(&v, &f) != DATABOX_FLOAT_32 ||
            mfwResultType(&f, &v) != DATABOX_FLOAT_32 ||
            mfwResultType(&bytes, &s) != DATABOX_BYTES) {
            ERR("float arithmetic or result type");
        }

        /* Compare: exact between an integer and a double, NaN and
         * non-numbers equal, VOID lowest */
        const databox nan = DATABOX_DOUBLE(NAN);
        if (mfwCmp(&big, &d) <= 0 || mfwCmp(&d, &big) >= 0 ||
            mfwCmp(&s, &half) >= 0 || mfwCmp(&f, &half) >= 0 ||
            mfwCmp(&half, &f) <= 0 || mfwCmp(&f, &f) != 0 ||
            mfwCmp(&u, &u) != 0 || mfwCmp(&half, &half) != 0 ||
            mfwCmp(&nan, &s) != 0 || mfwCmp(&bytes, &s) != 0 ||
            mfwCmp(&bytes, &bytes) != 0 || mfwCmp(&v, &s) >= 0 ||
            mfwCmp(&s, &v) <= 0 || mfwCmp(&v, &v) != 0 ||
            mfwCmp(NULL, &s) != 0) {
            ERR("compare");
        }

        /* Conversions: exact between integers, refused where the target
         * cannot hold the value */
        databox c = mfwConvert(&big, DATABOX_UNSIGNED_64);
        if (c.type != DATABOX_UNSIGNED_64 ||
            c.data.u64 != (UINT64_C(1) << 53) + 1 ||
            !mfwVoid(mfwConvert(&s, DATABOX_UNSIGNED_64)) ||
            !mfwVoid(mfwConvert(&u, DATABOX_SIGNED_64)) ||
            !mfwVoid(mfwConvert(&nan, DATABOX_SIGNED_64)) ||
            !mfwVoid(mfwConvert(&bytes, DATABOX_DOUBLE_64)) ||
            !mfwVoid(mfwConvert(&s, DATABOX_VOID)) ||
            !mfwVoid(mfwConvert(NULL, DATABOX_DOUBLE_64)) ||
            !mfwInt(mfwConvert(&half, DATABOX_SIGNED_64), 2) ||
            !mfwInt(mfwConvert(&v, DATABOX_UNSIGNED_64), 0) ||
            mfwConvert(&s, DATABOX_DOUBLE_64).data.d64 != -7.0 ||
            mfwConvert(&half, DATABOX_FLOAT_32).data.f32 != 2.5f ||
            mfwConvert(&f, DATABOX_DOUBLE_64).data.d64 != 1.5 ||
            mfwConvert(&half, DATABOX_DOUBLE_64).data.d64 != 2.5 ||
            !mfwVoid(mfwConvert(&half, DATABOX_BYTES))) {
            ERR("convert");
        }
        static constexpr double outside[] = {0x1p63, -0x1p64, 0x1p64, INFINITY,
                                             NAN};
        for (size_t i = 0; i < 5; i++) {
            const databox sv = databoxFromDouble(outside[i], DATABOX_SIGNED_64);
            const databox uv = databoxFromDouble(i == 1 ? -1.0 : outside[i],
                                                 DATABOX_UNSIGNED_64);
            if (!DATABOX_IS_VOID(&sv) || (i != 0 && !DATABOX_IS_VOID(&uv))) {
                ERR("from double %g kept a value", outside[i]);
            }
        }
        if (!mfwInt(databoxFromDouble(-0x1p63, DATABOX_SIGNED_64), INT64_MIN) ||
            databoxFromDouble(0x1p63, DATABOX_UNSIGNED_64).data.u64 !=
                UINT64_C(1) << 63 ||
            !mfwVoid(databoxFromDouble(1.0, DATABOX_BYTES))) {
            ERR("from double at the integer bounds");
        }

        /* Zeros and double views */
        double out = -1.0;
        static const databoxType types[] = {
            DATABOX_SIGNED_64, DATABOX_UNSIGNED_64, DATABOX_FLOAT_32,
            DATABOX_DOUBLE_64};
        for (size_t i = 0; i < 4; i++) {
            const databox z = databoxZeroOfType(types[i]);
            const databox zl = databoxZeroLike(&z);
            if (z.type != types[i] || zl.type != types[i] ||
                !databoxToDouble(&z, &out) || out != 0.0) {
                ERR("zero of type %d", (int)types[i]);
            }
        }
        if (!mfwVoid(databoxZeroOfType(DATABOX_BYTES)) ||
            !mfwVoid(databoxZeroLike(&bytes)) ||
            !mfwVoid(databoxZeroLike(NULL)) || !mfwVoid(databoxZeroLike(&v)) ||
            databoxToDouble(&bytes, &out) || databoxToDouble(NULL, &out) ||
            !databoxToDouble(&v, &out) || out != 0.0 ||
            !databoxToDouble(&u, &out) || out != 0x1p64) {
            ERR("zeros / double views");
        }

        /* The trees' exact int/double compare at its edges */
        if (multiFenwickCompareIntDouble_(0, 0x1p100) != -1 ||
            multiFenwickCompareIntDouble_(0, -0x1p100) != 1 ||
            multiFenwickCompareIntDouble_(3, 3.0) != 0 ||
            multiFenwickCompareIntDouble_(3, 3.5) != -1 ||
            multiFenwickCompareIntDouble_(3, 2.5) != 1 ||
            multiFenwickCompareIntDouble_(-3, -3.5) != 1 ||
            multiFenwickCompareIntDouble_(-3, -2.5) != -1 ||
            multiFenwickCompareIntDouble_(((__int128_t)1 << 53) + 1, 0x1p53) !=
                1) {
            ERR("int/double compare");
        }
        const multiFenwickNum ni = {.i = 4};
        const multiFenwickNum nd = {.d = 4.5, .isFloat = true};
        const multiFenwickNum mixed = {.i = 4, .d = 0.5, .isFloat = true};
        if (multiFenwickNumCompare_(ni, nd) >= 0 ||
            multiFenwickNumCompare_(nd, ni) <= 0 ||
            multiFenwickNumCompare_(mixed, nd) != 0 ||
            multiFenwickNumCompare_(ni, ni) != 0) {
            ERR("num compare");
        }
    }

    /* =================================================================
     * CATEGORY 1: BASIC OPERATIONS - SIGNED INT64 (6 tests)
     * ================================================================= */

    TEST("basic int64: empty tree operations") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        if (multiFenwickCount(mfw) != 0) {
            ERR("Empty tree should have count 0, got %zu",
                multiFenwickCount(mfw));
        }

        /* Query on empty tree should return VOID */
        databox result;
        if (multiFenwickQuery(mfw, 0, &result)) {
            ERR("Query on empty tree should fail");
        }
    }

    TEST("basic int64: single element operations") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        bool success;

        /* Add single element */
        databox val = DATABOX_SIGNED(42);
        success = multiFenwickUpdate(&mfw, 0, &val);
        if (!success) {
            ERR("Failed to update element 0");
        }

        if (multiFenwickCount(mfw) != 1) {
            ERR("Count should be 1, got %zu", multiFenwickCount(mfw));
        }

        /* Query should return the element */
        databox query;
        if (!multiFenwickQuery(mfw, 0, &query) || query.data.i64 != 42) {
            ERR("Query(0) should be 42, got %" PRId64, query.data.i64);
        }

        /* Get should return the element */
        databox get;
        if (!multiFenwickGet(mfw, 0, &get) || get.data.i64 != 42) {
            ERR("Get(0) should be 42, got %" PRId64, get.data.i64);
        }

        /* Update again (add delta) */
        databox delta = DATABOX_SIGNED(8);
        success = multiFenwickUpdate(&mfw, 0, &delta);
        if (!success) {
            ERR("Failed to update element 0 again");
        }

        if (!multiFenwickGet(mfw, 0, &get) || get.data.i64 != 50) {
            ERR("After adding 8, Get(0) should be 50, got %" PRId64,
                get.data.i64);
        }
    }

    TEST("basic int64: sequential updates") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Build array [1, 2, 3, 4, 5] */
        for (size_t i = 0; i < 5; i++) {
            databox val = DATABOX_SIGNED((int64_t)(i + 1));
            multiFenwickUpdate(&mfw, i, &val);
        }

        if (multiFenwickCount(mfw) != 5) {
            ERR("Count should be 5, got %zu", multiFenwickCount(mfw));
        }

        /* Verify prefix sums: [1, 3, 6, 10, 15] */
        int64_t expected[] = {1, 3, 6, 10, 15};
        for (size_t i = 0; i < 5; i++) {
            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum) ||
                sum.data.i64 != expected[i]) {
                ERR("Query(%zu) should be %" PRId64 ", got %" PRId64, i,
                    expected[i], sum.data.i64);
            }
        }

        /* Verify individual elements */
        for (size_t i = 0; i < 5; i++) {
            databox val;
            if (!multiFenwickGet(mfw, i, &val) ||
                val.data.i64 != (int64_t)(i + 1)) {
                ERR("Get(%zu) should be %" PRId64 ", got %" PRId64, i,
                    (int64_t)(i + 1), val.data.i64);
            }
        }
    }

    TEST("basic int64: prefix sum correctness") {
        databox values[8];
        int64_t vals[] = {3, 1, 4, 1, 5, 9, 2, 6};
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 8);

        /* Prefix sums: [3, 4, 8, 9, 14, 23, 25, 31] */
        int64_t expected[] = {3, 4, 8, 9, 14, 23, 25, 31};
        for (size_t i = 0; i < 8; i++) {
            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum) ||
                sum.data.i64 != expected[i]) {
                ERR("Prefix sum at %zu should be %" PRId64 ", got %" PRId64, i,
                    expected[i], sum.data.i64);
            }
        }
    }

    TEST("basic int64: range query correctness") {
        databox values[8];
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED((int64_t)(i + 1));
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 8);

        /* Range [1, 3] = 2+3+4 = 9 */
        databox range;
        if (!multiFenwickRangeQuery(mfw, 1, 3, &range) || range.data.i64 != 9) {
            ERR("Range [1,3] should be 9, got %" PRId64, range.data.i64);
        }

        /* Range [0, 7] = 1+2+3+4+5+6+7+8 = 36 */
        if (!multiFenwickRangeQuery(mfw, 0, 7, &range) ||
            range.data.i64 != 36) {
            ERR("Range [0,7] should be 36, got %" PRId64, range.data.i64);
        }

        /* Range [5, 5] = 6 */
        if (!multiFenwickRangeQuery(mfw, 5, 5, &range) || range.data.i64 != 6) {
            ERR("Range [5,5] should be 6, got %" PRId64, range.data.i64);
        }
    }

    TEST("basic int64: set operation") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Set elements directly */
        databox v1 = DATABOX_SIGNED(10);
        databox v2 = DATABOX_SIGNED(20);
        databox v3 = DATABOX_SIGNED(30);

        multiFenwickSet(&mfw, 0, &v1);
        multiFenwickSet(&mfw, 1, &v2);
        multiFenwickSet(&mfw, 2, &v3);

        databox get;
        if (!multiFenwickGet(mfw, 0, &get) || get.data.i64 != 10) {
            ERR("Get(0) should be 10, got %" PRId64, get.data.i64);
        }
        if (!multiFenwickGet(mfw, 1, &get) || get.data.i64 != 20) {
            ERR("Get(1) should be 20, got %" PRId64, get.data.i64);
        }
        if (!multiFenwickGet(mfw, 2, &get) || get.data.i64 != 30) {
            ERR("Get(2) should be 30, got %" PRId64, get.data.i64);
        }

        /* Change value with Set */
        databox v4 = DATABOX_SIGNED(25);
        multiFenwickSet(&mfw, 1, &v4);
        if (!multiFenwickGet(mfw, 1, &get) || get.data.i64 != 25) {
            ERR("After Set(1, 25), Get(1) should be 25, got %" PRId64,
                get.data.i64);
        }

        /* Prefix sum should reflect change: [10, 35, 65] */
        databox query;
        if (!multiFenwickQuery(mfw, 1, &query) || query.data.i64 != 35) {
            ERR("After changing index 1, Query(1) should be 35, got %" PRId64,
                query.data.i64);
        }
    }

    /* =================================================================
     * CATEGORY 2: FLOATING POINT OPERATIONS (4 tests)
     * ================================================================= */

    TEST("float64: basic operations with doubles") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Build array [1.5, 2.5, 3.5, 4.5] */
        for (size_t i = 0; i < 4; i++) {
            databox val = DATABOX_DOUBLE((double)(i + 1) + 0.5);
            multiFenwickUpdate(&mfw, i, &val);
        }

        /* Verify prefix sums: [1.5, 4.0, 7.5, 12.0] */
        double expected[] = {1.5, 4.0, 7.5, 12.0};
        for (size_t i = 0; i < 4; i++) {
            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum)) {
                ERR("Query failed");
            }

            double val = 0;
            if (!databoxToDouble(&sum, &val) ||
                fabs(val - expected[i]) > 0.001) {
                ERR("Query(%zu) should be %.2f, got %.2f", i, expected[i], val);
            }
        }
    }

    TEST("float64: range query with doubles") {
        databox values[4];
        for (int i = 0; i < 4; i++) {
            values[i] = DATABOX_DOUBLE((double)(i + 1) * 1.1);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 4);

        /* Range [1, 2] = 2.2 + 3.3 = 5.5 */
        databox range;
        if (!multiFenwickRangeQuery(mfw, 1, 2, &range)) {
            ERR("Range query failed");
        }

        double expected = 2.2 + 3.3;
        double val = 0;
        if (!databoxToDouble(&range, &val) || fabs(val - expected) > 0.001) {
            ERR("Range [1,2] should be %.2f, got %.2f", expected, val);
        }
    }

    TEST("float32: operations with floats") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Build array with float values */
        for (size_t i = 0; i < 5; i++) {
            databox val;
            DATABOX_SET_FLOAT(&val, (float)(i + 1) * 2.5f);
            multiFenwickUpdate(&mfw, i, &val);
        }

        /* Verify some values */
        databox get;
        if (!multiFenwickGet(mfw, 2, &get)) {
            ERR("Get failed");
        }

        float expected = 3.0f * 2.5f; /* 7.5 */
        double val = 0;
        if (!databoxToDouble(&get, &val) ||
            fabsf((float)val - expected) > 0.001f) {
            ERR("Get(2) should be %.2f, got %.2f", (double)expected, val);
        }
    }

    TEST("float64: precision with large sums") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Add many small values */
        databox small = DATABOX_DOUBLE(0.1);
        for (size_t i = 0; i < 1000; i++) {
            multiFenwickUpdate(&mfw, i, &small);
        }

        /* Sum should be 100.0 */
        databox sum;
        if (!multiFenwickQuery(mfw, 999, &sum)) {
            ERR("Query failed");
        }

        double val = 0;
        if (!databoxToDouble(&sum, &val) || fabs(val - 100.0) > 0.01) {
            ERR("Sum should be 100.0, got %.2f", val);
        }
    }

    /* =================================================================
     * CATEGORY 3: UNSIGNED INTEGER OPERATIONS (2 tests)
     * ================================================================= */

    TEST("uint64: basic unsigned operations") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Build array of unsigned values */
        for (uint64_t i = 0; i < 5; i++) {
            databox val = DATABOX_UNSIGNED(i * 100);
            multiFenwickUpdate(&mfw, i, &val);
        }

        /* Verify sum: 0 + 100 + 200 + 300 + 400 = 1000 */
        databox sum;
        if (!multiFenwickQuery(mfw, 4, &sum) || sum.data.u64 != 1000) {
            ERR("Sum should be 1000, got %" PRIu64, sum.data.u64);
        }
    }

    TEST("uint64: large unsigned values") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Use large unsigned values */
        databox v1 = DATABOX_UNSIGNED(UINT64_MAX / 4);
        databox v2 = DATABOX_UNSIGNED(UINT64_MAX / 4);

        multiFenwickSet(&mfw, 0, &v1);
        multiFenwickSet(&mfw, 1, &v2);

        databox sum;
        if (!multiFenwickQuery(mfw, 1, &sum)) {
            ERR("Query failed");
        }

        /* Expected: (UINT64_MAX/4) + (UINT64_MAX/4) = 2*(UINT64_MAX/4)
         * Due to integer division truncation, this equals UINT64_MAX/2 - 1 */
        uint64_t expected = (UINT64_MAX / 4) * 2;
        if (sum.data.u64 != expected) {
            ERR("Sum should be %" PRIu64 ", got %" PRIu64, expected,
                sum.data.u64);
        }
    }

    /* =================================================================
     * CATEGORY 4: EDGE CASES (5 tests)
     * ================================================================= */

    TEST("edge case: zero values handling") {
        databox values[5];
        for (int i = 0; i < 5; i++) {
            values[i] = DATABOX_SIGNED(0);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 5);

        /* All prefix sums should be 0 */
        for (size_t i = 0; i < 5; i++) {
            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum) || sum.data.i64 != 0) {
                ERR("Query(%zu) of all zeros should be 0, got %" PRId64, i,
                    sum.data.i64);
            }
        }

        /* Add some non-zero value */
        databox val = DATABOX_SIGNED(10);
        multiFenwickUpdate(&mfw, 2, &val);

        databox get;
        if (!multiFenwickGet(mfw, 2, &get) || get.data.i64 != 10) {
            ERR("Get(2) should be 10 after update");
        }
    }

    TEST("edge case: negative deltas") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Build array [10, 20, 30] */
        databox v1 = DATABOX_SIGNED(10);
        databox v2 = DATABOX_SIGNED(20);
        databox v3 = DATABOX_SIGNED(30);

        multiFenwickSet(&mfw, 0, &v1);
        multiFenwickSet(&mfw, 1, &v2);
        multiFenwickSet(&mfw, 2, &v3);

        /* Subtract from element 1 */
        databox delta = DATABOX_SIGNED(-5);
        multiFenwickUpdate(&mfw, 1, &delta);

        databox get;
        if (!multiFenwickGet(mfw, 1, &get) || get.data.i64 != 15) {
            ERR("After subtracting 5 from 20, should be 15, got %" PRId64,
                get.data.i64);
        }

        /* Prefix sum should be 10 + 15 + 30 = 55 */
        databox sum;
        if (!multiFenwickQuery(mfw, 2, &sum) || sum.data.i64 != 55) {
            ERR("Query(2) should be 55, got %" PRId64, sum.data.i64);
        }
    }

    TEST("edge case: sparse array (large index gaps)") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Add elements at sparse indices */
        databox v1 = DATABOX_SIGNED(1);
        databox v2 = DATABOX_SIGNED(2);
        databox v3 = DATABOX_SIGNED(3);

        multiFenwickSet(&mfw, 0, &v1);
        multiFenwickSet(&mfw, 100, &v2);
        multiFenwickSet(&mfw, 1000, &v3);

        if (multiFenwickCount(mfw) != 1001) {
            ERR("Count should be 1001, got %zu", multiFenwickCount(mfw));
        }

        /* Elements between should be 0 */
        databox get;
        if (!multiFenwickGet(mfw, 50, &get) || get.data.i64 != 0) {
            ERR("Gap element should be 0");
        }

        /* Range queries */
        databox range;
        if (!multiFenwickRangeQuery(mfw, 0, 100, &range) ||
            range.data.i64 != 3) {
            ERR("Range [0,100] should be 1+2=3, got %" PRId64, range.data.i64);
        }
    }

    TEST("edge case: boundary indices") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Test index 0 */
        databox v1 = DATABOX_SIGNED(42);
        multiFenwickSet(&mfw, 0, &v1);

        databox get;
        if (!multiFenwickGet(mfw, 0, &get) || get.data.i64 != 42) {
            ERR("Index 0 should work");
        }

        /* Test high index */
        databox v2 = DATABOX_SIGNED(100);
        multiFenwickSet(&mfw, 9999, &v2);

        if (!multiFenwickGet(mfw, 9999, &get) || get.data.i64 != 100) {
            ERR("High index 9999 should work");
        }

        if (multiFenwickCount(mfw) != 10000) {
            ERR("Count should be 10000, got %zu", multiFenwickCount(mfw));
        }
    }

    TEST("edge case: NULL parameter handling") {
        multiFenwickFree(NULL); /* Should not crash */

        databox result;
        if (multiFenwickQuery(NULL, 0, &result)) {
            ERR("Query on NULL should fail");
        }

        if (multiFenwickCount(NULL) != 0) {
            ERR("Count on NULL should return 0");
        }

        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        databox one = DATABOX_SIGNED(10);
        bool success = multiFenwickUpdate(&mfw, 0, &one);
        if (!success || mfw == NULL) {
            ERR("Update should create tree if NULL");
        }
    }

    /* =================================================================
     * CATEGORY 5: ADVANCED OPERATIONS (3 tests)
     * ================================================================= */

    TEST("advanced: lowerBound search") {
        databox values[5];
        int64_t vals[] = {1, 2, 3, 4, 5}; /* Prefix sums: [1, 3, 6, 10, 15] */
        for (int i = 0; i < 5; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 5);

        /* Find smallest index with sum >= target */
        databox target;

        target = DATABOX_SIGNED(1);
        if (multiFenwickLowerBound(mfw, &target) != 0) {
            ERR("LowerBound(1) should be 0, got %zu",
                multiFenwickLowerBound(mfw, &target));
        }

        target = DATABOX_SIGNED(3);
        if (multiFenwickLowerBound(mfw, &target) != 1) {
            ERR("LowerBound(3) should be 1, got %zu",
                multiFenwickLowerBound(mfw, &target));
        }

        target = DATABOX_SIGNED(10);
        if (multiFenwickLowerBound(mfw, &target) != 3) {
            ERR("LowerBound(10) should be 3, got %zu",
                multiFenwickLowerBound(mfw, &target));
        }

        target = DATABOX_SIGNED(100);
        if (multiFenwickLowerBound(mfw, &target) != SIZE_MAX) {
            ERR("LowerBound(100) should be SIZE_MAX (not found)");
        }
    }

    TEST("advanced: clear operation") {
        databox values[5];
        for (int i = 0; i < 5; i++) {
            values[i] = DATABOX_SIGNED((int64_t)(i + 1));
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 5);

        /* Clear all values */
        multiFenwickClear(mfw);

        /* All queries should return 0 */
        for (size_t i = 0; i < 5; i++) {
            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum) || sum.data.i64 != 0) {
                ERR("After clear, Query(%zu) should be 0, got %" PRId64, i,
                    sum.data.i64);
            }
        }

        /* Can still update after clear */
        databox val = DATABOX_SIGNED(10);
        multiFenwickUpdate(&mfw, 2, &val);

        databox get;
        if (!multiFenwickGet(mfw, 2, &get) || get.data.i64 != 10) {
            ERR("Should be able to update after clear");
        }
    }

    TEST("advanced: newFromArray construction") {
        databox values[8];
        int64_t vals[] = {5, 2, 8, 1, 9, 3, 7, 4};
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, 8);

        /* Verify all elements */
        for (size_t i = 0; i < 8; i++) {
            databox get;
            if (!multiFenwickGet(mfw, i, &get) || get.data.i64 != vals[i]) {
                ERR("Element %zu should be %" PRId64 ", got %" PRId64, i,
                    vals[i], get.data.i64);
            }
        }

        /* Verify prefix sums */
        int64_t sum = 0;
        for (size_t i = 0; i < 8; i++) {
            sum += vals[i];
            databox query;
            if (!multiFenwickQuery(mfw, i, &query) || query.data.i64 != sum) {
                ERR("Prefix sum at %zu should be %" PRId64 ", got %" PRId64, i,
                    sum, query.data.i64);
            }
        }
    }

    TEST("wide: 96/128-bit values are refused and never stored") {
        /* The sums have no wide arithmetic, so every entry point refuses
         * a 96/128-bit value, with or without a payload, and leaves the
         * tree as it was. */
        databoxBig wideS;
        databoxBig wideU;
        memset(&wideS, 0, sizeof(wideS));
        memset(&wideU, 0, sizeof(wideU));
        DATABOX_BIG_SIGNED_128(&wideS, -((__int128_t)1 << 100));
        DATABOX_BIG_UNSIGNED_128(&wideU, ((__uint128_t)1 << 64) + 5);
        databox deferred = {};
        deferred.type = DATABOX_SIGNED_128; /* no payload: data.ptr NULL */
        const databox *wides[3] = {(const databox *)&wideS,
                                   (const databox *)&wideU, &deferred};

        /* A refused first update creates no tree. */
        multiFenwick *none = NULL;
        if (multiFenwickUpdate(&none, 3, wides[0]) || none) {
            ERR("Update accepted a wide delta into an empty tree");
        }
        multiFenwickFree(none);

        constexpr int WIDE_N = 8;
        const int64_t vals[WIDE_N] = {5, -2, 8, 1, 9, -3, 7, 4};
        databox values[WIDE_N];
        for (size_t i = 0; i < WIDE_N; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, WIDE_N);
        if (!mfw) {
            ERR("NewFromArray of 64-bit values failed");
        } else {
            for (size_t w = 0; w < 3; w++) {
                const databox *wide = wides[w];
                for (size_t i = 0; i < WIDE_N; i++) {
                    if (multiFenwickUpdate(&mfw, i, wide)) {
                        ERR("wide %zu: Update(%zu) accepted it", w, i);
                    }

                    if (multiFenwickSet(&mfw, i, wide)) {
                        ERR("wide %zu: Set(%zu) accepted it", w, i);
                    }
                }

                if (multiFenwickUpdate(&mfw, 4 * WIDE_N, wide) ||
                    multiFenwickCount(mfw) != WIDE_N) {
                    ERR("wide %zu: an update past the end was not refused "
                        "cleanly",
                        w);
                }

                if (multiFenwickLowerBound(mfw, wide) != SIZE_MAX) {
                    ERR("wide %zu: LowerBound did not refuse it", w);
                }

                /* A wide value anywhere in the array refuses the array. */
                databox mixed[WIDE_N];
                memcpy(mixed, values, sizeof(mixed));
                mixed[w == 1 ? WIDE_N - 1 : w] = *wide;
                cleaner(multiFenwickFree) multiFenwick *bad =
                    multiFenwickNewFromArray(mixed, WIDE_N);
                if (bad) {
                    ERR("wide %zu: NewFromArray accepted it", w);
                }
            }

            /* The tree is unchanged, and still only 64-bit. */
            int64_t sum = 0;
            for (size_t i = 0; i < WIDE_N; i++) {
                sum += vals[i];
                const databox wantVal = DATABOX_SIGNED(vals[i]);
                const databox wantSum = DATABOX_SIGNED(sum);
                databox got;
                databox query;
                if (!multiFenwickGet(mfw, i, &got) ||
                    databoxCompareNumeric(&got, &wantVal) != 0 ||
                    got.type == DATABOX_SIGNED_128 ||
                    got.type == DATABOX_UNSIGNED_128) {
                    ERR("element %zu changed (want %" PRId64 ")", i, vals[i]);
                }

                if (!multiFenwickQuery(mfw, i, &query) ||
                    databoxCompareNumeric(&query, &wantSum) != 0 ||
                    query.type == DATABOX_SIGNED_128 ||
                    query.type == DATABOX_UNSIGNED_128) {
                    ERR("prefix sum %zu changed (want %" PRId64 ")", i, sum);
                }
            }
        }
    }

    TEST("non-numeric values are refused and never stored") {
        /* Only SIGNED_64, UNSIGNED_64, FLOAT_32 and DOUBLE_64 have
         * arithmetic: every other type is refused by every entry point,
         * and the tree is left as it was. */
        enum { NN_N = 6, NN_TYPES = 6 };
        databox odd[NN_TYPES];
        memset(odd, 0, sizeof(odd));
        odd[0].type = DATABOX_VOID;
        odd[1] = databoxNewBytesString("not a number");
        odd[2].type = DATABOX_TRUE;
        odd[3].type = DATABOX_FALSE;
        odd[4].type = DATABOX_NULL;
        odd[5].type = DATABOX_PTR;
        odd[5].data.ptr = odd;

        cleaner(multiFenwickFree) multiFenwick *none = NULL;
        if (multiFenwickUpdate(&none, 0, &odd[1]) || none) {
            ERR("Update accepted BYTES into an empty tree");
        }

        const int64_t vals[NN_N] = {7, -4, 0, 12, -9, 3};
        databox values[NN_N];
        for (size_t i = 0; i < NN_N; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, NN_N);
        for (size_t t = 0; mfw && t < NN_TYPES; t++) {
            for (size_t i = 0; i < NN_N; i++) {
                if (multiFenwickUpdate(&mfw, i, &odd[t]) ||
                    multiFenwickSet(&mfw, i, &odd[t])) {
                    ERR("type %d: Update or Set(%zu) accepted it",
                        (int)odd[t].type, i);
                }
            }

            if (multiFenwickUpdate(&mfw, 3 * NN_N, &odd[t]) ||
                multiFenwickCount(mfw) != NN_N) {
                ERR("type %d: an update past the end was not refused cleanly",
                    (int)odd[t].type);
            }

            if (multiFenwickLowerBound(mfw, &odd[t]) != SIZE_MAX) {
                ERR("type %d: LowerBound did not refuse it", (int)odd[t].type);
            }

            /* Not a value (VOID included): the array is refused. */
            for (size_t at = 0; at < NN_N; at += NN_N - 1) {
                databox mixed[NN_N];
                memcpy(mixed, values, sizeof(mixed));
                mixed[at] = odd[t];
                cleaner(multiFenwickFree) multiFenwick *bad =
                    multiFenwickNewFromArray(mixed, NN_N);
                if (bad) {
                    ERR("type %d at %zu: NewFromArray accepted it",
                        (int)odd[t].type, at);
                }
            }
        }

        int64_t prefix = 0;
        for (size_t i = 0; mfw && i < NN_N; i++) {
            prefix += vals[i];
            databox el;
            databox sum;
            if (!multiFenwickGet(mfw, i, &el) || !mfwIntIs(&el, vals[i]) ||
                !multiFenwickQuery(mfw, i, &sum) || !mfwIntIs(&sum, prefix)) {
                ERR("index %zu changed after refused writes", i);
            }
        }

        /* VOID is not zero anywhere, NewFromArray included: an array
         * with a VOID in the middle, or of nothing but VOID, is refused
         * like any other. */
        databox sparse[4];
        sparse[0] = DATABOX_SIGNED(4);
        sparse[1] = odd[0];
        sparse[2] = odd[0];
        sparse[3] = DATABOX_SIGNED(-3);
        cleaner(multiFenwickFree) multiFenwick *withVoid =
            multiFenwickNewFromArray(sparse, 4);
        if (withVoid) {
            ERR("NewFromArray accepted VOID elements as zero");
        }

        databox allVoid[3] = {odd[0], odd[0], odd[0]};
        cleaner(multiFenwickFree) multiFenwick *zeros =
            multiFenwickNewFromArray(allVoid, 3);
        if (zeros) {
            ERR("NewFromArray accepted an all-VOID array");
        }
    }

    TEST("transactional Update: a refused update changes nothing") {
        /* Every refusal (non-numeric delta, index above the maximum) is
         * decided before the first write: the elements, the count and the
         * byte size stay as they were, and a NULL tree stays NULL. */
        const databox one = DATABOX_SIGNED(1);
        const databox bytes = databoxNewBytesString("nope");
        const size_t badIdx[] = {(size_t)MULTIFENWICK_INDEX_MAX + 1,
                                 (size_t)1 << 40, SIZE_MAX};

        cleaner(multiFenwickFree) multiFenwick *none = NULL;
        for (size_t k = 0; k < countof(badIdx); k++) {
            if (multiFenwickUpdate(&none, badIdx[k], &one) ||
                multiFenwickSet(&none, badIdx[k], &one) || none) {
                ERR("index %zu: a refused write created a tree", badIdx[k]);
            }
        }

        constexpr int TX_N = 37;
        int64_t vals[TX_N];
        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        for (size_t i = 0; i < TX_N; i++) {
            vals[i] = (int64_t)(i * 13 % 29) - 14;
            const databox v = DATABOX_SIGNED(vals[i]);
            if (!multiFenwickUpdate(&mfw, i, &v)) {
                ERR("Update(%zu) failed", i);
            }
        }

        const size_t bytesBefore = multiFenwickBytes(mfw);
        for (size_t k = 0; k < countof(badIdx); k++) {
            if (multiFenwickUpdate(&mfw, badIdx[k], &one) ||
                multiFenwickSet(&mfw, badIdx[k], &one)) {
                ERR("index %zu was accepted", badIdx[k]);
            }
        }

        for (size_t i = 0; i < TX_N + 8; i++) {
            if (multiFenwickUpdate(&mfw, i, &bytes)) {
                ERR("a BYTES delta at %zu was accepted", i);
            }
        }

        if (multiFenwickCount(mfw) != TX_N ||
            multiFenwickBytes(mfw) != bytesBefore) {
            ERR("refused updates changed the count (%zu) or size (%zu -> "
                "%zu)",
                multiFenwickCount(mfw), bytesBefore, multiFenwickBytes(mfw));
        }

        int64_t prefix = 0;
        for (size_t i = 0; i < TX_N; i++) {
            prefix += vals[i];
            databox el;
            databox sum;
            if (!multiFenwickGet(mfw, i, &el) || !mfwIntIs(&el, vals[i]) ||
                !multiFenwickQuery(mfw, i, &sum) || !mfwIntIs(&sum, prefix)) {
                ERR("index %zu changed after refused updates", i);
                break;
            }
        }
    }

    TEST("differential: signed Update, Set, Get and Query against an array") {
        /* Random signed values, small and beyond 2^53, through Update
         * (add) and Set (assign), growing the tree as indexes arrive.
         * Each step checks the touched element, the count and one
         * prefix sum; every 150 steps every element and prefix sum is
         * checked against the oracle. */
        enum { DIFF_N = 300, DIFF_OPS = 1500 };
        static int64_t oracle[DIFF_N];
        memset(oracle, 0, sizeof(oracle));
        size_t count = 0;
        uint64_t seed = 4242;
        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        bool failed = false;
        for (uint32_t op = 0; op < DIFF_OPS && !failed; op++) {
            const size_t idx = randSeed(&seed) % DIFF_N;
            const uint64_t r = randSeed(&seed);
            const int64_t v = op % 3 == 0 ? (int64_t)(r % ((uint64_t)1 << 51)) -
                                                ((int64_t)1 << 50)
                                          : (int64_t)(r % 2001) - 1000;
            const databox box = DATABOX_SIGNED(v);
            const bool set = op % 4 == 3;
            if (!(set ? multiFenwickSet(&mfw, idx, &box)
                      : multiFenwickUpdate(&mfw, idx, &box))) {
                ERR("op %u: %s(%zu, %" PRId64 ") failed", op,
                    set ? "Set" : "Update", idx, v);
                failed = true;
                break;
            }

            oracle[idx] = set ? v : oracle[idx] + v;
            if (idx + 1 > count) {
                count = idx + 1;
            }

            databox got;
            if (multiFenwickCount(mfw) != count ||
                !multiFenwickGet(mfw, idx, &got) ||
                !mfwIntIs(&got, oracle[idx])) {
                ERR("op %u: element %zu or the count is wrong (want %" PRId64
                    ")",
                    op, idx, oracle[idx]);
                failed = true;
            }

            const size_t q = randSeed(&seed) % count;
            int64_t prefix = 0;
            for (size_t i = 0; i <= q; i++) {
                prefix += oracle[i];
            }

            if (!multiFenwickQuery(mfw, q, &got) || !mfwIntIs(&got, prefix)) {
                ERR("op %u: prefix sum %zu is wrong (want %" PRId64 ")", op, q,
                    prefix);
                failed = true;
            }

            if (op % 150 == 149) {
                prefix = 0;
                for (size_t i = 0; i < count && !failed; i++) {
                    prefix += oracle[i];
                    databox el;
                    databox sum;
                    if (!multiFenwickGet(mfw, i, &el) ||
                        !mfwIntIs(&el, oracle[i]) ||
                        !multiFenwickQuery(mfw, i, &sum) ||
                        !mfwIntIs(&sum, prefix)) {
                        ERR("op %u: sweep differs at %zu", op, i);
                        failed = true;
                    }
                }
            }
        }
    }

    TEST("growth: values and prefix sums survive every capacity doubling") {
        /* Appending one element at a time crosses every power of two up
         * to 256; after each append every element and prefix sum must
         * still match.  Then sparse indexes force multi-step doublings. */
        constexpr int GROW_N = 130;
        static int64_t oracle[GROW_N];
        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        bool failed = false;
        for (size_t n = 0; n < GROW_N && !failed; n++) {
            oracle[n] = (int64_t)(n * 37 + 1) * (n % 2 ? -1 : 1);
            const databox box = DATABOX_SIGNED(oracle[n]);
            if (!multiFenwickUpdate(&mfw, n, &box) ||
                multiFenwickCount(mfw) != n + 1) {
                ERR("append %zu failed", n);
                failed = true;
                break;
            }

            int64_t prefix = 0;
            for (size_t i = 0; i <= n && !failed; i++) {
                prefix += oracle[i];
                databox el;
                databox sum;
                if (!multiFenwickGet(mfw, i, &el) ||
                    !mfwIntIs(&el, oracle[i]) ||
                    !multiFenwickQuery(mfw, i, &sum) ||
                    !mfwIntIs(&sum, prefix)) {
                    ERR("after append %zu: index %zu differs", n, i);
                    failed = true;
                }
            }
        }

        const size_t sparse[] = {0, 3, 17, 300, 1025, 5000};
        const int64_t sparseVal[] = {-9, 40, -700, 12345, -1, 77};
        constexpr int SPARSE_N = countof(sparse);
        cleaner(multiFenwickFree) multiFenwick *sp = NULL;
        for (size_t k = 0; k < SPARSE_N && !failed; k++) {
            const databox box = DATABOX_SIGNED(sparseVal[k]);
            if (!multiFenwickUpdate(&sp, sparse[k], &box) ||
                multiFenwickCount(sp) != sparse[k] + 1) {
                ERR("sparse update at %zu failed", sparse[k]);
                failed = true;
                break;
            }

            int64_t prefix = 0;
            for (size_t j = 0; j <= k && !failed; j++) {
                prefix += sparseVal[j];
                databox el;
                databox sum;
                databox gap;
                if (!multiFenwickGet(sp, sparse[j], &el) ||
                    !mfwIntIs(&el, sparseVal[j]) ||
                    !multiFenwickQuery(sp, sparse[j], &sum) ||
                    !mfwIntIs(&sum, prefix) ||
                    /* no two sparse indexes are adjacent: the one
                     * below each is an untouched zero */
                    (sparse[j] > 0 &&
                     (!multiFenwickGet(sp, sparse[j] - 1, &gap) ||
                      !mfwIntIs(&gap, 0)))) {
                    ERR("after sparse step %zu: index %zu differs", k,
                        sparse[j]);
                    failed = true;
                }
            }
        }
    }

    TEST("differential: LowerBound against an array") {
        /* LowerBound needs non-negative values (monotonic prefix sums).
         * Targets are random, every exact prefix sum, and past the end. */
        constexpr int LB_N = 200;
        static int64_t prefix[LB_N];
        uint64_t seed = 777;
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        int64_t total = 0;
        for (size_t i = 0; i < LB_N; i++) {
            const int64_t v = (int64_t)(randSeed(&seed) % 1001);
            const databox box = DATABOX_SIGNED(v);
            if (!multiFenwickUpdate(&mfw, i, &box)) {
                ERR("Update(%zu) failed", i);
            }

            total += v;
            prefix[i] = total;
        }

        for (uint32_t k = 0; k < 3 * LB_N; k++) {
            const int64_t target =
                k < LB_N ? prefix[k]
                         : (int64_t)(randSeed(&seed) % (uint64_t)(total + 50));
            size_t want = SIZE_MAX;
            for (size_t i = 0; i < LB_N; i++) {
                if (prefix[i] >= target) {
                    want = i;
                    break;
                }
            }

            const databox t = DATABOX_SIGNED(target);
            const size_t got = multiFenwickLowerBound(mfw, &t);
            if (got != want) {
                ERR("LowerBound(%" PRId64 ") = %zu, want %zu", target, got,
                    want);
                break;
            }
        }
    }

    TEST("contract: NULL, empty and out-of-range arguments") {
        /* A NULL tree reads as empty, NULL inputs are refused, reads at
         * or past the count fail, and every failed read leaves VOID. */
        const databox one = DATABOX_SIGNED(1);
        const databox zero = DATABOX_SIGNED(0);
        databox out;

        out = one;
        if (multiFenwickQuery(NULL, 0, &out) || !DATABOX_IS_VOID(&out)) {
            ERR("Query(NULL) did not fail with VOID");
        }

        out = one;
        if (multiFenwickGet(NULL, 0, &out) || !DATABOX_IS_VOID(&out)) {
            ERR("Get(NULL) did not fail with VOID");
        }

        out = one;
        if (multiFenwickRangeQuery(NULL, 0, 0, &out) ||
            !DATABOX_IS_VOID(&out)) {
            ERR("RangeQuery(NULL) did not fail with VOID");
        }

        if (multiFenwickLowerBound(NULL, &one) != SIZE_MAX ||
            multiFenwickCount(NULL) != 0 || multiFenwickBytes(NULL) != 0) {
            ERR("a NULL tree does not read as empty");
        }

        multiFenwickClear(NULL);
        multiFenwickFree(NULL);

        if (multiFenwickUpdate(NULL, 0, &one) ||
            multiFenwickSet(NULL, 0, &one)) {
            ERR("a NULL pointer-to-pointer was accepted");
        }

        cleaner(multiFenwickFree) multiFenwick *none = NULL;
        if (multiFenwickUpdate(&none, 0, NULL) ||
            multiFenwickSet(&none, 0, NULL) || none) {
            ERR("a NULL delta or value was accepted");
        }

        cleaner(multiFenwickFree) multiFenwick *empty = multiFenwickNew();
        out = one;
        if (!empty || multiFenwickQuery(empty, 0, &out) ||
            !DATABOX_IS_VOID(&out) || multiFenwickGet(empty, 0, &out) ||
            multiFenwickRangeQuery(empty, 0, 0, &out) ||
            multiFenwickLowerBound(empty, &zero) != SIZE_MAX) {
            ERR("an empty tree answered a read");
        }

        multiFenwickClear(empty);
        if (multiFenwickCount(empty) != 0) {
            ERR("Clear changed an empty tree's count");
        }

        /* [4, 0, 7]: a zero delta still extends the count */
        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        const databox four = DATABOX_SIGNED(4);
        const databox seven = DATABOX_SIGNED(7);
        if (!multiFenwickUpdate(&mfw, 0, &four) ||
            !multiFenwickUpdate(&mfw, 1, &zero) ||
            !multiFenwickUpdate(&mfw, 2, &seven) ||
            multiFenwickCount(mfw) != 3) {
            ERR("building [4, 0, 7] failed");
        }

        const size_t past[] = {3, 4, 1000, SIZE_MAX};
        for (size_t k = 0; mfw && k < countof(past); k++) {
            out = one;
            if (multiFenwickQuery(mfw, past[k], &out) ||
                !DATABOX_IS_VOID(&out)) {
                ERR("Query(%zu) past the count did not fail with VOID",
                    past[k]);
            }

            out = one;
            if (multiFenwickGet(mfw, past[k], &out) || !DATABOX_IS_VOID(&out)) {
                ERR("Get(%zu) past the count did not fail with VOID", past[k]);
            }
        }

        out = one;
        if (multiFenwickRangeQuery(mfw, 2, 1, &out) || !DATABOX_IS_VOID(&out) ||
            multiFenwickRangeQuery(mfw, 0, 3, &out) || !DATABOX_IS_VOID(&out)) {
            ERR("RangeQuery with left > right or right >= count did not "
                "fail with VOID");
        }

        if (!multiFenwickRangeQuery(mfw, 1, 1, &out) || !mfwIntIs(&out, 0) ||
            !multiFenwickRangeQuery(mfw, 0, 2, &out) || !mfwIntIs(&out, 11) ||
            !multiFenwickRangeQuery(mfw, 2, 2, &out) || !mfwIntIs(&out, 7)) {
            ERR("RangeQuery over [4, 0, 7] is wrong");
        }

        if (multiFenwickQuery(mfw, 0, NULL) || multiFenwickGet(mfw, 0, NULL) ||
            multiFenwickRangeQuery(mfw, 0, 0, NULL) ||
            multiFenwickLowerBound(mfw, NULL) != SIZE_MAX) {
            ERR("a NULL result or target was accepted");
        }

        /* Clear keeps the count and zeroes every element */
        multiFenwickClear(mfw);
        if (multiFenwickCount(mfw) != 3 || !multiFenwickQuery(mfw, 2, &out) ||
            !mfwIntIs(&out, 0)) {
            ERR("Clear did not keep the count and zero the elements");
        }

        /* Past the end extends the count; the gap reads as zero */
        if (!multiFenwickUpdate(&mfw, 9, &one) ||
            multiFenwickCount(mfw) != 10 || !multiFenwickGet(mfw, 5, &out) ||
            !mfwIntIs(&out, 0) || !multiFenwickGet(mfw, 9, &out) ||
            !mfwIntIs(&out, 1)) {
            ERR("an update past the end did not extend the count");
        }
    }

    TEST("integer limits: exact at INT64/UINT64 bounds, refused past them") {
        const databox umax = DATABOX_UNSIGNED(UINT64_MAX);
        const databox smin = DATABOX_SIGNED(INT64_MIN);
        const databox smax = DATABOX_SIGNED(INT64_MAX);
        const databox one = DATABOX_SIGNED(1);
        const databox minusOne = DATABOX_SIGNED(-1);
        const databox zero = DATABOX_SIGNED(0);
        databox out;

        /* One element at each end of the range, and one step past it */
        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        if (!multiFenwickUpdate(&mfw, 0, &umax) ||
            !multiFenwickGet(mfw, 0, &out) || out.type != DATABOX_UNSIGNED_64 ||
            out.data.u64 != UINT64_MAX) {
            ERR("UINT64_MAX did not round-trip");
        }

        const size_t bytes = multiFenwickBytes(mfw);
        if (multiFenwickUpdate(&mfw, 0, &one) ||
            !multiFenwickGet(mfw, 0, &out) || out.data.u64 != UINT64_MAX ||
            multiFenwickBytes(mfw) != bytes) {
            ERR("UINT64_MAX + 1 was not refused cleanly");
        }

        /* Set across the whole range in one step: the difference needs
         * 65 bits, and is still exact */
        if (!multiFenwickSet(&mfw, 0, &smin) ||
            !multiFenwickGet(mfw, 0, &out) || out.type != DATABOX_SIGNED_64 ||
            out.data.i64 != INT64_MIN) {
            ERR("Set from UINT64_MAX to INT64_MIN failed");
        }

        if (multiFenwickUpdate(&mfw, 0, &minusOne) ||
            !multiFenwickGet(mfw, 0, &out) || out.data.i64 != INT64_MIN) {
            ERR("INT64_MIN - 1 was not refused cleanly");
        }

        if (!multiFenwickSet(&mfw, 0, &umax) ||
            !multiFenwickGet(mfw, 0, &out) || out.data.u64 != UINT64_MAX) {
            ERR("Set from INT64_MIN to UINT64_MAX failed");
        }

        /* Results are typed by value: INT64_MAX + 1 is UNSIGNED_64, a
         * negative is SIGNED_64, zero is UNSIGNED_64 */
        cleaner(multiFenwickFree) multiFenwick *t = NULL;
        if (!multiFenwickSet(&t, 0, &smax) ||
            !multiFenwickUpdate(&t, 0, &one) || !multiFenwickGet(t, 0, &out) ||
            out.type != DATABOX_UNSIGNED_64 ||
            out.data.u64 != (uint64_t)INT64_MAX + 1) {
            ERR("INT64_MAX + 1 is not UNSIGNED_64 2^63");
        }

        if (!multiFenwickSet(&t, 0, &minusOne) ||
            !multiFenwickGet(t, 0, &out) || out.type != DATABOX_SIGNED_64 ||
            out.data.i64 != -1 || !multiFenwickSet(&t, 0, &zero) ||
            !multiFenwickGet(t, 0, &out) || out.type != DATABOX_UNSIGNED_64 ||
            out.data.u64 != 0) {
            ERR("-1 and 0 are not typed by value");
        }

        /* Elements [A, 0, A, INT64_MIN] with A = 2^63 + 2^62 - 1: every
         * stored cell fits (the total is 2^64 - 2), but the prefix sum
         * through index 2 is 2A, above UINT64_MAX.  That one prefix sum
         * fails; the elements and ranges around it are exact. */
        const uint64_t a = ((uint64_t)1 << 63) + ((uint64_t)1 << 62) - 1;
        const databox boxA = DATABOX_UNSIGNED(a);
        cleaner(multiFenwickFree) multiFenwick *wide = NULL;
        if (!multiFenwickUpdate(&wide, 0, &boxA) ||
            !multiFenwickUpdate(&wide, 3, &smin) ||
            !multiFenwickUpdate(&wide, 2, &boxA)) {
            ERR("building [A, 0, A, INT64_MIN] failed");
        }

        databox values[4] = {boxA, zero, boxA, smin};
        cleaner(multiFenwickFree) multiFenwick *built =
            multiFenwickNewFromArray(values, 4);
        multiFenwick *const trees[2] = {wide, built};
        for (size_t w = 0; w < 2; w++) {
            multiFenwick *const tr = trees[w];
            if (!tr) {
                ERR("tree %zu was not built", w);
                continue;
            }

            if (!multiFenwickGet(tr, 0, &out) || out.data.u64 != a ||
                !multiFenwickGet(tr, 2, &out) || out.data.u64 != a ||
                !multiFenwickGet(tr, 3, &out) || out.data.i64 != INT64_MIN ||
                out.type != DATABOX_SIGNED_64) {
                ERR("tree %zu: an element is wrong", w);
            }

            out = one;
            if (multiFenwickQuery(tr, 2, &out) || !DATABOX_IS_VOID(&out)) {
                ERR("tree %zu: an out-of-range prefix sum was returned", w);
            }

            if (!multiFenwickQuery(tr, 3, &out) ||
                out.data.u64 != UINT64_MAX - 1 ||
                !multiFenwickRangeQuery(tr, 2, 3, &out) ||
                out.data.u64 != ((uint64_t)1 << 62) - 1 ||
                !multiFenwickRangeQuery(tr, 1, 2, &out) || out.data.u64 != a) {
                ERR("tree %zu: an in-range sum around it is wrong", w);
            }

            out = one;
            if (multiFenwickRangeQuery(tr, 0, 2, &out) ||
                !DATABOX_IS_VOID(&out)) {
                ERR("tree %zu: an out-of-range range was returned", w);
            }
        }

        /* Another A at index 2 would push cell 3 and the total out of
         * range: refused, nothing changes */
        if (wide && (multiFenwickUpdate(&wide, 2, &boxA) ||
                     !multiFenwickGet(wide, 2, &out) || out.data.u64 != a ||
                     !multiFenwickQuery(wide, 3, &out) ||
                     out.data.u64 != UINT64_MAX - 1)) {
            ERR("an update overflowing a cell was not refused cleanly");
        }

        /* An array whose total has no 64-bit value is refused */
        databox over[4] = {boxA, zero, boxA, boxA};
        cleaner(multiFenwickFree) multiFenwick *overTree =
            multiFenwickNewFromArray(over, 4);
        if (overTree) {
            ERR("NewFromArray accepted an out-of-range total");
        }
    }

    TEST("floating: double precision, non-finite refused, mixed cells") {
        databox out;

        /* [2^24, 0, 1]: 2^24 and 1 are each exact floats, so storage keeps
         * cells 2 and 3 as FLOAT_32; the prefix sum through index 2 adds
         * those two cells and must still be the double 2^24 + 1. */
        const databox big = DATABOX_DOUBLE(16777216.0);
        const databox oneD = DATABOX_DOUBLE(1.0);
        cleaner(multiFenwickFree) multiFenwick *d = NULL;
        if (!multiFenwickUpdate(&d, 0, &big) ||
            !multiFenwickUpdate(&d, 2, &oneD) ||
            !multiFenwickQuery(d, 2, &out) || out.type != DATABOX_DOUBLE_64 ||
            out.data.d64 != 16777217.0) {
            ERR("2^24 + 1 of doubles was rounded like a float");
        }

        /* FLOAT_32 inputs sum in double too: 1.5 + 2^-30 is exact in
         * double, not in float */
        databox f1;
        databox f2;
        DATABOX_SET_FLOAT(&f1, 1.5f);
        DATABOX_SET_FLOAT(&f2, 0x1p-30f);
        cleaner(multiFenwickFree) multiFenwick *f = NULL;
        if (!multiFenwickUpdate(&f, 0, &f1) ||
            !multiFenwickUpdate(&f, 1, &f2) || !multiFenwickQuery(f, 1, &out) ||
            out.type != DATABOX_DOUBLE_64 || out.data.d64 != 1.5 + 0x1p-30) {
            ERR("FLOAT_32 inputs were not summed in double");
        }

        /* NaN and infinities are refused by every entry point */
        databox odd[5];
        odd[0] = DATABOX_DOUBLE(NAN);
        odd[1] = DATABOX_DOUBLE(INFINITY);
        odd[2] = DATABOX_DOUBLE(-INFINITY);
        DATABOX_SET_FLOAT(&odd[3], NAN);
        DATABOX_SET_FLOAT(&odd[4], -INFINITY);
        for (size_t k = 0; k < 5; k++) {
            if (d && (multiFenwickUpdate(&d, 0, &odd[k]) ||
                      multiFenwickSet(&d, 1, &odd[k]) ||
                      multiFenwickUpdate(&d, 50, &odd[k]) ||
                      multiFenwickCount(d) != 3 ||
                      multiFenwickLowerBound(d, &odd[k]) != SIZE_MAX)) {
                ERR("non-finite %zu was accepted", k);
            }

            databox arr[2] = {oneD, odd[k]};
            cleaner(multiFenwickFree) multiFenwick *bad =
                multiFenwickNewFromArray(arr, 2);
            if (bad) {
                ERR("NewFromArray accepted non-finite %zu", k);
            }
        }

        if (!multiFenwickQuery(d, 2, &out) || out.data.d64 != 16777217.0) {
            ERR("refused non-finite values changed the tree");
        }

        /* A cell that would overflow to infinity is refused */
        const databox dmax = DATABOX_DOUBLE(DBL_MAX);
        const databox dmin = DATABOX_DOUBLE(-DBL_MAX);
        cleaner(multiFenwickFree) multiFenwick *g = NULL;
        if (!multiFenwickUpdate(&g, 0, &dmax) ||
            multiFenwickUpdate(&g, 0, &dmax) || !multiFenwickGet(g, 0, &out) ||
            out.data.d64 != DBL_MAX) {
            ERR("DBL_MAX + DBL_MAX was not refused cleanly");
        }

        /* [M, 0, M, -M], M = DBL_MAX: every cell is finite, the prefix
         * sum through index 2 is not and fails; the rest is exact */
        if (!multiFenwickUpdate(&g, 3, &dmin) ||
            !multiFenwickUpdate(&g, 2, &dmax)) {
            ERR("building [M, 0, M, -M] failed");
        }

        out = oneD;
        if (multiFenwickQuery(g, 2, &out) || !DATABOX_IS_VOID(&out) ||
            !multiFenwickGet(g, 2, &out) || out.data.d64 != DBL_MAX ||
            !multiFenwickRangeQuery(g, 2, 3, &out) || out.data.d64 != 0.0 ||
            !multiFenwickQuery(g, 3, &out) || out.data.d64 != DBL_MAX) {
            ERR("[M, 0, M, -M] reads are wrong");
        }

        /* Mixed cells: [3, 0.25, 5, 0.5].  Integer elements and integer
         * prefix sums stay integers; anything with a float part is a
         * double. */
        const databox three = DATABOX_SIGNED(3);
        const databox quarter = DATABOX_DOUBLE(0.25);
        const databox five = DATABOX_UNSIGNED(5);
        databox half;
        DATABOX_SET_FLOAT(&half, 0.5f);
        cleaner(multiFenwickFree) multiFenwick *m = NULL;
        if (!multiFenwickUpdate(&m, 0, &three) ||
            !multiFenwickUpdate(&m, 1, &quarter) ||
            !multiFenwickUpdate(&m, 2, &five) ||
            !multiFenwickUpdate(&m, 3, &half)) {
            ERR("building the mixed tree failed");
        }

        if (!multiFenwickGet(m, 0, &out) || !mfwIntIs(&out, 3) ||
            !multiFenwickGet(m, 1, &out) || out.type != DATABOX_DOUBLE_64 ||
            out.data.d64 != 0.25 || !multiFenwickGet(m, 2, &out) ||
            !mfwIntIs(&out, 5) || !multiFenwickGet(m, 3, &out) ||
            out.type != DATABOX_DOUBLE_64 || out.data.d64 != 0.5 ||
            !multiFenwickQuery(m, 0, &out) || !mfwIntIs(&out, 3) ||
            !multiFenwickQuery(m, 3, &out) || out.type != DATABOX_DOUBLE_64 ||
            out.data.d64 != 8.75) {
            ERR("the mixed tree reads are wrong");
        }

        /* An integer prefix next to a float cell stays exact above 2^53 */
        const databox huge = DATABOX_UNSIGNED(((uint64_t)1 << 60) + 1);
        cleaner(multiFenwickFree) multiFenwick *h = NULL;
        if (!multiFenwickUpdate(&h, 0, &huge) ||
            !multiFenwickUpdate(&h, 2, &quarter) ||
            !multiFenwickQuery(h, 1, &out) || out.type != DATABOX_UNSIGNED_64 ||
            out.data.u64 != ((uint64_t)1 << 60) + 1) {
            ERR("an integer prefix sum lost exactness next to a float");
        }
    }

    TEST("NewFromArray: strict, bounded, and the same tree as Updates") {
        const databox one = DATABOX_SIGNED(1);
        databox out;

        databox base[4] = {DATABOX_SIGNED(4), DATABOX_SIGNED(9),
                           DATABOX_SIGNED(-3), one};
        for (size_t at = 0; at < 4; at++) {
            databox mixed[4];
            memcpy(mixed, base, sizeof(mixed));
            mixed[at] = DATABOX_BOX_VOID;
            cleaner(multiFenwickFree) multiFenwick *bad =
                multiFenwickNewFromArray(mixed, 4);
            if (bad) {
                ERR("a VOID at %zu was accepted", at);
            }
        }

        /* count 0 is an empty tree, with or without 'values' */
        cleaner(multiFenwickFree) multiFenwick *e1 =
            multiFenwickNewFromArray(NULL, 0);
        cleaner(multiFenwickFree) multiFenwick *e2 =
            multiFenwickNewFromArray(base, 0);
        if (!e1 || !e2 || multiFenwickCount(e1) != 0 ||
            multiFenwickCount(e2) != 0 || multiFenwickQuery(e1, 0, &out)) {
            ERR("count 0 did not give an empty tree");
        }

        /* NULL values with a count, and a count past the index limit
         * (refused before 'values' is read), give NULL */
        cleaner(multiFenwickFree) multiFenwick *n1 =
            multiFenwickNewFromArray(NULL, 5);
        cleaner(multiFenwickFree) multiFenwick *n2 =
            multiFenwickNewFromArray(base, (size_t)MULTIFENWICK_INDEX_MAX + 2);
        cleaner(multiFenwickFree) multiFenwick *n3 =
            multiFenwickNewFromArray(base, SIZE_MAX);
        if (n1 || n2 || n3) {
            ERR("NULL values or an oversized count was accepted");
        }

        /* Every count from 1 to 70, crossing each power of two up to 64
         * exactly: the built tree holds the same elements and prefix sums
         * as the values applied one Update at a time.  Integer counts use
         * signed values up to 2^40; every third count uses halves, which
         * double sums keep exact. */
        constexpr int NFA_MAX = 70;
        static databox values[NFA_MAX];
        static int64_t ints[NFA_MAX];
        static double reals[NFA_MAX];
        uint64_t seed = 31337;
        bool failed = false;
        for (size_t n = 1; n <= NFA_MAX && !failed; n++) {
            const bool real = n % 3 == 0;
            for (size_t i = 0; i < n; i++) {
                const uint64_t r = randSeed(&seed);
                if (real) {
                    reals[i] = ((double)(r % 4001) - 2000.0) / 2.0;
                    values[i] = DATABOX_DOUBLE(reals[i]);
                } else {
                    ints[i] =
                        (int64_t)(r % ((uint64_t)1 << 41)) - ((int64_t)1 << 40);
                    values[i] = DATABOX_SIGNED(ints[i]);
                }
            }

            cleaner(multiFenwickFree) multiFenwick *built =
                multiFenwickNewFromArray(values, n);
            cleaner(multiFenwickFree) multiFenwick *stepped = NULL;
            for (size_t i = 0; i < n; i++) {
                if (!multiFenwickUpdate(&stepped, i, &values[i])) {
                    failed = true;
                }
            }

            if (!built || failed || multiFenwickCount(built) != n ||
                multiFenwickCount(stepped) != n) {
                ERR("n=%zu: a tree was not built", n);
                failed = true;
                break;
            }

            int64_t iSum = 0;
            double rSum = 0;
            for (size_t i = 0; i < n && !failed; i++) {
                databox el[2];
                databox sum[2];
                multiFenwick *const tr[2] = {built, stepped};
                for (size_t w = 0; w < 2; w++) {
                    if (!multiFenwickGet(tr[w], i, &el[w]) ||
                        !multiFenwickQuery(tr[w], i, &sum[w])) {
                        failed = true;
                    }
                }

                if (failed) {
                    ERR("n=%zu: a read at %zu failed", n, i);
                    break;
                }

                if (real) {
                    rSum += reals[i];
                    for (size_t w = 0; w < 2; w++) {
                        double ev;
                        double sv;
                        if (!databoxToDouble(&el[w], &ev) ||
                            !databoxToDouble(&sum[w], &sv) || ev != reals[i] ||
                            sv != rSum) {
                            ERR("n=%zu tree %zu: index %zu differs", n, w, i);
                            failed = true;
                        }
                    }
                } else {
                    iSum += ints[i];
                    for (size_t w = 0; w < 2; w++) {
                        if (!mfwIntIs(&el[w], ints[i]) ||
                            !mfwIntIs(&sum[w], iSum)) {
                            ERR("n=%zu tree %zu: index %zu differs", n, w, i);
                            failed = true;
                        }
                    }
                }
            }
        }
    }

    TEST("LowerBound: zero, negative and exact mixed targets") {
        /* [3, 0, 1, 4]: prefix sums 3, 3, 4, 8 */
        databox vals[4] = {DATABOX_SIGNED(3), DATABOX_SIGNED(0),
                           DATABOX_SIGNED(1), DATABOX_SIGNED(4)};
        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(vals, 4);
        databox f35;
        DATABOX_SET_FLOAT(&f35, 3.5f);
        const struct {
            databox target;
            size_t want;
        } cases[] = {
            {DATABOX_SIGNED(INT64_MIN), 0},
            {DATABOX_SIGNED(-5), 0},
            {DATABOX_SIGNED(0), 0},
            {DATABOX_DOUBLE(-0.0), 0},
            {DATABOX_UNSIGNED(3), 0},
            {DATABOX_DOUBLE(3.5), 2},
            {f35, 2},
            {DATABOX_SIGNED(4), 2},
            {DATABOX_DOUBLE(8.0), 3},
            {DATABOX_SIGNED(8), 3},
            {DATABOX_DOUBLE(8.000001), SIZE_MAX},
            {DATABOX_SIGNED(9), SIZE_MAX},
            {DATABOX_UNSIGNED(UINT64_MAX), SIZE_MAX},
        };

        for (size_t k = 0; mfw && k < countof(cases); k++) {
            const size_t got = multiFenwickLowerBound(mfw, &cases[k].target);
            if (got != cases[k].want) {
                ERR("case %zu: LowerBound = %zu, want %zu", k, got,
                    cases[k].want);
            }
        }

        /* Integers compare with doubles exactly: the prefix sum 2^53 + 3
         * rounds to the double 2^53 + 4, but is below it */
        const uint64_t p = ((uint64_t)1 << 53) + 3;
        const databox pBox = DATABOX_UNSIGNED(p);
        cleaner(multiFenwickFree) multiFenwick *exact =
            multiFenwickNewFromArray(&pBox, 1);
        const databox above = DATABOX_DOUBLE((double)(p + 1));
        const databox below = DATABOX_DOUBLE((double)(p - 1));
        const databox same = DATABOX_UNSIGNED(p);
        const databox next = DATABOX_UNSIGNED(p + 1);
        if (!exact || multiFenwickLowerBound(exact, &above) != SIZE_MAX ||
            multiFenwickLowerBound(exact, &below) != 0 ||
            multiFenwickLowerBound(exact, &same) != 0 ||
            multiFenwickLowerBound(exact, &next) != SIZE_MAX) {
            ERR("an integer prefix sum was compared through a double");
        }

        /* With negative elements a result is a crossing point:
         * prefix(i) >= target and (i == 0 or prefix(i - 1) < target).
         * When no prefix sum reaches the target the result is SIZE_MAX. */
        constexpr int LBN_N = 200;
        static int64_t prefix[LBN_N];
        uint64_t seed = 2024;
        cleaner(multiFenwickFree) multiFenwick *neg = NULL;
        int64_t total = 0;
        int64_t top = INT64_MIN;
        for (size_t i = 0; i < LBN_N; i++) {
            const databox v =
                DATABOX_SIGNED((int64_t)(randSeed(&seed) % 101) - 50);
            if (!multiFenwickUpdate(&neg, i, &v)) {
                ERR("Update(%zu) failed", i);
            }

            total += v.data.i64;
            prefix[i] = total;
            top = total > top ? total : top;
        }

        for (uint32_t k = 0; neg && k < 400; k++) {
            const int64_t target = (int64_t)(randSeed(&seed) % 801) - 400;
            const databox t = DATABOX_SIGNED(target);
            const size_t got = multiFenwickLowerBound(neg, &t);
            if (got == SIZE_MAX) {
                continue;
            }

            if (got >= LBN_N || prefix[got] < target ||
                (got > 0 && prefix[got - 1] >= target) || top < target) {
                ERR("LowerBound(%" PRId64 ") = %zu is not a crossing", target,
                    got);
                break;
            }
        }

        const databox unreachable = DATABOX_SIGNED(top + 1);
        if (neg && multiFenwickLowerBound(neg, &unreachable) != SIZE_MAX) {
            ERR("a target above every prefix sum was found");
        }
    }

    /* =================================================================
     * CATEGORY 6: STRESS TESTS (3 tests)
     * ================================================================= */

    TEST("stress: 10K element updates") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Add 10K elements */
        for (uint32_t i = 0; i < 10000; i++) {
            databox val = DATABOX_SIGNED((int64_t)(i + 1));
            multiFenwickUpdate(&mfw, i, &val);
        }

        if (multiFenwickCount(mfw) != 10000) {
            ERR("Count should be 10000, got %zu", multiFenwickCount(mfw));
        }

        /* Verify sum of first 10K natural numbers: n(n+1)/2 = 50005000 */
        int64_t expected = 10000LL * 10001LL / 2;
        databox sum;
        if (!multiFenwickQuery(mfw, 9999, &sum) || sum.data.i64 != expected) {
            ERR("Sum of 1..10000 should be %" PRId64 ", got %" PRId64, expected,
                sum.data.i64);
        }
    }

    TEST("stress: alternating update/query pattern") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        for (int i = 0; i < 1000; i++) {
            databox val = DATABOX_SIGNED((int64_t)i);
            multiFenwickUpdate(&mfw, i, &val);

            databox sum;
            if (!multiFenwickQuery(mfw, i, &sum)) {
                ERR("Query failed");
            }

            /* Sum should be 0 + 1 + 2 + ... + i = i(i+1)/2 */
            int64_t expected = (int64_t)i * (i + 1) / 2;
            if (sum.data.i64 != expected) {
                ERR("At iteration %d, sum should be %" PRId64 ", got %" PRId64,
                    i, expected, sum.data.i64);
            }
        }
    }

    TEST("stress: random sparse updates") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 12345;

        /* Random indices with gaps */
        for (int rep = 0; rep < 1000; rep++) {
            size_t idx = randSeed(&seed) % 10000;
            int64_t val = (int64_t)(randSeed(&seed) % 100);

            databox dval = DATABOX_SIGNED(val);
            multiFenwickSet(&mfw, idx, &dval);
        }

        /* Verify tree is consistent (no crashes on queries) */
        for (size_t i = 0; i < multiFenwickCount(mfw); i += 100) {
            databox sum;
            multiFenwickQuery(mfw, i, &sum);
            /* Just make sure it doesn't crash */
        }
    }

    /* =================================================================
     * CATEGORY 7: COMPREHENSIVE PERFORMANCE & MEMORY COMPARISON (6 tests)
     * ================================================================= */

    TEST("COMPARISON: Memory usage across sizes") {
        printf("\n=== Memory Usage Comparison ===\n");
        printf("Size     | multiFenwick | fenwick  | Naive Array | Ratio "
               "(mfw/fw)\n");
        printf("---------|--------------|----------|-------------|-------------"
               "--\n");

        size_t sizes[] = {100, 500, 1000, 2000, 3000};
        for (size_t s = 0; s < 5; s++) {
            size_t N = sizes[s];

            /* Build multiFenwick */
            cleanerName(zfree, DataboxArray) databox *mfwData =
                zcalloc(N, sizeof(databox));
            for (size_t i = 0; i < N; i++) {
                mfwData[i] = DATABOX_SIGNED((int64_t)i);
            }
            cleaner(multiFenwickFree) multiFenwick *mfw =
                multiFenwickNewFromArray(mfwData, N);
            size_t mfwBytes = multiFenwickBytes(mfw);

            /* Build fenwick */
            cleanerName(zfree, I64Array) int64_t *fwData =
                zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                fwData[i] = (int64_t)i;
            }
            void *fw = fenwickI64New();
            for (size_t i = 0; i < N; i++) {
                fenwickI64Update(&fw, i, fwData[i]);
            }
            size_t fwBytes = fenwickI64Bytes(fw);

            /* Naive array */
            size_t naiveBytes = N * sizeof(int64_t);

            double ratio = (double)mfwBytes / (double)fwBytes;
            printf("%-8zu | %12zu | %8zu | %11zu | %.2fx\n", N, mfwBytes,
                   fwBytes, naiveBytes, ratio);

            fenwickI64Free(fw);
        }
        printf("\n");
    }

    TEST("COMPARISON: Query performance across sizes") {
        printf("\n=== Query Performance Comparison (20K ops each) ===\n");
        printf("Size | multiFenwick | fenwick    | Naive      | mfw/fw  | "
               "fw/naive\n");
        printf("-----|--------------|------------|------------|---------|------"
               "---\n");

        size_t sizes[] = {100, 500, 1000, 2000, 3000};
        /* Checksum-verified smoke size; timing lives in fenwickBench */
        const size_t NUM_OPS = 20000;

        for (size_t s = 0; s < 5; s++) {
            size_t N = sizes[s];
            uint64_t seed = 12345;

            /* Build test data */
            cleanerName(zfree, DataboxArray) databox *mfwData =
                zcalloc(N, sizeof(databox));
            cleanerName(zfree, I64Array) int64_t *fwData =
                zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                int64_t val = (int64_t)((randSeed(&seed) % 1000) - 500);
                mfwData[i] = DATABOX_SIGNED(val);
                fwData[i] = val;
            }

            cleaner(multiFenwickFree) multiFenwick *mfw =
                multiFenwickNewFromArray(mfwData, N);
            void *fw = fenwickI64New();
            for (size_t i = 0; i < N; i++) {
                fenwickI64Update(&fw, i, fwData[i]);
            }
            naiveDataboxArray *naive = naiveNewDatabox(N, DATABOX_SIGNED(0));
            for (size_t i = 0; i < N; i++) {
                naive->values[i] = mfwData[i];
            }

            /* Benchmark multiFenwick */
            volatile int64_t mfwSum = 0;
            seed = 54321;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                databox result;
                multiFenwickQuery(mfw, randSeed(&seed) % N, &result);
                mfwSum += result.data.i64;
            }
            PERF_TIMERS_FINISH;
            double mfwTime = (double)lps.global.us.duration / 1e6;

            /* Benchmark fenwick */
            volatile int64_t fwSum = 0;
            seed = 54321;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                fwSum += fenwickI64Query(fw, randSeed(&seed) % N);
            }
            PERF_TIMERS_FINISH;
            double fwTime = (double)lps.global.us.duration / 1e6;

            /* Benchmark naive */
            volatile int64_t naiveSum = 0;
            seed = 54321;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                databox result;
                naiveQueryDatabox(naive, randSeed(&seed) % N, &result);
                naiveSum += result.data.i64;
            }
            PERF_TIMERS_FINISH;
            double naiveTime = (double)lps.global.us.duration / 1e6;

            /* Verify correctness */
            if (mfwSum != fwSum || mfwSum != naiveSum) {
                ERR("Checksum mismatch at N=%zu! mfw=%" PRId64 ", fw=%" PRId64
                    ", naive=%" PRId64,
                    N, (int64_t)mfwSum, (int64_t)fwSum, (int64_t)naiveSum);
            }

            printf("%4zu | %8.2f ns | %8.2f ns | %8.2f ns | %6.2fx | %7.0fx\n",
                   N, mfwTime * 1e9 / NUM_OPS, fwTime * 1e9 / NUM_OPS,
                   naiveTime * 1e9 / NUM_OPS, mfwTime / fwTime,
                   naiveTime / fwTime);

            fenwickI64Free(fw);
            naiveFreeDatabox(naive);
        }
        printf("\n");
    }

    TEST("COMPARISON: Update performance across sizes") {
        printf("\n=== Update Performance Comparison (20K ops each) ===\n");
        printf("Size | multiFenwick | fenwick    | Naive      | mfw/fw\n");
        printf("-----|--------------|------------|------------|---------\n");

        size_t sizes[] = {100, 500, 1000, 2000, 3000};
        /* Checksum-verified smoke size; timing lives in fenwickBench */
        const size_t NUM_OPS = 20000;

        for (size_t s = 0; s < 5; s++) {
            size_t N = sizes[s];
            uint64_t seed = 99999;

            cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
            void *fw = fenwickI64New();
            naiveDataboxArray *naive = naiveNewDatabox(N, DATABOX_SIGNED(0));

            /* Benchmark multiFenwick */
            seed = 99999;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                databox delta = DATABOX_SIGNED((int64_t)(randSeed(&seed) % 10));
                multiFenwickUpdate(&mfw, randSeed(&seed) % N, &delta);
            }
            PERF_TIMERS_FINISH;
            double mfwTime = (double)lps.global.us.duration / 1e6;

            /* Benchmark fenwick */
            seed = 99999;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                fenwickI64Update(&fw, randSeed(&seed) % N,
                                 (int64_t)(randSeed(&seed) % 10));
            }
            PERF_TIMERS_FINISH;
            double fwTime = (double)lps.global.us.duration / 1e6;

            /* Benchmark naive (just for reference) */
            seed = 99999;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < NUM_OPS; i++) {
                size_t idx = randSeed(&seed) % N;
                int64_t delta = (int64_t)(randSeed(&seed) % 10);
                if (idx < naive->count) {
                    databox d = DATABOX_SIGNED(delta);
                    databox temp;
                    databoxAdd(&naive->values[idx], &d, &temp);
                    naive->values[idx] = temp;
                }
            }
            PERF_TIMERS_FINISH;
            double naiveTime = (double)lps.global.us.duration / 1e6;

            printf("%4zu | %8.2f ns | %8.2f ns | %8.2f ns | %6.2fx\n", N,
                   mfwTime * 1e9 / NUM_OPS, fwTime * 1e9 / NUM_OPS,
                   naiveTime * 1e9 / NUM_OPS, mfwTime / fwTime);

            fenwickI64Free(fw);
            naiveFreeDatabox(naive);
        }
        printf("\n");
    }

    /* =================================================================
     * CATEGORY 8: FUZZING - ALL NUMERIC TYPES (4 tests)
     * ================================================================= */

    TEST("FUZZ: Pure SIGNED_64 operations (1000 random ops)") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 777;

        printf("Fuzzing SIGNED_64...\n");

        /* Random updates and queries */
        for (int op = 0; op < 1000; op++) {
            size_t idx = randSeed(&seed) % 500;
            int64_t val = (int64_t)((randSeed(&seed) % 2000) - 1000);

            databox dval = DATABOX_SIGNED(val);
            if (!multiFenwickUpdate(&mfw, idx, &dval)) {
                ERR("Update failed at op %d", op);
            }

            /* Verify with queries */
            if (op % 100 == 99) {
                for (size_t i = 0; i < 10; i++) {
                    databox result;
                    multiFenwickQuery(mfw, i, &result);
                    /* Just verify it doesn't crash */
                }
            }
        }

        printf("  Completed 1000 ops, final count=%zu, bytes=%zu\n",
               multiFenwickCount(mfw), multiFenwickBytes(mfw));
    }

    TEST("FUZZ: Pure UNSIGNED_64 operations (1000 random ops)") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 888;

        printf("Fuzzing UNSIGNED_64...\n");

        for (int op = 0; op < 1000; op++) {
            size_t idx = randSeed(&seed) % 500;
            uint64_t val = randSeed(&seed) % 10000;

            databox dval = DATABOX_UNSIGNED(val);
            if (!multiFenwickUpdate(&mfw, idx, &dval)) {
                ERR("Update failed at op %d", op);
            }

            if (op % 100 == 99) {
                for (size_t i = 0; i < 10; i++) {
                    databox result;
                    multiFenwickQuery(mfw, i, &result);
                }
            }
        }

        printf("  Completed 1000 ops, final count=%zu, bytes=%zu\n",
               multiFenwickCount(mfw), multiFenwickBytes(mfw));
    }

    TEST("FUZZ: Pure DOUBLE_64 operations (1000 random ops)") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 999;

        printf("Fuzzing DOUBLE_64...\n");

        for (int op = 0; op < 1000; op++) {
            size_t idx = randSeed(&seed) % 500;
            double val = ((double)(randSeed(&seed) % 10000) - 5000.0) / 100.0;

            databox dval = DATABOX_DOUBLE(val);
            if (!multiFenwickUpdate(&mfw, idx, &dval)) {
                ERR("Update failed at op %d", op);
            }

            if (op % 100 == 99) {
                for (size_t i = 0; i < 10; i++) {
                    databox result;
                    multiFenwickQuery(mfw, i, &result);
                }
            }
        }

        printf("  Completed 1000 ops, final count=%zu, bytes=%zu\n",
               multiFenwickCount(mfw), multiFenwickBytes(mfw));
    }

    TEST("FUZZ: MIXED TYPES - all combinations (2000 random ops)") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 111;

        printf("Fuzzing MIXED TYPES (SIGNED, UNSIGNED, FLOAT, DOUBLE)...\n");

        /* Track what we've stored for verification */
        cleanerName(zfree, DataboxArray) databox *expectedValues =
            zcalloc(1000, sizeof(databox));
        for (size_t i = 0; i < 1000; i++) {
            expectedValues[i] = DATABOX_SIGNED(0);
        }

        for (int op = 0; op < 2000; op++) {
            size_t idx = randSeed(&seed) % 1000;
            int typeChoice = randSeed(&seed) % 4;

            databox delta;
            double deltaVal = 0.0;

            switch (typeChoice) {
            case 0: /* SIGNED_64 */
            {
                int64_t val = (int64_t)((randSeed(&seed) % 200) - 100);
                delta = DATABOX_SIGNED(val);
                deltaVal = (double)val;
            } break;
            case 1: /* UNSIGNED_64 */
            {
                uint64_t val = randSeed(&seed) % 200;
                delta = DATABOX_UNSIGNED(val);
                deltaVal = (double)val;
            } break;
            case 2: /* FLOAT_32 */
            {
                float val = ((float)(randSeed(&seed) % 200) - 100.0f) / 10.0f;
                DATABOX_SET_FLOAT(&delta, val);
                deltaVal = (double)val;
            } break;
            case 3: /* DOUBLE_64 */
            {
                double val = ((double)(randSeed(&seed) % 200) - 100.0) / 10.0;
                delta = DATABOX_DOUBLE(val);
                deltaVal = val;
            } break;
            }

            /* Update both */
            if (!multiFenwickUpdate(&mfw, idx, &delta)) {
                ERR("Update failed at op %d", op);
            }

            /* Track expected value */
            double oldVal = 0;
            if (!databoxToDouble(&expectedValues[idx], &oldVal)) {
                ERR("op %d: the oracle at %zu is not a number", op, idx);
                break;
            }

            expectedValues[idx] = databoxFromDouble(
                oldVal + deltaVal,
                databoxResultType(&expectedValues[idx], &delta));

            /* Periodically verify consistency */
            if (op % 250 == 249) {
                printf("  Verified %d ops, checking sample values...\n",
                       op + 1);
                for (size_t i = 0; i < 50; i += 10) {
                    databox result;
                    double gotVal = 0;
                    double expectVal = 0;
                    if (!multiFenwickGet(mfw, i, &result) ||
                        !databoxToDouble(&result, &gotVal) ||
                        !databoxToDouble(&expectedValues[i], &expectVal)) {
                        ERR("Get(%zu) failed", i);
                        continue;
                    }

                    /* Allow small epsilon for floating point */
                    if (fabs(gotVal - expectVal) > 0.01) {
                        ERR("Value mismatch at idx %zu: expected %.2f, got "
                            "%.2f",
                            i, expectVal, gotVal);
                    }
                }
            }
        }

        printf("  MIXED TYPE FUZZING COMPLETE!\n");
        printf("  Final count=%zu, bytes=%zu\n", multiFenwickCount(mfw),
               multiFenwickBytes(mfw));
    }

    /* =================================================================
     * CATEGORY 9: ORIGINAL PERFORMANCE BENCHMARKS (3 tests)
     * ================================================================= */

    TEST("BENCH: Query performance - int64 (1K elements)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 20000; /* smoke size; see fenwickBench */
        uint64_t seed = 12345;

        /* Build test data */
        cleanerName(zfree, DataboxArray) databox *init =
            zcalloc(N, sizeof(databox));
        for (size_t i = 0; i < N; i++) {
            init[i] = DATABOX_SIGNED((int64_t)((randSeed(&seed) % 1000) - 500));
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(init, N);
        naiveDataboxArray *naive = naiveNewDatabox(N, DATABOX_SIGNED(0));
        for (size_t i = 0; i < N; i++) {
            naive->values[i] = init[i];
        }

        /* Benchmark multiFenwick queries */
        volatile int64_t mfwSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            databox result;
            multiFenwickQuery(mfw, randSeed(&seed) % N, &result);
            mfwSum += result.data.i64;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "multiFenwick queries (1K)");

        /* Benchmark naive queries */
        volatile int64_t naiveSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            databox result;
            naiveQueryDatabox(naive, randSeed(&seed) % N, &result);
            naiveSum += result.data.i64;
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive queries (1K)");

        /* Verify correctness */
        if (mfwSum != naiveSum) {
            ERR("Checksum mismatch! multiFenwick: %" PRId64 ", Naive: %" PRId64,
                (int64_t)mfwSum, (int64_t)naiveSum);
        }
        printf("    Checksum verified: %" PRId64 "\n", (int64_t)mfwSum);

        naiveFreeDatabox(naive);
    }

    TEST("BENCH: Update performance - double (500 elements)") {
        const size_t N = 500;
        const size_t NUM_OPS = 20000; /* smoke size; see fenwickBench */
        uint64_t seed = 99999;

        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();

        /* Benchmark multiFenwick updates */
        seed = 99999;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            double val = (double)(randSeed(&seed) % 100) / 10.0;
            databox dval = DATABOX_DOUBLE(val);
            multiFenwickUpdate(&mfw, idx, &dval);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "multiFenwick updates (500 doubles)");

        printf("    Final tree size: %zu bytes\n", multiFenwickBytes(mfw));
    }

    TEST("BENCH: Mixed workload - uint64 (1K elements)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 20000; /* smoke size; see fenwickBench */
        uint64_t seed = 11111;

        cleanerName(zfree, DataboxArray) databox *init =
            zcalloc(N, sizeof(databox));
        for (size_t i = 0; i < N; i++) {
            init[i] = DATABOX_UNSIGNED((uint64_t)(randSeed(&seed) % 1000));
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(init, N);

        /* Mixed: 50% query, 50% update */
        volatile uint64_t checksum = 0;
        seed = 11111;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            if (i % 2 == 0) {
                databox result;
                multiFenwickQuery(mfw, idx, &result);
                checksum += result.data.u64;
            } else {
                uint64_t val = randSeed(&seed) % 10;
                databox dval = DATABOX_UNSIGNED(val);
                multiFenwickUpdate(&mfw, idx, &dval);
            }
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "multiFenwick mixed (1K uint64)");

        printf("    Checksum: %" PRIu64 "\n", (uint64_t)checksum);
    }

    TEST("reads match the flat tree: every prefix, element and lower bound") {
        /* An independent implementation over the same values (5000 cells
         * span many list nodes, so each read walks between them); values
         * are non-negative, so prefix sums rise and lower bounds exist */
        constexpr int cells = 5000;
        uint64_t seed = 77;
        cleanerName(zfree, DataboxArray) databox *values =
            zcalloc(cells, sizeof(*values));
        for (size_t i = 0; i < cells; i++) {
            values[i] = DATABOX_SIGNED((int64_t)(randSeed(&seed) % 100));
        }

        cleaner(multiFenwickFree) multiFenwick *tree =
            multiFenwickNewFromArray(values, cells);
        cleaner(multiFenwickFlatFree) multiFenwickFlat *flat =
            multiFenwickFlatNewFromArray(values, cells);
        size_t mismatches = 0;
        for (size_t i = 0; i < cells; i++) {
            databox a = {};
            databox b = {};
            databox ea = {};
            databox eb = {};
            if (!multiFenwickQuery(tree, i, &a) ||
                !multiFenwickFlatQuery(flat, i, &b) || a.data.i != b.data.i ||
                !multiFenwickGet(tree, i, &ea) ||
                !multiFenwickFlatGet(flat, i, &eb) || ea.data.i != eb.data.i) {
                mismatches++;
            }
        }

        for (int k = 0; k < 2000; k++) {
            const databox t =
                DATABOX_SIGNED((int64_t)(randSeed(&seed) % (cells * 50)));
            if (multiFenwickLowerBound(tree, &t) !=
                multiFenwickFlatLowerBound(flat, &t)) {
                mismatches++;
            }
        }

        if (mismatches) {
            ERR("%zu reads differ from the flat tree", mismatches);
        }
    }

    TEST("Repr: nil, empty and holding values") {
        multiFenwickRepr(NULL);
        multiFenwick *mfw = multiFenwickNew();
        multiFenwickRepr(mfw);
        const databox one = DATABOX_SIGNED(1);
        multiFenwickUpdate(&mfw, 5, &one);
        multiFenwickRepr(mfw);
        multiFenwickFree(mfw);
    }

    TEST_FINAL_RESULT;
}
