#pragma once

#include "datakit.h"

/* multiarraySmall is a single array with entires of length 'len'
 * and can be randomly accessed directly by index with no traversals
 * required.
 * This is only efficient for inserts and deletes up to a total array size
 * between 8 KB and 16 KB.
 * (if entires are 16 bytes each, thats 512 to 1024 entries total) */

/* ====================================================================
 * Container / Function API
 * ==================================================================== */
typedef struct multiarraySmall multiarraySmall;

multiarraySmall *multiarraySmallNew(uint16_t len, uint16_t rowMax);
void multiarraySmallFree(multiarraySmall *mar);
void multiarraySmallInsert(multiarraySmall *mar, uint16_t idx, void *s);
void multiarraySmallDelete(multiarraySmall *mar, uint16_t idx);
#define multiarraySmallGet(mar, idx) ((mar)->data + _marsOff((mar)->len, idx))
#define multiarraySmallGetHead(mar) multiarraySmallGet(mar, 0)
#define multiarraySmallGetTail(mar) multiarraySmallGet(mar, (mar)->count - 1)

/* ====================================================================
 * Macro API
 * ==================================================================== */
/* Direct API requires the container be exactly the same type as the elements
 * in the container (e.g. sizeof(*container) must be the width of elements) */
#define _marsOff(width, offset) ((width) * (offset))
#define multiarraySmallDirectNew(e) (zcalloc(1, sizeof(*(e))))
#define multiarraySmallDirectFree(e) (zfree(e))
#define multiarraySmallDirectGet(e, idx) (&(e)[idx])
#define multiarraySmallDirectGetHead(e) multiarraySmallDirectGet(e, 0)
#define multiarraySmallDirectGetTail(e, count)                                 \
    multiarraySmallDirectGet(e, (count) - 1)

/* Native API allows you to use uint8_t to store arbitrary objects by letting
 * you specify the individual element width with each access. */
#define multiarraySmallNativeNew(what) (zcalloc(1, sizeof(what)))
#define multiarraySmallNativeFree(e) (zfree(e))
#define multiarraySmallNativeGet(e, what, idx) (&((what *)(e))[idx])
#define multiarraySmallNativeGetHead(e, what)                                  \
    multiarraySmallNativeGet(e, what, 0)
#define multiarraySmallNativeGetTail(e, what, count)                           \
    multiarraySmallNativeGet(e, what, (count) - 1)

/* SmallDirect macros work directly on an array where each entry
 * has width sizeof(*e).  These let you work on an array directly without
 * needing to round trip through 'multiarraySmall' as long as you know
 * your count up front (and as long as sizeof(*e) is the correct size
 * for your elements). */
/* Note: the 'count' below is the count *before* any changes.  You will
 * manually increment or decrement your external tracking count *after*
 * calling these macros. */
#define multiarraySmallDirectInsert(e, count, idx)                             \
    _multiarraySmallDirectInsert(e, count, idx)
#define multiarraySmallDirectDelete(e, count, idx)                             \
    _multiarraySmallDirectReallocDecrCount(e, count, idx)

#define multiarraySmallNativeInsert(e, holder, count, idx)                     \
    _multiarraySmallNativeInsert(e, holder, count, idx)
#define multiarraySmallNativeDelete(e, holder, count, idx)                     \
    _multiarraySmallNativeReallocDecrCount(e, holder, count, idx)

#ifdef DATAKIT_TEST
int multiarraySmallTest(int argc, char *argv[]);
#endif

/* ====================================================================
 * Native Helpers
 * ==================================================================== */
#define _multiarraySmallNativeGrowShrink(what, holder, count)                  \
    do {                                                                       \
        (what) = zrealloc((what), (sizeof(holder)) * (count));                 \
    } while (0)

/* ====================================================================
 * Native Insert  idx
 * ==================================================================== */
#define _multiarraySmallNativeInsert(what, holder, count, idx)                 \
    do {                                                                       \
        /* Increase => realloc *then* move elements */                         \
        _multiarraySmallNativeGrowShrink(what, holder, (count) + 1);           \
        _multiarraySmallNativeMemmoveOpenSlot(what, holder, count, idx);       \
        /* init new &m->node[idx]; */                                          \
    } while (0)

#define _multiarraySmallNativeMemmoveOpenSlot(what, holder, count, idx)        \
    do {                                                                       \
        /* Basically: if 'idx' is already at the end of the allocation,        \
         * there's nothing emaining to move, we just have one open slot. */    \
        if ((idx) < (count)) {                                                 \
            const uint32_t remaining = (count) - (idx);                        \
            _multiarraySmallNativeMemmoveOpen(what, holder, count, idx,        \
                                              remaining);                      \
        }                                                                      \
    } while (0)

