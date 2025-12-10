#pragma once

/* Fenwick Tree - uint32_t Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(U32, uint32_t, uint32_t, uint64_t, (32 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickU32Type {
    FENWICK_U32_TYPE_SMALL = 1,
    FENWICK_U32_TYPE_FULL = 2,
} fenwickU32Type;

/* Pointer tagging macros */
#define FENWICK_U32_TYPE_MASK 0x03
#define FENWICK_U32_TYPE(fw)                                                   \
    ((fenwickU32Type)((uintptr_t)(fw) & FENWICK_U32_TYPE_MASK))
#define FENWICK_U32_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_U32_TYPE_MASK))
#define FENWICK_U32_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickU32New(void);
void fenwickU32Free(void *fw);
bool fenwickU32Update(void **fw, size_t idx, uint32_t delta);
uint32_t fenwickU32Query(const void *fw, size_t idx);
uint32_t fenwickU32RangeQuery(const void *fw, size_t left, size_t right);
uint32_t fenwickU32Get(const void *fw, size_t idx);
bool fenwickU32Set(void **fw, size_t idx, uint32_t value);
size_t fenwickU32Count(const void *fw);
size_t fenwickU32Bytes(const void *fw);
size_t fenwickU32LowerBound(const void *fw, uint32_t target);
void fenwickU32Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickU32Repr(const void *fw);
int fenwickU32Test(int argc, char *argv[]);
#endif
