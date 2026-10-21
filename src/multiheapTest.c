#ifdef DATAKIT_TEST
#include "multiheap.h"

#include "ctest.h"
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct mhCursor {
    uint64_t position;
    uint64_t extent;
    uint64_t generation;
} mhCursor;

int multiheapTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("insert / read / restore round-trip") {
        multiheap *heap = multiheapNew();

        mhCursor save = {.position = 12345, .extent = 500, .generation = 7};
        multiheapInsert(&heap, 1, &save);

        /* The stored bytes are unaligned: copy them out before reading. */
        const void *live = multiheapRead(heap, 1);
        assert(live);
        mhCursor liveCopy;
        memcpy(&liveCopy, live, sizeof(liveCopy));
        assert(liveCopy.position == 12345 && liveCopy.extent == 500 &&
               liveCopy.generation == 7);

        mhCursor restored = {0};
        multiheapRestore(heap, 1, &restored);
        assert(memcmp(&restored, &save, sizeof(save)) == 0);

        multiheapFree(heap);
    }

    TEST("re-save overwrites in place (map mode; no duplicate rows)") {
        multiheap *heap = multiheapNew();

        mhCursor c = {.position = 1, .extent = 500, .generation = 1};
        multiheapInsert(&heap, 42, &c);

        /* Re-save the same ref with a value that memcmp-sorts BEFORE
         * the original: a full-width (set-mode) store would keep both
         * rows and the key-only reader could land on the stale one. */
        c.extent = 1500;
        c.generation = 2;
        multiheapInsert(&heap, 42, &c);

        mhCursor got = {0};
        multiheapRestore(heap, 42, &got);
        assert(got.extent == 1500 && got.generation == 2);
        assert(multimapCount((multimap *)heap) == 1);

        multiheapFree(heap);
    }

    TEST("many refs stay independently addressable") {
        multiheap *heap = multiheapNew();

        for (uint64_t ref = 0; ref < 300; ref++) {
            mhCursor c = {
                .position = ref, .extent = ref * 3, .generation = ref ^ 0xff};
            multiheapInsert(&heap, ref, &c);
        }

        for (uint64_t ref = 0; ref < 300; ref++) {
            mhCursor got = {0};
            multiheapRestore(heap, ref, &got);
            if (got.position != ref || got.extent != ref * 3 ||
                got.generation != (ref ^ 0xff)) {
                ERR("ref %" PRIu64 " restored wrong contents!", ref);
                assert(NULL);
            }
        }

        multiheapFree(heap);
    }

    TEST("realloc grows the VALUE and returns the live pointer") {
        multiheap *heap = multiheapNew();

        mhCursor c = {.position = 9, .extent = 9, .generation = 9};
        multiheapInsert(&heap, 5, &c);
        /* neighbors so a resize shifts surrounding entries too */
        mhCursor n = {.position = 1, .extent = 1, .generation = 1};
        multiheapInsert(&heap, 4, &n);
        multiheapInsert(&heap, 6, &n);

        /* Contract: contents are unspecified after a resize; the caller
         * repopulates through the returned live pointer. */
        uint8_t *grown = multiheapRealloc(&heap, 5, sizeof(mhCursor) * 4);
        assert(grown);
        memset(grown, 0xAB, sizeof(mhCursor) * 4);

        const uint8_t *reread = multiheapRead(heap, 5);
        assert(reread[0] == 0xAB);
        assert(reread[(sizeof(mhCursor) * 4) - 1] == 0xAB);

        /* The KEY must still be the lookup key (the resize targets the
         * value entry, not the key entry), and neighbors are intact. */
        mhCursor got = {0};
        multiheapRestore(heap, 4, &got);
        assert(got.position == 1);
        multiheapRestore(heap, 6, &got);
        assert(got.position == 1);
        assert(multimapCount((multimap *)heap) == 3);

        /* Shrink back down: repopulate, then round-trip */
        uint8_t *shrunk = multiheapRealloc(&heap, 5, sizeof(mhCursor));
        assert(shrunk);
        c.position = 77;
        memcpy(shrunk, &c, sizeof(c));
        multiheapRestore(heap, 5, &got);
        assert(got.position == 77);

        /* Neighbors survived both resizes */
        multiheapRestore(heap, 4, &got);
        assert(got.position == 1);
        multiheapRestore(heap, 6, &got);
        assert(got.position == 1);

        multiheapFree(heap);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
