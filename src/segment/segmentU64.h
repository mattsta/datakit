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

/* Public API - 2-tier automatic tier management */
void *segmentU64New(segmentOp op);
void segmentU64Free(void *seg);
void segmentU64Update(void **seg, size_t idx, uint64_t value);
uint64_t segmentU64Query(const void *seg, size_t left, size_t right);
uint64_t segmentU64Get(const void *seg, size_t idx);
size_t segmentU64Count(const void *seg);
size_t segmentU64Bytes(const void *seg);
void segmentU64RangeUpdate(void *seg, size_t left, size_t right,
                           uint64_t value);

#ifdef DATAKIT_TEST
void segmentU64Repr(const void *seg);
int segmentU64Test(int argc, char *argv[]);
#endif
