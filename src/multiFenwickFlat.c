#include "multiFenwickFlat.h"
#include "cleaner.h"
#include "datakit.h"
#include "multiFenwickCommon.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);
#include <stdio.h>
#include <string.h>

/* What a cell's 64 bits hold.  SIGNED is 0, so zero-filled memory is a
 * cell holding the integer 0. */
typedef enum multiFenwickFlatKind {
    MFWF_SIGNED = 0,
    MFWF_UNSIGNED = 1,
    MFWF_DOUBLE = 2,
} multiFenwickFlatKind;

/* Cells live in flat arrays of 'capacity' entries, so every cell read and
 * write is O(1): the value bits, and a one-byte kind per cell.  The kinds
 * array exists only once some cell needs a kind other than SIGNED (a
 * value above INT64_MAX, or a double); until then every cell is an int64
 * (8 bytes per cell) and sums take an integer-only fast path. */
struct multiFenwickFlat {
    uint64_t *bits;    /* cell values: int64, uint64 or double bits */
    uint8_t *kinds;    /* multiFenwickFlatKind per cell; NULL: all SIGNED */
    uint64_t count;    /* Logical count: highest index + 1 */
    uint64_t capacity; /* Allocated cells (power of two, for BIT traversal) */
};

/* ====================================================================
 * Cells
 * ==================================================================== */

/* The cell at 0-based position 'pos' < capacity.  Cells only ever hold
 * what multiFenwickFlatCellSet_ stores, so a read cannot fail. */
static multiFenwickNum multiFenwickFlatCellGet_(const multiFenwickFlat *mfw,
                                                uint64_t pos) {
    assert(pos < mfw->capacity);
    const uint64_t bits = mfw->bits[pos];
    const multiFenwickFlatKind kind =
        mfw->kinds ? (multiFenwickFlatKind)mfw->kinds[pos] : MFWF_SIGNED;
    switch (kind) {
    case MFWF_UNSIGNED:
        return (multiFenwickNum){.i = bits};
    case MFWF_DOUBLE: {
        double d;
        memcpy(&d, &bits, sizeof(d));
        return (multiFenwickNum){.d = d, .isFloat = true};
    }
    case MFWF_SIGNED:
    default:
        assert(kind == MFWF_SIGNED);
        return (multiFenwickNum){.i = (int64_t)bits};
    }
}

/* Store 'box' (SIGNED_64, UNSIGNED_64 or DOUBLE_64, as multiFenwickNumBox_
 * makes it) in the cell at 0-based position 'pos' < capacity.  The first
 * cell that is not an int64 creates the kinds array (all SIGNED, which is
 * what every cell so far is).  OOM is fail-fast. */
static void multiFenwickFlatCellSet_(multiFenwickFlat *mfw, uint64_t pos,
                                     const databox *box) {
    assert(pos < mfw->capacity);
    multiFenwickFlatKind kind = MFWF_SIGNED;
    switch (box->type) {
    case DATABOX_SIGNED_64:
        mfw->bits[pos] = (uint64_t)box->data.i64;
        break;
    case DATABOX_DOUBLE_64:
        memcpy(&mfw->bits[pos], &box->data.d64, sizeof(mfw->bits[pos]));
        kind = MFWF_DOUBLE;
        break;
    case DATABOX_UNSIGNED_64:
    default:
        assert(box->type == DATABOX_UNSIGNED_64);
        mfw->bits[pos] = box->data.u64;
        kind = box->data.u64 > INT64_MAX ? MFWF_UNSIGNED : MFWF_SIGNED;
        break;
    }

    if (kind != MFWF_SIGNED && !mfw->kinds) {
        /* @own: container; free=zfree */
        mfw->kinds = zcalloc(mfw->capacity, sizeof(*mfw->kinds));
    }

    if (mfw->kinds) {
        mfw->kinds[pos] = (uint8_t)kind;
    }
}

/* Grow the cell arrays to 'newCapacity' cells, zero-filling the new cells
 * (each then holds the integer 0).  OOM is fail-fast. */
static void multiFenwickFlatCellsGrow_(multiFenwickFlat *mfw,
                                       uint64_t newCapacity) {
    assert(newCapacity > mfw->capacity);
    const uint64_t added = newCapacity - mfw->capacity;
    /* @own: container; free=zfree */
    mfw->bits = zrealloc(mfw->bits, newCapacity * sizeof(*mfw->bits));
    memset(&mfw->bits[mfw->capacity], 0, added * sizeof(*mfw->bits));
    if (mfw->kinds) {
        /* @own: container; free=zfree */
        mfw->kinds = zrealloc(mfw->kinds, newCapacity * sizeof(*mfw->kinds));
        memset(&mfw->kinds[mfw->capacity], 0, added * sizeof(*mfw->kinds));
    }

    mfw->capacity = newCapacity;
}

