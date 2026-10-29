/* containerMatrix: per container, element kind and size, the throughput
 * of create, read, update and delete, and the bytes each element takes
 * (containerMatrix.h) */

#include "containerMatrix.h"

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "dkRandom.h"
#include "intset.h"
#include "mflex.h"
#include "multiOrderedSet.h"
#include "multiarray.h"
#include "multidict.h"
#include "multilist.h"
#include "multilru.h"
#include "multimap.h"
#include "multiroar.h"
#include "stringPool.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Elements
 * ==================================================================== */

/* What a container holds: small integers (0 .. n), integers across the
 * whole 64-bit range, and byte strings of 16 and 256 bytes */
typedef enum mxKind {
    MX_INT_SMALL,
    MX_INT_WIDE,
    MX_STR16,
    MX_STR256,
    MX_KINDS
} mxKind;

static const char *const mxKindName[MX_KINDS] = {"int-small", "int-wide",
                                                 "str16", "str256"};
#define MX_INTS ((1u << MX_INT_SMALL) | (1u << MX_INT_WIDE))
#define MX_ALL ((1u << MX_KINDS) - 1)

static size_t mxWidth(mxKind kind) {
    return kind == MX_STR16 ? 16 : kind == MX_STR256 ? 256 : 8;
}

/* The integer element 'i' of 'kind', at version 'round' (0 created, 1
 * updated) */
static int64_t mxInt(mxKind kind, size_t i, int round) {
    uint64_t seed = (uint64_t)i * 2 + (uint64_t)round;
    return kind == MX_INT_SMALL ? (int64_t)(i * 2 + (size_t)round)
                                : (int64_t)dkSplitmix64Next(&seed);
}

/* Element 'i' of 'kind' at 'round' as a box; a string's bytes go to 'buf'
 * (mxWidth bytes), a distinct prefix then filler */
static databox mxBox(mxKind kind, size_t i, int round, uint8_t *buf) {
    if (kind == MX_INT_SMALL || kind == MX_INT_WIDE) {
        return databoxNewSigned(mxInt(kind, i, round));
    }
    const size_t width = mxWidth(kind);
    const int head =
        snprintf((char *)buf, width, "%c%014zu", round ? 'u' : 'k', i);
    for (size_t b = (size_t)head; b < width; b++) {
        buf[b] = (uint8_t)('a' + (i + b) % 26);
    }
    return DATABOX_WITH_BYTES(buf, width);
}

/* Whether 'got' holds element 'i' of 'kind' at 'round' */
static bool mxSame(const databox *got, mxKind kind, size_t i, int round) {
    uint8_t buf[256];
    const databox want = mxBox(kind, i, round, buf);
    if (kind == MX_INT_SMALL || kind == MX_INT_WIDE) {
        uint64_t bits;
        if (got->type == DATABOX_SIGNED_64) {
            bits = (uint64_t)got->data.i64;
        } else if (got->type == DATABOX_UNSIGNED_64) {
            bits = got->data.u64;
        } else {
            return false;
        }
        return bits == (uint64_t)want.data.i64;
    }
    return DATABOX_IS_BYTES(got) && databoxLen(got) == want.len &&
           memcmp(databoxBytes(got), buf, want.len) == 0;
}

/* ====================================================================
 * Drivers: one per container
 * ==================================================================== */

/* A container under measurement and what its driver keeps beside it (the
 * handles a container gives out, scratch state) */
typedef struct mxCtx {
    void *c;
    void *aux;
    uint64_t *ids;
    mxKind kind;
    int round; /* the version every element holds: 0 created, 1 updated */
} mxCtx;

typedef struct mxDriver {
    const char *name;
    const char *shape; /* how its operations map onto CRUD */
    uint32_t kinds;    /* bitmask of mxKind it holds */
    size_t maxSize;    /* largest size measured */
    void (*make)(mxCtx *x, size_t n);
    void (*destroy)(mxCtx *x);
    bool (*create)(mxCtx *x, size_t i);
    bool (*read)(mxCtx *x, size_t i);
    bool (*update)(mxCtx *x, size_t i); /* NULL: no update */
    bool (*remove)(mxCtx *x, size_t i);
    size_t (*bytes)(const mxCtx *x);
    size_t (*count)(const mxCtx *x);
} mxDriver;

