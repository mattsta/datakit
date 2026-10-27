#include "intset.h"
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "intsetCommon.h"
#include "intsetFull.h"
#include "intsetMedium.h"
#include "intsetSmall.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(int64_t *, zfree, I64);
defcleanerName(int16_t *, zfree, I16);
defcleaner(intset *, intsetFree);

/* Differential oracle for INTSET_BRANCHLESS_LOWER_BOUND: a branchy
 * reference lower-bound search and the branchless macro MUST agree on both the
 * found flag and the index, for every probe. Arrays are sorted ascending and
 * duplicate-free (the intset invariant), so the branchy hit index equals the
 * lower bound. Returns 0 on agreement, 1 on mismatch. */
#define DEFINE_BLB_ORACLE(NAME, T)                                             \
    static int NAME(const T *arr, uint64_t n, T v) {                           \
        uint64_t blo = 0;                                                      \
        uint64_t bhi = n;                                                      \
        bool bfound = false;                                                   \
        uint64_t bidx = 0;                                                     \
        while (blo < bhi) {                                                    \
            const uint64_t mid = blo + (bhi - blo) / 2;                        \
            if (arr[mid] < v) {                                                \
                blo = mid + 1;                                                 \
            } else if (arr[mid] > v) {                                         \
                bhi = mid;                                                     \
            } else {                                                           \
                bfound = true;                                                 \
                bidx = mid;                                                    \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        const uint64_t refIdx = bfound ? bidx : blo;                           \
        uint64_t lidx = 0;                                                     \
        bool lhit = false;                                                     \
        if (n > 0) {                                                           \
            INTSET_BRANCHLESS_LOWER_BOUND(arr, n, v, lidx, lhit);              \
        }                                                                      \
        return (lhit == bfound && lidx == refIdx) ? 0 : 1;                     \
    }
DEFINE_BLB_ORACLE(blbOracle16, int16_t)
DEFINE_BLB_ORACLE(blbOracle32, int32_t)
DEFINE_BLB_ORACLE(blbOracle64, int64_t)

/* Run tests */
int intsetTest(int, char *[]) {
    int err = 0;

    TEST("basic small tier operations") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        intsetAdd(&is, 10, &success);
        if (!success) {
            ERR("Failed to add 10");
        }
        intsetAdd(&is, 5, &success);
        if (!success) {
            ERR("Failed to add 5");
        }
        intsetAdd(&is, 15, &success);
        if (!success) {
            ERR("Failed to add 15");
        }
        intsetAdd(&is, 10, &success); /* Duplicate */
        if (success) {
            ERR("Wrongly added duplicate 10");
        }

        if (intsetCount(is) != 3) {
            ERR("Count should be 3, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 10)) {
            ERR("Should find 10");
        }
        if (!intsetFind(is, 5)) {
            ERR("Should find 5");
        }
        if (!intsetFind(is, 15)) {
            ERR("Should find 15");
        }
        if (intsetFind(is, 20)) {
            ERR("Should not find 20");
        }

        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 5) {
            ERR("Position 0 should be 5");
        }
        if (!intsetGet(is, 1, &val) || val != 10) {
            ERR("Position 1 should be 10");
        }
        if (!intsetGet(is, 2, &val) || val != 15) {
            ERR("Position 2 should be 15");
        }

        intsetRepr(is);
    }

    TEST("small to medium tier upgrade") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add int16 values */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Verify still small */
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERR("Should still be SMALL tier");
        }

        /* Add int32 value to trigger upgrade */
        intsetAdd(&is, 100'000, &success);
        if (!success) {
            ERR("Failed to add 100000");
        }

        /* Should now be medium */
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM tier after adding int32 value");
        }
        if (intsetCount(is) != 101) {
            ERR("Count should be 101, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 50)) {
            ERR("Should find 50");
        }
        if (!intsetFind(is, 100'000)) {
            ERR("Should find 100000");
        }

        intsetRepr(is);
    }

    TEST("medium to full tier upgrade") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Force to medium */
        intsetAdd(&is, 100, nullptr);
        intsetAdd(&is, 100'000, nullptr);
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM tier");
        }

        /* Add int64 value to trigger upgrade */
        intsetAdd(&is, INT64_MAX, &success);
        if (!success) {
            ERR("Failed to add INT64_MAX");
        }

        /* Should now be full */
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should be FULL tier after adding int64 value");
        }
        if (intsetCount(is) != 3) {
            ERR("Count should be 3, got %zu", intsetCount(is));
        }
        if (!intsetFind(is, 100)) {
            ERR("Should find 100");
        }
        if (!intsetFind(is, 100'000)) {
            ERR("Should find 100000");
        }
        if (!intsetFind(is, INT64_MAX)) {
            ERR("Should find INT64_MAX");
        }

        intsetRepr(is);
    }

    TEST("stats: tier, width arrays, members and bytes in every tier") {
        cleaner(intsetFree) intset *is = intsetNew();
        const int64_t members[] = {7, -3, 100, 100'000, -70000, INT64_MAX};
        /* After each prefix: tier, then 16/32/64-bit member counts */
        const uint8_t tier[] = {1, 1, 1, 2, 2, 3};
        const uint64_t n16[] = {1, 2, 3, 3, 3, 3};
        const uint64_t n32[] = {0, 0, 0, 1, 2, 2};
        const uint64_t n64[] = {0, 0, 0, 0, 0, 1};
        const uint32_t ceiling[] = {INTSET_SMALL_MAX_BYTES,
                                    INTSET_MEDIUM_MAX_BYTES, 0};
        for (size_t i = 0; i < countof(members); i++) {
            intsetAdd(&is, members[i], nullptr);
            dkTierStats s;
            intsetStats(is, &s);
            const uint64_t data = n16[i] * 2 + n32[i] * 4 + n64[i] * 8;
            if (s.tier != tier[i] || s.units != tier[i] || s.count != i + 1 ||
                s.dataBytes != data || s.totalBytes != intsetBytes(is) ||
                s.totalBytes <= data || s.unitLimit != ceiling[tier[i] - 1]) {
                ERR("after %zu members: tier %u units %u count %" PRIu64
                    " data %" PRIu64 " total %" PRIu64 " limit %u",
                    i + 1, s.tier, s.units, s.count, s.dataBytes, s.totalBytes,
                    s.unitLimit);
            }
        }
        dkTierStats none;
        intsetStats(nullptr, &none);
        if (none.tier || none.count || none.totalBytes) {
            ERR("NULL intset reports a tier, members or bytes");
        }
    }

    TEST("Copy in every tier: same members and tier, independent of the "
         "original") {
        const int64_t reach[] = {7, 100'000, INT64_MAX};
        for (size_t t = 0; t < countof(reach); t++) {
            cleaner(intsetFree) intset *is = intsetNew();
            for (int64_t v = -40; v < 40; v += 3) {
                intsetAdd(&is, v, nullptr);
            }
            for (size_t i = 0; i <= t; i++) {
                intsetAdd(&is, reach[i], nullptr);
            }
            cleaner(intsetFree) intset *copy = intsetCopy(is);
            dkTierStats a;
            dkTierStats b;
            intsetStats(is, &a);
            intsetStats(copy, &b);
            bool same = a.tier == b.tier && a.count == b.count &&
                        a.totalBytes == b.totalBytes;
            intsetIterator ia;
            intsetIterator ib;
            intsetIteratorInit(&ia, is);
            intsetIteratorInit(&ib, copy);
            int64_t va;
            int64_t vb;
            while (intsetIteratorNext(&ia, &va)) {
                same &= intsetIteratorNext(&ib, &vb) && va == vb;
            }
            same &= !intsetIteratorNext(&ib, &vb);
            if (!same) {
                ERR("tier %zu: the copy differs", t + 1);
            }

            /* Changing the copy leaves the original alone */
            const size_t before = intsetCount(is);
            intsetAdd(&copy, -30000, nullptr);
            intsetRemove(&copy, reach[t], nullptr);
            if (intsetCount(is) != before || intsetFind(is, -30000) ||
                !intsetFind(is, reach[t])) {
                ERR("tier %zu: changing the copy changed the original", t + 1);
            }
        }
    }

    TEST("Compact moves to the narrowest tier the values allow") {
        cleaner(intsetFree) intset *is = intsetNew();
        for (int64_t v = -300; v < 300; v += 7) {
            intsetAdd(&is, v, nullptr);
        }
        const int64_t wide[] = {100'000, -70000, INT64_MAX, INT64_MIN + 1};
        for (size_t i = 0; i < countof(wide); i++) {
            intsetAdd(&is, wide[i], nullptr);
        }

        /* A 64-bit value left: Full stays Full */
        intsetRemove(&is, INT64_MAX, nullptr);
        dkTierStats s;
        intsetCompact(&is);
        intsetStats(is, &s);
        if (s.tier != 3) {
            ERR("a Full still holding a 64-bit value compacted to tier %u",
                s.tier);
        }

        /* Removal step, then the tier Compact must reach: no 64-bit values
         * (Medium), then no 32-bit values (Small) */
        const int64_t drop[] = {INT64_MIN + 1, 100'000, -70000};
        const uint8_t want[] = {2, 2, 1};
        for (size_t i = 0; i < countof(drop); i++) {
            intsetRemove(&is, drop[i], nullptr);
            dkTierStats was;
            intsetStats(is, &was);
            cleaner(intsetFree) intset *before = intsetCopy(is);
            intsetCompact(&is);
            intsetStats(is, &s);
            bool same = s.count == was.count;
            intsetIterator ia;
            intsetIterator ib;
            intsetIteratorInit(&ia, before);
            intsetIteratorInit(&ib, is);
            int64_t va;
            int64_t vb;
            while (intsetIteratorNext(&ia, &va)) {
                same &= intsetIteratorNext(&ib, &vb) && va == vb;
            }
            same &= !intsetIteratorNext(&ib, &vb);
            printf("  after removing %" PRId64 ": tier %u, %" PRIu64
                   " bytes -> tier %u, %" PRIu64 "\n",
                   drop[i], was.tier, was.totalBytes, s.tier, s.totalBytes);
            if (!same || s.tier != want[i] || s.totalBytes > was.totalBytes) {
                ERR("after removing %" PRId64 ": tier %u (want %u), members "
                    "%s",
                    drop[i], s.tier, want[i], same ? "same" : "changed");
            }
        }

        /* A wide value grows it back as usual */
        intsetAdd(&is, INT64_MAX, nullptr);
        intsetStats(is, &s);
        if (s.tier != 3 || !intsetFind(is, INT64_MAX) || !intsetFind(is, -6)) {
            ERR("a 64-bit add after Compact did not grow the set back");
        }
    }

    TEST("Reset from every tier gives an empty Small that takes new members") {
        /* A 16-bit, then a 32-bit, then a 64-bit member reach each tier */
        const int64_t reach[] = {7, 100'000, INT64_MAX};
        for (size_t t = 0; t < countof(reach); t++) {
            cleaner(intsetFree) intset *is = intsetNew();
            for (size_t i = 0; i <= t; i++) {
                intsetAdd(&is, reach[i], nullptr);
            }
            intsetReset(&is);
            dkTierStats s;
            intsetStats(is, &s);
            if (s.tier != 1 || intsetCount(is) != 0 || intsetFind(is, 7)) {
                ERR("tier %zu: reset to tier %u with %zu members", t + 1,
                    s.tier, intsetCount(is));
            }
            intsetAdd(&is, -2, nullptr);
            if (intsetCount(is) != 1 || !intsetFind(is, -2)) {
                ERR("tier %zu: a member after Reset is missing", t + 1);
            }
        }
    }

    TEST("memory efficiency - tiered vs monolithic") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add 1000 int16 values */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, nullptr);
        }

        size_t bytesBeforeLarge = intsetBytes(is);
        printf("  Before large value: %zu bytes for 1000 elements\n",
               bytesBeforeLarge);

        /* Add large value */
        intsetAdd(&is, INT64_MAX - 1000, nullptr);
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
    }

    TEST("remove operations") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        for (int64_t i = 0; i < 10; i++) {
            intsetAdd(&is, i, nullptr);
        }

        intsetRemove(&is, 5, &success);
        if (!success) {
            ERR("Failed to remove 5");
        }
        if (intsetCount(is) != 9) {
            ERR("Count should be 9 after removal, got %zu", intsetCount(is));
        }
        if (intsetFind(is, 5)) {
            ERR("Should not find removed value 5");
        }
        if (!intsetFind(is, 4)) {
            ERR("Should still find 4");
        }
        if (!intsetFind(is, 6)) {
            ERR("Should still find 6");
        }

        intsetRemove(&is, 5, &success); /* Already removed */
        if (success) {
            ERR("Should not succeed removing already-removed value");
        }
        if (intsetCount(is) != 9) {
            ERR("Count should still be 9, got %zu", intsetCount(is));
        }

        intsetRepr(is);
    }

    TEST("large dataset stress test") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add 10,000 values across all three width categories */
        for (int64_t i = 0; i < 3000; i++) {
            intsetAdd(&is, i, nullptr); /* int16 */
        }
        for (int64_t i = 40000; i < 42000; i++) {
            intsetAdd(&is, i, nullptr); /* int32 */
        }
        for (int64_t i = INT64_MAX - 1000; i < INT64_MAX; i++) {
            intsetAdd(&is, i, nullptr); /* int64 */
        }

        size_t totalCount = intsetCount(is);
        if (totalCount != 6000) {
            ERR("Should have 6000 elements, got %zu", totalCount);
        }

        /* Verify a few values */
        if (!intsetFind(is, 1500)) {
            ERR("Should find 1500");
        }
        if (!intsetFind(is, 41000)) {
            ERR("Should find 41000");
        }
        if (!intsetFind(is, INT64_MAX - 500)) {
            ERR("Should find INT64_MAX-500");
        }

        size_t bytes = intsetBytes(is);
        size_t monolithicBytes = 6000 * 8; /* All int64 in old system */
        printf("  Tiered: %zu bytes, Monolithic: %zu bytes, Savings: %.1f%%\n",
               bytes, monolithicBytes,
               100.0 * (1.0 - (double)bytes / monolithicBytes));
    }

    /* ===== EDGE CASES ===== */

    TEST("empty set operations") {
        cleaner(intsetFree) intset *is = intsetNew();

        if (intsetCount(is) != 0) {
            ERR("Empty set should have count 0");
        }
        if (intsetBytes(is) < sizeof(intsetSmall)) {
            ERR("Empty set should have some overhead bytes");
        }
        if (intsetFind(is, 0)) {
            ERR("Empty set should not find any value");
        }

        int64_t val;
        if (intsetGet(is, 0, &val)) {
            ERR("Empty set should not have position 0");
        }

        bool success;
        intsetRemove(&is, 5, &success);
        if (success) {
            ERR("Cannot remove from empty set");
        }

        intsetRepr(is);
    }

    TEST("single element operations") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add single element */
        intsetAdd(&is, 42, &success);
        if (!success) {
            ERR("Should add first element");
        }
        if (intsetCount(is) != 1) {
            ERR("Count should be 1");
        }
        if (!intsetFind(is, 42)) {
            ERR("Should find 42");
        }

        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 42) {
            ERR("Position 0 should be 42");
        }

        /* Remove the only element */
        intsetRemove(&is, 42, &success);
        if (!success) {
            ERR("Should remove element");
        }
        if (intsetCount(is) != 0) {
            ERR("Count should be 0 after removal");
        }
        if (intsetFind(is, 42)) {
            ERR("Should not find removed element");
        }
    }

    TEST("boundary value tests") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Test int16 boundaries */
        intsetAdd(&is, INT16_MIN, &success);
        if (!success) {
            ERR("Should add INT16_MIN");
        }
        intsetAdd(&is, INT16_MAX, &success);
        if (!success) {
            ERR("Should add INT16_MAX");
        }

        /* Test int32 boundaries (triggers upgrade) */
        intsetAdd(&is, INT32_MIN, &success);
        if (!success) {
            ERR("Should add INT32_MIN");
        }
        intsetAdd(&is, INT32_MAX, &success);
        if (!success) {
            ERR("Should add INT32_MAX");
        }

        /* Test int64 boundaries (triggers upgrade) */
        intsetAdd(&is, INT64_MIN, &success);
        if (!success) {
            ERR("Should add INT64_MIN");
        }
        intsetAdd(&is, INT64_MAX, &success);
        if (!success) {
            ERR("Should add INT64_MAX");
        }

        if (intsetCount(is) != 6) {
            ERR("Should have 6 boundary values, got %zu", intsetCount(is));
        }
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should be FULL tier");
        }

        /* Verify all boundaries are found */
        if (!intsetFind(is, INT16_MIN)) {
            ERR("Should find INT16_MIN");
        }
        if (!intsetFind(is, INT16_MAX)) {
            ERR("Should find INT16_MAX");
        }
        if (!intsetFind(is, INT32_MIN)) {
            ERR("Should find INT32_MIN");
        }
        if (!intsetFind(is, INT32_MAX)) {
            ERR("Should find INT32_MAX");
        }
        if (!intsetFind(is, INT64_MIN)) {
            ERR("Should find INT64_MIN");
        }
        if (!intsetFind(is, INT64_MAX)) {
            ERR("Should find INT64_MAX");
        }

        intsetRepr(is);
    }

    TEST("negative values handling") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add mix of negative and positive values */
        for (int64_t i = -100; i <= 100; i++) {
            intsetAdd(&is, i, nullptr);
        }

        if (intsetCount(is) != 201) {
            ERR("Should have 201 values, got %zu", intsetCount(is));
        }

        /* Verify sorted order (negative to positive) */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != -100) {
            ERR("First should be -100");
        }
        if (!intsetGet(is, 100, &val) || val != 0) {
            ERR("Middle should be 0");
        }
        if (!intsetGet(is, 200, &val) || val != 100) {
            ERR("Last should be 100");
        }
    }

    /* ===== SEQUENTIAL INSERTION TESTS ===== */

    TEST("ascending sequential insertion") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add 1000 values in ascending order */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, nullptr);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify all values are present and sorted */
        for (int64_t i = 0; i < 1000; i++) {
            if (!intsetFind(is, i)) {
                ERR("Should find value %" PRId64, i);
                break;
            }
        }

        /* Verify sorted access */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 0) {
            ERR("First should be 0");
        }
        if (!intsetGet(is, 500, &val) || val != 500) {
            ERR("Middle should be 500");
        }
        if (!intsetGet(is, 999, &val) || val != 999) {
            ERR("Last should be 999");
        }
    }

    TEST("descending sequential insertion") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add 1000 values in descending order (worst case for insertion) */
        for (int64_t i = 999; i >= 0; i--) {
            intsetAdd(&is, i, nullptr);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify sorted order is maintained */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 0) {
            ERR("First should be 0");
        }
        if (!intsetGet(is, 999, &val) || val != 999) {
            ERR("Last should be 999");
        }
    }

    /* ===== ADVERSARIAL PATTERNS ===== */

    TEST("adversarial: alternating extremes") {
        cleaner(intsetFree) intset *is = intsetNew();
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
            ERR("Should upgrade to FULL");
        }

        /* Verify count */
        size_t count = intsetCount(is);
        if (count == 0) {
            ERR("Should have elements");
        }
        printf("  Added %zu values with alternating extremes\n", count);
    }

    TEST("adversarial: worst-case tier transitions") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Fill small tier to near capacity */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, nullptr);
        }
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERR("Should still be SMALL");
        }

        /* Add one int32 to force upgrade */
        intsetAdd(&is, 100'000, nullptr);
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should upgrade to MEDIUM");
        }

        /* Fill medium tier with int32 values */
        for (int64_t i = 100001; i < 100100; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Add one int64 to force upgrade */
        intsetAdd(&is, INT64_MAX, nullptr);
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should upgrade to FULL");
        }

        if (intsetCount(is) != 201) {
            ERR("Should have 201 values, got %zu", intsetCount(is));
        }

        intsetRepr(is);
    }

    TEST("adversarial: insert at beginning pattern") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Always insert at beginning (forces maximum shifts) */
        for (int64_t i = 1000; i > 0; i--) {
            intsetAdd(&is, i, nullptr);
        }

        if (intsetCount(is) != 1000) {
            ERR("Should have 1000 values, got %zu", intsetCount(is));
        }

        /* Verify smallest is first */
        int64_t val;
        if (!intsetGet(is, 0, &val) || val != 1) {
            ERR("First should be 1");
        }
    }

    TEST("adversarial: duplicate spam") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add value */
        intsetAdd(&is, 42, &success);
        if (!success) {
            ERR("Should add first 42");
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
    }

    /* ===== TIER BOUNDARY TESTS ===== */

    TEST("tier boundary: small to medium threshold") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add values up to just before int16 can't represent */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Should still be small */
        if (INTSET_TYPE(is) != INTSET_TYPE_SMALL) {
            ERR("Should be SMALL with int16 values");
        }

        size_t bytesBeforeUpgrade = intsetBytes(is);

        /* Add first int32 value */
        intsetAdd(&is, INT16_MAX + 1, nullptr);

        /* Should now be medium */
        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM after int32 value");
        }

        size_t bytesAfterUpgrade = intsetBytes(is);
        printf("  Upgrade overhead: %zu bytes (%zu -> %zu)\n",
               bytesAfterUpgrade - bytesBeforeUpgrade, bytesBeforeUpgrade,
               bytesAfterUpgrade);

        /* All values should still be findable */
        if (!intsetFind(is, 500)) {
            ERR("Should still find old int16 values");
        }
        if (!intsetFind(is, INT16_MAX + 1)) {
            ERR("Should find new int32 value");
        }
    }

    TEST("tier boundary: medium to full threshold") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add mixed int16/int32 values */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, nullptr); /* int16 */
        }
        for (int64_t i = 40000; i < 40100; i++) {
            intsetAdd(&is, i, nullptr); /* int32 */
        }

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM");
        }

        /* Add first int64 value */
        intsetAdd(&is, INT32_MAX + 1LL, nullptr);

        /* Should now be full */
        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should be FULL after int64 value");
        }

        /* Verify all widths are preserved */
        if (!intsetFind(is, 50)) {
            ERR("Should find int16 value");
        }
        if (!intsetFind(is, 40050)) {
            ERR("Should find int32 value");
        }
        if (!intsetFind(is, INT32_MAX + 1LL)) {
            ERR("Should find int64 value");
        }

        intsetRepr(is);
    }

    /* ===== RANDOM/FUZZING TESTS ===== */

    TEST("random insertions with verification") {
        cleaner(intsetFree) intset *is = intsetNew();
        const int NUM_VALUES = 5000;
        cleanerName(zfree, I64) int64_t *oracle =
            zcalloc(NUM_VALUES, sizeof(int64_t));

        /* Generate random values across all width categories */
        for (int i = 0; i < NUM_VALUES; i++) {
            int category = rand() % 3;
            if (category == 0) {
                /* int16 range */
                oracle[i] = (rand() % (INT16_MAX - INT16_MIN + 1)) + INT16_MIN;
            } else if (category == 1) {
                /* int32 range (outside int16) */
                oracle[i] = (rand() % 1'000'000) + INT16_MAX + 1;
            } else {
                /* int64 range (large values) */
                oracle[i] = INT64_MAX - (rand() % 10000);
            }
        }

        /* Insert all values */
        for (int i = 0; i < NUM_VALUES; i++) {
            intsetAdd(&is, oracle[i], nullptr);
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
    }

    TEST("fuzzing: random add/remove operations") {
        cleaner(intsetFree) intset *is = intsetNew();
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
                int64_t value = rand() % 100'000;
                intsetAdd(&is, value, &success);
                if (success) {
                    addCount++;
                    expectedCount++;
                }
            } else {
                /* 30% chance: remove random value */
                int64_t value = rand() % 100'000;
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
                    ERR("Count mismatch at op %d: expected %" PRId64
                        ", got %zu",
                        i, expectedCount, actualCount);
                    break;
                }
            }
        }

        printf("  Operations: %d adds (%d succeeded), %d removes\n",
               NUM_OPS * 70 / 100, addCount, removeCount);
        printf("  Final count: %zu (expected %" PRId64 ")\n", intsetCount(is),
               expectedCount);

        if ((int64_t)intsetCount(is) != expectedCount) {
            ERR("Final count mismatch: expected %" PRId64 ", got %zu",
                expectedCount, intsetCount(is));
        }

        intsetRepr(is);
    }

    /* ===== COMPREHENSIVE API TESTS ===== */

    TEST("intsetRandom coverage") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Empty set */
        int64_t r = intsetRandom(is);
        /* Random from empty is undefined but shouldn't crash */

        /* Single element */
        intsetAdd(&is, 42, nullptr);
        r = intsetRandom(is);
        if (r != 42) {
            ERR("Random from single-element set should return that element");
        }

        /* Multiple elements */
        for (int64_t i = 0; i < 100; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Get 100 random values and verify they're all in range */
        bool foundDifferent = false;
        int64_t first = intsetRandom(is);
        for (int i = 0; i < 100; i++) {
            r = intsetRandom(is);
            if (r < 0 || r >= 100) {
                ERR("Random value %" PRId64 " out of range [0,100)", r);
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
    }

    TEST("intsetGet out of bounds") {
        cleaner(intsetFree) intset *is = intsetNew();
        int64_t val;

        /* Empty set */
        if (intsetGet(is, 0, &val)) {
            ERR("Get from empty set should fail");
        }
        if (intsetGet(is, 100, &val)) {
            ERR("Get beyond bounds should fail");
        }

        /* Add 10 elements */
        for (int64_t i = 0; i < 10; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Valid positions */
        if (!intsetGet(is, 0, &val)) {
            ERR("Should get position 0");
        }
        if (!intsetGet(is, 9, &val)) {
            ERR("Should get position 9");
        }

        /* Invalid positions */
        if (intsetGet(is, 10, &val)) {
            ERR("Should not get position 10");
        }
        if (intsetGet(is, 1000, &val)) {
            ERR("Should not get position 1000");
        }
    }

    TEST("NULL parameter handling") {
        cleaner(intsetFree) intset *is = intsetNew();
        bool success;

        /* Add with NULL success pointer */
        intsetAdd(&is, 42, nullptr);
        if (!intsetFind(is, 42)) {
            ERR("Should add even with NULL success");
        }

        /* Remove with NULL success pointer */
        intsetRemove(&is, 42, nullptr);
        if (intsetFind(is, 42)) {
            ERR("Should remove even with NULL success");
        }

        /* Get with NULL value pointer */
        intsetAdd(&is, 100, nullptr);
        if (!intsetGet(is, 0, nullptr)) {
            ERR("Should succeed with NULL value ptr");
        }

        /* Operations on NULL intset */
        cleaner(intsetFree) intset *null_is = nullptr;
        if (intsetCount(null_is) != 0) {
            ERR("NULL intset should have count 0");
        }
        if (intsetFind(null_is, 42)) {
            ERR("NULL intset should not find anything");
        }

        intsetAdd(&null_is, 42, &success);
        if (!success) {
            ERR("Should create new intset from NULL");
        }
        if (intsetCount(null_is) != 1) {
            ERR("Should have 1 element");
        }
    }

    TEST("stress: many removes from large set") {
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add 1000 values */
        for (int64_t i = 0; i < 1000; i++) {
            intsetAdd(&is, i, nullptr);
        }

        /* Remove every other value */
        for (int64_t i = 0; i < 1000; i += 2) {
            bool success;
            intsetRemove(&is, i, &success);
            if (!success) {
                ERR("Failed to remove %" PRId64, i);
                break;
            }
        }

        if (intsetCount(is) != 500) {
            ERR("Should have 500 values left, got %zu", intsetCount(is));
        }

        /* Verify odd values remain */
        for (int64_t i = 1; i < 1000; i += 2) {
            if (!intsetFind(is, i)) {
                ERR("Should find odd value %" PRId64, i);
                break;
            }
        }

        /* Verify even values are gone */
        for (int64_t i = 0; i < 1000; i += 2) {
            if (intsetFind(is, i)) {
                ERR("Should not find removed even value %" PRId64, i);
                break;
            }
        }
    }

    /* ===== CROSS-TIER VIRTUAL MERGE ITERATION TESTS ===== */

    TEST("MEDIUM: virtual merge iteration with interleaved values") {
        /* Test that intsetGet returns values in sorted order when
         * int16 and int32 values are interleaved in the sorted sequence.
         * This exercises the virtual merge algorithm in intsetMediumGet. */
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add int16 values: 10, 30, 50, 70, 90 */
        intsetAdd(&is, 10, nullptr);
        intsetAdd(&is, 30, nullptr);
        intsetAdd(&is, 50, nullptr);
        intsetAdd(&is, 70, nullptr);
        intsetAdd(&is, 90, nullptr);

        /* Add int32 values that interleave: 20000, 40000, 60000, 80000 */
        intsetAdd(&is, 20000, nullptr);
        intsetAdd(&is, 40000, nullptr);
        intsetAdd(&is, 60000, nullptr);
        intsetAdd(&is, 80000, nullptr);

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM tier");
        }

        /* Expected sorted order: 10, 30, 50, 70, 90, 20000, 40000, 60000, 80000
         */
        int64_t expected[] = {10, 30, 50, 70, 90, 20000, 40000, 60000, 80000};
        size_t count = countof(expected);

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
    }

    TEST("MEDIUM: virtual merge with densely interleaved widths") {
        /* Test with values that truly interleave across width boundaries.
         * int16 max is 32767, so values like 32760, 32765, 32770, 32775
         * cross the int16/int32 boundary. */
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add values that cross the int16/int32 boundary */
        int64_t values[] = {32760, 32765, 32770, 32775, 32780, 32785};
        size_t count = countof(values);

        for (size_t i = 0; i < count; i++) {
            intsetAdd(&is, values[i], nullptr);
        }

        if (INTSET_TYPE(is) != INTSET_TYPE_MEDIUM) {
            ERR("Should be MEDIUM tier (has int32 values)");
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
    }

    TEST("FULL: virtual 3-way merge iteration") {
        /* Test that intsetGet returns values in sorted order when
         * int16, int32, and int64 values are all present.
         * This exercises the virtual 3-way merge algorithm in intsetFullGet. */
        cleaner(intsetFree) intset *is = intsetNew();

        /* Add values across all three width categories */
        /* int16 values */
        intsetAdd(&is, 100, nullptr);
        intsetAdd(&is, 200, nullptr);
        intsetAdd(&is, 300, nullptr);

        /* int32 values */
        intsetAdd(&is, 100'000, nullptr);
        intsetAdd(&is, 200'000, nullptr);
        intsetAdd(&is, 300'000, nullptr);

        /* int64 values - trigger upgrade to FULL */
        intsetAdd(&is, INT32_MAX + 1000LL, nullptr);
        intsetAdd(&is, INT32_MAX + 2000LL, nullptr);
        intsetAdd(&is, INT32_MAX + 3000LL, nullptr);

        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should be FULL tier");
        }

        /* Expected sorted order */
        int64_t expected[] = {100,
                              200,
                              300,
                              100'000,
                              200'000,
                              300'000,
                              INT32_MAX + 1000LL,
                              INT32_MAX + 2000LL,
                              INT32_MAX + 3000LL};
        size_t count = countof(expected);

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
    }

    TEST("FULL: 3-way merge with interleaved values across all widths") {
        /* Create a scenario where values from all three widths
         * interleave in the final sorted order. */
        cleaner(intsetFree) intset *is = intsetNew();

        /* Create interleaved pattern by using negative numbers too */
        /* int16: -1000, -100, 0, 100, 1000 */
        intsetAdd(&is, -1000, nullptr);
        intsetAdd(&is, -100, nullptr);
        intsetAdd(&is, 0, nullptr);
        intsetAdd(&is, 100, nullptr);
        intsetAdd(&is, 1000, nullptr);

        /* int32: -50000, 50000 */
        intsetAdd(&is, -50000, nullptr);
        intsetAdd(&is, 50000, nullptr);

        /* int64: values outside int32 range */
        intsetAdd(&is, INT32_MIN - 1LL, nullptr);
        intsetAdd(&is, INT32_MAX + 1LL, nullptr);

        if (INTSET_TYPE(is) != INTSET_TYPE_FULL) {
            ERR("Should be FULL tier");
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
    }

    TEST("virtual merge: randomized cross-tier iteration") {
        /* Generate random values across all width categories and verify
         * that intsetGet always returns sorted order. */
        cleaner(intsetFree) intset *is = intsetNew();
        const int NUM_VALUES = 200;
        cleanerName(zfree, I64) int64_t *oracle =
            zcalloc(NUM_VALUES, sizeof(int64_t));

        /* Generate values across all three width categories */
        uint64_t rng = 0x1A75E7; /* replayable full-width int64 draws */
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
            case 2: /* int64 range, both extremes reachable */
                val = (int64_t)dkSplitmix64Next(&rng);
                break;
            default:
                val = rand();
            }
            oracle[i] = val;
            intsetAdd(&is, val, nullptr);
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
    }

    TEST(
        "intset branchless lower-bound == branchy (oracle, all widths/edges)") {
        static int32_t a32[4096];
        const int sizes[] = {0,   1,    2,    3,    4,    5,   7,   8,
                             16,  17,   31,   32,   63,   64,  255, 256,
                             257, 1023, 1024, 1025, 4095, 4096};
        for (size_t si = 0; si < countof(sizes); si++) {
            const int n = sizes[si];
            for (int i = 0; i < n; i++) {
                a32[i] = (int32_t)(i * 2 - n); /* sorted, unique, spans 0 */
            }
            for (int i = 0; i < n; i++) { /* every present value */
                if (blbOracle32(a32, (uint64_t)n, a32[i])) {
                    ERR("blb32 present mismatch n=%d i=%d", n, i);
                }
            }
            for (int i = 0; i <= n; i++) { /* gaps + below + above */
                const int32_t gap = (i < n)
                                        ? (int32_t)(a32[i] - 1)
                                        : (n ? (int32_t)(a32[n - 1] + 1) : 0);
                if (blbOracle32(a32, (uint64_t)n, gap)) {
                    ERR("blb32 gap mismatch n=%d i=%d", n, i);
                }
            }
            if (blbOracle32(a32, (uint64_t)n, INT32_MIN) ||
                blbOracle32(a32, (uint64_t)n, INT32_MAX)) {
                ERR("blb32 extreme mismatch n=%d", n);
            }
        }

        /* int16 + int64 width extremes */
        static int16_t a16[400];
        for (int i = 0; i < 400; i++) {
            a16[i] = (int16_t)(i * 2 - 400);
        }
        for (int i = 0; i < 400; i++) {
            if (blbOracle16(a16, 400, a16[i])) {
                ERR("blb16 present mismatch i=%d", i);
            }
        }
        if (blbOracle16(a16, 400, INT16_MIN) ||
            blbOracle16(a16, 400, INT16_MAX) ||
            blbOracle16(a16, 400, (int16_t)(a16[0] - 1)) ||
            blbOracle16(a16, 400, (int16_t)(a16[399] + 1))) {
            ERR("blb16 edge mismatch");
        }
        static int64_t a64[400];
        for (int i = 0; i < 400; i++) {
            a64[i] = (int64_t)i * 1'000'000'000LL - 200000000000LL;
        }
        for (int i = 0; i < 400; i++) {
            if (blbOracle64(a64, 400, a64[i])) {
                ERR("blb64 present mismatch i=%d", i);
            }
        }
        if (blbOracle64(a64, 400, INT64_MIN) ||
            blbOracle64(a64, 400, INT64_MAX)) {
            ERR("blb64 extreme mismatch");
        }

        /* Random sorted-unique arrays + arbitrary probes. */
        uint64_t seed = 0x1234567ULL;
        for (int iter = 0; iter < 4000; iter++) {
            dkLcg64Next(&seed);
            int n = (int)(seed % 600);
            int32_t prev = INT32_MIN;
            int built = 0;
            for (int i = 0; i < n; i++) {
                dkLcg64Next(&seed);
                const int32_t step = (int32_t)(seed % 9) + 1;
                if (prev > INT32_MAX - step) {
                    break;
                }
                prev += step;
                a32[built++] = prev;
            }
            dkLcg64Next(&seed);
            const int32_t probe = (int32_t)(uint32_t)(seed >> 16);
            if (blbOracle32(a32, (uint64_t)built, probe)) {
                ERR("blb32 random mismatch n=%d probe=%d", built, probe);
            }
        }
    }

    TEST("Full iterator: real INT64_MAX must not collide with sentinel") {
        /* Exhausted arrays use an INT64_MAX sentinel in the 3-way
         * merge; a set actually containing INT64_MAX must still
         * terminate and yield each member exactly once */
        intsetFull *f = intsetFullNew();
        bool added;
        f = intsetFullAdd(f, 5, &added);
        f = intsetFullAdd(f, INT64_MAX, &added);
        f = intsetFullAdd(f, -9000000000LL, &added);

        intsetFullIterator it;
        intsetFullIteratorInit(&it, f);

        int64_t got[8];
        int yields = 0;
        int64_t v;
        while (yields < 8 && intsetFullIteratorNext(&it, &v)) {
            got[yields++] = v;
        }

        if (yields != 3) {
            ERR("Expected exactly 3 yields, got %d (infinite-loop guard)",
                yields);
        } else if (got[0] != -9000000000LL || got[1] != 5 ||
                   got[2] != INT64_MAX) {
            ERR("Merge order wrong with INT64_MAX present");
        }

        intsetFullFree(f);
    }

    TEST("Min/Max: O(1) accessors across tiers and sign boundaries") {
        cleaner(intsetFree) intset *is = intsetNew();
        int64_t v;
        bool ok;

        if (intsetMin(is, &v) || intsetMax(is, &v)) {
            ERR("Min/Max must report false on empty set");
        }

        /* Small tier */
        intsetAdd(&is, 100, &ok);
        intsetAdd(&is, -50, &ok);
        if (!intsetMin(is, &v) || v != -50 || !intsetMax(is, &v) || v != 100) {
            ERR("Small-tier Min/Max wrong");
        }

        /* Force Medium (int32 range), negative wide value is the min */
        intsetAdd(&is, -100'000, &ok);
        intsetAdd(&is, 70000, &ok);
        if (!intsetMin(is, &v) || v != -100'000 || !intsetMax(is, &v) ||
            v != 70000) {
            ERR("Medium-tier Min/Max wrong (negative wide value)");
        }

        /* Force Full with the extreme boundaries */
        intsetAdd(&is, INT64_MIN, &ok);
        intsetAdd(&is, INT64_MAX, &ok);
        if (!intsetMin(is, &v) || v != INT64_MIN || !intsetMax(is, &v) ||
            v != INT64_MAX) {
            ERR("Full-tier Min/Max wrong at INT64 boundaries");
        }
    }

    TEST("iterator and Get agree with a sorted oracle in every tier") {
        /* Values of every width and sign; the iterator yields exactly the
         * sorted distinct values and Get(i) is the i-th of them */
        static constexpr int64_t scale[] = {1, 100'000, INT64_C(10000000000)};
        for (size_t tier = 0; tier < 3; tier++) {
            cleaner(intsetFree) intset *is = intsetNew();
            cleanerName(zfree, I64) int64_t *want =
                zcalloc(600, sizeof(int64_t));
            size_t n = 0;
            uint64_t rng = 0x1d5e7 + tier;
            for (int i = 0; i < 600; i++) {
                dkLcg64Next(&rng);
                const size_t width = (rng >> 33) % (tier + 1);
                const int64_t v =
                    ((int64_t)((rng >> 40) % 4000) - 2000) * scale[width];
                bool added;
                intsetAdd(&is, v, &added);
                if (added) {
                    size_t at = n++;
                    while (at > 0 && want[at - 1] > v) {
                        want[at] = want[at - 1];
                        at--;
                    }
                    want[at] = v;
                }
            }

            intsetIterator it;
            intsetIteratorInit(&it, is);
            int64_t got;
            size_t seen = 0;
            while (intsetIteratorNext(&it, &got)) {
                if (seen >= n || got != want[seen]) {
                    ERR("tier %zu: iterator value %zu wrong", tier, seen);
                    break;
                }
                seen++;
            }
            if (seen != n || intsetCount(is) != n) {
                ERR("tier %zu: iterated %zu of %zu", tier, seen, n);
            }
            for (size_t i = 0; i < n; i++) {
                if (!intsetGet(is, (uint32_t)i, &got) || got != want[i]) {
                    ERR("tier %zu: Get(%zu) wrong", tier, i);
                    break;
                }
            }
            if (intsetGet(is, (uint32_t)n, &got)) {
                ERR("tier %zu: Get past the end succeeded", tier);
            }

            /* The tier's own Get and iterator agree with the oracle too */
            const void *raw = INTSET_UNTAG(is);
            size_t tierSeen = 0;
            switch (INTSET_TYPE(is)) {
            case INTSET_TYPE_SMALL: {
                const intsetSmall *s = raw;
                intsetSmallIterator sit;
                intsetSmallIteratorInit(&sit, s);
                while (intsetSmallIteratorNext(&sit, &got) && tierSeen < n &&
                       got == want[tierSeen]) {
                    tierSeen++;
                }
                for (size_t i = 0; i < n; i++) {
                    if (!intsetSmallGet(s, (uint32_t)i, &got) ||
                        got != want[i]) {
                        tierSeen = 0;
                        break;
                    }
                }

                /* FromArray rebuilds the same tier from the sorted values */
                cleanerName(zfree, I16) int16_t *raw16 =
                    zcalloc(n ? n : 1, sizeof(int16_t));
                for (size_t i = 0; i < n; i++) {
                    raw16[i] = (int16_t)want[i];
                }
                intsetSmall *rebuilt = intsetSmallFromArray(raw16, (uint32_t)n);
                for (size_t i = 0; i < n; i++) {
                    if (!intsetSmallGet(rebuilt, (uint32_t)i, &got) ||
                        got != want[i]) {
                        ERR("tier %zu: FromArray value %zu wrong", tier, i);
                        break;
                    }
                }
                intsetSmallFree(rebuilt);
                break;
            }
            case INTSET_TYPE_MEDIUM: {
                const intsetMedium *m = raw;
                intsetMediumIterator mit;
                intsetMediumIteratorInit(&mit, m);
                while (intsetMediumIteratorNext(&mit, &got) && tierSeen < n &&
                       got == want[tierSeen]) {
                    tierSeen++;
                }
                for (size_t i = 0; i < n; i++) {
                    if (!intsetMediumGet(m, i, &got) || got != want[i]) {
                        tierSeen = 0;
                        break;
                    }
                }
                break;
            }
            case INTSET_TYPE_FULL: {
                const intsetFull *f = raw;
                tierSeen = n;
                for (size_t i = 0; i < n; i++) {
                    if (!intsetFullGet(f, i, &got) || got != want[i]) {
                        tierSeen = 0;
                        break;
                    }
                }
                break;
            }
            }
            if (tierSeen != n) {
                ERR("tier %zu (type %d): the tier's Get or iterator disagree",
                    tier, (int)INTSET_TYPE(is));
            }
        }
    }

    TEST_FINAL_RESULT;
}
