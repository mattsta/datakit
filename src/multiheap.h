#pragma once

#include "multimap.h"
#include <assert.h>

/* multiheap is a multimap of {key -> bytes} mappings.
 *
 * MAP MODE IS LOAD-BEARING, NOT A STYLE CHOICE.  A multiheap slot must
 * behave as "save = overwrite this ref's value" (last write wins),
 * because every reader (multiheapReadByKey below) looks the slot up by
 * KEY ONLY.  Map mode gives exactly that: key-only compare, found =>
 * replace the value in place (multimapSmall.c: "Maps (!mapIsSet):
 * key-only compare, found => replace values").
 *
 * This was `multimapSetNew` — SET mode — which compares FULL WIDTH
 * {key,value}, so re-saving a ref with a CHANGED value appended a
 * SECOND row for that ref instead of replacing it.  The key-only
 * reader then returned whichever duplicate the sorted search landed
 * on, and because these values are raw little-endian integers stored
 * as DATABOX_BYTES, "sorted" is memcmp order — numerically scrambled
 * (extent=1500 sorts BEFORE extent=500).  A reader that saved
 * yield-resume state more than once therefore restored a STALE value
 * and could never converge: any iteration long enough to yield more
 * than twice resumed from the stale cursor forever and never
 * terminated. */
typedef multimap multiheap;
#define multiheapNew() (multiheap *)multimapNew(2)
#define multiheapFree(mh) multimapFree(mh)

#define multiheapInsert(heap, ref, obj)                                        \
    multiheapInsertObj(heap, ref, obj, sizeof(*obj))

#define multiheapRealloc(heap, ref, newSize)                                   \
    ({                                                                         \
        databox key_ = {.data.u = ref, .type = DATABOX_UNSIGNED_64};           \
        multiheapReallocByKey(heap, &key_);                                    \
    })

#define multiheapReallocByKey(heap, key, newSize)                              \
    ({                                                                         \
        multimapEntry entry_MHRBK;                                             \
        const bool found =                                                     \
            multimapGetUnderlyingEntry((multimap *)*heap, key, &entry_MHRBK);  \
        assert(found);                                                         \
        (void)found;                                                           \
                                                                               \
        multimapResizeEntry((multimap **)heap, &entry_MHRBK, newSize);         \
                                                                               \
        /* Resize could have moved our base pointer, so caller must use the    \
         * newly returned base pointer value. */                               \
        multiheapReadByKey(heap, key);                                         \
    })

/* Macro returns the aligned pointer for 'ref' inside 'heap' */
#define multiheapRead(heap, ref)                                               \
    ({                                                                         \
        databox key_ = {.data.u = ref, .type = DATABOX_UNSIGNED_64};           \
        multiheapReadByKey(heap, &key_);                                       \
    })

/* Macro returns value 'alignedPtr' when called */
DK_FN_UNUSED static void *
multiheapReadByKey(multiheap *restrict const heap,
                   const databox *restrict const key) {
    multimapEntry entry_MHRBK;
    const bool found =
        multimapGetUnderlyingEntry((multimap *)heap, key, &entry_MHRBK);
    assert(found);
    (void)found;

    /* the retrieved mulimapEntry points *to* 'key', so we need to advance
     * by one to reach the value offset: */
    const flexEntry *const valEntry =
        flexNext(*entry_MHRBK.map, entry_MHRBK.fe);

    /* Now strip off the flex metadata bytes to reveal the actual data start */
    databox val;
    flexGetByType(valEntry, &val);

    void *const start = val.data.bytes.start;
    assert(start);

    return start;
}

/* For restoring struct-like things with known sizeof() size */
#define multiheapRestore(heap, ref, copyTo)                                    \
    do {                                                                       \
        memcpy(copyTo, multiheapRead(heap, ref), sizeof(*(copyTo)));           \
    } while (0)

#define multiheapInsertObj(heap, ref, obj, objSize)                            \
    do {                                                                       \
        /* Right now all references are integers, but we could easily          \
         * allow name-based lookups too... */                                  \
        databox key_ = {.data.u = ref, .type = DATABOX_UNSIGNED_64};           \
        databox val_ = {                                                       \
            .len = objSize, .data.ptr = obj, .type = DATABOX_BYTES};           \
        const databox *insertBox[] = {&key_, &val_};                           \
                                                                               \
        multimapInsert((multimap **)heap, insertBox);                          \
    } while (0)
