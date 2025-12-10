#pragma once

/* Segment Tree - int64_t Specialization (2-TIER SYSTEM)
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

SEGMENT_DECLARE_TYPE(I64, int64_t, uint32_t, uint64_t, (8 * 1024), INT64_MIN,
                     INT64_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentI64Type {
    SEGMENT_I64_TYPE_SMALL = 1,
    SEGMENT_I64_TYPE_FULL = 2,
} segmentI64Type;

/* Pointer tagging macros */
#define SEGMENT_I64_TYPE_MASK 0x03
#define SEGMENT_I64_TYPE(seg)                                                  \
    ((segmentI64Type)((uintptr_t)(seg) & SEGMENT_I64_TYPE_MASK))
#define SEGMENT_I64_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_I64_TYPE_MASK))
#define SEGMENT_I64_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentI64New(segmentOp op);
void segmentI64Free(void *seg);
void segmentI64Update(void **seg, size_t idx, int64_t value);
int64_t segmentI64Query(const void *seg, size_t left, size_t right);
int64_t segmentI64Get(const void *seg, size_t idx);
size_t segmentI64Count(const void *seg);
size_t segmentI64Bytes(const void *seg);
void segmentI64RangeUpdate(void *seg, size_t left, size_t right, int64_t value);

#ifdef DATAKIT_TEST
void segmentI64Repr(const void *seg);
int segmentI64Test(int argc, char *argv[]);
#endif
