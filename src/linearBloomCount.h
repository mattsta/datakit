#pragma once

#include "datakit.h"
#include <math.h>

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
#define divCeil(a, b) (((a) + (b) - 1) / (b))

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

/* SWAR (SIMD Within A Register) optimization for halving 3-bit packed values.
 *
 * 3-bit values are packed continuously across 64-bit words. In each group of
 * 3 words (192 bits = 64 values), entries 21 and 42 span word boundaries and
 * cannot be correctly halved with word-level SWAR alone.
 *
 * Approach: Save boundary values, SWAR all complete entries, restore boundary
 * values.
 */

/* Number of 64-bit words in the bloom filter */
#define LBC_NUM_WORDS (LINEARBLOOMCOUNT_EXTENT_BYTES / sizeof(linearBloomCount))

/* KEEP masks clear bit 0 of each complete entry AND all partial bits.
 * Type 0 (w%3==0): entries 0-20 at bits 0-62, partial entry 21 at bit 63
 * Type 1 (w%3==1): partial 21 at bits 0-1, entries 22-41 at bits 2-61, partial
 * 42 at bits 62-63 Type 2 (w%3==2): partial 42 at bit 0, entries 43-63 at bits
 * 1-63 */
#define LBC_KEEP0                                                              \
    0x6DB6DB6DB6DB6DB6ULL /* clear bits 0,3,6,...,60,63 (entries 0-21 LSBs) */
#define LBC_KEEP1                                                              \
    0x36DB6DB6DB6DB6D8ULL /* clear bits 0,1,2,5,8,...,59,62,63 (entries 22-41  \
                             LSBs + partials) */
#define LBC_KEEP2                                                              \
    0xDB6DB6DB6DB6DB6DULL /* clear bits 1,4,7,...,61 (entries 43-63 LSBs) */

/* Reference scalar implementation - always correct */
DK_INLINE_ALWAYS void
linearBloomCountHalfScalar(linearBloomCount *restrict const bloom) {
    for (uint64_t i = 0; i < LINEARBLOOMCOUNT_EXTENT_ENTRIES; i++) {
        varintPacked3SetHalf(bloom, i);
    }
}

