#include "multilist.h"

#include "multilistSmall.h"
#include "multilistSmallInternal.h"

#include "multilistMedium.h"
#include "multilistMediumInternal.h"

#include "multilistFull.h"

#include "flexCapacityManagement.h"

#if MULTILIST_INLINE
#include "multilistFull.c"
#include "multilistMedium.c"
#include "multilistSmall.c"
#endif

#define PTRLIB_BITS_LOW 2
#define PTRLIB_BITS_HIGH 16
#include "ptrlib.h"
#include "ptrlibCompress.h"

/* multilistFull has different arguments than Small/Medium, so we need to make
 * fake prototypes for each Small/Medium function to throw away the unused
 * compression state argument. */
#include "multilistAdapter.h"

typedef enum multilistType {
    MULTILIST_TYPE_INVALID = 0,
    MULTILIST_TYPE_SMALL = 1,  /* 8 bytes, fixed. */
    MULTILIST_TYPE_MEDIUM = 2, /* 16 bytes, fixed. */
    MULTILIST_TYPE_FULL = 3,   /* 24 bytes, grows as necessary. */
} multilistType;

#define _multilistType(list) _PTRLIB_TYPE(list)
#define _MULTILIST_USE(list) _PTRLIB_TOP_USE_ALL(list)
#define _MULTILIST_TAG(list, type) ((multilist *)_PTRLIB_TAG(list, type))

#define mls(m) ((multilistSmall *)_MULTILIST_USE(m))
#define mlm(m) ((multilistMedium *)_MULTILIST_USE(m))
#define mlf(m) ((multilistFull *)_MULTILIST_USE(m))

#define _MULTILIST(ret, m, func, ...)                                          \
    do {                                                                       \
        switch (_multilistType(m)) {                                           \
        case MULTILIST_TYPE_SMALL:                                             \
            ret multilistSmallAdapter##func(mls(m), __VA_ARGS__);              \
            break;                                                             \
        case MULTILIST_TYPE_MEDIUM:                                            \
            ret multilistMediumAdapter##func(mlm(m), __VA_ARGS__);             \
            break;                                                             \
        case MULTILIST_TYPE_FULL:                                              \
            ret multilistFull##func(mlf(m), __VA_ARGS__);                      \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTILIST_NORETURN(m, func, ...) _MULTILIST(, m, func, __VA_ARGS__)
#define MULTILIST_RETURN(m, func, ...) _MULTILIST(return, m, func, __VA_ARGS__)

#define _MULTILIST_SINGLE(ret, m, func)                                        \
    do {                                                                       \
        switch (_multilistType(m)) {                                           \
        case MULTILIST_TYPE_SMALL:                                             \
            ret multilistSmall##func(mls(m));                                  \
            break;                                                             \
        case MULTILIST_TYPE_MEDIUM:                                            \
            ret multilistMedium##func(mlm(m));                                 \
            break;                                                             \
        case MULTILIST_TYPE_FULL:                                              \
            ret multilistFull##func(mlf(m));                                   \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTILIST_SINGLE_NORETURN(m, func) _MULTILIST_SINGLE(, m, func)
#define MULTILIST_SINGLE_RETURN(m, func) _MULTILIST_SINGLE(return, m, func)

/* ====================================================================
 * Manage
 * ==================================================================== */
multilist *multilistNew(const flexCapSizeLimit limit, const uint32_t depth) {
    multilist *ml = (multilist *)multilistSmallCreate();
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, depth, limit);
    return _MULTILIST_TAG(ml, MULTILIST_TYPE_SMALL);
}

multilist *multilistNewFromFlex(const flexCapSizeLimit limit,
                                const uint32_t depth, flex *fl) {
    /* Retain 'fl' inside our new multilistSmall.
     * Note: we don't auto-upgrade here if the flex is too big.
     * During the next data write operation, the multilistUpgradeIfNecessary()
     * call will be triggered to become MEDIUM or FULL as required. */
    multilist *ml = (multilist *)multilistSmallNewFromFlexConsume(fl);
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, depth, limit);
    return _MULTILIST_TAG(ml, MULTILIST_TYPE_SMALL);
}

multilist *multilistDuplicate(const multilist *mlOrig) {
    const static size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    const multilistType type = _multilistType(mlOrig);
    goto *dupers[type];

    void *ml;
sm:
    ml = multilistSmallDuplicate(mls(mlOrig));
    goto goodbye;

md:
    ml = multilistMediumDuplicate(mlm(mlOrig));
    goto goodbye;

full:
    ml = multilistFullDuplicate(mlf(mlOrig));
    /* fallthrough */

goodbye:
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, COMPRESS_DEPTH(mlOrig),
                                  COMPRESS_LIMIT(mlOrig));
    return _MULTILIST_TAG(ml, type);
}

void multilistFree(multilist *m) {
    if (m) {
        MULTILIST_SINGLE_NORETURN(m, Free);
    }
}

size_t multilistCount(const multilist *m) {
    MULTILIST_SINGLE_RETURN(m, Count);
}

size_t multilistBytes(const multilist *m) {
    MULTILIST_SINGLE_RETURN(m, Bytes);
}

static void multilistUpgradeIfNecessary(multilist **m, mflexState *state,
                                        const uint_fast32_t depth,
                                        const uint_fast32_t limit) {
    const multilistType type = _multilistType(*m);
    if (type == MULTILIST_TYPE_SMALL) {
        multilistSmall *small = mls(*m);
        /* Statically defined growth cases: fix?  parameterize?  modularize? */
        if (multilistSmallBytes(small) > flexOptimizationSizeLimit[limit]) {
            multilistMedium *medium =
                multilistMediumNewFromFlexConsumeGrow(small, small->fl);
            *m = SET_COMPRESS_DEPTH_LIMIT(medium, depth, limit);
            *m = _MULTILIST_TAG(medium, MULTILIST_TYPE_MEDIUM);
        }
    } else if (type == MULTILIST_TYPE_MEDIUM) {
        multilistMedium *medium = mlm(*m);
        if (multilistMediumBytes(medium) >
            (flexOptimizationSizeLimit[limit] * 3)) {
            flex *f[2] = {medium->fl[0], medium->fl[1]};
            multilistFull *full = multilistFullNewFromFlexConsumeGrow(
                medium, state, f, 2, depth, limit);
            *m = _MULTILIST_TAG(full, MULTILIST_TYPE_FULL);
        }
    } /* else, is FULL and no grow necessary. */
}

/* ====================================================================
 * Insert
 * ==================================================================== */
void multilistPushByTypeHead(multilist **ml, mflexState *state,
                             const databox *box) {
    MULTILIST_NORETURN(*ml, PushByTypeHead, state, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state, depth, limit);
}

void multilistPushByTypeTail(multilist **ml, mflexState *state,
                             const databox *box) {
    MULTILIST_NORETURN(*ml, PushByTypeTail, state, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state, depth, limit);
}

void multilistInsertByTypeAfter(multilist **ml, mflexState *state[2],
                                multilistEntry *node, const databox *box) {
    MULTILIST_NORETURN(*ml, InsertByTypeAfter, state, node, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state[0], depth, limit);
}

void multilistInsertByTypeBefore(multilist **ml, mflexState *state[2],
                                 multilistEntry *node, const databox *box) {
    MULTILIST_NORETURN(*ml, InsertByTypeBefore, state, node, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state[0], depth, limit);
}

/* ====================================================================
 * Remove
 * ==================================================================== */
void multilistDelEntry(multilistIterator *iter, multilistEntry *entry) {
    const static size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    goto *dupers[iter->type];

sm:
    multilistSmallDelEntry(iter, entry);
    return;

md:
    multilistMediumDelEntry(iter, entry);
    return;

full:
    multilistFullDelEntry(iter, entry);
    return;
}

bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values) {
    MULTILIST_RETURN(*m, DelRange, state, start, values);
}

bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlNodeId index, const databox *box) {
    MULTILIST_RETURN(*m, ReplaceByTypeAtIndex, state, index, box);
}

/* ====================================================================
 * Iterate
 * ==================================================================== */
bool multilistIndex(const multilist *ml, mflexState *state,
                    const mlOffsetId index, multilistEntry *entry,
                    const bool openNode) {
    MULTILIST_RETURN(ml, Index, state, index, entry, openNode);
}

void multilistIteratorInit(multilist *ml, mflexState *state[2],
                           multilistIterator *iter, bool forward,
                           bool readOnly) {
    iter->type = _multilistType(ml);
    MULTILIST_NORETURN(ml, IteratorInit, state, iter, forward, readOnly);
}

bool multilistIteratorInitAtIdx(const multilist *ml, mflexState *state[2],
                                multilistIterator *iter, mlOffsetId idx,
                                bool forward, bool readOnly) {
    iter->type = _multilistType(ml);
    MULTILIST_RETURN(ml, IteratorInitAtIdx, state, iter, idx, forward,
                     readOnly);
}

void multilistIteratorRelease(multilistIterator *iter) {
    /* Small and Medium have no iterator state to release... */
    if (iter->type == MULTILIST_TYPE_FULL) {
        multilistFullIteratorRelease(iter);
    }
}

bool multilistNext(multilistIterator *iter, multilistEntry *entry) {
    const static size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    goto *dupers[iter->type];

sm:
    return multilistSmallNext(iter, entry);

md:
    return multilistMediumNext(iter, entry);

full:
    return multilistFullNext(iter, entry);
}

bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail) {
    MULTILIST_RETURN(*m, Pop, state, got, fromTail);
}

