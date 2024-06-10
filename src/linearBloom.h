#pragma once

#include "datakit.h"

typedef uint_fast32_t linearBloom;

/* These defaults (m = 2^21 bits (256 KB), k = 13 hashes)
 * are approximately equivalent to a 1 in 10,000 false positive
 * rate for storing 100,000 items */

/* These defaults (m = 2^23 bits (1 MB), k = 13 hashes)
 * are approximately equivalent to a 1 in 10,000 false positive
 * rate for storing 430,000 items */

#ifndef LINEARBLOOM_HASHES
#define LINEARBLOOM_HASHES 13
#endif

#ifndef LINEARBLOOM_EXTENT_BITS
#define LINEARBLOOM_EXTENT_BITS (1ULL << 23)
#endif

#define divCeil(a, b) (((a) + (b)-1) / (b))

/* This is a bit of a mess because we need a:
 *  - CEILING ROUND from bits to whole bytes
 *  - then a CEILING ROUND from bytes to our storage width
 *  - then turn our storage width count back into a byte count with multiply */
#define LINEARBLOOM_EXTENT_BYTES                                               \
    (divCeil(divCeil(LINEARBLOOM_EXTENT_BITS, 8), sizeof(linearBloom)) *       \
     sizeof(linearBloom))

#define LINEARBLOOM_KIRSCHMITZENMACHER(iteration, hash1, hash2)                \
    ((hash1) + (iteration) * (hash2))

DK_INLINE_ALWAYS linearBloom *linearBloomNew(void) {
    /* We need to use 'divCeil' here because regular division would give us
     * a floor and that could break some very end-of-array math if our
     * bit length isn't evenly divisible by 8 */
    linearBloom *const created = zcalloc(1, LINEARBLOOM_EXTENT_BYTES);
    return created;
}

DK_INLINE_ALWAYS void linearBloomFree(linearBloom *bloom) {
    if (bloom) {
        zfree(bloom);
    }
}

DK_INLINE_ALWAYS void linearBloomReset(linearBloom *restrict bloom) {
    memset(bloom, 0, LINEARBLOOM_EXTENT_BYTES);
}

#define LB_BITS_PER_SLOT (sizeof(linearBloom) * 8)
DK_INLINE_ALWAYS uint_fast8_t
linearBloomHashSet(linearBloom *restrict const bloom, uint64_t hash[2]) {
    uint_fast32_t exists = 0;
    for (uint32_t i = 0; i < LINEARBLOOM_HASHES; i++) {
        const uint64_t setBit =
            LINEARBLOOM_KIRSCHMITZENMACHER(i, hash[0], hash[1]) %
            LINEARBLOOM_EXTENT_BITS;
        const size_t byte = setBit / 8;
        const size_t offset = byte / sizeof(linearBloom);
        const linearBloom mask = 1ULL << (setBit % LB_BITS_PER_SLOT);
        exists += (bloom[offset] & mask);
        bloom[offset] |= mask;
    }

    return exists == LINEARBLOOM_HASHES;
}

DK_INLINE_ALWAYS bool
linearBloomHashCheck(const linearBloom *restrict const bloom,
                     uint64_t hash[2]) {
    uint_fast32_t exists = 0;
    for (uint32_t i = 0; i < LINEARBLOOM_HASHES; i++) {
        const uint64_t setBit =
            LINEARBLOOM_KIRSCHMITZENMACHER(i, hash[0], hash[1]) %
            LINEARBLOOM_EXTENT_BITS;
        const size_t byte = setBit / 8;
        const size_t offset = byte / sizeof(linearBloom);
        const linearBloom mask = 1ULL << (setBit % LB_BITS_PER_SLOT);

        /* Note: this isn't an "if (!exists)" because that would introduce
         *       a branch inside the loop and we just want to run all the
         *       bits then check at the end instead. */
        exists += (bloom[offset] & mask);
    }

    return exists == LINEARBLOOM_HASHES;
}
