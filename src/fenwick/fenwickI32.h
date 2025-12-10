#pragma once

/* Fenwick Tree - int32_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(32 * 1024) elements (cache-friendly, contiguous)
 *   Full: (32 * 1024)+ elements (unlimited growth)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fenwickCore.h"

/* Type declarations */
FENWICK_DECLARE_TYPE(I32, int32_t, uint32_t, uint64_t, (32 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickI32Type {
    FENWICK_I32_TYPE_SMALL = 1,
    FENWICK_I32_TYPE_FULL = 2,
} fenwickI32Type;

/* Pointer tagging macros */
#define FENWICK_I32_TYPE_MASK 0x03
#define FENWICK_I32_TYPE(fw)                                                   \
    ((fenwickI32Type)((uintptr_t)(fw) & FENWICK_I32_TYPE_MASK))
#define FENWICK_I32_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_I32_TYPE_MASK))
#define FENWICK_I32_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickI32New(void);
void fenwickI32Free(void *fw);
bool fenwickI32Update(void **fw, size_t idx, int32_t delta);
int32_t fenwickI32Query(const void *fw, size_t idx);
int32_t fenwickI32RangeQuery(const void *fw, size_t left, size_t right);
int32_t fenwickI32Get(const void *fw, size_t idx);
bool fenwickI32Set(void **fw, size_t idx, int32_t value);
size_t fenwickI32Count(const void *fw);
size_t fenwickI32Bytes(const void *fw);
size_t fenwickI32LowerBound(const void *fw, int32_t target);
void fenwickI32Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickI32Repr(const void *fw);
int fenwickI32Test(int argc, char *argv[]);
#endif
