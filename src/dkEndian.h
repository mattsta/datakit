#pragma once

/* Fixed-width integers to and from bytes in a stated order, at any
 * alignment.  Each is a memcpy of the word plus a byte swap that exists
 * only when the host order differs, so on every target it compiles to
 * one load or store (with a byte-reverse instruction for the other
 * order).  Formats, digests and wire encodings name their byte order
 * with these instead of shifting bytes by hand. */

#include "dkC23.h" /* __STDC_ENDIAN_NATIVE__ */

#include <stdint.h>
#include <string.h>

#define DK_ENDIAN_ACCESS_(width)                                               \
    static inline uint##width##_t dkLoadLe##width(const void *p) {             \
        uint##width##_t v;                                                     \
        memcpy(&v, p, sizeof(v));                                              \
        if (__STDC_ENDIAN_NATIVE__ != __STDC_ENDIAN_LITTLE__) {                \
            v = __builtin_bswap##width(v);                                     \
        }                                                                      \
        return v;                                                              \
    }                                                                          \
    static inline uint##width##_t dkLoadBe##width(const void *p) {             \
        uint##width##_t v;                                                     \
        memcpy(&v, p, sizeof(v));                                              \
        if (__STDC_ENDIAN_NATIVE__ != __STDC_ENDIAN_BIG__) {                   \
            v = __builtin_bswap##width(v);                                     \
        }                                                                      \
        return v;                                                              \
    }                                                                          \
    static inline void dkStoreLe##width(void *p, uint##width##_t v) {          \
        if (__STDC_ENDIAN_NATIVE__ != __STDC_ENDIAN_LITTLE__) {                \
            v = __builtin_bswap##width(v);                                     \
        }                                                                      \
        memcpy(p, &v, sizeof(v));                                              \
    }                                                                          \
    static inline void dkStoreBe##width(void *p, uint##width##_t v) {          \
        if (__STDC_ENDIAN_NATIVE__ != __STDC_ENDIAN_BIG__) {                   \
            v = __builtin_bswap##width(v);                                     \
        }                                                                      \
        memcpy(p, &v, sizeof(v));                                              \
    }

DK_ENDIAN_ACCESS_(16)
DK_ENDIAN_ACCESS_(32)
DK_ENDIAN_ACCESS_(64)
#undef DK_ENDIAN_ACCESS_

#ifdef DATAKIT_TEST
int dkEndianTest(int argc, char *argv[]);
#endif
