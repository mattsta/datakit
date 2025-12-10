#pragma once

/* Common utilities for the Fenwick tree system (2-TIER ARCHITECTURE)
 *
 * Provides helper functions used by all Fenwick tree implementations.
 * The old 3-tier runtime type checking macros have been removed.
 */

#include <stdbool.h>
#include <stdint.h>

/* Inline helper: isolate least significant bit (LSB)
 * This is the core trick of Fenwick trees: x & -x gives 2^r where r is the
 * position of the rightmost 1-bit.
 * Works via two's complement: -x = ~x + 1 = a1b̄ + 1 = ā1b where b are all 0s
 * So x & -x = a1b & ā1b = 1b (only the LSB survives)
 */
static inline uint64_t fenwickLSB(uint64_t x) {
    return x & (-(int64_t)x);
}

/* Inline helper: get parent index in BIT (move up tree)
 * Add LSB to current index */
static inline uint64_t fenwickParent(uint64_t idx) {
    return idx + fenwickLSB(idx);
}

/* Inline helper: get previous index in query (move down tree)
 * Subtract LSB from current index */
static inline uint64_t fenwickPrev(uint64_t idx) {
    return idx - fenwickLSB(idx);
}