#define _multiarraySmallNativeMemmoveOpen(what, holder, count, idx, remaining) \
    do {                                                                       \
        /* Move elements up one position leaving room for a new                \
         * element at 'idx' */                                                 \
        memmove((holder *)(what) + (idx) + 1, (holder *)(what) + (idx),        \
                (sizeof(holder)) * (remaining));                               \
    } while (0)

/* ====================================================================
 * Native Delete idx
 * ==================================================================== */
#define _multiarraySmallNativeReallocDecrCount(what, holder, count, idx)       \
    do {                                                                       \
        /* Decrease => move elements *then* realloc smaller */                 \
        _multiarraySmallNativeMemmoveCloseSlot(what, holder, count, idx);      \
        _multiarraySmallNativeGrowShrink(what, holder, (count) - 1);           \
    } while (0)

#define _multiarraySmallNativeMemmoveClose(what, holder, count, idx,           \
                                           remaining)                          \
    do {                                                                       \
        memmove((holder *)(what) + (idx), (holder *)(what) + (idx) + 1,        \
                (sizeof(holder)) * (remaining));                               \
    } while (0)

#define _multiarraySmallNativeMemmoveCloseSlot(what, holder, count, idx)       \
    do {                                                                       \
        /* We *don't* minus two here because we already subtracted and we      \
         * are shrinking entries, so we just use the difference directly. */   \
        /* Basically: If we just deleted the last slot, we don't need to       \
         * memmove anything because it got realloc()'d away into nothing. */   \
        if ((idx) < (count)) {                                                 \
            const uint32_t remaining = ((count) - (idx) - 1);                  \
            _multiarraySmallNativeMemmoveClose(what, holder, count, idx,       \
                                               remaining);                     \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Direct Helpers
 * ==================================================================== */
#define _multiarraySmallDirectGrowShrink(what, count)                          \
    do {                                                                       \
        (what) = zrealloc((what), (sizeof(*(what))) * (count));                \
    } while (0)

/* ====================================================================
 * Direct Insert  idx
 * ==================================================================== */
#define _multiarraySmallDirectInsert(what, count, idx)                         \
    do {                                                                       \
        /* Increase => realloc *then* move elements */                         \
        _multiarraySmallDirectGrowShrink(what, (count) + 1);                   \
        _multiarraySmallDirectMemmoveOpenSlot(what, count, idx);               \
        /* init new &m->node[idx]; */                                          \
    } while (0)

#define _multiarraySmallDirectMemmoveOpenSlot(what, count, idx)                \
    do {                                                                       \
        /* Basically: if 'idx' is already at the end of the allocation,        \
         * there's nothing emaining to move, we just have one open slot. */    \
        if ((idx) < (count)) {                                                 \
            const uint32_t remaining = (count) - (idx);                        \
            _multiarraySmallDirectMemmoveOpen(what, count, idx, remaining);    \
        }                                                                      \
    } while (0)

#define _multiarraySmallDirectMemmoveOpen(what, count, idx, remaining)         \
    do {                                                                       \
        /* Move elements up one position leaving room for a new                \
         * element at 'idx' */                                                 \
        memmove((what) + (idx) + 1, (what) + (idx),                            \
                (sizeof(*(what))) * (remaining));                              \
    } while (0)

/* ====================================================================
 * Direct Delete idx
 * ==================================================================== */
#define _multiarraySmallDirectReallocDecrCount(what, count, idx)               \
    do {                                                                       \
        /* Decrease => move elements *then* realloc smaller */                 \
        _multiarraySmallDirectMemmoveCloseSlot(what, count, idx);              \
        _multiarraySmallDirectGrowShrink(what, (count) - 1);                   \
    } while (0)

#define _multiarraySmallDirectMemmoveClose(what, count, idx, remaining)        \
    do {                                                                       \
        memmove((what) + (idx), (what) + (idx) + 1,                            \
                (sizeof(*(what))) * (remaining));                              \
    } while (0)

#define _multiarraySmallDirectMemmoveCloseSlot(what, count, idx)               \
    do {                                                                       \
        /* We *don't* minus two here because we already subtracted and we      \
         * are shrinking entries, so we just use the difference directly. */   \
        /* Basically: If we just deleted the last slot, we don't need to       \
         * memmove anything because it got realloc()'d away into nothing. */   \
        if ((idx) < (count)) {                                                 \
            const uint32_t remaining = ((count) - (idx) - 1);                  \
            _multiarraySmallDirectMemmoveClose(what, count, idx, remaining);   \
        }                                                                      \
    } while (0)
