#pragma once

/* Adapted from jemalloc/include/jemalloc/internal/bit_util.h */
DK_FN_UNUSED DK_FN_PURE DK_INLINE_ALWAYS uint64_t pow2Ceiling64(uint64_t x) {
#if __amd64__
    if (unlikely(x <= 1)) {
        /* Let's make power of 2 ceiling of 0 and 1 == 2 */
        return 2;
    }

    size_t indexOfHighestBitSet;

    /* There is no cross-compiler intrinsic to do this,
     * so we need the asm directly.
     * On GCC 2017+ march=haswell+, builtin_clz generates bsrq,
     * but Clang generates lzvnt using builtin_clz which isn't
     * the same thing. */
    asm("bsrq %1, %0" : "=r"(indexOfHighestBitSet) : "r"(x - 1));

    return 1ULL << (indexOfHighestBitSet + 1);
#elif defined(__aarch64__)
    if (unlikely(x <= 1)) {
        /* Let's make power of 2 ceiling of 0 and 1 == 2 */
        return 2;
    }

    /* On ARM64, use CLZ (count leading zeros) instruction.
     * CLZ returns 0-63 for the number of leading zeros.
     * For x-1, if CLZ returns n, then the highest bit set is at position 63-n.
     * So we need to shift 1 by (64 - n) = (64 - CLZ(x-1)) positions.
     *
     * Note: For x > 2^63, the result would be 2^64 which overflows uint64_t.
     * We return 0 in this case to match overflow behavior. */
    const int leadingZeros = __builtin_clzll(x - 1);
    if (leadingZeros == 0) {
        /* Result would be 2^64, which overflows - return 0 */
        return 0;
    }

    return 1ULL << (64 - leadingZeros);
#else
#warning "Using unoptimized version..."
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return x;
#endif
}

/* We don't care about 32 bit targets currently */
#if 0
DK_FN_UNUSED DK_FN_PURE DK_INLINE_ALWAYS uint32_t
pow2Ceiling32(uint32_t x) {
#if __i386__
    if (unlikely(x <= 1)) {
        return 2;
    }

    size_t indexOfHighestBitSet;

    asm("bsr %1, %0" : "=r"(indexOfHighestBitSet) : "r"(x - 1));

    return 1U << (indexOfHighestBitSet + 1);
#else
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x++;
    return x;
#endif
}
#endif

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
