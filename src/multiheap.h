#pragma once

#include "multimap.h"
#include <assert.h>

/* multiheap is a multimap of {key -> bytes} mappings */
typedef multimap multiheap;
#define multiheapNew() (multiheap *)multimapSetNew(2)
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
