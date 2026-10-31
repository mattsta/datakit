/* ====================================================================
 * Segment Tree Dispatcher Template (2-TIER SYSTEM): the public API
 * ====================================================================
 *
 * Included once per value type (segment<Suffix>.c) to generate the public
 * functions declared in segment<Suffix>.h.  A tree is a tagged pointer to
 * its Small tier (one contiguous allocation) or, past the Small tier's
 * limits, its Full tier.
 *
 * REQUIRED DEFINES:
 *   SEGMENT_SUFFIX     - Type suffix (I64, U32, Double, etc.)
 *   SEGMENT_VALUE_TYPE - C type for values
 *
 * Contract (segmentCore.h): Update assigns one element, extending the
 * tree (gaps hold the operation's identity); on a NULL tree it creates a
 * SUM tree.  RangeUpdate assigns the existing elements of a range and
 * never extends the tree.  Query and Get outside the tree return the
 * identity (0 for a NULL tree). */

#define SEGMENT_D_FN_(name) SEGMENT_NAME(SEGMENT_SUFFIX, name)
#define SEGMENT_D_STR2_(x) #x
#define SEGMENT_D_STR_(x) SEGMENT_D_STR2_(x)
#include "../dkDenseGrowth.h"

#define SEGMENT_D_SMALL_ SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)
#define SEGMENT_D_FULL_ SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX)

/* Every type tags Small as 1 and Full as 2 in the low two bits */
#define SEGMENT_D_TAG_SMALL_ ((uintptr_t)1)
#define SEGMENT_D_TAG_FULL_ ((uintptr_t)2)
#define SEGMENT_D_TAG_MASK_ ((uintptr_t)3)
#define SEGMENT_D_IS_SMALL_(seg)                                               \
    (((uintptr_t)(seg) & SEGMENT_D_TAG_MASK_) == SEGMENT_D_TAG_SMALL_)
#define SEGMENT_D_UNTAG_(seg)                                                  \
    ((void *)((uintptr_t)(seg) & ~SEGMENT_D_TAG_MASK_))
#define SEGMENT_D_TAG_(ptr, tag) ((void *)((uintptr_t)(ptr) | (tag)))

void *SEGMENT_D_FN_(New)(segmentOp op) {
    return SEGMENT_D_TAG_(SEGMENT_D_FN_(SmallNew)(op), SEGMENT_D_TAG_SMALL_);
}

void SEGMENT_D_FN_(Free)(void *seg) {
    if (!seg) {
        return;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        SEGMENT_D_FN_(SmallFree)((SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg));
    } else {
        SEGMENT_D_FN_(FullFree)((SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg));
    }
}

size_t SEGMENT_D_FN_(Count)(const void *seg) {
    if (!seg) {
        return 0;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        return SEGMENT_D_FN_(SmallCount)(
            (const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg));
    }

    return (size_t)SEGMENT_D_FN_(FullCount)(
        (const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg));
}

/* Assign element idx; moves a Small tree to the Full tier first when the
 * Small tier's limits would be passed or idx jumps far past the end.
 * False, changing nothing, for a refused write (a tree created here for it
 * is freed again). */