/* Sum of elements [0, idx], for idx < count.  With every cell an int64
 * the sum stays integer: int128 adds only (at most 64 cells of 64 bits). */
static multiFenwickNum multiFenwickFlatPrefix_(const multiFenwickFlat *mfw,
                                               uint64_t idx) {
    if (!mfw->kinds) {
        __int128_t sum = 0;
        for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
            sum += (int64_t)mfw->bits[i - 1];
        }

        return (multiFenwickNum){.i = sum};
    }

    multiFenwickNum acc = {0};
    for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
        acc = multiFenwickNumAdd_(acc, multiFenwickFlatCellGet_(mfw, i - 1));
    }

    return acc;
}

/* Element idx, for idx < count.  Cell i = idx + 1 (1-based) holds the
 * elements (i - LSB(i), i]; the cells i - 1, Prev(i - 1), ... down to
 * i - LSB(i) hold exactly (i - LSB(i), i - 1], so subtracting them leaves
 * element i alone, in O(log n) reads and with no prefix sum ever formed. */
static multiFenwickNum multiFenwickFlatElement_(const multiFenwickFlat *mfw,
                                                uint64_t idx) {
    const uint64_t i = idx + 1;
    const uint64_t stop = multiFenwickPrev(i);
    if (!mfw->kinds) {
        __int128_t value = (int64_t)mfw->bits[i - 1];
        for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
            value -= (int64_t)mfw->bits[j - 1];
        }

        return (multiFenwickNum){.i = value};
    }

    multiFenwickNum acc = multiFenwickFlatCellGet_(mfw, i - 1);
    for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
        acc = multiFenwickNumSub_(acc, multiFenwickFlatCellGet_(mfw, j - 1));
    }

    return acc;
}

/* ====================================================================
 * Creation
 * ==================================================================== */

multiFenwickFlat *multiFenwickFlatNew(void) {
    /* @own: caller; free=multiFenwickFlatFree */
    return zcalloc(1, sizeof(multiFenwickFlat));
}

/* Create from array of databoxes.
 *
 * Every element is checked before anything is allocated.  The cells then
 * start as the elements themselves (zero past the end), and one pass in
 * index order adds each cell into its parent: by the time cell j is
 * reached, every cell below it that feeds it has already been added, so
 * cell j is final.  That builds the tree in O(n) cell reads and writes. */
multiFenwickFlat *multiFenwickFlatNewFromArray(const databox *values,
                                               size_t count) {
    if (count == 0) {
        /* @own: caller; free=multiFenwickFlatFree */
        return multiFenwickFlatNew();
    }

    if (!values || count - 1 > MULTIFENWICK_FLAT_INDEX_MAX) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        multiFenwickNum n;
        if (!multiFenwickNumFrom_(&values[i], &n)) {
            return NULL;
        }
    }

    cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw = multiFenwickFlatNew();
    if (!mfw) {
        return NULL;
    }

    const uint64_t capacity = multiFenwickCapacityFor_(0, count - 1);
    multiFenwickFlatCellsGrow_(mfw, capacity); /* zero past the end */
    for (uint64_t i = 0; i < count; i++) {
        multiFenwickNum n;
        databox cell;
        const bool ok = multiFenwickNumFrom_(&values[i], &n) &&
                        multiFenwickNumBox_(n, &cell);
        assert(ok); /* one checked element always boxes */
        (void)ok;
        multiFenwickFlatCellSet_(mfw, i, &cell);
    }

    mfw->count = count;

    for (uint64_t j = 1; j <= capacity; j++) {
        const uint64_t parent = multiFenwickParent(j);
        if (parent > capacity) {
            continue;
        }

        /* A block of elements whose sum has no 64-bit value refuses the
         * whole array (the cleaner frees the partial tree). */
        databox box;
        if (!multiFenwickNumBox_(
                multiFenwickNumAdd_(multiFenwickFlatCellGet_(mfw, parent - 1),
                                    multiFenwickFlatCellGet_(mfw, j - 1)),
                &box)) {
            return NULL;
        }

        multiFenwickFlatCellSet_(mfw, parent - 1, &box);
    }

    multiFenwickFlat *out = mfw;
    mfw = NULL;
    return out;
}

void multiFenwickFlatFree(multiFenwickFlat *mfw) {
    if (!mfw) {
        return;
    }

    zfree(mfw->bits);
    zfree(mfw->kinds);
    zfree(mfw);
}

/* Grow the tree's cells to 'newCapacity' (a power of two above the
 * current capacity), keeping every element's value.  'total' is the
 * value of cell 'oldCapacity' (1-based), the sum of every existing
 * element.
 *
 * A new cell j in (oldCapacity, newCapacity] covers (j - LSB(j), j],
 * which reaches an existing element only when j is a power of two, and
 * then holds exactly that total; every other new cell is zero. */
