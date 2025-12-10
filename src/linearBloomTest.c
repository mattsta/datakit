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

    TEST("linearBloom: performance benchmark (1+ second runs)") {
        linearBloom *bloom = linearBloomNew();
        const size_t numOps = 10000000; /* 10M ops for 1+ second runtime */
        uint64_t hash[2];

        PERF_TIMERS_SETUP;

        /* Benchmark insertions - should take 1+ seconds */
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "insert operations");

        PERF_TIMERS_SETUP;

        /* Benchmark lookups (positive - items exist) */
        size_t found = 0;
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            found += linearBloomHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "lookup (positive)");

        if (found != numOps) {
            ERR("Expected %zu found, got %zu", numOps, found);
        }

        PERF_TIMERS_SETUP;

        /* Benchmark lookups (negative - items don't exist) */
        size_t falsePos = 0;
        for (size_t i = numOps; i < numOps * 2; i++) {
            hashFromInt(i, hash);
            falsePos += linearBloomHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "lookup (negative)");

        printf("    False positives in negative lookups: %zu (%.4f%%)\n",
               falsePos, (double)falsePos / numOps * 100);

        PERF_TIMERS_SETUP;

        /* Benchmark early-exit lookups (negative) */
        size_t falsePos2 = 0;
        for (size_t i = numOps; i < numOps * 2; i++) {
            hashFromInt(i, hash);
            falsePos2 += linearBloomHashCheckEarlyExit(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps,
                                         "lookup early-exit (negative)");

        if (falsePos != falsePos2) {
            ERR("Early-exit mismatch: %zu vs %zu", falsePos, falsePos2);
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

    TEST("linearBloomCount: performance benchmark (1+ second runs)") {
        linearBloomCount *bloom = linearBloomCountNew();
        const size_t numOps = 3000000; /* 3M ops for 1+ second runtime */
        uint64_t hash[2];

        PERF_TIMERS_SETUP;

        /* Benchmark insertions - should take 1+ seconds */
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "counting insert operations");

        PERF_TIMERS_SETUP;

        /* Benchmark lookups (positive) */
        size_t totalCount = 0;
        for (size_t i = 0; i < numOps; i++) {
            hashFromInt(i, hash);
            totalCount += linearBloomCountHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "counting lookup (positive)");

        if (totalCount < numOps) {
            ERR("Total count %zu unexpectedly lower than %zu", totalCount,
                numOps);
        }
        printf("    Total count: %zu (expected >= %zu due to collisions)\n",
               totalCount, numOps);

        PERF_TIMERS_SETUP;

        /* Benchmark lookups (negative - items don't exist) */
        size_t negativeCount = 0;
        for (size_t i = numOps; i < numOps * 2; i++) {
            hashFromInt(i, hash);
            negativeCount += linearBloomCountHashCheck(bloom, hash);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "counting lookup (negative)");

        printf("    Negative lookup total count: %zu (false positive counts)\n",
               negativeCount);

        /* Benchmark multiple half operations */
        PERF_TIMERS_SETUP;

        const size_t numHalfs = 100;
        for (size_t i = 0; i < numHalfs; i++) {
            linearBloomCountHalf(bloom);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numHalfs, "half (decay) operations");

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

    /* ================================================================
     * Fuzz tests
     * ================================================================ */

    TEST("linearBloom FUZZ: random insertions and checks") {
        linearBloom *bloom = linearBloomNew();
        const size_t numItems = 50000;
        uint64_t *inserted = zcalloc(numItems, sizeof(uint64_t));
        uint64_t hash[2];

        srand(12345);

        /* Insert random items */
        for (size_t i = 0; i < numItems; i++) {
            inserted[i] = ((uint64_t)rand() << 32) | rand();
            hashFromInt(inserted[i], hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Verify no false negatives */
        size_t falseNegatives = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(inserted[i], hash);
            if (!linearBloomHashCheck(bloom, hash)) {
                falseNegatives++;
            }
        }

        if (falseNegatives > 0) {
            ERR("FUZZ: %zu false negatives detected!", falseNegatives);
        }

        /* Count false positives on random non-inserted items */
        size_t falsePositives = 0;
        for (size_t i = 0; i < numItems; i++) {
            uint64_t notInserted =
                ((uint64_t)rand() << 32) | rand() | 0x8000000000000000ULL;
            hashFromInt(notInserted, hash);
            if (linearBloomHashCheck(bloom, hash)) {
                falsePositives++;
            }
        }

        double fpRate = (double)falsePositives / numItems;
        printf("    FUZZ FP rate: %.4f%% (%zu/%zu)\n", fpRate * 100,
               falsePositives, numItems);

        zfree(inserted);
        linearBloomFree(bloom);
    }

    TEST("linearBloom: early-exit check correctness") {
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        /* Add items */
        for (size_t i = 0; i < 10000; i++) {
            hashFromInt(i, hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Verify both check variants give same results */
        size_t mismatches = 0;
        for (size_t i = 0; i < 20000; i++) {
            hashFromInt(i, hash);
            bool regular = linearBloomHashCheck(bloom, hash);
            bool earlyExit = linearBloomHashCheckEarlyExit(bloom, hash);
            if (regular != earlyExit) {
                mismatches++;
            }
        }

        if (mismatches > 0) {
            ERR("Early-exit check had %zu mismatches vs regular check!",
                mismatches);
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloomCount FUZZ: random operations with oracle") {
        linearBloomCount *bloom = linearBloomCountNew();
        const size_t numItems = 5000;
        uint64_t hash[2];

        srand(54321);

        /* Track counts in simple oracle (limited to first numItems integers) */
        uint8_t *oracle = zcalloc(numItems, sizeof(uint8_t));

        /* Add items with varying counts */
        for (size_t round = 0; round < 3; round++) {
            for (size_t i = 0; i < numItems; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
                if (oracle[i] < 7) {
                    oracle[i]++;
                }
            }
        }

        /* Verify all items have count >= oracle (may be higher due to
         * collisions) */
        size_t underCount = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            uint_fast32_t bloomCount = linearBloomCountHashCheck(bloom, hash);
            /* Count should be at least what we inserted (collisions can
             * increase it) */
            if (bloomCount < oracle[i]) {
                underCount++;
            }
        }

        if (underCount > 0) {
            ERR("FUZZ: %zu items had count lower than expected!", underCount);
        }

        zfree(oracle);
        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: SWAR half correctness vs scalar") {
        /* Verify the SWAR-optimized half matches scalar reference */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        /* Add various items to both filters */
        for (size_t i = 0; i < 10000; i++) {
            hashFromInt(i * 7, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        /* Apply SWAR half to bloom, scalar to reference */
        linearBloomCountHalf(bloom);
        linearBloomCountHalfScalar(reference);

        /* Verify they match byte-by-byte */
        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("SWAR half differs from scalar reference!%s", "");
        }

        /* Verify values are actually halved */
        for (size_t i = 0; i < 1000; i++) {
            hashFromInt(i * 7, hash);
            uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
            /* After one half, counts that were 1-3 should be 0-1 */
            if (count > 3) {
                ERR("After half, count %" PRIuFAST32 " is too high!", count);
            }
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: half implementation comparison benchmark") {
        printf("    === Half Implementation Performance Comparison ===\n");
        uint64_t hash[2];

        /* Test scalar implementation */
        {
            linearBloomCount *bloom = linearBloomCountNew();
            for (size_t i = 0; i < 100000; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
            }

            PERF_TIMERS_SETUP;
            linearBloomCountHalfScalar(bloom);
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "Scalar half");

            linearBloomCountFree(bloom);
        }

        /* Test SWAR implementation */
        {
            linearBloomCount *bloom = linearBloomCountNew();
            for (size_t i = 0; i < 100000; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
            }

            PERF_TIMERS_SETUP;
            linearBloomCountHalf(bloom);
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "SWAR half (default)");

            linearBloomCountFree(bloom);
        }

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* Test NEON implementation */
        {
            linearBloomCount *bloom = linearBloomCountNew();
            for (size_t i = 0; i < 100000; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
            }

            PERF_TIMERS_SETUP;
            linearBloomCountHalfNEON(bloom);
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "NEON half");

            linearBloomCountFree(bloom);
        }
#endif

#if defined(__SSE2__)
        /* Test SSE2 implementation */
        {
            linearBloomCount *bloom = linearBloomCountNew();
            for (size_t i = 0; i < 100000; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
            }

            PERF_TIMERS_SETUP;
            linearBloomCountHalfSSE2(bloom);
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "SSE2 half");

            linearBloomCountFree(bloom);
        }
#endif

#if defined(__AVX2__)
        /* Test AVX2 implementation */
        {
            linearBloomCount *bloom = linearBloomCountNew();
            for (size_t i = 0; i < 100000; i++) {
                hashFromInt(i, hash);
                linearBloomCountHashSet(bloom, hash);
            }

            PERF_TIMERS_SETUP;
            linearBloomCountHalfAVX2(bloom);
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "AVX2 half");

            linearBloomCountFree(bloom);
        }
#endif

        printf("    === End Performance Comparison ===\n");
    }

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    TEST("linearBloomCount: NEON half correctness vs scalar") {
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill with random data */
        for (size_t i = 0; i < 50000; i++) {
            hashFromInt(i * 13, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        /* Apply NEON half to bloom, scalar to reference */
        linearBloomCountHalfNEON(bloom);
        linearBloomCountHalfScalar(reference);

        /* Verify they match */
        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("NEON half differs from scalar reference!%s", "");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }
#endif

#if defined(__SSE2__)
    TEST("linearBloomCount: SSE2 half correctness vs scalar") {
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        for (size_t i = 0; i < 50000; i++) {
            hashFromInt(i * 13, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        linearBloomCountHalfSSE2(bloom);
        linearBloomCountHalfScalar(reference);

        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("SSE2 half differs from scalar reference!%s", "");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }
#endif

#if defined(__AVX2__)
    TEST("linearBloomCount: AVX2 half correctness vs scalar") {
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        for (size_t i = 0; i < 50000; i++) {
            hashFromInt(i * 13, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        linearBloomCountHalfAVX2(bloom);
        linearBloomCountHalfScalar(reference);

        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("AVX2 half differs from scalar reference!%s", "");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }
#endif

    TEST("linearBloom FUZZ: adversarial hash collision test") {
        /* Test with hashes that might produce similar bit patterns */
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        /* Add items with sequential hashes (might cluster) */
        for (size_t i = 0; i < 1000; i++) {
            hash[0] = i;
            hash[1] = i + 1;
            linearBloomHashSet(bloom, hash);
        }

        /* Verify no false negatives */
        size_t falseNeg = 0;
        for (size_t i = 0; i < 1000; i++) {
            hash[0] = i;
            hash[1] = i + 1;
            if (!linearBloomHashCheck(bloom, hash)) {
                falseNeg++;
            }
        }

        if (falseNeg > 0) {
            ERR("Adversarial test had %zu false negatives!", falseNeg);
        }

        linearBloomFree(bloom);
    }

    TEST("linearBloom: boundary bit positions") {
        linearBloom *bloom = linearBloomNew();

        /* Test setting bits at various boundary positions */
        uint64_t testPositions[] = {
            0,                           /* First bit */
            LB_BITS_PER_SLOT - 1,        /* Last bit of first slot */
            LB_BITS_PER_SLOT,            /* First bit of second slot */
            LINEARBLOOM_EXTENT_BITS / 2, /* Middle */
            LINEARBLOOM_EXTENT_BITS - LB_BITS_PER_SLOT, /* Start of last slot */
            LINEARBLOOM_EXTENT_BITS - 1                 /* Last bit */
        };

        for (size_t i = 0; i < sizeof(testPositions) / sizeof(testPositions[0]);
             i++) {
            uint64_t hash[2] = {
                testPositions[i],
                0}; /* hash[1]=0 so position = hash[0] % EXTENT */
            linearBloomHashSet(bloom, hash);
        }

        /* Verify all positions are set */
        for (size_t i = 0; i < sizeof(testPositions) / sizeof(testPositions[0]);
             i++) {
            uint64_t hash[2] = {testPositions[i], 0};
            if (!linearBloomHashCheck(bloom, hash)) {
                ERR("Boundary position %" PRIu64 " not found!",
                    testPositions[i]);
            }
        }

        linearBloomFree(bloom);
    }

    /* ================================================================
     * Extended SWAR boundary tests
     * ================================================================ */

    TEST("linearBloomCount: SWAR boundary entries (21, 42, 85, 106...)") {
        /* Test entries that span 64-bit word boundaries in the packed 3-bit
         * layout. In each 192-bit (3-word) group:
         * - Entry 21 spans words 0-1 (bit 63 of word 0, bits 0-1 of word 1)
         * - Entry 42 spans words 1-2 (bits 62-63 of word 1, bit 0 of word 2)
         * These are the critical entries for SWAR correctness. */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();

        /* Test first 1000 boundary entries */
        size_t boundaryEntries[1000];
        size_t numBoundary = 0;

        for (size_t g = 0; g < 500 && numBoundary < 1000; g++) {
            boundaryEntries[numBoundary++] = g * 64 + 21;
            if (numBoundary < 1000) {
                boundaryEntries[numBoundary++] = g * 64 + 42;
            }
        }

        /* Set boundary entries to known values using direct packed access */
        for (size_t i = 0; i < numBoundary; i++) {
            size_t entry = boundaryEntries[i];
            if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                /* Set to value 4 (will become 2 after half) */
                varintPacked3Set(bloom, entry, 4);
                varintPacked3Set(reference, entry, 4);
            }
        }

        /* Apply half using SWAR and scalar */
        linearBloomCountHalf(bloom);
        linearBloomCountHalfScalar(reference);

        /* Verify boundary entries match */
        size_t mismatches = 0;
        for (size_t i = 0; i < numBoundary; i++) {
            size_t entry = boundaryEntries[i];
            if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                uint8_t swarVal = varintPacked3Get(bloom, entry);
                uint8_t scalarVal = varintPacked3Get(reference, entry);
                if (swarVal != scalarVal) {
                    if (mismatches < 10) {
                        printf("    Entry %zu: SWAR=%u, Scalar=%u\n", entry,
                               swarVal, scalarVal);
                    }
                    mismatches++;
                }
            }
        }

        if (mismatches > 0) {
            ERR("SWAR boundary entries had %zu mismatches!", mismatches);
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: all count values 0-7 half correctly") {
        /* Test that all possible 3-bit values (0-7) are halved correctly */
        for (uint8_t val = 0; val <= 7; val++) {
            linearBloomCount *bloom = linearBloomCountNew();
            linearBloomCount *reference = linearBloomCountNew();

            /* Set first 1000 entries to this value */
            for (size_t i = 0; i < 1000; i++) {
                varintPacked3Set(bloom, i, val);
                varintPacked3Set(reference, i, val);
            }

            /* Half both */
            linearBloomCountHalf(bloom);
            linearBloomCountHalfScalar(reference);

            /* Verify all entries match */
            size_t mismatches = 0;
            for (size_t i = 0; i < 1000; i++) {
                uint8_t swarVal = varintPacked3Get(bloom, i);
                uint8_t scalarVal = varintPacked3Get(reference, i);
                if (swarVal != scalarVal) {
                    mismatches++;
                }
                /* Also verify the actual halving is correct */
                uint8_t expected = val / 2;
                if (scalarVal != expected) {
                    ERR("Value %u halved to %u, expected %u", val, scalarVal,
                        expected);
                }
            }

            if (mismatches > 0) {
                ERR("Value %u: SWAR had %zu mismatches vs scalar!", val,
                    mismatches);
            }

            linearBloomCountFree(bloom);
            linearBloomCountFree(reference);
        }
        printf("    All values 0-7 half correctly\n");
    }

    TEST("linearBloomCount: repeated half operations stress test") {
        /* Verify multiple consecutive half operations work correctly */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill with random data */
        srand(98765);
        for (size_t i = 0; i < 100000; i++) {
            hashFromInt(rand(), hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        /* Apply 10 consecutive halves */
        for (int32_t round = 0; round < 10; round++) {
            linearBloomCountHalf(bloom);
            linearBloomCountHalfScalar(reference);

            /* Verify they match after each half */
            if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
                ERR("Mismatch after half round %d!", round + 1);
            }
        }

        /* After 10 halves, most values should be 0 */
        size_t nonZero = 0;
        for (size_t i = 0; i < 10000; i++) {
            if (varintPacked3Get(bloom, i) > 0) {
                nonZero++;
            }
        }
        printf("    After 10 halves: %zu/10000 entries non-zero\n", nonZero);

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: every entry position SWAR correctness") {
        /* Test SWAR correctness for entries at every position 0-63 in a group
         */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();

        /* Set entries at every position in first few groups */
        for (size_t group = 0; group < 10; group++) {
            for (size_t pos = 0; pos < 64; pos++) {
                size_t entry = group * 64 + pos;
                if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                    varintPacked3Set(bloom, entry,
                                     6); /* High value to test halving */
                    varintPacked3Set(reference, entry, 6);
                }
            }
        }

        /* Half both */
        linearBloomCountHalf(bloom);
        linearBloomCountHalfScalar(reference);

        /* Verify every position */
        size_t mismatches = 0;
        for (size_t group = 0; group < 10; group++) {
            for (size_t pos = 0; pos < 64; pos++) {
                size_t entry = group * 64 + pos;
                if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                    uint8_t swarVal = varintPacked3Get(bloom, entry);
                    uint8_t scalarVal = varintPacked3Get(reference, entry);
                    if (swarVal != scalarVal) {
                        if (mismatches < 10) {
                            printf("    Group %zu pos %zu (entry %zu): "
                                   "SWAR=%u, Scalar=%u\n",
                                   group, pos, entry, swarVal, scalarVal);
                        }
                        mismatches++;
                    }
                }
            }
        }

        if (mismatches > 0) {
            ERR("Entry position test had %zu mismatches!", mismatches);
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloom: large-scale stress test (1M items)") {
        linearBloom *bloom = linearBloomNew();
        const size_t numItems = 1000000;
        uint64_t hash[2];

        /* Insert 1M random items */
        srand(11111);
        uint64_t *items = zcalloc(numItems, sizeof(uint64_t));
        for (size_t i = 0; i < numItems; i++) {
            items[i] = ((uint64_t)rand() << 32) | rand();
            hashFromInt(items[i], hash);
            linearBloomHashSet(bloom, hash);
        }

        /* Verify zero false negatives */
        size_t falseNegatives = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(items[i], hash);
            if (!linearBloomHashCheck(bloom, hash)) {
                falseNegatives++;
            }
        }

        if (falseNegatives > 0) {
            ERR("Large-scale test: %zu false negatives!", falseNegatives);
        }

        /* Verify early-exit check consistency */
        size_t checkMismatches = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(items[i], hash);
            bool regular = linearBloomHashCheck(bloom, hash);
            bool earlyExit = linearBloomHashCheckEarlyExit(bloom, hash);
            if (regular != earlyExit) {
                checkMismatches++;
            }
        }

        if (checkMismatches > 0) {
            ERR("Large-scale test: %zu check mismatches!", checkMismatches);
        }

        /* Count false positives */
        size_t falsePositives = 0;
        for (size_t i = 0; i < numItems; i++) {
            uint64_t notInserted = items[i] ^ 0xFFFFFFFFFFFFFFFFULL;
            hashFromInt(notInserted, hash);
            if (linearBloomHashCheck(bloom, hash)) {
                falsePositives++;
            }
        }

        double fpRate = (double)falsePositives / numItems * 100;
        printf("    1M items: FP rate %.4f%% (%zu/%zu)\n", fpRate,
               falsePositives, numItems);

        zfree(items);
        linearBloomFree(bloom);
    }

    TEST("linearBloomCount: large-scale counting stress test") {
        linearBloomCount *bloom = linearBloomCountNew();
        const size_t numItems = 500000;
        uint64_t hash[2];

        /* Track expected minimum counts */
        uint8_t *expectedMin = zcalloc(numItems, sizeof(uint8_t));

        /* Add items multiple times with varying counts */
        srand(22222);
        for (size_t round = 0; round < 5; round++) {
            for (size_t i = 0; i < numItems; i++) {
                if (rand() % 2 == 0) {
                    hashFromInt(i, hash);
                    linearBloomCountHashSet(bloom, hash);
                    if (expectedMin[i] < 7) {
                        expectedMin[i]++;
                    }
                }
            }
        }

        /* Verify counts are at least expected (collisions may increase) */
        size_t underCount = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            uint_fast32_t actual = linearBloomCountHashCheck(bloom, hash);
            if (actual < expectedMin[i]) {
                underCount++;
            }
        }

        if (underCount > 0) {
            ERR("Large-scale counting: %zu items under expected count!",
                underCount);
        }

        /* Apply half and verify counts decrease appropriately */
        linearBloomCountHalf(bloom);
        for (size_t i = 0; i < numItems; i++) {
            expectedMin[i] /= 2;
        }

        size_t underCountAfterHalf = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            uint_fast32_t actual = linearBloomCountHashCheck(bloom, hash);
            if (actual < expectedMin[i]) {
                underCountAfterHalf++;
            }
        }

        if (underCountAfterHalf > 0) {
            ERR("After half: %zu items under expected count!",
                underCountAfterHalf);
        }

        zfree(expectedMin);
        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: full filter stress test") {
        /* Test behavior when filter is heavily loaded */
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Add many items to stress the filter */
        const size_t numItems = LINEARBLOOMCOUNT_EXTENT_ENTRIES / 2;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Verify no false negatives */
        size_t falseNeg = 0;
        for (size_t i = 0; i < numItems; i++) {
            hashFromInt(i, hash);
            if (linearBloomCountHashCheck(bloom, hash) == 0) {
                falseNeg++;
            }
        }

        if (falseNeg > 0) {
            ERR("Full filter test: %zu false negatives!", falseNeg);
        }

        /* Half and verify structure is intact */
        linearBloomCount *reference = linearBloomCountNew();
        memcpy(reference, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);

        linearBloomCountHalf(bloom);
        linearBloomCountHalfScalar(reference);

        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("Full filter: SWAR half differs from scalar!%s", "");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: half performance extended benchmark (1+ sec)") {
        printf("    === Extended Half Performance Benchmark ===\n");
        uint64_t hash[2];

        /* Fill a bloom filter for realistic conditions */
        linearBloomCount *bloom = linearBloomCountNew();
        for (size_t i = 0; i < 500000; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Benchmark scalar - run enough iterations for 1+ second */
        {
            linearBloomCount *test = linearBloomCountNew();
            memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);

            const size_t numIters = 30;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                linearBloomCountHalfScalar(test);
                /* Refill some data to keep it non-zero */
                if (i % 5 == 4) {
                    memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                }
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters,
                                             "Scalar half (extended)");
            linearBloomCountFree(test);
        }

        /* Benchmark SWAR */
        {
            linearBloomCount *test = linearBloomCountNew();
            memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);

            const size_t numIters = 300;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                linearBloomCountHalf(test);
                if (i % 50 == 49) {
                    memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                }
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "SWAR half (extended)");
            linearBloomCountFree(test);
        }

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        /* Benchmark NEON */
        {
            linearBloomCount *test = linearBloomCountNew();
            memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);

            const size_t numIters = 300;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                linearBloomCountHalfNEON(test);
                if (i % 50 == 49) {
                    memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                }
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "NEON half (extended)");
            linearBloomCountFree(test);
        }
