#include "multiarray.h"

#include "multiarrayLarge.h"
#include "multiarrayLargeInternal.h"

#include "multiarrayMedium.h"
#include "multiarrayMediumInternal.h"

#include "multiarraySmall.h"
#include "multiarraySmallInternal.h"

#if MULTIARRAY_INLINE
#include "multiarrayLarge.c"
#include "multiarrayMedium.c"
#include "multiarraySmall.c"
#endif

#include <assert.h>

#define mars(m) ((multiarraySmall *)_MULTIARRAY_USE(m))
#define marm(m) ((multiarrayMedium *)_MULTIARRAY_USE(m))
#define marl(m) ((multiarrayLarge *)_MULTIARRAY_USE(m))

#define _MULTIARRAY(ret, m, func, ...)                                         \
    do {                                                                       \
        switch (_multiarrayType(m)) {                                          \
        case MULTIARRAY_TYPE_SMALL:                                            \
            ret multiarraySmall##func(mars(m), __VA_ARGS__);                   \
            break;                                                             \
        case MULTIARRAY_TYPE_MEDIUM:                                           \
            ret multiarrayMedium##func(marm(m), __VA_ARGS__);                  \
            break;                                                             \
        case MULTIARRAY_TYPE_LARGE:                                            \
            ret multiarrayLarge##func(marl(m), __VA_ARGS__);                   \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTIARRAY_NORETURN(m, func, ...) _MULTIARRAY(, m, func, __VA_ARGS__)
#define MULTIARRAY_RETURN(m, func, ...)                                        \
    _MULTIARRAY(return, m, func, __VA_ARGS__)

#define _MULTIARRAY_SINGLE(ret, m, func)                                       \
    do {                                                                       \
        switch (_multiarrayType(m)) {                                          \
        case MULTIARRAY_TYPE_SMALL:                                            \
            ret multiarraySmall##func(mars(m));                                \
            break;                                                             \
        case MULTIARRAY_TYPE_MEDIUM:                                           \
            ret multiarrayMedium##func(marm(m));                               \
            break;                                                             \
        case MULTIARRAY_TYPE_LARGE:                                            \
            ret multiarrayLarge##func(marl(m));                                \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTIARRAY_SINGLE_NORETURN(m, func) _MULTIARRAY_SINGLE(, m, func)
#define MULTIARRAY_SINGLE_RETURN(m, func) _MULTIARRAY_SINGLE(return, m, func)

#if 0
#define MULTIARRAY_LARGE_ELEMENTS_PER_ENTRY_MAX ((1 << (sizeof(multiarrayElements) * 8) - 1)
size_t multiarrayCount(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Count);
}

size_t multiarrayBytes(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Bytes);
}

static uint32_t MAX_FULL_SIZE = 2048;
#endif

multiarray *multiarrayNew(uint16_t len, uint16_t rowMax) {
    multiarraySmall *mar = multiarraySmallNew(len, rowMax);
    return _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_SMALL);
}

void *multiarrayGet(multiarray *m, multiarrayIdx idx) {
    MULTIARRAY_RETURN(m, Get, idx);
}

void *multiarrayGetHead(multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, GetHead);
}

void *multiarrayGetTail(multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, GetTail);
}

DK_STATIC inline void _multiarrayUpgrade(multiarray **m) {
    /* Now check multiarray to see if we should grow to a larger
     * representation. */
    multiarraySmall *small;
    multiarrayMedium *medium;
    multiarrayLarge *full;
    if (_multiarrayType(*m) == MULTIARRAY_TYPE_SMALL) {
        small = mars(*m);
        if (small->count == small->rowMax) {
            medium = multiarrayMediumFromSmall(small);
            *m = _MULTIARRAY_TAG(medium, MULTIARRAY_TYPE_MEDIUM);
        }
    } else if (_multiarrayType(*m) == MULTIARRAY_TYPE_MEDIUM) {
        medium = marm(*m);
        if (medium->count == medium->rowMax) {
            full = multiarrayLargeFromMedium(medium);
            *m = _MULTIARRAY_TAG(full, MULTIARRAY_TYPE_LARGE);
        }
    }
}

