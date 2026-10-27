#pragma once

/* Bit arrays held in bytes or 64-bit words: bit i is bit (i % width) of
 * element i / width, least significant first.  dkBitGet / dkBitSet /
 * dkBitClear take either element type and pick the matching form, so a
 * site names the operation and the array, not the shift and mask. */

#include <stddef.h>
#include <stdint.h>

static inline bool dkBitGetU8_(const uint8_t *a, size_t i) {
    return (a[i >> 3] >> (i & 7)) & 1;
}
static inline void dkBitSetU8_(uint8_t *a, size_t i) {
    a[i >> 3] |= (uint8_t)(1u << (i & 7));
}
static inline void dkBitClearU8_(uint8_t *a, size_t i) {
    a[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static inline bool dkBitGetU64_(const uint64_t *a, size_t i) {
    return (a[i >> 6] >> (i & 63)) & 1;
}
static inline void dkBitSetU64_(uint64_t *a, size_t i) {
    a[i >> 6] |= UINT64_C(1) << (i & 63);
}
static inline void dkBitClearU64_(uint64_t *a, size_t i) {
    a[i >> 6] &= ~(UINT64_C(1) << (i & 63));
}

#define dkBitGet(a, i)                                                         \
    _Generic((a),                                                              \
        uint8_t *: dkBitGetU8_,                                                \
        const uint8_t *: dkBitGetU8_,                                          \
        uint64_t *: dkBitGetU64_,                                              \
        const uint64_t *: dkBitGetU64_)((a), (i))
#define dkBitSet(a, i)                                                         \
    _Generic((a), uint8_t *: dkBitSetU8_, uint64_t *: dkBitSetU64_)((a), (i))
#define dkBitClear(a, i)                                                       \
    _Generic((a), uint8_t *: dkBitClearU8_, uint64_t *: dkBitClearU64_)((a),   \
                                                                        (i))

/* `a` / `b` rounded up: the units of `b` that hold `a` (bits in bytes,
 * bits in words, items in chunks).  No overflow near the type's top, as
 * (a + b - 1) / b would have, and a constant expression when both are.
 * Each argument is evaluated twice. */
#define dkDivCeil(a, b) ((a) / (b) + ((a) % (b) != 0))

/* The bytes / 64-bit words that hold `bits` bits (no overflow near
 * SIZE_MAX) */
static inline size_t dkBitBytes(size_t bits) {
    return bits / 8 + (bits % 8 != 0);
}
static inline size_t dkBitWords(size_t bits) {
    return bits / 64 + (bits % 64 != 0);
}

#ifdef DATAKIT_TEST
int dkBitsTest(int argc, char *argv[]);
#endif
