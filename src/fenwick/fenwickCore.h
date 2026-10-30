#pragma once

/* ====================================================================
 * Fenwick Tree Core Template System (2-TIER ARCHITECTURE)
 * ====================================================================
 *
 * This file contains the macro-based template system for generating
 * type-specific Fenwick tree implementations. It follows the pattern:
 *   dks → mds → mdsc
 *
 * Architecture: Small (one contiguous allocation) → Full (separately
 * allocated cells, uint64_t indexes, capacity overflow refused).
 *
 * The algorithms (fenwickCoreAlgo.h), both tiers (fenwickCoreImpl.h,
 * fenwickCoreFullImpl.h) and the public API (fenwickCoreDispatchImpl.h)
 * are each written once and instantiated for every supported type (I16,
 * I32, I64, U16, U32, U64, Float, Double, I128, U128).
 *
 * VALUES
 * ------
 * - Elements are stored values and never wrap: Update refuses (returns
 *   false, changing nothing; a NULL tree stays NULL) a delta whose new
 *   element would not fit the type.  Set always fits.
 * - Growth: one Update or Set may grow the tree by one doubling, or to
 *   2^22 cells (dkDenseGrowth.h); an index past that is refused the same
 *   way, so a far index never allocates everything up to it.  Float trees also
 *   refuse a non-finite delta, value or element (and a Set whose change
 *   from the current element is not finite).
 * - Integer trees: Get returns the element exactly.  Query and
 *   RangeQuery return the exact sum whenever it fits the type; an
 *   integer sum that does not fit comes back modulo 2^bits (cells add
 *   with wrapping, never signed-overflow UB, and Fenwick sums are
 *   linear, so wrapped cells still give every representable sum
 *   exactly).
 * - Float and Double trees round: each cell is a sum computed in the
 *   tree's type, and Get is a difference of cells, so elements and sums
 *   are exact only while every partial sum is.  An element far smaller
 *   than its neighbours' sum can read back as 0 (Set(0, 1e16) then
 *   Set(1, 1.0) leaves Get(1) == 0); Update and Set, which start from
 *   Get, carry that error.  Use multiFenwickFlat for exact mixed sums.
 * - Query, RangeQuery and Get past the count return zero.  LowerBound
 *   assumes non-negative elements.
 * - Time: Update, Set, Query, RangeQuery and Get are O(log n) (Update
 *   and Set read the element first); NewFromArray and growth by doubling
 *   are O(n) and O(log n) per doubling.
 *
 * USAGE:
 *   #define FENWICK_SUFFIX I64
 *   #define FENWICK_VALUE_TYPE int64_t
 *   #define FENWICK_INDEX_TYPE_SMALL uint32_t
 *   #define FENWICK_INDEX_TYPE_FULL uint64_t
 *   #define FENWICK_SMALL_MAX_COUNT (16 * 1024)
 *   #include "fenwickCoreImpl.h"
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../datakit.h"
#include "fenwickCommon.h"

#ifdef DATAKIT_TEST
/* Differential tests of every typed tree (fenwickTypesTest.c) */
int fenwickTypesTest(int argc, char *argv[]);
#endif

/* ====================================================================
 * TYPE PARAMETERS
 * ==================================================================== */

/* Each Fenwick type instantiation must define these before including
 * fenwickCoreImpl.h (2-TIER SYSTEM):
 *
 * FENWICK_SUFFIX          - Type suffix (I64, U32, Double, etc.)
 * FENWICK_VALUE_TYPE      - C type for values (int64_t, uint32_t, double)
 * FENWICK_INDEX_TYPE_SMALL - Index type for Small tier (uint32_t)
 * FENWICK_INDEX_TYPE_FULL - Index type for Full tier (uint64_t)
 * FENWICK_SMALL_MAX_COUNT - Max elements for Small tier before upgrade
 * FENWICK_IS_SIGNED       - 1 if signed type, 0 otherwise
 * FENWICK_IS_FLOATING     - 1 if floating-point, 0 otherwise
 */

/* ====================================================================
 * HELPER MACROS - NAME GENERATION
 * ==================================================================== */

/* Generate function and type names with suffix */
#define FENWICK_CAT(a, b) FENWICK_CAT_IMPL(a, b)
#define FENWICK_CAT_IMPL(a, b) a##b

#define FENWICK_NAME(suffix, name)                                             \
    FENWICK_CAT(fenwick, FENWICK_CAT(suffix, name))

