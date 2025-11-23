#include "wordsize.h"
#include <stdint.h> /* provides uintptr_t */

#define _PTRLIB_GENMASK(maskbits) ((1ULL << (maskbits)) - 1)

/* ====================================================================
 * Low bits tagging (max == 4 bits)
 * ==================================================================== */
/* Store type info in the lower 1 to 4 bits of the pointer */
#if PTRLIB_BITS_LOW
#if (PTRLIB_BITS_LOW == 1 || PTRLIB_BITS_LOW == 2 || PTRLIB_BITS_LOW == 3) ||  \
    (PTRLIB_BITS_LOW == 4 && DK_WORDSIZE == 64) ||                             \
    PTRLIB_BITS_ALIGNMENT_CUSTOM
#define _PTRLIB_TAGGED_PTR_MASK (uintptr_t)_PTRLIB_GENMASK(PTRLIB_BITS_LOW)
#else
#error "PTRLIB_BITS_LOW must be <= 4 (64-bit) or <= 3 (32-bit)"
#endif

#define _PTRLIB_TYPE(ptr) ((uintptr_t)(ptr) & _PTRLIB_TAGGED_PTR_MASK)
#define _PTRLIB_USE(ptr) (void *)((uintptr_t)(ptr) & ~(_PTRLIB_TAGGED_PTR_MASK))
#define _PTRLIB_TAG(ptr, type) ((uintptr_t)(ptr) | (type))
#define _PTRLIB_RETAG(ptr, type) _PTRLIB_TAG(_PTRLIB_USE(ptr), type)

#endif /* PTRLIB_BITS_LOW */

/* ====================================================================
 * High bits tagging (max == 16 bits)
 * ==================================================================== */
/* Store any data in the upper 16 bits of a 64-bit pointer */
#if PTRLIB_BITS_HIGH && DK_WORDSIZE == 32
#error "Can't use high bit pointer tagging on 32-bit platforms"
#endif

#if PTRLIB_BITS_HIGH
#define _PTRLIB_EXTRA_USE 1

#define _PTRLIB_TOP_1_MASK (uintptr_t)_PTRLIB_GENMASK(1)
#define _PTRLIB_TOP_8_MASK (uintptr_t)_PTRLIB_GENMASK(8)
#define _PTRLIB_TOP_16_MASK (uintptr_t)_PTRLIB_GENMASK(16)

#define _PTRLIB_TOP_1_N_MASK(n) (uintptr_t)(_PTRLIB_TOP_1_MASK << (64 - (n)))
#define _PTRLIB_TOP_8_1_MASK (uintptr_t)(_PTRLIB_TOP_8_MASK << (64 - 16))
#define _PTRLIB_TOP_8_2_MASK (uintptr_t)(_PTRLIB_TOP_8_MASK << (64 - 8))
#define _PTRLIB_TOP_16_1_MASK (uintptr_t)(_PTRLIB_TOP_16_MASK << (64 - 16))

#define _PTRLIB_TOP_CLEAR_1_N(ptr, n)                                          \
    ((uintptr_t)(ptr) & ~(_PTRLIB_TOP_1_N_MASK(n)))
#define _PTRLIB_TOP_CLEAR_8_1(ptr) ((uintptr_t)(ptr) & ~(_PTRLIB_TOP_8_1_MASK))
#define _PTRLIB_TOP_CLEAR_8_2(ptr) ((uintptr_t)(ptr) & ~(_PTRLIB_TOP_8_2_MASK))
#define _PTRLIB_TOP_CLEAR_16(ptr) ((uintptr_t)(ptr) & ~(_PTRLIB_TOP_16_1_MASK))

#define _PTRLIB_TOP_USE(ptr) (void *)_PTRLIB_TOP_CLEAR_16(ptr)
#define _PTRLIB_TOP_USE_ALL(ptr) (void *)_PTRLIB_USE(_PTRLIB_TOP_CLEAR_16(ptr))

#define _PTRLIB_TOP_BOOL_N(ptr, n) (((uintptr_t)(ptr) >> (64 - (n))) & 0x01)
#define _PTRLIB_TOP_8_1(ptr) (((uintptr_t)(ptr) >> (64 - 16)) & 0xff)
#define _PTRLIB_TOP_8_2(ptr) ((uintptr_t)(ptr) >> (64 - 8))
#define _PTRLIB_TOP_16(ptr) ((uintptr_t)(ptr) >> (64 - 16))

#define _PTRLIB_TOP_SET_BOOL_N(ptr, n, val)                                    \
    (void *)((uintptr_t)(ptr) & | ((uintptr_t)(val)) << (64 - (n)))
#define _PTRLIB_TOP_SET_8_1(ptr, val)                                          \
    (void *)((uintptr_t)(ptr) | ((uintptr_t)(val)) << (64 - 16))
#define _PTRLIB_TOP_SET_8_2(ptr, val)                                          \
    (void *)((uintptr_t)(ptr) | ((uintptr_t)(val)) << (64 - 8))
#define _PTRLIB_TOP_SET_16(ptr, val)                                           \
    (void *)((uintptr_t)(ptr) | ((uintptr_t)(val)) << (64 - 16))

#define _PTRLIB_TOP_RESET_BOOL_N(ptr, n, val)                                  \
    _PTRLIB_TOP_SET_BOOL_N(_PTRLIB_TOP_CLEAR_1_N(ptr, n), n, val)
#define _PTRLIB_TOP_RESET_8_1(ptr, val)                                        \
    _PTRLIB_TOP_SET_8_1(_PTRLIB_TOP_CLEAR_8_1(ptr), val)
#define _PTRLIB_TOP_RESET_8_2(ptr, val)                                        \
    _PTRLIB_TOP_SET_8_2(_PTRLIB_TOP_CLEAR_8_2(ptr), val)
#define _PTRLIB_TOP_RESET_16(ptr, val)                                         \
    _PTRLIB_TOP_SET_16(_PTRLIB_TOP_CLEAR_16(ptr), val)

#endif /* PTRLIB_BITS_HIGH */
