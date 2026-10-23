#pragma once

/* Segment Tree - uint64_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(8 * 1024) elements (eager updates, cache-friendly)
 *   Full: (8 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "segmentCore.h"

SEGMENT_DECLARE_TYPE(U64, uint64_t, uint32_t, uint64_t, (8 * 1024), 0,
                     UINT64_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentU64Type {
    SEGMENT_U64_TYPE_SMALL = 1,
    SEGMENT_U64_TYPE_FULL = 2,
} segmentU64Type;

/* Pointer tagging macros */
#define SEGMENT_U64_TYPE_MASK 0x03
#define SEGMENT_U64_TYPE(seg)                                                  \
    ((segmentU64Type)((uintptr_t)(seg) & SEGMENT_U64_TYPE_MASK))
#define SEGMENT_U64_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_U64_TYPE_MASK))
#define SEGMENT_U64_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API (contract: segmentCore.h, CONTRACT).  New(op) makes a SUM,
 * MIN or MAX tree.  Update assigns one element, extending the tree (gaps
 * hold the op's identity); on a NULL tree it creates a SUM tree, so make
 * MIN and MAX trees with New first.  RangeUpdate assigns existing
 * elements only.  Query and Get outside the tree return the identity. */
void *segmentU64New(segmentOp op);
void segmentU64Free(void *seg);
void segmentU64Update(void **seg, size_t idx, uint64_t value);
uint64_t segmentU64Query(const void *seg, size_t left, size_t right);
uint64_t segmentU64Get(const void *seg, size_t idx);
size_t segmentU64Count(const void *seg);
size_t segmentU64Bytes(const void *seg);
void segmentU64RangeUpdate(void *seg, size_t left, size_t right,
                           uint64_t value);
/* A tree of 'count' elements in O(n) (count 0 or NULL: an empty tree) */
void *segmentU64NewFromArray(segmentOp op, const uint64_t *values,
                             size_t count);
/* Every element, as Get returns it, into out[0, Count) in O(n) */
void segmentU64CopyTo(const void *seg, uint64_t *out);

#ifdef DATAKIT_TEST
void segmentU64Repr(const void *seg);
#endif
