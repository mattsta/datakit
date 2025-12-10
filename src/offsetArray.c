/* offsetArray - Sparse Array with Automatic Offset Adjustment
 *
 * See offsetArray.h for API documentation.
 */

#include "offsetArray.h"
#include "datakit.h"

/* Placeholder for non-test builds */
const char *offsetArrayDummy_(void) {
    return "offsetArray";
}

#ifdef DATAKIT_TEST
#include "ctest.h"

/* Test types */
offsetArrayCreateTypes(Int, int, int);
offsetArrayCreateTypes(SizeT, size_t, size_t);

/* Test struct for complex storage */
typedef struct {
    int id;
    char name[32];
    double value;
} TestStruct;
offsetArrayCreateTypes(Struct, TestStruct, int);

int offsetArrayTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* ----------------------------------------------------------------
     * Basic Operations
     * ---------------------------------------------------------------- */

    TEST("empty array state") {
        offsetArrayInt a = {0};
        if (!offsetArrayEmpty(&a)) {
            ERRR("New array should be empty");
        }
        if (offsetArrayCount(&a) != 0) {
            ERR("Empty array count should be 0, got %zu", offsetArrayCount(&a));
        }
    }

    TEST("first grow sets offset and highest") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);

        if (offsetArrayEmpty(&a)) {
            ERRR("Array should not be empty after grow");
        }
        if (a.offset != 100) {
            ERR("offset should be 100, got %d", a.offset);
        }
        if (a.highest != 100) {
            ERR("highest should be 100, got %d", a.highest);
        }
        if (offsetArrayCount(&a) != 1) {
            ERR("count should be 1, got %zu", offsetArrayCount(&a));
        }
        if (offsetArrayLow(&a) != 100) {
            ERR("Low should be 100, got %d", offsetArrayLow(&a));
        }
        if (offsetArrayHigh(&a) != 100) {
            ERR("High should be 100, got %d", offsetArrayHigh(&a));
        }

        offsetArrayFree(&a);
    }

    TEST("grow upward") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGet(&a, 100) = 1000;

        offsetArrayGrow(&a, 200);
        offsetArrayGet(&a, 200) = 2000;

        if (a.offset != 100) {
            ERR("offset should stay 100, got %d", a.offset);
        }
        if (a.highest != 200) {
            ERR("highest should be 200, got %d", a.highest);
        }
        if (offsetArrayCount(&a) != 101) {
            ERR("count should be 101, got %zu", offsetArrayCount(&a));
        }
        if (offsetArrayGet(&a, 100) != 1000) {
            ERR("Value at 100 should be 1000, got %d", offsetArrayGet(&a, 100));
        }
        if (offsetArrayGet(&a, 200) != 2000) {
            ERR("Value at 200 should be 2000, got %d", offsetArrayGet(&a, 200));
        }

        offsetArrayFree(&a);
    }

    TEST("grow downward preserves data") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGet(&a, 100) = 1000;

        offsetArrayGrow(&a, 200);
        offsetArrayGet(&a, 200) = 2000;

        /* Now grow downward */
        offsetArrayGrow(&a, 50);
        offsetArrayGet(&a, 50) = 500;

        if (a.offset != 50) {
            ERR("offset should be 50, got %d", a.offset);
        }
        if (a.highest != 200) {
            ERR("highest should stay 200, got %d", a.highest);
        }
        if (offsetArrayCount(&a) != 151) {
            ERR("count should be 151, got %zu", offsetArrayCount(&a));
        }

        /* Verify data survived memmove */
        if (offsetArrayGet(&a, 50) != 500) {
            ERR("Value at 50 should be 500, got %d", offsetArrayGet(&a, 50));
        }
        if (offsetArrayGet(&a, 100) != 1000) {
            ERR("Value at 100 should be 1000 after memmove, got %d",
                offsetArrayGet(&a, 100));
        }
        if (offsetArrayGet(&a, 200) != 2000) {
            ERR("Value at 200 should be 2000 after memmove, got %d",
                offsetArrayGet(&a, 200));
        }

        offsetArrayFree(&a);
    }

    TEST("grow to existing index is no-op") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGet(&a, 100) = 42;

        void *oldObj = a.obj;
        offsetArrayGrow(&a, 100); /* Should be no-op */

        if (a.obj != oldObj) {
            ERRR("Grow to existing index should not reallocate");
        }
        if (offsetArrayGet(&a, 100) != 42) {
            ERR("Value should be unchanged, got %d", offsetArrayGet(&a, 100));
        }

        offsetArrayFree(&a);
    }

    TEST("grow within range is no-op") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGrow(&a, 200);
        offsetArrayGet(&a, 150) = 1500;

        void *oldObj = a.obj;
        int oldOffset = a.offset;
        int oldHighest = a.highest;

        offsetArrayGrow(&a, 150); /* Already in range */

        if (a.obj != oldObj || a.offset != oldOffset ||
            a.highest != oldHighest) {
            ERRR("Grow within range should be complete no-op");
        }
        if (offsetArrayGet(&a, 150) != 1500) {
            ERR("Value should be unchanged, got %d", offsetArrayGet(&a, 150));
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * offsetArrayContains
     * ---------------------------------------------------------------- */

    TEST("contains checks bounds") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGrow(&a, 200);

        if (!offsetArrayContains(&a, 100)) {
            ERRR("Should contain low bound");
        }
        if (!offsetArrayContains(&a, 200)) {
            ERRR("Should contain high bound");
        }
        if (!offsetArrayContains(&a, 150)) {
            ERRR("Should contain middle value");
        }
        if (offsetArrayContains(&a, 99)) {
            ERRR("Should not contain below low");
        }
        if (offsetArrayContains(&a, 201)) {
            ERRR("Should not contain above high");
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * offsetArrayDirect (iteration)
     * ---------------------------------------------------------------- */

    TEST("direct access for iteration") {
        offsetArrayInt a = {0};

        /* Create array from 100 to 104 */
        for (int i = 100; i <= 104; i++) {
            offsetArrayGrow(&a, i);
            offsetArrayGet(&a, i) = i * 10;
        }

        /* Iterate using direct access */
        size_t count = offsetArrayCount(&a);
        if (count != 5) {
            ERR("Count should be 5, got %zu", count);
        }

        for (size_t i = 0; i < count; i++) {
            int expected = (100 + (int)i) * 10;
            if (offsetArrayDirect(&a, i) != expected) {
                ERR("Direct[%zu] should be %d, got %d", i, expected,
                    offsetArrayDirect(&a, i));
            }
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * offsetArrayFree
     * ---------------------------------------------------------------- */

    TEST("free resets to empty state") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGrow(&a, 200);

        offsetArrayFree(&a);

        if (!offsetArrayEmpty(&a)) {
            ERRR("Array should be empty after free");
        }
        if (a.obj != NULL) {
            ERRR("obj should be NULL after free");
        }
        if (a.offset != 0) {
            ERR("offset should be 0 after free, got %d", a.offset);
        }
        if (a.highest != 0) {
            ERR("highest should be 0 after free, got %d", a.highest);
        }
        if (offsetArrayCount(&a) != 0) {
            ERR("count should be 0 after free, got %zu", offsetArrayCount(&a));
        }
    }

    TEST("can reuse array after free") {
        offsetArrayInt a = {0};
        offsetArrayGrow(&a, 100);
        offsetArrayGet(&a, 100) = 1;
        offsetArrayFree(&a);

        /* Reuse with different offset */
        offsetArrayGrow(&a, 500);
        offsetArrayGet(&a, 500) = 5;

        if (a.offset != 500) {
            ERR("New offset should be 500, got %d", a.offset);
        }
        if (offsetArrayGet(&a, 500) != 5) {
            ERR("Value should be 5, got %d", offsetArrayGet(&a, 500));
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * offsetArrayGrowZero
     * ---------------------------------------------------------------- */

    TEST("growZero initializes to zero on first grow") {
        offsetArrayInt a = {0};
        offsetArrayGrowZero(&a, 100);

        if (offsetArrayGet(&a, 100) != 0) {
            ERR("GrowZero should initialize to 0, got %d",
                offsetArrayGet(&a, 100));
        }

        offsetArrayFree(&a);
    }

    TEST("growZero zeros new upward elements") {
        offsetArrayInt a = {0};
        offsetArrayGrowZero(&a, 100);
        offsetArrayGet(&a, 100) = 1000; /* Set known value */

        offsetArrayGrowZero(&a, 105);

        /* Original should be preserved */
        if (offsetArrayGet(&a, 100) != 1000) {
            ERR("Original value should be preserved, got %d",
                offsetArrayGet(&a, 100));
        }

        /* New elements 101-105 should be zero */
        for (int i = 101; i <= 105; i++) {
            if (offsetArrayGet(&a, i) != 0) {
                ERR("Element %d should be 0, got %d", i, offsetArrayGet(&a, i));
            }
        }

        offsetArrayFree(&a);
    }

    TEST("growZero zeros new downward elements") {
        offsetArrayInt a = {0};
        offsetArrayGrowZero(&a, 100);
        offsetArrayGet(&a, 100) = 1000;

        offsetArrayGrowZero(&a, 95);

        /* Original should be preserved */
        if (offsetArrayGet(&a, 100) != 1000) {
            ERR("Original value should be preserved, got %d",
                offsetArrayGet(&a, 100));
        }

        /* New elements 95-99 should be zero */
        for (int i = 95; i <= 99; i++) {
            if (offsetArrayGet(&a, i) != 0) {
                ERR("Element %d should be 0, got %d", i, offsetArrayGet(&a, i));
            }
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * Complex Storage Type
     * ---------------------------------------------------------------- */

    TEST("struct storage") {
        offsetArrayStruct a = {0};

        offsetArrayGrow(&a, 10);
        TestStruct *s = &offsetArrayGet(&a, 10);
        s->id = 42;
        snprintf(s->name, sizeof(s->name), "test");
        s->value = 3.14;

        offsetArrayGrow(&a, 20);
        offsetArrayGet(&a, 20).id = 99;

        /* Verify first struct survived */
        if (offsetArrayGet(&a, 10).id != 42) {
            ERR("Struct id should be 42, got %d", offsetArrayGet(&a, 10).id);
        }
        if (strcmp(offsetArrayGet(&a, 10).name, "test") != 0) {
            ERR("Struct name should be 'test', got '%s'",
                offsetArrayGet(&a, 10).name);
        }
        if (offsetArrayGet(&a, 20).id != 99) {
            ERR("Second struct id should be 99, got %d",
                offsetArrayGet(&a, 20).id);
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * size_t Index Type
     * ---------------------------------------------------------------- */

    TEST("size_t index type") {
        offsetArraySizeT a = {0};

        size_t idx1 = 1000000;
        size_t idx2 = 1000100;

        offsetArrayGrow(&a, idx1);
        offsetArrayGet(&a, idx1) = 111;

        offsetArrayGrow(&a, idx2);
        offsetArrayGet(&a, idx2) = 222;

        if (offsetArrayCount(&a) != 101) {
            ERR("count should be 101, got %zu", offsetArrayCount(&a));
        }
        if (offsetArrayGet(&a, idx1) != 111) {
            ERR("Value at idx1 should be 111, got %zu",
                offsetArrayGet(&a, idx1));
        }
        if (offsetArrayGet(&a, idx2) != 222) {
            ERR("Value at idx2 should be 222, got %zu",
                offsetArrayGet(&a, idx2));
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * Zero Index
     * ---------------------------------------------------------------- */

    TEST("zero index") {
        offsetArrayInt a = {0};

        offsetArrayGrow(&a, 0);
        offsetArrayGet(&a, 0) = 42;

        if (a.offset != 0) {
            ERR("offset should be 0, got %d", a.offset);
        }
        if (offsetArrayGet(&a, 0) != 42) {
            ERR("Value at 0 should be 42, got %d", offsetArrayGet(&a, 0));
        }

        /* Grow upward from zero */
        offsetArrayGrow(&a, 5);
        offsetArrayGet(&a, 5) = 55;

        if (a.offset != 0) {
            ERR("offset should stay 0, got %d", a.offset);
        }
        if (offsetArrayGet(&a, 0) != 42) {
            ERR("Value at 0 should still be 42, got %d", offsetArrayGet(&a, 0));
        }
        if (offsetArrayGet(&a, 5) != 55) {
            ERR("Value at 5 should be 55, got %d", offsetArrayGet(&a, 5));
        }

        offsetArrayFree(&a);
    }

    /* ----------------------------------------------------------------
     * Stress Tests
     * ---------------------------------------------------------------- */

    TEST("sequential upward growth") {
        offsetArrayInt a = {0};

        for (int i = 1000; i < 2000; i++) {
            offsetArrayGrow(&a, i);
            offsetArrayGet(&a, i) = i;
        }

        if (offsetArrayCount(&a) != 1000) {
            ERR("count should be 1000, got %zu", offsetArrayCount(&a));
        }

        for (int i = 1000; i < 2000; i++) {
            if (offsetArrayGet(&a, i) != i) {
                ERR("Value at %d should be %d, got %d", i, i,
                    offsetArrayGet(&a, i));
                break;
            }
        }

        offsetArrayFree(&a);
    }

    TEST("sequential downward growth") {
        offsetArrayInt a = {0};

        for (int i = 2000; i >= 1000; i--) {
            offsetArrayGrow(&a, i);
            offsetArrayGet(&a, i) = i;
        }

        if (offsetArrayCount(&a) != 1001) {
            ERR("count should be 1001, got %zu", offsetArrayCount(&a));
        }

        for (int i = 1000; i <= 2000; i++) {
            if (offsetArrayGet(&a, i) != i) {
                ERR("Value at %d should be %d, got %d", i, i,
                    offsetArrayGet(&a, i));
                break;
            }
        }

        offsetArrayFree(&a);
    }

    TEST("alternating growth pattern") {
        offsetArrayInt a = {0};

        /* Start in middle, alternate up and down */
        offsetArrayGrow(&a, 500);
        offsetArrayGet(&a, 500) = 500;

        for (int i = 1; i <= 100; i++) {
            offsetArrayGrow(&a, 500 + i);
            offsetArrayGet(&a, 500 + i) = 500 + i;

            offsetArrayGrow(&a, 500 - i);
            offsetArrayGet(&a, 500 - i) = 500 - i;
        }

        if (a.offset != 400) {
            ERR("offset should be 400, got %d", a.offset);
        }
        if (a.highest != 600) {
            ERR("highest should be 600, got %d", a.highest);
        }
        if (offsetArrayCount(&a) != 201) {
            ERR("count should be 201, got %zu", offsetArrayCount(&a));
        }

        /* Verify all values */
        for (int i = 400; i <= 600; i++) {
            if (offsetArrayGet(&a, i) != i) {
                ERR("Value at %d should be %d, got %d", i, i,
                    offsetArrayGet(&a, i));
                break;
            }
        }

        offsetArrayFree(&a);
    }

    TEST_FINAL_RESULT;
}
#endif
