#pragma once

/* Segment Tree - __int128_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(4 * 1024) elements (eager updates, cache-friendly)
 *   Full: (4 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __SIZEOF_INT128__

#include "segmentCore.h"

/* Min and max built from the unsigned type (1 << 127 is signed overflow) */
SEGMENT_DECLARE_TYPE(I128, __int128_t, uint32_t, uint64_t, (4 * 1024),
                     (-(__int128_t)(~(__uint128_t)0 >> 1) - 1),
                     ((__int128_t)(~(__uint128_t)0 >> 1)))

/* Tier type enum (2-TIER) */
typedef enum segmentI128Type {
    SEGMENT_I128_TYPE_SMALL = 1,
    SEGMENT_I128_TYPE_FULL = 2,
} segmentI128Type;

/* Pointer tagging macros */
#define SEGMENT_I128_TYPE_MASK 0x03
#define SEGMENT_I128_TYPE(seg)                                                 \
    ((segmentI128Type)((uintptr_t)(seg) & SEGMENT_I128_TYPE_MASK))
#define SEGMENT_I128_UNTAG(seg)                                                \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_I128_TYPE_MASK))
#define SEGMENT_I128_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API (contract: segmentCore.h, CONTRACT).  New(op) makes a SUM,
 * MIN or MAX tree.  Update assigns one element, extending the tree (gaps
 * hold the op's identity); on a NULL tree it creates a SUM tree, so make
 * MIN and MAX trees with New first.  RangeUpdate assigns existing
 * elements only.  Query and Get outside the tree return the identity. */
void *segmentI128New(segmentOp op);
void segmentI128Free(void *seg);
bool segmentI128Update(void **seg, size_t idx, __int128_t value);
__int128_t segmentI128Query(const void *seg, size_t left, size_t right);
__int128_t segmentI128Get(const void *seg, size_t idx);
size_t segmentI128Count(const void *seg);
size_t segmentI128Bytes(const void *seg);
void segmentI128RangeUpdate(void *seg, size_t left, size_t right,
                            __int128_t value);
/* A tree of 'count' elements in O(n) (count 0 or NULL: an empty tree) */
void *segmentI128NewFromArray(segmentOp op, const __int128_t *values,
                              size_t count);
/* Every element, as Get returns it, into out[0, Count) in O(n) */
void segmentI128CopyTo(const void *seg, __int128_t *out);

#ifdef DATAKIT_TEST
void segmentI128Repr(const void *seg);
#endif

#endif /* __SIZEOF_INT128__ */