/* --- multimap: key -> value ----------------------------------------- */
static void mxMapMake(mxCtx *x, size_t) {
    /* @own: container; free=multimapFree */
    x->c = multimapNew(2);
}
static void mxMapDestroy(mxCtx *x) {
    multimapFree(x->c);
}
static bool mxMapPut(mxCtx *x, size_t i, int round) {
    uint8_t kb[256], vb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    const databox val = mxBox(x->kind, i, round, vb);
    const databox *row[2] = {&key, &val};
    multimap *m = x->c;
    multimapInsert(&m, row);
    x->c = m;
    return true;
}
static bool mxMapCreate(mxCtx *x, size_t i) {
    return mxMapPut(x, i, 0);
}
static bool mxMapUpdate(mxCtx *x, size_t i) {
    return mxMapPut(x, i, 1);
}
static bool mxMapRead(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    databox got = {};
    databox *out[1] = {&got};
    return multimapLookup(x->c, &key, out) &&
           mxSame(&got, x->kind, i, x->round);
}
static bool mxMapRemove(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    multimap *m = x->c;
    const bool deleted = multimapDelete(&m, &key);
    x->c = m;
    return deleted;
}
static size_t mxMapBytes(const mxCtx *x) {
    return multimapBytes(x->c);
}
static size_t mxMapCount(const mxCtx *x) {
    return multimapCount(x->c);
}

/* --- multidict: hashed key -> value --------------------------------- */
static void mxDictMake(mxCtx *x, size_t) {
    /* @own: container; free=multidictDefaultClassFree */
    x->aux = multidictDefaultClassNew();
    /* @own: container; free=multidictFree */
    x->c = multidictNew(&multidictTypeExactKey, x->aux, 7);
}
static void mxDictDestroy(mxCtx *x) {
    multidictFree(x->c);
    multidictDefaultClassFree(x->aux);
}
static bool mxDictPut(mxCtx *x, size_t i, int round) {
    uint8_t kb[256], vb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    const databox val = mxBox(x->kind, i, round, vb);
    const multidictResult r = multidictAdd(x->c, &key, &val);
    return r == MULTIDICT_OK_INSERTED || r == MULTIDICT_OK_REPLACED;
}
static bool mxDictCreate(mxCtx *x, size_t i) {
    return mxDictPut(x, i, 0);
}
static bool mxDictUpdate(mxCtx *x, size_t i) {
    return mxDictPut(x, i, 1);
}
static bool mxDictRead(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    databox got = {};
    return multidictFind(x->c, &key, &got) &&
           mxSame(&got, x->kind, i, x->round);
}
static bool mxDictRemove(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox key = mxBox(x->kind, i, 0, kb);
    return multidictDelete(x->c, &key);
}
static size_t mxDictBytes(const mxCtx *x) {
    return multidictBytes(x->c);
}
static size_t mxDictCount(const mxCtx *x) {
    return multidictCount(x->c);
}