bool SEGMENT_D_FN_(Update)(void **seg, size_t idx, SEGMENT_VALUE_TYPE value) {
    if (!seg) {
        return false;
    }

    /* A far index is refused before any tree is made or moved for it (the
     * capacity counts nodes: twice the leaves) */
    const uint64_t leaves =
        !*seg ? 0
        : SEGMENT_D_IS_SMALL_(*seg)
            ? ((const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(*seg))->capacity / 2
            : ((const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(*seg))->capacity / 2;
    if (!dkDenseIndexAllowed(leaves, idx)) {
        return false;
    }

    const bool created = !*seg;
    if (created) {
        *seg = SEGMENT_D_FN_(New)(SEGMENT_OP_SUM);
    }

    if (SEGMENT_D_IS_SMALL_(*seg)) {
        SEGMENT_D_SMALL_ *small = SEGMENT_D_UNTAG_(*seg);
        if (SEGMENT_D_FN_(SmallShouldUpgrade)(small) ||
            idx >= (size_t)SEGMENT_D_FN_(SmallCount)(small) + 1000 ||
            idx * 2 * sizeof(SEGMENT_VALUE_TYPE) > SEGMENT_SMALL_BYTES_MAX) {
            SEGMENT_D_FULL_ *full = SEGMENT_D_FN_(FullFromSmall)(small);
            SEGMENT_D_FN_(SmallFree)(small);
            *seg = SEGMENT_D_TAG_(full, SEGMENT_D_TAG_FULL_);
        }
    }

    bool success = false;
    if (SEGMENT_D_IS_SMALL_(*seg)) {
        /* idx < 256 KiB / (2 * sizeof(value)) here, so it fits uint32_t */
        SEGMENT_D_SMALL_ *small = SEGMENT_D_FN_(SmallUpdate)(
            SEGMENT_D_UNTAG_(*seg), (uint32_t)idx, value, &success);
        *seg = SEGMENT_D_TAG_(small, SEGMENT_D_TAG_SMALL_);
    } else {
        SEGMENT_D_FN_(FullUpdate)
        (SEGMENT_D_UNTAG_(*seg), (uint64_t)idx, value, &success);
    }

    if (!success && created) {
        SEGMENT_D_FN_(Free)(*seg);
        *seg = nullptr;
    }

    return success;
}

/* Fold [left, right] with the tree's operation */
SEGMENT_VALUE_TYPE SEGMENT_D_FN_(Query)(const void *seg, size_t left,
                                        size_t right) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    const size_t count = SEGMENT_D_FN_(Count)(seg);
    if (right >= count || left > right) {
        /* Out of range or empty: the tier returns the identity (before
         * the Small tier's 32-bit bounds could cut a huge 'left' down to
         * a range that holds something) */
        right = count;
        left = count;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        return SEGMENT_D_FN_(SmallQuery)(
            (const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg), (uint32_t)left,
            (uint32_t)right);
    }

    return SEGMENT_D_FN_(FullQuery)(
        (const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg), (uint64_t)left,
        (uint64_t)right);
}

SEGMENT_VALUE_TYPE SEGMENT_D_FN_(Get)(const void *seg, size_t idx) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    const size_t count = SEGMENT_D_FN_(Count)(seg);
    if (idx > count) {
        idx = count; /* out of range: the tier returns the identity */
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        return SEGMENT_D_FN_(SmallGet)(
            (const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg), (uint32_t)idx);
    }

    return SEGMENT_D_FN_(FullGet)(
        (const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg), (uint64_t)idx);
}

size_t SEGMENT_D_FN_(Bytes)(const void *seg) {
    if (!seg) {
        return 0;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        return SEGMENT_D_FN_(SmallBytes)(
            (const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg));
    }

    return SEGMENT_D_FN_(FullBytes)(
        (const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg));
}

/* Assign 'value' to every existing element in [left, right].  Never
 * extends or moves the tree, so 'seg' stays valid. */
void SEGMENT_D_FN_(RangeUpdate)(void *seg, size_t left, size_t right,
                                SEGMENT_VALUE_TYPE value) {
    const size_t count = SEGMENT_D_FN_(Count)(seg);
    if (count == 0 || left > right || left >= count) {
        return;
    }

    if (right >= count) {
        right = count - 1;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        SEGMENT_D_FN_(SmallRangeUpdate)
        ((SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg), (uint32_t)left,
         (uint32_t)right, value);
    } else {
        SEGMENT_D_FN_(FullRangeUpdate)
        ((SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg), (uint64_t)left,
         (uint64_t)right, value);
    }
}

/* ====================================================================
 * Bulk build, element copy and persistence
 * ==================================================================== */
#include "../cleaner.h"
#include "../flex.h"
#include "../persist.h"
#include "segmentPersist.h" /* the persistOps declarations defined below */

defcleanerName(SEGMENT_VALUE_TYPE *, zfree, SegmentValues);

#define FPV_T SEGMENT_VALUE_TYPE
#define FPV_IS_FLOATING SEGMENT_IS_FLOATING
#define FPV_FN(name) SEGMENT_D_FN_(SEGMENT_CAT(Persist, name))
#include "../fenwick/fenwickPersistValueImpl.h"

