#include "multiFenwick.h"
#include "cleaner.h"
#include "datakit.h"
#include "flexCapacityManagement.h"
#include "mflexInternal.h"
#include "multiFenwickCommon.h"
#include "multilist.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiFenwick *, multiFenwickFree);
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* multiFenwick structure - uses multilist for storage
 * Multilist handles all tiering and memory management automatically */
struct multiFenwick {
    multilist *tree;   /* BIT values stored as flex components in multilist */
    uint64_t count;    /* Logical count: highest index + 1 */
    uint64_t capacity; /* Allocated size (power-of-2, for BIT traversal) */
    mflexState *state; /* State for multilist operations */
};

/* ====================================================================
 * Arithmetic
 * ====================================================================
 * Every value the tree reads, sums or stores is a multiFenwickNum: an
 * exact integer part 'i' plus a floating part 'd'.  A value with
 * 'isFloat' false is the integer 'i' (and 'd' is 0); otherwise it is
 * i + d, and becomes the double nearest that when it is stored or
 * returned.
 *
 * Integer cells come back from storage as SIGNED_64 or UNSIGNED_64 by
 * value (flex stores integers canonically), so integer arithmetic never
 * depends on which of the two carries a value.  Float cells come back as
 * FLOAT_32 or DOUBLE_64 (flex keeps a double in a float when that loses
 * nothing), so all floating arithmetic runs in double.
 *
 * A sum over many cells keeps its integer cells exact even when some
 * cells are floats: the integer part only meets the floating part once,
 * at the end. */
typedef struct multiFenwickNum {
    __int128_t i;
    double d;
    bool isFloat;
} multiFenwickNum;

/* The value of 'box', which must be SIGNED_64, UNSIGNED_64, or a finite
 * FLOAT_32 or DOUBLE_64.  Every other type (VOID, BYTES, booleans,
 * pointers, containers, 96/128-bit values) and every NaN or infinity is
 * refused, so no entry point ever stores one. */
static bool multiFenwickNumFrom_(const databox *box, multiFenwickNum *n) {
    switch (box->type) {
    case DATABOX_SIGNED_64:
        *n = (multiFenwickNum){.i = box->data.i64};
        return true;
    case DATABOX_UNSIGNED_64:
        *n = (multiFenwickNum){.i = box->data.u64};
        return true;
    case DATABOX_FLOAT_32:
        if (!isfinite(box->data.f32)) {
            return false;
        }

        *n = (multiFenwickNum){.d = box->data.f32, .isFloat = true};
        return true;
    case DATABOX_DOUBLE_64:
        if (!isfinite(box->data.d64)) {
            return false;
        }

        *n = (multiFenwickNum){.d = box->data.d64, .isFloat = true};
        return true;
    default:
        return false;
    }
}

static multiFenwickNum multiFenwickNumAdd_(const multiFenwickNum a,
                                           const multiFenwickNum b) {
    return (multiFenwickNum){
        .i = a.i + b.i, .d = a.d + b.d, .isFloat = a.isFloat || b.isFloat};
}

static multiFenwickNum multiFenwickNumSub_(const multiFenwickNum a,
                                           const multiFenwickNum b) {
    return (multiFenwickNum){
        .i = a.i - b.i, .d = a.d - b.d, .isFloat = a.isFloat || b.isFloat};
}

/* 'n' as one value: unchanged for an integer, else the double nearest
 * i + d (in 'd', with 'i' zero). */
static multiFenwickNum multiFenwickNumCollapse_(multiFenwickNum n) {
    if (n.isFloat && n.i != 0) {
        n.d += (double)n.i;
        n.i = 0;
    }

    return n;
}

/* Box 'n' into 'out'.  Fails, leaving 'out' alone, when 'n' has no 64-bit
 * value: an integer outside [INT64_MIN, UINT64_MAX], or a float that is
 * not finite.  An integer result is SIGNED_64 when negative and
 * UNSIGNED_64 otherwise (the form flex reads it back in); a float result
 * is DOUBLE_64. */
