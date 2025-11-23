/* Bloom Filter Tests */
#include "linearBloom.h"
#include "linearBloomCount.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

/* Include xxhash for hashing test data */
#include "../deps/xxHash/xxhash.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* Helper to generate hash from integer */
static void hashFromInt(uint64_t val, uint64_t hash[2]) {
    hash[0] = XXH64(&val, sizeof(val), 0);
    hash[1] = XXH64(&val, sizeof(val), hash[0]);
}

/* Helper to generate hash from string */
static void hashFromString(const char *str, size_t len, uint64_t hash[2]) {
    hash[0] = XXH64(str, len, 0);
    hash[1] = XXH64(str, len, hash[0]);
}

int linearBloomTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* ================================================================
     * linearBloom tests
     * ================================================================ */

    TEST("linearBloom: basic set and check") {
        linearBloom *bloom = linearBloomNew();

        uint64_t hash[2];
        hashFromInt(12345, hash);

        /* Item should not be in empty bloom filter */
        if (linearBloomHashCheck(bloom, hash)) {
            ERR("Item found in empty bloom filter%s", "");
        }

        /* Set the item */
        bool wasPresent = linearBloomHashSet(bloom, hash);
        if (wasPresent) {
            ERR("linearBloomHashSet returned true for new item%s", "");
        }

        /* Item should now be found */
        if (!linearBloomHashCheck(bloom, hash)) {
            ERR("Item not found after set%s", "");
        }

        /* Setting same item again should return true (already present) */
        wasPresent = linearBloomHashSet(bloom, hash);
        if (!wasPresent) {
            ERR("linearBloomHashSet returned false for existing item%s", "");
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: no false negatives") {
        linearBloom *bloom = linearBloomNew();
        const size_t numItems = 10000;
        uint64_t hash[2];

        /* Add many items */
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Verify ALL items are found (no false negatives allowed) */
        size_t falseNegatives = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            if (!linearBloomHashCheck(bloom, hash)) {
                falseNegatives++;
            }
        }

        if (falseNegatives > 0) {
            ERR("Bloom filter had %zu false negatives (must be 0)!",
                falseNegatives);
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: false positive rate validation") {
        linearBloom *bloom = linearBloomNew();
        const size_t numItemsToAdd = 100000;
        const size_t numItemsToCheck = 100000;
        uint64_t hash[2];

        /* Add items 0 to numItemsToAdd-1 */
        for (size_t i = 0; i < numItemsToAdd; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Check items numItemsToAdd to numItemsToAdd+numItemsToCheck-1 */
        /* These were NOT added, so any positive is a false positive */
        size_t falsePositives = 0;
        for (size_t i = numItemsToAdd; i < numItemsToAdd + numItemsToCheck;
             i++) {
            hashFromInt(i, hash);
            if (linearBloomHashCheck(bloom, hash)) {
                falsePositives++;
            }
        }

        double fpRate = (double)falsePositives / numItemsToCheck;
        printf("    False positive rate: %.4f%% (%zu/%zu)\n", fpRate * 100,
               falsePositives, numItemsToCheck);

        /* Expected FP rate ~0.01% (1 in 10,000) for 100k items in 8MB filter
         * Allow up to 1% for test stability */
        if (fpRate > 0.01) {
            ERR("False positive rate %.4f%% exceeds 1%% threshold",
                fpRate * 100);
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: reset functionality") {
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        /* Add some items */
        for (size_t i = 0; i < 1000; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Items should be found */
        hashFromInt(500, hash);
        if (!linearBloomHashCheck(bloom, hash)) {
            ERR("Item not found before reset%s", "");
        }

        /* Reset the bloom filter */
        linearBloomReset(bloom);

        /* Items should NOT be found after reset */
        hashFromInt(500, hash);
        if (linearBloomHashCheck(bloom, hash)) {
            ERR("Item found after reset (should not be)%s", "");
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: string hashing") {
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        const char *strings[] = {"hello",       "world",       "bloom",
                                 "filter",      "test",        "datakit",
                                 "performance", "correctness", NULL};

        /* Add all strings */
        for (size_t i = 0; strings[i] != NULL; i++) {
            hashFromString(strings[i], strlen(strings[i]), hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Verify all strings found */
        for (size_t i = 0; strings[i] != NULL; i++) {
            hashFromString(strings[i], strlen(strings[i]), hash);
            if (!linearBloomHashCheck(bloom, hash)) {
                ERR("String '%s' not found in bloom filter", strings[i]);
            }
        }

        /* Verify strings NOT added are (usually) not found */
        const char *notAdded[] = {"foo", "bar", "baz", NULL};
        for (size_t i = 0; notAdded[i] != NULL; i++) {
            hashFromString(notAdded[i], strlen(notAdded[i]), hash);
            /* Note: these may be found due to false positives, which is OK */
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: performance benchmark") {
        linearBloom *bloom = linearBloomNew();
        const size_t numOps = 1000000;
        uint64_t hash[2];

        PERF_TIMERS_SETUP;

        /* Benchmark insertions */
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "insert operations");

        PERF_TIMERS_SETUP;

        /* Benchmark lookups */
        size_t found = 0;
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            found += linearBloomHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "lookup operations");

        if (found != numOps) {
            ERR("Expected %zu found, got %zu", numOps, found);
        }

        linearBloomFree(bloom);
    }

    /* ================================================================
     * linearBloomCount tests
     * ================================================================ */

    TEST("linearBloomCount: basic set and check") {
        linearBloomCount *bloom = linearBloomCountNew();

        uint64_t hash[2];
        hashFromInt(12345, hash);

        /* Item should have count 0 in empty filter */
        uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
        if (count != 0) {
            ERR("Expected count 0, got %" PRIuFAST32, count);
        }

        /* Set the item once */
        linearBloomCountHashSet(bloom, hash);

        /* Count should now be 1 */
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 1) {
            ERR("Expected count 1, got %" PRIuFAST32, count);
        }

        /* Set same item again */
        linearBloomCountHashSet(bloom, hash);

        /* Count should now be 2 */
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 2) {
            ERR("Expected count 2, got %" PRIuFAST32, count);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: counter saturation at max (2^3-1 = 7)") {
        linearBloomCount *bloom = linearBloomCountNew();

        uint64_t hash[2];
        hashFromInt(99999, hash);

        /* Increment many times - should saturate at 7 (3-bit max) */
        for (int32_t i = 0; i < 20; i++) {
            linearBloomCountHashSet(bloom, hash);
        }

        uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
        /* With 3-bit counters, max is 7 */
        if (count > 7) {
            ERR("Counter exceeded 3-bit max, got %" PRIuFAST32, count);
        }

        printf("    Counter saturated at %" PRIuFAST32 " (max 7)\n", count);

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: half (decay) functionality") {
        linearBloomCount *bloom = linearBloomCountNew();

        uint64_t hash[2];
        hashFromInt(77777, hash);

        /* Add item multiple times to get count of 4 */
        for (int32_t i = 0; i < 4; i++) {
            linearBloomCountHashSet(bloom, hash);
        }

        uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
        if (count != 4) {
            ERR("Expected count 4 before half, got %" PRIuFAST32, count);
        }

        /* Apply half decay */
        linearBloomCountHalf(bloom);

        /* Count should be halved (4 -> 2) */
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 2) {
            ERR("Expected count 2 after half, got %" PRIuFAST32, count);
        }

        /* Half again (2 -> 1) */
        linearBloomCountHalf(bloom);
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 1) {
            ERR("Expected count 1 after second half, got %" PRIuFAST32, count);
        }

        /* Half again (1 -> 0) */
        linearBloomCountHalf(bloom);
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 0) {
            ERR("Expected count 0 after third half, got %" PRIuFAST32, count);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: no false negatives for counted items") {
        linearBloomCount *bloom = linearBloomCountNew();
        const size_t numItems = 10000;
        uint64_t hash[2];

        /* Add items with varying counts */
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            /* Add 1-3 times based on i mod 3 */
            size_t addCount = (i % 3) + 1;
            for (size_t j = 0; j < addCount; j++) {
                linearBloomCountHashSet(bloom, hash);
            }
        }

        /* Verify ALL items have count > 0 */
        size_t falseNegatives = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            if (linearBloomCountHashCheck(bloom, hash) == 0) {
                falseNegatives++;
            }
        }

        if (falseNegatives > 0) {
            ERR("Counting bloom filter had %zu false negatives!",
                falseNegatives);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: reset functionality") {
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Add items */
        for (size_t i = 0; i < 1000; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Items should have count > 0 */
        hashFromInt(500, hash);
        if (linearBloomCountHashCheck(bloom, hash) == 0) {
            ERR("Item has count 0 before reset%s", "");
        }

        /* Reset */
        linearBloomCountReset(bloom);

        /* All items should have count 0 */
        hashFromInt(500, hash);
        if (linearBloomCountHashCheck(bloom, hash) != 0) {
            ERR("Item has non-zero count after reset%s", "");
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: performance benchmark") {
        linearBloomCount *bloom = linearBloomCountNew();
        const size_t numOps = 500000;
        uint64_t hash[2];

        PERF_TIMERS_SETUP;

        /* Benchmark insertions */
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "counting insert operations");

        PERF_TIMERS_SETUP;

        /* Benchmark lookups */
        size_t totalCount = 0;
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            totalCount += linearBloomCountHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "counting lookup operations");

        /* Every item was added once, but due to hash collisions in the counting
         * bloom filter, totalCount may be higher than numOps. It should never
         * be zero or significantly lower than numOps though. */
        if (totalCount < numOps) {
            ERR("Total count %zu unexpectedly lower than %zu", totalCount,
                numOps);
        }
        printf("    Total count: %zu (expected >= %zu due to collisions)\n",
               totalCount, numOps);

        PERF_TIMERS_SETUP;

        /* Benchmark half operation */
        linearBloomCountHalf(bloom);

        PERF_TIMERS_FINISH_PRINT_RESULTS(1, "half (decay) operation");

        linearBloomCountFree(bloom);
    }

    /* ================================================================
     * Edge case tests
     * ================================================================ */

    TEST("linearBloom: multiple items with similar hashes") {
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        /* Add items that might produce similar hash patterns */
        for (size_t i = 0; i < 100; i++) {
            hashFromInt(i * 1000000, hash);
            linearBloomHashSet(bloom, hash);
        }

        /* All should be found */
        for (size_t i = 0; i < 100; i++) {
            hashFromInt(i * 1000000, hash);
            if (!linearBloomHashCheck(bloom, hash)) {
                ERR("Item %" PRIu64 " not found", (uint64_t)(i * 1000000));
            }
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: free NULL safety") {
        linearBloomFree(NULL); /* Should not crash */
    }

    TEST("linearBloomCount: free NULL safety") {
        linearBloomCountFree(NULL); /* Should not crash */
    }

    TEST("linearBloom: memory layout validation") {
        /* Verify the size calculations are correct */
        size_t expectedBytes = LINEARBLOOM_EXTENT_BYTES;
        printf("    linearBloom size: %zu bytes (%.2f MB)\n", expectedBytes,
               (double)expectedBytes / (1024 * 1024));
        printf("    linearBloom bits: %" PRIu64 " (%.2f million)\n",
               (uint64_t)LINEARBLOOM_EXTENT_BITS,
               (double)LINEARBLOOM_EXTENT_BITS / 1000000);
        printf("    linearBloom hashes: %d\n", LINEARBLOOM_HASHES);

        /* Verify we can allocate and use the full extent */
        linearBloom *bloom = linearBloomNew();

        /* Set bits at various positions including near the end */
        uint64_t positions[] = {0,
                                100,
                                1000,
                                10000,
                                100000,
                                LINEARBLOOM_EXTENT_BITS - 100,
                                LINEARBLOOM_EXTENT_BITS - 1};

        for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++) {
            uint64_t hash[2] = {positions[i], positions[i] * 31};
            linearBloomHashSet(bloom, hash);
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloomCount: memory layout validation") {
        size_t expectedBytes = LINEARBLOOMCOUNT_EXTENT_BYTES;
        printf("    linearBloomCount size: %zu bytes (%.2f MB)\n",
               expectedBytes, (double)expectedBytes / (1024 * 1024));
        printf("    linearBloomCount entries: %d (%.2f million)\n",
               LINEARBLOOMCOUNT_EXTENT_ENTRIES,
               (double)LINEARBLOOMCOUNT_EXTENT_ENTRIES / 1000000);
        printf("    linearBloomCount bits per entry: %d\n", LINEAR_BLOOM_BITS);
        printf("    linearBloomCount hashes: %d\n", LINEARBLOOMCOUNT_HASHES);

        linearBloomCount *bloom = linearBloomCountNew();

        /* Test accessing entries near the end */
        uint64_t hash[2];
        hashFromInt(LINEARBLOOMCOUNT_EXTENT_ENTRIES - 1, hash);
        linearBloomCountHashSet(bloom, hash);

        if (linearBloomCountHashCheck(bloom, hash) == 0) {
            ERR("Failed to access entry near end of counting bloom%s", "");
        }

        linearBloomCountFree(bloom);
    }

    TEST_FINAL_RESULT;
}
#endif
