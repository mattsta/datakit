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
/* @own: caller; free=multiheapFree */
#define multiheapNew() (multiheap *)multimapNew(2)
#define multiheapFree(mh) multimapFree(mh)

#define multiheapInsert(heap, ref, obj)                                        \
    multiheapInsertObj(heap, ref, obj, sizeof(*obj))

/* The bytes stored under 'key' */
DK_FN_UNUSED static void *
multiheapReadByKey(multiheap *restrict const heap,
                   const databox *restrict const key) {
    multimapEntry entry_MHRBK;
    [[maybe_unused]] const bool found =
        multimapGetUnderlyingEntry((multimap *)heap, key, &entry_MHRBK);
    assert(found);

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

/* A pointer to the bytes stored for 'ref' inside 'heap'.  The bytes live
 * inside a flex entry and are NOT aligned for any type: copy them out with
 * multiheapRestore (memcpy) rather than dereferencing the pointer as a
 * struct. */
DK_FN_UNUSED static void *multiheapRead(multiheap *heap, uint64_t ref) {
    const databox key = {.data.u = ref, .type = DATABOX_UNSIGNED_64};
    return multiheapReadByKey(heap, &key);
}

/* Resize the bytes under 'key' to 'newSize' and return where they now
 * are: the resize can move them, and their CONTENTS are unspecified after
 * it (flex entry resizing does not preserve a prefix), so repopulate
 * through the returned pointer before reading. */
DK_FN_UNUSED static void *
multiheapReallocByKey(multiheap **heap, const databox *key, size_t newSize) {
    multimapEntry entry;
    [[maybe_unused]] const bool found =
        multimapGetUnderlyingEntry((multimap *)*heap, key, &entry);
    assert(found);

    /* The entry points AT the key: resize the value after it */
    entry.fe = flexNext(*entry.map, entry.fe);
    multimapResizeEntry((multimap **)heap, &entry, newSize);
    return multiheapReadByKey(*heap, key);
}

DK_FN_UNUSED static void *multiheapRealloc(multiheap **heap, uint64_t ref,
                                           size_t newSize) {
    const databox key = {.data.u = ref, .type = DATABOX_UNSIGNED_64};
    return multiheapReallocByKey(heap, &key, newSize);
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