static bool multiFenwickNumBox_(multiFenwickNum n, databox *out) {
    n = multiFenwickNumCollapse_(n);
    if (n.isFloat) {
        if (!isfinite(n.d)) {
            return false;
        }

        *out = DATABOX_DOUBLE(n.d);
        return true;
    }

    if (n.i < INT64_MIN || n.i > (__int128_t)UINT64_MAX) {
        return false;
    }

    if (n.i < 0) {
        *out = DATABOX_SIGNED((int64_t)n.i);
    } else {
        *out = DATABOX_UNSIGNED((uint64_t)n.i);
    }

    return true;
}

/* Exact comparison of the integer 'a' with the double 'd' (not NaN).
 * Every integer the tree sums stays far inside +/-2^100. */
static int multiFenwickCompareIntDouble_(const __int128_t a, const double d) {
    if (d >= 0x1p100) {
        return -1;
    }

    if (d <= -0x1p100) {
        return 1;
    }

    /* |d| < 2^100, so its integer part fits, and both the integer part
     * and the fraction left over are exact doubles. */
    const __int128_t whole = (__int128_t)d;
    if (a != whole) {
        return a < whole ? -1 : 1;
    }

    const double frac = d - (double)whole;
    return frac > 0 ? -1 : frac < 0 ? 1 : 0;
}

/* <0, 0 or >0 as a is below, equal to or above b.  Integers compare
 * exactly, with each other and with doubles. */
static int multiFenwickNumCompare_(multiFenwickNum a, multiFenwickNum b) {
    a = multiFenwickNumCollapse_(a);
    b = multiFenwickNumCollapse_(b);
    if (!a.isFloat && !b.isFloat) {
        return (a.i > b.i) - (a.i < b.i);
    }

    if (a.isFloat && b.isFloat) {
        return (a.d > b.d) - (a.d < b.d);
    }

    if (!a.isFloat) {
        return multiFenwickCompareIntDouble_(a.i, b.d);
    }

    return -multiFenwickCompareIntDouble_(b.i, a.d);
}

/* ====================================================================
 * Cells
 * ==================================================================== */

/* Read the BIT cell at 0-based position 'pos'.  Cells only ever hold
 * values multiFenwickNumFrom_ accepts; anything else (such as a deferred
 * 96/128-bit box with no payload) is still refused here, so it can never
 * reach the arithmetic. */
static bool multiFenwickCellGet_(const multiFenwick *mfw, uint64_t pos,
                                 multiFenwickNum *n) {
    multilistEntry entry;
    if (!multilistIndex(mfw->tree, mfw->state, (mlOffsetId)pos, &entry, true)) {
        return false;
    }

    return multiFenwickNumFrom_(&entry.box, n);
}

/* Sum of elements [0, idx], for idx < count. */
static bool multiFenwickPrefix_(const multiFenwick *mfw, uint64_t idx,
                                multiFenwickNum *sum) {
    multiFenwickNum acc = {0};
    for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
        multiFenwickNum cell;
        if (!multiFenwickCellGet_(mfw, i - 1, &cell)) {
            return false;
        }

        acc = multiFenwickNumAdd_(acc, cell);
    }

    *sum = acc;
    return true;
}

/* Element idx, for idx < count.  Cell i = idx + 1 (1-based) holds the
 * elements (i - LSB(i), i]; the cells i - 1, Prev(i - 1), ... down to
 * i - LSB(i) hold exactly (i - LSB(i), i - 1], so subtracting them leaves
 * element i alone, in O(log n) reads and with no prefix sum ever formed. */
static bool multiFenwickElement_(const multiFenwick *mfw, uint64_t idx,
                                 multiFenwickNum *value) {
    const uint64_t i = idx + 1;
    multiFenwickNum acc;
    if (!multiFenwickCellGet_(mfw, i - 1, &acc)) {
        return false;
    }

    const uint64_t stop = multiFenwickPrev(i);
    for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
        multiFenwickNum cell;
        if (!multiFenwickCellGet_(mfw, j - 1, &cell)) {
            return false;
        }

        acc = multiFenwickNumSub_(acc, cell);
    }

    *value = acc;
    return true;
}

