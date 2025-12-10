#pragma once

/* Fenwick Tree - __uint128_t Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(U128, __uint128_t, uint32_t, uint64_t, (8 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickU128Type {
    FENWICK_U128_TYPE_SMALL = 1,
    FENWICK_U128_TYPE_FULL = 2,
} fenwickU128Type;

/* Pointer tagging macros */
#define FENWICK_U128_TYPE_MASK 0x03
#define FENWICK_U128_TYPE(fw)                                                  \
    ((fenwickU128Type)((uintptr_t)(fw) & FENWICK_U128_TYPE_MASK))
#define FENWICK_U128_UNTAG(fw)                                                 \
    ((void *)((uintptr_t)(fw) & ~FENWICK_U128_TYPE_MASK))
#define FENWICK_U128_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickU128New(void);
void fenwickU128Free(void *fw);
bool fenwickU128Update(void **fw, size_t idx, __uint128_t delta);
__uint128_t fenwickU128Query(const void *fw, size_t idx);
__uint128_t fenwickU128RangeQuery(const void *fw, size_t left, size_t right);
__uint128_t fenwickU128Get(const void *fw, size_t idx);
bool fenwickU128Set(void **fw, size_t idx, __uint128_t value);
size_t fenwickU128Count(const void *fw);
size_t fenwickU128Bytes(const void *fw);
size_t fenwickU128LowerBound(const void *fw, __uint128_t target);
void fenwickU128Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickU128Repr(const void *fw);
int fenwickU128Test(int argc, char *argv[]);
#endif

#endif /* __SIZEOF_INT128__ */