#endif

        printf("    === End Extended Benchmark ===\n");
        linearBloomCountFree(bloom);
    }

    TEST("linearBloom: check variants comprehensive comparison") {
        linearBloom *bloom = linearBloomNew();
        uint64_t hash[2];

        /* Test with various fill levels */
        size_t fillLevels[] = {100, 1000, 10000, 100000, 500000};

        for (size_t f = 0; f < sizeof(fillLevels) / sizeof(fillLevels[0]);
             f++) {
            linearBloomReset(bloom);
            size_t fillLevel = fillLevels[f];

            /* Add items */
            for (size_t i = 0; i < fillLevel; i++) {
                hashFromInt(i * 3, hash);
                linearBloomHashSet(bloom, hash);
            }

            /* Compare check variants on positive lookups */
            size_t posMismatches = 0;
            for (size_t i = 0; i < fillLevel; i++) {
                hashFromInt(i * 3, hash);
                bool regular = linearBloomHashCheck(bloom, hash);
                bool earlyExit = linearBloomHashCheckEarlyExit(bloom, hash);
                if (regular != earlyExit) {
                    posMismatches++;
                }
            }

            /* Compare check variants on negative lookups */
            size_t negMismatches = 0;
            for (size_t i = 0; i < fillLevel; i++) {
                hashFromInt(i * 3 + 1, hash); /* Different items */
                bool regular = linearBloomHashCheck(bloom, hash);
                bool earlyExit = linearBloomHashCheckEarlyExit(bloom, hash);
                if (regular != earlyExit) {
                    negMismatches++;
                }
            }

            if (posMismatches > 0 || negMismatches > 0) {
                ERR("Fill %zu: pos mismatches=%zu, neg mismatches=%zu",
                    fillLevel, posMismatches, negMismatches);
            }
        }

        printf("    All fill levels: check variants consistent\n");
        linearBloomFree(bloom);
    }

    TEST("linearBloomCount: entry alignment edge cases") {
        /* Test entries at specific alignments within 64-bit words */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();

        /* Test entries at word boundaries and various offsets */
        size_t testEntries[] = {
            0,    1,    2,    /* Start of word 0 */
            19,   20,   21,   /* End of word 0, spanning to word 1 */
            22,   23,   24,   /* Start entries in word 1 */
            40,   41,   42,   /* End of word 1, spanning to word 2 */
            43,   44,   45,   /* Start entries in word 2 */
            62,   63,   64,   /* Boundary between groups */
            84,   85,   86,   /* Another boundary (21 + 64) */
            105,  106,  107,  /* Another boundary (42 + 64) */
            1000, 1001, 1002, /* Arbitrary mid-range */
        };

        /* Set each to max value */
        for (size_t i = 0; i < sizeof(testEntries) / sizeof(testEntries[0]);
             i++) {
            size_t entry = testEntries[i];
            if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                varintPacked3Set(bloom, entry, 7);
                varintPacked3Set(reference, entry, 7);
            }
        }

        /* Half both */
        linearBloomCountHalf(bloom);
        linearBloomCountHalfScalar(reference);

        /* Verify all match */
        size_t mismatches = 0;
        for (size_t i = 0; i < sizeof(testEntries) / sizeof(testEntries[0]);
             i++) {
            size_t entry = testEntries[i];
            if (entry < LINEARBLOOMCOUNT_EXTENT_ENTRIES) {
                uint8_t swarVal = varintPacked3Get(bloom, entry);
                uint8_t scalarVal = varintPacked3Get(reference, entry);
                if (swarVal != scalarVal) {
                    printf("    Entry %zu: SWAR=%u, Scalar=%u\n", entry,
                           swarVal, scalarVal);
                    mismatches++;
                }
                /* Value should be 3 (7/2 = 3) */
                if (scalarVal != 3) {
                    ERR("Entry %zu: expected 3, got %u", entry, scalarVal);
                }
            }
        }

        if (mismatches > 0) {
            ERR("Entry alignment test had %zu mismatches!", mismatches);
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: remaining entries after last complete group") {
        /* Test that entries in the incomplete final group are handled correctly
         */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();

        /* Calculate where remaining entries start */
        size_t numGroups = LINEARBLOOMCOUNT_EXTENT_ENTRIES / 64;
        size_t remaining = LINEARBLOOMCOUNT_EXTENT_ENTRIES % 64;
        size_t startOfRemaining = numGroups * 64;

        printf("    Total entries: %d, Groups: %zu, Remaining: %zu\n",
               LINEARBLOOMCOUNT_EXTENT_ENTRIES, numGroups, remaining);

        if (remaining > 0) {
            /* Set all remaining entries to max value */
            for (size_t i = 0; i < remaining; i++) {
                size_t entry = startOfRemaining + i;
                varintPacked3Set(bloom, entry, 7);
                varintPacked3Set(reference, entry, 7);
            }

            /* Half both */
            linearBloomCountHalf(bloom);
            linearBloomCountHalfScalar(reference);

            /* Verify remaining entries */
            size_t mismatches = 0;
            for (size_t i = 0; i < remaining; i++) {
                size_t entry = startOfRemaining + i;
                uint8_t swarVal = varintPacked3Get(bloom, entry);
                uint8_t scalarVal = varintPacked3Get(reference, entry);
                if (swarVal != scalarVal) {
                    mismatches++;
                }
            }

            if (mismatches > 0) {
                ERR("Remaining entries had %zu mismatches!", mismatches);
            }
        } else {
            printf(
                "    No remaining entries (entries evenly divisible by 64)\n");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    /* ================================================================
     * Exponential Decay Tests
     * ================================================================ */

    TEST("linearBloomCount: decay factor computation") {
        /* Verify decay factor calculation */
        double factor;

        /* At t=0, factor should be 1.0 */
        factor = linearBloomCountComputeDecayFactor(0, 1000);
        if (fabs(factor - 1.0) > 0.0001) {
            ERR("At t=0, expected factor 1.0, got %f", factor);
        }

        /* At t=half_life, factor should be 0.5 */
        factor = linearBloomCountComputeDecayFactor(1000, 1000);
        if (fabs(factor - 0.5) > 0.0001) {
            ERR("At t=half_life, expected factor 0.5, got %f", factor);
        }

        /* At t=2*half_life, factor should be 0.25 */
        factor = linearBloomCountComputeDecayFactor(2000, 1000);
        if (fabs(factor - 0.25) > 0.0001) {
            ERR("At t=2*half_life, expected factor 0.25, got %f", factor);
        }

        /* At t=half_life/2, factor should be ~0.707 */
        factor = linearBloomCountComputeDecayFactor(500, 1000);
        if (fabs(factor - 0.7071) > 0.001) {
            ERR("At t=half_life/2, expected factor ~0.707, got %f", factor);
        }

        printf("    Decay factor computation verified\n");
    }

    TEST("linearBloomCount: decay by factor edge cases") {
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Add some items */
        for (size_t i = 0; i < 1000; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Factor >= 1.0 should do nothing */
        linearBloomCount *copy = linearBloomCountNew();
        memcpy(copy, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
        linearBloomCountDecayByFactor(bloom, 1.0, 0);
        if (memcmp(bloom, copy, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("Factor 1.0 should not change values%s", "");
        }

        linearBloomCountDecayByFactor(bloom, 1.5, 0);
        if (memcmp(bloom, copy, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("Factor > 1.0 should not change values%s", "");
        }

        /* Factor <= 0.0 should reset to zero */
        linearBloomCountDecayByFactor(bloom, 0.0, 0);
        hashFromInt(0, hash);
        if (linearBloomCountHashCheck(bloom, hash) != 0) {
            ERR("Factor 0.0 should reset all values to zero%s", "");
        }

        /* Verify completely zeroed */
        size_t nonZero = 0;
        for (size_t i = 0; i < 10000; i++) {
            if (varintPacked3Get(bloom, i) > 0) {
                nonZero++;
            }
        }
        if (nonZero > 0) {
            ERR("After factor 0.0, found %zu non-zero entries", nonZero);
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(copy);
    }

    TEST("linearBloomCount: decay factor 0.5 uses optimized half") {
        /* Verify that factor=0.5 produces same results as linearBloomCountHalf
         */
        linearBloomCount *bloom1 = linearBloomCountNew();
        linearBloomCount *bloom2 = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill both with same data */
        for (size_t i = 0; i < 50000; i++) {
            hashFromInt(i * 7, hash);
            linearBloomCountHashSet(bloom1, hash);
            linearBloomCountHashSet(bloom2, hash);
        }

        /* Apply half to one, factor 0.5 to other */
        linearBloomCountHalf(bloom1);
        linearBloomCountDecayByFactor(bloom2, 0.5, 0);

        /* They should match exactly (factor=0.5 delegates to Half) */
        if (memcmp(bloom1, bloom2, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("Factor 0.5 should produce same result as Half%s", "");
        }

        linearBloomCountFree(bloom1);
        linearBloomCountFree(bloom2);
    }

    TEST("linearBloomCount: probabilistic decay statistical accuracy") {
        /* Verify probabilistic rounding maintains statistical accuracy.
         * Test the RNG and rounding directly rather than full bloom decay. */
        linearBloomCountRNG rng;
        const size_t numTrials = 100000;
        double totalResult = 0;

        /* Test: value 6 with factor 0.75 -> expected 4.5 */
        for (size_t trial = 0; trial < numTrials; trial++) {
            linearBloomCountRNGInit(&rng, trial + 1);
            double decayed = 6.0 * 0.75; /* 4.5 */
            uint8_t rounded = linearBloomCountProbRound(decayed, &rng);
            totalResult += rounded;
        }

        double avgResult = totalResult / numTrials;
        double expected = 4.5;

        /* Should be within 2% of expected */
        if (fabs(avgResult - expected) > expected * 0.02) {
            ERR("Probabilistic rounding: expected avg ~%.2f, got %.2f",
                expected, avgResult);
        }

        printf("    Probabilistic rounding: expected %.2f, got %.4f (%.2f%% "
               "error)\n",
               expected, avgResult,
               fabs(avgResult - expected) / expected * 100);

        /* Test another value: 5 * 0.7 = 3.5 */
        totalResult = 0;
        for (size_t trial = 0; trial < numTrials; trial++) {
            linearBloomCountRNGInit(&rng, trial + 1000000);
            uint8_t rounded = linearBloomCountProbRound(3.5, &rng);
            totalResult += rounded;
        }

        avgResult = totalResult / numTrials;
        expected = 3.5;

        if (fabs(avgResult - expected) > expected * 0.02) {
            ERR("Probabilistic rounding (3.5): expected avg ~%.2f, got %.2f",
                expected, avgResult);
        }

        printf("    Probabilistic rounding (3.5): expected %.2f, got %.4f\n",
               expected, avgResult);
    }

    TEST("linearBloomCount: deterministic vs probabilistic decay") {
        /* Compare deterministic (floor) vs probabilistic decay behavior */
        linearBloomCount *det = linearBloomCountNew();
        linearBloomCount *prob = linearBloomCountNew();

        /* Set many entries to value 5 */
        for (size_t i = 0; i < 10000; i++) {
            varintPacked3Set(det, i, 5);
            varintPacked3Set(prob, i, 5);
        }

        /* Apply 0.7 decay: 5 * 0.7 = 3.5 */
        linearBloomCountDecayByFactorDeterministic(det, 0.7);
        linearBloomCountDecayByFactor(prob, 0.7, 12345);

        /* Deterministic should all be 3 (floor of 3.5) */
        size_t detSum = 0;
        for (size_t i = 0; i < 10000; i++) {
            detSum += varintPacked3Get(det, i);
        }
        if (detSum != 30000) { /* 10000 * 3 */
            ERR("Deterministic decay: expected sum 30000, got %zu", detSum);
        }

        /* Probabilistic should average ~3.5 per entry, so sum ~35000 */
        size_t probSum = 0;
        for (size_t i = 0; i < 10000; i++) {
            probSum += varintPacked3Get(prob, i);
        }
        double probAvg = (double)probSum / 10000;
        if (fabs(probAvg - 3.5) > 0.1) {
            ERR("Probabilistic decay: expected avg ~3.5, got %.2f", probAvg);
        }

        printf("    Deterministic sum: %zu (avg 3.0), Probabilistic sum: %zu "
               "(avg %.2f)\n",
               detSum, probSum, probAvg);

        linearBloomCountFree(det);
        linearBloomCountFree(prob);
    }

    TEST("linearBloomCount: time-based decay correctness") {
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Add items with count 4 */
        for (size_t i = 0; i < 1000; i++) {
            hashFromInt(i, hash);
            for (int j = 0; j < 4; j++) {
                linearBloomCountHashSet(bloom, hash);
            }
        }

        /* Verify initial count */
        hashFromInt(0, hash);
        uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
        if (count < 4) {
            ERR("Expected initial count >= 4, got %" PRIuFAST32, count);
        }

        /* Apply decay: 1 hour elapsed, 1 hour half-life -> factor 0.5 */
        linearBloomCountDecay(bloom, 3600000, 3600000, 0);

        /* Count should be halved (4 -> 2) */
        count = linearBloomCountHashCheck(bloom, hash);
        if (count != 2) {
            ERR("After 1 half-life, expected count 2, got %" PRIuFAST32, count);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: time-based decay various intervals") {
        /* Test decay at various time intervals */
        struct {
            uint64_t elapsed_ms;
            uint64_t half_life_ms;
            const char *desc;
            double expected_factor;
        } tests[] = {
            {0, 1000, "0 elapsed", 1.0},
            {1000, 1000, "1 half-life", 0.5},
            {2000, 1000, "2 half-lives", 0.25},
            {500, 1000, "0.5 half-life", 0.7071},
            {100, 1000, "0.1 half-life", 0.9330},
            {5000, 1000, "5 half-lives", 0.03125},
            {60000, 3600000, "1 min / 1 hr half-life", 0.9885},
            {1800000, 3600000, "30 min / 1 hr half-life",
             0.7071}, /* 0.5 half-lives */
            {3600000, 3600000, "60 min / 1 hr half-life",
             0.5}, /* 1 half-life */
        };

        for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
            double factor = linearBloomCountComputeDecayFactor(
                tests[t].elapsed_ms, tests[t].half_life_ms);
            if (fabs(factor - tests[t].expected_factor) > 0.01) {
                ERR("Test '%s': expected factor %.4f, got %.4f", tests[t].desc,
                    tests[t].expected_factor, factor);
            }
        }

        printf("    All time-based decay intervals verified\n");
    }

    TEST("linearBloomCount: decay with real-world scenario") {
        /* Simulate a real-world rate limiting scenario:
         * - Half-life of 10 minutes (600000 ms)
         * - Events happen, then time passes, then we check
         */
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* User makes 6 requests */
        hashFromString("user123", 7, hash);
        for (int i = 0; i < 6; i++) {
            linearBloomCountHashSet(bloom, hash);
        }

        /* Check count */
        uint_fast32_t count = linearBloomCountHashCheck(bloom, hash);
        printf("    Initial count: %" PRIuFAST32 "\n", count);

        /* 5 minutes pass (half of half-life) */
        linearBloomCountDecay(bloom, 300000, 600000, 0);
        count = linearBloomCountHashCheck(bloom, hash);
        printf("    After 5 min (factor ~0.71): %" PRIuFAST32 "\n", count);

        /* Another 5 minutes pass (total: 1 half-life) */
        linearBloomCountDecay(bloom, 300000, 600000, 0);
        count = linearBloomCountHashCheck(bloom, hash);
        printf("    After 10 min (1 half-life): %" PRIuFAST32 "\n", count);

        /* 20 more minutes pass (total: 3 half-lives) */
        linearBloomCountDecay(bloom, 1200000, 600000, 0);
        count = linearBloomCountHashCheck(bloom, hash);
        printf("    After 30 min (3 half-lives): %" PRIuFAST32 "\n", count);

        /* Count should be very low (6 * 0.125 = 0.75 -> 0 or 1) */
        if (count > 2) {
            ERR("After 3 half-lives, expected count ~0-1, got %" PRIuFAST32,
                count);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: decay preserves non-decayed entries") {
        /* Verify that zero entries stay zero after decay */
        linearBloomCount *bloom = linearBloomCountNew();

        /* Set only even entries */
        for (size_t i = 0; i < 10000; i += 2) {
            varintPacked3Set(bloom, i, 4);
        }

        /* Apply decay */
        linearBloomCountDecayByFactor(bloom, 0.6, 0);

        /* Odd entries should still be zero */
        size_t oddNonZero = 0;
        for (size_t i = 1; i < 10000; i += 2) {
            if (varintPacked3Get(bloom, i) > 0) {
                oddNonZero++;
            }
        }

        if (oddNonZero > 0) {
            ERR("Decay created %zu non-zero odd entries!", oddNonZero);
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: decay performance benchmark (1+ sec)") {
        printf("    === Decay Performance Benchmark ===\n");
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill bloom filter */
        for (size_t i = 0; i < 500000; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Benchmark deterministic decay */
        {
            linearBloomCount *test = linearBloomCountNew();

            const size_t numIters = 25;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountDecayByFactorDeterministic(test, 0.75);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters,
                                             "Deterministic decay (0.75)");
            linearBloomCountFree(test);
        }

        /* Benchmark probabilistic decay */
        {
            linearBloomCount *test = linearBloomCountNew();

            const size_t numIters = 20;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountDecayByFactor(test, 0.75, i + 1);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters,
                                             "Probabilistic decay (0.75)");
            linearBloomCountFree(test);
        }

        /* Benchmark half (for comparison) */
        {
            linearBloomCount *test = linearBloomCountNew();

            const size_t numIters = 300;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountHalf(test);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "Half (SWAR optimized)");
            linearBloomCountFree(test);
        }

        printf("    === End Decay Benchmark ===\n");
        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: decay reproducibility with same seed") {
        /* Verify that same seed produces same results */
        linearBloomCount *bloom1 = linearBloomCountNew();
        linearBloomCount *bloom2 = linearBloomCountNew();

        /* Set identical data */
        for (size_t i = 0; i < 10000; i++) {
            varintPacked3Set(bloom1, i, 5);
            varintPacked3Set(bloom2, i, 5);
        }

        /* Apply decay with same seed */
        linearBloomCountDecayByFactor(bloom1, 0.6, 42);
        linearBloomCountDecayByFactor(bloom2, 0.6, 42);

        /* Should be identical */
        if (memcmp(bloom1, bloom2, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("Same seed should produce identical results%s", "");
        }

        linearBloomCountFree(bloom1);
        linearBloomCountFree(bloom2);
    }

    TEST("linearBloomCount: decay with different seeds differs") {
        /* Verify that different seeds produce different results */
        linearBloomCount *bloom1 = linearBloomCountNew();
        linearBloomCount *bloom2 = linearBloomCountNew();

        /* Set identical data - use value 5 with factor 0.7 = 3.5 (fractional)
         */
        for (size_t i = 0; i < 10000; i++) {
            varintPacked3Set(bloom1, i, 5);
            varintPacked3Set(bloom2, i, 5);
        }

        /* Apply decay with different seeds - 5 * 0.7 = 3.5 requires rounding */
        linearBloomCountDecayByFactor(bloom1, 0.7, 42);
        linearBloomCountDecayByFactor(bloom2, 0.7, 99);

        /* Should differ (with high probability for 10000 entries with 50% round
         * chance) */
        if (memcmp(bloom1, bloom2, LINEARBLOOMCOUNT_EXTENT_BYTES) == 0) {
            ERR("Different seeds should produce different results%s", "");
        }

        /* But sums should be similar (statistical equivalence) */
        size_t sum1 = 0, sum2 = 0;
        for (size_t i = 0; i < 10000; i++) {
            sum1 += varintPacked3Get(bloom1, i);
            sum2 += varintPacked3Get(bloom2, i);
        }

        /* Both should be around 35000 (10000 * 5 * 0.7 = 35000) */
        if (abs((int)sum1 - (int)sum2) > 1000) {
            ERR("Different seeds had very different sums: %zu vs %zu", sum1,
                sum2);
        }

        printf("    Different seeds: sum1=%zu, sum2=%zu (both ~35000)\n", sum1,
               sum2);

        linearBloomCountFree(bloom1);
        linearBloomCountFree(bloom2);
    }

    /* ================================================================
     * SWAR-Optimized Quarter Tests
     * ================================================================ */

    TEST("linearBloomCount: quarter (0.25) SWAR correctness") {
        /* Verify SWAR quarter matches scalar reference */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill with data */
        for (size_t i = 0; i < 50000; i++) {
            hashFromInt(i * 7, hash);
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
            /* Add more to get higher counts */
            linearBloomCountHashSet(bloom, hash);
            linearBloomCountHashSet(reference, hash);
        }

        /* Apply SWAR quarter to bloom, scalar to reference */
        linearBloomCountQuarter(bloom);
        linearBloomCountQuarterScalar(reference);

        /* Verify they match byte-by-byte */
        if (memcmp(bloom, reference, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("SWAR quarter differs from scalar reference!%s", "");
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: quarter value correctness") {
        /* Verify quarter produces correct values: v -> v/4 (floor) */
        linearBloomCount *bloom = linearBloomCountNew();

        /* Set entries to specific values */
        varintPacked3Set(bloom, 0, 7); /* 7/4 = 1 */
        varintPacked3Set(bloom, 1, 6); /* 6/4 = 1 */
        varintPacked3Set(bloom, 2, 5); /* 5/4 = 1 */
        varintPacked3Set(bloom, 3, 4); /* 4/4 = 1 */
        varintPacked3Set(bloom, 4, 3); /* 3/4 = 0 */
        varintPacked3Set(bloom, 5, 2); /* 2/4 = 0 */
        varintPacked3Set(bloom, 6, 1); /* 1/4 = 0 */
        varintPacked3Set(bloom, 7, 0); /* 0/4 = 0 */

        linearBloomCountQuarter(bloom);

        uint8_t expected[] = {1, 1, 1, 1, 0, 0, 0, 0};
        for (size_t i = 0; i < 8; i++) {
            uint8_t val = varintPacked3Get(bloom, i);
            if (val != expected[i]) {
                ERR("Entry %zu: expected %u after quarter, got %u", i,
                    expected[i], val);
            }
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: quarter boundary entries") {
        /* Test quarter on boundary-spanning entries (21, 42, etc.) */
        linearBloomCount *bloom = linearBloomCountNew();
        linearBloomCount *reference = linearBloomCountNew();

        /* Set boundary entries to max value */
        for (size_t g = 0; g < 100; g++) {
            varintPacked3Set(bloom, g * 64 + 21, 7);
            varintPacked3Set(bloom, g * 64 + 42, 7);
            varintPacked3Set(reference, g * 64 + 21, 7);
            varintPacked3Set(reference, g * 64 + 42, 7);
        }

        linearBloomCountQuarter(bloom);
        linearBloomCountQuarterScalar(reference);

        /* Verify boundary entries match */
        size_t mismatches = 0;
        for (size_t g = 0; g < 100; g++) {
            if (varintPacked3Get(bloom, g * 64 + 21) !=
                varintPacked3Get(reference, g * 64 + 21)) {
                mismatches++;
            }
            if (varintPacked3Get(bloom, g * 64 + 42) !=
                varintPacked3Get(reference, g * 64 + 42)) {
                mismatches++;
            }
        }

        if (mismatches > 0) {
            ERR("Quarter boundary entries had %zu mismatches!", mismatches);
        }

        /* Verify values are correct (7/4 = 1) */
        for (size_t g = 0; g < 100; g++) {
            if (varintPacked3Get(bloom, g * 64 + 21) != 1) {
                ERR("Entry 21 at group %zu: expected 1, got %u", g,
                    varintPacked3Get(bloom, g * 64 + 21));
            }
        }

        linearBloomCountFree(bloom);
        linearBloomCountFree(reference);
    }

    TEST("linearBloomCount: decay auto-detects power-of-2 factors") {
        /* Verify decay with factor 0.25 uses optimized quarter */
        linearBloomCount *bloom1 = linearBloomCountNew();
        linearBloomCount *bloom2 = linearBloomCountNew();

        /* Fill both identically */
        for (size_t i = 0; i < 10000; i++) {
            varintPacked3Set(bloom1, i, 6);
            varintPacked3Set(bloom2, i, 6);
        }

        /* One via DecayByFactor(0.25), other via Quarter directly */
        linearBloomCountDecayByFactor(bloom1, 0.25, 0);
        linearBloomCountQuarter(bloom2);

        /* Should be identical (DecayByFactor delegates to Quarter) */
        if (memcmp(bloom1, bloom2, LINEARBLOOMCOUNT_EXTENT_BYTES) != 0) {
            ERR("DecayByFactor(0.25) should produce same result as Quarter%s",
                "");
        }

        linearBloomCountFree(bloom1);
        linearBloomCountFree(bloom2);
    }

    TEST("linearBloomCount: LUT-based decay correctness") {
        /* Verify LUT-based decay matches float-based for same factor */
        linearBloomCount *bloom = linearBloomCountNew();

        /* Test with various values */
        for (size_t i = 0; i < 8; i++) {
            varintPacked3Set(bloom, i, i); /* Values 0-7 */
        }

        /* Build LUT for 0.6 factor */
        uint8_t lut[8];
        linearBloomCountBuildDecayLUT(lut, 0.6);

        /* Verify LUT values: floor(v * 0.6) */
        uint8_t expected_lut[] = {0, 0, 1, 1, 2, 3, 3, 4};
        for (int v = 0; v < 8; v++) {
            if (lut[v] != expected_lut[v]) {
                ERR("LUT[%d]: expected %u, got %u", v, expected_lut[v], lut[v]);
            }
        }

        linearBloomCountDecayByLUT(bloom, lut);

        /* Verify values match expected */
        for (size_t i = 0; i < 8; i++) {
            uint8_t val = varintPacked3Get(bloom, i);
            if (val != expected_lut[i]) {
                ERR("After LUT decay, entry %zu: expected %u, got %u", i,
                    expected_lut[i], val);
            }
        }

        linearBloomCountFree(bloom);
    }

    TEST("linearBloomCount: quarter vs half vs decay performance") {
        printf("    === SWAR Decay Performance Comparison ===\n");
        linearBloomCount *bloom = linearBloomCountNew();
        uint64_t hash[2];

        /* Fill bloom filter */
        for (size_t i = 0; i < 500000; i++) {
            hashFromInt(i, hash);
            linearBloomCountHashSet(bloom, hash);
        }

        /* Benchmark Half */
        {
            linearBloomCount *test = linearBloomCountNew();
            const size_t numIters = 300;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountHalf(test);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "Half (SWAR, 0.5)");
            linearBloomCountFree(test);
        }

        /* Benchmark Quarter */
        {
            linearBloomCount *test = linearBloomCountNew();
            const size_t numIters = 300;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountQuarter(test);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "Quarter (SWAR, 0.25)");
            linearBloomCountFree(test);
        }

        /* Benchmark LUT-based (0.75) */
        {
            linearBloomCount *test = linearBloomCountNew();
            uint8_t lut[8];
            linearBloomCountBuildDecayLUT(lut, 0.75);

            const size_t numIters = 30;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountDecayByLUT(test, lut);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters, "LUT-based (0.75)");
            linearBloomCountFree(test);
        }

        /* Benchmark Scalar Quarter (for comparison) */
        {
            linearBloomCount *test = linearBloomCountNew();
            const size_t numIters = 20;
            PERF_TIMERS_SETUP;

            for (size_t i = 0; i < numIters; i++) {
                memcpy(test, bloom, LINEARBLOOMCOUNT_EXTENT_BYTES);
                linearBloomCountQuarterScalar(test);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(numIters,
                                             "Quarter Scalar (reference)");
            linearBloomCountFree(test);
        }

        printf("    === End Performance Comparison ===\n");
        linearBloomCountFree(bloom);
    }

    TEST_FINAL_RESULT;
}
#endif