DK_INLINE_ALWAYS void
linearBloomCountHalf(linearBloomCount *restrict const bloom) {
    /* Hybrid SWAR approach:
     * 1. Save boundary values (entries 21, 42, 85, 106, ... that span word
     * boundaries)
     * 2. SWAR-half only COMPLETE groups (3 words = 64 entries each)
     * 3. Restore correctly-halved boundary values
     * 4. Handle remaining entries with scalar
     *
     * Boundary entries: 21+64k and 42+64k for k = 0, 1, 2, ... */
    const size_t numGroups = LINEARBLOOMCOUNT_EXTENT_ENTRIES / 64;
    const size_t numBoundary = numGroups * 2;
    const size_t numCompleteWords =
        numGroups * 3; /* Only process complete groups */

    /* Stack allocation for boundary values - about 90KB, fits in stack */
    uint8_t boundaryValues[90000];
    if (numBoundary > sizeof(boundaryValues)) {
        /* Fallback to scalar if too many boundary values */
        linearBloomCountHalfScalar(bloom);
        return;
    }

    /* Step 1: Save boundary values */
    for (size_t g = 0; g < numGroups; g++) {
        boundaryValues[g * 2] = varintPacked3Get(bloom, g * 64 + 21);
        boundaryValues[g * 2 + 1] = varintPacked3Get(bloom, g * 64 + 42);
    }

    /* Step 2: SWAR-half only complete groups (don't touch partial group at end)
     */
    for (size_t w = 0; w + 3 <= numCompleteWords; w += 3) {
        bloom[w] = (bloom[w] & LBC_KEEP0) >> 1;
        bloom[w + 1] = (bloom[w + 1] & LBC_KEEP1) >> 1;
        bloom[w + 2] = (bloom[w + 2] & LBC_KEEP2) >> 1;
    }

    /* Step 3: Restore boundary values (halved) */
    for (size_t g = 0; g < numGroups; g++) {
        varintPacked3Set(bloom, g * 64 + 21, boundaryValues[g * 2] / 2);
        varintPacked3Set(bloom, g * 64 + 42, boundaryValues[g * 2 + 1] / 2);
    }

    /* Step 4: Handle remaining entries after last complete group with scalar */
    size_t remaining = LINEARBLOOMCOUNT_EXTENT_ENTRIES % 64;
    if (remaining > 0) {
        size_t startEntry = numGroups * 64;
        for (size_t i = 0; i < remaining; i++) {
            varintPacked3SetHalf(bloom, startEntry + i);
        }
    }
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* NEON-optimized version (uses SWAR - 3-word grouping doesn't vectorize well)
 */
DK_INLINE_ALWAYS void
linearBloomCountHalfNEON(linearBloomCount *restrict const bloom) {
    const size_t numGroups = LINEARBLOOMCOUNT_EXTENT_ENTRIES / 64;
    const size_t numCompleteWords = numGroups * 3;
    uint8_t boundaryValues[90000];

    /* Save boundary values */
    for (size_t g = 0; g < numGroups; g++) {
        boundaryValues[g * 2] = varintPacked3Get(bloom, g * 64 + 21);
        boundaryValues[g * 2 + 1] = varintPacked3Get(bloom, g * 64 + 42);
    }

    /* SWAR with scalar ops - only process complete groups */
    for (size_t w = 0; w + 3 <= numCompleteWords; w += 3) {
        bloom[w] = (bloom[w] & LBC_KEEP0) >> 1;
        bloom[w + 1] = (bloom[w + 1] & LBC_KEEP1) >> 1;
        bloom[w + 2] = (bloom[w + 2] & LBC_KEEP2) >> 1;
    }

    /* Restore boundary values */
    for (size_t g = 0; g < numGroups; g++) {
        varintPacked3Set(bloom, g * 64 + 21, boundaryValues[g * 2] / 2);
        varintPacked3Set(bloom, g * 64 + 42, boundaryValues[g * 2 + 1] / 2);
    }

    /* Handle remaining entries with scalar */
    size_t remaining = LINEARBLOOMCOUNT_EXTENT_ENTRIES % 64;
    if (remaining > 0) {
        size_t startEntry = numGroups * 64;
        for (size_t i = 0; i < remaining; i++) {
            varintPacked3SetHalf(bloom, startEntry + i);
        }
    }
}
#endif

#if defined(__SSE2__)
#include <emmintrin.h>

/* SSE2-optimized version */
DK_INLINE_ALWAYS void
linearBloomCountHalfSSE2(linearBloomCount *restrict const bloom) {
    /* Same hybrid approach as SWAR */
    linearBloomCountHalf(bloom);
}
#endif

#if defined(__AVX2__)
#include <immintrin.h>

/* AVX2-optimized version */
DK_INLINE_ALWAYS void
linearBloomCountHalfAVX2(linearBloomCount *restrict const bloom) {
    /* Same hybrid approach as SWAR */
    linearBloomCountHalf(bloom);
}
#endif

/* ====================================================================
 * SWAR-Optimized Power-of-2 Decay Operations
 * ==================================================================== */

/* Quarter (factor=0.25): Right-shift each 3-bit value by 2.
 * SWAR-optimized similar to Half but with different masks.
 *
 * For right-shift by 2, we keep only bit 2 of each 3-bit entry
 * (the MSB), then shift the entire word right by 2.
 */

/* QUARTER masks: keep only bit 2 of each 3-bit entry */
#define LBC_QUARTER_KEEP0                                                      \
    0x4924924924924924ULL /* entries 0-20: bits 2,5,8,...,62 */
#define LBC_QUARTER_KEEP1                                                      \
    0x2492492492492492ULL /* entry 21 bit2 at pos 1, entries 22-41 */
#define LBC_QUARTER_KEEP2                                                      \
    0x9249249249249249ULL /* entry 42 bit2 at pos 0, entries 43-63 */

DK_INLINE_ALWAYS void
linearBloomCountQuarterScalar(linearBloomCount *restrict const bloom) {
    for (uint64_t i = 0; i < LINEARBLOOMCOUNT_EXTENT_ENTRIES; i++) {
        uint8_t val = varintPacked3Get(bloom, i);
        varintPacked3Set(bloom, i, val >> 2);
    }
}

DK_INLINE_ALWAYS void
linearBloomCountQuarter(linearBloomCount *restrict const bloom) {
    const size_t numGroups = LINEARBLOOMCOUNT_EXTENT_ENTRIES / 64;
    const size_t numCompleteWords = numGroups * 3;
    const size_t numBoundary = numGroups * 2;

    uint8_t boundaryValues[90000];
    if (numBoundary > sizeof(boundaryValues)) {
        linearBloomCountQuarterScalar(bloom);
        return;
    }

    /* Save boundary values */
    for (size_t g = 0; g < numGroups; g++) {
        boundaryValues[g * 2] = varintPacked3Get(bloom, g * 64 + 21);
        boundaryValues[g * 2 + 1] = varintPacked3Get(bloom, g * 64 + 42);
    }

    /* SWAR quarter: mask to keep only MSB of each entry, then shift by 2 */
    for (size_t w = 0; w + 3 <= numCompleteWords; w += 3) {
        bloom[w] = (bloom[w] & LBC_QUARTER_KEEP0) >> 2;
        bloom[w + 1] = (bloom[w + 1] & LBC_QUARTER_KEEP1) >> 2;
        bloom[w + 2] = (bloom[w + 2] & LBC_QUARTER_KEEP2) >> 2;
    }

    /* Restore boundary values (quartered) */
    for (size_t g = 0; g < numGroups; g++) {
        varintPacked3Set(bloom, g * 64 + 21, boundaryValues[g * 2] >> 2);
        varintPacked3Set(bloom, g * 64 + 42, boundaryValues[g * 2 + 1] >> 2);
    }

    /* Handle remaining entries */
    size_t remaining = LINEARBLOOMCOUNT_EXTENT_ENTRIES % 64;
    if (remaining > 0) {
        size_t startEntry = numGroups * 64;
        for (size_t i = 0; i < remaining; i++) {
            uint8_t val = varintPacked3Get(bloom, startEntry + i);
            varintPacked3Set(bloom, startEntry + i, val >> 2);
        }
    }
}

/* Eighth (factor=0.125): All values 0-7 become 0 except 7->0, so just reset.
 * Values: 0->0, 1->0, 2->0, 3->0, 4->0, 5->0, 6->0, 7->0
 * This is equivalent to reset for 3-bit values. */
DK_INLINE_ALWAYS void
linearBloomCountEighth(linearBloomCount *restrict const bloom) {
    /* For 3-bit values, dividing by 8 always gives 0 */
    linearBloomCountReset(bloom);
}

/* ====================================================================
 * Time-Based Exponential Decay
 * ====================================================================
 *
 * Provides proper exponential decay: new_value = old_value * e^(-λt)
 * where λ = ln(2) / half_life, so after one half_life, value halves.
 *
 * For discrete 3-bit counters (0-7), we use probabilistic rounding
 * to maintain statistical accuracy. If value should become 2.7,
 * we set to 3 with 70% probability, 2 with 30% probability.
 */

/* Fast xorshift64 PRNG for probabilistic rounding */
typedef struct {
    uint64_t state;
} linearBloomCountRNG;

DK_INLINE_ALWAYS void linearBloomCountRNGInit(linearBloomCountRNG *rng,
                                              uint64_t seed) {
    rng->state = seed ? seed : 0x853c49e6748fea9bULL;
}

DK_INLINE_ALWAYS uint64_t linearBloomCountRNGNext(linearBloomCountRNG *rng) {
    uint64_t x = rng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

/* Returns a random double in [0, 1) */
DK_INLINE_ALWAYS double linearBloomCountRNGDouble(linearBloomCountRNG *rng) {
    return (double)(linearBloomCountRNGNext(rng) >> 11) *
           (1.0 / 9007199254740992.0);
}

/* Probabilistic rounding: value 2.7 becomes 3 with 70% probability, 2 with 30%
 * This maintains statistical accuracy for discrete counters over time. */
DK_INLINE_ALWAYS uint8_t linearBloomCountProbRound(double value,
                                                   linearBloomCountRNG *rng) {
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 7.0) {
        return 7;
    }

    uint8_t floor_val = (uint8_t)value;
    double frac = value - floor_val;

    /* With probability 'frac', round up; otherwise round down */
    if (linearBloomCountRNGDouble(rng) < frac) {
        return floor_val + 1;
    }
    return floor_val;
}

/* Apply decay by a factor (0.0 to 1.0) to all entries.
 * new_value = old_value * decay_factor
 *
 * Uses probabilistic rounding for statistical accuracy with discrete counters.
 *
 * @param bloom The counting bloom filter
 * @param decay_factor Multiplier between 0.0 and 1.0 (e.g., 0.5 halves values)
 * @param rng_seed Seed for probabilistic rounding RNG (use 0 for default)
 */
DK_INLINE_ALWAYS void
linearBloomCountDecayByFactor(linearBloomCount *restrict const bloom,
                              double decay_factor, uint64_t rng_seed) {
    if (decay_factor <= 0.0) {
        /* Full decay - reset to zero */
        linearBloomCountReset(bloom);
        return;
    }
    if (decay_factor >= 1.0) {
        /* No decay */
        return;
    }

    /* Check for power-of-2 factors - use SWAR-optimized implementations */
    if (decay_factor == 0.5) {
        linearBloomCountHalf(bloom);
        return;
    }
    if (decay_factor == 0.25) {
        linearBloomCountQuarter(bloom);
        return;
    }
    if (decay_factor <= 0.125) {
        linearBloomCountReset(bloom);
        return;
    }

    linearBloomCountRNG rng;
    linearBloomCountRNGInit(&rng, rng_seed);

    for (uint64_t i = 0; i < LINEARBLOOMCOUNT_EXTENT_ENTRIES; i++) {
        uint8_t old_val = varintPacked3Get(bloom, i);
        if (old_val > 0) {
            double new_val = (double)old_val * decay_factor;
            uint8_t rounded = linearBloomCountProbRound(new_val, &rng);
            varintPacked3Set(bloom, i, rounded);
        }
    }
}

/* ====================================================================
 * LUT-Based Deterministic Decay (SWAR-accelerated)
 * ==================================================================== */

/* Apply decay using a precomputed lookup table.
 * Much faster than float multiply for each entry.
 *
 * The LUT maps each 3-bit value (0-7) to its decayed value.
 * This avoids floating-point operations in the hot loop.
 */
DK_INLINE_ALWAYS void
linearBloomCountDecayByLUT(linearBloomCount *restrict const bloom,
                           const uint8_t lut[8]) {
    for (uint64_t i = 0; i < LINEARBLOOMCOUNT_EXTENT_ENTRIES; i++) {
        uint8_t old_val = varintPacked3Get(bloom, i);
        varintPacked3Set(bloom, i, lut[old_val]);
    }
}

/* Build a decay LUT for a given factor */
DK_INLINE_ALWAYS void linearBloomCountBuildDecayLUT(uint8_t lut[8],
                                                    double decay_factor) {
    for (int v = 0; v < 8; v++) {
        lut[v] = (uint8_t)(v * decay_factor);
    }
}

/* Deterministic version of DecayByFactor that uses floor rounding.
 * Optimized with LUT to avoid per-entry float multiply.
 * Also detects power-of-2 factors for SWAR acceleration. */
DK_INLINE_ALWAYS void linearBloomCountDecayByFactorDeterministic(
    linearBloomCount *restrict const bloom, double decay_factor) {
    if (decay_factor <= 0.0) {
        linearBloomCountReset(bloom);
        return;
    }
    if (decay_factor >= 1.0) {
        return;
    }

    /* Check for power-of-2 factors that have SWAR implementations */
    if (decay_factor == 0.5) {
        linearBloomCountHalf(bloom);
        return;
    }
    if (decay_factor == 0.25) {
        linearBloomCountQuarter(bloom);
        return;
    }
    if (decay_factor <= 0.125) {
        /* For factor <= 0.125, all 3-bit values become 0 */
        linearBloomCountReset(bloom);
        return;
    }

    /* Use LUT-based decay for other factors (faster than float multiply) */
    uint8_t lut[8];
    linearBloomCountBuildDecayLUT(lut, decay_factor);
    linearBloomCountDecayByLUT(bloom, lut);
}

/* Apply time-based exponential decay.
 *
 * Decay formula: new_value = old_value * 2^(-elapsed / half_life)
 *
 * After 'half_life' time units, values are halved.
 * After 2*half_life, values are quartered, etc.
 *
 * @param bloom The counting bloom filter
 * @param elapsed_ms Milliseconds elapsed since last decay
 * @param half_life_ms Half-life in milliseconds
 * @param rng_seed Seed for probabilistic rounding (use 0 for default)
 *
 * Example usage:
 *   // Decay with 1-hour half-life, 5 minutes elapsed
 *   linearBloomCountDecay(bloom, 5 * 60 * 1000, 60 * 60 * 1000, 0);
 */
DK_INLINE_ALWAYS void
linearBloomCountDecay(linearBloomCount *restrict const bloom,
                      uint64_t elapsed_ms, uint64_t half_life_ms,
                      uint64_t rng_seed) {
    if (elapsed_ms == 0 || half_life_ms == 0) {
        return;
    }

    /* decay_factor = 2^(-elapsed / half_life) = exp(-ln(2) * elapsed /
     * half_life) */
    double ratio = (double)elapsed_ms / (double)half_life_ms;
    double decay_factor = exp(-0.693147180559945309 * ratio); /* ln(2) */

    linearBloomCountDecayByFactor(bloom, decay_factor, rng_seed);
}

/* Deterministic version of time-based decay using floor rounding */
DK_INLINE_ALWAYS void
linearBloomCountDecayDeterministic(linearBloomCount *restrict const bloom,
                                   uint64_t elapsed_ms, uint64_t half_life_ms) {
    if (elapsed_ms == 0 || half_life_ms == 0) {
        return;
    }

    double ratio = (double)elapsed_ms / (double)half_life_ms;
    double decay_factor = exp(-0.693147180559945309 * ratio);

    linearBloomCountDecayByFactorDeterministic(bloom, decay_factor);
}

/* Compute the decay factor for a given time ratio.
 * Useful for pre-computing decay factors or debugging.
 *
 * @param elapsed_ms Milliseconds elapsed
 * @param half_life_ms Half-life in milliseconds
 * @return Decay factor between 0.0 and 1.0
 */
DK_INLINE_ALWAYS double
linearBloomCountComputeDecayFactor(uint64_t elapsed_ms, uint64_t half_life_ms) {
    if (half_life_ms == 0) {
        return 1.0;
    }
    double ratio = (double)elapsed_ms / (double)half_life_ms;
    return exp(-0.693147180559945309 * ratio);
}
