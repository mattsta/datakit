#pragma once

#include "databox.h"

#include "multiarrayLarge.h"
#include "multiarrayMedium.h"
#include "multiarrayMediumInternal.h"
#include "multiarraySmall.h"

typedef enum multiarrayType {
    MULTIARRAY_TYPE_NATIVE = 0, /* 8 bytes, fixed. */
    MULTIARRAY_TYPE_SMALL = 1,  /* 16 + 8 bytes, fixed. */
    MULTIARRAY_TYPE_MEDIUM = 2, /* 16 + (16 * N) bytes pointer array. */
    MULTIARRAY_TYPE_LARGE = 3   /* 24 + (16 * N) bytes pointer linked list.  */
} multiarrayType;

/* opaque multiarray type; there's no user accessible data here. */
typedef struct multiarray multiarray;
typedef uint32_t multiarrayIdx;

#define _MULTIARRAY_TAGGED_PTR_MASK (uintptr_t)(0x03)
/* We store type info in the lower two bits of the pointer */
#define _multiarrayType(map)                                                   \
    (multiarrayType)((uintptr_t)(map) & _MULTIARRAY_TAGGED_PTR_MASK)
#define _MULTIARRAY_USE(map)                                                   \
    ((void *)((uintptr_t)(map) & ~(_MULTIARRAY_TAGGED_PTR_MASK)))
#define _MULTIARRAY_TAG(map, type) ((multiarray *)((uintptr_t)(map) | (type)))

#define mars(m) ((multiarraySmall *)_MULTIARRAY_USE(m))
#define marm(m) ((multiarrayMedium *)_MULTIARRAY_USE(m))
#define marl(m) ((multiarrayLarge *)_MULTIARRAY_USE(m))

/* ====================================================================
 * Starting with native...
 * ==================================================================== */
#define multiarrayNativeNew(holder) (multiarraySmallNativeNew(holder))

#define multiarrayNativeFree(mar)                                              \
    do {                                                                       \
        switch (_multiarrayType(mar)) {                                        \
        case MULTIARRAY_TYPE_NATIVE:                                           \
            multiarraySmallNativeFree(mar);                                    \
            break;                                                             \
        case MULTIARRAY_TYPE_MEDIUM:                                           \
            multiarrayMediumFree(marm(mar));                                   \
            break;                                                             \
        case MULTIARRAY_TYPE_LARGE:                                            \
            multiarrayLargeFree(marl(mar));                                    \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define multiarrayNativeGet(mar, holder, result, idx)                          \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            (result) = multiarraySmallNativeGet(mar, holder, idx);             \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            (result) = multiarrayMediumGet(marm(mar), idx);                    \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            (result) = multiarrayLargeGet(marl(mar), idx);                     \
        }                                                                      \
    } while (0)

#define multiarrayNativeGetForward(mar, holder, result, idx)                   \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            (result) = multiarraySmallNativeGet(mar, holder, idx);             \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            (result) = multiarrayMediumGetForward(marm(mar), idx);             \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            (result) = multiarrayLargeGetForward(marl(mar), idx);              \
        }                                                                      \
    } while (0)

#define multiarrayNativeGetHead(mar, holder, result)                           \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            (result) = multiarraySmallNativeGetHead(mar, holder);              \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            (result) = multiarrayMediumGetHead(marm(mar));                     \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            (result) = multiarrayLargeGetHead(marl(mar));                      \
        }                                                                      \
    } while (0)

#define multiarrayNativeGetTail(mar, holder, result, count)                    \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            (result) = multiarraySmallNativeGetTail(mar, holder, count);       \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            (result) = multiarrayMediumGetTail(marm(mar));                     \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            (result) = multiarrayLargeGetTail(marl(mar));                      \
        }                                                                      \
    } while (0)

#define multiarrayNativeInsert(mar, holder, rowMax, localCount, idx, s)        \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            if ((localCount) == (rowMax)) {                                    \
                (mar) = (multiarray *)multiarrayMediumNewWithData(             \
                    sizeof(holder), rowMax, localCount, mar);                  \
                (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_MEDIUM);          \
                multiarrayMediumInsert(marm(mar), idx, s);                     \
            } else {                                                           \
                multiarraySmallNativeInsert(mar, holder, localCount, idx);     \
                memcpy((holder *)(mar) + (idx), s, sizeof(holder));            \
            }                                                                  \
            (localCount)++;                                                    \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            /* Grow medium if *total bytes* used by pointer array is greater   \
             * than total size of the array used in Small. */                  \
            if ((sizeof(void *) * marm(mar)->count) >                          \
                (sizeof(holder) * (rowMax))) {                                 \
                (mar) = (multiarray *)multiarrayLargeFromMedium(marm(mar));    \
                (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_LARGE);           \
                multiarrayLargeInsert(marl(mar), idx, s);                      \
            } else {                                                           \
                multiarrayMediumInsert(marm(mar), idx, s);                     \
            }                                                                  \
            (localCount)++;                                                    \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            multiarrayLargeInsert(marl(mar), idx, s);                          \
            (localCount)++;                                                    \
        }                                                                      \
    } while (0)

#define multiarrayNativeDelete(mar, holder, count, idx)                        \
    do {                                                                       \
        const multiarrayType _t = _multiarrayType(mar);                        \
        if (_t == MULTIARRAY_TYPE_NATIVE) {                                    \
            multiarraySmallNativeDelete(mar, holder, count, idx);              \
            (count)--;                                                         \
        } else if (_t == MULTIARRAY_TYPE_MEDIUM) {                             \
            multiarrayMediumDelete(marm(mar), idx);                            \
            (count)--;                                                         \
        } else { /*  MULTIARRAY_TYPE_LARGE: */                                 \
            multiarrayLargeDelete(marl(mar), idx);                             \
            (count)--;                                                         \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Starting with containers...
 * ==================================================================== */
multiarray *multiarrayNew(uint16_t len, uint16_t rowMax);
size_t multiarrayCount(const multiarray *m);
size_t multiarrayBytes(const multiarray *m);

void *multiarrayGet(multiarray *m, multiarrayIdx idx);
void *multiarrayGetHead(multiarray *m);
void *multiarrayGetTail(multiarray *m);
void multiarrayInsertAfter(multiarray **m, multiarrayIdx idx, void *what);
void multiarrayInsertBefore(multiarray **m, multiarrayIdx idx, void *what);
void multiarrayDelete(multiarray **m, const uint32_t index);

#if 0
bool multiarrayDelRange(multiarray **m, const int64_t start, const int64_t stop);
bool multiarrayReplaceAtIndex(multiarray **m, nodeId index, databox *box);

multiarrayIter *multiarrayGetIterator(const multiarray **multiarray, int direction);
multiarrayIter *multiarrayGetIteratorAtIdx(const multiarray **multiarray,
                                         int direction, const int64_t idx);
bool multiarrayNext(multiarrayIter *iter, multiarrayEntry *node);
void multiarrayReleaseIterator(multiarrayIter *iter);

bool multiarrayIndex(const multiarray *multiarray, const int64_t index,
                    multiarrayEntry *entry);

void multiarrayRotate(multiarray *m);
bool multiarrayPop(multiarray **m, int64_t idx, databox *got);
#endif

#ifdef DATAKIT_TEST
int multiarrayTest(int argc, char *argv[]);
#endif
