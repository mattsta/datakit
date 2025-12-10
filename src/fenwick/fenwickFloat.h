#pragma once

/* Fenwick Tree - float Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(Float, float, uint32_t, uint64_t, (32 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickFloatType {
    FENWICK_FLOAT_TYPE_SMALL = 1,
    FENWICK_FLOAT_TYPE_FULL = 2,
} fenwickFloatType;

/* Pointer tagging macros */
#define FENWICK_FLOAT_TYPE_MASK 0x03
#define FENWICK_FLOAT_TYPE(fw)                                                 \
    ((fenwickFloatType)((uintptr_t)(fw) & FENWICK_FLOAT_TYPE_MASK))
#define FENWICK_FLOAT_UNTAG(fw)                                                \
    ((void *)((uintptr_t)(fw) & ~FENWICK_FLOAT_TYPE_MASK))
#define FENWICK_FLOAT_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickFloatNew(void);
void fenwickFloatFree(void *fw);
bool fenwickFloatUpdate(void **fw, size_t idx, float delta);
float fenwickFloatQuery(const void *fw, size_t idx);
float fenwickFloatRangeQuery(const void *fw, size_t left, size_t right);
float fenwickFloatGet(const void *fw, size_t idx);
bool fenwickFloatSet(void **fw, size_t idx, float value);
size_t fenwickFloatCount(const void *fw);
size_t fenwickFloatBytes(const void *fw);
size_t fenwickFloatLowerBound(const void *fw, float target);
void fenwickFloatClear(void *fw);

#ifdef DATAKIT_TEST
void fenwickFloatRepr(const void *fw);
int fenwickFloatTest(int argc, char *argv[]);
#endif
