#pragma once

#include "multimap.h"
#include <assert.h>

/* multiheap is a multimap of {key -> bytes} mappings.
 *
 * MAP MODE IS LOAD-BEARING, NOT A STYLE CHOICE.  A multiheap slot must
 * behave as "save = overwrite this ref's value" (last write wins),
 * because every reader (multiheapReadByKey below) looks the slot up by
 * KEY ONLY.  Map mode gives exactly that: key-only compare, found =>
 * replace the value in place.  SET mode would compare full-width
 * {key,value} and append a second row for a re-saved ref instead of
 * replacing it, leaving key-only readers on an arbitrary duplicate.
 *
 * Pointer discipline: readers (multiheapRead/Restore) take the heap by
 * value; mutators (multiheapInsert/Realloc) take a `multiheap **`
 * because the underlying multimap may reallocate or change tiers. */
typedef multimap multiheap;
#define multiheapNew() (multiheap *)multimapNew(2)
#define multiheapFree(mh) multimapFree(mh)

#define multiheapInsert(heap, ref, obj)                                        \
    multiheapInsertObj(heap, ref, obj, sizeof(*obj))

#define multiheapRealloc(heap, ref, newSize)                                   \
    ({                                                                         \
        databox key_ = {.data.u = ref, .type = DATABOX_UNSIGNED_64};           \
        multiheapReallocByKey(heap, &key_, newSize);                           \
    })

#define multiheapReallocByKey(heap, key, newSize)                              \
    ({                                                                         \
        multimapEntry entry_MHRBK;                                             \
        const bool found = multimapGetUnderlyingEntry((multimap *)*(heap),     \
                                                      key, &entry_MHRBK);      \
        assert(found);                                                         \
        (void)found;                                                           \
                                                                               \
        /* Advance from the key entry to its value entry before                \
         * resizing: entry_MHRBK.fe points AT the key. */                      \
        entry_MHRBK.fe = flexNext(*entry_MHRBK.map, entry_MHRBK.fe);           \
        multimapResizeEntry((multimap **)(heap), &entry_MHRBK, newSize);       \
                                                                               \
        /* Resize could have moved our base pointer, so caller must use        \
         * the newly returned base pointer value — and must treat the        \
         * region's CONTENTS as unspecified after a resize (flex entry         \
         * resizing does not guarantee prefix preservation): repopulate        \
         * through the returned pointer before reading. */                     \
        multiheapReadByKey((multiheap *)*(heap), key);                         \
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

#ifdef DATAKIT_TEST
int multiheapTest(int argc, char *argv[]);
#endif