void multiarrayInsert(multiarray **m, multiarrayIdx idx, void *what) {
    /* Insert new elements */
    /* Upgrade *before* insert so we don't end up with dangling
     * containers (e.g. insert 512 items, upgrade container, but
     * never insert a 513 item, so now we have a big container
     * for no reason). */
    _multiarrayUpgrade(m);
    MULTIARRAY_NORETURN(*m, Insert, idx, what);
}

void multiarrayDelete(multiarray **m, const uint32_t index) {
    /* TODO: auto-shrink behavior?  How to decide when to shrink
     * from Large -> Medium -> Small? */
    MULTIARRAY_NORETURN(*m, Delete, index);
}

void multiarrayFree(multiarray *m) {
    MULTIARRAY_SINGLE_NORETURN(m, Free);
}

#ifdef DATAKIT_TEST

#include "ctest.h"

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

int multiarrayTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    printf("Testing direct...\n");
    static const int globalMax = 512000;
    static const int rowMax = 512;
    TEST("create") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        int idx = 0;
        s16 _s;
        multiarrayNativeInsert(s, s16, rowMax, count, idx, &_s);

        multiarrayNativeFree(s);
    }

    TEST("insert before") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = NULL;
            multiarrayNativeGet(s, s16, got, idx);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert before constant zero") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, 0, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = NULL;
            multiarrayNativeGet(s, s16, got, idx);
            assert(got->a == globalMax - 1 - idx);
            assert(got->b == globalMax - 1 - idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert after") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;

        s16 __s = {.a = 0, .b = 0};
        multiarrayNativeInsert(s, s16, rowMax, count, 0, &__s);
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = NULL;
            multiarrayNativeGet(s, s16, got, idx + 1);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert before randoms") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            int32_t i = rand() % (idx + 1);
            multiarrayNativeInsert(s, s16, rowMax, count, i, &_s);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert after") {
        multiarray *s = multiarrayNativeNew(s16);
        assert(s);

        int count = 0;

        s16 __s = {.a = 0, .b = 0};
        multiarrayNativeInsert(s, s16, rowMax, count, 0, &__s);
        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayNativeInsert(s, s16, rowMax, count, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            s16 *got = NULL;
            multiarrayNativeGet(s, s16, got, idx + 1);
            assert(got->a == idx);
            assert(got->b == idx);
        }

        multiarrayNativeFree(s);
    }

    printf("Testing container...\n");
    TEST("create") {
        multiarray *s = multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        int idx = 0;
        s16 _s = {0};
        multiarrayInsert(&s, idx, &_s);

        multiarrayFree(s);
    }

    TEST("insert before") {
        multiarray *s = multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {0};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, idx, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx))->a == idx);
            assert(((s16 *)multiarrayGet(s, idx))->b == idx);
        }

        multiarrayFree(s);
    }

    TEST("insert before constant zero") {
        multiarray *s = multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {0};
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, 0, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx))->a == globalMax - 1 - idx);
            assert(((s16 *)multiarrayGet(s, idx))->b == globalMax - 1 - idx);
        }

        multiarrayFree(s);
    }

    TEST("insert before randoms") {
        multiarray *s = multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayInsert(&s, rand() % (idx + 1), &_s);
        }

        multiarrayNativeFree(s);
    }

    TEST("insert after") {
        multiarray *s = multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        s16 _s = {0};
        multiarrayInsert(&s, 0, &_s);
        for (int idx = 0; idx < globalMax; idx++) {
            _s.a = idx;
            _s.b = idx;
            multiarrayInsert(&s, idx + 1, &_s);
        }

        for (int idx = 0; idx < globalMax; idx++) {
            assert(((s16 *)multiarrayGet(s, idx + 1))->a == idx);
            assert(((s16 *)multiarrayGet(s, idx + 1))->b == idx);
        }

        multiarrayFree(s);
    }

    TEST_FINAL_RESULT;
}

#endif
