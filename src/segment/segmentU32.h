#pragma once

/* Segment Tree - uint32_t Specialization (2-TIER SYSTEM)
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

SEGMENT_DECLARE_TYPE(U32, uint32_t, uint32_t, uint64_t, (16 * 1024), 0,
                     UINT32_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentU32Type {
    SEGMENT_U32_TYPE_SMALL = 1,
    SEGMENT_U32_TYPE_FULL = 2,
} segmentU32Type;

/* Pointer tagging macros */
#define SEGMENT_U32_TYPE_MASK 0x03
#define SEGMENT_U32_TYPE(seg)                                                  \
    ((segmentU32Type)((uintptr_t)(seg) & SEGMENT_U32_TYPE_MASK))
#define SEGMENT_U32_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_U32_TYPE_MASK))
#define SEGMENT_U32_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentU32New(segmentOp op);
void segmentU32Free(void *seg);
void segmentU32Update(void **seg, size_t idx, uint32_t value);
uint32_t segmentU32Query(const void *seg, size_t left, size_t right);
uint32_t segmentU32Get(const void *seg, size_t idx);
size_t segmentU32Count(const void *seg);
size_t segmentU32Bytes(const void *seg);
void segmentU32RangeUpdate(void *seg, size_t left, size_t right,
                           uint32_t value);

#ifdef DATAKIT_TEST
void segmentU32Repr(const void *seg);
int segmentU32Test(int argc, char *argv[]);
#endif
