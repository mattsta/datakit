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
 * ====================================================================
 * The container API works on multiarrays from multiarrayNew /
 * multiarrayNewFromArray.  Given a native multiarray (multiarrayNativeNew:
 * a bare array whose element size only the macro API above knows) or NULL,
 * every call is refused safely: readers return 0 / NULL, writers and Free
 * change nothing, and iterators yield nothing.
 *
 * Indexes count from the front.  Insert takes one up to the count (the
 * append); Get, Replace, Delete, Pop and IteratorInitAt take an element
 * that exists; DeleteRange takes a range inside the array (an empty range
 * changes nothing).  Outside that, Get answers NULL, an iterator yields
 * nothing and a change is refused with the array (and Pop's 'out')
 * unchanged; each reports DK_DIAG_MULTIARRAY_INDEX (dkDiag.h).  GetHead
 * and GetTail answer NULL for an empty array.  The checks sit where each
 * tier already finds the index, so in-range calls cost what they did. */
multiarray *multiarrayNew(uint16_t len, uint16_t rowMax);
void multiarrayFree(multiarray *m);
/* Empty the array: an empty Small with the same element width and rowMax,
 * freeing a larger tier's nodes.  Rewrites '*m'. */
void multiarrayReset(multiarray **m);
/* A deep copy in the same tier and node layout (NULL for a native array
 * or NULL) */
multiarray *multiarrayCopy(const multiarray *m);
/* Pack the elements into full nodes of rowMax (the last holds the rest),
 * in order, in the tier multiarrayNewFromArray would choose: Small at
 * rowMax elements or fewer, Large at rowMax nodes or more, else Medium.
 * O(elements); the old and new nodes coexist while it runs.  Rewrites
 * '*m'; a native array or NULL is refused. */
void multiarrayCompact(multiarray **m);

void *multiarrayGet(multiarray *m, multiarrayIdx idx);
void *multiarrayGetHead(multiarray *m);
void *multiarrayGetTail(multiarray *m);
/* Inserts grow the tier (Small -> Medium at rowMax elements, Medium ->
 * Large at rowMax nodes); Delete, Pop and DeleteRange move a drained array
 * back down (Large -> Medium at rowMax/2 nodes or fewer, Medium -> Small at
 * rowMax/2 elements or fewer in at most two nodes).  Any of them may
 * rewrite '*m': keep the pointer they were given.  An array the Native
 * macros made stays with the macros, even once they promoted it to Medium
 * or Large: a container delete may turn it into a Small, which the macros
 * don't handle. */
void multiarrayInsert(multiarray **m, multiarrayIdx idx, void *what);
void multiarrayDelete(multiarray **m, const uint32_t index);

/* Element count and data-storage bytes (metadata overhead excluded).
 * Container API only — the Native macro API tracks count externally. */
size_t multiarrayCount(const multiarray *m);
size_t multiarrayBytes(const multiarray *m);
/* The tier, nodes (1 for Small; Medium's and Large's node counts),
 * elements, and bytes: the element data, and that plus each tier's struct
 * and node records.  'unitLimit' is a node's capacity in bytes (rowMax
 * elements).  A native array or NULL reports all zeros (no struct holds
 * its count). */
void multiarrayStats(const multiarray *m, dkTierStats *out);

/* The element width and the entries per node it was created with */
uint16_t multiarrayElementLen(const multiarray *m);
uint16_t multiarrayRowMax(const multiarray *m);

/* Copy every element, in order, to 'out' (multiarrayCount * element
 * width bytes): a walk over the nodes, O(n), unlike per-index Get */
void multiarrayCopyTo(const multiarray *m, void *out);

/* A multiarray of 'count' elements of 'len' bytes copied from 'elements',
 * built straight into the tier and nodes inserts would reach (full nodes
 * of 'rowMax'), in O(count).  rowMax and len must be at least 1. */
multiarray *multiarrayNewFromArray(uint16_t len, uint16_t rowMax,
                                   const void *elements, size_t count);

/* Overwrite element 'idx' with the multiarrayElementLen bytes at 'what' */
void multiarrayReplace(multiarray *m, multiarrayIdx idx, const void *what);

/* Copy element 'idx' to 'out' (multiarrayElementLen bytes) and delete it,
 * locating it once */
void multiarrayPop(multiarray **m, multiarrayIdx idx, void *out);

/* Delete the 'count' elements from 'start' (all within the array), located
 * once: whole nodes are dropped and a partial node moves its tail once,
 * where 'count' single Deletes would locate and move 'count' times */
void multiarrayDeleteRange(multiarray **m, multiarrayIdx start, uint32_t count);

/* Move the last element to the front */
void multiarrayRotate(multiarray **m);

/* Walks the elements node by node in either direction: each Next is O(1)
 * amortized, where Get(i) on a Medium or Large walks nodes from the front.
 * Next returns a pointer to the element in place (valid until the array
 * changes; the array must not change during the walk), NULL at the end.
 * The iterator lives on the caller's stack and needs no release. */
typedef struct multiarrayIter {
    const void *tier;
    const void *node; /* Large: the node being walked */
    const void *prev; /* Large: the node behind it in the walk */
    uint32_t nodeIdx; /* Medium: the node being walked */
    int32_t offset;   /* the next element's index in its node */
    uint8_t type;
    bool forward;
} multiarrayIter;

/* From the first element (forward) or the last (backward) */
void multiarrayIteratorInit(const multiarray *m, multiarrayIter *iter,
                            bool forward);
/* From element 'idx' (within the array), in either direction */
void multiarrayIteratorInitAt(const multiarray *m, multiarrayIter *iter,
                              multiarrayIdx idx, bool forward);
void *multiarrayIteratorNext(multiarrayIter *iter);

#ifdef DATAKIT_TEST
int multiarrayTest(int argc, char *argv[]);
#endif
