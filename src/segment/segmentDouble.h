#pragma once

/* Segment Tree - double Specialization (2-TIER SYSTEM)
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

SEGMENT_DECLARE_TYPE(Double, double, uint32_t, uint64_t, (8 * 1024), -DBL_MAX,
                     DBL_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentDoubleType {
    SEGMENT_DOUBLE_TYPE_SMALL = 1,
    SEGMENT_DOUBLE_TYPE_FULL = 2,
} segmentDoubleType;

/* Pointer tagging macros */
#define SEGMENT_DOUBLE_TYPE_MASK 0x03
#define SEGMENT_DOUBLE_TYPE(seg)                                               \
    ((segmentDoubleType)((uintptr_t)(seg) & SEGMENT_DOUBLE_TYPE_MASK))
#define SEGMENT_DOUBLE_UNTAG(seg)                                              \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_DOUBLE_TYPE_MASK))
#define SEGMENT_DOUBLE_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentDoubleNew(segmentOp op);
void segmentDoubleFree(void *seg);
void segmentDoubleUpdate(void **seg, size_t idx, double value);
double segmentDoubleQuery(const void *seg, size_t left, size_t right);
double segmentDoubleGet(const void *seg, size_t idx);
size_t segmentDoubleCount(const void *seg);
size_t segmentDoubleBytes(const void *seg);
void segmentDoubleRangeUpdate(void *seg, size_t left, size_t right,
                              double value);

#ifdef DATAKIT_TEST
void segmentDoubleRepr(const void *seg);
int segmentDoubleTest(int argc, char *argv[]);
#endif
