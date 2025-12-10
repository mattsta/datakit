#pragma once

/* Segment Tree - int32_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(16 * 1024) elements (eager updates, cache-friendly)
 *   Full: (16 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "segmentCore.h"

SEGMENT_DECLARE_TYPE(I32, int32_t, uint32_t, uint64_t, (16 * 1024), INT32_MIN,
                     INT32_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentI32Type {
    SEGMENT_I32_TYPE_SMALL = 1,
    SEGMENT_I32_TYPE_FULL = 2,
} segmentI32Type;

/* Pointer tagging macros */
#define SEGMENT_I32_TYPE_MASK 0x03
#define SEGMENT_I32_TYPE(seg)                                                  \
    ((segmentI32Type)((uintptr_t)(seg) & SEGMENT_I32_TYPE_MASK))
#define SEGMENT_I32_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_I32_TYPE_MASK))
#define SEGMENT_I32_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentI32New(segmentOp op);
void segmentI32Free(void *seg);
void segmentI32Update(void **seg, size_t idx, int32_t value);
int32_t segmentI32Query(const void *seg, size_t left, size_t right);
int32_t segmentI32Get(const void *seg, size_t idx);
size_t segmentI32Count(const void *seg);
size_t segmentI32Bytes(const void *seg);
void segmentI32RangeUpdate(void *seg, size_t left, size_t right, int32_t value);

#ifdef DATAKIT_TEST
void segmentI32Repr(const void *seg);
int segmentI32Test(int argc, char *argv[]);
#endif