/* Type names (renamed to avoid conflict with runtime FENWICK_TYPE macro) */
#define FENWICK_TYPENAME(suffix) FENWICK_CAT(fenwick, suffix)
#define FENWICK_SMALL_TYPENAME(suffix)                                         \
    FENWICK_CAT(fenwick, FENWICK_CAT(suffix, Small))
#define FENWICK_FULL_TYPENAME(suffix)                                          \
    FENWICK_CAT(fenwick, FENWICK_CAT(suffix, Full))

/* ====================================================================
 * ZERO VALUE HELPER
 * ==================================================================== */

/* ====================================================================
 * STRUCTURE DEFINITION MACROS
 * ==================================================================== */

/* Small tier structure - contiguous allocation with flexible array */
#define FENWICK_DEFINE_SMALL_STRUCT(suffix, value_type, index_type)            \
    typedef struct FENWICK_SMALL_TYPENAME(suffix) {                            \
        index_type count;    /* Logical count: highest index + 1 */            \
        index_type capacity; /* Allocated size (power-of-2) */                 \
        value_type tree[];   /* Flexible array - BIT values */                 \
    } FENWICK_SMALL_TYPENAME(suffix);

/* Full tier structure - unlimited growth with overflow protection (2-TIER) */
#define FENWICK_DEFINE_FULL_STRUCT(suffix, value_type, index_type)             \
    typedef struct FENWICK_FULL_TYPENAME(suffix) {                             \
        index_type count;       /* Logical count: highest index + 1 */         \
        index_type capacity;    /* Allocated size (power-of-2) */              \
        index_type maxCapacity; /* Maximum capacity before realloc fails */    \
        value_type *tree;       /* Separately allocated BIT array */           \
    } FENWICK_FULL_TYPENAME(suffix);

/* ====================================================================
 * FORWARD DECLARATIONS MACRO
 * ==================================================================== */

#define FENWICK_DECLARE_FUNCTIONS(suffix, value_type, index_type_small,        \
                                  index_type_full)                             \
    /* Small tier */                                                           \
    FENWICK_SMALL_TYPENAME(suffix) * FENWICK_NAME(suffix, SmallNew)(void);     \
    FENWICK_SMALL_TYPENAME(suffix) *                                           \
        FENWICK_NAME(suffix, SmallNewFromArray)(const value_type *values,      \
                                                index_type_small count);       \
    void FENWICK_NAME(suffix, SmallFree)(FENWICK_SMALL_TYPENAME(suffix) * fw); \
    FENWICK_SMALL_TYPENAME(suffix) *                                           \
        FENWICK_NAME(suffix, SmallUpdate)(FENWICK_SMALL_TYPENAME(suffix) * fw, \
                                          index_type_small idx,                \
                                          value_type delta, bool *success);    \
    value_type FENWICK_NAME(suffix, SmallQuery)(                               \
        const FENWICK_SMALL_TYPENAME(suffix) * fw, index_type_small idx);      \
    value_type FENWICK_NAME(suffix, SmallRangeQuery)(                          \
        const FENWICK_SMALL_TYPENAME(suffix) * fw, index_type_small left,      \
        index_type_small right);                                               \
    value_type FENWICK_NAME(suffix, SmallGet)(                                 \
        const FENWICK_SMALL_TYPENAME(suffix) * fw, index_type_small idx);      \
    FENWICK_SMALL_TYPENAME(suffix) *                                           \
        FENWICK_NAME(suffix, SmallSet)(FENWICK_SMALL_TYPENAME(suffix) * fw,    \
                                       index_type_small idx, value_type value, \
                                       bool *success);                         \
    index_type_small FENWICK_NAME(suffix, SmallCount)(                         \
        const FENWICK_SMALL_TYPENAME(suffix) * fw);                            \
    size_t FENWICK_NAME(suffix, SmallBytes)(                                   \
        const FENWICK_SMALL_TYPENAME(suffix) * fw);                            \
    bool FENWICK_NAME(suffix, SmallShouldUpgrade)(                             \
        const FENWICK_SMALL_TYPENAME(suffix) * fw);                            \
    index_type_small FENWICK_NAME(suffix, SmallLowerBound)(                    \
        const FENWICK_SMALL_TYPENAME(suffix) * fw, value_type target);         \
    void FENWICK_NAME(suffix,                                                  \
                      SmallClear)(FENWICK_SMALL_TYPENAME(suffix) * fw);        \
                                                                               \
    /* Full tier (2-TIER: no Medium) */                                        \
    FENWICK_FULL_TYPENAME(suffix) * FENWICK_NAME(suffix, FullNew)(void);       \
    FENWICK_FULL_TYPENAME(suffix) *                                            \
        FENWICK_NAME(suffix, FullNewFromArray)(const value_type *values,       \
                                               index_type_full count);         \
    FENWICK_FULL_TYPENAME(suffix) *                                            \
        FENWICK_NAME(suffix,                                                   \
                     FullFromSmall)(FENWICK_SMALL_TYPENAME(suffix) * small);   \
    void FENWICK_NAME(suffix, FullFree)(FENWICK_FULL_TYPENAME(suffix) * fw);   \
    FENWICK_FULL_TYPENAME(suffix) *                                            \
        FENWICK_NAME(suffix, FullUpdate)(FENWICK_FULL_TYPENAME(suffix) * fw,   \
                                         index_type_full idx,                  \
                                         value_type delta, bool *success);     \
    value_type FENWICK_NAME(suffix, FullQuery)(                                \
        const FENWICK_FULL_TYPENAME(suffix) * fw, index_type_full idx);        \
    value_type FENWICK_NAME(suffix, FullRangeQuery)(                           \
        const FENWICK_FULL_TYPENAME(suffix) * fw, index_type_full left,        \
        index_type_full right);                                                \
    value_type FENWICK_NAME(suffix, FullGet)(                                  \
        const FENWICK_FULL_TYPENAME(suffix) * fw, index_type_full idx);        \
    FENWICK_FULL_TYPENAME(suffix) *                                            \
        FENWICK_NAME(suffix, FullSet)(FENWICK_FULL_TYPENAME(suffix) * fw,      \
                                      index_type_full idx, value_type value,   \
                                      bool *success);                          \
    index_type_full FENWICK_NAME(suffix, FullCount)(                           \
        const FENWICK_FULL_TYPENAME(suffix) * fw);                             \
    size_t FENWICK_NAME(suffix,                                                \
                        FullBytes)(const FENWICK_FULL_TYPENAME(suffix) * fw);  \
    index_type_full FENWICK_NAME(suffix, FullLowerBound)(                      \
        const FENWICK_FULL_TYPENAME(suffix) * fw, value_type target);          \
    void FENWICK_NAME(suffix, FullClear)(FENWICK_FULL_TYPENAME(suffix) * fw);

