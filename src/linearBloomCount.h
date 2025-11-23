#pragma once

#include "datakit.h"

typedef uint64_t linearBloomCount;

/* These defaults (m = 2.8M slots (~1 MB for 3 bit values), k = 13 hashes)
 * are approximately equivalent to a 1 in 10,000 false positive rate
 * for storing 150,000 items */

#ifndef LINEARBLOOMCOUNT_HASHES
#define LINEARBLOOMCOUNT_HASHES 13
#endif

#ifndef LINEARBLOOMCOUNT_EXTENT_ENTRIES
#define LINEARBLOOMCOUNT_EXTENT_ENTRIES (2875518)
#endif

#define LINEARBLOOMCOUNT_KIRSCHMITZENMACHER(iteration, hash1, hash2)           \
    ((hash1) + (iteration) * (hash2))

/* ====================================================================
 * Packed Bit Management
 * ==================================================================== */
#define PACKED_STATIC DK_INLINE_ALWAYS
#define LINEAR_BLOOM_BITS 3
#define PACK_STORAGE_BITS LINEAR_BLOOM_BITS
#define PACK_MAX_ELEMENTS LINEARBLOOMCOUNT_EXTENT_ENTRIES
#define PACK_STORAGE_SLOT_STORAGE_TYPE linearBloomCount
#define PACK_STORAGE_MICRO_PROMOTION_TYPE linearBloomCount
#define PACK_FUNCTION_PREFIX varintPacked
#include "../deps/varint/src/varintPacked.h"

/* Define divCeil BEFORE using it */
#define divCeil(a, b) (((a) + (b)-1) / (b))

/* See comment for this in linearBloom.h */
#define LINEARBLOOMCOUNT_EXTENT_BYTES                                          \
    (divCeil(divCeil(LINEARBLOOMCOUNT_EXTENT_ENTRIES * LINEAR_BLOOM_BITS, 8),  \
             sizeof(linearBloomCount)) *                                       \
     sizeof(linearBloomCount))
DK_INLINE_ALWAYS linearBloomCount *linearBloomCountNew(void) {
    /* We need to use 'divCeil' here because regular division would give us
     * a floor and that could break some very end-of-array math if our
     * bit length isn't evenly divisible by 8 */
    return zcalloc(1, LINEARBLOOMCOUNT_EXTENT_BYTES);
}

DK_INLINE_ALWAYS void linearBloomCountFree(linearBloomCount *bloom) {
    if (bloom) {
        zfree(bloom);
    }
}

DK_INLINE_ALWAYS void
linearBloomCountReset(linearBloomCount *restrict const bloom) {
    memset(bloom, 0, LINEARBLOOMCOUNT_EXTENT_BYTES);
}

DK_INLINE_ALWAYS void
linearBloomCountHashSet(linearBloomCount *restrict const bloom,
                        uint64_t hash[2]) {
    struct {
        uint_fast32_t bit;
        uint_fast32_t value;
    } bitPositions[LINEARBLOOMCOUNT_HASHES];

    uint_fast32_t minimumValue = UINT_FAST32_MAX;

    /* O(2N) Steps:
     *      - Read all positions
     *      - Only increment minimum value slots */

    /* O(N) */
    for (uint32_t i = 0; i < LINEARBLOOMCOUNT_HASHES; i++) {
        const uint64_t setBit =
            LINEARBLOOMCOUNT_KIRSCHMITZENMACHER(i, hash[0], hash[1]) %
            LINEARBLOOMCOUNT_EXTENT_ENTRIES;

        /* Read position */
        bitPositions[i].bit = setBit;
        bitPositions[i].value = varintPacked3Get(bloom, setBit);

        /* If position has new minimum value, set new minimum */
        if (bitPositions[i].value < minimumValue) {
            minimumValue = bitPositions[i].value;
        }
    }

    /* O(N) */
    for (uint32_t i = 0; i < LINEARBLOOMCOUNT_HASHES; i++) {
        /* If position is minimum value, increment */
        if (bitPositions[i].value == minimumValue) {
            varintPacked3SetIncr(bloom, bitPositions[i].bit, 1);
        }
    }
}

DK_INLINE_ALWAYS uint_fast32_t linearBloomCountHashCheck(
    const linearBloomCount *restrict const bloom, uint64_t hash[2]) {
    uint_fast32_t minimumValue = UINT_FAST32_MAX;
    for (uint32_t i = 0; i < LINEARBLOOMCOUNT_HASHES; i++) {
        const uint64_t checkBit =
            LINEARBLOOMCOUNT_KIRSCHMITZENMACHER(i, hash[0], hash[1]) %
            LINEARBLOOMCOUNT_EXTENT_ENTRIES;

        const uint64_t value = varintPacked3Get(bloom, checkBit);
        if (value < minimumValue) {
            minimumValue = value;
        }
    }

    return minimumValue;
}

DK_INLINE_ALWAYS void
linearBloomCountHalf(linearBloomCount *restrict const bloom) {
    for (uint64_t i = 0; i < LINEARBLOOMCOUNT_EXTENT_ENTRIES; i++) {
        varintPacked3SetHalf(bloom, i);
    }
}
