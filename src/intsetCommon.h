#pragma once

/* Common types, enums, and macros for the tiered intset system */

#include "intset.h" /* For intsetEnc enum */
#include <stdbool.h>
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
typedef enum intsetSearchResult {
    INTSET_FOUND = 0,     /* Value found at returned position */
    INTSET_NOT_FOUND = 1, /* Value not found, position is insert point */
} intsetSearchResult;
