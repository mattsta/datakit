/* ====================================================================
 * Fenwick Tree Dispatcher Template (2-TIER SYSTEM): the public API
 * ====================================================================
 *
 * Included once per value type (fenwick<Suffix>.c) to generate the public
 * functions declared in fenwick<Suffix>.h.  A tree is a tagged pointer to
 * its Small tier (one contiguous allocation) or, past the Small tier's
 * limits, its Full tier.
 *
 * REQUIRED DEFINES:
 *   FENWICK_SUFFIX     - Type suffix (I64, U32, Double, etc.)
 *   FENWICK_VALUE_TYPE - C type for values
 *   FENWICK_IS_FLOATING - 1 for float types, 0 for integers
 *
 * Contract (see fenwickCore.h, "VALUES"): Update and Set refuse an
 * element that would not fit the type (float trees: a non-finite delta,
 * value or element) and then change nothing (a NULL tree stays NULL);
 * Query, RangeQuery and Get past the count return zero. */

#include <math.h>

#define FENWICK_D_FN_(name) FENWICK_NAME(FENWICK_SUFFIX, name)
#define FENWICK_D_STR2_(x) #x
#define FENWICK_D_STR_(x) FENWICK_D_STR2_(x)
#define FENWICK_D_SMALL_ FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)
#define FENWICK_D_FULL_ FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)

/* Every type tags Small as 1 and Full as 2 in the low two bits */
#define FENWICK_D_TAG_SMALL_ ((uintptr_t)1)
#define FENWICK_D_TAG_FULL_ ((uintptr_t)2)
#define FENWICK_D_TAG_MASK_ ((uintptr_t)3)
#define FENWICK_D_IS_SMALL_(fw)                                                \
    (((uintptr_t)(fw) & FENWICK_D_TAG_MASK_) == FENWICK_D_TAG_SMALL_)
#define FENWICK_D_UNTAG_(fw) ((void *)((uintptr_t)(fw) & ~FENWICK_D_TAG_MASK_))
#define FENWICK_D_TAG_(ptr, tag) ((void *)((uintptr_t)(ptr) | (tag)))

/* Create new tree - starts as small tier */
void *FENWICK_D_FN_(New)(void) {
    return FENWICK_D_TAG_(FENWICK_D_FN_(SmallNew)(), FENWICK_D_TAG_SMALL_);
}

void FENWICK_D_FN_(Free)(void *fw) {
    if (!fw) {
        return;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        FENWICK_D_FN_(SmallFree)((FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullFree)((FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }
}

/* A write at idx: moves a Small tree to the Full tier first when the
 * Small tier's limits would be passed or idx jumps far past the end, then
 * applies Update (isSet false) or Set.  A tree created here for a refused
 * write is freed again. */
static bool FENWICK_D_FN_(Write_)(void **fw, size_t idx, FENWICK_VALUE_TYPE v,
                                  bool isSet) {
    if (!fw) {
        return false;
    }

#if FENWICK_IS_FLOATING
    /* A non-finite delta or value is always refused: say so before
     * creating or upgrading anything */
    if (!isfinite(v)) {
        return false;
    }
#endif

    const bool created = !*fw;
    if (created) {
        *fw = FENWICK_D_FN_(New)();
    }

    if (FENWICK_D_IS_SMALL_(*fw)) {
        FENWICK_D_SMALL_ *small = FENWICK_D_UNTAG_(*fw);
        if (FENWICK_D_FN_(SmallShouldUpgrade)(small) ||
            idx >= (size_t)FENWICK_D_FN_(SmallCount)(small) + 1000 ||
            idx * sizeof(FENWICK_VALUE_TYPE) > 128 * 1024) {
            FENWICK_D_FULL_ *full = FENWICK_D_FN_(FullFromSmall)(small);
            FENWICK_D_FN_(SmallFree)(small);
            *fw = FENWICK_D_TAG_(full, FENWICK_D_TAG_FULL_);
        }
    }

    bool success = false;
    if (FENWICK_D_IS_SMALL_(*fw)) {
        /* idx < 128 KiB / sizeof(value) here, so it fits uint32_t */
        FENWICK_D_SMALL_ *small = FENWICK_D_UNTAG_(*fw);
        small =
            isSet
                ? FENWICK_D_FN_(SmallSet)(small, (uint32_t)idx, v, &success)
                : FENWICK_D_FN_(SmallUpdate)(small, (uint32_t)idx, v, &success);
        *fw = FENWICK_D_TAG_(small, FENWICK_D_TAG_SMALL_);
    } else {
        FENWICK_D_FULL_ *full = FENWICK_D_UNTAG_(*fw);
        if (isSet) {
            FENWICK_D_FN_(FullSet)(full, (uint64_t)idx, v, &success);
        } else {
            FENWICK_D_FN_(FullUpdate)(full, (uint64_t)idx, v, &success);
        }
    }

    if (!success && created) {
        FENWICK_D_FN_(Free)(*fw);
        *fw = NULL;
    }

    return success;
}

bool FENWICK_D_FN_(Update)(void **fw, size_t idx, FENWICK_VALUE_TYPE delta) {
    return FENWICK_D_FN_(Write_)(fw, idx, delta, false);
}

bool FENWICK_D_FN_(Set)(void **fw, size_t idx, FENWICK_VALUE_TYPE value) {
    return FENWICK_D_FN_(Write_)(fw, idx, value, true);
}

size_t FENWICK_D_FN_(Count)(const void *fw) {
    if (!fw) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallCount)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    }

    return (size_t)FENWICK_D_FN_(FullCount)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
}

/* Query prefix sum [0, idx] */
FENWICK_VALUE_TYPE FENWICK_D_FN_(Query)(const void *fw, size_t idx) {
    if (idx >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallQuery)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)idx);
    }

    return FENWICK_D_FN_(FullQuery)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), (uint64_t)idx);
}

