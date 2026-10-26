#pragma once

/* ====================================================================
 * Adaptive Fenwick cell store (multiFenwickFlat, multiFenwick2d)
 * ====================================================================
 *
 * Flat arrays of cells, O(1) per access.  Each cell holds 64 bits: an
 * int64, a uint64 or a double, told apart by a one-byte kind.  The kinds
 * array exists only once some cell needs a kind other than SIGNED (a
 * value above INT64_MAX, or a double): until then every cell is an int64,
 * the store costs 8 bytes per cell, and owners may take integer-only fast
 * paths (multiFenwickCellsAllInt).  SIGNED is kind 0, so zeroed bits and
 * zeroed kinds both read as the integer 0.
 *
 * The owner keeps the cell count; every function that allocates takes it.
 * Arithmetic is multiFenwickNum (multiFenwickCommon.h).  OOM is
 * fail-fast. */

#include "datakit.h"
#include "multiFenwickCommon.h"

#include <string.h>

typedef enum multiFenwickCellKind : uint8_t {
    MFWC_SIGNED = 0,
    MFWC_UNSIGNED = 1,
    MFWC_DOUBLE = 2,
} multiFenwickCellKind;

typedef struct multiFenwickCells {
    uint64_t *bits; /* cell values: int64, uint64 or double bits */
    uint8_t *kinds; /* multiFenwickCellKind per cell; NULL: all SIGNED */
} multiFenwickCells;

/* One encoded cell value, ready to store */
typedef struct multiFenwickCell {
    uint64_t bits;
    multiFenwickCellKind kind;
} multiFenwickCell;

/* True while every cell is an int64 (read them with multiFenwickCellsInt) */
static inline bool multiFenwickCellsAllInt(const multiFenwickCells *c) {
    return !c->kinds;
}

static inline int64_t multiFenwickCellsInt(const multiFenwickCells *c,
                                           uint64_t pos) {
    return (int64_t)c->bits[pos];
}

static inline multiFenwickNum multiFenwickCellsGet(const multiFenwickCells *c,
                                                   uint64_t pos) {
    const uint64_t bits = c->bits[pos];
    switch (c->kinds ? (multiFenwickCellKind)c->kinds[pos] : MFWC_SIGNED) {
    case MFWC_UNSIGNED:
        return (multiFenwickNum){.i = bits};
    case MFWC_DOUBLE: {
        double d;
        memcpy(&d, &bits, sizeof(d));
        return (multiFenwickNum){.d = d, .isFloat = true};
    }
    case MFWC_SIGNED:
    default:
        return (multiFenwickNum){.i = (int64_t)bits};
    }
}

/* Encode 'n' as a cell; false (nothing written) when no cell holds it: an
 * integer outside [INT64_MIN, UINT64_MAX] or a non-finite float */
static inline bool multiFenwickCellEncode(multiFenwickNum n,
                                          multiFenwickCell *out) {
    databox box;
    if (!multiFenwickNumBox_(n, &box)) {
        return false;
    }

    switch (box.type) {
    case DATABOX_DOUBLE_64:
        memcpy(&out->bits, &box.data.d64, sizeof(out->bits));
        out->kind = MFWC_DOUBLE;
        break;
    case DATABOX_SIGNED_64:
        out->bits = (uint64_t)box.data.i64;
        out->kind = MFWC_SIGNED;
        break;
    case DATABOX_UNSIGNED_64:
    default:
        out->bits = box.data.u64;
        out->kind = box.data.u64 > INT64_MAX ? MFWC_UNSIGNED : MFWC_SIGNED;
        break;
    }

    return true;
}

/* Store 'cell' at 'pos' of a store of 'count' cells.  The first cell that
 * is not an int64 creates the kinds array (all SIGNED, which is what
 * every cell so far is). */
