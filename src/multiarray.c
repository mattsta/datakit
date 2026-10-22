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

/* Count/Bytes only apply to the tagged container API (multiarrayNew);
 * the Native macro API tracks its count externally by design. */
size_t multiarrayCount(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Count);
}

size_t multiarrayBytes(const multiarray *m) {
    MULTIARRAY_SINGLE_RETURN(m, Bytes);
}

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

uint16_t multiarrayElementLen(const multiarray *m) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        return mars(m)->len;
    case MULTIARRAY_TYPE_MEDIUM:
        return marm(m)->len;
    case MULTIARRAY_TYPE_LARGE:
        return marl(m)->len;
    default:
        assert(NULL && "container API on a native multiarray");
        __builtin_unreachable();
    }
}

uint16_t multiarrayRowMax(const multiarray *m) {
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL:
        return mars(m)->rowMax;
    case MULTIARRAY_TYPE_MEDIUM:
        return marm(m)->rowMax;
    case MULTIARRAY_TYPE_LARGE:
        return marl(m)->rowMax;
    default:
        assert(NULL && "container API on a native multiarray");
        __builtin_unreachable();
    }
}

void multiarrayCopyTo(const multiarray *m, void *out) {
    uint8_t *to = out;
    switch (_multiarrayType(m)) {
    case MULTIARRAY_TYPE_SMALL: {
        const multiarraySmall *s = mars(m);
        if (s->count) {
            memcpy(to, s->data, (size_t)s->count * s->len);
        }
        break;
    }
    case MULTIARRAY_TYPE_MEDIUM: {
        const multiarrayMedium *med = marm(m);
        for (uint32_t i = 0; i < med->count; i++) {
            const size_t bytes = (size_t)med->node[i].count * med->len;
            if (bytes) {
                memcpy(to, med->node[i].data, bytes);
                to += bytes;
            }
        }
        break;
    }
    case MULTIARRAY_TYPE_LARGE: {
        const multiarrayLarge *large = marl(m);
        multiarrayLargeNode *prev = NULL;
        multiarrayLargeNode *cur = large->head;
        while (cur) {
            const size_t bytes = (size_t)cur->count * large->len;
            memcpy(to, cur->data, bytes);
            to += bytes;
            /* XOR-linked: the next node is prev ^ cur->prevNext */
            multiarrayLargeNode *next =
                (multiarrayLargeNode *)((uintptr_t)prev ^
                                        (uintptr_t)cur->prevNext);
            prev = cur;
            cur = next;
        }
        break;
    }
    default:
        assert(NULL && "container API on a native multiarray");
        __builtin_unreachable();
    }
}

multiarray *multiarrayNewFromArray(uint16_t len, uint16_t rowMax,
                                   const void *elements, size_t count) {
    const uint8_t *from = elements;

    /* One node's worth: Small */
    if (count <= rowMax) {
        multiarraySmall *s = multiarraySmallNew(len, rowMax);
        if (count) {
            /* @own: container; free=multiarraySmallFree */
            s->data = zrealloc(s->data, count * len);
            memcpy(s->data, from, count * len);
            s->count = (uint16_t)count;
        }
        return _MULTIARRAY_TAG(s, MULTIARRAY_TYPE_SMALL);
    }

    /* Full nodes of rowMax entries (the last holds the rest) in a Medium,
     * which becomes Large at rowMax nodes, as inserts would make it */
    const size_t nodes = (count + rowMax - 1) / rowMax;
    /* @own: caller; free=multiarrayMediumFree */
    multiarrayMedium *med = zcalloc(1, sizeof(*med));
    /* @own: container; free=multiarrayMediumFree */
    med->node = zcalloc(nodes, sizeof(*med->node));
    med->count = (uint32_t)nodes;
    med->len = len;
    med->rowMax = rowMax;
    for (size_t i = 0; i < nodes; i++) {
        const size_t here = i + 1 < nodes ? rowMax : count - i * rowMax;
        /* @own: container; free=multiarrayMediumFree */
        med->node[i].data = zmalloc(here * len);
        memcpy(med->node[i].data, from + i * rowMax * (size_t)len, here * len);
        med->node[i].count = (uint16_t)here;
    }

    if (nodes >= rowMax) {
        multiarrayLarge *large = multiarrayLargeFromMedium(med);
        return _MULTIARRAY_TAG(large, MULTIARRAY_TYPE_LARGE);
    }
    return _MULTIARRAY_TAG(med, MULTIARRAY_TYPE_MEDIUM);
}

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "str.h"

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

