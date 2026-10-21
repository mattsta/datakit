/* ====================================================================
 * Fenwick Tree Shared Algorithms (both tiers)
 * ====================================================================
 *
 * Included by fenwickCoreImpl.h (Small tier) and fenwickCoreFullImpl.h
 * (Full tier) with the instantiation's FENWICK_SUFFIX,
 * FENWICK_VALUE_TYPE and FENWICK_IS_FLOATING defined.  Every function is
 * static to the including file and works on a 1-based BIT stored in
 * tree[0, capacity), capacity a power of two at or above the count.
 *
 * Arithmetic: integer cells add and subtract with wrapping (through
 * __builtin_add_overflow / __builtin_sub_overflow, so never signed
 * overflow UB, at every width including 128 bits).  Fenwick sums are
 * linear, so every sum computed from the cells is the exact sum modulo
 * 2^bits: exact whenever the true sum fits the type.  Elements, the
 * stored values, never wrap: AlgoNewElement_ refuses an element outside
 * the type (or, for floats, a non-finite one). */

#include <math.h>

#define FENWICK_ALGO_(name) FENWICK_NAME(FENWICK_SUFFIX, name)

static inline FENWICK_VALUE_TYPE FENWICK_ALGO_(AlgoAdd_)(FENWICK_VALUE_TYPE a,
                                                         FENWICK_VALUE_TYPE b) {
#if FENWICK_IS_FLOATING
    return a + b;
#else
    FENWICK_VALUE_TYPE r;
    (void)__builtin_add_overflow(a, b, &r); /* wraps by design */
    return r;
#endif
}

static inline FENWICK_VALUE_TYPE FENWICK_ALGO_(AlgoSub_)(FENWICK_VALUE_TYPE a,
                                                         FENWICK_VALUE_TYPE b) {
#if FENWICK_IS_FLOATING
    return a - b;
#else
    FENWICK_VALUE_TYPE r;
    (void)__builtin_sub_overflow(a, b, &r); /* wraps by design */
    return r;
#endif
}

/* True for a value a tree may hold: any integer; a finite float */
static inline bool FENWICK_ALGO_(AlgoValid_)(FENWICK_VALUE_TYPE v) {
#if FENWICK_IS_FLOATING
    return isfinite(v);
#else
    (void)v;
    return true;
#endif
}

/* Smallest power of two, at least 'current' (and at least 1), above 'idx' */
static inline uint64_t FENWICK_ALGO_(AlgoCapacityFor_)(uint64_t current,
                                                       uint64_t idx) {
    uint64_t capacity = current == 0 ? 1 : current;
    while (capacity <= idx) {
        capacity <<= 1;
    }

    return capacity;
}

/* Sum of elements [0, idx] */
static inline FENWICK_VALUE_TYPE
FENWICK_ALGO_(AlgoPrefix_)(const FENWICK_VALUE_TYPE *tree, uint64_t idx) {
    FENWICK_VALUE_TYPE sum = 0;
    for (uint64_t i = idx + 1; i > 0; i = fenwickPrev(i)) {
        sum = FENWICK_ALGO_(AlgoAdd_)(sum, tree[i - 1]);
    }

    return sum;
}

/* Element idx: cell i = idx + 1 covers (i - LSB(i), i]; the cells i - 1,
 * Prev(i - 1), ... down to i - LSB(i) cover (i - LSB(i), i - 1], so
 * subtracting them leaves element i alone, in one O(log n) walk. */
static inline FENWICK_VALUE_TYPE
FENWICK_ALGO_(AlgoElement_)(const FENWICK_VALUE_TYPE *tree, uint64_t idx) {
    const uint64_t i = idx + 1;
    FENWICK_VALUE_TYPE value = tree[i - 1];
    const uint64_t stop = fenwickPrev(i);
    for (uint64_t j = i - 1; j > stop; j = fenwickPrev(j)) {
        value = FENWICK_ALGO_(AlgoSub_)(value, tree[j - 1]);
    }

    return value;
}

/* Add 'delta' to element idx (idx < capacity) */
static inline void FENWICK_ALGO_(AlgoAddChain_)(FENWICK_VALUE_TYPE *tree,
                                                uint64_t capacity, uint64_t idx,
                                                FENWICK_VALUE_TYPE delta) {
    for (uint64_t i = idx + 1; i <= capacity; i = fenwickParent(i)) {
        tree[i - 1] = FENWICK_ALGO_(AlgoAdd_)(tree[i - 1], delta);
    }
}

/* Turn tree[0, capacity), holding the elements themselves (zero past the
 * count), into the BIT in O(n): each cell, final once reached in index
 * order, is added into its parent. */
static inline void FENWICK_ALGO_(AlgoBuild_)(FENWICK_VALUE_TYPE *tree,
                                             uint64_t capacity) {
    for (uint64_t j = 1; j <= capacity; j++) {
        const uint64_t parent = fenwickParent(j);
        if (parent <= capacity) {
            tree[parent - 1] =
                FENWICK_ALGO_(AlgoAdd_)(tree[parent - 1], tree[j - 1]);
        }
    }
}

/* Cells (oldCapacity, newCapacity] were just zeroed.  A new cell j covers
 * (j - LSB(j), j], which reaches an existing element only when j is a
 * power of two, and then holds the total: cell oldCapacity.  O(log n). */
static inline void FENWICK_ALGO_(AlgoGrow_)(FENWICK_VALUE_TYPE *tree,
                                            uint64_t oldCapacity,
                                            uint64_t newCapacity) {
    if (oldCapacity == 0) {
        return;
    }

    const FENWICK_VALUE_TYPE total = tree[oldCapacity - 1];
    for (uint64_t j = oldCapacity * 2; j <= newCapacity; j <<= 1) {
        tree[j - 1] = total;
    }
}

/* The element idx would hold after adding 'delta', in '*out'.  False when
 * that element does not fit the type (floats: when the delta or the
 * element is not finite); elements past the count are zero. */
static inline bool
FENWICK_ALGO_(AlgoNewElement_)(const FENWICK_VALUE_TYPE *tree, uint64_t count,
                               uint64_t idx, FENWICK_VALUE_TYPE delta,
                               FENWICK_VALUE_TYPE *out) {
    const FENWICK_VALUE_TYPE old =
        idx < count ? FENWICK_ALGO_(AlgoElement_)(tree, idx) : 0;
#if FENWICK_IS_FLOATING
    *out = old + delta;
    return isfinite(delta) && isfinite(*out);
#else
    return !__builtin_add_overflow(old, delta, out);
#endif
}

/* Smallest index whose prefix sum reaches 'target', for non-negative
 * elements (binary lifting, O(log n)); UINT64_MAX when none does. */
static inline uint64_t
FENWICK_ALGO_(AlgoLowerBound_)(const FENWICK_VALUE_TYPE *tree, uint64_t count,
                               FENWICK_VALUE_TYPE target) {
    if (count == 0) {
        return UINT64_MAX;
    }

    uint64_t bitMask = 1;
    while (bitMask <= count) {
        bitMask <<= 1;
    }

    bitMask >>= 1;
    uint64_t pos = 0;
    FENWICK_VALUE_TYPE sum = 0;
    for (; bitMask > 0; bitMask >>= 1) {
        const uint64_t next = pos + bitMask;
        if (next <= count) {
            const FENWICK_VALUE_TYPE withNext =
                FENWICK_ALGO_(AlgoAdd_)(sum, tree[next - 1]);
            if (withNext < target) {
                pos = next;
                sum = withNext;
            }
        }
    }

    return pos < count ? pos : UINT64_MAX;
}

#undef FENWICK_ALGO_
