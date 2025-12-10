#pragma once

/* ====================================================================
 * Segment Tree Core Template System (2-TIER ARCHITECTURE)
 * ====================================================================
 *
 * Macro-based template system for generating type-specific segment tree
 * implementations. Follows the proven Fenwick pattern: dks → mds → mdsc
 *
 * Architecture: Small (eager updates) → Full (lazy propagation)
 * - Benefit: 37% fewer files, 50% fewer transitions vs 3-tier
 * - Small: Cache-friendly, eager updates, optimal for small trees
 * - Full: Unlimited growth, lazy propagation for range updates
 *
 * Supported operations: MIN, MAX, SUM (all types)
 * Supported types: I16, I32, I64, U16, U32, U64, Float, Double, I128, U128
 *
 * USAGE:
 *   #define SEGMENT_SUFFIX I64
 *   #define SEGMENT_VALUE_TYPE int64_t
 *   #define SEGMENT_INDEX_TYPE_SMALL uint32_t
 *   #define SEGMENT_INDEX_TYPE_FULL uint64_t
 *   #define SEGMENT_SMALL_MAX_COUNT (8 * 1024)
 *   #include "segmentCoreImpl.h"
 */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../datakit.h"
#include "segmentCommon.h"

/* ====================================================================
 * TYPE PARAMETERS (must be defined before including templates)
 * ==================================================================== */

/* Required defines:
 * SEGMENT_SUFFIX          - Type suffix (I64, U32, Double, etc.)
 * SEGMENT_VALUE_TYPE      - C type for values (int64_t, uint32_t, double)
 * SEGMENT_INDEX_TYPE_SMALL - Index type for Small tier (uint32_t)
 * SEGMENT_INDEX_TYPE_FULL - Index type for Full tier (uint64_t)
 * SEGMENT_SMALL_MAX_COUNT - Max elements for Small tier before upgrade
 * SEGMENT_IS_SIGNED       - 1 if signed type, 0 otherwise
 * SEGMENT_IS_FLOATING     - 1 if floating-point, 0 otherwise
 * SEGMENT_TYPE_MIN        - Minimum value for type (for MAX op identity)
 * SEGMENT_TYPE_MAX        - Maximum value for type (for MIN op identity)
 */

/* ====================================================================
 * HELPER MACROS - NAME GENERATION
 * ==================================================================== */

#define SEGMENT_CAT(a, b) SEGMENT_CAT_IMPL(a, b)
#define SEGMENT_CAT_IMPL(a, b) a##b

#define SEGMENT_NAME(suffix, name)                                             \
    SEGMENT_CAT(segment, SEGMENT_CAT(suffix, name))

/* Type names (avoid conflict with runtime macros) */
#define SEGMENT_TYPENAME(suffix) SEGMENT_CAT(segment, suffix)
#define SEGMENT_SMALL_TYPENAME(suffix)                                         \
    SEGMENT_CAT(segment, SEGMENT_CAT(suffix, Small))
#define SEGMENT_FULL_TYPENAME(suffix)                                          \
    SEGMENT_CAT(segment, SEGMENT_CAT(suffix, Full))

/* ====================================================================
 * IDENTITY VALUE HELPERS
 * ==================================================================== */

/* Get identity value for operation and type */
#define SEGMENT_IDENTITY(op, type_min, type_max, is_floating)                  \
    ((op) == SEGMENT_OP_SUM   ? (is_floating ? 0.0 : 0)                        \
     : (op) == SEGMENT_OP_MIN ? type_max                                       \
     : (op) == SEGMENT_OP_MAX ? type_min                                       \
                              : (is_floating ? 0.0 : 0))

/* ====================================================================
 * STRUCTURE DEFINITION MACROS
 * ==================================================================== */

/* Small tier structure - contiguous allocation with flexible array */
#define SEGMENT_DEFINE_SMALL_STRUCT(suffix, value_type, index_type)            \
    typedef struct SEGMENT_SMALL_TYPENAME(suffix) {                            \
        index_type count;    /* Number of leaf elements */                     \
        index_type capacity; /* Total tree capacity (power of 2) */            \
        segmentOp operation; /* Query operation type (MIN/MAX/SUM) */          \
        value_type tree[];   /* Flexible array: segment tree nodes */          \
    } SEGMENT_SMALL_TYPENAME(suffix);

/* Full tier structure - separate allocation with lazy propagation */
#define SEGMENT_DEFINE_FULL_STRUCT(suffix, value_type, index_type)             \
    typedef struct SEGMENT_FULL_TYPENAME(suffix) {                             \
        index_type count;       /* Number of leaf elements */                  \
        index_type capacity;    /* Total tree capacity (power of 2) */         \
        index_type maxCapacity; /* Maximum capacity before overflow */         \
        segmentOp operation;    /* Query operation type */                     \
        value_type *tree;       /* Segment tree nodes (separate allocation) */ \
        value_type *lazy; /* Lazy propagation array (separate allocation) */   \
    } SEGMENT_FULL_TYPENAME(suffix);

/* ====================================================================
 * FORWARD DECLARATIONS MACRO (2-TIER)
 * ==================================================================== */