/* --- multiOrderedSet: member -> score, ordered by score ------------- */
static void mxSetMake(mxCtx *x, size_t) {
    /* @own: container; free=multiOrderedSetFree */
    x->c = multiOrderedSetNew();
}
static void mxSetDestroy(mxCtx *x) {
    multiOrderedSetFree(x->c);
}
static bool mxSetPut(mxCtx *x, size_t i, int round) {
    uint8_t kb[256];
    const databox member = mxBox(x->kind, i, 0, kb);
    const databox score = databoxNewSigned(mxInt(MX_INT_WIDE, i, round));
    multiOrderedSet *s = x->c;
    multiOrderedSetAdd(&s, &score, &member);
    x->c = s;
    return true;
}
static bool mxSetCreate(mxCtx *x, size_t i) {
    return mxSetPut(x, i, 0);
}
static bool mxSetUpdate(mxCtx *x, size_t i) {
    return mxSetPut(x, i, 1);
}
static bool mxSetRead(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox member = mxBox(x->kind, i, 0, kb);
    databox score = {};
    return multiOrderedSetGetScore(x->c, &member, &score) &&
           mxSame(&score, MX_INT_WIDE, i, x->round);
}
static bool mxSetRemove(mxCtx *x, size_t i) {
    uint8_t kb[256];
    const databox member = mxBox(x->kind, i, 0, kb);
    multiOrderedSet *s = x->c;
    const bool removed = multiOrderedSetRemove(&s, &member);
    x->c = s;
    return removed;
}
static size_t mxSetBytes(const mxCtx *x) {
    return multiOrderedSetBytes(x->c);
}
static size_t mxSetCount(const mxCtx *x) {
    return multiOrderedSetCount(x->c);
}

/* --- multilist: a sequence; read and update by index, delete the tail */
static void mxListMake(mxCtx *x, size_t) {
    /* @own: container; free=multilistFree */
    x->c = multilistNew();
    /* @own: container; free=mflexStateFree */
    x->aux = mflexStateCreate();
}
static void mxListDestroy(mxCtx *x) {
    multilistFree(x->c);
    mflexStateFree(x->aux);
}
static bool mxListCreate(mxCtx *x, size_t i) {
    uint8_t b[256];
    const databox box = mxBox(x->kind, i, 0, b);
    multilist *l = x->c;
    multilistPushByTypeTail(&l, x->aux, &box);
    x->c = l;
    return true;
}
static bool mxListRead(mxCtx *x, size_t i) {
    multilistEntry e;
    return multilistIndex(x->c, x->aux, (mlOffsetId)i, &e, true) &&
           mxSame(&e.box, x->kind, i, x->round);
}
static bool mxListUpdate(mxCtx *x, size_t i) {
    uint8_t b[256];
    const databox box = mxBox(x->kind, i, 1, b);
    multilist *l = x->c;
    const bool replaced =
        multilistReplaceByTypeAtIndex(&l, x->aux, (mlOffsetId)i, &box);
    x->c = l;
    return replaced;
}
static bool mxListRemove(mxCtx *x, size_t) {
    databox got = {};
    multilist *l = x->c;
    const bool popped = multilistPopTail(&l, x->aux, &got);
    x->c = l;
    return popped;
}
static size_t mxListBytes(const mxCtx *x) {
    return multilistBytes(x->c);
}
static size_t mxListCount(const mxCtx *x) {
    return multilistCount(x->c);
}

/* --- multiarray: fixed-width elements by index, delete the tail ------ */
static void mxArrayMake(mxCtx *x, size_t) {
    /* @own: container; free=multiarrayFree */
    x->c = multiarrayNew((uint16_t)mxWidth(x->kind), 1024);
}
static void mxArrayDestroy(mxCtx *x) {
    multiarrayFree(x->c);
}
/* Element 'i' at 'round' as its fixed-width bytes in 'buf' */
static void mxArrayElement(mxKind kind, size_t i, int round, uint8_t *buf) {
    const databox box = mxBox(kind, i, round, buf);
    if (kind == MX_INT_SMALL || kind == MX_INT_WIDE) {
        memcpy(buf, &box.data.i64, sizeof(int64_t));
    }
}
static bool mxArrayCreate(mxCtx *x, size_t i) {
    uint8_t b[256];
    mxArrayElement(x->kind, i, 0, b);
    multiarray *a = x->c;
    multiarrayInsert(&a, (multiarrayIdx)i, b);
    x->c = a;
    return true;
}
static bool mxArrayRead(mxCtx *x, size_t i) {
    uint8_t want[256];
    mxArrayElement(x->kind, i, x->round, want);
    const uint8_t *got = multiarrayGet(x->c, (multiarrayIdx)i);
    const size_t width = mxWidth(x->kind);
    return got && memcmp(got, want, width) == 0;
}
static bool mxArrayUpdate(mxCtx *x, size_t i) {
    uint8_t b[256];
    mxArrayElement(x->kind, i, 1, b);
    multiarrayReplace(x->c, (multiarrayIdx)i, b);
    return true;
}
static bool mxArrayRemove(mxCtx *x, size_t) {
    multiarray *a = x->c;
    const size_t count = multiarrayCount(a);
    if (!count) {
        return false;
    }
    multiarrayDelete(&a, (uint32_t)(count - 1));
    x->c = a;
    return true;
}
static size_t mxArrayBytes(const mxCtx *x) {
    return multiarrayBytes(x->c);
}
static size_t mxArrayCount(const mxCtx *x) {
    return multiarrayCount(x->c);
}

