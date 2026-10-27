#include "dkBits.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h>

/* An array of element type T driven by random sets and clears against a
 * bool-per-bit oracle, read back through a mutable and a const pointer */
#define DKBITS_SWEEP(T, n)                                                     \
    do {                                                                       \
        T arr[(n) / 8 + 1] = {};                                               \
        bool oracle[n] = {};                                                   \
        uint64_t s = 0x5EED;                                                   \
        for (int op = 0; op < 4000; op++) {                                    \
            const size_t i = dkSplitmix64Next(&s) % (n);                       \
            if (dkSplitmix64Next(&s) & 1) {                                    \
                dkBitSet(arr, i);                                              \
                oracle[i] = true;                                              \
            } else {                                                           \
                dkBitClear(arr, i);                                            \
                oracle[i] = false;                                             \
            }                                                                  \
        }                                                                      \
        for (size_t i = 0; i < (n); i++) {                                     \
            const T *ro = arr;                                                 \
            if (dkBitGet(arr, i) != oracle[i] ||                               \
                dkBitGet(ro, i) != oracle[i]) {                                \
                ERR(#T " bit %zu", i);                                         \
            }                                                                  \
        }                                                                      \
    } while (0)

int dkBitsTest(int, char *[]) {
    int err = 0;

    TEST("byte and word bit arrays against a bool per bit") {
        DKBITS_SWEEP(uint8_t, 517);
        DKBITS_SWEEP(uint64_t, 517);
    }

    TEST("bit 0 is the low bit of element 0; bit 9 is bit 1 of byte 1") {
        uint8_t b[2] = {};
        dkBitSet(b, 0);
        dkBitSet(b, 9);
        uint64_t w[2] = {};
        dkBitSet(w, 65);
        if (b[0] != 1 || b[1] != 2 || w[0] != 0 || w[1] != 2) {
            ERR("bit placement: %u %u %" PRIu64, b[0], b[1], w[1]);
        }
    }

    TEST("sizes round up, without overflow at SIZE_MAX") {
        if (dkBitBytes(0) != 0 || dkBitBytes(1) != 1 || dkBitBytes(8) != 1 ||
            dkBitBytes(9) != 2 || dkBitWords(64) != 1 || dkBitWords(65) != 2 ||
            dkBitBytes(SIZE_MAX) != SIZE_MAX / 8 + 1 ||
            dkBitWords(SIZE_MAX) != SIZE_MAX / 64 + 1) {
            ERR("dkBitBytes / dkBitWords");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
