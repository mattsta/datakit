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
        s16 _s = {0};
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

    /* ========================================
     * FUZZ TESTS - Oracle-Based Verification
     * ======================================== */
    printf("\n=== MULTIARRAY FUZZ TESTING ===\n\n");

    TEST("FUZZ: random insertions with oracle verification") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        srand(12345);
        const int numOps = 10000;

        /* Oracle is a simple array */
        s16 *oracle = zmalloc(numOps * sizeof(s16));
        int oracleCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (oracleCount > 0) ? (rand() % (oracleCount + 1)) : 0;
            s16 val = {.a = i, .b = i * 2};

            /* Insert into multiarray */
            multiarrayInsert(&mar, idx, &val);

            /* Insert into oracle */
            memmove(&oracle[idx + 1], &oracle[idx],
                    (oracleCount - idx) * sizeof(s16));
            oracle[idx] = val;
            oracleCount++;

            /* Periodic verification */
            if (i % 1000 == 0) {
                for (int j = 0; j < oracleCount; j++) {
                    s16 *got = (s16 *)multiarrayGet(mar, j);
                    if (got->a != oracle[j].a || got->b != oracle[j].b) {
                        ERR("Random insert at round %d idx %d: got (%" PRId64
                            ",%" PRId64 ") "
                            "expected (%" PRId64 ",%" PRId64 ")",
                            i, j, got->a, got->b, oracle[j].a, oracle[j].b);
                    }
                }
            }
        }

        /* Final verification */
        int mismatches = 0;
        for (int j = 0; j < oracleCount; j++) {
            s16 *got = (s16 *)multiarrayGet(mar, j);
            if (got->a != oracle[j].a || got->b != oracle[j].b) {
                if (mismatches < 5) {
                    ERR("Final verify idx %d: got (%" PRId64 ",%" PRId64
                        ") expected "
                        "(%" PRId64 ",%" PRId64 ")",
                        j, got->a, got->b, oracle[j].a, oracle[j].b);
                }
                mismatches++;
            }
        }

        printf("  %d random insertions, %d mismatches\n", numOps, mismatches);
        zfree(oracle);
        multiarrayFree(mar);
    }

    TEST("FUZZ: tier transitions - small to medium to large") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        const int targetCount =
            rowMax * 4; /* Force multiple tier transitions */

        for (int i = 0; i < targetCount; i++) {
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, i, &val);

            /* Verify at tier boundaries */
            if (i == rowMax - 1 || i == rowMax || i == rowMax + 1 ||
                i == rowMax * 2 - 1 || i == rowMax * 2 || i == rowMax * 2 + 1) {
                /* Verify all elements so far */
                for (int j = 0; j <= i; j++) {
                    s16 *got = (s16 *)multiarrayGet(mar, j);
                    if (got->a != j || got->b != -j) {
                        ERR("Tier transition at %d, verify %d: got (%" PRId64
                            ",%" PRId64 ") "
                            "expected (%d,%d)",
                            i, j, got->a, got->b, j, -j);
                    }
                }
            }
        }

        /* Final verification */
        for (int i = 0; i < targetCount; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got->a != i || got->b != -i) {
                ERR("Final at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, i, -i);
            }
        }

        printf("  inserted %d elements through tier transitions\n",
               targetCount);
        multiarrayFree(mar);
    }

    TEST("FUZZ: insert at head repeatedly") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        const int count = 5000;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            multiarrayInsert(&mar, 0, &val); /* Always insert at head */
        }

        /* Verify - elements should be in reverse order */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            int expected = count - 1 - i;
            if (got->a != expected || got->b != expected) {
                ERR("Head insert at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, expected, expected);
            }
        }

        printf("  %d head insertions verified\n", count);
        multiarrayFree(mar);
    }

    TEST("FUZZ: insert at tail repeatedly") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        const int count = 5000;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            multiarrayInsert(&mar, i, &val); /* Insert at tail */
        }

        /* Verify - elements should be in order */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got->a != i || got->b != i) {
                ERR("Tail insert at %d: got (%" PRId64 ",%" PRId64
                    ") expected (%d,%d)",
                    i, got->a, got->b, i, i);
            }
        }

        printf("  %d tail insertions verified\n", count);
        multiarrayFree(mar);
    }

    TEST("FUZZ: alternating head/tail insertions") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        const int count = 4000;
        int currentCount = 0;

        for (int i = 0; i < count; i++) {
            s16 val = {.a = i, .b = i};
            if (i % 2 == 0) {
                multiarrayInsert(&mar, 0, &val); /* Head */
            } else {
                multiarrayInsert(&mar, currentCount, &val); /* Tail */
            }
            currentCount++;
        }

        /* Verify we can read all elements */
        for (int i = 0; i < count; i++) {
            s16 *got = (s16 *)multiarrayGet(mar, i);
            if (got == NULL) {
                ERR("Alternating insert: NULL at idx %d", i);
            }
        }

        printf("  %d alternating insertions verified\n", count);
        multiarrayFree(mar);
    }

    TEST("FUZZ: native API with oracle") {
        multiarray *s = multiarrayNativeNew(s16);
        int count = 0;

        srand(54321);
        const int numOps = 5000;

        /* Oracle */
        s16 *oracle = zmalloc(numOps * sizeof(s16));
        int oracleCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (oracleCount > 0) ? (rand() % (oracleCount + 1)) : 0;
            s16 val = {.a = i, .b = i * 3};

            /* Insert into multiarray */
            multiarrayNativeInsert(s, s16, rowMax, count, idx, &val);

            /* Insert into oracle */
            memmove(&oracle[idx + 1], &oracle[idx],
                    (oracleCount - idx) * sizeof(s16));
            oracle[idx] = val;
            oracleCount++;
        }

        /* Verify all elements */
        int mismatches = 0;
        for (int j = 0; j < oracleCount; j++) {
            s16 *got = NULL;
            multiarrayNativeGet(s, s16, got, j);
            if (got->a != oracle[j].a || got->b != oracle[j].b) {
                if (mismatches < 5) {
                    ERR("Native API verify idx %d: got (%" PRId64 ",%" PRId64
                        ") expected "
                        "(%" PRId64 ",%" PRId64 ")",
                        j, got->a, got->b, oracle[j].a, oracle[j].b);
                }
                mismatches++;
            }
        }

        printf("  %d native API insertions, %d mismatches\n", numOps,
               mismatches);
        zfree(oracle);
        multiarrayNativeFree(s);
    }

    TEST("FUZZ: stress test - 100K insertions") {
        multiarray *mar = multiarrayNew(sizeof(s16), rowMax);

        srand(88888);
        const int numOps = 100000;
        int currentCount = 0;

        for (int i = 0; i < numOps; i++) {
            int idx = (currentCount > 0) ? (rand() % (currentCount + 1)) : 0;
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, idx, &val);
            currentCount++;

            /* Spot check periodically */
            if (i % 10000 == 0 && currentCount > 0) {
                int checkIdx = rand() % currentCount;
                s16 *got = (s16 *)multiarrayGet(mar, checkIdx);
                if (got == NULL) {
                    ERR("100K stress: NULL at round %d, idx %d", i, checkIdx);
                }
            }
        }

        /* Sample verification - check 1000 random indices */
        int verified = 0;
        for (int i = 0; i < 1000; i++) {
            int idx = rand() % currentCount;
            s16 *got = (s16 *)multiarrayGet(mar, idx);
            if (got != NULL) {
                verified++;
            } else {
                ERR("100K stress final: NULL at idx %d", idx);
            }
        }

        printf("  100K insertions completed, sampled %d indices\n", verified);
        multiarrayFree(mar);
    }

    printf("\n=== All multiarray fuzz tests completed! ===\n\n");

    TEST_FINAL_RESULT;
}

#endif
