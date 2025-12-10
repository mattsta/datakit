#pragma once

/* Fenwick Tree - double Specialization (2-TIER SYSTEM)
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
FENWICK_DECLARE_TYPE(Double, double, uint32_t, uint64_t, (16 * 1024),
                     0 /* medium_max unused in 2-tier */
)

/* Tier type enum (2-TIER) */
typedef enum fenwickDoubleType {
    FENWICK_DOUBLE_TYPE_SMALL = 1,
    FENWICK_DOUBLE_TYPE_FULL = 2,
} fenwickDoubleType;

/* Pointer tagging macros */
#define FENWICK_DOUBLE_TYPE_MASK 0x03
#define FENWICK_DOUBLE_TYPE(fw)                                                \
    ((fenwickDoubleType)((uintptr_t)(fw) & FENWICK_DOUBLE_TYPE_MASK))
#define FENWICK_DOUBLE_UNTAG(fw)                                               \
    ((void *)((uintptr_t)(fw) & ~FENWICK_DOUBLE_TYPE_MASK))
#define FENWICK_DOUBLE_TAG(ptr, type) ((void *)((uintptr_t)(ptr) | (type)))

/* Public API - 2-tier automatic tier management */
void *fenwickDoubleNew(void);
void fenwickDoubleFree(void *fw);
bool fenwickDoubleUpdate(void **fw, size_t idx, double delta);
double fenwickDoubleQuery(const void *fw, size_t idx);
double fenwickDoubleRangeQuery(const void *fw, size_t left, size_t right);
double fenwickDoubleGet(const void *fw, size_t idx);
bool fenwickDoubleSet(void **fw, size_t idx, double value);
size_t fenwickDoubleCount(const void *fw);
size_t fenwickDoubleBytes(const void *fw);
size_t fenwickDoubleLowerBound(const void *fw, double target);
void fenwickDoubleClear(void *fw);

#ifdef DATAKIT_TEST
void fenwickDoubleRepr(const void *fw);
int fenwickDoubleTest(int argc, char *argv[]);
#endif