/* ====================================================================
 * Creation
 * ==================================================================== */

/* Create new multiFenwick tree */
multiFenwick *multiFenwickNew(void) {
    /* multiFenwickFree NULL-checks each member, so the cleaner is safe
     * on every partial-construction exit below. */
    cleaner(multiFenwickFree) multiFenwick *mfw = zcalloc(1, sizeof(*mfw));
    if (!mfw) {
        return NULL;
    }

    /* Create multilist with reasonable limits */
    /* Tradeoff: the larger the multilist flex:
     *   - more memory saved for longer flex compaction
     *   - slower speed because it requires more linear updates */
    /* @own: container; free=multilistFree */
    mfw->tree = multilistNew(FLEX_CAP_LEVEL_8192, 0);
    if (!mfw->tree) {
        return NULL;
    }

    mfw->count = 0;
    mfw->capacity = 0;

    /* Allocate state */
    /* @own: container; free=mflexStateFree */
    mfw->state = zcalloc(1, sizeof(mflexState));
    if (!mfw->state) {
        return NULL;
    }

    multiFenwick *out = mfw;
    mfw = NULL;
    return out;
}

/* Capacity (the smallest power of two, at least 'current', that holds
 * index 'idx') */
static uint64_t multiFenwickCapacityFor_(uint64_t current, uint64_t idx) {
    uint64_t capacity = current == 0 ? 1 : current;
    while (capacity < idx + 1) {
        capacity <<= 1;
    }

    return capacity;
}

/* Create from array of databoxes.
 *
 * Every element is checked before anything is allocated.  The cells then
 * start as the elements themselves (zero past the end), and one pass in
 * index order adds each cell into its parent: by the time cell j is
 * reached, every cell below it that feeds it has already been added, so
 * cell j is final.  That builds the tree in O(n) cell reads and writes. */
multiFenwick *multiFenwickNewFromArray(const databox *values, size_t count) {
    if (count == 0) {
        /* @own: caller; free=multiFenwickFree */
        return multiFenwickNew();
    }

    if (!values || count - 1 > MULTIFENWICK_INDEX_MAX) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        multiFenwickNum n;
        if (!multiFenwickNumFrom_(&values[i], &n)) {
            return NULL;
        }
    }

    cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
    if (!mfw) {
        return NULL;
    }

    const uint64_t capacity = multiFenwickCapacityFor_(0, count - 1);
    for (uint64_t i = 0; i < capacity; i++) {
        databox cell = DATABOX_UNSIGNED(0);
        if (i < count) {
            multiFenwickNum n;
            const bool ok = multiFenwickNumFrom_(&values[i], &n) &&
                            multiFenwickNumBox_(n, &cell);
            assert(ok); /* one checked element always boxes */
            (void)ok;
        }

        multilistPushByTypeTail(&mfw->tree, mfw->state, &cell);
    }

    mfw->count = count;
    mfw->capacity = capacity;

    for (uint64_t j = 1; j <= capacity; j++) {
        const uint64_t parent = multiFenwickParent(j);
        if (parent > capacity) {
            continue;
        }

        multiFenwickNum child;
        multiFenwickNum sum;
        if (!multiFenwickCellGet_(mfw, j - 1, &child) ||
            !multiFenwickCellGet_(mfw, parent - 1, &sum)) {
            return NULL;
        }

        /* A block of elements whose sum has no 64-bit value refuses the
         * whole array (the cleaner frees the partial tree). */
        databox box;
        if (!multiFenwickNumBox_(multiFenwickNumAdd_(sum, child), &box)) {
            return NULL;
        }

        const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)(parent - 1), &box);
        assert(replaced);
        (void)replaced;
    }

    multiFenwick *out = mfw;
    mfw = NULL;
    return out;
}

