#pragma once

/* Segment Tree - int16_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(32 * 1024) elements (eager updates, cache-friendly)
 *   Full: (32 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "segmentCore.h"

SEGMENT_DECLARE_TYPE(I16, int16_t, uint32_t, uint64_t, (32 * 1024), INT16_MIN,
                     INT16_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentI16Type {
    SEGMENT_I16_TYPE_SMALL = 1,
    SEGMENT_I16_TYPE_FULL = 2,
} segmentI16Type;

/* Pointer tagging macros */
#define SEGMENT_I16_TYPE_MASK 0x03
#define SEGMENT_I16_TYPE(seg)                                                  \
    ((segmentI16Type)((uintptr_t)(seg) & SEGMENT_I16_TYPE_MASK))
#define SEGMENT_I16_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_I16_TYPE_MASK))
#define SEGMENT_I16_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentI16New(segmentOp op);
void segmentI16Free(void *seg);
void segmentI16Update(void **seg, size_t idx, int16_t value);
int16_t segmentI16Query(const void *seg, size_t left, size_t right);
int16_t segmentI16Get(const void *seg, size_t idx);
size_t segmentI16Count(const void *seg);
size_t segmentI16Bytes(const void *seg);
void segmentI16RangeUpdate(void *seg, size_t left, size_t right, int16_t value);

#ifdef DATAKIT_TEST
void segmentI16Repr(const void *seg);
int segmentI16Test(int argc, char *argv[]);
#endif