void *SEGMENT_D_FN_(NewFromArray)(segmentOp op,
                                  const SEGMENT_VALUE_TYPE *values,
                                  size_t count) {
    if (!values || count == 0) {
        return SEGMENT_D_FN_(New)(op);
    }

    const uint64_t leaves = stdc_bit_ceil((uint64_t)count);

    /* The tier Update would keep it in (see Update) */
    if (2 * leaves * sizeof(SEGMENT_VALUE_TYPE) + sizeof(SEGMENT_D_SMALL_) <=
        SEGMENT_SMALL_BYTES_MAX) {
        SEGMENT_D_SMALL_ *small =
            SEGMENT_D_FN_(SmallNewFromArray)(values, (uint32_t)count, op);
        return SEGMENT_D_TAG_(small, SEGMENT_D_TAG_SMALL_);
    }

    SEGMENT_D_FULL_ *full =
        SEGMENT_D_FN_(FullNewFromArray)(values, (uint64_t)count, op);
    return SEGMENT_D_TAG_(full, SEGMENT_D_TAG_FULL_);
}

/* The leaves, count and operation of 'seg' */
static const SEGMENT_VALUE_TYPE *
SEGMENT_D_FN_(Leaves_)(const void *seg, uint64_t *count, segmentOp *op) {
    if (SEGMENT_D_IS_SMALL_(seg)) {
        const SEGMENT_D_SMALL_ *small = SEGMENT_D_UNTAG_(seg);
        *count = small->count;
        *op = small->operation;
        return &small->tree[small->capacity / 2];
    }

    const SEGMENT_D_FULL_ *full = SEGMENT_D_UNTAG_(seg);
    *count = full->count;
    *op = full->operation;
    return full->tree ? &full->tree[full->capacity / 2] : nullptr;
}

void SEGMENT_D_FN_(CopyTo)(const void *seg, SEGMENT_VALUE_TYPE *out) {
    if (!seg) {
        return;
    }

    uint64_t count;
    segmentOp op;
    const SEGMENT_VALUE_TYPE *leaves = SEGMENT_D_FN_(Leaves_)(seg, &count, &op);
    if (count) {
        memcpy(out, leaves, count * sizeof(*out));
    }
}

/* Persistence.  Every node is its children's fold, so the elements
 * determine the tree (float sums included).
 *   flex [operation, element 0, ..., element count-1]
 * WAL (args: const databox *const *): UPDATE [index, value] (Update),
 * UPDATE [left, right, value] (RangeUpdate). */
