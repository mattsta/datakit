#pragma once

/* Fenwick Tree - uint16_t Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(U16, uint16_t, uint32_t, uint64_t, (64 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickU16Type {
    FENWICK_U16_TYPE_SMALL = 1,
    FENWICK_U16_TYPE_FULL = 2,
} fenwickU16Type;

/* Pointer tagging macros */
#define FENWICK_U16_TYPE_MASK 0x03
#define FENWICK_U16_TYPE(fw)                                                   \
    ((fenwickU16Type)((uintptr_t)(fw) & FENWICK_U16_TYPE_MASK))
#define FENWICK_U16_UNTAG(fw)                                                  \
    ((void *)((uintptr_t)(fw) & ~FENWICK_U16_TYPE_MASK))
#define FENWICK_U16_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickU16New(void);
void fenwickU16Free(void *fw);
bool fenwickU16Update(void **fw, size_t idx, uint16_t delta);
uint16_t fenwickU16Query(const void *fw, size_t idx);
uint16_t fenwickU16RangeQuery(const void *fw, size_t left, size_t right);
uint16_t fenwickU16Get(const void *fw, size_t idx);
bool fenwickU16Set(void **fw, size_t idx, uint16_t value);
size_t fenwickU16Count(const void *fw);
size_t fenwickU16Bytes(const void *fw);
size_t fenwickU16LowerBound(const void *fw, uint16_t target);
void fenwickU16Clear(void *fw);

#ifdef DATAKIT_TEST
void fenwickU16Repr(const void *fw);
int fenwickU16Test(int argc, char *argv[]);
#endif
