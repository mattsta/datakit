#include "multiFenwick.h"
#include "cleaner.h"
#include "datakit.h"
#include "dkDenseGrowth.h"
#include "flexCapacityManagement.h"
#include "mflexInternal.h"
#include "multiFenwickCommon.h"
#include "multilist.h"
#include "persist.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiFenwick *, multiFenwickFree);
defcleaner(mflexState *, mflexStateFree);
defcleaner(flex *, flexFree);
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

/* Arithmetic is multiFenwickNum (multiFenwickCommon.h).  Integer cells come
 * back from storage as SIGNED_64 or UNSIGNED_64 by value (flex stores
 * integers canonically); float cells come back as FLOAT_32 or DOUBLE_64
 * (flex keeps a double in a float when that loses nothing). */

/* ====================================================================
 * Cells
 * ==================================================================== */

/* Read the BIT cell at 0-based position 'pos'.  Cells only ever hold
 * values multiFenwickNumFrom_ accepts; anything else (such as a deferred
 * 96/128-bit box with no payload) is still refused here, so it can never
 * reach the arithmetic. */
/* A read's last located cell: the next lookup walks from it when it is
 * nearer than either end of the list (multilistIndexNear), as a Fenwick
 * walk's cells are.  Valid until the tree changes, so only a read keeps
 * one; NULL looks up from the ends. */
typedef struct multiFenwickCursor_ {
    multilistEntry at;
    uint64_t pos;
    bool valid;
} multiFenwickCursor_;

static bool multiFenwickCellGet_(const multiFenwick *mfw, uint64_t pos,
                                 multiFenwickCursor_ *cursor,
                                 multiFenwickNum *n) {
    multilistEntry entry;
    const bool found =
        cursor && cursor->valid
            ? multilistIndexNear(mfw->tree, mfw->state, (mlOffsetId)pos,
                                 &cursor->at, (mlOffsetId)cursor->pos, &entry,
                                 true)
            : multilistIndex(mfw->tree, mfw->state, (mlOffsetId)pos, &entry,
                             true);
    if (!found) {
        return false;
    }

    if (cursor) {
        *cursor = (multiFenwickCursor_){.at = entry, .pos = pos, .valid = true};
    }

    return multiFenwickNumFrom_(&entry.box, n);
}

