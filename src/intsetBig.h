#pragma once

#include "databox.h"
#include "multimap.h" /* for multimapIterator */

/* Attempting to use integers outside of this range breaks all guarantees of
 * intsetBig stability */
#define INTSET_BIG_INT128_MIN -((((__int128_t)1) << (63 + 20)) - 1)
#define INTSET_BIG_INT128_MAX ((((__int128_t)1) << (63 + 20)) - 1)
#define INTSET_BIG_UINT128_MIN 0
#define INTSET_BIG_UINT128_MAX ((((__uint128_t)1) << (64 + 20)) - 1)

/* Helper macro to check ranges for consumers to consume in consumption code */
#define INTSET_BIG_RANGECHECK_INT128(a)                                        \
    ((a) >= INTSET_BIG_INT128_MIN && (a) <= INTSET_BIG_INT128_MAX)

#define INTSET_BIG_RANGECHECK_UINT128(a)                                       \
    ((a) >= INTSET_BIG_UINT128_MIN && (a) <= INTSET_BIG_UINT128_MAX)

#define INTSET_BIG_RANGECHECK(box)                                             \
    ({                                                                         \
        bool ok_range = true;                                                  \
        switch ((box)->type) {                                                 \
        /* we know 64 bit integers are okay! */                                \
        case DATABOX_UNSIGNED_64:                                              \
        /* fallthrough */                                                      \
        case DATABOX_SIGNED_64:                                                \
            break;                                                             \
        case DATABOX_UNSIGNED_128:                                             \
            ok_range = INTSET_BIG_RANGECHECK_UINT128(*(box)->data.u128);       \
            break;                                                             \
        case DATABOX_SIGNED_128:                                               \
            ok_range = INTSET_BIG_RANGECHECK_INT128(*(box)->data.i128);        \
            break;                                                             \
        default:                                                               \
            assert(NULL && "Invalid type for numeric compare?");               \
            __builtin_unreachable();                                           \
        }                                                                      \
                                                                               \
        ok_range;                                                              \
    })

typedef struct intsetBigSet intsetBig;

intsetBig *intsetBigNew(void);
void intsetBigFree(intsetBig *isb);
intsetBig *intsetBigCopy(const intsetBig *isb);

bool intsetBigAdd(intsetBig *isb, const databoxBig *val);
bool intsetBigRemove(intsetBig *isb, const databoxBig *val);
bool intsetBigExists(const intsetBig *isb, const databoxBig *val);
bool intsetBigRandom(const intsetBig *isb, databoxBig *val);
bool intsetBigRandomDelete(intsetBig *isb, databoxBig *deleted);

size_t intsetBigIntersect(const intsetBig *a, const intsetBig *b,
                          intsetBig *result);
void intsetBigMergeInto(intsetBig *into, const intsetBig *from);

typedef struct intsetBigIterator {
    multimapIterator iter;
    struct {
        databoxBig box;
    } bucket;

    struct {
        ssize_t position;
        ssize_t count;
        uint32_t *array;
    } elements;

    bool forward; /* true if iterating from smallest to largest numbers */
} intsetBigIterator;

void intsetBigIteratorInit(const intsetBig *isb, intsetBigIterator *isbIter);
bool intsetBigIteratorNextBox(intsetBigIterator *isbIter, databoxBig *val);

bool intsetBigEqual(const intsetBig *a, const intsetBig *b);
bool intsetBigSubset(const intsetBig *a, const intsetBig *b);
size_t intsetBigCountBuckets(const intsetBig *isb);
size_t intsetBigCountElements(const intsetBig *isb);
size_t intsetBigBytes(const intsetBig *isb);

#ifdef DATAKIT_TEST
int intsetBigTest(int argc, char *argv[]);
void intsetBigRepr(const intsetBig *isb);
#endif
