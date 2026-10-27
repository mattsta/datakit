#pragma once

#include "dkC23.h" /* __STDC_ENDIAN_NATIVE__ */

/* Whether the host stores the low byte of a word first (C23 7.18.2), a
 * constant, so a branch on it costs nothing */
static inline bool endianIsLittle(void) {
    return __STDC_ENDIAN_NATIVE__ == __STDC_ENDIAN_LITTLE__;
}