/* Free multiFenwick tree */
void multiFenwickFree(multiFenwick *mfw) {
    if (!mfw) {
        return;
    }

    if (mfw->tree) {
        multilistFree(mfw->tree);
    }

    /* mflexStateFree also releases any scratch buffer the state took on */
    mflexStateFree(mfw->state);
    zfree(mfw);
}

/* Grow the tree's cells to 'newCapacity' (a power of two above the
 * current capacity), keeping every element's value.  'total' is the
 * value of cell 'oldCapacity' (1-based), the sum of every existing
 * element.
 *
 * A new cell j in (oldCapacity, newCapacity] covers (j - LSB(j), j],
 * which reaches an existing element only when j is a power of two, and
 * then holds exactly that total; every other new cell is zero.  Pushes
 * and in-range replaces cannot fail, so neither can this. */
static void multiFenwickGrow_(multiFenwick *mfw, uint64_t newCapacity,
                              const databox *total) {
    const uint64_t oldCapacity = mfw->capacity;

    const databox zero = DATABOX_UNSIGNED(0);
    for (uint64_t i = oldCapacity; i < newCapacity; i++) {
        multilistPushByTypeTail(&mfw->tree, mfw->state, &zero);
    }

    for (uint64_t j = oldCapacity * 2; oldCapacity > 0 && j <= newCapacity;
         j <<= 1) {
        const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)(j - 1), total);
        assert(replaced);
        (void)replaced;
    }

    mfw->capacity = newCapacity;
}

/* Add 'delta' to element idx (idx <= MULTIFENWICK_INDEX_MAX).
 *
 * Transactional: every step that can fail (the read of every cell the
 * update touches, and the check that each new cell value has a 64-bit
 * value) runs before the first write.  The write phase is pushes and
 * in-range replaces, which cannot fail, so a false return leaves the tree
 * (cells, count and capacity) exactly as it was, and a NULL '*mfw' stays
 * NULL. */
static bool multiFenwickApply_(multiFenwick **mfw, uint64_t idx,
                               const multiFenwickNum delta) {
    /* Plan: the capacity this index needs, and the new value of every
     * cell on its update chain, computed from the cells as they are.
     * A chain cell beyond the current capacity does not exist yet: it
     * will hold the existing total (see multiFenwickGrow_) or zero. */
    const multiFenwick *cur = *mfw; /* NULL: a new, empty tree */
    const uint64_t oldCapacity = cur ? cur->capacity : 0;
    const uint64_t newCapacity =
        idx < oldCapacity ? oldCapacity
                          : multiFenwickCapacityFor_(oldCapacity, idx);
    multiFenwickNum total = {0};
    databox totalBox = DATABOX_UNSIGNED(0);
    if (newCapacity > oldCapacity && oldCapacity > 0) {
        if (!multiFenwickCellGet_(cur, oldCapacity - 1, &total)) {
            return false;
        }

        const bool boxed = multiFenwickNumBox_(total, &totalBox);
        assert(boxed); /* a stored cell always boxes */
        (void)boxed;
    }

    /* A chain has at most one cell per bit of the capacity */
    databox planned[64];
    size_t steps = 0;
    for (uint64_t i = idx + 1; i <= newCapacity; i = multiFenwickParent(i)) {
        multiFenwickNum base = {0};
        if (i <= oldCapacity) {
            if (!multiFenwickCellGet_(cur, i - 1, &base)) {
                return false;
            }
        } else if (oldCapacity > 0 && (i & (i - 1)) == 0) {
            base = total;
        }

        assert(steps < sizeof(planned) / sizeof(*planned));
        if (!multiFenwickNumBox_(multiFenwickNumAdd_(base, delta),
                                 &planned[steps])) {
            return false;
        }

        steps++;
    }

    /* Apply: past creating a new tree, nothing below can fail. */
    if (!*mfw) {
        /* @own: caller; free=multiFenwickFree */
        *mfw = multiFenwickNew();
        if (!*mfw) {
            return false;
        }
    }

    multiFenwick *tree = *mfw;
    if (newCapacity > oldCapacity) {
        multiFenwickGrow_(tree, newCapacity, &totalBox);
    }

    steps = 0;
    for (uint64_t i = idx + 1; i <= tree->capacity; i = multiFenwickParent(i)) {
        const bool replaced = multilistReplaceByTypeAtIndex(
            &tree->tree, tree->state, (mlNodeId)(i - 1), &planned[steps++]);
        assert(replaced);
        (void)replaced;
    }

    if (idx + 1 > tree->count) {
        tree->count = idx + 1;
    }

    return true;
}

