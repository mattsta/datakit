#include "varintBitmap.h"
#include "ctest.h"
#include <stdlib.h>
#include <string.h>

int varintBitmapTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Bitmap initialization and cleanup") {
        varintBitmap *bm = varintBitmapCreate();
        if (bm == NULL) {
            ERRR("Failed to create bitmap");
        }

        if (bm->type != VARINT_BITMAP_ARRAY) {
            ERR("Initial type = %d, expected ARRAY", bm->type);
        }

        if (bm->cardinality != 0) {
            ERR("Initial cardinality = %u, expected 0", bm->cardinality);
        }

        varintBitmapFree(bm);
    }

    TEST("Basic add and contains") {
        varintBitmap *bm = varintBitmapCreate();

        /* Add values */
        varintBitmapAdd(bm, 10);
        varintBitmapAdd(bm, 20);
        varintBitmapAdd(bm, 30);

        if (bm->cardinality != 3) {
            ERR("Cardinality = %u, expected 3", bm->cardinality);
        }

        /* Check contains */
        if (!varintBitmapContains(bm, 10)) {
            ERRR("Does not contain 10");
        }
        if (!varintBitmapContains(bm, 20)) {
            ERRR("Does not contain 20");
        }
        if (!varintBitmapContains(bm, 30)) {
            ERRR("Does not contain 30");
        }
        if (varintBitmapContains(bm, 99)) {
            ERRR("Contains non-existent value 99");
        }

        varintBitmapFree(bm);
    }

    TEST("Remove operation") {
        varintBitmap *bm = varintBitmapCreate();

        varintBitmapAdd(bm, 100);
        varintBitmapAdd(bm, 200);

        varintBitmapRemove(bm, 100);

        if (varintBitmapContains(bm, 100)) {
            ERRR("Still contains removed value");
        }
        if (!varintBitmapContains(bm, 200)) {
            ERRR("Lost non-removed value");
        }
        if (bm->cardinality != 1) {
            ERR("Cardinality after remove = %u, expected 1", bm->cardinality);
        }

        varintBitmapFree(bm);
    }

    TEST("Container type adaptation: ARRAY to BITMAP") {
        varintBitmap *bm = varintBitmapCreate();

        /* Add enough values to trigger ARRAY→BITMAP conversion */
        /* Threshold is VARINT_BITMAP_ARRAY_MAX (4096) */
        for (uint32_t i = 0; i < 5000; i++) {
            varintBitmapAdd(bm, (uint16_t)i);
        }

        /* Should have converted to BITMAP container */
        if (bm->type != VARINT_BITMAP_BITMAP) {
            ERR("Type = %d, expected BITMAP after adding 5000 values",
                bm->type);
        }

        /* Verify all values present */
        for (uint32_t i = 0; i < 5000; i++) {
            if (!varintBitmapContains(bm, (uint16_t)i)) {
                ERR("Missing value %u after conversion", i);
                break;
            }
        }

        varintBitmapFree(bm);
    }

    TEST("Container type adaptation: BITMAP to ARRAY") {
        varintBitmap *bm = varintBitmapCreate();

        /* Create BITMAP container */
        for (uint32_t i = 0; i < 5000; i++) {
            varintBitmapAdd(bm, (uint16_t)i);
        }

        /* Remove most values to trigger BITMAP→ARRAY */
        for (uint32_t i = 0; i < 4900; i++) {
            varintBitmapRemove(bm, (uint16_t)i);
        }

        /* Should convert back to ARRAY (cardinality 100 < threshold) */
        if (bm->type != VARINT_BITMAP_ARRAY) {
            ERR("Type = %d, expected ARRAY after removing most values",
                bm->type);
        }

        varintBitmapFree(bm);
    }

    TEST("Set operation: AND") {
        varintBitmap *bm1 = varintBitmapCreate();
        varintBitmap *bm2 = varintBitmapCreate();

        /* bm1: {1, 2, 3, 4, 5} */
        for (uint16_t i = 1; i <= 5; i++) {
            varintBitmapAdd(bm1, i);
        }

        /* bm2: {3, 4, 5, 6, 7} */
        for (uint16_t i = 3; i <= 7; i++) {
            varintBitmapAdd(bm2, i);
        }

        varintBitmap *result = varintBitmapAnd(bm1, bm2);

        /* Result should be {3, 4, 5} */
        if (result->cardinality != 3) {
            ERR("AND cardinality = %u, expected 3", result->cardinality);
        }

        if (!varintBitmapContains(result, 3) ||
            !varintBitmapContains(result, 4) ||
            !varintBitmapContains(result, 5)) {
            ERRR("AND result missing expected values");
        }

        if (varintBitmapContains(result, 1) ||
            varintBitmapContains(result, 7)) {
            ERRR("AND result contains unexpected values");
        }

        varintBitmapFree(bm1);
        varintBitmapFree(bm2);
        varintBitmapFree(result);
    }

    TEST("Set operation: OR") {
        varintBitmap *bm1 = varintBitmapCreate();
        varintBitmap *bm2 = varintBitmapCreate();

        varintBitmapAdd(bm1, 10);
        varintBitmapAdd(bm1, 20);

        varintBitmapAdd(bm2, 20);
        varintBitmapAdd(bm2, 30);

        varintBitmap *result = varintBitmapOr(bm1, bm2);

        /* Result should be {10, 20, 30} */
        if (result->cardinality != 3) {
            ERR("OR cardinality = %u, expected 3", result->cardinality);
        }

        if (!varintBitmapContains(result, 10) ||
            !varintBitmapContains(result, 20) ||
            !varintBitmapContains(result, 30)) {
            ERRR("OR result missing values");
        }

        varintBitmapFree(bm1);
        varintBitmapFree(bm2);
        varintBitmapFree(result);
    }

    TEST("Set operation: XOR") {
        varintBitmap *bm1 = varintBitmapCreate();
        varintBitmap *bm2 = varintBitmapCreate();

        varintBitmapAdd(bm1, 1);
        varintBitmapAdd(bm1, 2);
        varintBitmapAdd(bm1, 3);

        varintBitmapAdd(bm2, 2);
        varintBitmapAdd(bm2, 3);
        varintBitmapAdd(bm2, 4);

        varintBitmap *result = varintBitmapXor(bm1, bm2);

        /* Result should be {1, 4} (symmetric difference) */
        if (result->cardinality != 2) {
            ERR("XOR cardinality = %u, expected 2", result->cardinality);
        }

        if (!varintBitmapContains(result, 1) ||
            !varintBitmapContains(result, 4)) {
            ERRR("XOR result incorrect");
        }

        if (varintBitmapContains(result, 2) ||
            varintBitmapContains(result, 3)) {
            ERRR("XOR result contains common elements");
        }

        varintBitmapFree(bm1);
        varintBitmapFree(bm2);
        varintBitmapFree(result);
    }

    TEST("Set operation: ANDNOT") {
        varintBitmap *bm1 = varintBitmapCreate();
        varintBitmap *bm2 = varintBitmapCreate();

        for (uint16_t i = 1; i <= 10; i++) {
            varintBitmapAdd(bm1, i);
        }

        for (uint16_t i = 5; i <= 15; i++) {
            varintBitmapAdd(bm2, i);
        }

        varintBitmap *result = varintBitmapAndNot(bm1, bm2);

        /* Result should be {1, 2, 3, 4} (bm1 - bm2) */
        if (result->cardinality != 4) {
            ERR("ANDNOT cardinality = %u, expected 4", result->cardinality);
        }

        for (uint16_t i = 1; i <= 4; i++) {
            if (!varintBitmapContains(result, i)) {
                ERR("ANDNOT missing value %u", i);
            }
        }

        for (uint16_t i = 5; i <= 10; i++) {
            if (varintBitmapContains(result, i)) {
                ERR("ANDNOT contains removed value %u", i);
            }
        }

        varintBitmapFree(bm1);
        varintBitmapFree(bm2);
        varintBitmapFree(result);
    }

    TEST("Iterator functionality") {
        varintBitmap *bm = varintBitmapCreate();

        const uint16_t values[] = {5, 15, 25, 35, 45};
        for (int i = 0; i < 5; i++) {
            varintBitmapAdd(bm, values[i]);
        }

        varintBitmapIterator iter = varintBitmapCreateIterator(bm);
        int count = 0;

        while (varintBitmapIteratorNext(&iter)) {
            uint16_t val = iter.currentValue;
            /* Verify it's one of our values */
            int found = 0;
            for (int i = 0; i < 5; i++) {
                if (val == values[i]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                ERR("Iterator returned unexpected value %u", val);
            }
            count++;
        }

        if (count != 5) {
            ERR("Iterator count = %d, expected 5", count);
        }

        varintBitmapFree(bm);
    }

    TEST("Clone operation") {
        varintBitmap *bm = varintBitmapCreate();

        for (uint16_t i = 0; i < 100; i += 10) {
            varintBitmapAdd(bm, i);
        }

        varintBitmap *clone = varintBitmapClone(bm);

        if (clone->cardinality != bm->cardinality) {
            ERR("Clone cardinality = %u, expected %u", clone->cardinality,
                bm->cardinality);
        }

        /* Verify all values present in clone */
        for (uint16_t i = 0; i < 100; i += 10) {
            if (!varintBitmapContains(clone, i)) {
                ERR("Clone missing value %u", i);
            }
        }

        /* Modify original, verify clone unchanged */
        varintBitmapAdd(bm, 999);
        if (varintBitmapContains(clone, 999)) {
            ERRR("Clone affected by original modification");
        }

        varintBitmapFree(bm);
        varintBitmapFree(clone);
    }

    TEST("Clear operation") {
        varintBitmap *bm = varintBitmapCreate();

        for (uint16_t i = 0; i < 50; i++) {
            varintBitmapAdd(bm, i);
        }

        varintBitmapClear(bm);

        if (bm->cardinality != 0) {
            ERR("Cardinality after clear = %u, expected 0", bm->cardinality);
        }

        for (uint16_t i = 0; i < 50; i++) {
            if (varintBitmapContains(bm, i)) {
                ERR("Contains value %u after clear", i);
                break;
            }
        }

        varintBitmapFree(bm);
    }

    TEST("Duplicate add idempotency") {
        varintBitmap *bm = varintBitmapCreate();

        varintBitmapAdd(bm, 42);
        varintBitmapAdd(bm, 42);
        varintBitmapAdd(bm, 42);

        if (bm->cardinality != 1) {
            ERR("Cardinality after duplicate adds = %u, expected 1",
                bm->cardinality);
        }

        varintBitmapFree(bm);
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_BITMAP_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintBitmapTest(argc, argv);
}
#endif
