#include "intset.h"
#include "ctest.h"
#include "datakit.h"
#include "intsetCommon.h"
#include "intsetFull.h"
#include "intsetMedium.h"
#include "intsetSmall.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

/* Run tests */
int intsetTest(int argc, char *argv[]) {
    int err = 0;

    (void)argc;
    (void)argv;

    TEST("basic small tier operations") {
        intset *is = intsetNew();
        bool success;

        intsetAdd(&is, 10, &success);
        if (!success) {
            ERRR("Failed to add 10");
        }
        intsetAdd(&is, 5, &success);
        if (!success) {
            ERRR("Failed to add 5");
        }
        intsetAdd(&is, 15, &success);
        if (!success) {
            ERRR("Failed to add 15");
        }
        intsetAdd(&is, 10, &success); /* Duplicate */
        if (success) {
            ERRR("Wrongly added duplicate 10");
        }

        if (intsetCount(is) != 3) {
            ERR("Count should be 3, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 10)) {
            ERRR("Should find 10");
        }
        if (!intsetFind(is, 5)) {
            ERRR("Should find 5");
        }
        if (!intsetFind(is, 15)) {
            ERRR("Should find 15");
        }
        if (intsetFind(is, 20)) {
            ERRR("Should not find 20");
        }

        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 5) {
            ERRR("Position 0 should be 5");
        }
        if (!intsetGet(is, 1, &val) || val != 10) {
            ERRR("Position 1 should be 10");
        }
        if (!intsetGet(is, 2, &val) || val != 15) {
            ERRR("Position 2 should be 15");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("small to medium tier upgrade") {
        intset *is = intsetNew();
        bool success;

        /* Add int16 values */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Verify still small */
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERRR("Should still be SMALL tier");
        }

        /* Add int32 value to trigger upgrade */
        intsetAdd(&is, 100000, &success);
        if (!success) {
            ERRR("Failed to add 100000");
        }

        /* Should now be medium */
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM tier after adding int32 value");
        }
        if (intsetCount(is) != 101) {
            ERR("Count should be 101, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 50)) {
            ERRR("Should find 50");
        }
        if (!intsetFind(is, 100000)) {
            ERRR("Should find 100000");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("medium to full tier upgrade") {
        intset *is = intsetNew();
        bool success;

        /* Force to medium */
        intsetAdd(&is, 100, NULL);
        intsetAdd(&is, 100000, NULL);
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM tier");
        }

        /* Add int64 value to trigger upgrade */
        intsetAdd(&is, INT64_MAX, &success);
        if (!success) {
            ERRR("Failed to add INT64_MAX");
        }

        /* Should now be full */
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should be FULL tier after adding int64 value");
        }
        if (intsetCount(is) != 3) {
            ERR("Count should be 3, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 100)) {
            ERRR("Should find 100");
        }
        if (!intsetFind(is, 100000)) {
            ERRR("Should find 100000");
        }
        if (!intsetFind(is, INT64_MAX)) {
            ERRR("Should find INT64_MAX");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("memory efficiency - tiered vs monolithic") {
        intset *is = intsetNew();

        /* Add 1000 int16 values */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, NULL);
        }

        size_t bytesBeforeLarge = intsetBytes(is);
        printf("  Before large value: %zu bytes for 1000 elements\n",
               bytesBeforeLarge);

        /* Add large value */
        intsetAdd(&is, INT64_MAX - 1000, NULL);
        size_t bytesAfterLarge = intsetBytes(is);
        printf("  After large value: %zu bytes for 1001 elements\n",
               bytesAfterLarge);

        /* In old implementation, this would be 1001 * 8 = 8008 bytes
         * In new implementation: 1000 * 2 + 1 * 8 = 2008 bytes (plus overhead)
         */
        printf("  Overhead from tier upgrade: %zu bytes\n",
               bytesAfterLarge - bytesBeforeLarge);

        /* Old monolithic would use 8x space for all elements */
        size_t oldMonolithicBytes = 1001 * 8;
        printf("  Old monolithic would use: %zu bytes\n", oldMonolithicBytes);
        printf("  Memory savings: %.1f%%\n",
               100.0 * (1.0 - (double)bytesAfterLarge / oldMonolithicBytes));

        /* Verify we're saving significant memory */
        if (bytesAfterLarge >= oldMonolithicBytes / 2) {
            ERR("Should save >50%% memory vs monolithic, actual bytes=%zu "
                "old=%zu",
                bytesAfterLarge, oldMonolithicBytes);
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("remove operations") {
        intset *is = intsetNew();
        bool success;

        for (int64_t i = 0; i < 10; i++) {
            intsetAdd(&is, i, NULL);
        }

        intsetRemove(&is, 5, &success);
        if (!success) {
            ERRR("Failed to remove 5");
        }
        if (intsetCount(is) != 9) {
            ERR("Count should be 9 after removal, got %zu", intsetCount(is));
        }
        if (intsetFind(is, 5)) {
            ERRR("Should not find removed value 5");
        }
        if (!intsetFind(is, 4)) {
            ERRR("Should still find 4");
        }
        if (!intsetFind(is, 6)) {
            ERRR("Should still find 6");
        }

        intsetRemove(&is, 5, &success); /* Already removed */
        if (success) {
            ERRR("Should not succeed removing already-removed value");
        }
        if (intsetCount(is) != 9) {
            ERR("Count should still be 9, got %zu", intsetCount(is));
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("large dataset stress test") {
        intset *is = intsetNew();

        /* Add 10,000 values across all three width categories */
        for (int64_t i = 0; i < 3000; i++) {
            intsetAdd(&is, i, NULL); /* int16 */
        }
        for (int64_t i = 40000; i < 42000; i++) {
            intsetAdd(&is, i, NULL); /* int32 */
        }
        for (int64_t i = INT64_MAX - 1000; i < INT64_MAX; i++) {
            intsetAdd(&is, i, NULL); /* int64 */
        }

        size_t totalCount = intsetCount(is);
        if (totalCount != 6000) {
            ERR("Should have 6000 elements, got %zu", totalCount);
        }

        /* Verify a few values */
        if (!intsetFind(is, 1500)) {
            ERRR("Should find 1500");
        }
        if (!intsetFind(is, 41000)) {
            ERRR("Should find 41000");
        }
        if (!intsetFind(is, INT64_MAX - 500)) {
            ERRR("Should find INT64_MAX-500");
        }

        size_t bytes = intsetBytes(is);
        size_t monolithicBytes = 6000 * 8; /* All int64 in old system */
        printf("  Tiered: %zu bytes, Monolithic: %zu bytes, Savings: %.1f%%\n",
               bytes, monolithicBytes,
               100.0 * (1.0 - (double)bytes / monolithicBytes));

        intsetFree(is);
    }

    /* ===== EDGE CASES ===== */

    TEST("empty set operations") {
        intset *is = intsetNew();

        if (intsetCount(is) != 0) {
            ERRR("Empty set should have count 0");
        }
        if (intsetBytes(is) < sizeof(intsetSmall)) {
            ERRR("Empty set should have some overhead bytes");
        }
        if (intsetFind(is, 0)) {
            ERRR("Empty set should not find any value");
        }

        int64_t val;
        if (intsetGet(is, 0, &val)) {
            ERRR("Empty set should not have position 0");
        }

        bool success;
        intsetRemove(&is, 5, &success);
        if (success) {
            ERRR("Cannot remove from empty set");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("single element operations") {
        intset *is = intsetNew();
        bool success;

        /* Add single element */
        intsetAdd(&is, 42, &success);
        if (!success) {
            ERRR("Should add first element");
        }
        if (intsetCount(is) != 1) {
            ERRR("Count should be 1");
        }
        if (!intsetFind(is, 42)) {
            ERRR("Should find 42");
        }

        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 42) {
            ERRR("Position 0 should be 42");
        }

        /* Remove the only element */
        intsetRemove(&is, 42, &success);
        if (!success) {
            ERRR("Should remove element");
        }
        if (intsetCount(is) != 0) {
            ERRR("Count should be 0 after removal");
        }
        if (intsetFind(is, 42)) {
            ERRR("Should not find removed element");
        }

        intsetFree(is);
    }

    TEST("boundary value tests") {
        intset *is = intsetNew();
        bool success;

        /* Test int16 boundaries */
        intsetAdd(&is, INT16_MIN, &success);
        if (!success) {
            ERRR("Should add INT16_MIN");
        }
        intsetAdd(&is, INT16_MAX, &success);
        if (!success) {
            ERRR("Should add INT16_MAX");
        }

        /* Test int32 boundaries (triggers upgrade) */
        intsetAdd(&is, INT32_MIN, &success);
        if (!success) {
            ERRR("Should add INT32_MIN");
        }
        intsetAdd(&is, INT32_MAX, &success);
        if (!success) {
            ERRR("Should add INT32_MAX");
        }

        /* Test int64 boundaries (triggers upgrade) */
        intsetAdd(&is, INT64_MIN, &success);
        if (!success) {
            ERRR("Should add INT64_MIN");
        }
        intsetAdd(&is, INT64_MAX, &success);
        if (!success) {
            ERRR("Should add INT64_MAX");
        }

        if (intsetCount(is) != 6) {
            ERR("Should have 6 boundary values, got %zu", intsetCount(is));
        }
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should be FULL tier");
        }

        /* Verify all boundaries are found */
        if (!intsetFind(is, INT16_MIN)) {
            ERRR("Should find INT16_MIN");
        }
        if (!intsetFind(is, INT16_MAX)) {
            ERRR("Should find INT16_MAX");
        }
        if (!intsetFind(is, INT32_MIN)) {
            ERRR("Should find INT32_MIN");
        }
        if (!intsetFind(is, INT32_MAX)) {
            ERRR("Should find INT32_MAX");
        }
        if (!intsetFind(is, INT64_MIN)) {
            ERRR("Should find INT64_MIN");
        }
        if (!intsetFind(is, INT64_MAX)) {
            ERRR("Should find INT64_MAX");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("negative values handling") {
        intset *is = intsetNew();

        /* Add mix of negative and positive values */
        for (int64_t i = -100; i <= 100; i++) {
            intsetAdd(&is, i, NULL);
        }

        if (intsetCount(is) != 201) {
            ERR("Should have 201 values, got %zu", intsetCount(is));
        }

        /* Verify sorted order (negative to positive) */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != -100) {
            ERRR("First should be -100");
        }
        if (!intsetGet(is, 100, &val) || val != 0) {
            ERRR("Middle should be 0");
        }
        if (!intsetGet(is, 200, &val) || val != 100) {
            ERRR("Last should be 100");
        }

        intsetFree(is);
    }

    /* ===== SEQUENTIAL INSERTION TESTS ===== */

    TEST("ascending sequential insertion") {
        intset *is = intsetNew();

        /* Add 1000 values in ascending order */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, NULL);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify all values are present and sorted */
        for (int64_t i = 0; i < 1000; i++) {
            if (!intsetFind(is, i)) {
                ERR("Should find value %lld", (long long)i);
                break;
            }
        }

        /* Verify sorted access */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 0) {
            ERRR("First should be 0");
        }
        if (!intsetGet(is, 500, &val) || val != 500) {
            ERRR("Middle should be 500");
        }
        if (!intsetGet(is, 999, &val) || val != 999) {
            ERRR("Last should be 999");
        }

        intsetFree(is);
    }

    TEST("descending sequential insertion") {
        intset *is = intsetNew();

        /* Add 1000 values in descending order (worst case for insertion) */
        for (int64_t i = 999; i >= 0; i--) {
            intsetAdd(&is, i, NULL);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify sorted order is maintained */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 0) {
            ERRR("First should be 0");
        }
        if (!intsetGet(is, 999, &val) || val != 999) {
            ERRR("Last should be 999");
        }

        intsetFree(is);
    }

    /* ===== ADVERSARIAL PATTERNS ===== */

    TEST("adversarial: alternating extremes") {
        intset *is = intsetNew();
        bool success;

        /* Alternate between very small and very large values
         * This forces frequent array shifts in medium/full tiers */
        for (int i = 0; i < 100; i++) {
            intsetAdd(&is, i % 2 == 0 ? (int64_t)i : (INT64_MAX - i), &success);
            if (!success && i < 50) {
                ERR("Failed to add value at iteration %d", i);
                break;
            }
        }

        /* Should be in FULL tier */
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should upgrade to FULL");
        }

        /* Verify count */
        size_t count = intsetCount(is);
        if (count == 0) {
            ERRR("Should have elements");
        }
        printf("  Added %zu values with alternating extremes\n", count);

        intsetFree(is);
    }

    TEST("adversarial: worst-case tier transitions") {
        intset *is = intsetNew();

        /* Fill small tier to near capacity */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, NULL);
        }
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERRR("Should still be SMALL");
        }

        /* Add one int32 to force upgrade */
        intsetAdd(&is, 100000, NULL);
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should upgrade to MEDIUM");
        }

        /* Fill medium tier with int32 values */
        for (int64_t i = 100001; i < 100100; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Add one int64 to force upgrade */
        intsetAdd(&is, INT64_MAX, NULL);
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should upgrade to FULL");
        }

        if (intsetCount(is) != 201) {
            ERR("Should have 201 values, got %zu", intsetCount(is));
        }

        intsetRepr(is);
        intsetFree(is);
    }

    TEST("adversarial: insert at beginning pattern") {
        intset *is = intsetNew();

        /* Always insert at beginning (forces maximum shifts) */
        for (int64_t i = 1000; i > 0; i--) {
            intsetAdd(&is, i, NULL);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify smallest is first */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 1) {
            ERRR("First should be 1");
        }

        intsetFree(is);
    }

    TEST("adversarial: duplicate spam") {
        intset *is = intsetNew();
        bool success;

        /* Add value */
        intsetAdd(&is, 42, &success);
        if (!success) {
            ERRR("Should add first 42");
        }

        /* Try to add same value 1000 times */
        for (int i = 0; i < 1000; i++) {
            intsetAdd(&is, 42, &success);
            if (success) {
                ERR("Should not add duplicate at iteration %d", i);
                break;
            }
        }

        if (intsetCount(is) != 1) {
            ERR("Should still have 1 value, got %zu", intsetCount(is));
        }

        intsetFree(is);
    }

    /* ===== TIER BOUNDARY TESTS ===== */

    TEST("tier boundary: small to medium threshold") {
        intset *is = intsetNew();

        /* Add values up to just before int16 can't represent */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Should still be small */
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERRR("Should be SMALL with int16 values");
        }

        size_t bytesBeforeUpgrade = intsetBytes(is);

        /* Add first int32 value */
        intsetAdd(&is, INT16_MAX + 1, NULL);

        /* Should now be medium */
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM after int32 value");
        }

        size_t bytesAfterUpgrade = intsetBytes(is);
        printf("  Upgrade overhead: %zu bytes (%zu -> %zu)\n",
               bytesAfterUpgrade - bytesBeforeUpgrade, bytesBeforeUpgrade,
               bytesAfterUpgrade);

        /* All values should still be findable */
        if (!intsetFind(is, 500)) {
            ERRR("Should still find old int16 values");
        }
        if (!intsetFind(is, INT16_MAX + 1)) {
            ERRR("Should find new int32 value");
        }

        intsetFree(is);
    }

    TEST("tier boundary: medium to full threshold") {
        intset *is = intsetNew();

        /* Add mixed int16/int32 values */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, NULL); /* int16 */
        }
        for (int64_t i = 40000; i < 40100; i++) {
            intsetAdd(&is, i, NULL); /* int32 */
        }

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM");
        }

        /* Add first int64 value */
        intsetAdd(&is, INT32_MAX + 1LL, NULL);

        /* Should now be full */
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should be FULL after int64 value");
        }

        /* Verify all widths are preserved */
        if (!intsetFind(is, 50)) {
            ERRR("Should find int16 value");
        }
        if (!intsetFind(is, 40050)) {
            ERRR("Should find int32 value");
        }
        if (!intsetFind(is, INT32_MAX + 1LL)) {
            ERRR("Should find int64 value");
        }

        intsetRepr(is);
        intsetFree(is);
    }

    /* ===== RANDOM/FUZZING TESTS ===== */

    TEST("random insertions with verification") {
        intset *is = intsetNew();
        const int NUM_VALUES = 5000;
        int64_t *oracle = zcalloc(NUM_VALUES, sizeof(int64_t));

        /* Generate random values across all width categories */
        for (int i = 0; i < NUM_VALUES; i++) {
            int category = rand() % 3;
            if (category == 0) {
                /* int16 range */
                oracle[i] = (rand() % (INT16_MAX - INT16_MIN + 1)) + INT16_MIN;
            } else if (category == 1) {
                /* int32 range (outside int16) */
                oracle[i] = (rand() % 1000000) + INT16_MAX + 1;
            } else {
                /* int64 range (large values) */
                oracle[i] = INT64_MAX - (rand() % 10000);
            }
        }

        /* Insert all values */
        for (int i = 0; i < NUM_VALUES; i++) {
            intsetAdd(&is, oracle[i], NULL);
        }

        printf("  Inserted %d random values, final count: %zu\n", NUM_VALUES,
               intsetCount(is));
        printf("  Final tier: %s\n",
               INTSET_TYPE(is) == INTSET_TYPE_SMALL    ? "SMALL"
               : INTSET_TYPE(is) == INTSET_TYPE_MEDIUM ? "MEDIUM"
                                                       : "FULL");

        /* Verify all values are findable */
        int notFound = 0;
        for (int i = 0; i < NUM_VALUES; i++) {
            if (!intsetFind(is, oracle[i])) {
                notFound++;
            }
        }

        if (notFound > 0) {
            ERR("Failed to find %d values", notFound);
        }

        zfree(oracle);
        intsetFree(is);
    }

    TEST("fuzzing: random add/remove operations") {
        intset *is = intsetNew();
        const int NUM_OPS = 10000;
        int addCount = 0;
        int removeCount = 0;
        bool success;

        /* Track expected count */
        int64_t expectedCount = 0;

        for (int i = 0; i < NUM_OPS; i++) {
            int op = rand() % 100;

            if (op < 70) {
                /* 70% chance: add random value */
                int64_t value = rand() % 100000;
                intsetAdd(&is, value, &success);
                if (success) {
                    addCount++;
                    expectedCount++;
                }
            } else {
                /* 30% chance: remove random value */
                int64_t value = rand() % 100000;
                intsetRemove(&is, value, &success);
                if (success) {
                    removeCount++;
                    expectedCount--;
                }
            }

            /* Periodic sanity checks */
            if (i % 1000 == 0 && i > 0) {
                size_t actualCount = intsetCount(is);
                if ((int64_t)actualCount != expectedCount) {
                    ERR("Count mismatch at op %d: expected %lld, got %zu", i,
                        (long long)expectedCount, actualCount);
                    break;
                }
            }
        }

        printf("  Operations: %d adds (%d succeeded), %d removes\n",
               NUM_OPS * 70 / 100, addCount, removeCount);
        printf("  Final count: %zu (expected %lld)\n", intsetCount(is),
               (long long)expectedCount);

        if ((int64_t)intsetCount(is) != expectedCount) {
            ERR("Final count mismatch: expected %lld, got %zu",
                (long long)expectedCount, intsetCount(is));
        }

        intsetRepr(is);
        intsetFree(is);
    }

    /* ===== COMPREHENSIVE API TESTS ===== */

    TEST("intsetRandom coverage") {
        intset *is = intsetNew();

        /* Empty set */
        int64_t r = intsetRandom(is);
        (void)r; /* Random from empty is undefined but shouldn't crash */

        /* Single element */
        intsetAdd(&is, 42, NULL);
        r = intsetRandom(is);
        if (r != 42) {
            ERRR("Random from single-element set should return that element");
        }

        /* Multiple elements */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Get 100 random values and verify they're all in range */
        bool foundDifferent = false;
        int64_t first = intsetRandom(is);
        for (int i = 0; i < 100; i++) {
            r = intsetRandom(is);
            if (r < 0 || r >= 100) {
                ERR("Random value %lld out of range [0,100)", (long long)r);
                break;
            }
            if (r != first) {
                foundDifferent = true;
            }
        }

        if (!foundDifferent) {
            printf("  Warning: Got same random value 100 times (low "
                   "probability)\n");
        }

        intsetFree(is);
    }

    TEST("intsetGet out of bounds") {
        intset *is = intsetNew();
        int64_t val;

        /* Empty set */
        if (intsetGet(is, 0, &val)) {
            ERRR("Get from empty set should fail");
        }
        if (intsetGet(is, 100, &val)) {
            ERRR("Get beyond bounds should fail");
        }

        /* Add 10 elements */
        for (int64_t i = 0; i < 10; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Valid positions */
        if (!intsetGet(is, 0, &val)) {
            ERRR("Should get position 0");
        }
        if (!intsetGet(is, 9, &val)) {
            ERRR("Should get position 9");
        }

        /* Invalid positions */
        if (intsetGet(is, 10, &val)) {
            ERRR("Should not get position 10");
        }
        if (intsetGet(is, 1000, &val)) {
            ERRR("Should not get position 1000");
        }

        intsetFree(is);
    }

    TEST("NULL parameter handling") {
        intset *is = intsetNew();
        bool success;

        /* Add with NULL success pointer */
        intsetAdd(&is, 42, NULL);
        if (!intsetFind(is, 42)) {
            ERRR("Should add even with NULL success");
        }

        /* Remove with NULL success pointer */
        intsetRemove(&is, 42, NULL);
        if (intsetFind(is, 42)) {
            ERRR("Should remove even with NULL success");
        }

        /* Get with NULL value pointer */
        intsetAdd(&is, 100, NULL);
        if (!intsetGet(is, 0, NULL)) {
            ERRR("Should succeed with NULL value ptr");
        }

        /* Operations on NULL intset */
        intset *null_is = NULL;
        if (intsetCount(null_is) != 0) {
            ERRR("NULL intset should have count 0");
        }
        if (intsetFind(null_is, 42)) {
            ERRR("NULL intset should not find anything");
        }

        intsetAdd(&null_is, 42, &success);
        if (!success) {
            ERRR("Should create new intset from NULL");
        }
        if (intsetCount(null_is) != 1) {
            ERRR("Should have 1 element");
        }

        intsetFree(null_is);
        intsetFree(is);
    }

    TEST("stress: many removes from large set") {
        intset *is = intsetNew();

        /* Add 1000 values */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, NULL);
        }

        /* Remove every other value */
        for (int64_t i = 0; i < 1000; i += 2) {
            bool success;
            intsetRemove(&is, i, &success);
            if (!success) {
                ERR("Failed to remove %lld", (long long)i);
                break;
            }
        }

        if (intsetCount(is) != 500) {
            ERR("Should have 500 values left, got %zu", intsetCount(is));
        }

        /* Verify odd values remain */
        for (int64_t i = 1; i < 1000; i += 2) {
            if (!intsetFind(is, i)) {
                ERR("Should find odd value %lld", (long long)i);
                break;
            }
        }

        /* Verify even values are gone */
        for (int64_t i = 0; i < 1000; i += 2) {
            if (intsetFind(is, i)) {
                ERR("Should not find removed even value %lld", (long long)i);
                break;
            }
        }

        intsetFree(is);
    }

    /* ===== CROSS-TIER VIRTUAL MERGE ITERATION TESTS ===== */

    TEST("MEDIUM: virtual merge iteration with interleaved values") {
        /* Test that intsetGet returns values in sorted order when
         * int16 and int32 values are interleaved in the sorted sequence.
         * This exercises the virtual merge algorithm in intsetMediumGet. */
        intset *is = intsetNew();

        /* Add int16 values: 10, 30, 50, 70, 90 */
        intsetAdd(&is, 10, NULL);
        intsetAdd(&is, 30, NULL);
        intsetAdd(&is, 50, NULL);
        intsetAdd(&is, 70, NULL);
        intsetAdd(&is, 90, NULL);

        /* Add int32 values that interleave: 20000, 40000, 60000, 80000 */
        intsetAdd(&is, 20000, NULL);
        intsetAdd(&is, 40000, NULL);
        intsetAdd(&is, 60000, NULL);
        intsetAdd(&is, 80000, NULL);

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM tier");
        }

        /* Expected sorted order: 10, 30, 50, 70, 90, 20000, 40000, 60000, 80000
         */
        int64_t expected[] = {10, 30, 50, 70, 90, 20000, 40000, 60000, 80000};
        size_t count = sizeof(expected) / sizeof(expected[0]);

        if (intsetCount(is) != count) {
            ERR("Count should be %zu, got %zu", count, intsetCount(is));
        }

        /* Verify intsetGet returns values in sorted order */
        int64_t prev = INT64_MIN;
        for (size_t i = 0; i < count; i++) {
            int64_t val;
            if (!intsetGet(is, (uint32_t)i, &val)) {
                ERR("intsetGet failed at position %zu", i);
                break;
            }
            if (val != expected[i]) {
                ERR("Position %zu: got %" PRId64 " expected %" PRId64, i, val,
                    expected[i]);
            }
            if (val <= prev) {
                ERR("Out of order at position %zu: %" PRId64 " <= %" PRId64, i,
                    val, prev);
            }
            prev = val;
        }

        intsetFree(is);
    }

    TEST("MEDIUM: virtual merge with densely interleaved widths") {
        /* Test with values that truly interleave across width boundaries.
         * int16 max is 32767, so values like 32760, 32765, 32770, 32775
         * cross the int16/int32 boundary. */
        intset *is = intsetNew();

        /* Add values that cross the int16/int32 boundary */
        int64_t values[] = {32760, 32765, 32770, 32775, 32780, 32785};
        size_t count = sizeof(values) / sizeof(values[0]);

        for (size_t i = 0; i < count; i++) {
            intsetAdd(&is, values[i], NULL);
        }

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERRR("Should be MEDIUM tier (has int32 values)");
        }

        /* Verify sorted iteration */
        int64_t prev = INT64_MIN;
        for (size_t i = 0; i < count; i++) {
            int64_t val;
            if (!intsetGet(is, (uint32_t)i, &val)) {
                ERR("intsetGet failed at position %zu", i);
                break;
            }
            if (val <= prev) {
                ERR("Out of order at position %zu: %" PRId64 " <= %" PRId64, i,
                    val, prev);
            }
            prev = val;
        }

        /* Also verify specific positions match expected values */
        for (size_t i = 0; i < count; i++) {
            int64_t val;
            intsetGet(is, (uint32_t)i, &val);
            if (val != values[i]) {
                ERR("Position %zu: got %" PRId64 " expected %" PRId64, i, val,
                    values[i]);
            }
        }

        intsetFree(is);
    }

    TEST("FULL: virtual 3-way merge iteration") {
        /* Test that intsetGet returns values in sorted order when
         * int16, int32, and int64 values are all present.
         * This exercises the virtual 3-way merge algorithm in intsetFullGet. */
        intset *is = intsetNew();

        /* Add values across all three width categories */
        /* int16 values */
        intsetAdd(&is, 100, NULL);
        intsetAdd(&is, 200, NULL);
        intsetAdd(&is, 300, NULL);

        /* int32 values */
        intsetAdd(&is, 100000, NULL);
        intsetAdd(&is, 200000, NULL);
        intsetAdd(&is, 300000, NULL);

        /* int64 values - trigger upgrade to FULL */
        intsetAdd(&is, INT32_MAX + 1000LL, NULL);
        intsetAdd(&is, INT32_MAX + 2000LL, NULL);
        intsetAdd(&is, INT32_MAX + 3000LL, NULL);

        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should be FULL tier");
        }

        /* Expected sorted order */
        int64_t expected[] = {100,
                              200,
                              300,
                              100000,
                              200000,
                              300000,
                              INT32_MAX + 1000LL,
                              INT32_MAX + 2000LL,
                              INT32_MAX + 3000LL};
        size_t count = sizeof(expected) / sizeof(expected[0]);

        if (intsetCount(is) != count) {
            ERR("Count should be %zu, got %zu", count, intsetCount(is));
        }

        /* Verify intsetGet returns values in sorted order */
        int64_t prev = INT64_MIN;
        for (size_t i = 0; i < count; i++) {
            int64_t val;
            if (!intsetGet(is, (uint32_t)i, &val)) {
                ERR("intsetGet failed at position %zu", i);
                break;
            }
            if (val != expected[i]) {
                ERR("Position %zu: got %" PRId64 " expected %" PRId64, i, val,
                    expected[i]);
            }
            if (val <= prev) {
                ERR("Out of order at position %zu: %" PRId64 " <= %" PRId64, i,
                    val, prev);
            }
            prev = val;
        }

        intsetFree(is);
    }

    TEST("FULL: 3-way merge with interleaved values across all widths") {
        /* Create a scenario where values from all three widths
         * interleave in the final sorted order. */
        intset *is = intsetNew();

        /* Create interleaved pattern by using negative numbers too */
        /* int16: -1000, -100, 0, 100, 1000 */
        intsetAdd(&is, -1000, NULL);
        intsetAdd(&is, -100, NULL);
        intsetAdd(&is, 0, NULL);
        intsetAdd(&is, 100, NULL);
        intsetAdd(&is, 1000, NULL);

        /* int32: -50000, 50000 */
        intsetAdd(&is, -50000, NULL);
        intsetAdd(&is, 50000, NULL);

        /* int64: values outside int32 range */
        intsetAdd(&is, INT32_MIN - 1LL, NULL);
        intsetAdd(&is, INT32_MAX + 1LL, NULL);

        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERRR("Should be FULL tier");
        }

        size_t count = intsetCount(is);
        if (count != 9) {
            ERR("Count should be 9, got %zu", count);
        }

        /* Verify strict sorted order */
        int64_t prev = INT64_MIN;
        for (size_t i = 0; i < count; i++) {
            int64_t val;
            if (!intsetGet(is, (uint32_t)i, &val)) {
                ERR("intsetGet failed at position %zu", i);
                break;
            }
            if (val <= prev) {
                ERR("Out of order at position %zu: %" PRId64 " <= %" PRId64, i,
                    val, prev);
                break;
            }
            prev = val;
        }

        /* Verify expected order:
         * INT32_MIN-1, -50000, -1000, -100, 0, 100, 1000, 50000, INT32_MAX+1 */
        int64_t val;
        intsetGet(is, 0, &val);
        if (val != INT32_MIN - 1LL) {
            ERR("Position 0 should be INT32_MIN-1, got %" PRId64, val);
        }
        intsetGet(is, 4, &val);
        if (val != 0) {
            ERR("Position 4 should be 0, got %" PRId64, val);
        }
        intsetGet(is, 8, &val);
        if (val != INT32_MAX + 1LL) {
            ERR("Position 8 should be INT32_MAX+1, got %" PRId64, val);
        }

        intsetFree(is);
    }

    TEST("virtual merge: randomized cross-tier iteration") {
        /* Generate random values across all width categories and verify
         * that intsetGet always returns sorted order. */
        intset *is = intsetNew();
        const int NUM_VALUES = 200;
        int64_t *oracle = zcalloc(NUM_VALUES, sizeof(int64_t));

        /* Generate values across all three width categories */
        for (int i = 0; i < NUM_VALUES; i++) {
            int category = rand() % 3;
            int64_t val;
            switch (category) {
            case 0: /* int16 range */
                val = (rand() % 60000) - 30000;
                break;
            case 1: /* int32 range */
                val = ((int64_t)rand() * rand()) % 2000000000LL;
                if (rand() % 2) {
                    val = -val;
                }
                break;
            case 2: /* int64 range */
                val = ((int64_t)rand() << 32) | rand();
                if (rand() % 2) {
                    val = -val;
                }
                break;
            default:
                val = rand();
            }
            oracle[i] = val;
            intsetAdd(&is, val, NULL);
        }

        size_t count = intsetCount(is);

        /* intsetAdd deduplicates, so count may be less than NUM_VALUES */
        if (count > (size_t)NUM_VALUES) {
            ERR("Count %zu exceeds inserted %d", count, NUM_VALUES);
        }

        /* Verify strict sorted order through intsetGet */
        int64_t prev = INT64_MIN;
        bool orderOK = true;
        for (size_t i = 0; i < count && orderOK; i++) {
            int64_t val;
            if (!intsetGet(is, (uint32_t)i, &val)) {
                ERR("intsetGet failed at position %zu", i);
                orderOK = false;
                break;
            }
            if (val <= prev) {
                ERR("Out of order at position %zu: %" PRId64 " <= %" PRId64, i,
                    val, prev);
                orderOK = false;
            }
            prev = val;
        }

        if (orderOK) {
            printf("  Verified sorted order across %zu values (tier=%d)\n",
                   count, INTSET_TYPE(is));
        }

        zfree(oracle);
        intsetFree(is);
    }

    TEST_FINAL_RESULT;
}