/* ====================================================================
 * TIER THRESHOLD CALCULATION
 * ==================================================================== */

/* Calculate tier thresholds based on value type size
 * Target: 128KB for Small, 16MB for Medium */
#define FENWICK_CALC_SMALL_MAX_BYTES (128 * 1024)
#define FENWICK_CALC_MEDIUM_MAX_BYTES (16 * 1024 * 1024)

#define FENWICK_CALC_SMALL_MAX_COUNT(value_type)                               \
    ((FENWICK_CALC_SMALL_MAX_BYTES) / sizeof(value_type))

#define FENWICK_CALC_MEDIUM_MAX_COUNT(value_type)                              \
    ((FENWICK_CALC_MEDIUM_MAX_BYTES) / sizeof(value_type))

/* ====================================================================
 * UTILITY MACROS FOR TESTS
 * ==================================================================== */

#ifdef DATAKIT_TEST
#define FENWICK_DECLARE_TEST_FUNCTIONS(suffix, value_type)                     \
    void FENWICK_NAME(suffix,                                                  \
                      SmallRepr)(const FENWICK_SMALL_TYPENAME(suffix) * fw);   \
    void FENWICK_NAME(suffix,                                                  \
                      FullRepr)(const FENWICK_FULL_TYPENAME(suffix) * fw);
#else
#define FENWICK_DECLARE_TEST_FUNCTIONS(suffix, value_type)
#endif

/* ====================================================================
 * COMPLETE TYPE INSTANTIATION MACRO
 * ==================================================================== */

/* This macro generates all structure and function declarations for a type
 * (2-TIER) */
#define FENWICK_DECLARE_TYPE(suffix, value_type, index_type_small,             \
                             index_type_full, small_max, unused_medium_max)    \
    FENWICK_DEFINE_SMALL_STRUCT(suffix, value_type, index_type_small)          \
    FENWICK_DEFINE_FULL_STRUCT(suffix, value_type, index_type_full)            \
    FENWICK_DECLARE_FUNCTIONS(suffix, value_type, index_type_small,            \
                              index_type_full)                                 \
    FENWICK_DECLARE_TEST_FUNCTIONS(suffix, value_type)