static inline void multiFenwickCellsPut(multiFenwickCells *c, uint64_t count,
                                        uint64_t pos, multiFenwickCell cell) {
    c->bits[pos] = cell.bits;
    if (cell.kind != MFWC_SIGNED && !c->kinds) {
        /* @own: container; free=zfree */
        c->kinds = zcalloc(count, sizeof(*c->kinds));
    }

    if (c->kinds) {
        c->kinds[pos] = cell.kind;
    }
}

/* Store the int64 'v' at 'pos' of an all-int64 store */
static inline void multiFenwickCellsPutInt(multiFenwickCells *c, uint64_t pos,
                                           int64_t v) {
    c->bits[pos] = (uint64_t)v;
}

static inline void multiFenwickCellsCopy(multiFenwickCells *c, uint64_t dst,
                                         uint64_t src) {
    c->bits[dst] = c->bits[src];
    if (c->kinds) {
        c->kinds[dst] = c->kinds[src];
    }
}

/* Grow a store of 'oldCount' cells to 'newCount', the new cells zero */
static inline void multiFenwickCellsGrow(multiFenwickCells *c,
                                         uint64_t oldCount, uint64_t newCount) {
    /* @own: container; free=zfree */
    c->bits = zrealloc(c->bits, newCount * sizeof(*c->bits));
    memset(&c->bits[oldCount], 0, (newCount - oldCount) * sizeof(*c->bits));
    if (c->kinds) {
        /* @own: container; free=zfree */
        c->kinds = zrealloc(c->kinds, newCount * sizeof(*c->kinds));
        memset(&c->kinds[oldCount], 0,
               (newCount - oldCount) * sizeof(*c->kinds));
    }
}

/* Re-lay a row-major oldR x oldC array of 'elem'-byte cells out as
 * newR x newC (each at least the old one), the new cells zero */
static inline void multiFenwickCellsRelayoutArray_(void **arr, size_t elem,
                                                   uint64_t oldR, uint64_t oldC,
                                                   uint64_t newR,
                                                   uint64_t newC) {
    if (newC == oldC) {
        /* @own: container; free=zfree */
        *arr = zrealloc(*arr, newR * newC * elem);
        memset((uint8_t *)*arr + oldR * oldC * elem, 0,
               (newR - oldR) * newC * elem);
        return;
    }

    /* @own: container; free=zfree */
    uint8_t *fresh = zcalloc(newR * newC, elem);
    for (uint64_t r = 0; r < oldR; r++) {
        memcpy(fresh + r * newC * elem, (uint8_t *)*arr + r * oldC * elem,
               oldC * elem);
    }

    zfree(*arr);
    *arr = fresh;
}

/* Re-lay a row-major oldR x oldC store out as newR x newC */
static inline void multiFenwickCellsRelayout(multiFenwickCells *c,
                                             uint64_t oldR, uint64_t oldC,
                                             uint64_t newR, uint64_t newC) {
    void *bits = c->bits;
    multiFenwickCellsRelayoutArray_(&bits, sizeof(*c->bits), oldR, oldC, newR,
                                    newC);
    c->bits = bits;
    if (c->kinds) {
        void *kinds = c->kinds;
        multiFenwickCellsRelayoutArray_(&kinds, sizeof(*c->kinds), oldR, oldC,
                                        newR, newC);
        c->kinds = kinds;
    }
}

/* Every cell becomes the int64 0; the kinds array is dropped */
static inline void multiFenwickCellsClear(multiFenwickCells *c,
                                          uint64_t count) {
    if (c->bits) {
        memset(c->bits, 0, count * sizeof(*c->bits));
    }

    zfree(c->kinds);
    c->kinds = nullptr;
}

static inline size_t multiFenwickCellsBytes(const multiFenwickCells *c,
                                            uint64_t count) {
    return count * sizeof(*c->bits) +
           (c->kinds ? count * sizeof(*c->kinds) : 0);
}

static inline void multiFenwickCellsFree(multiFenwickCells *c) {
    zfree(c->bits);
    zfree(c->kinds);
    c->bits = nullptr;
    c->kinds = nullptr;
}
