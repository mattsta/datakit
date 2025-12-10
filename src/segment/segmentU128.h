#pragma once

/* Segment Tree - __uint128_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(4 * 1024) elements (eager updates, cache-friendly)
 *   Full: (4 * 1024)+ elements (lazy propagation, unlimited)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __SIZEOF_INT128__

#include "segmentCore.h"

SEGMENT_DECLARE_TYPE(U128, __uint128_t, uint32_t, uint64_t, (4 * 1024), 0,
                     ((__uint128_t)-1))

/* Tier type enum (2-TIER) */
typedef enum segmentU128Type {
    SEGMENT_U128_TYPE_SMALL = 1,
    SEGMENT_U128_TYPE_FULL = 2,
} segmentU128Type;

/* Pointer tagging macros */
#define SEGMENT_U128_TYPE_MASK 0x03
#define SEGMENT_U128_TYPE(seg)                                                 \
    ((segmentU128Type)((uintptr_t)(seg) & SEGMENT_U128_TYPE_MASK))
#define SEGMENT_U128_UNTAG(seg)                                                \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_U128_TYPE_MASK))
#define SEGMENT_U128_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentU128New(segmentOp op);
void segmentU128Free(void *seg);
void segmentU128Update(void **seg, size_t idx, __uint128_t value);
__uint128_t segmentU128Query(const void *seg, size_t left, size_t right);
__uint128_t segmentU128Get(const void *seg, size_t idx);
size_t segmentU128Count(const void *seg);
size_t segmentU128Bytes(const void *seg);
void segmentU128RangeUpdate(void *seg, size_t left, size_t right,
                            __uint128_t value);

#ifdef DATAKIT_TEST
void segmentU128Repr(const void *seg);
int segmentU128Test(int argc, char *argv[]);
#endif

#endif /* __SIZEOF_INT128__ */
