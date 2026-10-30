#include "multiFenwickFlat.h"
#include "cleaner.h"
#include "datakit.h"
#include "dkDenseGrowth.h"
#include "flex.h"
#include "multiFenwickCells.h"
#include "multiFenwickCommon.h"
#include "persist.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);
#include <stdio.h>
#include <string.h>

/* The cells are an adaptive store (multiFenwickCells.h): 8 bytes per cell
 * while every cell is an int64, which is when sums, updates and lifting
 * take integer-only fast paths; a kinds array joins once a cell holds a
 * value above INT64_MAX or a double. */
struct multiFenwickFlat {
    multiFenwickCells cells; /* 'capacity' cells */
    uint64_t count;          /* Logical count: highest index + 1 */
    uint64_t capacity;       /* Allocated cells (power of two) */
};

/* ====================================================================
 * Cells
 * ==================================================================== */

static multiFenwickNum multiFenwickFlatCellGet_(const multiFenwickFlat *mfw,
                                                uint64_t pos) {
    assert(pos < mfw->capacity);
    return multiFenwickCellsGet(&mfw->cells, pos);
}

/* Grow the cells to 'newCapacity', the new ones zero */
static void multiFenwickFlatCellsGrow_(multiFenwickFlat *mfw,
                                       uint64_t newCapacity) {
    assert(newCapacity > mfw->capacity);
    multiFenwickCellsGrow(&mfw->cells, mfw->capacity, newCapacity);
    mfw->capacity = newCapacity;
}

/* Sum of elements [0, idx], for idx < count.  With every cell an int64
 * the sum stays integer: int128 adds only (at most 64 cells of 64 bits). */
static multiFenwickNum multiFenwickFlatPrefix_(const multiFenwickFlat *mfw,
                                               uint64_t idx) {
    const multiFenwickCells *c = &mfw->cells;
    if (multiFenwickCellsAllInt(c)) {
        __int128_t sum = 0;
        for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
            sum += multiFenwickCellsInt(c, i - 1);
        }

        return (multiFenwickNum){.i = sum};
    }

    multiFenwickNum acc = {};
    for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
        acc = multiFenwickNumAdd_(acc, multiFenwickCellsGet(c, i - 1));
    }

    return acc;
}

/* Element idx, for idx < count.  Cell i = idx + 1 (1-based) holds the
 * elements (i - LSB(i), i]; the cells i - 1, Prev(i - 1), ... down to
 * i - LSB(i) hold exactly (i - LSB(i), i - 1], so subtracting them leaves
 * element i alone, in O(log n) reads and with no prefix sum ever formed. */
static multiFenwickNum multiFenwickFlatElement_(const multiFenwickFlat *mfw,
                                                uint64_t idx) {
    const multiFenwickCells *c = &mfw->cells;
    const uint64_t i = idx + 1;
    const uint64_t stop = multiFenwickPrev(i);
    if (multiFenwickCellsAllInt(c)) {
        __int128_t value = multiFenwickCellsInt(c, i - 1);
        for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
            value -= multiFenwickCellsInt(c, j - 1);
        }

        return (multiFenwickNum){.i = value};
    }

    multiFenwickNum acc = multiFenwickCellsGet(c, i - 1);
    for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
        acc = multiFenwickNumSub_(acc, multiFenwickCellsGet(c, j - 1));
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
        return nullptr;
    }

    for (size_t i = 0; i < count; i++) {
        multiFenwickNum n;
        if (!multiFenwickNumFrom_(&values[i], &n)) {
            return nullptr;
        }
    }

    cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw = multiFenwickFlatNew();
    if (!mfw) {
        return nullptr;
    }

    const uint64_t capacity = multiFenwickCapacityFor_(0, count - 1);
    multiFenwickFlatCellsGrow_(mfw, capacity); /* zero past the end */
    for (uint64_t i = 0; i < count; i++) {
        multiFenwickNum n;
        multiFenwickCell cell = {};
        [[maybe_unused]] const bool ok = multiFenwickNumFrom_(&values[i], &n) &&
                                         multiFenwickCellEncode(n, &cell);
        assert(ok); /* one checked element always encodes */
        multiFenwickCellsPut(&mfw->cells, capacity, i, cell);
    }

    mfw->count = count;

    for (uint64_t j = 1; j <= capacity; j++) {
        const uint64_t parent = multiFenwickParent(j);
        if (parent > capacity) {
            continue;
        }

        /* A block of elements whose sum has no 64-bit value refuses the
         * whole array (the cleaner frees the partial tree). */
        multiFenwickCell cell;
        if (!multiFenwickCellEncode(
                multiFenwickNumAdd_(multiFenwickFlatCellGet_(mfw, parent - 1),
                                    multiFenwickFlatCellGet_(mfw, j - 1)),
                &cell)) {
            return nullptr;
        }

        multiFenwickCellsPut(&mfw->cells, capacity, parent - 1, cell);
    }

    multiFenwickFlat *out = mfw;
    mfw = nullptr;
    return out;
}