void multilistRotate(multilist *m, mflexState *state[2]) {
    MULTILIST_NORETURN(m, Rotate, state);
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST

#include "str.h" /* for StrInt64ToBuf */
#include "timeUtil.h"

#include "ctest.h"

CTEST_INCLUDE_GEN(str)

#if 0
#define DOUBLE_NEWLINE 0
#include "perf.h"

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif
#endif
void multilistRepr(const multilist *m) {
    const multilistType type = _multilistType(m);
    printf("multilist type: %s\n", type == MULTILIST_TYPE_SMALL    ? "SMALL"
                                   : type == MULTILIST_TYPE_MEDIUM ? "MEDIUM"
                                                                   : "FULL");
    MULTILIST_SINGLE_NORETURN(m, Repr);
}

#define multilistPush_(WHAT, a, b, c, d)                                       \
    do {                                                                       \
        databox pushBox_ = databoxNewBytesString(c);                           \
        pushBox_.len = d;                                                      \
        multilistPushByType##WHAT(&a, b, &pushBox_);                           \
    } while (0)

#define multilistPushHead(a, b, c, d) multilistPush_(Head, a, b, c, d)
#define multilistPushTail(a, b, c, d) multilistPush_(Tail, a, b, c, d)

static void multilistInsertBefore(multilist **ml, mflexState *state[2],
                                  multilistEntry *entry, void *data,
                                  size_t len) {
    multilistInsertByTypeBefore(ml, state, entry,
                                &DATABOX_WITH_BYTES(data, len));
}

static void multilistInsertAfter(multilist **ml, mflexState *state[2],
                                 multilistEntry *entry, void *data,
                                 size_t len) {
    multilistInsertByTypeAfter(ml, state, entry,
                               &DATABOX_WITH_BYTES(data, len));
}

bool multilistReplaceAtIndex(multilist **ml, mflexState *state,
                             mlOffsetId index, void *data, size_t len) {
    return multilistReplaceByTypeAtIndex(ml, state, index,
                                         &DATABOX_WITH_BYTES(data, len));
}

#define OK printf("\tOK\n")
int multilistTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    uint32_t err = 0;
    mflexState *s0 = mflexStateCreate();
    mflexState *s1 = mflexStateCreate();
    mflexState *s[2] = {s0, s1};

    const int depth[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const size_t depthCount = sizeof(depth) / sizeof(*depth);
    int64_t runtime[depthCount];
    const int defaultCompressSizeLimit = 1;

    for (size_t _i = 0; _i < depthCount; _i++) {
        printf("Testing Option %d\n", depth[_i]);
        int64_t start = timeUtilMs();

        TEST("create list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistFree(ml);
        }

        TEST("add to tail of empty list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistPushByTypeTail(&ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            multilistFree(ml);
        }

        TEST("add to head of empty list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistPushByTypeHead(&ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            multilistFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 5) {
                    ERROR;
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistPushByTypeHead(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 5) {
                    ERROR;
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 64;
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 500) {
                    ERROR;
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 32);
                }

                if (multilistCount(ml) != 500) {
                    ERROR;
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        TEST("rotate empty") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistRotate(ml, s);
            multilistFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate one val once at fill %zu", f) {
                multilist *ml = multilistNew(f, depth[_i]);
                multilistPushHead(ml, s0, "hello", 6);

                multilistRotate(ml, s);

                /* Ignore compression verify because flex is
                 * too small to compress. */
                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate 504 val 5000 times at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "900", 3);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "7000", 4);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "-1200", 5);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "42", 2);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 64);
                }

                assert(multilistCount(ml) == 504);

                for (int i = 0; i < 5000; i++) {
                    assert(multilistCount(ml) == 504);
                    multilistRotate(ml, s);
                    assert(multilistCount(ml) == 504);
                }

                if (f == 1) {
                } else if (f == 2) {
                } else if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        TEST("pop empty") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            databox box = {{0}};
            bool found = multilistPopHead(&ml, s0, &box);
            assert(!found);
            (void)found;
            multilistFree(ml);
        }

        TEST("pop 1 string from 1") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            char *populate = genstr("hello", 331);
            multilistPushHead(ml, s0, populate, 32);
            uint8_t *data;
            uint32_t bytes;
            databox box = {{0}};
            multilistPopHead(&ml, s0, &box);
            bytes = box.len;
            data = box.data.bytes.start;
            assert(data != NULL);
            assert(bytes == 32);
            if (strcmp(populate, (char *)data)) {
                ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                    bytes, data, populate);
            }

            databoxFreeData(&box);
            multilistFree(ml);
        }

        TEST("pop head 1 number from 1") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, "55513", 5);
            int64_t lv;
            databox box = {{0}};
            multilistPopHead(&ml, s0, &box);
            lv = box.data.i;
            assert(lv == 55513);
            (void)lv;
            databoxFreeData(&box);
            multilistFree(ml);
        }

        TEST("pop head 500 from 500") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            for (int i = 0; i < 500; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistPopHead(&ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                assert(found);
                (void)found;
                assert(data != NULL);
                assert(bytes == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        bytes, data, genstr("hello", 499 - i));
                }
                databoxFreeData(&box);
            }

            multilistFree(ml);
        }

        TEST("pop head 5000 from 500") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            for (int i = 0; i < 5000; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistPopHead(&ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                if (i < 500) {
                    assert(found);
                    assert(data);
                    assert(bytes == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            bytes, data, genstr("hello", 499 - i));
                        assert(NULL);
                    }
                } else {
                    assert(!found);
                }
                (void)found;
                databoxFreeData(&box);
            }
            multilistFree(ml);
        }

        TEST("iterate forward over 500 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistIteratorInitForwardReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 499;
            int values = 0;
            while (multilistNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i--;
                values++;
            }

            if (values != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }

            multilistFree(ml);
        }

        TEST("iterate reverse over 500 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistIteratorInitReverseReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 0;
            while (multilistNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i++;
            }

            if (i != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }

            multilistFree(ml);
        }

        TEST("insert before with 0 elements") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            printf("WHAT? %p, %p, %d\n", entry.ml, entry.fe, entry.offset);
            multilistInsertBefore(&ml, s, &entry, "abc", 4);
            multilistFree(ml);
        }

        TEST("insert after with 0 elements") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
            multilistFree(ml);
        }

        TEST("insert after 1 element") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
            multilistFree(ml);
        }

        TEST("insert before 1 element") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
            multilistFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert once in elements while iterating at fill %zu at "
                      "compress %d\n",
                      f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                multilistPushTail(ml, s0, "abc", 3);
                multilistPushTail(ml, s0, "def", 3); /* unique node */

                multilistPushTail(ml, s0, "bob", 3); /* reset for +3 */
                multilistPushTail(ml, s0, "foo", 3);
                multilistPushTail(ml, s0, "zoo", 3);

                /* insert "bar" before "bob" while iterating over list. */
                multilistIterator iter = {0};
                multilistIteratorInitForwardReadOnly(ml, s, &iter);
                multilistEntry entry;
                while (multilistNext(&iter, &entry)) {
                    if (!strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        multilistInsertBefore(&ml, s, &entry, "bar", 3);
                        /* note: we DO NOT support insert while iterating,
                         * meaning if you insert during an iteration, you
                         * must immediately exit the iteration.
                         *
                         * if you need more generic insert-while-iterating
                         * behavior, create a series of
                         * IteratorInsert{Before,After}Entry, etc. */
                        break;
                    }
                }

                multilistRepr(ml);

                /* verify results */
                bool got = multilistIndexGet(ml, s0, 0, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "abc", 3)) {
                    ERR("Value 0 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 1, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "def", 3)) {
                    ERR("Value 1 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                    assert(NULL);
                }

                got = multilistIndexGet(ml, s0, 2, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "bar", 3)) {
                    ERR("Value 2 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 3, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                    ERR("Value 3 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 4, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "foo", 3)) {
                    ERR("Value 4 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 5, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "zoo", 3)) {
                    ERR("Value 5 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }
                (void)got;

                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %zu at compress %d",
                f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistIndexGet(ml, s0, 250, &entry);
                    multilistInsertBefore(&ml, s, &entry, genstr("abc", i), 32);
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %zu at compress %d",
                      f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistIndexGet(ml, s0, 250, &entry);
                    multilistInsertAfter(&ml, s, &entry, genstr("abc", i), 32);
                }

                if (multilistCount(ml) != 750) {
                    ERR("List size not 750, but rather %zu",
                        multilistCount(ml));
                }

                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        TEST("duplicate empty list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilist *copy = multilistDuplicate(ml);
            multilistFree(ml);
            multilistFree(copy);
        }

        TEST("duplicate list of 1 element") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, genstr("hello", 3), 32);
            multilist *copy = multilistDuplicate(ml);
            multilistFree(ml);
            multilistFree(copy);
        }

        TEST("duplicate list of 500") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilist *copy = multilistDuplicate(ml);
            multilistFree(ml);
            multilistFree(copy);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello2")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
                multilistIndexGet(ml, s0, 200, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello201")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFree(ml);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, -1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello500")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistIndexGet(ml, s0, -2, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello499")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFree(ml);
            }

            TEST_DESC("index -100 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, -100, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello401")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFree(ml);
            }

            TEST_DESC(
                "index too big +1 from 50 list at fill %zu at compress %d", f,
                depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                for (int i = 0; i < 50; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                if (multilistIndexCheck(ml, s0, 50, &entry)) {
                    ERR("Index found at 50 with 50 list: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                } else {
                    OK;
                }

                multilistFree(ml);
            }
        }

        TEST("delete range empty list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistDelRange(&ml, s0, 5, 20);
            multilistFree(ml);
        }

        TEST("delete range of entire node in list of one node") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistDelRange(&ml, s0, 0, 32);
            multilistFree(ml);
        }

        TEST("delete range of entire node with overflow valuess") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistDelRange(&ml, s0, 0, 128);
            multilistFree(ml);
        }

        TEST("delete middle 100 of 500 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, 200, 100);
            multilistFree(ml);
        }

        TEST("delete negative 1 from 500 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -1, 1);
            multilistFree(ml);
        }

        TEST("delete negative 1 from 500 list with overflow valuess") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -1, 128);
            multilistFree(ml);
        }

        TEST("delete negative 100 from 500 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -100, 100);
            multilistFree(ml);
        }

        TEST("delete -10 values 5 from 50 list") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 50; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -10, 5);
            multilistFree(ml);
        }

        TEST("numbers only list read") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushTail(ml, s0, "1111", 4);
            multilistPushTail(ml, s0, "2222", 4);
            multilistPushTail(ml, s0, "3333", 4);
            multilistPushTail(ml, s0, "4444", 4);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 1, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 3, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444, %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistIndexGet(ml, s0, 4, &entry)) {
                ERR("Index past elements: %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -1, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -3, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -4, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistIndexGet(ml, s0, -5, &entry)) {
                ERR("Index past elements (reverse), %" PRIi64 "",
                    entry.box.data.i64);
            }

            multilistFree(ml);
        }

        TEST("numbers larger list read") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            int64_t nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                const databox pushBox = {.data.i64 = nums[i],
                                         .type = DATABOX_SIGNED_64};
                multilistPushByTypeTail(&ml, s0, &pushBox);
            }

            multilistPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistEntry entry;
            for (int i = 0; i < 5000; i++) {
                multilistIndexGet(ml, s0, i, &entry);
                if (entry.box.data.i64 != nums[i]) {
                    ERR("[%d] Not longval %" PRIi64 " but rather %" PRIi64 "",
                        i, nums[i], entry.box.data.i64);
                }

                entry.box.data.i64 = 0xdeadbeef;
            }

            multilistIndexGet(ml, s0, 5000, &entry);
            if (strncmp(entry.box.data.bytes.cstart, "xxxxxxxxxxxxxxxxxxxx",
                        20)) {
                ERR("String val not match: %s", entry.box.data.bytes.start);
            }

            multilistFree(ml);
        }

        TEST("numbers larger list read B") {
            multilist *ml = multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushTail(ml, s0, "99", 2);
            multilistPushTail(ml, s0, "98", 2);
            multilistPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistPushTail(ml, s0, "96", 2);
            multilistPushTail(ml, s0, "95", 2);
            multilistReplaceAtIndex(&ml, s0, 1, "foo", 3);
            multilistReplaceAtIndex(&ml, s0, -1, "bar", 3);
            multilistFree(ml);
            OK;
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("lrem test at fill %zu at compress %d", f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++) {
                    multilistPushTail(ml, s0, words[i], strlen(words[i]));
                }

                multilistRepr(ml);

                /* lrem 0 bar */
                multilistIterator iter = {0};
                multilistIteratorInitForward(ml, s, &iter);
                multilistEntry entry;
                int i = 0;
                while (multilistNext(&iter, &entry)) {
                    if (flexCompareBytes(entry.fe, "bar", 3)) {
                        multilistDelEntry(&iter, &entry);
                    }

                    i++;
                }

                /* check result of lrem 0 bar */
                multilistIteratorInitForwardReadOnly(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                int ok = 1;
                while (multilistNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart, result[i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, result[i]);
                        ok = 0;
                    }

                    i++;
                }

                multilistPushTail(ml, s0, "foo", 3);

                /* lrem -2 foo */
                multilistIteratorInitReverse(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                int del = 2;
                while (multilistNext(&iter, &entry)) {
                    if (flexCompareBytes(entry.fe, "foo", 3)) {
                        multilistDelEntry(&iter, &entry);
                        del--;
                    }

                    if (!del) {
                        break;
                    }

                    i++;
                }

                multilistIteratorRelease(&iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because we only have two foo) */
                multilistIteratorInitReverseReadOnly(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                const size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (multilistNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart,
                                resultB[resB - 1 - i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, resultB[resB - 1 - i]);
                        ok = 0;
                    }

                    i++;
                }

                /* final result of all tests */
                if (ok) {
                    OK;
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterate reverse + delete at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                multilistPushTail(ml, s0, "abc", 3);
                multilistPushTail(ml, s0, "def", 3);
                multilistPushTail(ml, s0, "hij", 3);
                multilistPushTail(ml, s0, "jkl", 3);
                multilistPushTail(ml, s0, "oop", 3);

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistIteratorInitReverse(ml, s, &iter);
                int i = 0;
                while (multilistNext(&iter, &entry)) {
                    printf("Entry fe: %p (%.*s)\n", entry.fe, 3, entry.fe + 1);
                    if (flexCompareBytes(entry.fe, "hij", 3)) {
                        multilistDelEntry(&iter, &entry);
                    }

                    i++;
                }

                if (i != 5) {
                    ERR("Didn't iterate 5 times, iterated %d times.", i);
                    multilistRepr(ml);
                }

                /* Check results after deletion of "hij" */
                multilistIteratorInitForward(ml, s, &iter);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (multilistNext(&iter, &entry)) {
                    if (!flexCompareBytes(entry.fe, vals[i], 3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }

                    i++;
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterator at index test at fill %zu at compress %d", f,
                      depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = {.data.i64 = nums[i],
                                             .type = DATABOX_SIGNED_64};
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistIteratorInitAtIdxForwardReadOnly(ml, s, &iter, 437);
                int i = 437;
                while (multilistNext(&iter, &entry)) {
                    if (entry.box.data.i64 != nums[i]) {
                        ERR("Expected %" PRIi64 ", but got %" PRIi64 "",
                            entry.box.data.i64, nums[i]);
                    }

                    i++;
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test A at fill %zu at compress %d", f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (f == 32) {
                }

                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                multilistDelRange(&ml, s0, 0, 25);
                multilistDelRange(&ml, s0, 0, 0);
                multilistEntry entry;
                for (int i = 0; i < 7; i++) {
                    multilistIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[25 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[25 + i]);
                    }
                }
                if (f == 32) {
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test B at fill %zu at compress %d", f, depth[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                multilist *ml = multilistNew(f, 0);
                char num[32];
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    multilistPushTail(ml, s0, num, bytes);
                }

                if (f == 32) {
                }

                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                multilistDelRange(&ml, s0, 0, 5);
                multilistDelRange(&ml, s0, -16, 16);
                if (f == 32) {
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != 5) {
                    ERR("A: longval not 5, but %" PRIi64 "",
                        entry.box.data.i64);
                } else {
                    OK;
                }
                multilistIndexGet(ml, s0, -1, &entry);
                if (entry.box.data.i64 != 16) {
                    ERR("B! got instead: %" PRIi64 "", entry.box.data.i64);
                } else {
                    OK;
                }
                multilistPushTail(ml, s0, "bobobob", 7);
                multilistIndexGet(ml, s0, -1, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "bobobob", 7)) {
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                for (int i = 0; i < 12; i++) {
                    multilistIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[5 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[5 + i]);
                    }
                }
                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test C at fill %zu at compress %d", f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (f == 32) {
                }

                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                multilistDelRange(&ml, s0, 0, 3);
                multilistDelRange(&ml, s0, -29,
                                  4000); /* make sure not loop forever */
                if (f == 32) {
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != -5157318210846258173) {
                    ERROR;
                } else {
                    OK;
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test D at fill %zu at compress %d", f, depth[_i]) {
                multilist *ml = multilistNew(f, depth[_i]);
                char num[32];
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    multilistPushTail(ml, s0, num, bytes);
                }

                if (f == 32) {
                }

                multilistDelRange(&ml, s0, -12, 3);
                if (multilistCount(ml) != 30) {
                    ERR("Didn't delete exactly three elements!  values is: %zu",
                        multilistCount(ml));
                }

                multilistFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("create multilist from flex at fill %zu at compress %d",
                      f, depth[_i]) {
                flex *fl = flexNew();
                int64_t nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    flexPushBytes(&fl, num, bytes, FLEX_ENDPOINT_TAIL);
                }

                for (int i = 0; i < 33; i++) {
                    flexPushBytes(&fl, genstr("hello", i), 32,
                                  FLEX_ENDPOINT_TAIL);
                }

                multilist *ml = multilistNewFromFlex(f, depth[_i], fl);

                if (f == 1) {
                } else if (f == 32) {
                } else if (f == 66) {
                }

                multilistFree(ml);
            }
        }

        int64_t stop = timeUtilMs();
        runtime[_i] = stop - start;
    }

    mflexStateReset(s0);
    mflexStateReset(s1);

    /* Run a longer test of compression depth outside of primary test loop. */
    int listSizes[] = {30, 40, 50, 100, 250, 251, 500, 999, 1000, 5000, 10000};
    int64_t start = timeUtilMs();
    for (size_t list = 0; list < (sizeof(listSizes) / sizeof(*listSizes));
         list++) {
        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            for (size_t _depth = 1; _depth < 40; _depth++) {
                /* skip over many redundant test cases */
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %zu at compress depth %zu",
                          listSizes[list], f, _depth) {
                    multilist *ml = multilistNew(f, _depth);
                    for (int i = 0; i < listSizes[list]; i++) {
                        multilistPushTail(ml, s0, genstr("hello TAIL", i + 1),
                                          64);
                        multilistPushHead(ml, s0, genstr("hello HEAD", i + 1),
                                          64);
                    }

                    multilistFree(ml);
                }
            }
        }
    }
    int64_t stop = timeUtilMs();

    printf("\n");
    for (size_t i = 0; i < depthCount; i++) {
        fprintf(stderr, "Compress Depth %02d: %0.3f seconds.\n", depth[i],
                (float)runtime[i] / 1000);
    }

    fprintf(stderr, "Final Stress Loop: %0.2f seconds.\n",
            (float)(stop - start) / 1000);
    printf("\n");

    /* ================================================================
     * COMPREHENSIVE FUZZ TESTS
     * ================================================================ */

    printf("\n=== MULTILIST FUZZ TESTING ===\n\n");

    TEST("FUZZ: push/pop operations with oracle verification") {
        multilist *ml = multilistNew(4, 2);
        const size_t maxSize = 1000;
        int64_t *oracle = zcalloc(maxSize, sizeof(int64_t));
        size_t oracleCount = 0;

        srand(12345);
        size_t pushOps = 0, popOps = 0;

        for (size_t round = 0; round < 5000; round++) {
            int op = rand() % 10;

            if (op < 6 && oracleCount < maxSize) {
                /* Push (60%) */
                int64_t val = (int64_t)(rand() % 100000) - 50000;
                int where = rand() % 2;
                databox box = databoxNewSigned(val);

                if (where == 0) {
                    /* Push head */
                    multilistPushByTypeHead(&ml, s0, &box);
                    memmove(&oracle[1], &oracle[0],
                            oracleCount * sizeof(int64_t));
                    oracle[0] = val;
                } else {
                    /* Push tail */
                    multilistPushByTypeTail(&ml, s0, &box);
                    oracle[oracleCount] = val;
                }
                oracleCount++;
                pushOps++;
            } else if (oracleCount > 0) {
                /* Pop (40%) */
                int where = rand() % 2;
                databox got;

                if (where == 0) {
                    /* Pop head */
                    multilistPopHead(&ml, s0, &got);
                    int64_t expected = oracle[0];
                    int64_t actual = (got.type == DATABOX_SIGNED_64)
                                         ? got.data.i
                                         : (int64_t)got.data.u;
                    if (actual != expected) {
                        ERR("head mismatch: got %" PRId64 " expected %" PRId64,
                            actual, expected);
                    }
                    memmove(&oracle[0], &oracle[1],
                            (oracleCount - 1) * sizeof(int64_t));
                } else {
                    /* Pop tail */
                    multilistPopTail(&ml, s0, &got);
                    int64_t expected = oracle[oracleCount - 1];
                    int64_t actual = (got.type == DATABOX_SIGNED_64)
                                         ? got.data.i
                                         : (int64_t)got.data.u;
                    if (actual != expected) {
                        ERR("tail mismatch: got %" PRId64 " expected %" PRId64,
                            actual, expected);
                    }
                }
                oracleCount--;
                popOps++;
            }

            /* Periodic verification */
            if (round % 500 == 0) {
                if (multilistCount(ml) != oracleCount) {
                    ERR("count mismatch at round %zu: ml=%zu oracle=%zu", round,
                        multilistCount(ml), oracleCount);
                }
            }
        }

        /* Final verification */
        if (multilistCount(ml) != oracleCount) {
            ERR("final count mismatch: ml=%zu oracle=%zu", multilistCount(ml),
                oracleCount);
        }

        printf("  push=%zu pop=%zu final=%zu\n", pushOps, popOps, oracleCount);
        zfree(oracle);
        multilistFree(ml);
    }

    TEST("FUZZ: index access verification") {
        multilist *ml = multilistNew(4, 2);
        const size_t count = 500;

        /* Populate list */
        for (size_t i = 0; i < count; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        /* Verify positive index access */
        for (size_t i = 0; i < count; i++) {
            multilistEntry entry;
            if (!multilistIndexGet(ml, s0, i, &entry)) {
                ERR("index %zu failed", i);
            }
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if ((size_t)val != i) {
                ERR("index %zu returned %" PRId64, i, val);
            }
        }

        /* Verify negative index access */
        for (size_t i = 0; i < count; i++) {
            multilistEntry entry;
            int64_t negIdx = -(int64_t)(i + 1);
            if (!multilistIndexGet(ml, s0, negIdx, &entry)) {
                ERR("negative index %" PRId64 " failed", negIdx);
            }
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            int64_t expected = (int64_t)(count - 1 - i);
            if (val != expected) {
                ERR("negative index %" PRId64 " returned %" PRId64
                    ", expected %" PRId64,
                    negIdx, val, expected);
            }
        }

        printf("  verified %zu positive and %zu negative indices\n", count,
               count);
        multilistFree(ml);
    }

    TEST("FUZZ: iterator forward/backward consistency") {
        multilist *ml = multilistNew(4, 2);
        const size_t count = 200;

        /* Populate list */
        for (size_t i = 0; i < count; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        /* Forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t idx = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if ((size_t)val != idx) {
                ERR("forward iter at %zu: got %" PRId64, idx, val);
            }
            idx++;
        }
        multilistIteratorRelease(&iter);

        if (idx != count) {
            ERR("forward iteration count: got %zu expected %zu", idx, count);
        }

        /* Backward iteration */
        multilistIteratorInitReverseReadOnly(ml, s, &iter);
        idx = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            int64_t expected = (int64_t)(count - 1 - idx);
            if (val != expected) {
                ERR("backward iter at %zu: got %" PRId64 " expected %" PRId64,
                    idx, val, expected);
            }
            idx++;
        }
        multilistIteratorRelease(&iter);

        if (idx != count) {
            ERR("backward iteration count: got %zu expected %zu", idx, count);
        }

        printf("  verified forward and backward iteration of %zu elements\n",
               count);
        multilistFree(ml);
    }

    TEST("FUZZ: mixed types in list") {
        multilist *ml = multilistNew(4, 2);
        const size_t count = 300;

        srand(77777);
        for (size_t i = 0; i < count; i++) {
            int type = rand() % 4;
            databox box;
            char buf[64]; /* Declare outside switch to keep in scope */
            switch (type) {
            case 0:
                box = databoxNewSigned((int64_t)(rand() % 100000) - 50000);
                break;
            case 1:
                box = databoxNewUnsigned(rand() % 100000);
                break;
            case 2:
                snprintf(buf, sizeof(buf), "str_%d", rand());
                box = databoxNewBytesString(buf);
                break;
            case 3:
                box.type = DATABOX_DOUBLE_64;
                box.data.d64 = (double)(rand() % 10000) / 100.0;
                break;
            }
            multilistPushByTypeTail(&ml, s0, &box);
        }

        if (multilistCount(ml) != count) {
            ERR("mixed type count: got %zu expected %zu", multilistCount(ml),
                count);
        }

        /* Verify we can iterate all elements */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != count) {
            ERR("mixed type iter count: got %zu expected %zu", iterCount,
                count);
        }

        printf("  verified %zu mixed-type elements\n", count);
        multilistFree(ml);
    }

    TEST("FUZZ: tier transitions - small to medium to full") {
        /* Test that lists correctly transition between tiers */
        for (size_t fill = 1; fill <= 8; fill++) {
            multilist *ml = multilistNew(fill, 0);

            /* Keep pushing until we force tier transitions */
            const size_t targetCount = 1000;
            for (size_t i = 0; i < targetCount; i++) {
                databox box = databoxNewSigned((int64_t)i);
                multilistPushByTypeTail(&ml, s0, &box);

                /* Periodically verify count */
                if ((i + 1) % 100 == 0) {
                    if (multilistCount(ml) != i + 1) {
                        ERR("fill %zu at i=%zu: count=%zu expected=%zu", fill,
                            i, multilistCount(ml), i + 1);
                    }
                }
            }

            /* Verify final state */
            if (multilistCount(ml) != targetCount) {
                ERR("fill %zu final: count=%zu expected=%zu", fill,
                    multilistCount(ml), targetCount);
            }

            /* Pop half and verify */
            for (size_t i = 0; i < targetCount / 2; i++) {
                databox got;
                if (!multilistPopTail(&ml, s0, &got)) {
                    ERR("fill %zu: pop failed at %zu", fill, i);
                }
            }

            if (multilistCount(ml) != targetCount / 2) {
                ERR("fill %zu after pops: count=%zu expected=%zu", fill,
                    multilistCount(ml), targetCount / 2);
            }

            multilistFree(ml);
        }
        printf("  verified tier transitions across fill levels 1-8\n");
    }

    TEST("MEDIUM: unbalanced head-only push through tier") {
        /* Push only to head - F0 grows large, F1 stays empty.
         * This tests the unbalanced scenario where rebalancing TODO
         * is not yet implemented (multilistMedium.c lines 143, 156, 479).
         * Use limit=6 (2048 bytes) so Medium→Full is at 6144 bytes,
         * giving room to stay in Medium with ~100 small integers. */
        multilist *ml = multilistNew(6, 0);

        /* Push enough to get into Medium tier but not Full tier */
        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeHead(&ml, s0, &box);
        }

        /* Verify count is correct */
        if (multilistCount(ml) != targetCount) {
            ERR("head-only count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify index access works across the unbalanced structure */
        for (size_t i = 0; i < targetCount; i++) {
            multilistEntry entry;
            if (!multilistIndex(ml, s0, (int64_t)i, &entry, false)) {
                ERR("head-only index failed at %zu", i);
            }
            /* Head push inverts order: index 0 = value (targetCount-1) */
            int64_t expected = (int64_t)(targetCount - 1 - i);
            int64_t got;
            if (DATABOX_IS_SIGNED_INTEGER(&entry.box)) {
                got = entry.box.data.i64;
            } else if (DATABOX_IS_UNSIGNED_INTEGER(&entry.box)) {
                got = (int64_t)entry.box.data.u64;
            } else {
                got = -9999;
            }
            if (got != expected) {
                ERR("head-only index %zu: got %" PRId64 " expected %" PRId64, i,
                    got, expected);
            }
        }

        /* Verify forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        int64_t expectedVal = (int64_t)(targetCount - 1);
        while (multilistNext(&iter, &entry)) {
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expectedVal) {
                ERR("head-only iter at %zu: got %" PRId64 " expected %" PRId64,
                    iterCount, got, expectedVal);
            }
            expectedVal--;
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("head-only iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        /* Verify backward iteration - reverse iter still uses multilistNext */
        multilistIteratorInitReverseReadOnly(ml, s, &iter);
        iterCount = 0;
        expectedVal = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expectedVal) {
                ERR("head-only reverse iter at %zu: got %" PRId64
                    " expected %" PRId64,
                    iterCount, got, expectedVal);
            }
            expectedVal++;
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("head-only reverse iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified unbalanced head-only push (%zu elements)\n",
               targetCount);
        multilistFree(ml);
    }

    TEST("MEDIUM: unbalanced tail-only push through tier") {
        /* Push only to tail - F1 grows large, F0 starts minimal.
         * After NewFromFlex, F0 has half the initial flex, F1 has other half,
         * then all new pushes go to F1 only.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        if (multilistCount(ml) != targetCount) {
            ERR("tail-only count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify index access - tail push maintains order */
        for (size_t i = 0; i < targetCount; i++) {
            multilistEntry entry;
            if (!multilistIndex(ml, s0, (int64_t)i, &entry, false)) {
                ERR("tail-only index failed at %zu", i);
            }
            int64_t expected = (int64_t)i;
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("tail-only index %zu: got %" PRId64 " expected %" PRId64, i,
                    got, expected);
            }
        }

        /* Verify forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t expected = (int64_t)iterCount;
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("tail-only iter at %zu: got %" PRId64 " expected %" PRId64,
                    iterCount, got, expected);
            }
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("tail-only iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified unbalanced tail-only push (%zu elements)\n",
               targetCount);
        multilistFree(ml);
    }

    TEST("MEDIUM: alternating head/tail maintains balance") {
        /* Alternating pushes should keep F0 and F1 roughly balanced.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            if (i % 2 == 0) {
                multilistPushByTypeHead(&ml, s0, &box);
            } else {
                multilistPushByTypeTail(&ml, s0, &box);
            }
        }

        if (multilistCount(ml) != targetCount) {
            ERR("alternating count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify we can iterate all elements */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("alternating iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified alternating head/tail push (%zu elements)\n",
               targetCount);
        multilistFree(ml);
    }

    TEST("MEDIUM: unbalanced delete operations") {
        /* Build a balanced list, then delete from one end only.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        multilist *ml = multilistNew(6, 0);

        const size_t buildCount = 80;
        for (size_t i = 0; i < buildCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            if (i % 2 == 0) {
                multilistPushByTypeHead(&ml, s0, &box);
            } else {
                multilistPushByTypeTail(&ml, s0, &box);
            }
        }

        /* Pop only from head until half remain */
        size_t remaining = buildCount;
        while (remaining > buildCount / 2) {
            databox got;
            if (!multilistPopHead(&ml, s0, &got)) {
                ERR("unbalanced delete pop failed at remaining=%zu", remaining);
            }
            remaining--;
        }

        if (multilistCount(ml) != remaining) {
            ERR("unbalanced delete count: got %zu expected %zu",
                multilistCount(ml), remaining);
        }

        /* Verify iteration still works after unbalanced deletions */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != remaining) {
            ERR("unbalanced delete iter: got %zu expected %zu", iterCount,
                remaining);
        }

        printf("  verified unbalanced delete operations (%zu remaining)\n",
               remaining);
        multilistFree(ml);
    }

    TEST("MEDIUM: negative index access with unbalanced structure") {
        /* Test negative indexing when F0/F1 are unbalanced.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        /* Push all to head - creates unbalanced F0 */
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeHead(&ml, s0, &box);
        }

        /* Test negative indices */
        for (size_t i = 1; i <= targetCount; i++) {
            multilistEntry entry;
            int64_t negIdx = -(int64_t)i;
            if (!multilistIndex(ml, s0, negIdx, &entry, false)) {
                ERR("negative index failed at %" PRId64, negIdx);
            }
            /* -1 should be value 0, -2 should be value 1, etc. */
            int64_t expected = (int64_t)(i - 1);
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("negative index %" PRId64 ": got %" PRId64
                    " expected %" PRId64,
                    negIdx, got, expected);
            }
        }

        printf("  verified negative index access with unbalanced structure\n");
        multilistFree(ml);
    }

    TEST("FUZZ: stress random operations") {
        multilist *ml = multilistNew(4, 2);
        size_t count = 0;

        srand(99999);
        for (size_t round = 0; round < 10000; round++) {
            int op = rand() % 10;

            if (op < 4) {
                /* Push head (40%) */
                databox box = databoxNewSigned((int64_t)rand());
                multilistPushByTypeHead(&ml, s0, &box);
                count++;
            } else if (op < 8) {
                /* Push tail (40%) */
                databox box = databoxNewSigned((int64_t)rand());
                multilistPushByTypeTail(&ml, s0, &box);
                count++;
            } else if (count > 0) {
                /* Pop (20%) */
                databox got;
                if (rand() % 2) {
                    multilistPopHead(&ml, s0, &got);
                } else {
                    multilistPopTail(&ml, s0, &got);
                }
                count--;
            }

            /* Periodic verification */
            if (round % 1000 == 0) {
                if (multilistCount(ml) != count) {
                    ERR("round %zu: ml=%zu oracle=%zu", round,
                        multilistCount(ml), count);
                }
            }
        }

        if (multilistCount(ml) != count) {
            ERR("final: ml=%zu oracle=%zu", multilistCount(ml), count);
        }

        printf("  completed 10K random operations, final count=%zu\n", count);
        multilistFree(ml);
    }

    printf("\n=== All multilist fuzz tests completed! ===\n\n");

    mflexStateFree(s0);
    mflexStateFree(s1);

    if (!err) {
        printf("ALL TESTS PASSED!\n");
    } else {
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);
    }

    return err;
}
#endif /* DATAKIT_TEST */