static void multiFenwickFlatGrow_(multiFenwickFlat *mfw, uint64_t newCapacity,
                                  const databox *total) {
    const uint64_t oldCapacity = mfw->capacity;
    multiFenwickFlatCellsGrow_(mfw, newCapacity);
    for (uint64_t j = oldCapacity * 2; oldCapacity > 0 && j <= newCapacity;
         j <<= 1) {
        multiFenwickFlatCellSet_(mfw, j - 1, total);
    }
}

/* Add 'delta' to element idx (idx <= MULTIFENWICK_FLAT_INDEX_MAX).
 *
 * Transactional: the check that each new cell value has a 64-bit value
 * runs before the first write, and the writes (growth is fail-fast)
 * cannot fail, so a false return leaves the tree exactly as it was, and a
 * NULL '*mfw' stays NULL. */
static bool multiFenwickFlatApply_(multiFenwickFlat **mfw, uint64_t idx,
                                   const multiFenwickNum delta) {
    /* Fast path: an integer delta, within the capacity, into a tree whose
     * cells are all int64, where every new cell value stays an int64.
     * Plan in int128, then write; anything else takes the general path
     * below (which may add the kinds array). */
    multiFenwickFlat *tree = *mfw;
    if (tree && !tree->kinds && !delta.isFloat && idx < tree->capacity) {
        int64_t fast[64];
        size_t n = 0;
        bool fits = true;
        for (uint64_t i = idx + 1; i <= tree->capacity;
             i = multiFenwickParent(i)) {
            const __int128_t v =
                (__int128_t)(int64_t)tree->bits[i - 1] + delta.i;
            if (v < INT64_MIN || v > INT64_MAX) {
                fits = false;
                break;
            }

            fast[n++] = (int64_t)v;
        }

        if (fits) {
            n = 0;
            for (uint64_t i = idx + 1; i <= tree->capacity;
                 i = multiFenwickParent(i)) {
                tree->bits[i - 1] = (uint64_t)fast[n++];
            }

            if (idx + 1 > tree->count) {
                tree->count = idx + 1;
            }

            return true;
        }
    }

    /* Plan: the capacity this index needs, and the new value of every
     * cell on its update chain, computed from the cells as they are.
     * A chain cell beyond the current capacity does not exist yet: it
     * will hold the existing total (see multiFenwickFlatGrow_) or zero. */
    const multiFenwickFlat *cur = *mfw; /* NULL: a new, empty tree */
    const uint64_t oldCapacity = cur ? cur->capacity : 0;
    const uint64_t newCapacity =
        idx < oldCapacity ? oldCapacity
                          : multiFenwickCapacityFor_(oldCapacity, idx);
    multiFenwickNum total = {0};
    databox totalBox = DATABOX_UNSIGNED(0);
    if (newCapacity > oldCapacity && oldCapacity > 0) {
        total = multiFenwickFlatCellGet_(cur, oldCapacity - 1);
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
            base = multiFenwickFlatCellGet_(cur, i - 1);
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
        /* @own: caller; free=multiFenwickFlatFree */
        *mfw = multiFenwickFlatNew();
        if (!*mfw) {
            return false;
        }
    }

    tree = *mfw;
    if (newCapacity > oldCapacity) {
        multiFenwickFlatGrow_(tree, newCapacity, &totalBox);
    }

    steps = 0;
    for (uint64_t i = idx + 1; i <= tree->capacity; i = multiFenwickParent(i)) {
        multiFenwickFlatCellSet_(tree, i - 1, &planned[steps++]);
    }

    if (idx + 1 > tree->count) {
        tree->count = idx + 1;
    }

    return true;
}

/* ====================================================================
 * Public operations
 * ==================================================================== */

bool multiFenwickFlatUpdate(multiFenwickFlat **mfw, size_t idx,
                            const databox *delta) {
    multiFenwickNum d;
    if (!mfw || !delta || idx > MULTIFENWICK_FLAT_INDEX_MAX ||
        !multiFenwickNumFrom_(delta, &d)) {
        return false;
    }

    return multiFenwickFlatApply_(mfw, idx, d);
}

