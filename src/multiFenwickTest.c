#include "multiFenwick.h"
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "fenwick/fenwickI64.h" /* Updated for new 2-tier system */
#include "multiFenwickCommon.h"
#include "multilist.h"
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

int multiFenwickTest(int argc, char *argv[]) {
    int err = 0;

    (void)argc;
    (void)argv;

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
            ERRR("Query on empty tree should fail");
        }
    }

    TEST("basic int64: single element operations") {
        cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
        bool success;

        /* Add single element */
        databox val = DATABOX_SIGNED(42);
        success = multiFenwickUpdate(&mfw, 0, &val);
        if (!success) {
            ERRR("Failed to update element 0");
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
            ERRR("Failed to update element 0 again");
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
                ERRR("Query failed");
            }

            double val;
            databoxToDouble(&sum, &val);
            if (fabs(val - expected[i]) > 0.001) {
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
            ERRR("Range query failed");
        }

        double expected = 2.2 + 3.3;
        double val;
        databoxToDouble(&range, &val);
        if (fabs(val - expected) > 0.001) {
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
            ERRR("Get failed");
        }

        float expected = 3.0f * 2.5f; /* 7.5 */
        double val;
        databoxToDouble(&get, &val);
        if (fabsf((float)val - expected) > 0.001f) {
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
            ERRR("Query failed");
        }

        double val;
        databoxToDouble(&sum, &val);
        if (fabs(val - 100.0) > 0.01) {
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
            ERRR("Query failed");
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
            ERRR("Get(2) should be 10 after update");
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
            ERRR("Gap element should be 0");
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
            ERRR("Index 0 should work");
        }

        /* Test high index */
        databox v2 = DATABOX_SIGNED(100);
        multiFenwickSet(&mfw, 9999, &v2);

        if (!multiFenwickGet(mfw, 9999, &get) || get.data.i64 != 100) {
            ERRR("High index 9999 should work");
        }

        if (multiFenwickCount(mfw) != 10000) {
            ERR("Count should be 10000, got %zu", multiFenwickCount(mfw));
        }
    }

    TEST("edge case: NULL parameter handling") {
        multiFenwickFree(NULL); /* Should not crash */

        databox result;
        if (multiFenwickQuery(NULL, 0, &result)) {
            ERRR("Query on NULL should fail");
        }

        if (multiFenwickCount(NULL) != 0) {
            ERRR("Count on NULL should return 0");
        }

        cleaner(multiFenwickFree) multiFenwick *mfw = NULL;
        databox one = DATABOX_SIGNED(10);
        bool success = multiFenwickUpdate(&mfw, 0, &one);
        if (!success || mfw == NULL) {
            ERRR("Update should create tree if NULL");
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
            ERRR("LowerBound(100) should be SIZE_MAX (not found)");
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
            ERRR("Should be able to update after clear");
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
        databox deferred = {{0}};
        deferred.type = DATABOX_SIGNED_128; /* no payload: data.ptr NULL */
        const databox *wides[3] = {(const databox *)&wideS,
                                   (const databox *)&wideU, &deferred};

        /* A refused first update creates no tree. */
        multiFenwick *none = NULL;
        if (multiFenwickUpdate(&none, 3, wides[0]) || none) {
            ERRR("Update accepted a wide delta into an empty tree");
        }
        multiFenwickFree(none);

        enum { WIDE_N = 8 };
        const int64_t vals[WIDE_N] = {5, -2, 8, 1, 9, -3, 7, 4};
        databox values[WIDE_N];
        for (size_t i = 0; i < WIDE_N; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        cleaner(multiFenwickFree) multiFenwick *mfw =
            multiFenwickNewFromArray(values, WIDE_N);
        if (!mfw) {
            ERRR("NewFromArray of 64-bit values failed");
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
        enum { GROW_N = 130 };
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
        enum { SPARSE_N = sizeof(sparse) / sizeof(*sparse) };
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
        enum { LB_N = 200 };
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
                ERRR("Query failed");
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
        printf("\n=== Query Performance Comparison (1M ops each) ===\n");
        printf("Size | multiFenwick | fenwick    | Naive      | mfw/fw  | "
               "fw/naive\n");
        printf("-----|--------------|------------|------------|---------|------"
               "---\n");

        size_t sizes[] = {100, 500, 1000, 2000, 3000};
        const size_t NUM_OPS = 1000000;

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
                ERR("Checksum mismatch at N=%zu! mfw=%lld, fw=%lld, naive=%lld",
                    N, (long long)mfwSum, (long long)fwSum,
                    (long long)naiveSum);
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
        printf("\n=== Update Performance Comparison (1M ops each) ===\n");
        printf("Size | multiFenwick | fenwick    | Naive      | mfw/fw\n");
        printf("-----|--------------|------------|------------|---------\n");

        size_t sizes[] = {100, 500, 1000, 2000, 3000};
        const size_t NUM_OPS = 1000000;

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
            double oldVal;
            databoxToDouble(&expectedValues[idx], &oldVal);
            expectedValues[idx] = databoxFromDouble(
                oldVal + deltaVal,
                databoxResultType(&expectedValues[idx], &delta));

            /* Periodically verify consistency */
            if (op % 250 == 249) {
                printf("  Verified %d ops, checking sample values...\n",
                       op + 1);
                for (size_t i = 0; i < 50; i += 10) {
                    databox result;
                    multiFenwickGet(mfw, i, &result);

                    double gotVal, expectVal;
                    databoxToDouble(&result, &gotVal);
                    databoxToDouble(&expectedValues[i], &expectVal);

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
        const size_t NUM_OPS = 1000000; /* 1M queries */
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
        const size_t NUM_OPS = 1000000; /* 1M updates */
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
        const size_t NUM_OPS = 1000000; /* 1M mixed ops */
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

    TEST_FINAL_RESULT;
}
