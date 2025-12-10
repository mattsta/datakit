#pragma once

/* Fenwick Tree - int16_t Specialization (2-TIER SYSTEM)
 * Auto-generated type-specific header.
 *
 * Architecture:
 *   Small: 0-(64 * 1024) elements (cache-friendly, contiguous)
 *   Full: (64 * 1024)+ elements (unlimited growth)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fenwickCore.h"

/* Type declarations */
FENWICK_DECLARE_TYPE(I16, int16_t, uint32_t, uint64_t, (64 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickI16Type {
    FENWICK_I16_TYPE_SMALL = 1,
    FENWICK_I16_TYPE_FULL = 2,
} fenwickI16Type;

/* Pointer tagging macros */
#define FENWICK_I16_TYPE_MASK 0x03
#define FENWICK_I16_TYPE(fw)                                                   \
    ((fenwickI16Type)((uintptr_t)(fw) & FENWICK_I16_TYPE_MASK))
#define FENWICK_I16_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_I16_TYPE_MASK))
#define FENWICK_I16_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickI16New(void);
void fenwickI16Free(void *fw);
bool fenwickI16Update(void **fw, size_t idx, int16_t delta);
int16_t fenwickI16Query(const void *fw, size_t idx);
int16_t fenwickI16RangeQuery(const void *fw, size_t left, size_t right);
int16_t fenwickI16Get(const void *fw, size_t idx);
bool fenwickI16Set(void **fw, size_t idx, int16_t value);
size_t fenwickI16Count(const void *fw);
size_t fenwickI16Bytes(const void *fw);
size_t fenwickI16LowerBound(const void *fw, int16_t target);
void fenwickI16Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickI16Repr(const void *fw);
int fenwickI16Test(int argc, char *argv[]);
#endif
