#pragma once

#include "datakit.h"

/* The smallest power of two >= x, with 0 and 1 giving 2 and a ceiling past
 * 2^63 giving 0 (C23 leaves stdc_bit_ceil unspecified there, so that case
 * is decided here) */
DK_FN_UNUSED DK_FN_PURE DK_INLINE_ALWAYS uint64_t pow2Ceiling64(uint64_t x) {
    if (unlikely(x <= 1)) {
        return 2;
    }

    if (unlikely(x > (UINT64_C(1) << 63))) {
        return 0;
    }

    return stdc_bit_ceil(x);
}

/* A compile-time version of lg_floor and lg_ceil. */
#define LG_FLOOR_1(x) 0
#define LG_FLOOR_2(x) (x < (1ULL << 1) ? LG_FLOOR_1(x) : 1 + LG_FLOOR_1(x >> 1))
#define LG_FLOOR_4(x) (x < (1ULL << 2) ? LG_FLOOR_2(x) : 2 + LG_FLOOR_2(x >> 2))
#define LG_FLOOR_8(x) (x < (1ULL << 4) ? LG_FLOOR_4(x) : 4 + LG_FLOOR_4(x >> 4))
#define LG_FLOOR_16(x)                                                         \
    (x < (1ULL << 8) ? LG_FLOOR_8(x) : 8 + LG_FLOOR_8(x >> 8))
#define LG_FLOOR_32(x)                                                         \
    (x < (1ULL << 16) ? LG_FLOOR_16(x) : 16 + LG_FLOOR_16(x >> 16))
#define LG_FLOOR_64(x)                                                         \
    (x < (1ULL << 32) ? LG_FLOOR_32(x) : 32 + LG_FLOOR_32(x >> 32))

#define LG_CEIL(x) (LG_FLOOR(x) + (((x) & ((x) - 1)) == 0 ? 0 : 1))
