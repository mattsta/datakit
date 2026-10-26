#pragma once

/* Common types, enums, and macros for the tiered intset system */

#include "intset.h" /* For intsetEnc enum */
#include <stdint.h>

/* Tier type identifiers (stored in lower 2 bits of pointer) */
typedef enum intsetType {
    INTSET_TYPE_SMALL = 1,  /* int16_t only */
    INTSET_TYPE_MEDIUM = 2, /* int16_t and int32_t */
    INTSET_TYPE_FULL = 3,   /* int16_t, int32_t, and int64_t */
} intsetType;

/* Pointer tagging macros for tier type */
#define INTSET_TYPE_MASK 0x03
#define INTSET_TYPE(is) ((intsetType)((uintptr_t)(is) & INTSET_TYPE_MASK))
#define INTSET_UNTAG(is) ((void *)((uintptr_t)(is) & ~INTSET_TYPE_MASK))
#define INTSET_TAG(ptr, type) ((intset *)((uintptr_t)(ptr) | (type)))

/* Tier transition thresholds */
#define INTSET_SMALL_MAX_BYTES (64 * 1024)        /* 64KB */
#define INTSET_SMALL_MAX_COUNT (32 * 1024)        /* 32K elements */
#define INTSET_MEDIUM_MAX_BYTES (8 * 1024 * 1024) /* 8MB */
#define INTSET_MEDIUM_MAX_COUNT (2 * 1024 * 1024) /* 2M elements */

/* Determine encoding needed for a value */
static inline uint8_t intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX) {
        return INTSET_ENC_INT64;
    }
    if (v < INT16_MIN || v > INT16_MAX) {
        return INTSET_ENC_INT32;
    }
    return INTSET_ENC_INT16;
}

/* Check if value fits in specific encoding */
static inline bool intsetValueFitsInt16(int64_t v) {
    return v >= INT16_MIN && v <= INT16_MAX;
}

static inline bool intsetValueFitsInt32(int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX;
}

/* Binary search helper status codes */
typedef enum [[nodiscard]] intsetSearchResult {
    INTSET_FOUND = 0,     /* Value found at returned position */
    INTSET_NOT_FOUND = 1, /* Value not found, position is insert point */
} intsetSearchResult;

/* Branchless + prefetch lower-bound over a sorted, ascending, duplicate-free
 * signed array `arr[0..n)` (n >= 1). Sets outIdx = count of elements strictly
 * less than v (= the lower bound / insertion point), and outHit = (arr[outIdx]
 * == v). The `?:` compiles to a conditional-move on clang/gcc (no data branch),
 * and the two prefetches hide the cache-miss tail — measured 1.3-2.1x faster
 * than the branchy search across all sizes (intsetSearchBench). Comparisons use
 * the element type T so signedness matches the original. Multi-evaluates
 * arr/n/v (callers pass plain lvalues, no side effects). */
#define INTSET_BRANCHLESS_LOWER_BOUND(T, arr, n, v, outIdx, outHit)            \
    do {                                                                       \
        const T *_b = (arr);                                                   \
        uint64_t _len = (uint64_t)(n);                                         \
        while (_len > 1) {                                                     \
            const uint64_t _half = _len >> 1;                                  \
            __builtin_prefetch(_b + (_half >> 1));                             \
            __builtin_prefetch(_b + _half + (_half >> 1));                     \
            _b += (_b[_half - 1] < (v)) ? _half : 0;                           \
            _len -= _half;                                                     \
        }                                                                      \
        uint64_t _idx = (uint64_t)(_b - (arr));                                \
        _idx += (_b[0] < (v)) ? 1 : 0;                                         \
        (outIdx) = _idx;                                                       \
        (outHit) = (_idx < (uint64_t)(n)) && ((arr)[_idx] == (v));             \
    } while (0)
