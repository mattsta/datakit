#pragma once

/* How far one write may grow a dense indexed array (the fenwick, segment
 * and multiFenwick trees: arrays as long as their largest index).  A write
 * may grow the array by one doubling, or to any size up to
 * DK_DENSE_JUMP_CELLS cells; a write past that is refused, so an index far
 * past the end never allocates everything up to it.  Appending, or writing
 * anywhere below twice the current size, is never refused. */

#include <stdint.h>

static constexpr uint64_t DK_DENSE_JUMP_CELLS = UINT64_C(1) << 22;

/* One dimension's growth: at most one doubling */
static inline bool dkDenseDoubling(uint64_t cells, uint64_t newCells) {
    return newCells / 2 <= cells;
}

static inline bool dkDenseGrowthAllowed(uint64_t cells, uint64_t newCells) {
    return newCells <= DK_DENSE_JUMP_CELLS || dkDenseDoubling(cells, newCells);
}

/* Whether a write at 'idx' may go to an array of 'cells' (a power of two,
 * or 0) that grows to the power of two above idx: the test a tree makes
 * before it creates or moves anything for the write */
static inline bool dkDenseIndexAllowed(uint64_t cells, uint64_t idx) {
    if (idx < cells) {
        return true;
    }

    if (idx >= UINT64_C(1) << 62) {
        return false;
    }

    uint64_t newCells = cells ? cells : 1;
    while (newCells <= idx) {
        newCells <<= 1;
    }
    return dkDenseGrowthAllowed(cells, newCells);
}
