#pragma once

#include "endianIsLittle.h"

/* Byte-reverse 'what' in place on a big-endian host; on a little-endian one
 * endianIsLittle() is a constant and the body compiles away. */
#define _conformToLittleEndian(size, what)                                     \
    do {                                                                       \
        if (!endianIsLittle()) {                                               \
            uint##size##_t holder = 0;                                         \
            memcpy(&holder, &(what), sizeof(holder));                          \
            /* swap the reinterpreted BITS in holder, not `what` itself:       \
             * `what` may be a float/double the bswap intrinsic would          \
             * truncate to an integer value rather than byte-reverse. */       \
            holder = __builtin_bswap##size(holder);                            \
            memcpy(&(what), &holder, sizeof(holder));                          \
        }                                                                      \
    } while (0)

#define conformToLittleEndian16(e) _conformToLittleEndian(16, e)
#define conformToLittleEndian32(e) _conformToLittleEndian(32, e)
#define conformToLittleEndian64(e) _conformToLittleEndian(64, e)
