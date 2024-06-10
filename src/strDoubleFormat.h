#pragma once

#include <stddef.h> /* size_t */

/* ====================================================================
 * Layouts
 * ==================================================================== */
#define realConforms(expBits, fracBits, expCompare, fracCompare)               \
    (((expCompare) <= ((1 << (expBits)) - 1)) &&                               \
     ((fracCompare) <= ((1 << (fracBits)) - 1)))

/* Mini Float (8) */
#define realMiniConforms(exp, frac) realConforms(4, 3, exp, frac)

/* Half Float (16) */
#define realHalfConforms(exp, frac) realConforms(5, 10, exp, frac)

/* Float (32) */
#define realFloatConforms(exp, frac) realConforms(8, 23, exp, frac)

#define genRealDeconstruct(name, type, expBits, fracBits)                      \
    /* Little Endian IEEE Layout */                                            \
    typedef struct real##name##LayoutLittle {                                  \
        type fraction : fracBits;                                              \
        type exponent : expBits;                                               \
        type sign : 1;                                                         \
    } real##name##LayoutLittle;                                                \
                                                                               \
    /* Big Endian IEEE Layout */                                               \
    typedef struct real##name##LayoutBig {                                     \
        type sign : 1;                                                         \
        type exponent : expBits;                                               \
        type fraction : fracBits;                                              \
    } real##name##LayoutBig;

/* Manifest structs for deconstructing real components */
genRealDeconstruct(Mini, uint8_t, 4, 3);
genRealDeconstruct(Half, uint16_t, 5, 10);
genRealDeconstruct(Float, uint32_t, 8, 23);
genRealDeconstruct(Double, uint64_t, 11, 52);

size_t StrDoubleFormatToBufNice(void *buf, size_t len, double v);

#ifdef DATAKIT_TEST
int strDoubleFormatTest(int argc, char *argv[]);
#endif
