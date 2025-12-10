/* Fenwick Tree - I64 Type Comprehensive Tests (2-TIER SYSTEM)
 * Migrated and adapted from original fenwickTest.c
 * Tests for int64_t specialization across Small and Full tiers
 */

#include "fenwickI64.h"
#include "../ctest.h"
#include "../datakit.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DATAKIT_TEST

#define DOUBLE_NEWLINE 1
#include "../perf.h"

/* Naive implementation for benchmark comparison */
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

static void naiveUpdate(naiveArray *arr, size_t idx, int64_t delta) {
    if (idx < arr->count) {
        arr->values[idx] += delta;
    }
}

static int64_t naiveQuery(const naiveArray *arr, size_t idx) {
    int64_t sum = 0;
    if (idx >= arr->count) {
        idx = arr->count - 1;
    }
    for (size_t i = 0; i <= idx; i++) {
        sum += arr->values[i];
    }
    return sum;
}

static uint64_t randSeed(uint64_t *seed) {
    *seed = (*seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *seed;
}

int fenwickI64Test(int argc, char *argv[]) {
    int err = 0;

    (void)argc;
    (void)argv;

    /* =================================================================
     * CATEGORY 1: BASIC OPERATIONS (6 tests)
     * ================================================================= */

    TEST("basic: empty tree operations") {
        void *fw = fenwickI64New();

        if (fenwickI64Count(fw) != 0) {
            ERR("Empty tree should have count 0, got %zu", fenwickI64Count(fw));
        }

        /* Query on empty tree should return 0 */
        if (fenwickI64Query(fw, 0) != 0) {
            ERRR("Query on empty tree should return 0");
        }

        /* Get on empty tree should return 0 */
        if (fenwickI64Get(fw, 0) != 0) {
            ERRR("Get on empty tree should return 0");
        }

        fenwickI64Free(fw);
    }

    TEST("basic: single element operations") {
        void *fw = fenwickI64New();
        bool success;

        /* Add single element */
        success = fenwickI64Update(&fw, 0, 42);
        if (!success) {
            ERRR("Failed to update element 0");
        }

        if (fenwickI64Count(fw) != 1) {
            ERR("Count should be 1, got %zu", fenwickI64Count(fw));
        }

        /* Query should return the element */
        if (fenwickI64Query(fw, 0) != 42) {
            ERR("Query(0) should be 42, got %" PRId64, fenwickI64Query(fw, 0));
        }

        /* Get should return the element */
        if (fenwickI64Get(fw, 0) != 42) {
            ERR("Get(0) should be 42, got %" PRId64, fenwickI64Get(fw, 0));
        }

        /* Update again (add delta) */
        success = fenwickI64Update(&fw, 0, 8);
        if (!success) {
            ERRR("Failed to update element 0 again");
        }

        if (fenwickI64Get(fw, 0) != 50) {
            ERR("After adding 8, Get(0) should be 50, got %" PRId64,
                fenwickI64Get(fw, 0));
        }

        fenwickI64Free(fw);
    }

    TEST("basic: sequential updates (small tier)") {
        void *fw = fenwickI64New();

        /* Build array [1, 2, 3, 4, 5] */
        for (size_t i = 0; i < 5; i++) {
            fenwickI64Update(&fw, i, (int64_t)(i + 1));
        }

        if (fenwickI64Count(fw) != 5) {
            ERR("Count should be 5, got %zu", fenwickI64Count(fw));
        }

        /* Verify prefix sums: [1, 3, 6, 10, 15] */
        int64_t expected[] = {1, 3, 6, 10, 15};
        for (size_t i = 0; i < 5; i++) {
            int64_t sum = fenwickI64Query(fw, i);
            if (sum != expected[i]) {
                ERR("Query(%zu) should be %" PRId64 ", got %" PRId64, i,
                    expected[i], sum);
            }
        }

        /* Verify individual elements */
        for (size_t i = 0; i < 5; i++) {
            int64_t val = fenwickI64Get(fw, i);
            if (val != (int64_t)(i + 1)) {
                ERR("Get(%zu) should be %" PRId64 ", got %" PRId64, i,
                    (int64_t)(i + 1), val);
            }
        }

        fenwickI64Free(fw);
    }

    TEST("basic: prefix sum correctness") {
        int64_t values[] = {3, 1, 4, 1, 5, 9, 2, 6};
        /* Note: Can't use fenwickI64NewFromArray yet - need to implement */
        void *fw = fenwickI64New();
        for (int i = 0; i < 8; i++) {
            fenwickI64Update(&fw, i, values[i]);
        }

        /* Prefix sums: [3, 4, 8, 9, 14, 23, 25, 31] */
        int64_t expected[] = {3, 4, 8, 9, 14, 23, 25, 31};
        for (size_t i = 0; i < 8; i++) {
            int64_t sum = fenwickI64Query(fw, i);
            if (sum != expected[i]) {
                ERR("Prefix sum at %zu should be %" PRId64 ", got %" PRId64, i,
                    expected[i], sum);
            }
        }

        fenwickI64Free(fw);
    }

    TEST("basic: range query correctness") {
        int64_t values[] = {1, 2, 3, 4, 5, 6, 7, 8};
        void *fw = fenwickI64New();
        for (int i = 0; i < 8; i++) {
            fenwickI64Update(&fw, i, values[i]);
        }

        /* Range [1, 3] = 2+3+4 = 9 */
        if (fenwickI64RangeQuery(fw, 1, 3) != 9) {
            ERR("Range [1,3] should be 9, got %" PRId64,
                fenwickI64RangeQuery(fw, 1, 3));
        }

        /* Range [0, 7] = 1+2+3+4+5+6+7+8 = 36 */
        if (fenwickI64RangeQuery(fw, 0, 7) != 36) {
            ERR("Range [0,7] should be 36, got %" PRId64,
                fenwickI64RangeQuery(fw, 0, 7));
        }

        /* Range [5, 5] = 6 */
        if (fenwickI64RangeQuery(fw, 5, 5) != 6) {
            ERR("Range [5,5] should be 6, got %" PRId64,
                fenwickI64RangeQuery(fw, 5, 5));
        }

        fenwickI64Free(fw);
    }

    TEST("basic: set operation") {
        void *fw = fenwickI64New();

        /* Set elements directly (not delta) */
        fenwickI64Set(&fw, 0, 10);
        fenwickI64Set(&fw, 1, 20);
        fenwickI64Set(&fw, 2, 30);

        if (fenwickI64Get(fw, 0) != 10) {
            ERR("Get(0) should be 10, got %" PRId64, fenwickI64Get(fw, 0));
        }
        if (fenwickI64Get(fw, 1) != 20) {
            ERR("Get(1) should be 20, got %" PRId64, fenwickI64Get(fw, 1));
        }
        if (fenwickI64Get(fw, 2) != 30) {
            ERR("Get(2) should be 30, got %" PRId64, fenwickI64Get(fw, 2));
        }

        /* Change value with Set */
        fenwickI64Set(&fw, 1, 25);
        if (fenwickI64Get(fw, 1) != 25) {
            ERR("After Set(1, 25), Get(1) should be 25, got %" PRId64,
                fenwickI64Get(fw, 1));
        }

        /* Prefix sum should reflect change: [10, 35, 65] */
        if (fenwickI64Query(fw, 1) != 35) {
            ERR("After changing index 1, Query(1) should be 35, got %" PRId64,
                fenwickI64Query(fw, 1));
        }

        fenwickI64Free(fw);
    }

    /* =================================================================
     * CATEGORY 2: TIER TRANSITIONS (2 tests - 2-TIER SYSTEM)
     * ================================================================= */

    TEST("tier upgrade: debug tier type values") {
        void *fw = fenwickI64New();

        /* Debug: Check tier type constants */
        printf("    DEBUG: SMALL=%d, FULL=%d\n", FENWICK_I64_TYPE_SMALL,
               FENWICK_I64_TYPE_FULL);

        fenwickI64Update(&fw, 0, 1);
        printf("    After insert at 0: type=%d, count=%zu\n",
               FENWICK_I64_TYPE(fw), fenwickI64Count(fw));

        fenwickI64Update(&fw, 20000, 1);
        printf("    After insert at 20000: type=%d, count=%zu\n",
               FENWICK_I64_TYPE(fw), fenwickI64Count(fw));

        fenwickI64Free(fw);
    }

    TEST("tier upgrade: small to full at threshold (2-TIER)") {
        void *fw = fenwickI64New();

        /* Add 100 elements - should stay in Small */
        for (uint32_t i = 0; i < 100; i++) {
            fenwickI64Update(&fw, i, 1);
        }

        fenwickI64Type type1 = FENWICK_I64_TYPE(fw);
        if (type1 != FENWICK_I64_TYPE_SMALL) {
            ERR("100 elements should be SMALL tier, got type=%d", type1);
        }

        /* Force upgrade by adding at large index (beyond Small threshold) */
        fenwickI64Update(&fw, 20000, 42);

        fenwickI64Type type2 = FENWICK_I64_TYPE(fw);
        if (type2 != FENWICK_I64_TYPE_FULL) {
            ERR("After idx 20000, should upgrade to FULL tier, got type=%d",
                type2);
        }

        /* Verify original 100 values preserved */
        for (uint32_t i = 0; i < 100; i++) {
            if (fenwickI64Get(fw, i) != 1) {
                ERR("After upgrade, element %u should be 1, got %" PRId64, i,
                    fenwickI64Get(fw, i));
            }
        }

        /* Verify new value */
        if (fenwickI64Get(fw, 20000) != 42) {
            ERR("Element 20000 should be 42, got %" PRId64,
                fenwickI64Get(fw, 20000));
        }

        fenwickI64Free(fw);
    }

    TEST("tier upgrade: data integrity across transition (2-TIER)") {
        int64_t values[100];
        for (int i = 0; i < 100; i++) {
            values[i] = i + 1;
        }

        void *fw = fenwickI64New();
        for (int i = 0; i < 100; i++) {
            fenwickI64Update(&fw, i, values[i]);
        }

        /* Should be small tier */
        if (FENWICK_I64_TYPE(fw) != FENWICK_I64_TYPE_SMALL) {
            ERRR("100 elements should be in SMALL tier");
        }

        /* Force upgrade to Full (2-tier: no Medium!) */
        fenwickI64Update(&fw, 20000, 42);

        if (FENWICK_I64_TYPE(fw) != FENWICK_I64_TYPE_FULL) {
            ERRR("Should be in FULL tier after exceeding small threshold");
        }

        /* Verify original data still intact */
        for (int i = 0; i < 100; i++) {
            if (fenwickI64Get(fw, i) != i + 1) {
                ERR("After upgrade, element %d should be %d, got %" PRId64, i,
                    i + 1, fenwickI64Get(fw, i));
            }
        }

        fenwickI64Free(fw);
    }

    /* =================================================================
     * CATEGORY 3: EDGE CASES (5 tests)
     * ================================================================= */

    TEST("edge case: zero values handling") {
        void *fw = fenwickI64New();
        for (int i = 0; i < 5; i++) {
            fenwickI64Update(&fw, i, 0);
        }

        /* All prefix sums should be 0 */
        for (size_t i = 0; i < 5; i++) {
            if (fenwickI64Query(fw, i) != 0) {
                ERR("Query(%zu) of all zeros should be 0, got %" PRId64, i,
                    fenwickI64Query(fw, i));
            }
        }

        /* Add some non-zero values */
        fenwickI64Update(&fw, 2, 10);

        if (fenwickI64Get(fw, 2) != 10) {
            ERRR("Get(2) should be 10 after update");
        }

        if (fenwickI64Query(fw, 4) != 10) {
            ERRR("Query(4) should be 10 (only element 2 is non-zero)");
        }

        fenwickI64Free(fw);
    }

    TEST("edge case: negative deltas") {
        void *fw = fenwickI64New();

        /* Build array [10, 20, 30] */
        fenwickI64Set(&fw, 0, 10);
        fenwickI64Set(&fw, 1, 20);
        fenwickI64Set(&fw, 2, 30);

        /* Subtract from element 1 */
        fenwickI64Update(&fw, 1, -5);

        if (fenwickI64Get(fw, 1) != 15) {
            ERR("After subtracting 5 from 20, should be 15, got %" PRId64,
                fenwickI64Get(fw, 1));
        }

        /* Prefix sum should be 10 + 15 + 30 = 55 */
        if (fenwickI64Query(fw, 2) != 55) {
            ERR("Query(2) should be 55, got %" PRId64, fenwickI64Query(fw, 2));
        }

        fenwickI64Free(fw);
    }

    TEST("edge case: sparse array (large index gaps)") {
        void *fw = fenwickI64New();

        /* Add elements at sparse indices */
        fenwickI64Set(&fw, 0, 1);
        fenwickI64Set(&fw, 100, 2);
        fenwickI64Set(&fw, 1000, 3);

        if (fenwickI64Count(fw) != 1001) {
            ERR("Count should be 1001, got %zu", fenwickI64Count(fw));
        }

        /* Elements between should be 0 */
        if (fenwickI64Get(fw, 50) != 0) {
            ERRR("Gap element should be 0");
        }

        /* Range queries */
        if (fenwickI64RangeQuery(fw, 0, 100) != 3) {
            ERR("Range [0,100] should be 1+2=3, got %" PRId64,
                fenwickI64RangeQuery(fw, 0, 100));
        }

        if (fenwickI64RangeQuery(fw, 101, 999) != 0) {
            ERRR("Range [101,999] should be 0 (all zeros)");
        }

        fenwickI64Free(fw);
    }

    TEST("edge case: INT64_MAX values") {
        void *fw = fenwickI64New();

        /* Add large positive value */
        fenwickI64Set(&fw, 0, INT64_MAX - 1000);
        fenwickI64Set(&fw, 1, 500);

        /* Verify no overflow in query */
        int64_t sum = fenwickI64Query(fw, 1);
        if (sum != INT64_MAX - 500) {
            ERR("Sum should be INT64_MAX - 500, got %" PRId64, sum);
        }

        fenwickI64Free(fw);
    }

    TEST("edge case: boundary indices") {
        void *fw = fenwickI64New();

        /* Test index 0 */
        fenwickI64Set(&fw, 0, 42);
        if (fenwickI64Get(fw, 0) != 42) {
            ERRR("Index 0 should work");
        }

        /* Test high index */
        fenwickI64Set(&fw, 9999, 100);
        if (fenwickI64Get(fw, 9999) != 100) {
            ERRR("High index 9999 should work");
        }

        if (fenwickI64Count(fw) != 10000) {
            ERR("Count should be 10000, got %zu", fenwickI64Count(fw));
        }

        fenwickI64Free(fw);
    }

    /* =================================================================
     * CATEGORY 4: ADVANCED OPERATIONS (4 tests)
     * ================================================================= */

    TEST("advanced: lowerBound search") {
        int64_t values[] = {1, 2, 3, 4, 5}; /* Prefix sums: [1, 3, 6, 10, 15] */
        void *fw = fenwickI64New();
        for (int i = 0; i < 5; i++) {
            fenwickI64Update(&fw, i, values[i]);
        }

        /* Find smallest index with sum >= target */
        if (fenwickI64LowerBound(fw, 1) != 0) {
            ERR("LowerBound(1) should be 0, got %zu",
                fenwickI64LowerBound(fw, 1));
        }

        if (fenwickI64LowerBound(fw, 3) != 1) {
            ERR("LowerBound(3) should be 1, got %zu",
                fenwickI64LowerBound(fw, 3));
        }

        if (fenwickI64LowerBound(fw, 10) != 3) {
            ERR("LowerBound(10) should be 3, got %zu",
                fenwickI64LowerBound(fw, 10));
        }

        if (fenwickI64LowerBound(fw, 15) != 4) {
            ERR("LowerBound(15) should be 4, got %zu",
                fenwickI64LowerBound(fw, 15));
        }

        if (fenwickI64LowerBound(fw, 100) != SIZE_MAX) {
            ERRR("LowerBound(100) should be SIZE_MAX (not found)");
        }

        fenwickI64Free(fw);
    }

    TEST("advanced: clear operation") {
        int64_t values[] = {1, 2, 3, 4, 5};
        void *fw = fenwickI64New();
        for (int i = 0; i < 5; i++) {
            fenwickI64Update(&fw, i, values[i]);
        }

        /* Clear all values */
        fenwickI64Clear(fw);

        /* All queries should return 0 */
        for (size_t i = 0; i < 5; i++) {
            if (fenwickI64Query(fw, i) != 0) {
                ERR("After clear, Query(%zu) should be 0, got %" PRId64, i,
                    fenwickI64Query(fw, i));
            }
        }

        /* Count should remain */
        if (fenwickI64Count(fw) != 5) {
            ERRR("Count should remain 5 after clear");
        }

        /* Can still update after clear */
        fenwickI64Update(&fw, 2, 10);
        if (fenwickI64Get(fw, 2) != 10) {
            ERRR("Should be able to update after clear");
        }

        fenwickI64Free(fw);
    }

    TEST("advanced: NULL parameter handling") {
        /* Operations on NULL should be safe */
        fenwickI64Free(NULL); /* Should not crash */

        if (fenwickI64Query(NULL, 0) != 0) {
            ERRR("Query on NULL should return 0");
        }

        if (fenwickI64Get(NULL, 0) != 0) {
            ERRR("Get on NULL should return 0");
        }

        if (fenwickI64Count(NULL) != 0) {
            ERRR("Count on NULL should return 0");
        }

        if (fenwickI64Bytes(NULL) != 0) {
            ERRR("Bytes on NULL should return 0");
        }

        void *fw = NULL;
        bool success = fenwickI64Update(&fw, 0, 10);
        if (!success) {
            ERRR("Update should create tree if NULL");
        }
        if (fw == NULL) {
            ERRR("Tree should be created");
        }

        fenwickI64Free(fw);
    }

    /* =================================================================
     * CATEGORY 5: PERFORMANCE & STRESS (4 tests)
     * ================================================================= */

    TEST("stress: 10K element updates") {
        void *fw = fenwickI64New();

        /* Add 10K elements */
        for (uint32_t i = 0; i < 10000; i++) {
            fenwickI64Update(&fw, i, (int64_t)(i + 1));
        }

        if (fenwickI64Count(fw) != 10000) {
            ERR("Count should be 10000, got %zu", fenwickI64Count(fw));
        }

        /* Verify sum of first 10K natural numbers: n(n+1)/2 = 50005000 */
        int64_t expected = 10000LL * 10001LL / 2;
        if (fenwickI64Query(fw, 9999) != expected) {
            ERR("Sum of 1..10000 should be %" PRId64 ", got %" PRId64, expected,
                fenwickI64Query(fw, 9999));
        }

        /* Random access verification */
        if (fenwickI64Get(fw, 5000) != 5001) {
            ERR("Element 5000 should be 5001, got %" PRId64,
                fenwickI64Get(fw, 5000));
        }

        fenwickI64Free(fw);
    }

    TEST("stress: alternating update/query pattern") {
        void *fw = fenwickI64New();

        for (int i = 0; i < 1000; i++) {
            fenwickI64Update(&fw, i, i);
            int64_t sum = fenwickI64Query(fw, i);
            /* Sum should be 0 + 1 + 2 + ... + i = i(i+1)/2 */
            int64_t expected = (int64_t)i * (i + 1) / 2;
            if (sum != expected) {
                ERR("At iteration %d, sum should be %" PRId64 ", got %" PRId64,
                    i, expected, sum);
            }
        }

        fenwickI64Free(fw);
    }

    TEST("stress: memory efficiency validation (2-TIER)") {
        /* Small tier: 1000 elements */
        void *fw1 = fenwickI64New();
        for (int i = 0; i < 1000; i++) {
            fenwickI64Update(&fw1, i, 1);
        }

        size_t small_bytes = fenwickI64Bytes(fw1);
        /* Should be: sizeof(fenwickI64Small) + capacity * 8
         * count=1000 → capacity=1024 (next power of 2)
         * = 8 + 1024*8 = 8,200 bytes */
        if (small_bytes < 8000 || small_bytes > 9000) {
            ERR("Small tier bytes suspicious: %zu", small_bytes);
        }

        /* Full tier: 20K elements (beyond Small threshold) */
        void *fw2 = fenwickI64New();
        for (uint32_t i = 0; i < 20000; i++) {
            fenwickI64Update(&fw2, i, 1);
        }

        /* Should be in Full tier */
        if (FENWICK_I64_TYPE(fw2) != FENWICK_I64_TYPE_FULL) {
            ERRR("20K elements should be in FULL tier");
        }

        size_t full_bytes = fenwickI64Bytes(fw2);
        /* count=20000 → capacity=32768 (next power of 2 > 20000)
         * bytes = sizeof(fenwickI64Full) + 32768 * 8 ≈ 262,176 */
        if (full_bytes < 260000 || full_bytes > 265000) {
            ERR("Full tier bytes suspicious: %zu", full_bytes);
        }

        fenwickI64Free(fw1);
        fenwickI64Free(fw2);
    }

    /* =================================================================
     * CATEGORY 6: ADVERSARIAL PATTERNS (3 tests)
     * ================================================================= */

    TEST("adversarial: backwards index access") {
        void *fw = fenwickI64New();

        /* Add elements in reverse order */
        for (int i = 999; i >= 0; i--) {
            fenwickI64Update(&fw, i, 1);
        }

        /* Verify all elements present */
        if (fenwickI64Count(fw) != 1000) {
            ERR("Count should be 1000, got %zu", fenwickI64Count(fw));
        }

        /* All elements are 1, so query at any index i should be i+1 */
        for (int i = 0; i < 1000; i++) {
            if (fenwickI64Query(fw, i) != i + 1) {
                ERR("Query(%d) should be %d, got %" PRId64, i, i + 1,
                    fenwickI64Query(fw, i));
            }
        }

        fenwickI64Free(fw);
    }

    TEST("adversarial: random sparse updates") {
        void *fw = fenwickI64New();

        /* Random indices with gaps */
        uint32_t indices[] = {5, 100, 37, 999, 2, 500, 750, 250};
        int64_t values[] = {10, 20, 30, 40, 50, 60, 70, 80};

        for (size_t i = 0; i < 8; i++) {
            fenwickI64Set(&fw, indices[i], values[i]);
        }

        /* Verify each value */
        for (size_t i = 0; i < 8; i++) {
            if (fenwickI64Get(fw, indices[i]) != values[i]) {
                ERR("Element at %u should be %" PRId64 ", got %" PRId64,
                    indices[i], values[i], fenwickI64Get(fw, indices[i]));
            }
        }

        /* Query should sum all values up to index */
        int64_t total = 10 + 20 + 30 + 40 + 50 + 60 + 70 + 80;
        if (fenwickI64Query(fw, 999) != total) {
            ERR("Total sum should be %" PRId64 ", got %" PRId64, total,
                fenwickI64Query(fw, 999));
        }

        fenwickI64Free(fw);
    }

    TEST("adversarial: extreme value ranges") {
        void *fw = fenwickI64New();

        /* Mix of very small and very large values */
        fenwickI64Set(&fw, 0, 1);
        fenwickI64Set(&fw, 1, INT64_MAX / 2);
        fenwickI64Set(&fw, 2, -INT64_MAX / 2);
        fenwickI64Set(&fw, 3, 1);

        /* Net sum should be 2 */
        if (fenwickI64Query(fw, 3) != 2) {
            ERR("Sum should be 2, got %" PRId64, fenwickI64Query(fw, 3));
        }

        /* Verify individual elements */
        if (fenwickI64Get(fw, 1) != INT64_MAX / 2) {
            ERRR("Large positive value incorrect");
        }

        if (fenwickI64Get(fw, 2) != -INT64_MAX / 2) {
            ERRR("Large negative value incorrect");
        }

        fenwickI64Free(fw);
    }

    /* =================================================================
     * Performance Benchmarks - Fenwick vs Naive Array
     * ================================================================= */

    TEST("BENCH: Small dataset (1K) - Query Performance") {
        const size_t N = 1000;
        const size_t NUM_OPS = 50000000; /* 50M for ~1 second */
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *fw = fenwickI64New();
        for (size_t i = 0; i < N; i++) {
            fenwickI64Update(&fw, i, init[i]);
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Benchmark Fenwick queries (Small tier - cache hot!) */
        volatile int64_t fwSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            fwSum += fenwickI64Query(fw, randSeed(&seed) % N);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "fenwickI64 queries (1K/Small)");

        /* Benchmark naive */
        volatile int64_t naiveSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            naiveSum += naiveQuery(naive, randSeed(&seed) % N);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive queries (1K)");

        if (fwSum != naiveSum) {
            ERR("Checksum mismatch! Fenwick: %" PRId64 ", Naive: %" PRId64,
                fwSum, naiveSum);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", fwSum);

        fenwickI64Free(fw);
        naiveFree(naive);
        zfree(init);
    }

    TEST("BENCH: Medium dataset (20K) - Full Tier Performance") {
        const size_t N = 20000;          /* Forces Full tier */
        const size_t NUM_OPS = 25000000; /* 25M */
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *fw = fenwickI64New();
        for (size_t i = 0; i < N; i++) {
            fenwickI64Update(&fw, i, init[i]);
        }

        /* Verify we're in Full tier */
        if (FENWICK_I64_TYPE(fw) != FENWICK_I64_TYPE_FULL) {
            ERRR("20K elements should be in FULL tier");
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Benchmark Fenwick queries (Full tier) */
        volatile int64_t fwSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            fwSum += fenwickI64Query(fw, randSeed(&seed) % N);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS,
                                         "fenwickI64 queries (20K/Full)");

        /* Benchmark naive */
        volatile int64_t naiveSum = 0;
        seed = 54321;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            naiveSum += naiveQuery(naive, randSeed(&seed) % N);
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive queries (20K)");

        if (fwSum != naiveSum) {
            ERR("Checksum mismatch! Fenwick: %" PRId64 ", Naive: %" PRId64,
                fwSum, naiveSum);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", fwSum);

        fenwickI64Free(fw);
        naiveFree(naive);
        zfree(init);
    }

    TEST("BENCH: Update Performance") {
        const size_t N = 5000;
        const size_t NUM_OPS = 25000000;
        uint64_t seed = 12345;

        void *fw = fenwickI64New();
        naiveArray *naive = naiveNew(N);

        /* Fenwick updates */
        seed = 99999;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            fenwickI64Update(&fw, idx, (randSeed(&seed) % 10));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "fenwickI64 updates (5K)");

        /* Naive updates */
        seed = 99999;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            naiveUpdate(naive, randSeed(&seed) % N, (randSeed(&seed) % 10));
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive updates (5K)");

        fenwickI64Free(fw);
        naiveFree(naive);
    }

    TEST("BENCH: Mixed workload (50% query / 50% update)") {
        const size_t N = 10000;
        const size_t NUM_OPS = 25000000;
        uint64_t seed = 12345;

        int64_t *init = zcalloc(N, sizeof(int64_t));
        for (size_t i = 0; i < N; i++) {
            init[i] = (randSeed(&seed) % 1000) - 500;
        }

        void *fw = fenwickI64New();
        for (size_t i = 0; i < N; i++) {
            fenwickI64Update(&fw, i, init[i]);
        }

        naiveArray *naive = naiveNew(N);
        memcpy(naive->values, init, N * sizeof(int64_t));

        /* Fenwick mixed */
        volatile int64_t fwSum = 0;
        seed = 11111;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            if (i % 2 == 0) {
                fwSum += fenwickI64Query(fw, idx);
            } else {
                fenwickI64Update(&fw, idx, (randSeed(&seed) % 10));
            }
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "fenwickI64 mixed (10K)");

        /* Naive mixed */
        volatile int64_t naiveSum = 0;
        seed = 11111;
        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < NUM_OPS; i++) {
            size_t idx = randSeed(&seed) % N;
            if (i % 2 == 0) {
                naiveSum += naiveQuery(naive, idx);
            } else {
                naiveUpdate(naive, idx, (randSeed(&seed) % 10));
            }
        }
        PERF_TIMERS_FINISH_PRINT_RESULTS(NUM_OPS, "Naive mixed (10K)");

        if (fwSum != naiveSum) {
            ERR("Checksum mismatch! Fenwick: %" PRId64 ", Naive: %" PRId64,
                fwSum, naiveSum);
        }
        printf("    ✓ Checksum verified: %" PRId64 "\n", fwSum);

        fenwickI64Free(fw);
        naiveFree(naive);
        zfree(init);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