/* --- intset: a sorted set of integers -------------------------------- */
static void mxIntsetMake(mxCtx *x, size_t) {
    /* @own: container; free=intsetFree */
    x->c = intsetNew();
}
static void mxIntsetDestroy(mxCtx *x) {
    intsetFree(x->c);
}
static bool mxIntsetCreate(mxCtx *x, size_t i) {
    bool added = false;
    intset *s = x->c;
    intsetAdd(&s, mxInt(x->kind, i, 0), &added);
    x->c = s;
    return added;
}
static bool mxIntsetRead(mxCtx *x, size_t i) {
    return intsetFind(x->c, mxInt(x->kind, i, 0));
}
static bool mxIntsetRemove(mxCtx *x, size_t i) {
    bool removed = false;
    intset *s = x->c;
    intsetRemove(&s, mxInt(x->kind, i, 0), &removed);
    x->c = s;
    return removed;
}
static size_t mxIntsetBytes(const mxCtx *x) {
    return intsetBytes(x->c);
}
static size_t mxIntsetCount(const mxCtx *x) {
    return intsetCount(x->c);
}

/* --- multiroar: a bitmap of 64-bit positions ------------------------- */
static void mxRoarMake(mxCtx *x, size_t) {
    /* @own: container; free=multiroarFree */
    x->c = multiroarBitNew();
}
static void mxRoarDestroy(mxCtx *x) {
    multiroarFree(x->c);
}
static bool mxRoarCreate(mxCtx *x, size_t i) {
    return !multiroarBitSet(x->c, (uint64_t)mxInt(x->kind, i, 0));
}
static bool mxRoarRead(mxCtx *x, size_t i) {
    return multiroarBitGet(x->c, (uint64_t)mxInt(x->kind, i, 0));
}
static bool mxRoarRemove(mxCtx *x, size_t i) {
    return multiroarBitClear(x->c, (uint64_t)mxInt(x->kind, i, 0));
}
static size_t mxRoarBytes(const mxCtx *x) {
    return (size_t)multiroarMemoryUsage(x->c);
}
static size_t mxRoarCount(const mxCtx *x) {
    return (size_t)multiroarBitCount(x->c);
}

/* --- stringPool: interned strings by id ------------------------------ */
static void mxPoolMake(mxCtx *x, size_t n) {
    /* @own: container; free=stringPoolFree */
    x->c = stringPoolNew();
    /* @own: container; free=zfree */
    x->ids = zcalloc(n ? n : 1, sizeof(uint64_t));
}
static void mxPoolDestroy(mxCtx *x) {
    stringPoolFree(x->c);
    zfree(x->ids);
}
static bool mxPoolCreate(mxCtx *x, size_t i) {
    uint8_t b[256];
    const databox s = mxBox(x->kind, i, 0, b);
    x->ids[i] = stringPoolIntern(x->c, &s);
    return x->ids[i] != 0;
}
static bool mxPoolRead(mxCtx *x, size_t i) {
    databox got = {};
    return stringPoolLookup(x->c, x->ids[i], &got) &&
           mxSame(&got, x->kind, i, 0);
}
static bool mxPoolRemove(mxCtx *x, size_t i) {
    stringPoolRelease(x->c, x->ids[i]);
    return true;
}
static size_t mxPoolBytes(const mxCtx *x) {
    return stringPoolBytes(x->c);
}
static size_t mxPoolCount(const mxCtx *x) {
    return stringPoolCount(x->c);
}