bool multiFenwickFlatQuery(const multiFenwickFlat *mfw, size_t idx,
                           databox *result) {
    if (!result) {
        return false;
    }

    if (!mfw || idx >= mfw->count ||
        !multiFenwickNumBox_(multiFenwickFlatPrefix_(mfw, idx), result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Range query: sum [left, right], formed exactly from the two prefix sums
 * (so it can be returned even when a prefix sum itself has no 64-bit
 * value) */
bool multiFenwickFlatRangeQuery(const multiFenwickFlat *mfw, size_t left,
                                size_t right, databox *result) {
    if (!result) {
        return false;
    }

    if (!mfw || left > right || right >= mfw->count) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    const multiFenwickNum hi = multiFenwickFlatPrefix_(mfw, right);
    const multiFenwickNum lo = left > 0 ? multiFenwickFlatPrefix_(mfw, left - 1)
                                        : (multiFenwickNum){0};
    if (!multiFenwickNumBox_(multiFenwickNumSub_(hi, lo), result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

bool multiFenwickFlatGet(const multiFenwickFlat *mfw, size_t idx,
                         databox *result) {
    if (!result) {
        return false;
    }

    if (!mfw || idx >= mfw->count ||
        !multiFenwickNumBox_(multiFenwickFlatElement_(mfw, idx), result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

/* Set single element to exact value: one Update by value - current, with
 * the difference kept exact (it may need more than 64 bits, as from
 * INT64_MIN to UINT64_MAX) */
bool multiFenwickFlatSet(multiFenwickFlat **mfw, size_t idx,
                         const databox *value) {
    multiFenwickNum want;
    if (!mfw || !value || idx > MULTIFENWICK_FLAT_INDEX_MAX ||
        !multiFenwickNumFrom_(value, &want)) {
        return false;
    }

    /* An absent tree or index is zero; Apply creates the tree, so a
     * refused Set leaves a NULL '*mfw' NULL. */
    const multiFenwickNum current = *mfw && idx < (*mfw)->count
                                        ? multiFenwickFlatElement_(*mfw, idx)
                                        : (multiFenwickNum){0};
    return multiFenwickFlatApply_(mfw, idx, multiFenwickNumSub_(want, current));
}

size_t multiFenwickFlatCount(const multiFenwickFlat *mfw) {
    return mfw ? mfw->count : 0;
}

size_t multiFenwickFlatBytes(const multiFenwickFlat *mfw) {
    if (!mfw) {
        return 0;
    }

    return sizeof(*mfw) + mfw->capacity * sizeof(*mfw->bits) +
           (mfw->kinds ? mfw->capacity * sizeof(*mfw->kinds) : 0);
}

/* Lower bound: binary search over the prefix sums, O(log^2 n).
 *
 * The search keeps 'result' as the lowest index seen whose prefix sum
 * reaches the target, and only ever moves 'left' past an index whose
 * prefix sum is below it.  So a returned index i always has
 * prefix(i) >= target and either i == 0 or prefix(i - 1) < target; with
 * non-negative elements the prefix sums never fall, and that index is
 * the smallest one. */
size_t multiFenwickFlatLowerBound(const multiFenwickFlat *mfw,
                                  const databox *target) {
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
        if (multiFenwickNumCompare_(multiFenwickFlatPrefix_(mfw, mid), want) >=
            0) {
            /* prefix(mid) >= target, so mid is a candidate */
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

void multiFenwickFlatClear(multiFenwickFlat *mfw) {
    if (!mfw || mfw->capacity == 0) {
        return;
    }

    /* Every cell becomes the int64 0, so the kinds array is not needed */
    memset(mfw->bits, 0, mfw->capacity * sizeof(*mfw->bits));
    zfree(mfw->kinds);
    mfw->kinds = NULL;
}

#ifdef DATAKIT_TEST
static void multiFenwickFlatReprBox_(bool ok, const databox *box) {
    if (!ok) {
        printf("?");
        return;
    }

    switch (box->type) {
    case DATABOX_SIGNED_64:
        printf("%" PRId64, box->data.i64);
        break;
    case DATABOX_UNSIGNED_64:
        printf("%" PRIu64, box->data.u64);
        break;
    case DATABOX_DOUBLE_64:
        printf("%.2f", box->data.d64);
        break;
    default:
        printf("?");
        break;
    }
}

void multiFenwickFlatRepr(const multiFenwickFlat *mfw) {
    if (!mfw) {
        printf("multiFenwickFlat: (nil)\n");
        return;
    }

    const size_t count = multiFenwickFlatCount(mfw);
    printf("multiFenwickFlat [count=%zu, capacity=%zu, bytes=%zu]\n", count,
           (size_t)mfw->capacity, multiFenwickFlatBytes(mfw));
    if (count == 0) {
        return;
    }

    const size_t toShow = count < 20 ? count : 20;
    printf("  Prefix sums: [");
    for (size_t i = 0; i < toShow; i++) {
        databox sum;
        multiFenwickFlatReprBox_(multiFenwickFlatQuery(mfw, i, &sum), &sum);
        printf("%s", i + 1 < toShow ? ", " : "");
    }

    printf("%s]\n  Elements: [", count > toShow ? ", ..." : "");
    for (size_t i = 0; i < toShow; i++) {
        databox val;
        multiFenwickFlatReprBox_(multiFenwickFlatGet(mfw, i, &val), &val);
        printf("%s", i + 1 < toShow ? ", " : "");
    }

    printf("%s]\n", count > toShow ? ", ..." : "");
}
#endif
