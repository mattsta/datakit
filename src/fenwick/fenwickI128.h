#pragma once

/* Fenwick Tree - __int128_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(8 * 1024) elements (cache-friendly, contiguous)
 *   Full: (8 * 1024)+ elements (unlimited growth)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __SIZEOF_INT128__

#include "fenwickCore.h"

/* Type declarations */
FENWICK_DECLARE_TYPE(I128, __int128_t, uint32_t, uint64_t, (8 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickI128Type {
    FENWICK_I128_TYPE_SMALL = 1,
    FENWICK_I128_TYPE_FULL = 2,
} fenwickI128Type;

/* Pointer tagging macros */
#define FENWICK_I128_TYPE_MASK 0x03
#define FENWICK_I128_TYPE(fw)                                                  \
    ((fenwickI128Type)((uintptr_t)(fw) & FENWICK_I128_TYPE_MASK))
#define FENWICK_I128_UNTAG(fw)                                                 \
    ((void *)((uintptr_t)(fw) & ~FENWICK_I128_TYPE_MASK))
#define FENWICK_I128_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickI128New(void);
void fenwickI128Free(void *fw);
bool fenwickI128Update(void **fw, size_t idx, __int128_t delta);
__int128_t fenwickI128Query(const void *fw, size_t idx);
__int128_t fenwickI128RangeQuery(const void *fw, size_t left, size_t right);
__int128_t fenwickI128Get(const void *fw, size_t idx);
bool fenwickI128Set(void **fw, size_t idx, __int128_t value);
size_t fenwickI128Count(const void *fw);
size_t fenwickI128Bytes(const void *fw);
size_t fenwickI128LowerBound(const void *fw, __int128_t target);
void fenwickI128Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickI128Repr(const void *fw);
int fenwickI128Test(int argc, char *argv[]);
#endif

#endif /* __SIZEOF_INT128__ */