#define SEGMENT_DECLARE_FUNCTIONS(suffix, value_type, index_type_small,        \
                                  index_type_full)                             \
    /* Small tier */                                                           \
    SEGMENT_SMALL_TYPENAME(suffix) *                                           \
        SEGMENT_NAME(suffix, SmallNew)(segmentOp op);                          \
    SEGMENT_SMALL_TYPENAME(suffix) *                                           \
        SEGMENT_NAME(suffix, SmallNewFromArray)(                               \
            const value_type *values, index_type_small count, segmentOp op);   \
    void SEGMENT_NAME(suffix,                                                  \
                      SmallFree)(SEGMENT_SMALL_TYPENAME(suffix) * seg);        \
    SEGMENT_SMALL_TYPENAME(suffix) * SEGMENT_NAME(suffix, SmallUpdate)(        \
                                         SEGMENT_SMALL_TYPENAME(suffix) * seg, \
                                         index_type_small idx,                 \
                                         value_type value, bool *success);     \
    value_type SEGMENT_NAME(suffix, SmallQuery)(                               \
        const SEGMENT_SMALL_TYPENAME(suffix) * seg, index_type_small left,     \
        index_type_small right);                                               \
    value_type SEGMENT_NAME(suffix, SmallGet)(                                 \
        const SEGMENT_SMALL_TYPENAME(suffix) * seg, index_type_small idx);     \
    index_type_small SEGMENT_NAME(suffix, SmallCount)(                         \
        const SEGMENT_SMALL_TYPENAME(suffix) * seg);                           \
    size_t SEGMENT_NAME(suffix, SmallBytes)(                                   \
        const SEGMENT_SMALL_TYPENAME(suffix) * seg);                           \
    bool SEGMENT_NAME(suffix, SmallShouldUpgrade)(                             \
        const SEGMENT_SMALL_TYPENAME(suffix) * seg);                           \
                                                                               \
    /* Full tier (2-TIER: no Medium) */                                        \
    SEGMENT_FULL_TYPENAME(suffix) *                                            \
        SEGMENT_NAME(suffix, FullNew)(segmentOp op);                           \
    SEGMENT_FULL_TYPENAME(suffix) *                                            \
        SEGMENT_NAME(suffix, FullNewFromArray)(                                \
            const value_type *values, index_type_full count, segmentOp op);    \
    SEGMENT_FULL_TYPENAME(suffix) *                                            \
        SEGMENT_NAME(suffix,                                                   \
                     FullFromSmall)(SEGMENT_SMALL_TYPENAME(suffix) * small);   \
    void SEGMENT_NAME(suffix, FullFree)(SEGMENT_FULL_TYPENAME(suffix) * seg);  \
    SEGMENT_FULL_TYPENAME(suffix) *                                            \
        SEGMENT_NAME(suffix, FullUpdate)(SEGMENT_FULL_TYPENAME(suffix) * seg,  \
                                         index_type_full idx,                  \
                                         value_type value, bool *success);     \
    value_type SEGMENT_NAME(suffix, FullQuery)(                                \
        const SEGMENT_FULL_TYPENAME(suffix) * seg, index_type_full left,       \
        index_type_full right);                                                \
    value_type SEGMENT_NAME(suffix, FullGet)(                                  \
        const SEGMENT_FULL_TYPENAME(suffix) * seg, index_type_full idx);       \
    index_type_full SEGMENT_NAME(suffix, FullCount)(                           \
        const SEGMENT_FULL_TYPENAME(suffix) * seg);                            \
    size_t SEGMENT_NAME(suffix,                                                \
                        FullBytes)(const SEGMENT_FULL_TYPENAME(suffix) * seg); \
    void SEGMENT_NAME(suffix, FullRangeUpdate)(                                \
        SEGMENT_FULL_TYPENAME(suffix) * seg, index_type_full left,             \
        index_type_full right, value_type value);

/* ====================================================================
 * TEST FUNCTION DECLARATIONS
 * ==================================================================== */

#ifdef DATAKIT_TEST
#define SEGMENT_DECLARE_TEST_FUNCTIONS(suffix, value_type)                     \
    void SEGMENT_NAME(suffix,                                                  \
                      SmallRepr)(const SEGMENT_SMALL_TYPENAME(suffix) * seg);  \
    void SEGMENT_NAME(suffix,                                                  \
                      FullRepr)(const SEGMENT_FULL_TYPENAME(suffix) * seg);
#else
#define SEGMENT_DECLARE_TEST_FUNCTIONS(suffix, value_type)
#endif

/* ====================================================================
 * COMPLETE TYPE INSTANTIATION MACRO (2-TIER)
 * ==================================================================== */

#define SEGMENT_DECLARE_TYPE(suffix, value_type, index_type_small,             \
                             index_type_full, small_max, type_min, type_max)   \
    SEGMENT_DEFINE_SMALL_STRUCT(suffix, value_type, index_type_small)          \
    SEGMENT_DEFINE_FULL_STRUCT(suffix, value_type, index_type_full)            \
    SEGMENT_DECLARE_FUNCTIONS(suffix, value_type, index_type_small,            \
                              index_type_full)                                 \
    SEGMENT_DECLARE_TEST_FUNCTIONS(suffix, value_type)
