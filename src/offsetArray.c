#include "offsetArray.h"
#include "datakit.h"

const char *dummy(void) {
    return "non-empty object when testing disabled";
}

#ifdef DATAKIT_TEST
offsetArrayCreateTypes(Tester, int, int);

int offsetArrayTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    offsetArrayTester a = {0};

    offsetArrayGrow(&a, 100);
    offsetArrayGet(&a, 100) = 1;
    assert(offsetArrayGet(&a, 100));

    offsetArrayGrow(&a, 200);
    offsetArrayGet(&a, 200) = 1;
    assert(offsetArrayGet(&a, 200));

    assert(a.highest == 200);
    assert(a.offset == 100);

    offsetArrayGrow(&a, 50);
    offsetArrayGet(&a, 50) = 1;
    assert(a.offset == 50);

    assert(offsetArrayGet(&a, 50) == 1);
    assert(offsetArrayGet(&a, 100) == 1);
    assert(offsetArrayGet(&a, 200) == 1);

    for (int i = 50; i >= 0; i--) {
        offsetArrayGrow(&a, i);
        assert(a.offset == i);
        offsetArrayGet(&a, i) = i;
    }

    for (int i = 0; i < 600; i++) {
        offsetArrayGrow(&a, i);
        offsetArrayGet(&a, i) = i;
    }

    zfree(a.obj);
    a = (offsetArrayTester){0};

    for (int i = 8192; i >= 0; i--) {
        offsetArrayGrow(&a, i);
        offsetArrayGet(&a, i) = i;
    }

    zfree(a.obj);
    a = (offsetArrayTester){0};

    for (int i = 0; i < 8192; i++) {
        offsetArrayGrow(&a, i);
        offsetArrayGet(&a, i) = i;
    }

    zfree(a.obj);
    a = (offsetArrayTester){0};

    for (int i = 77; i < 8192; i++) {
        offsetArrayGrow(&a, i);
        offsetArrayGet(&a, i) = i;

        offsetArrayGrow(&a, i + 1);
        offsetArrayGrow(&a, i + 2);
        offsetArrayGrow(&a, i - 1);
        offsetArrayGrow(&a, i - 2);
    }

    zfree(a.obj);
    a = (offsetArrayTester){0};

    assert(printf("ALL TESTS PASSED!\n"));
    return 0;
}
#endif