/* --- multilru: recency metadata per handle (no element data) --------- */
static void mxLruMake(mxCtx *x, size_t n) {
    const multilruConfig config = {.enableWeights = true};
    /* @own: container; free=multilruFree */
    x->c = multilruNewWithConfig(&config);
    /* @own: container; free=zfree */
    x->ids = zcalloc(n ? n : 1, sizeof(uint64_t));
}
static void mxLruDestroy(mxCtx *x) {
    multilruFree(x->c);
    zfree(x->ids);
}
static bool mxLruCreate(mxCtx *x, size_t i) {
    x->ids[i] = multilruInsertWeighted(x->c, 1);
    return x->ids[i] != 0;
}
static bool mxLruRead(mxCtx *x, size_t i) {
    /* A cache hit: the entry is promoted */
    multilruIncrease(x->c, x->ids[i]);
    return multilruIsPopulated(x->c, x->ids[i]);
}
static bool mxLruUpdate(mxCtx *x, size_t i) {
    multilruUpdateWeight(x->c, x->ids[i], 2);
    return multilruGetWeight(x->c, x->ids[i]) == 2;
}
static bool mxLruRemove(mxCtx *x, size_t i) {
    multilruDelete(x->c, x->ids[i]);
    return true;
}
static size_t mxLruBytes(const mxCtx *x) {
    return multilruBytes(x->c);
}
static size_t mxLruCount(const mxCtx *x) {
    return multilruCount(x->c);
}

