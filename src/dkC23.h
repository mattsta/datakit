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

DK_STDBIT_(uc, unsigned char, UCHAR_WIDTH)
DK_STDBIT_(us, unsigned short, USHRT_WIDTH)
DK_STDBIT_(ui, unsigned int, UINT_WIDTH)
DK_STDBIT_(ul, unsigned long, ULONG_WIDTH)
DK_STDBIT_(ull, unsigned long long, ULLONG_WIDTH)
#undef DK_STDBIT_

#define DK_STDBIT_GENERIC_(fn, x)                                              \
    _Generic((x),                                                              \
        unsigned char: fn##_uc,                                                \
        unsigned short: fn##_us,                                               \
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

/* stdc_rotate_left / stdc_rotate_right (value, count): the next C
 * standard's <stdbit.h> rotates, until the library ships them: the
 * compiler's builtin where it has one, else one function per width (the
 * shift pair each compiles to a single rotate instruction).  The result has
 * the value's unsigned type and the count is taken modulo its width, so a
 * count of 0 or the full width returns the value. */
#ifndef stdc_rotate_left
#if __has_builtin(__builtin_stdc_rotate_left) &&                               \
    __has_builtin(__builtin_stdc_rotate_right)
#define stdc_rotate_left(x, n) __builtin_stdc_rotate_left(x, n)
#define stdc_rotate_right(x, n) __builtin_stdc_rotate_right(x, n)
#else
#define DK_STDBIT_ROTATE_(suffix, T, width)                                    \
    static inline T stdc_rotate_left_##suffix(T x, unsigned int n) {           \
        n %= (width);                                                          \
        return (T)((T)(x << n) | (T)(x >> (((width) - n) % (width))));         \
    }                                                                          \
    static inline T stdc_rotate_right_##suffix(T x, unsigned int n) {          \
        n %= (width);                                                          \
        return (T)((T)(x >> n) | (T)(x << (((width) - n) % (width))));         \
    }

DK_STDBIT_ROTATE_(uc, unsigned char, UCHAR_WIDTH)
DK_STDBIT_ROTATE_(us, unsigned short, USHRT_WIDTH)
DK_STDBIT_ROTATE_(ui, unsigned int, UINT_WIDTH)
DK_STDBIT_ROTATE_(ul, unsigned long, ULONG_WIDTH)
DK_STDBIT_ROTATE_(ull, unsigned long long, ULLONG_WIDTH)
#undef DK_STDBIT_ROTATE_

#define DK_STDBIT_ROTATE_GENERIC_(fn, x, n)                                    \
    _Generic((x),                                                              \
        unsigned char: fn##_uc,                                                \
        unsigned short: fn##_us,                                               \
        unsigned int: fn##_ui,                                                 \
        unsigned long: fn##_ul,                                                \
        unsigned long long: fn##_ull)((x), (unsigned int)(n))

#define stdc_rotate_left(x, n) DK_STDBIT_ROTATE_GENERIC_(stdc_rotate_left, x, n)
#define stdc_rotate_right(x, n)                                                \
    DK_STDBIT_ROTATE_GENERIC_(stdc_rotate_right, x, n)
#endif
#endif

/* __STDC_ENDIAN_NATIVE__ and its two values (C23 7.18.2), from the
 * compiler's byte-order macros where the library lacks <stdbit.h> */
#ifndef __STDC_ENDIAN_NATIVE__
#define __STDC_ENDIAN_LITTLE__ __ORDER_LITTLE_ENDIAN__
#define __STDC_ENDIAN_BIG__ __ORDER_BIG_ENDIAN__
#define __STDC_ENDIAN_NATIVE__ __BYTE_ORDER__
#endif

/* countof(array): its number of elements (the next C standard's
 * <stdcountof.h>, here until every compiler ships it). A pointer is
 * refused at compile time, where sizeof(p) / sizeof(*p) would quietly
 * count the pointer's bytes. */
#if __has_include(<stdcountof.h>)
#include <stdcountof.h>
#else
#define countof(a) (sizeof(a) / sizeof((a)[0]) + DK_COUNTOF_ARRAY_(a))
/* 0, or for a pointer a compile error ("negative array size": countof was
 * handed a pointer). No type is defined, so countof stays usable in a
 * constexpr initializer, which C23 forbids to define one. */
#define DK_COUNTOF_ARRAY_(a)                                                   \
    (0 * sizeof(char[1 - 2 * __builtin_types_compatible_p(typeof(a),           \
                                                          typeof(&(a)[0]))]))
#endif

#ifdef DATAKIT_TEST
int dkC23Test(int argc, char *argv[]);
#endif
