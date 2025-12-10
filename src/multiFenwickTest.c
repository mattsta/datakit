#include "multiFenwick.h"
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

/* Naive implementation for benchmark comparison */
typedef struct {
    databox *values;
    size_t count;
} naiveDataboxArray;

static naiveDataboxArray *naiveNewDatabox(size_t n, databox zero) {
    naiveDataboxArray *arr = zmalloc(sizeof(*arr));
    arr->count = n;
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

int multiFenwickTest(int argc, char *argv[]) {
    int err = 0;

    (void)argc;
    (void)argv;

    /* =================================================================
     * CATEGORY 1: BASIC OPERATIONS - SIGNED INT64 (6 tests)
     * ================================================================= */

    TEST("basic int64: empty tree operations") {
        multiFenwick *mfw = multiFenwickNew();

        if (multiFenwickCount(mfw) != 0) {
            ERR("Empty tree should have count 0, got %zu",
                multiFenwickCount(mfw));
        }

        /* Query on empty tree should return VOID */
        databox result;
        if (multiFenwickQuery(mfw, 0, &result)) {
            ERRR("Query on empty tree should fail");
        }

        multiFenwickFree(mfw);
    }

    TEST("basic int64: single element operations") {
        multiFenwick *mfw = multiFenwickNew();
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

        multiFenwickFree(mfw);
    }

    TEST("basic int64: sequential updates") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("basic int64: prefix sum correctness") {
        databox values[8];
        int64_t vals[] = {3, 1, 4, 1, 5, 9, 2, 6};
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 8);

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

        multiFenwickFree(mfw);
    }

    TEST("basic int64: range query correctness") {
        databox values[8];
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED((int64_t)(i + 1));
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 8);

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

        multiFenwickFree(mfw);
    }

    TEST("basic int64: set operation") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    /* =================================================================
     * CATEGORY 2: FLOATING POINT OPERATIONS (4 tests)
     * ================================================================= */

    TEST("float64: basic operations with doubles") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("float64: range query with doubles") {
        databox values[4];
        for (int i = 0; i < 4; i++) {
            values[i] = DATABOX_DOUBLE((double)(i + 1) * 1.1);
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 4);

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

        multiFenwickFree(mfw);
    }

    TEST("float32: operations with floats") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("float64: precision with large sums") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    /* =================================================================
     * CATEGORY 3: UNSIGNED INTEGER OPERATIONS (2 tests)
     * ================================================================= */

    TEST("uint64: basic unsigned operations") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("uint64: large unsigned values") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    /* =================================================================
     * CATEGORY 4: EDGE CASES (5 tests)
     * ================================================================= */

    TEST("edge case: zero values handling") {
        databox values[5];
        for (int i = 0; i < 5; i++) {
            values[i] = DATABOX_SIGNED(0);
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 5);

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

        multiFenwickFree(mfw);
    }

    TEST("edge case: negative deltas") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("edge case: sparse array (large index gaps)") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("edge case: boundary indices") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
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

        multiFenwick *mfw = NULL;
        databox one = DATABOX_SIGNED(10);
        bool success = multiFenwickUpdate(&mfw, 0, &one);
        if (!success || mfw == NULL) {
            ERRR("Update should create tree if NULL");
        }

        multiFenwickFree(mfw);
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

        multiFenwick *mfw = multiFenwickNewFromArray(values, 5);

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

        multiFenwickFree(mfw);
    }

    TEST("advanced: clear operation") {
        databox values[5];
        for (int i = 0; i < 5; i++) {
            values[i] = DATABOX_SIGNED((int64_t)(i + 1));
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 5);

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

        multiFenwickFree(mfw);
    }

    TEST("advanced: newFromArray construction") {
        databox values[8];
        int64_t vals[] = {5, 2, 8, 1, 9, 3, 7, 4};
        for (int i = 0; i < 8; i++) {
            values[i] = DATABOX_SIGNED(vals[i]);
        }

        multiFenwick *mfw = multiFenwickNewFromArray(values, 8);

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

        multiFenwickFree(mfw);
    }

    /* =================================================================
     * CATEGORY 6: STRESS TESTS (3 tests)
     * ================================================================= */

    TEST("stress: 10K element updates") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("stress: alternating update/query pattern") {
        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("stress: random sparse updates") {
        multiFenwick *mfw = multiFenwickNew();
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

        multiFenwickFree(mfw);
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
            databox *mfwData = zcalloc(N, sizeof(databox));
            for (size_t i = 0; i < N; i++) {
                mfwData[i] = DATABOX_SIGNED((int64_t)i);
            }
            multiFenwick *mfw = multiFenwickNewFromArray(mfwData, N);
            size_t mfwBytes = multiFenwickBytes(mfw);

            /* Build fenwick */
            int64_t *fwData = zcalloc(N, sizeof(int64_t));
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

            multiFenwickFree(mfw);
            fenwickI64Free(fw);
            zfree(mfwData);
            zfree(fwData);
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
            databox *mfwData = zcalloc(N, sizeof(databox));
            int64_t *fwData = zcalloc(N, sizeof(int64_t));
            for (size_t i = 0; i < N; i++) {
                int64_t val = (int64_t)((randSeed(&seed) % 1000) - 500);
                mfwData[i] = DATABOX_SIGNED(val);
                fwData[i] = val;
            }

            multiFenwick *mfw = multiFenwickNewFromArray(mfwData, N);
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

            multiFenwickFree(mfw);
            fenwickI64Free(fw);
            naiveFreeDatabox(naive);
            zfree(mfwData);
            zfree(fwData);
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

            multiFenwick *mfw = multiFenwickNew();
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

            multiFenwickFree(mfw);
            fenwickI64Free(fw);
            naiveFreeDatabox(naive);
        }
        printf("\n");
    }

    /* =================================================================
     * CATEGORY 8: FUZZING - ALL NUMERIC TYPES (4 tests)
     * ================================================================= */

    TEST("FUZZ: Pure SIGNED_64 operations (1000 random ops)") {
        multiFenwick *mfw = multiFenwickNew();
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
        multiFenwickFree(mfw);
    }

    TEST("FUZZ: Pure UNSIGNED_64 operations (1000 random ops)") {
        multiFenwick *mfw = multiFenwickNew();
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
        multiFenwickFree(mfw);
    }

    TEST("FUZZ: Pure DOUBLE_64 operations (1000 random ops)") {
        multiFenwick *mfw = multiFenwickNew();
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
        multiFenwickFree(mfw);
    }

    TEST("FUZZ: MIXED TYPES - all combinations (2000 random ops)") {
        multiFenwick *mfw = multiFenwickNew();
        uint64_t seed = 111;

        printf("Fuzzing MIXED TYPES (SIGNED, UNSIGNED, FLOAT, DOUBLE)...\n");

        /* Track what we've stored for verification */
        databox *expectedValues = zcalloc(1000, sizeof(databox));
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

        multiFenwickFree(mfw);
        zfree(expectedValues);
    }

    /* =================================================================
     * CATEGORY 9: ORIGINAL PERFORMANCE BENCHMARKS (3 tests)
     * ================================================================= */

    TEST("BENCH: Query performance - int64 (1K elements)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 1000000; /* 1M queries */
        uint64_t seed = 12345;

        /* Build test data */
        databox *init = zcalloc(N, sizeof(databox));
        for (size_t i = 0; i < N; i++) {
            init[i] = DATABOX_SIGNED((int64_t)((randSeed(&seed) % 1000) - 500));
        }

        multiFenwick *mfw = multiFenwickNewFromArray(init, N);
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

        multiFenwickFree(mfw);
        naiveFreeDatabox(naive);
        zfree(init);
    }

    TEST("BENCH: Update performance - double (500 elements)") {
        const size_t N = 500;
        const size_t NUM_OPS = 1000000; /* 1M updates */
        uint64_t seed = 99999;

        multiFenwick *mfw = multiFenwickNew();

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

        multiFenwickFree(mfw);
    }

    TEST("BENCH: Mixed workload - uint64 (1K elements)") {
        const size_t N = 1000;
        const size_t NUM_OPS = 1000000; /* 1M mixed ops */
        uint64_t seed = 11111;

        databox *init = zcalloc(N, sizeof(databox));
        for (size_t i = 0; i < N; i++) {
            init[i] = DATABOX_UNSIGNED((uint64_t)(randSeed(&seed) % 1000));
        }

        multiFenwick *mfw = multiFenwickNewFromArray(init, N);

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

        multiFenwickFree(mfw);
        zfree(init);
    }

    TEST_FINAL_RESULT;
}
