#pragma once

/* Segment Tree - float Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(16 * 1024) elements (eager updates, cache-friendly)
 *   Full: (16 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stddef.h>
#include <stdint.h>

#include "segmentCore.h"

SEGMENT_DECLARE_TYPE(Float, float, uint32_t, uint64_t, (16 * 1024), -FLT_MAX,
                     FLT_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentFloatType {
    SEGMENT_FLOAT_TYPE_SMALL = 1,
    SEGMENT_FLOAT_TYPE_FULL = 2,
} segmentFloatType;

/* Pointer tagging macros */
#define SEGMENT_FLOAT_TYPE_MASK 0x03
#define SEGMENT_FLOAT_TYPE(seg)                                                \
    ((segmentFloatType)((uintptr_t)(seg) & SEGMENT_FLOAT_TYPE_MASK))
#define SEGMENT_FLOAT_UNTAG(seg)                                               \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_FLOAT_TYPE_MASK))
#define SEGMENT_FLOAT_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API (contract: segmentCore.h, CONTRACT).  New(op) makes a SUM,
 * MIN or MAX tree.  Update assigns one element, extending the tree (gaps
 * hold the op's identity); on a NULL tree it creates a SUM tree, so make
 * MIN and MAX trees with New first.  RangeUpdate assigns existing
 * elements only.  Query and Get outside the tree return the identity. */
void *segmentFloatNew(segmentOp op);
void segmentFloatFree(void *seg);
bool segmentFloatUpdate(void **seg, size_t idx, float value);
float segmentFloatQuery(const void *seg, size_t left, size_t right);
float segmentFloatGet(const void *seg, size_t idx);
size_t segmentFloatCount(const void *seg);
size_t segmentFloatBytes(const void *seg);
void segmentFloatRangeUpdate(void *seg, size_t left, size_t right, float value);
/* A tree of 'count' elements in O(n) (count 0 or NULL: an empty tree) */
void *segmentFloatNewFromArray(segmentOp op, const float *values, size_t count);
/* Every element, as Get returns it, into out[0, Count) in O(n) */
void segmentFloatCopyTo(const void *seg, float *out);

#ifdef DATAKIT_TEST
void segmentFloatRepr(const void *seg);
#endif
