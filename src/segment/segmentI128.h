#pragma once

/* Segment Tree - __int128_t Specialization (2-TIER SYSTEM)
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

SEGMENT_DECLARE_TYPE(I128, __int128_t, uint32_t, uint64_t, (4 * 1024),
                     ((__int128_t)1 << 127), (~((__int128_t)1 << 127)))

/* Tier type enum (2-TIER) */
typedef enum segmentI128Type {
    SEGMENT_I128_TYPE_SMALL = 1,
    SEGMENT_I128_TYPE_FULL = 2,
} segmentI128Type;

/* Pointer tagging macros */
#define SEGMENT_I128_TYPE_MASK 0x03
#define SEGMENT_I128_TYPE(seg)                                                 \
    ((segmentI128Type)((uintptr_t)(seg) & SEGMENT_I128_TYPE_MASK))
#define SEGMENT_I128_UNTAG(seg)                                                \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_I128_TYPE_MASK))
#define SEGMENT_I128_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentI128New(segmentOp op);
void segmentI128Free(void *seg);
void segmentI128Update(void **seg, size_t idx, __int128_t value);
__int128_t segmentI128Query(const void *seg, size_t left, size_t right);
__int128_t segmentI128Get(const void *seg, size_t idx);
size_t segmentI128Count(const void *seg);
size_t segmentI128Bytes(const void *seg);
void segmentI128RangeUpdate(void *seg, size_t left, size_t right,
                            __int128_t value);

#ifdef DATAKIT_TEST
void segmentI128Repr(const void *seg);
int segmentI128Test(int argc, char *argv[]);
#endif

#endif /* __SIZEOF_INT128__ */
