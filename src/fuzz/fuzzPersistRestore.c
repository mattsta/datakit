/* fuzzPersistRestore - a snapshot body read by its type's restore
 *
 * Input: one byte naming the type (fuzzPickOps), then the snapshot
 * bytes.  A restore either refuses the bytes or gives a structure its
 * validate accepts, whose own snapshot restores to the same count. */

#include "fuzzCommon.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    const persistOps *ops = fuzzPickOps(&data, &len);
    if (!ops) {
        return 0;
    }

    void *token = ops->foldEnter ? ops->foldEnter() : NULL;
    void *restored = ops->restore(data, len);
    if (restored) {
        FUZZ_REQUIRE(!ops->validate || ops->validate(restored));

        size_t snapLen = 0;
        uint8_t *snap = ops->snapshot(restored, &snapLen);
        FUZZ_REQUIRE(snap);
        void *again = ops->restore(snap, snapLen);
        FUZZ_REQUIRE(again);
        FUZZ_REQUIRE(!ops->count || ops->count(again) == ops->count(restored));
        zfree(snap);
        ops->free(again);
        ops->free(restored);
    }

    if (ops->foldExit) {
        ops->foldExit(token);
    }

    return 0;
}