/* Sum of elements [0, idx], for idx < count. */
static bool multiFenwickPrefix_(const multiFenwick *mfw, uint64_t idx,
                                multiFenwickNum *sum) {
    multiFenwickNum acc = {};
    multiFenwickCursor_ cursor = {};
    for (uint64_t i = idx + 1; i > 0; i = multiFenwickPrev(i)) {
        multiFenwickNum cell;
        if (!multiFenwickCellGet_(mfw, i - 1, &cursor, &cell)) {
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
    multiFenwickCursor_ cursor = {};
    if (!multiFenwickCellGet_(mfw, i - 1, &cursor, &acc)) {
        return false;
    }

    const uint64_t stop = multiFenwickPrev(i);
    for (uint64_t j = i - 1; j > stop; j = multiFenwickPrev(j)) {
        multiFenwickNum cell;
        if (!multiFenwickCellGet_(mfw, j - 1, &cursor, &cell)) {
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
        return nullptr;
    }

    /* The cells live in a multilist at the family's measured node limit
     * (dkTierLimit.h): every prefix walk reads ~log2(n) cells, and each
     * read walks entries within one node, so the node size sets the cost
     * of a read */
    /* @own: container; free=multilistFree */
    mfw->tree = multilistNewLimit(dkTierDefaultLimit(DK_TIER_MULTILIST), 0);
    if (!mfw->tree) {
        return nullptr;
    }

    mfw->count = 0;
    mfw->capacity = 0;

    /* Allocate state */
    /* @own: container; free=mflexStateFree */
    mfw->state = zcalloc(1, sizeof(mflexState));
    if (!mfw->state) {
        return nullptr;
    }

    multiFenwick *out = mfw;
    mfw = nullptr;
    return out;
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
        return nullptr;
    }

    for (size_t i = 0; i < count; i++) {
        multiFenwickNum n;
        if (!multiFenwickNumFrom_(&values[i], &n)) {
            return nullptr;
        }
    }

    cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
    if (!mfw) {
        return nullptr;
    }

    const uint64_t capacity = multiFenwickCapacityFor_(0, count - 1);
    for (uint64_t i = 0; i < capacity; i++) {
        databox cell = DATABOX_UNSIGNED(0);
        if (i < count) {
            multiFenwickNum n;
            [[maybe_unused]] const bool ok =
                multiFenwickNumFrom_(&values[i], &n) &&
                multiFenwickNumBox_(n, &cell);
            assert(ok); /* one checked element always boxes */
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
        if (!multiFenwickCellGet_(mfw, j - 1, nullptr, &child) ||
            !multiFenwickCellGet_(mfw, parent - 1, nullptr, &sum)) {
            return nullptr;
        }

        /* A block of elements whose sum has no 64-bit value refuses the
         * whole array (the cleaner frees the partial tree). */
        databox box;
        if (!multiFenwickNumBox_(multiFenwickNumAdd_(sum, child), &box)) {
            return nullptr;
        }

        [[maybe_unused]] const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)(parent - 1), &box);
        assert(replaced);
    }

    multiFenwick *out = mfw;
    mfw = nullptr;
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
        [[maybe_unused]] const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)(j - 1), total);
        assert(replaced);
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
    if (!dkDenseGrowthAllowed(oldCapacity, newCapacity)) {
        return false;
    }

    multiFenwickNum total = {};
    databox totalBox = DATABOX_UNSIGNED(0);
    if (newCapacity > oldCapacity && oldCapacity > 0) {
        if (!multiFenwickCellGet_(cur, oldCapacity - 1, nullptr, &total)) {
            return false;
        }

        [[maybe_unused]] const bool boxed =
            multiFenwickNumBox_(total, &totalBox);
        assert(boxed); /* a stored cell always boxes */
    }

    /* A chain has at most one cell per bit of the capacity */
    databox planned[64];
    size_t steps = 0;
    for (uint64_t i = idx + 1; i <= newCapacity; i = multiFenwickParent(i)) {
        multiFenwickNum base = {};
        if (i <= oldCapacity) {
            if (!multiFenwickCellGet_(cur, i - 1, nullptr, &base)) {
                return false;
            }
        } else if (oldCapacity > 0 && stdc_has_single_bit(i)) {
            base = total;
        }

        assert(steps < countof(planned));
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
        [[maybe_unused]] const bool replaced = multilistReplaceByTypeAtIndex(
            &tree->tree, tree->state, (mlNodeId)(i - 1), &planned[steps++]);
        assert(replaced);
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
    multiFenwickNum lo = {};
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
    multiFenwickNum current = {};
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

/* Lower bound: binary lifting down the implicit tree, O(log n) cell reads.
 *
 * From the largest power of two at or below the count, each step tries
 * the block of cells after 'pos' and takes it while the running sum (a
 * prefix sum) stays below the target.  So prefix(pos) < target always
 * (or pos is 0), and the single element after the final pos was not
 * taken: prefix(pos + 1) >= target.  The result pos is therefore a
 * crossing (prefix(i) >= target, and i == 0 or prefix(i - 1) < target),
 * the smallest one when the elements are non-negative; it is SIZE_MAX
 * when the lifting runs past the count. */
size_t multiFenwickLowerBound(const multiFenwick *mfw, const databox *target) {
    multiFenwickNum want;
    if (!mfw || !target || mfw->count == 0 ||
        !multiFenwickNumFrom_(target, &want)) {
        return SIZE_MAX;
    }

    uint64_t pos = 0;
    multiFenwickNum sum = {};
    multiFenwickCursor_ cursor = {};
    for (uint64_t bit = stdc_bit_floor((uint64_t)mfw->count); bit > 0;
         bit >>= 1) {
        const uint64_t next = pos + bit;
        if (next > mfw->count) {
            continue;
        }

        multiFenwickNum cell;
        if (!multiFenwickCellGet_(mfw, next - 1, &cursor, &cell)) {
            return SIZE_MAX;
        }

        const multiFenwickNum withNext = multiFenwickNumAdd_(sum, cell);
        if (multiFenwickNumCompare_(withNext, want) < 0) {
            pos = next;
            sum = withNext;
        }
    }

    return pos < mfw->count ? (size_t)pos : SIZE_MAX;
}

/* Clear all values to zero */
void multiFenwickClear(multiFenwick *mfw) {
    if (!mfw || !mfw->tree) {
        return;
    }

    const databox zero = DATABOX_UNSIGNED(0);
    for (uint64_t i = 0; i < mfw->capacity; i++) {
        [[maybe_unused]] const bool replaced = multilistReplaceByTypeAtIndex(
            &mfw->tree, mfw->state, (mlNodeId)i, &zero);
        assert(replaced);
    }
}

/* ====================================================================
 * Persistence
 * ====================================================================
 * Snapshot: flex [capacity, count, cell 0, ..., cell capacity-1], each
 * cell as stored.  Cells, not elements: a cell that has taken a double
 * holds rounding the elements do not record, so restore keeps every
 * cell as it was.  WAL (args: const databox *const *, two of them):
 * NUM_INCR [index, delta] (Update), UPDATE [index, value] (Set), CLEAR. */

static uint8_t *multiFenwickPersistSnapshot_(const void *structure,
                                             size_t *len) {
    const multiFenwick *mfw = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox capacity = DATABOX_UNSIGNED(mfw->capacity);
    const databox count = DATABOX_UNSIGNED(mfw->count);
    flexPushByTypeExact(&f, &capacity, FLEX_ENDPOINT_TAIL);
    flexPushByTypeExact(&f, &count, FLEX_ENDPOINT_TAIL);

    if (mfw->capacity) {
        cleaner(mflexStateFree) mflexState *s0 = mflexStateCreate();
        cleaner(mflexStateFree) mflexState *s1 = mflexStateCreate();
        mflexState *s[2] = {s0, s1};
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(mfw->tree, s, &iter);

        /* 64 cells per flex growth */
        constexpr int BATCH = 64;
        databox boxes[BATCH];
        const databox *ptrs[BATCH];
        size_t used = 0;
        multilistEntry entry;
        while (multilistNext(&iter, &entry)) {
            boxes[used] = entry.box;
            ptrs[used] = &boxes[used];
            if (++used == BATCH) {
                flexAppendMultipleExact(&f, (uint_fast32_t)used, ptrs);
                used = 0;
            }
        }

        if (used) {
            flexAppendMultipleExact(&f, (uint_fast32_t)used, ptrs);
        }

        multilistIteratorRelease(&iter);
    }

    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *multiFenwickPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t capacity;
    uint64_t count;
    if (n < 2 || !flexReadUnsigned(f, &fe, &capacity) ||
        !flexReadUnsigned(f, &fe, &count) || capacity != n - 2 ||
        count > capacity || (capacity & (capacity - 1)) ||
        (count == 0) != (capacity == 0) ||
        capacity > MULTIFENWICK_INDEX_MAX + 1) {
        return nullptr;
    }

    cleaner(multiFenwickFree) multiFenwick *mfw = multiFenwickNew();
    if (!mfw) {
        return nullptr;
    }

    /* Every cell must be a value a cell can hold, stored as the tree
     * stores it.  The cells are gathered 64 at a time into one flex, then
     * cut into the multilist's nodes in one pass. */
    cleaner(flexFree) flex *cells = flexNew();
    constexpr int BATCH = 64;
    databox boxes[BATCH];
    const databox *ptrs[BATCH];
    for (uint64_t at = 0; at < capacity; at += BATCH) {
        const uint64_t batch = capacity - at < BATCH ? capacity - at : BATCH;
        for (uint64_t i = 0; i < batch; i++) {
            databoxBig box;
            flexGetByTypeBig(fe, &box);
            fe = flexNext(f, fe);
            multiFenwickNum num;
            if (!multiFenwickNumFrom_((const databox *)&box, &num) ||
                !multiFenwickNumBox_(num, &boxes[i])) {
                return nullptr;
            }

            ptrs[i] = &boxes[i];
        }

        /* Exact: a restored cell keeps its type (a whole double stays a
         * double) */
        flexAppendMultipleExact(&cells, (uint_fast32_t)batch, ptrs);
    }

    multilistFree(mfw->tree);
    /* @own: container; free=multilistFree */
    mfw->tree = multilistNewFromDump(dkTierDefaultLimit(DK_TIER_MULTILIST), 0,
                                     mfw->state, cells);
    cells = nullptr; /* taken by the multilist */
    mfw->capacity = capacity;
    mfw->count = count;

    multiFenwick *out = mfw;
    mfw = nullptr;
    return out;
}

static uint8_t *multiFenwickPersistEncodeOp_(persistOp op, const void *args,
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

static bool multiFenwickPersistApplyOp_(void *structure, persistOp op,
                                        const uint8_t *data, size_t len) {
    multiFenwick **mfw = structure;
    if (op == PERSIST_OP_CLEAR) {
        multiFenwickClear(*mfw);
        return true;
    }

    size_t idx;
    databoxBig value;
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) ||
        !persistOpDecodeIndexValue(data, len, &idx, &value)) {
        return false;
    }

    /* Update and Set refuse every value a tree does not take */
    return op == PERSIST_OP_NUM_INCR
               ? multiFenwickUpdate(mfw, idx, (const databox *)&value)
               : multiFenwickSet(mfw, idx, (const databox *)&value);
}

static void *multiFenwickPersistNewEmpty_(void) {
    /* @own: caller; free=multiFenwickFree */
    return multiFenwickNew();
}

static size_t multiFenwickPersistCount_(const void *structure) {
    return multiFenwickCount(structure);
}

static size_t multiFenwickPersistBytes_(const void *structure) {
    return multiFenwickBytes(structure);
}

static void multiFenwickPersistFree_(void *structure) {
    multiFenwickFree(structure);
}

const persistOps persistOpsMultiFenwick = {
    .type = PERSIST_TYPE_MULTIFENWICK,
    .name = "multiFenwick",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = multiFenwickPersistSnapshot_,
    .restore = multiFenwickPersistRestore_,
    .newEmpty = multiFenwickPersistNewEmpty_,
    .count = multiFenwickPersistCount_,
    .estimateSize = multiFenwickPersistBytes_,
    .encodeOp = multiFenwickPersistEncodeOp_,
    .applyOp = multiFenwickPersistApplyOp_,
    .free = multiFenwickPersistFree_,
};

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