/* ====================================================================
 * Public operations
 * ==================================================================== */

bool multiFenwickUpdate(multiFenwick **mfw, size_t idx, const databox *delta) {
    multiFenwickNum d;
    if (!mfw || !delta || idx > MULTIFENWICK_INDEX_MAX ||
        !multiFenwickNumFrom_(delta, &d)) {
        return false;
    }

    return multiFenwickApply_(mfw, idx, d);
}

/* Query: compute prefix sum [0, idx] */
bool multiFenwickQuery(const multiFenwick *mfw, size_t idx, databox *result) {
    if (!result) {
        return false;
    }

    multiFenwickNum sum;
    if (!mfw || idx >= mfw->count || !multiFenwickPrefix_(mfw, idx, &sum) ||
        !multiFenwickNumBox_(sum, result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Range query: sum [left, right], formed exactly from the two prefix sums
 * (so it can be returned even when a prefix sum itself has no 64-bit
 * value) */
bool multiFenwickRangeQuery(const multiFenwick *mfw, size_t left, size_t right,
                            databox *result) {
    if (!result) {
        return false;
    }

    multiFenwickNum hi;
    multiFenwickNum lo = {0};
    if (!mfw || left > right || right >= mfw->count ||
        !multiFenwickPrefix_(mfw, right, &hi) ||
        (left > 0 && !multiFenwickPrefix_(mfw, left - 1, &lo)) ||
        !multiFenwickNumBox_(multiFenwickNumSub_(hi, lo), result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Get single element value */
bool multiFenwickGet(const multiFenwick *mfw, size_t idx, databox *result) {
    if (!result) {
        return false;
    }

    multiFenwickNum value;
    if (!mfw || idx >= mfw->count || !multiFenwickElement_(mfw, idx, &value) ||
        !multiFenwickNumBox_(value, result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Set single element to exact value: one Update by value - current, with
 * the difference kept exact (it may need more than 64 bits, as from
 * INT64_MIN to UINT64_MAX) */
bool multiFenwickSet(multiFenwick **mfw, size_t idx, const databox *value) {
    multiFenwickNum want;
    if (!mfw || !value || idx > MULTIFENWICK_INDEX_MAX ||
        !multiFenwickNumFrom_(value, &want)) {
        return false;
    }

    /* An absent tree or index is zero; Apply creates the tree, so a
     * refused Set leaves a NULL '*mfw' NULL. */
    multiFenwickNum current = {0};
    if (*mfw && idx < (*mfw)->count &&
        !multiFenwickElement_(*mfw, idx, &current)) {
        return false;
    }

    return multiFenwickApply_(mfw, idx, multiFenwickNumSub_(want, current));
}

/* Get count */
size_t multiFenwickCount(const multiFenwick *mfw) {
    return mfw ? mfw->count : 0;
}

/* Get bytes used */
size_t multiFenwickBytes(const multiFenwick *mfw) {
    if (!mfw) {
        return 0;
    }

    size_t total = sizeof(multiFenwick);
    total += sizeof(mflexState);

    if (mfw->tree) {
        total += multilistBytes(mfw->tree);
    }

    return total;
}

/* Lower bound: binary search over the prefix sums, O(log^2 n).
 *
 * The search keeps 'result' as the lowest index seen whose prefix sum
 * reaches the target, and only ever moves 'left' past an index whose
 * prefix sum is below it.  So a returned index i always has
 * prefix(i) >= target and either i == 0 or prefix(i - 1) < target; with
 * non-negative elements the prefix sums never fall, and that index is
 * the smallest one. */
size_t multiFenwickLowerBound(const multiFenwick *mfw, const databox *target) {
    multiFenwickNum want;
    if (!mfw || !target || mfw->count == 0 ||
        !multiFenwickNumFrom_(target, &want)) {
        return SIZE_MAX;
    }

    size_t left = 0;
    size_t right = mfw->count - 1;
    size_t result = SIZE_MAX;

    while (left <= right) {
        const size_t mid = left + (right - left) / 2;

        multiFenwickNum midSum;
        if (!multiFenwickPrefix_(mfw, mid, &midSum)) {
            return SIZE_MAX;
        }

        if (multiFenwickNumCompare_(midSum, want) >= 0) {
            /* midSum >= target, so mid is a candidate */
            result = mid;
            if (mid == 0) {
                break;
            }

            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }

    return result;
}

/* Clear all values to zero */
void multiFenwickClear(multiFenwick *mfw) {
    if (!mfw || !mfw->tree) {
        return;
    }

    const databox zero = DATABOX_UNSIGNED(0);
    for (uint64_t i = 0; i < mfw->capacity; i++) {
        const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)i, &zero);
        assert(replaced);
        (void)replaced;
    }
}

#ifdef DATAKIT_TEST
/* Print debug representation */
void multiFenwickRepr(const multiFenwick *mfw) {
    if (!mfw) {
        printf("multiFenwick: (nil)\n");
        return;
    }

    size_t count = multiFenwickCount(mfw);
    size_t bytes = multiFenwickBytes(mfw);

    printf("multiFenwick [count=%zu, capacity=%zu, bytes=%zu]\n", count,
           (size_t)mfw->capacity, bytes);

    /* Print first few prefix sums */
    if (count > 0) {
        printf("  Prefix sums: [");
        size_t toShow = count < 20 ? count : 20;
        for (size_t i = 0; i < toShow; i++) {
            databox sum;
            if (multiFenwickQuery(mfw, i, &sum)) {
                switch (sum.type) {
                case DATABOX_SIGNED_64:
                    printf("%" PRId64, sum.data.i64);
                    break;
                case DATABOX_UNSIGNED_64:
                    printf("%" PRIu64, sum.data.u64);
                    break;
                case DATABOX_FLOAT_32:
                    printf("%.2f", (double)sum.data.f32);
                    break;
                case DATABOX_DOUBLE_64:
                    printf("%.2f", sum.data.d64);
                    break;
                default:
                    printf("?");
                    break;
                }
            } else {
                printf("?");
            }

            if (i < toShow - 1) {
                printf(", ");
            }
        }
        if (count > 20) {
            printf(", ... (%zu more)", count - 20);
        }
        printf("]\n");

        /* Print first few element values */
        printf("  Elements: [");
        for (size_t i = 0; i < toShow; i++) {
            databox val;
            if (multiFenwickGet(mfw, i, &val)) {
                switch (val.type) {
                case DATABOX_SIGNED_64:
                    printf("%" PRId64, val.data.i64);
                    break;
                case DATABOX_UNSIGNED_64:
                    printf("%" PRIu64, val.data.u64);
                    break;
                case DATABOX_FLOAT_32:
                    printf("%.2f", (double)val.data.f32);
                    break;
                case DATABOX_DOUBLE_64:
                    printf("%.2f", val.data.d64);
                    break;
                default:
                    printf("?");
                    break;
                }
            } else {
                printf("?");
            }

            if (i < toShow - 1) {
                printf(", ");
            }
        }
        if (count > 20) {
            printf(", ...");
        }
        printf("]\n");
    }
}
#endif
