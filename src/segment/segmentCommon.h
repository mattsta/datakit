#pragma once

/* Common types, enums, and macros for the tiered Segment tree system
 *
 * Segment trees support efficient range queries (min, max, sum) and
 * range updates with lazy propagation.
 */

#include <stdbool.h>
#include <stdint.h>

/* Operation type for range queries */
typedef enum segmentOp {
    SEGMENT_OP_SUM = 0, /* Range sum query */
    SEGMENT_OP_MIN = 1, /* Range minimum query */
    SEGMENT_OP_MAX = 2, /* Range maximum query */
} segmentOp;

/* Helper: compute tree size for n elements (4n nodes for segment tree) */
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