/* Range query [left, right] */
FENWICK_VALUE_TYPE FENWICK_D_FN_(RangeQuery)(const void *fw, size_t left,
                                             size_t right) {
    if (left > right || right >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallRangeQuery)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)left,
            (uint32_t)right);
    }

    return FENWICK_D_FN_(FullRangeQuery)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), (uint64_t)left,
        (uint64_t)right);
}

/* Get single element */
FENWICK_VALUE_TYPE FENWICK_D_FN_(Get)(const void *fw, size_t idx) {
    if (idx >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallGet)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)idx);
    }

    return FENWICK_D_FN_(FullGet)((const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw),
                                  (uint64_t)idx);
}

size_t FENWICK_D_FN_(Bytes)(const void *fw) {
    if (!fw) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallBytes)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    }

    return FENWICK_D_FN_(FullBytes)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
}

/* Smallest index whose prefix sum reaches 'target' (non-negative
 * elements); SIZE_MAX when none does */
size_t FENWICK_D_FN_(LowerBound)(const void *fw, FENWICK_VALUE_TYPE target) {
    if (!fw) {
        return SIZE_MAX;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        const uint32_t pos = FENWICK_D_FN_(SmallLowerBound)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), target);
        return pos == (uint32_t)-1 ? SIZE_MAX : (size_t)pos;
    }

    const uint64_t pos = FENWICK_D_FN_(FullLowerBound)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), target);
    return pos == (uint64_t)-1 ? SIZE_MAX : (size_t)pos;
}