static uint8_t *SEGMENT_D_FN_(PersistSnapshot_)(const void *structure,
                                                size_t *len) {
    uint64_t count;
    segmentOp op;
    const SEGMENT_VALUE_TYPE *leaves =
        SEGMENT_D_FN_(Leaves_)(structure, &count, &op);

    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox operation = DATABOX_UNSIGNED((uint64_t)op);
    flexPushByTypeExact(&f, &operation, FLEX_ENDPOINT_TAIL);
    SEGMENT_D_FN_(PersistAppend_)(&f, leaves, count);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *SEGMENT_D_FN_(PersistRestore_)(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    if (n < 1) {
        return nullptr;
    }

    flexEntry *fe = flexHead(f);
    databoxBig box;
    flexGetByTypeBig(fe, &box);
    fe = flexNext(f, fe);
    /* SUM, MIN or MAX (MAX is the last operation) */
    if (box.type != DATABOX_UNSIGNED_64 ||
        box.data.u64 > (uint64_t)SEGMENT_OP_MAX) {
        return nullptr;
    }

    const segmentOp op = (segmentOp)box.data.u64;
    const size_t count = n - 1;
    cleanerName(zfree, SegmentValues) SEGMENT_VALUE_TYPE *values =
        zmalloc((count ? count : 1) * sizeof(*values));
    for (size_t i = 0; i < count; i++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        if (!SEGMENT_D_FN_(PersistUnbox_)(&box, &values[i])) {
            return nullptr;
        }
    }

    return SEGMENT_D_FN_(NewFromArray)(op, values, count);
}

static uint8_t *SEGMENT_D_FN_(PersistEncodeOp_)(persistOp op, const void *args,
                                                size_t argc, size_t *len) {
    *len = 0;
    if (op != PERSIST_OP_UPDATE || (argc != 2 && argc != 3)) {
        return nullptr;
    }

    return persistOpEncodeBoxes(args, argc, len);
}

static bool SEGMENT_D_FN_(PersistApplyOp_)(void *structure, persistOp op,
                                           const uint8_t *data, size_t len) {
    void **seg = structure;
    if (op != PERSIST_OP_UPDATE || !flexValidateBufferDeep(data, len)) {
        return false;
    }

    const flex *f = (const flex *)data;
    const size_t argc = flexCount(f);
    if (argc != 2 && argc != 3) {
        return false;
    }

    /* Two or three operands: the index (or range), then the value */
    databoxBig args[3];
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < argc; i++) {
        flexGetByTypeBig(fe, &args[i]);
        fe = flexNext(f, fe);
    }

    SEGMENT_VALUE_TYPE v;
    for (size_t i = 0; i + 1 < argc; i++) {
        if (args[i].type != DATABOX_UNSIGNED_64 ||
            args[i].data.u64 > SIZE_MAX) {
            return false;
        }
    }

    if (!SEGMENT_D_FN_(PersistUnbox_)(&args[argc - 1], &v)) {
        return false;
    }

    if (argc == 2) {
        return SEGMENT_D_FN_(Update)(seg, (size_t)args[0].data.u64, v);
    } else {
        SEGMENT_D_FN_(RangeUpdate)
        (*seg, (size_t)args[0].data.u64, (size_t)args[1].data.u64, v);
    }

    return true;
}

static void *SEGMENT_D_FN_(PersistNewEmpty_)(void) {
    return SEGMENT_D_FN_(New)(SEGMENT_OP_SUM);
}

const persistOps SEGMENT_CAT(persistOpsSegment, SEGMENT_SUFFIX) = {
    .type = SEGMENT_PERSIST_TYPE,
    .name = "segment" SEGMENT_D_STR_(SEGMENT_SUFFIX),
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = SEGMENT_D_FN_(PersistSnapshot_),
    .restore = SEGMENT_D_FN_(PersistRestore_),
    .newEmpty = SEGMENT_D_FN_(PersistNewEmpty_),
    .count = SEGMENT_D_FN_(Count),
    .estimateSize = SEGMENT_D_FN_(Bytes),
    .encodeOp = SEGMENT_D_FN_(PersistEncodeOp_),
    .applyOp = SEGMENT_D_FN_(PersistApplyOp_),
    .free = SEGMENT_D_FN_(Free),
};

#ifdef DATAKIT_TEST
void SEGMENT_D_FN_(Repr)(const void *seg) {
    if (!seg) {
        printf("segment" SEGMENT_D_STR_(SEGMENT_SUFFIX) ": (nil)\n");
        return;
    }

    if (SEGMENT_D_IS_SMALL_(seg)) {
        SEGMENT_D_FN_(SmallRepr)(
            (const SEGMENT_D_SMALL_ *)SEGMENT_D_UNTAG_(seg));
    } else {
        SEGMENT_D_FN_(FullRepr)((const SEGMENT_D_FULL_ *)SEGMENT_D_UNTAG_(seg));
    }
}
#endif

#undef SEGMENT_D_FN_
#undef SEGMENT_D_STR2_
#undef SEGMENT_D_STR_
#undef SEGMENT_D_SMALL_
#undef SEGMENT_D_FULL_
#undef SEGMENT_D_TAG_SMALL_
#undef SEGMENT_D_TAG_FULL_
#undef SEGMENT_D_TAG_MASK_
#undef SEGMENT_D_IS_SMALL_
#undef SEGMENT_D_UNTAG_
#undef SEGMENT_D_TAG_
#undef SEGMENT_SUFFIX
#undef SEGMENT_VALUE_TYPE
#undef SEGMENT_IS_FLOATING
#undef SEGMENT_PERSIST_TYPE
