#pragma once

/* Fenwick Tree - uint64_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(16 * 1024) elements (cache-friendly, contiguous)
 *   Full: (16 * 1024)+ elements (unlimited growth)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fenwickCore.h"

/* Type declarations */
FENWICK_DECLARE_TYPE(U64, uint64_t, uint32_t, uint64_t, (16 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickU64Type {
    FENWICK_U64_TYPE_SMALL = 1,
    FENWICK_U64_TYPE_FULL = 2,
} fenwickU64Type;

/* Pointer tagging macros */
#define FENWICK_U64_TYPE_MASK 0x03
#define FENWICK_U64_TYPE(fw)                                                   \
    ((fenwickU64Type)((uintptr_t)(fw) & FENWICK_U64_TYPE_MASK))
#define FENWICK_U64_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_U64_TYPE_MASK))
#define FENWICK_U64_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickU64New(void);
void fenwickU64Free(void *fw);
bool fenwickU64Update(void **fw, size_t idx, uint64_t delta);
uint64_t fenwickU64Query(const void *fw, size_t idx);
uint64_t fenwickU64RangeQuery(const void *fw, size_t left, size_t right);
uint64_t fenwickU64Get(const void *fw, size_t idx);
bool fenwickU64Set(void **fw, size_t idx, uint64_t value);
size_t fenwickU64Count(const void *fw);
size_t fenwickU64Bytes(const void *fw);
size_t fenwickU64LowerBound(const void *fw, uint64_t target);
void fenwickU64Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickU64Repr(const void *fw);
int fenwickU64Test(int argc, char *argv[]);
#endif