defcleanerName(s16 *, zfree, S16);
defcleaner(multiarray *, multiarrayFree);

int multiarrayTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    printf("Testing direct...\n");
    /* Small up to rowMax, Medium up to rowMax * rowMax, Large beyond: three
     * times the Large threshold covers every tier and its transitions */
    static const int rowMax = 64;
    static const int globalMax = 3 * 64 * 64;
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
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        int idx = 0;
        s16 _s = {0};
        multiarrayInsert(&s, idx, &_s);
    }

    TEST("insert before") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
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
    }

    TEST("insert before constant zero") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
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
    }

    TEST("insert before randoms") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
        assert(s);

        for (int idx = 0; idx < globalMax; idx++) {
            s16 _s = {.a = idx, .b = idx};
            multiarrayInsert(&s, rand() % (idx + 1), &_s);
        }
    }

    TEST("insert after") {
        cleaner(multiarrayFree) multiarray *s =
            multiarrayNew(sizeof(s16), rowMax);
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
    }

    /* ========================================
     * FUZZ TESTS - Oracle-Based Verification
     * ======================================== */
    printf("\n=== MULTIARRAY FUZZ TESTING ===\n\n");

    TEST("FUZZ: random insertions with oracle verification") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

        srand(12345);
        const int numOps = 10000;

        /* Oracle is a simple array */
        cleanerName(zfree, S16) s16 *oracle = zmalloc(numOps * sizeof(s16));
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
    }

    TEST("FUZZ: tier transitions - small to medium to large") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

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
    }

    TEST("Count/Bytes: exact across all tiers, inserts and deletes") {
        /* Small rowMax so the sweep is cheap: Small upgrades to Medium
         * at rowMax ELEMENTS, but Medium upgrades to Large only at
         * rowMax NODES (rowMax^2 elements) — with 8 that's 8 and 64. */
        const int tinyRowMax = 8;
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), tinyRowMax);
        const int targetCount = tinyRowMax * tinyRowMax * 2;

        /* Empty container reports zero */
        assert(multiarrayCount(mar) == 0);
        assert(multiarrayBytes(mar) == 0);

        bool sawSmall = false;
        bool sawMedium = false;
        bool sawLarge = false;
        for (int i = 0; i < targetCount; i++) {
            s16 val = {.a = i, .b = -i};
            multiarrayInsert(&mar, i, &val);

            const size_t expect = (size_t)i + 1;
            if (multiarrayCount(mar) != expect) {
                ERR("Count after insert %d: got %zu expected %zu", i,
                    multiarrayCount(mar), expect);
            }

            assert(multiarrayBytes(mar) == expect * sizeof(s16));

            switch (_multiarrayType(mar)) {
            case MULTIARRAY_TYPE_SMALL:
                sawSmall = true;
                break;
            case MULTIARRAY_TYPE_MEDIUM:
                sawMedium = true;
                break;
            case MULTIARRAY_TYPE_LARGE:
                sawLarge = true;
                break;
            default:
                assert(NULL);
            }
        }

        /* The sweep must exercise every tier or the accessors are only
         * partially covered. */
        assert(sawSmall && sawMedium && sawLarge);

        /* Deletes shrink the count symmetrically (Large tier). */
        size_t remaining = targetCount;
        while (remaining > (size_t)tinyRowMax) {
            multiarrayDelete(&mar, 0);
            remaining--;
            assert(multiarrayCount(mar) == remaining);
            assert(multiarrayBytes(mar) == remaining * sizeof(s16));
        }

        printf("  Count/Bytes verified across %d inserts + deletes\n",
               targetCount);
    }

    TEST("FUZZ: insert at head repeatedly") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

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
    }

    TEST("FUZZ: insert at tail repeatedly") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

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
    }

    TEST("FUZZ: alternating head/tail insertions") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

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
    }

    TEST("FUZZ: native API with oracle") {
        multiarray *s = multiarrayNativeNew(s16);
        int count = 0;

        srand(54321);
        const int numOps = 5000;

        /* Oracle */
        cleanerName(zfree, S16) s16 *oracle = zmalloc(numOps * sizeof(s16));
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
        multiarrayNativeFree(s);
    }

    TEST("FUZZ: stress test - 100K insertions") {
        cleaner(multiarrayFree) multiarray *mar =
            multiarrayNew(sizeof(s16), rowMax);

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
    }

    printf("\n=== All multiarray fuzz tests completed! ===\n\n");

    TEST("NewFromArray and CopyTo round trip in every tier; edits agree") {
        /* Counts that make Small, Medium and Large at rowMax 8; the bulk
         * build copies out the same elements, keeps its shape accessors,
         * head and tail, and later inserts and deletes leave it equal to
         * an insert-built twin */
        static const size_t counts[] = {0, 5, 8, 9, 40, 63, 64, 65, 500};
        const uint16_t perNode = 8;
        for (size_t c = 0; c < COUNT_ARRAY(counts); c++) {
            const size_t n = counts[c];
            cleanerName(zfree, S16) s16 *want = zcalloc(n + 1, sizeof(s16));
            cleaner(multiarrayFree) multiarray *twin =
                multiarrayNew(sizeof(s16), perNode);
            for (size_t i = 0; i < n; i++) {
                want[i] = (s16){.a = (int64_t)i, .b = -(int64_t)i * 3};
                multiarrayInsert(&twin, (multiarrayIdx)i, &want[i]);
            }

            cleaner(multiarrayFree) multiarray *bulk =
                multiarrayNewFromArray(sizeof(s16), perNode, want, n);
            cleanerName(zfree, S16) s16 *got = zcalloc(n + 1, sizeof(s16));
            multiarrayCopyTo(bulk, got);
            if (multiarrayCount(bulk) != n ||
                memcmp(got, want, n * sizeof(s16)) != 0 ||
                multiarrayElementLen(bulk) != sizeof(s16) ||
                multiarrayRowMax(bulk) != perNode) {
                ERR("count %zu: bulk build differs", n);
                continue;
            }
            if (n &&
                (memcmp(multiarrayGetHead(bulk), &want[0], sizeof(s16)) != 0 ||
                 memcmp(multiarrayGetTail(bulk), &want[n - 1], sizeof(s16)) !=
                     0)) {
                ERR("count %zu: head or tail wrong", n);
            }

            /* The same edits to both */
            uint64_t rng = 0xa77a + n;
            for (int e = 0; e < 60; e++) {
                const size_t count = multiarrayCount(twin);
                const uint64_t at = splitmix64(&rng);
                if (count && at % 3 == 0) {
                    multiarrayDelete(&twin, (uint32_t)(at % count));
                    multiarrayDelete(&bulk, (uint32_t)(at % count));
                } else {
                    s16 v = {.a = e, .b = (int64_t)at};
                    multiarrayInsert(&twin, (multiarrayIdx)(at % (count + 1)),
                                     &v);
                    multiarrayInsert(&bulk, (multiarrayIdx)(at % (count + 1)),
                                     &v);
                }
            }
            const size_t after = multiarrayCount(twin);
            cleanerName(zfree, S16) s16 *a = zcalloc(after + 1, sizeof(s16));
            cleanerName(zfree, S16) s16 *b = zcalloc(after + 1, sizeof(s16));
            multiarrayCopyTo(twin, a);
            multiarrayCopyTo(bulk, b);
            if (multiarrayCount(bulk) != after ||
                memcmp(a, b, after * sizeof(s16)) != 0) {
                ERR("count %zu: edits disagree after a bulk build", n);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif
