#pragma once

/* C23 library pieces a platform's C library may not ship yet, under their
 * standard names.  Where the library has the header, it is used as is; this
 * file adds nothing there.  Code uses the standard names only, so the shim
 * disappears once every supported libc ships them. */

#include <limits.h>
#include <stdint.h>

/* <stdint.h> width macros (C23 7.22.2), where the library lacks them */
#ifndef UINT64_WIDTH
#define INT8_WIDTH 8
#define UINT8_WIDTH 8
#define INT16_WIDTH 16
#define UINT16_WIDTH 16
#define INT32_WIDTH 32
#define UINT32_WIDTH 32
#define INT64_WIDTH 64
#define UINT64_WIDTH 64
#endif
#ifndef SIZE_WIDTH
#define SIZE_WIDTH ULONG_WIDTH /* size_t is unsigned long on every target */
#endif

#if __has_include(<stdbit.h>)
#include <stdbit.h>
#else
/* <stdbit.h>: the subset datakit uses, per C23 7.18, over the compilers'
 * type-generic builtins (one instruction or close to it).  stdc_bit_ceil of
 * a value whose ceiling does not fit the type is 0 here (C23 leaves it
 * unspecified). */
#define DK_STDBIT_(suffix, T, width)                                           \
    static inline unsigned int stdc_leading_zeros_##suffix(T x) {              \
        return (unsigned int)__builtin_clzg(x, (int)(width));                  \
    }                                                                          \
    static inline unsigned int stdc_trailing_zeros_##suffix(T x) {             \
        return (unsigned int)__builtin_ctzg(x, (int)(width));                  \
    }                                                                          \
    static inline unsigned int stdc_count_ones_##suffix(T x) {                 \
        return (unsigned int)__builtin_popcountg(x);                           \
    }                                                                          \
    static inline bool stdc_has_single_bit_##suffix(T x) {                     \
        return __builtin_popcountg(x) == 1;                                    \
    }                                                                          \
    static inline unsigned int stdc_bit_width_##suffix(T x) {                  \
        return (unsigned int)(width) - stdc_leading_zeros_##suffix(x);         \
    }                                                                          \
    static inline T stdc_bit_floor_##suffix(T x) {                             \
        return x ? (T)((T)1 << (stdc_bit_width_##suffix(x) - 1)) : (T)0;       \
    }                                                                          \
    static inline T stdc_bit_ceil_##suffix(T x) {                              \
        if (x <= 1) {                                                          \
            return 1;                                                          \
        }                                                                      \
        const unsigned int w = stdc_bit_width_##suffix((T)(x - 1));            \
        return w < (unsigned int)(width) ? (T)((T)1 << w) : (T)0;              \
    }

DK_STDBIT_(ui, unsigned int, UINT_WIDTH)
DK_STDBIT_(ul, unsigned long, ULONG_WIDTH)
DK_STDBIT_(ull, unsigned long long, ULLONG_WIDTH)
#undef DK_STDBIT_

#define DK_STDBIT_GENERIC_(fn, x)                                              \
    _Generic((x),                                                              \
        unsigned int: fn##_ui,                                                 \
        unsigned long: fn##_ul,                                                \
        unsigned long long: fn##_ull)(x)

#define stdc_leading_zeros(x) DK_STDBIT_GENERIC_(stdc_leading_zeros, x)
#define stdc_trailing_zeros(x) DK_STDBIT_GENERIC_(stdc_trailing_zeros, x)
#define stdc_count_ones(x) DK_STDBIT_GENERIC_(stdc_count_ones, x)
#define stdc_has_single_bit(x) DK_STDBIT_GENERIC_(stdc_has_single_bit, x)
#define stdc_bit_width(x) DK_STDBIT_GENERIC_(stdc_bit_width, x)
#define stdc_bit_floor(x) DK_STDBIT_GENERIC_(stdc_bit_floor, x)
#define stdc_bit_ceil(x) DK_STDBIT_GENERIC_(stdc_bit_ceil, x)
#endif

#ifdef DATAKIT_TEST
int dkC23Test(int argc, char *argv[]);
#endif
