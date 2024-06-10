#pragma once

#include "config.h"
#include "endianIsLittle.h"

#if DK_GNUC_LT(4, 3, 0) && !DK_CLANG_VERSION_CODE
#warning "Your old compiler (probably) doesn't have __builtin_bswap()."
#endif

/* Note: If your platform *is* little endian, your compiler will detect
 *       it at compile time and remove all these macro bodies since nothing
 *       would happen on your platform.  */
#if 1
/* -fstrict-aliasing */
#define _conformToLittleEndian(size, what)                                     \
    do {                                                                       \
        if (!endianIsLittle()) {                                               \
            uint##size##_t holder = 0;                                         \
            memcpy(&holder, &(what), sizeof(holder));                          \
            holder = __builtin_bswap##size(what);                              \
            memcpy(&(what), &holder, sizeof(holder));                          \
        }                                                                      \
    } while (0)
#else
/* -fno-strict-aliasing */
/* this version breaks strict aliasing rules (and 'what' is a pointer here) */
#define _conformToLittleEndian(size, what)                                     \
    if (!endianIsLittle()) {                                                   \
        *(what) = __builtin_bswap##size(*(what));                              \
    }                                                                          \
    }                                                                          \
    while (0)
#endif

#define conformToLittleEndian16(e) _conformToLittleEndian(16, e)
#define conformToLittleEndian32(e) _conformToLittleEndian(32, e)
#define conformToLittleEndian64(e) _conformToLittleEndian(64, e)
