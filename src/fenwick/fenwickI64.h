#pragma once

/* Fenwick Tree - int64_t Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(I64, int64_t, uint32_t, uint64_t, (16 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickI64Type {
    FENWICK_I64_TYPE_SMALL = 1,
    FENWICK_I64_TYPE_FULL = 2,
} fenwickI64Type;

/* Pointer tagging macros */
#define FENWICK_I64_TYPE_MASK 0x03
#define FENWICK_I64_TYPE(fw)                                                   \
    ((fenwickI64Type)((uintptr_t)(fw) & FENWICK_I64_TYPE_MASK))
#define FENWICK_I64_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_I64_TYPE_MASK))
#define FENWICK_I64_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickI64New(void);
void fenwickI64Free(void *fw);
bool fenwickI64Update(void **fw, size_t idx, int64_t delta);
int64_t fenwickI64Query(const void *fw, size_t idx);
int64_t fenwickI64RangeQuery(const void *fw, size_t left, size_t right);
int64_t fenwickI64Get(const void *fw, size_t idx);
bool fenwickI64Set(void **fw, size_t idx, int64_t value);
size_t fenwickI64Count(const void *fw);
size_t fenwickI64Bytes(const void *fw);
size_t fenwickI64LowerBound(const void *fw, int64_t target);
void fenwickI64Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickI64Repr(const void *fw);
int fenwickI64Test(int argc, char *argv[]);
#endif
