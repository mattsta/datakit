#pragma once

/* Segment Tree - float Specialization (2-TIER SYSTEM)
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

/* Public API - 2-tier automatic tier management */
void *segmentFloatNew(segmentOp op);
void segmentFloatFree(void *seg);
void segmentFloatUpdate(void **seg, size_t idx, float value);
float segmentFloatQuery(const void *seg, size_t left, size_t right);
float segmentFloatGet(const void *seg, size_t idx);
size_t segmentFloatCount(const void *seg);
size_t segmentFloatBytes(const void *seg);
void segmentFloatRangeUpdate(void *seg, size_t left, size_t right, float value);

#ifdef DATAKIT_TEST
void segmentFloatRepr(const void *seg);
int segmentFloatTest(int argc, char *argv[]);
#endif