void FENWICK_D_FN_(Clear)(void *fw) {
    if (!fw) {
        return;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        FENWICK_D_FN_(SmallClear)((FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullClear)((FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }
}

/* ====================================================================
 * Bulk build, element copy and persistence
 * ==================================================================== */
#include "../cleaner.h"
#include "../flex.h"
#include "../persist.h"
#include "fenwickCoreAlgo.h"

defcleanerName(FENWICK_VALUE_TYPE *, zfree, FenwickValues);

#define FENWICK_D_SIGNED_ ((FENWICK_VALUE_TYPE) - 1 < (FENWICK_VALUE_TYPE)0)
#define FENWICK_D_OPS_ FENWICK_CAT(persistOpsFenwick, FENWICK_SUFFIX)

/* True when a tree of 'capacity' cells fits the Small tier */
static bool FENWICK_D_FN_(FitsSmall_)(uint64_t capacity) {
    return capacity * sizeof(FENWICK_VALUE_TYPE) + sizeof(FENWICK_D_SMALL_) <=
           FENWICK_CALC_SMALL_MAX_BYTES;
}

void *FENWICK_D_FN_(NewFromArray)(const FENWICK_VALUE_TYPE *values,
                                  size_t count) {
    if (!values || count == 0) {
        return FENWICK_D_FN_(New)();
    }

    const uint64_t capacity = FENWICK_D_FN_(AlgoCapacityFor_)(0, count - 1);
    if (FENWICK_D_FN_(FitsSmall_)(capacity)) {
        FENWICK_D_SMALL_ *small =
            FENWICK_D_FN_(SmallNewFromArray)(values, (uint32_t)count);
        return small ? FENWICK_D_TAG_(small, FENWICK_D_TAG_SMALL_) : NULL;
    }

    FENWICK_D_FULL_ *full =
        FENWICK_D_FN_(FullNewFromArray)(values, (uint64_t)count);
    return full ? FENWICK_D_TAG_(full, FENWICK_D_TAG_FULL_) : NULL;
}

/* The cells, the count and the capacity of 'fw' (all zero for NULL) */
static void FENWICK_D_FN_(Cells_)(const void *fw,
                                  const FENWICK_VALUE_TYPE **tree,
                                  uint64_t *count, uint64_t *capacity) {
    *tree = NULL;
    *count = 0;
    *capacity = 0;
    if (!fw) {
        return;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        const FENWICK_D_SMALL_ *small = FENWICK_D_UNTAG_(fw);
        *tree = small->tree;
        *count = small->count;
        *capacity = small->capacity;
    } else {
        const FENWICK_D_FULL_ *full = FENWICK_D_UNTAG_(fw);
        *tree = full->tree;
        *count = full->count;
        *capacity = full->capacity;
    }
}

void FENWICK_D_FN_(CopyTo)(const void *fw, FENWICK_VALUE_TYPE *out) {
    const FENWICK_VALUE_TYPE *tree;
    uint64_t count;
    uint64_t capacity;
    FENWICK_D_FN_(Cells_)(fw, &tree, &count, &capacity);
    if (count == 0) {
        return;
    }

    /* Undo AlgoBuild_ over the first 'count' cells: in descending order
     * each cell still holds its whole range when it is taken out of its
     * parent, and every child of a cell below the count is below it too.
     * The subtractions are Get's, in Get's order. */
    memcpy(out, tree, count * sizeof(*out));
    for (uint64_t j = count; j >= 1; j--) {
        const uint64_t parent = fenwickParent(j);
        if (parent <= count) {
            out[parent - 1] =
                FENWICK_D_FN_(AlgoSub_)(out[parent - 1], out[j - 1]);
        }
    }
}

/* Persistence.  Integer trees snapshot their elements, which determine
 * the tree (wrapping cells are linear).  Float trees snapshot their
 * capacity, count and every cell: rounding makes the cells history, not
 * a function of the elements, so restore keeps them bit for bit.
 *   integer: flex [element 0, ..., element count-1]
 *   float:   flex [capacity, count, cell 0, ..., cell capacity-1]
 * WAL (args: const databox *const *): NUM_INCR [index, delta] (Update),
 * UPDATE [index, value] (Set), CLEAR. */
static void FENWICK_D_FN_(PersistBox_)(FENWICK_VALUE_TYPE v, databoxBig *out) {
    memset(out, 0, sizeof(*out));
#if FENWICK_IS_FLOATING
    if (sizeof(v) == sizeof(float)) {
        out->type = DATABOX_FLOAT_32;
        out->data.f32 = (float)v;
    } else {
        out->type = DATABOX_DOUBLE_64;
        out->data.d64 = (double)v;
    }
#else
    if (sizeof(v) == 16) {
        DATABOX_BIG_INIT(out);
        if (FENWICK_D_SIGNED_) {
            out->type = DATABOX_SIGNED_128;
            *out->data.i128 = (__int128_t)v;
        } else {
            out->type = DATABOX_UNSIGNED_128;
            *out->data.u128 = (__uint128_t)v;
        }
    } else if (FENWICK_D_SIGNED_) {
        out->type = DATABOX_SIGNED_64;
        out->data.i64 = (int64_t)v;
    } else {
        out->type = DATABOX_UNSIGNED_64;
        out->data.u64 = (uint64_t)v;
    }
#endif
}

/* A value of the tree's type, exactly: any integer box whose value fits
 * the type; a Float tree takes FLOAT_32, a Double tree DOUBLE_64 or
 * FLOAT_32 */
static bool FENWICK_D_FN_(PersistUnbox_)(const databoxBig *b,
                                         FENWICK_VALUE_TYPE *out) {
#if FENWICK_IS_FLOATING
    if (b->type == DATABOX_FLOAT_32) {
        *out = (FENWICK_VALUE_TYPE)b->data.f32;
        return true;
    }

    if (b->type == DATABOX_DOUBLE_64 &&
        sizeof(FENWICK_VALUE_TYPE) == sizeof(double)) {
        *out = (FENWICK_VALUE_TYPE)b->data.d64;
        return true;
    }

    return false;
#else
    const __uint128_t int128Max = ~(__uint128_t)0 >> 1;
    __int128_t s;
    __uint128_t u;
    switch (b->type) {
    case DATABOX_SIGNED_64:
        s = b->data.i64;
        u = (__uint128_t)s;
        break;
    case DATABOX_UNSIGNED_64:
        u = b->data.u64;
        s = (__int128_t)u;
        break;
    case DATABOX_SIGNED_128:
        s = *b->data.i128;
        u = (__uint128_t)s;
        break;
    case DATABOX_UNSIGNED_128:
        u = *b->data.u128;
        s = u > int128Max ? -1 : (__int128_t)u; /* -1: no signed fit */
        if (FENWICK_D_SIGNED_ && u > int128Max) {
            return false;
        }
        break;
    default:
        return false;
    }

    if (FENWICK_D_SIGNED_) {
        const __int128_t max =
            (__int128_t)((((__uint128_t)1)
                          << (8 * sizeof(FENWICK_VALUE_TYPE) - 1)) -
                         1);
        if (s > max || s < -max - 1) {
            return false;
        }

        *out = (FENWICK_VALUE_TYPE)s;
    } else {
        const bool negative =
            (b->type == DATABOX_SIGNED_64 || b->type == DATABOX_SIGNED_128) &&
            s < 0;
        if (negative ||
            u > (__uint128_t)(FENWICK_VALUE_TYPE) ~(FENWICK_VALUE_TYPE)0) {
            return false;
        }

        *out = (FENWICK_VALUE_TYPE)u;
    }

    return true;
#endif
}

/* Append values[0, n) exactly, 64 entries per flex growth */
static void FENWICK_D_FN_(PersistAppend_)(flex **f,
                                          const FENWICK_VALUE_TYPE *values,
                                          uint64_t n) {
    enum { BATCH = 64 };
    databoxBig boxes[BATCH];
    const databox *ptrs[BATCH];
    for (uint64_t at = 0; at < n; at += BATCH) {
        const uint64_t len = n - at < BATCH ? n - at : BATCH;
        for (uint64_t i = 0; i < len; i++) {
            FENWICK_D_FN_(PersistBox_)(values[at + i], &boxes[i]);
            ptrs[i] = (const databox *)&boxes[i];
        }

        flexAppendMultipleExact(f, (uint_fast32_t)len, ptrs);
    }
}

static uint8_t *FENWICK_D_FN_(PersistSnapshot_)(const void *structure,
                                                size_t *len) {
    const FENWICK_VALUE_TYPE *tree;
    uint64_t count;
    uint64_t capacity;
    FENWICK_D_FN_(Cells_)(structure, &tree, &count, &capacity);

    /* @own: caller; free=zfree */
    flex *f = flexNew();
#if FENWICK_IS_FLOATING
    const databox cap = DATABOX_UNSIGNED(capacity);
    const databox cnt = DATABOX_UNSIGNED(count);
    flexPushByTypeExact(&f, &cap, FLEX_ENDPOINT_TAIL);
    flexPushByTypeExact(&f, &cnt, FLEX_ENDPOINT_TAIL);
    FENWICK_D_FN_(PersistAppend_)(&f, tree, capacity);
#else
    if (count) {
        cleanerName(zfree, FenwickValues) FENWICK_VALUE_TYPE *elements =
            zmalloc(count * sizeof(*elements));
        FENWICK_D_FN_(CopyTo)(structure, elements);
        FENWICK_D_FN_(PersistAppend_)(&f, elements, count);
    }
#endif

    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *FENWICK_D_FN_(PersistRestore_)(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    databoxBig box;
#if FENWICK_IS_FLOATING
    if (n < 2) {
        return NULL;
    }

    flexGetByTypeBig(fe, &box);
    const uint64_t capacity = box.data.u64;
    const bool capOk = box.type == DATABOX_UNSIGNED_64;
    fe = flexNext(f, fe);
    flexGetByTypeBig(fe, &box);
    const uint64_t count = box.data.u64;
    if (!capOk || box.type != DATABOX_UNSIGNED_64 || n - 2 != capacity ||
        count > capacity || (capacity & (capacity - 1)) ||
        (capacity == 0) != (count == 0)) {
        return NULL;
    }

    if (capacity == 0) {
        return FENWICK_D_FN_(New)();
    }

    /* Rebuild the tier holding these exact cells */
    void *fw;
    FENWICK_VALUE_TYPE *cells;
    if (FENWICK_D_FN_(FitsSmall_)(capacity)) {
        /* @own: caller; free=zfree */
        FENWICK_D_SMALL_ *small =
            zcalloc(1, sizeof(*small) + capacity * sizeof(FENWICK_VALUE_TYPE));
        small->count = (uint32_t)count;
        small->capacity = (uint32_t)capacity;
        cells = small->tree;
        fw = FENWICK_D_TAG_(small, FENWICK_D_TAG_SMALL_);
    } else {
        FENWICK_D_FULL_ *full = FENWICK_D_FN_(FullNew)();
        /* @own: container; free=zfree */
        full->tree = zcalloc(capacity, sizeof(FENWICK_VALUE_TYPE));
        full->count = count;
        full->capacity = capacity;
        cells = full->tree;
        fw = FENWICK_D_TAG_(full, FENWICK_D_TAG_FULL_);
    }

    for (uint64_t i = 0; i < capacity; i++) {
        fe = flexNext(f, fe);
        flexGetByTypeBig(fe, &box);
        if (!FENWICK_D_FN_(PersistUnbox_)(&box, &cells[i])) {
            FENWICK_D_FN_(Free)(fw);
            return NULL;
        }
    }

    return fw;
#else
    cleanerName(zfree, FenwickValues) FENWICK_VALUE_TYPE *values =
        zmalloc((n ? n : 1) * sizeof(*values));
    for (size_t i = 0; i < n; i++) {
        flexGetByTypeBig(fe, &box);
        if (!FENWICK_D_FN_(PersistUnbox_)(&box, &values[i])) {
            return NULL;
        }

        fe = flexNext(f, fe);
    }

    return FENWICK_D_FN_(NewFromArray)(values, n);
#endif
}

static uint8_t *FENWICK_D_FN_(PersistEncodeOp_)(persistOp op, const void *args,
                                                size_t argc, size_t *len) {
    const databox *const *boxes = args;
    *len = 0;
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) || argc != 2 ||
        !boxes) {
        /* CLEAR has no operand; nothing else is replayed */
        return NULL;
    }

    flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
    if (f) {
        *len = flexBytes(f);
    }

    return (uint8_t *)f;
}

static bool FENWICK_D_FN_(PersistApplyOp_)(void *structure, persistOp op,
                                           const uint8_t *data, size_t len) {
    void **fw = structure;
    if (op == PERSIST_OP_CLEAR) {
        FENWICK_D_FN_(Clear)(*fw);
        return true;
    }

    if (op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) {
        return false;
    }

    databoxBig idx;
    databoxBig value;
    FENWICK_VALUE_TYPE v;
    if (!flexGetTwoDataboxesBig(data, len, &idx, &value) ||
        idx.type != DATABOX_UNSIGNED_64 || idx.data.u64 > SIZE_MAX ||
        !FENWICK_D_FN_(PersistUnbox_)(&value, &v)) {
        return false;
    }

    return op == PERSIST_OP_NUM_INCR
               ? FENWICK_D_FN_(Update)(fw, (size_t)idx.data.u64, v)
               : FENWICK_D_FN_(Set)(fw, (size_t)idx.data.u64, v);
}

const persistOps FENWICK_D_OPS_ = {
    .type = FENWICK_PERSIST_TYPE,
    .name = "fenwick" FENWICK_D_STR_(FENWICK_SUFFIX),
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = FENWICK_D_FN_(PersistSnapshot_),
    .restore = FENWICK_D_FN_(PersistRestore_),
    .newEmpty = FENWICK_D_FN_(New),
    .count = FENWICK_D_FN_(Count),
    .estimateSize = FENWICK_D_FN_(Bytes),
    .encodeOp = FENWICK_D_FN_(PersistEncodeOp_),
    .applyOp = FENWICK_D_FN_(PersistApplyOp_),
    .free = FENWICK_D_FN_(Free),
};

#ifdef DATAKIT_TEST
void FENWICK_D_FN_(Repr)(const void *fw) {
    if (!fw) {
        printf("fenwick" FENWICK_D_STR_(FENWICK_SUFFIX) ": (nil)\n");
        return;
    }

    const bool small = FENWICK_D_IS_SMALL_(fw);
    if (small) {
        FENWICK_D_FN_(SmallRepr)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullRepr)((const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", small ? "SMALL" : "FULL",
           FENWICK_D_FN_(Count)(fw), FENWICK_D_FN_(Bytes)(fw));
}
#endif

#undef FENWICK_D_SIGNED_
#undef FENWICK_D_OPS_
#undef FENWICK_PERSIST_TYPE
#undef FENWICK_D_FN_
#undef FENWICK_D_STR2_
#undef FENWICK_D_STR_
#undef FENWICK_D_SMALL_
#undef FENWICK_D_FULL_
#undef FENWICK_D_TAG_SMALL_
#undef FENWICK_D_TAG_FULL_
#undef FENWICK_D_TAG_MASK_
#undef FENWICK_D_IS_SMALL_
#undef FENWICK_D_UNTAG_
#undef FENWICK_D_TAG_
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_IS_FLOATING