#define MX_DRIVER(NAME, SHAPE, KINDS, MAX, P, HAS_UPDATE)                      \
    {.name = NAME,                                                             \
     .shape = SHAPE,                                                           \
     .kinds = KINDS,                                                           \
     .maxSize = MAX,                                                           \
     .make = P##Make,                                                          \
     .destroy = P##Destroy,                                                    \
     .create = P##Create,                                                      \
     .read = P##Read,                                                          \
     .update = HAS_UPDATE,                                                     \
     .remove = P##Remove,                                                      \
     .bytes = P##Bytes,                                                        \
     .count = P##Count}

static const mxDriver mxDrivers[] = {
    MX_DRIVER("multimap", "key -> value; ordered", MX_ALL, 1'000'000, mxMap,
              mxMapUpdate),
    MX_DRIVER("multidict", "key -> value; hashed", MX_ALL, 1'000'000, mxDict,
              mxDictUpdate),
    MX_DRIVER("multiOrderedSet", "member -> score; by score", MX_ALL, 1'000'000,
              mxSet, mxSetUpdate),
    MX_DRIVER("multilist", "sequence; by index, delete at tail", MX_ALL,
              1'000'000, mxList, mxListUpdate),
    MX_DRIVER("multiarray", "fixed-width sequence; by index", MX_ALL, 1'000'000,
              mxArray, mxArrayUpdate),
    MX_DRIVER("intset", "sorted integer set", MX_INTS, 1'000'000, mxIntset,
              nullptr),
    MX_DRIVER("multiroar", "bitmap of positions", MX_INTS, 1'000'000, mxRoar,
              nullptr),
    MX_DRIVER("stringPool", "interned strings by id",
              (1u << MX_STR16) | (1u << MX_STR256), 1'000'000, mxPool, nullptr),
    MX_DRIVER("multilru", "recency per handle", 1u << MX_INT_SMALL, 1'000'000,
              mxLru, mxLruUpdate),
};

/* ====================================================================
 * Measurement
 * ==================================================================== */

typedef bool mxOp(mxCtx *x, size_t i);

defcleanerName(size_t *, zfree, Sizes);

/* Fisher-Yates over order[0, n) */
static void mxShuffle(size_t *order, size_t n, uint64_t *seed) {
    for (size_t i = n; i > 1; i--) {
        const size_t j = (size_t)dkSplitmix64Bounded(seed, i);
        const size_t swap = order[i - 1];
        order[i - 1] = order[j];
        order[j] = swap;
    }
}

/* 'op' on elements order[0, n); false (counting) for any refused one */
static double mxTime(mxCtx *x, mxOp *op, const size_t *order, size_t n,
                     size_t *failed) {
    const uint64_t t0 = timeUtilMonotonicNs();
    for (size_t k = 0; k < n; k++) {
        *failed += !op(x, order[k]);
    }
    return (double)(timeUtilMonotonicNs() - t0) / 1e9;
}

static void mxRow(const mxDriver *d, mxKind kind, size_t n, const char *op,
                  double secs, double bytesPerElement) {
    printf("MATRIX\t%s\t%s\t%zu\t%s\t%zu\t%.9f\t%.2f\n", d->name,
           mxKindName[kind], n, op, n, secs, bytesPerElement);
}

/* One container, kind and size: every phase timed over n elements in a
 * shuffled order (sequences create in order: they append), every result
 * checked; returns the failures */
static size_t mxMeasure(const mxDriver *d, mxKind kind, size_t n,
                        uint64_t seed) {
    size_t failed = 0;
    cleanerName(zfree, Sizes) size_t *order = zcalloc(n, sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        order[i] = i;
    }
    const bool sequence = d->make == mxListMake || d->make == mxArrayMake;
    mxCtx x = {.kind = kind};
    d->make(&x, n);

    /* Create: keyed containers in a shuffled order */
    if (!sequence) {
        mxShuffle(order, n, &seed);
    }
    const double createSecs = mxTime(&x, d->create, order, n, &failed);
    const double perElement = n ? (double)d->bytes(&x) / (double)n : 0;
    failed += d->count(&x) != n;
    if (sequence) {
        mxShuffle(order, n, &seed);
    }
    mxRow(d, kind, n, "create", createSecs, perElement);

    mxRow(d, kind, n, "read", mxTime(&x, d->read, order, n, &failed),
          perElement);
    if (d->update) {
        mxShuffle(order, n, &seed);
        mxRow(d, kind, n, "update", mxTime(&x, d->update, order, n, &failed),
              perElement);
        /* Untimed: every element reads back as its update */
        x.round = 1;
        for (size_t i = 0; i < n; i++) {
            failed += !d->read(&x, i);
        }
    }
    mxShuffle(order, n, &seed);
    mxRow(d, kind, n, "delete", mxTime(&x, d->remove, order, n, &failed),
          perElement);
    failed += d->count(&x) != 0;
    d->destroy(&x);
    return failed;
}

int containerMatrixTest(int, char *[]) {
    int err = 0;
    const char *mode = getenv("DK_MATRIX");
    const bool full = mode && strcmp(mode, "full") == 0;
    static constexpr size_t smoke[] = {100};
    static constexpr size_t fullSizes[] = {1'000, 100'000, 1'000'000};
    const size_t *sizes = full ? fullSizes : smoke;
    const size_t sizeCount = full ? countof(fullSizes) : countof(smoke);

    TEST("every container's create, read, update and delete check out") {
        printf("MATRIX\tcontainer\tkind\tsize\top\tops\tseconds\t"
               "bytes_per_element\n");
        for (size_t d = 0; d < countof(mxDrivers); d++) {
            const mxDriver *drv = &mxDrivers[d];
            printf("MATRIXSHAPE\t%s\t%s\n", drv->name, drv->shape);
            for (size_t k = 0; k < MX_KINDS; k++) {
                if (!(drv->kinds & (1u << k))) {
                    continue;
                }
                for (size_t s = 0; s < sizeCount; s++) {
                    /* 256-byte strings stop at 100K elements (memory) */
                    if (sizes[s] > drv->maxSize ||
                        (k == MX_STR256 && sizes[s] > 100'000)) {
                        continue;
                    }
                    const size_t failed =
                        mxMeasure(drv, (mxKind)k, sizes[s], 0x9E37 + s);
                    if (failed) {
                        ERR("%s %s at %zu: %zu operations failed", drv->name,
                            mxKindName[k], sizes[s], failed);
                    }
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