void multiFenwickFlatFree(multiFenwickFlat *mfw) {
    if (!mfw) {
        return;
    }

    multiFenwickCellsFree(&mfw->cells);
    zfree(mfw);
}

/* Grow the tree's cells to 'newCapacity' (a power of two above the
 * current capacity), keeping every element's value.
 *
 * A new cell j in (oldCapacity, newCapacity] covers (j - LSB(j), j],
 * which reaches an existing element only when j is a power of two, and
 * then holds exactly the total: a copy of cell oldCapacity.  Every other
 * new cell is zero. */
static void multiFenwickFlatGrow_(multiFenwickFlat *mfw, uint64_t newCapacity) {
    const uint64_t oldCapacity = mfw->capacity;
    multiFenwickFlatCellsGrow_(mfw, newCapacity);
    for (uint64_t j = oldCapacity * 2; oldCapacity > 0 && j <= newCapacity;
         j <<= 1) {
        multiFenwickCellsCopy(&mfw->cells, j - 1, oldCapacity - 1);
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
    if (tree && multiFenwickCellsAllInt(&tree->cells) && !delta.isFloat &&
        idx < tree->capacity) {
        int64_t fast[64];
        size_t n = 0;
        bool fits = true;
        for (uint64_t i = idx + 1; i <= tree->capacity;
             i = multiFenwickParent(i)) {
            const __int128_t v =
                (__int128_t)multiFenwickCellsInt(&tree->cells, i - 1) + delta.i;
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
                multiFenwickCellsPutInt(&tree->cells, i - 1, fast[n++]);
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
    if (!dkDenseGrowthAllowed(oldCapacity, newCapacity)) {
        return false;
    }

    const multiFenwickNum total =
        newCapacity > oldCapacity && oldCapacity > 0
            ? multiFenwickFlatCellGet_(cur, oldCapacity - 1)
            : (multiFenwickNum){};

    /* A chain has at most one cell per bit of the capacity */
    multiFenwickCell planned[64];
    size_t steps = 0;
    for (uint64_t i = idx + 1; i <= newCapacity; i = multiFenwickParent(i)) {
        multiFenwickNum base = {};
        if (i <= oldCapacity) {
            base = multiFenwickFlatCellGet_(cur, i - 1);
        } else if (oldCapacity > 0 && stdc_has_single_bit(i)) {
            base = total;
        }

        assert(steps < countof(planned));
        if (!multiFenwickCellEncode(multiFenwickNumAdd_(base, delta),
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
        multiFenwickFlatGrow_(tree, newCapacity);
    }

    steps = 0;
    for (uint64_t i = idx + 1; i <= tree->capacity; i = multiFenwickParent(i)) {
        multiFenwickCellsPut(&tree->cells, tree->capacity, i - 1,
                             planned[steps++]);
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
    const multiFenwickNum lo =
        left > 0 ? multiFenwickFlatPrefix_(mfw, left - 1) : (multiFenwickNum){};
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
                                        : (multiFenwickNum){};
    return multiFenwickFlatApply_(mfw, idx, multiFenwickNumSub_(want, current));
}

size_t multiFenwickFlatCount(const multiFenwickFlat *mfw) {
    return mfw ? mfw->count : 0;
}

size_t multiFenwickFlatBytes(const multiFenwickFlat *mfw) {
    if (!mfw) {
        return 0;
    }

    return sizeof(*mfw) + multiFenwickCellsBytes(&mfw->cells, mfw->capacity);
}

/* Lower bound: binary lifting down the implicit tree, O(log n) cell reads,
 * the same algorithm and results as multiFenwickLowerBound (see there for
 * why the result is a crossing, and the smallest one for non-negative
 * elements).  With every cell an int64 and an integer target, the
 * running sum stays in int128. */
size_t multiFenwickFlatLowerBound(const multiFenwickFlat *mfw,
                                  const databox *target) {
    multiFenwickNum want;
    if (!mfw || !target || mfw->count == 0 ||
        !multiFenwickNumFrom_(target, &want)) {
        return SIZE_MAX;
    }

    /* The largest step that fits; each later step halves it */
    const uint64_t top = stdc_bit_floor((uint64_t)mfw->count);
    const multiFenwickCells *c = &mfw->cells;
    uint64_t pos = 0;
    if (multiFenwickCellsAllInt(c) && !want.isFloat) {
        __int128_t sum = 0;
        for (uint64_t bit = top; bit > 0; bit >>= 1) {
            const uint64_t next = pos + bit;
            if (next <= mfw->count) {
                const __int128_t withNext =
                    sum + multiFenwickCellsInt(c, next - 1);
                if (withNext < want.i) {
                    pos = next;
                    sum = withNext;
                }
            }
        }
    } else {
        multiFenwickNum sum = {};
        for (uint64_t bit = top; bit > 0; bit >>= 1) {
            const uint64_t next = pos + bit;
            if (next <= mfw->count) {
                const multiFenwickNum withNext =
                    multiFenwickNumAdd_(sum, multiFenwickCellsGet(c, next - 1));
                if (multiFenwickNumCompare_(withNext, want) < 0) {
                    pos = next;
                    sum = withNext;
                }
            }
        }
    }

    return pos < mfw->count ? (size_t)pos : SIZE_MAX;
}

void multiFenwickFlatClear(multiFenwickFlat *mfw) {
    if (!mfw || mfw->capacity == 0) {
        return;
    }

    /* Every cell becomes the int64 0, so the kinds array is not needed */
    multiFenwickCellsClear(&mfw->cells, mfw->capacity);
}

/* ====================================================================
 * Persistence
 * ====================================================================
 * As multiFenwick's: flex [capacity, count, cell 0, ..., cell
 * capacity-1], every cell as its value (SIGNED/UNSIGNED_64 by value, a
 * double cell as a float).  WAL: NUM_INCR / UPDATE [index, value],
 * CLEAR. */

static uint8_t *multiFenwickFlatPersistSnapshot_(const void *structure,
                                                 size_t *len) {
    const multiFenwickFlat *mfw = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox capacity = DATABOX_UNSIGNED(mfw->capacity);
    const databox count = DATABOX_UNSIGNED(mfw->count);
    flexPushByTypeExact(&f, &capacity, FLEX_ENDPOINT_TAIL);
    flexPushByTypeExact(&f, &count, FLEX_ENDPOINT_TAIL);

    /* 64 cells per flex growth */
    constexpr int BATCH = 64;
    databox boxes[BATCH];
    const databox *ptrs[BATCH];
    for (uint64_t at = 0; at < mfw->capacity; at += BATCH) {
        const uint64_t n =
            mfw->capacity - at < BATCH ? mfw->capacity - at : BATCH;
        for (uint64_t i = 0; i < n; i++) {
            /* A stored cell always boxes */
            [[maybe_unused]] const bool ok = multiFenwickNumBox_(
                multiFenwickCellsGet(&mfw->cells, at + i), &boxes[i]);
            assert(ok);
            ptrs[i] = &boxes[i];
        }

        flexAppendMultipleExact(&f, (uint_fast32_t)n, ptrs);
    }

    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *multiFenwickFlatPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    databoxBig box;
    uint64_t header[2];
    for (size_t h = 0; h < 2 && h < n; h++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        header[h] = box.type == DATABOX_UNSIGNED_64 ? box.data.u64 : UINT64_MAX;
    }

    const uint64_t capacity = n >= 2 ? header[0] : UINT64_MAX;
    const uint64_t count = n >= 2 ? header[1] : UINT64_MAX;
    if (n < 2 || capacity != n - 2 || count > capacity ||
        (capacity & (capacity - 1)) || (count == 0) != (capacity == 0) ||
        capacity > MULTIFENWICK_FLAT_INDEX_MAX + 1) {
        return nullptr;
    }

    cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw = multiFenwickFlatNew();
    if (capacity) {
        multiFenwickCellsGrow(&mfw->cells, 0, capacity);
    }

    mfw->capacity = capacity;
    mfw->count = count;
    for (uint64_t i = 0; i < capacity; i++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        multiFenwickNum num;
        multiFenwickCell cell;
        if (!multiFenwickNumFrom_((const databox *)&box, &num) ||
            !multiFenwickCellEncode(num, &cell)) {
            return nullptr;
        }

        multiFenwickCellsPut(&mfw->cells, capacity, i, cell);
    }

    multiFenwickFlat *out = mfw;
    mfw = nullptr;
    return out;
}

static uint8_t *multiFenwickFlatPersistEncodeOp_(persistOp op, const void *args,
                                                 size_t argc, size_t *len) {
    *len = 0;
    if (op == PERSIST_OP_CLEAR) {
        return persistOpNoBody(len);
    }
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) || argc != 2) {
        return nullptr; /* nothing else is replayed */
    }
    return persistOpEncodeBoxes(args, argc, len);
}

static bool multiFenwickFlatPersistApplyOp_(void *structure, persistOp op,
                                            const uint8_t *data, size_t len) {
    multiFenwickFlat **mfw = structure;
    if (op == PERSIST_OP_CLEAR) {
        multiFenwickFlatClear(*mfw);
        return true;
    }

    size_t idx;
    databoxBig value;
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) ||
        !persistOpDecodeIndexValue(data, len, &idx, &value)) {
        return false;
    }

    return op == PERSIST_OP_NUM_INCR
               ? multiFenwickFlatUpdate(mfw, idx, (const databox *)&value)
               : multiFenwickFlatSet(mfw, idx, (const databox *)&value);
}

static void *multiFenwickFlatPersistNewEmpty_(void) {
    /* @own: caller; free=multiFenwickFlatFree */
    return multiFenwickFlatNew();
}

static size_t multiFenwickFlatPersistCount_(const void *structure) {
    return multiFenwickFlatCount(structure);
}

static size_t multiFenwickFlatPersistBytes_(const void *structure) {
    return multiFenwickFlatBytes(structure);
}

static void multiFenwickFlatPersistFree_(void *structure) {
    multiFenwickFlatFree(structure);
}

const persistOps persistOpsMultiFenwickFlat = {
    .type = PERSIST_TYPE_MULTIFENWICK_FLAT,
    .name = "multiFenwickFlat",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = multiFenwickFlatPersistSnapshot_,
    .restore = multiFenwickFlatPersistRestore_,
    .newEmpty = multiFenwickFlatPersistNewEmpty_,
    .count = multiFenwickFlatPersistCount_,
    .estimateSize = multiFenwickFlatPersistBytes_,
    .encodeOp = multiFenwickFlatPersistEncodeOp_,
    .applyOp = multiFenwickFlatPersistApplyOp_,
    .free = multiFenwickFlatPersistFree_,
};

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
