#pragma once

/* Common types and index helpers for the tiered Segment tree system
 *
 * Segment trees answer range queries (min, max, sum).  A range update
 * assigns its leaves and refreshes their ancestors: there is no lazy
 * state, so every node is always its children's fold.  Layout: leaves at
 * tree[leaves, 2 * leaves), node i's children at 2i and 2i + 1, the root
 * at 1.
 */

#include <stdbool.h>
#include <stdint.h>

/* Operation type for range queries */
typedef enum segmentOp {
    SEGMENT_OP_SUM = 0, /* Range sum query */
    SEGMENT_OP_MIN = 1, /* Range minimum query */
    SEGMENT_OP_MAX = 2, /* Range maximum query */
} segmentOp;

/* Tree slots for n leaves (n >= 1): twice the next power of two */
static inline uint64_t segmentTreeSize(uint64_t n) {
    /* Round up to next power of 2, then multiply by 2 for tree structure */
    uint64_t size = 1;
    while (size < n) {
        size <<= 1;
    }
    return size << 1;
}

/* Helper: get parent index */
static inline uint64_t segmentParent(uint64_t idx) {
    return idx >> 1;
}

/* Helper: get left child index */
static inline uint64_t segmentLeft(uint64_t idx) {
    return idx << 1;
}

/* Helper: get right child index */
static inline uint64_t segmentRight(uint64_t idx) {
    return (idx << 1) | 1;
}
