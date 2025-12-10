#pragma once

/* Segment Tree - uint16_t Specialization (2-TIER SYSTEM)
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

SEGMENT_DECLARE_TYPE(U16, uint16_t, uint32_t, uint64_t, (32 * 1024), 0,
                     UINT16_MAX)

/* Tier type enum (2-TIER) */
typedef enum segmentU16Type {
    SEGMENT_U16_TYPE_SMALL = 1,
    SEGMENT_U16_TYPE_FULL = 2,
} segmentU16Type;

/* Pointer tagging macros */
#define SEGMENT_U16_TYPE_MASK 0x03
#define SEGMENT_U16_TYPE(seg)                                                  \
    ((segmentU16Type)((uintptr_t)(seg) & SEGMENT_U16_TYPE_MASK))
#define SEGMENT_U16_UNTAG(seg)                                                 \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_U16_TYPE_MASK))
#define SEGMENT_U16_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *segmentU16New(segmentOp op);
void segmentU16Free(void *seg);
void segmentU16Update(void **seg, size_t idx, uint16_t value);
uint16_t segmentU16Query(const void *seg, size_t left, size_t right);
uint16_t segmentU16Get(const void *seg, size_t idx);
size_t segmentU16Count(const void *seg);
size_t segmentU16Bytes(const void *seg);
void segmentU16RangeUpdate(void *seg, size_t left, size_t right,
                           uint16_t value);

#ifdef DATAKIT_TEST
void segmentU16Repr(const void *seg);
int segmentU16Test(int argc, char *argv[]);
#endif
