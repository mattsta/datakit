#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
__BEGIN_DECLS

#ifndef __clang__
#define varint_fallthrough __attribute__((fallthrough))
#else
#define varint_fallthrough (void)0
#endif

/* ====================================================================
 * Common Data
 * ==================================================================== */
typedef enum varintWidth {
    VARINT_WIDTH_INVALID = 0,
    VARINT_WIDTH_8B = 1,
    VARINT_WIDTH_16B,
    VARINT_WIDTH_24B,
    VARINT_WIDTH_32B,
    VARINT_WIDTH_40B,
    VARINT_WIDTH_48B,
    VARINT_WIDTH_56B,
    VARINT_WIDTH_64B = 8,  /* external varints can use up to 8 bytes */
    VARINT_WIDTH_72B = 9,  /* tagged, chained, and split can use 9 bytes */
    VARINT_WIDTH_80B = 10, /* a naive chained can use 10 bytes */
    VARINT_WIDTH_88B,
    VARINT_WIDTH_96B,
    VARINT_WIDTH_104B,
    VARINT_WIDTH_112B,
    VARINT_WIDTH_120B,
    VARINT_WIDTH_128B = 16 /* 16 bytes should be enough for anybody */
} varintWidth;

typedef enum varintSplitFullStorage {
    VARINT_SPLIT_FULL_STORAGE_1 = (1 << 6) - 1,
    VARINT_SPLIT_FULL_STORAGE_2 =
        VARINT_SPLIT_FULL_STORAGE_1 + (1ULL << 14) - 1,
    VARINT_SPLIT_FULL_STORAGE_3 =
        VARINT_SPLIT_FULL_STORAGE_2 + (1ULL << 22) - 1,
    VARINT_SPLIT_FULL_STORAGE_4 =
        VARINT_SPLIT_FULL_STORAGE_3 + (1ULL << 24) - 1,
} varintSplitFullStorage;

#define VARINT_SPLIT_FULL_STORAGE_5                                            \
    (VARINT_SPLIT_FULL_STORAGE_3 + (1ULL << 32) - 1)
#define VARINT_SPLIT_FULL_STORAGE_6                                            \
    (VARINT_SPLIT_FULL_STORAGE_3 + (1ULL << 40) - 1)
#define VARINT_SPLIT_FULL_STORAGE_7                                            \
    (VARINT_SPLIT_FULL_STORAGE_3 + (1ULL << 48) - 1)
#define VARINT_SPLIT_FULL_STORAGE_8                                            \
    (VARINT_SPLIT_FULL_STORAGE_3 + (1ULL << 56) - 1)
#define VARINT_SPLIT_FULL_STORAGE_9 (UINT64_MAX)

typedef enum varintSplitFullNoZeroStorage {
    VARINT_SPLIT_FULL_NO_ZERO_STORAGE_1 = (1 << 6),
    VARINT_SPLIT_FULL_NO_ZERO_STORAGE_2 =
        VARINT_SPLIT_FULL_NO_ZERO_STORAGE_1 + (1ULL << 14) - 1,
    VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 =
        VARINT_SPLIT_FULL_NO_ZERO_STORAGE_2 + (1ULL << 22) - 1,
    VARINT_SPLIT_FULL_NO_ZERO_STORAGE_4 =
        VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 + (1ULL << 24) - 1,
} varintSplitFullNoZeroStorage;

#define VARINT_SPLIT_FULL_NO_ZERO_STORAGE_5                                    \
    (VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 + (1ULL << 32) - 1)
#define VARINT_SPLIT_FULL_NO_ZERO_STORAGE_6                                    \
    (VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 + (1ULL << 40) - 1)
#define VARINT_SPLIT_FULL_NO_ZERO_STORAGE_7                                    \
    (VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 + (1ULL << 48) - 1)
#define VARINT_SPLIT_FULL_NO_ZERO_STORAGE_8                                    \
    (VARINT_SPLIT_FULL_NO_ZERO_STORAGE_3 + (1ULL << 56) - 1)
#define VARINT_SPLIT_FULL_NO_ZERO_STORAGE_9 (UINT64_MAX)

/* define a fake __has_builtin() so GCC doesn't complain */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* GCC added this in gcc-5 (2015), clang added it in 2013.
 * Clang has a simple builtin detector macro, but gcc doesn't, so we need
 * to combine both version and builtin detection. */
#if __GNUC__ > 5 || __has_builtin(__builtin_saddll_overflow)
#define VARINT_ADD_OR_ABORT_OVERFLOW_(updatingVal, add, newVal)                \
    do {                                                                       \
        if (__builtin_saddll_overflow((updatingVal), (add), &(newVal)) == 0) { \
            return VARINT_WIDTH_INVALID;                                       \
        }                                                                      \
    } while (0)
#else
#define VARINT_ADD_OR_ABORT_OVERFLOW_(updatingVal, add, newVal)                \
    do {                                                                       \
        if (((updatingVal) < 0 && (add) < (INT64_MIN - (updatingVal))) ||      \
            ((updatingVal) > 0 && (add) > (INT64_MAX - (updatingVal)))) {      \
            return VARINT_WIDTH_INVALID;                                       \
        }                                                                      \
        (newVal) = (updatingVal) + (add);                                      \
    } while (0)
#endif

__END_DECLS
