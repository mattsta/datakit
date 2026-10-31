/* Differential tests for every typed segment tree, under every operation
 * (SUM, MIN, MAX), against a naive array.
 *
 * Contract checked (segmentCore.h):
 *   - Update assigns one element; an index past the end extends the tree,
 *     and the elements between hold the operation's identity (0 for SUM,
 *     the type's max for MIN, its min for MAX).
 *   - RangeUpdate assigns every existing element in [left, right] and
 *     never extends the tree.
 *   - Query folds [left, right] with the operation; an integer SUM that
 *     does not fit the type comes back modulo 2^bits (never UB).  Query
 *     and Get outside the tree return the identity.
 *   - The tier change at the Small tier's limit keeps every element. */

#include "../cleaner.h"
#include "../ctest.h"
#include "../datakit.h"
#include "../flex.h"
#include "../persist.h"
#include "segmentDouble.h"
#include "segmentFloat.h"
#include "segmentI16.h"
#include "segmentI32.h"
#include "segmentI64.h"
#include "segmentPersist.h"
#include "segmentU16.h"
#include "segmentU32.h"
#include "segmentU64.h"
#ifdef __SIZEOF_INT128__
#include "segmentI128.h"
#include "segmentU128.h"
#endif

#include <float.h>
#include <inttypes.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);
defcleaner(flex *, flexFree);

/* Mostly small indexes; sometimes a jump past the end or across the
 * Small tier's limit */
static size_t sgtIndex_(uint64_t *s) {
    const uint64_t r = dkSplitmix64Next(s) % 128;
    if (r == 0) {
        return 20000 + (size_t)(dkSplitmix64Next(s) % 64);
    }

    if (r < 4) {
        return 1000 + (size_t)(dkSplitmix64Next(s) % 2000);
    }

    return (size_t)(dkSplitmix64Next(s) % 300);
}

static constexpr int SGT_ELEMS = 21000;
static constexpr int SGT_OPS = 1500;

/* The fold the oracle expects: SUM in the unsigned type of the same
 * width (the sum modulo 2^bits), MIN and MAX directly. */
#define SGT_DIFF(T, VT, UT, VMIN, VMAX, IS_FLOAT)                              \
    static int sgtDiff##T(segmentOp op, uint64_t seed) {                       \
        int err = 0;                                                           \
        const VT identity = op == SEGMENT_OP_SUM   ? (VT)0                     \
                            : op == SEGMENT_OP_MIN ? (VT)(VMAX)                \
                                                   : (VT)(VMIN);               \
        cleaner(zfree) void *elemMem = zmalloc(SGT_ELEMS * sizeof(VT));        \
        VT *elem = elemMem;                                                    \
        for (size_t i = 0; i < SGT_ELEMS; i++) {                               \
            elem[i] = identity;                                                \
        }                                                                      \
        size_t count = 0;                                                      \
        void *seg = segment##T##New(op);                                       \
        for (size_t step = 0; step < SGT_OPS && !err; step++) {                \
            const size_t idx = sgtIndex_(&seed);                               \
            VT v;                                                              \
            const uint64_t pick = dkSplitmix64Next(&seed) % 8;                 \
            if (IS_FLOAT || pick < 5) {                                        \
                v = (VT)((int64_t)(dkSplitmix64Next(&seed) % 201) - 100);      \
                if ((VT) - 1 > 0) {                                            \
                    v = (VT)(dkSplitmix64Next(&seed) % 101);                   \
                }                                                              \
            } else if (pick == 5) {                                            \
                v = (VT)(VMAX);                                                \
            } else if (pick == 6) {                                            \
                v = (VT)(VMIN);                                                \
            } else {                                                           \
                v = (VT)dkSplitmix64Next(&seed);                               \
            }                                                                  \
                                                                               \
            if (dkSplitmix64Next(&seed) % 8 == 0 && count > 0) {               \
                const size_t l = (size_t)(dkSplitmix64Next(&seed) % count);    \
                const size_t r = l + (size_t)(dkSplitmix64Next(&seed) % 40);   \
                segment##T##RangeUpdate(seg, l, r, v);                         \
                for (size_t i = l; i <= r && i < count; i++) {                 \
                    elem[i] = v;                                               \
                }                                                              \
            } else {                                                           \
                segment##T##Update(&seg, idx, v);                              \
                elem[idx] = v;                                                 \
                count = idx + 1 > count ? idx + 1 : count;                     \
            }                                                                  \
                                                                               \
            if (segment##T##Count(seg) != count) {                             \
                ERR(#T " op %d step %zu: count %zu, want %zu", (int)op, step,  \
                    segment##T##Count(seg), count);                            \
            }                                                                  \
                                                                               \
            /* Every element copied out (range assignments still pending       \
             * in Full included), and a tree rebuilt from them answering       \
             * as this one does */                                             \
            if (step % 128 == 127 && count) {                                  \
                cleaner(zfree) void *copyMem = zmalloc(count * sizeof(VT));    \
                VT *copy = copyMem;                                            \
                segment##T##CopyTo(seg, copy);                                 \
                for (size_t i = 0; i < count && !err; i++) {                   \
                    if (copy[i] != elem[i]) {                                  \
                        ERR(#T " op %d step %zu: CopyTo[%zu] wrong", (int)op,  \
                            step, i);                                          \
                    }                                                          \
                }                                                              \
                void *rebuilt = segment##T##NewFromArray(op, copy, count);     \
                for (int k = 0; k < 8 && !err; k++) {                          \
                    const size_t r =                                           \
                        (size_t)(dkSplitmix64Next(&seed) % count);             \
                    const size_t l =                                           \
                        (size_t)(dkSplitmix64Next(&seed) % (r + 1));           \
                    if (segment##T##Query(rebuilt, l, r) !=                    \
                        segment##T##Query(seg, l, r)) {                        \
                        ERR(#T " op %d step %zu: the rebuilt tree's "          \
                               "Query(%zu, %zu) differs",                      \
                            (int)op, step, l, r);                              \
                    }                                                          \
                }                                                              \
                segment##T##Free(rebuilt);                                     \
            }                                                                  \
                                                                               \
            for (int k = 0; k < 3 && !err; k++) {                              \
                const size_t span = count < 1024 ? count : 1024;               \
                const size_t r =                                               \
                    k == 0 && step % 256 == 255                                \
                        ? count - 1                                            \
                        : (size_t)(dkSplitmix64Next(&seed) % span);            \
                const size_t l = (size_t)(dkSplitmix64Next(&seed) % (r + 1));  \
                VT want = identity;                                            \
                UT sum = 0;                                                    \
                for (size_t i = l; i <= r; i++) {                              \
                    if (op == SEGMENT_OP_SUM) {                                \
                        sum += (UT)elem[i];                                    \
                    } else if (op == SEGMENT_OP_MIN) {                         \
                        want = elem[i] < want ? elem[i] : want;                \
                    } else {                                                   \
                        want = elem[i] > want ? elem[i] : want;                \
                    }                                                          \
                }                                                              \
                if (op == SEGMENT_OP_SUM) {                                    \
                    want = IS_FLOAT ? want : (VT)sum;                          \
                    if (IS_FLOAT) {                                            \
                        for (size_t i = l; i <= r; i++) {                      \
                            want += elem[i];                                   \
                        }                                                      \
                    }                                                          \
                }                                                              \
                if (segment##T##Query(seg, l, r) != want) {                    \
                    ERR(#T " op %d step %zu: Query(%zu, %zu) wrong", (int)op,  \
                        step, l, r);                                           \
                }                                                              \
                if (segment##T##Get(seg, r) != elem[r]) {                      \
                    ERR(#T " op %d step %zu: Get(%zu) wrong", (int)op, step,   \
                        r);                                                    \
                }                                                              \
            }                                                                  \
        }                                                                      \
                                                                               \
        for (size_t i = 0; i < count && !err; i++) {                           \
            if (segment##T##Get(seg, i) != elem[i]) {                          \
                ERR(#T " op %d sweep: Get(%zu) wrong", (int)op, i);            \
            }                                                                  \
        }                                                                      \
        if (segment##T##Get(seg, count) != identity ||                         \
            segment##T##Query(seg, 0, count) != identity) {                    \
            ERR(#T " op %d: past the end is not the identity", (int)op);       \
        }                                                                      \
        /* The tree holds at least one cell per element */                     \
        if (count && segment##T##Bytes(seg) < count * sizeof(elem[0])) {       \
            ERR(#T ": Bytes %zu below %zu elements", segment##T##Bytes(seg),   \
                count);                                                        \
        }                                                                      \
        segment##T##Free(seg);                                                 \
        return err;                                                            \
    }

SGT_DIFF(I16, int16_t, uint16_t, INT16_MIN, INT16_MAX, 0)
SGT_DIFF(I32, int32_t, uint32_t, INT32_MIN, INT32_MAX, 0)
SGT_DIFF(I64, int64_t, uint64_t, INT64_MIN, INT64_MAX, 0)
SGT_DIFF(U16, uint16_t, uint16_t, 0, UINT16_MAX, 0)
SGT_DIFF(U32, uint32_t, uint32_t, 0, UINT32_MAX, 0)
SGT_DIFF(U64, uint64_t, uint64_t, 0, UINT64_MAX, 0)
SGT_DIFF(Float, float, float, -FLT_MAX, FLT_MAX, 1)
SGT_DIFF(Double, double, double, -DBL_MAX, DBL_MAX, 1)
#ifdef __SIZEOF_INT128__
#define SGT_I128_MAX ((__int128_t)(~(__uint128_t)0 >> 1))
SGT_DIFF(I128, __int128_t, __uint128_t, -SGT_I128_MAX - 1, SGT_I128_MAX, 0)
SGT_DIFF(U128, __uint128_t, __uint128_t, 0, ~(__uint128_t)0, 0)
#endif

/* NewFromArray and CopyTo against the source array and a tree built with
 * one Update per element.  Every node folds its children, so the two
 * trees answer every query identically, float sums included: the floats
 * here are arbitrary, not small integers. */
#define SGT_BULK(T, VT, IS_FLOAT)                                              \
    static int sgtBulk##T(segmentOp op, uint64_t seed) {                       \
        int err = 0;                                                           \
        static constexpr size_t sizes[] = {0, 1, 5, 1000, 20000};              \
        for (size_t s = 0; s < countof(sizes); s++) {                          \
            const size_t n = sizes[s];                                         \
            cleaner(zfree) void *bufA = zcalloc(n + 1, sizeof(VT));            \
            cleaner(zfree) void *bufB = zcalloc(n + 1, sizeof(VT));            \
            VT *values = bufA;                                                 \
            VT *copied = bufB;                                                 \
            void *byUpdate = segment##T##New(op);                              \
            for (size_t i = 0; i < n; i++) {                                   \
                const uint64_t r = dkSplitmix64Next(&seed);                    \
                values[i] = IS_FLOAT ? (VT)((double)(int64_t)r / 1e9) : (VT)r; \
                segment##T##Update(&byUpdate, i, values[i]);                   \
            }                                                                  \
                                                                               \
            void *seg = segment##T##NewFromArray(op, n ? values : nullptr, n); \
            segment##T##CopyTo(seg, copied);                                   \
            if (!seg || segment##T##Count(seg) != n ||                         \
                memcmp(copied, values, n * sizeof(VT)) != 0) {                 \
                ERR(#T ": NewFromArray/CopyTo of %zu elements", n);            \
            }                                                                  \
                                                                               \
            for (size_t q = 0; n && q < 64; q++) {                             \
                const size_t a = (size_t)(dkSplitmix64Next(&seed) % n);        \
                const size_t z = (size_t)(dkSplitmix64Next(&seed) % n);        \
                const size_t left = a < z ? a : z;                             \
                const size_t right = a < z ? z : a;                            \
                const VT x = segment##T##Query(seg, left, right);              \
                const VT y = segment##T##Query(byUpdate, left, right);         \
                if (memcmp(&x, &y, sizeof(x)) != 0) {                          \
                    ERR(#T ": [%zu, %zu] of %zu differs from the Update tree", \
                        left, right, n);                                       \
                    break;                                                     \
                }                                                              \
            }                                                                  \
                                                                               \
            segment##T##Free(seg);                                             \
            segment##T##Free(byUpdate);                                        \
        }                                                                      \
                                                                               \
        return err;                                                            \
    }

SGT_BULK(I16, int16_t, 0)
SGT_BULK(I32, int32_t, 0)
SGT_BULK(I64, int64_t, 0)
SGT_BULK(U16, uint16_t, 0)
SGT_BULK(U32, uint32_t, 0)
SGT_BULK(U64, uint64_t, 0)
SGT_BULK(Float, float, 1)
SGT_BULK(Double, double, 1)
#ifdef __SIZEOF_INT128__
SGT_BULK(I128, __int128_t, 0)
SGT_BULK(U128, __uint128_t, 0)
#endif

/* The Small and Full tier functions (public in segmentCore.h), the
 * dispatch API and the persist descriptor at their edges: a NULL tree
 * reads empty and refuses writes; an empty array builds an empty tree;
 * reads outside the tree give the identity (0 for SUM); RangeUpdate
 * never extends the tree and clamps its right end; a Full update whose
 * capacity would overflow is refused; restore and applyOp refuse bodies
 * that are not theirs. */
#define SGT_TIER_EDGES(T, VT)                                                  \
    static int sgtTierEdges##T(void) {                                         \
        int err = 0;                                                           \
        typedef typeof(segment##T##FullCount(nullptr)) FullIdx;                \
        bool ok = true;                                                        \
                                                                               \
        if (segment##T##SmallQuery(nullptr, 0, 1) != 0 ||                      \
            segment##T##SmallGet(nullptr, 0) != 0 ||                           \
            segment##T##SmallCount(nullptr) != 0 ||                            \
            segment##T##SmallBytes(nullptr) != 0 ||                            \
            segment##T##SmallShouldUpgrade(nullptr) ||                         \
            segment##T##SmallUpdate(nullptr, 0, 1, &ok) || ok) {               \
            ERR(#T ": a NULL Small tree did not read empty");                  \
        }                                                                      \
                                                                               \
        ok = true;                                                             \
        if (segment##T##FullQuery(nullptr, 0, 1) != 0 ||                       \
            segment##T##FullGet(nullptr, 0) != 0 ||                            \
            segment##T##FullCount(nullptr) != 0 ||                             \
            segment##T##FullBytes(nullptr) != 0 ||                             \
            segment##T##FullUpdate(nullptr, 0, 1, &ok) || ok) {                \
            ERR(#T ": a NULL Full tree did not read empty");                   \
        }                                                                      \
                                                                               \
        segment##T##SmallRangeUpdate(nullptr, 0, 1, 1);                        \
        segment##T##FullRangeUpdate(nullptr, 0, 1, 1);                         \
        segment##T##Free(nullptr);                                             \
        segment##T##CopyTo(nullptr, nullptr);                                  \
        segment##T##Update(nullptr, 0, 1);                                     \
        if (segment##T##Count(nullptr) != 0 ||                                 \
            segment##T##Query(nullptr, 0, 1) != 0 ||                           \
            segment##T##Get(nullptr, 0) != 0 ||                                \
            segment##T##Bytes(nullptr) != 0) {                                 \
            ERR(#T ": a NULL tree did not read empty through the API");        \
        }                                                                      \
                                                                               \
        /* Empty arrays, and a Full tree from no Small one */                  \
        const VT three[3] = {(VT)1, (VT)2, (VT)3};                             \
        segment##T##Small *emptySmall =                                        \
            segment##T##SmallNewFromArray(nullptr, 3, SEGMENT_OP_SUM);         \
        segment##T##Full *emptyFull =                                          \
            segment##T##FullNewFromArray(three, 0, SEGMENT_OP_SUM);            \
        segment##T##Full *fromNone = segment##T##FullFromSmall(nullptr);       \
        segment##T##SmallRangeUpdate(emptySmall, 0, 1, 1);                     \
        segment##T##FullRangeUpdate(emptyFull, 0, 1, 1);                       \
        if (!emptySmall || segment##T##SmallCount(emptySmall) != 0 ||          \
            !emptyFull || segment##T##FullCount(emptyFull) != 0 ||             \
            !fromNone || segment##T##FullCount(fromNone) != 0) {               \
            ERR(#T ": an empty array did not build an empty tree");            \
        }                                                                      \
        segment##T##SmallFree(emptySmall);                                     \
        segment##T##FullFree(emptyFull);                                       \
        segment##T##FullFree(fromNone);                                        \
                                                                               \
        segment##T##Small *small =                                             \
            segment##T##SmallNewFromArray(three, 3, SEGMENT_OP_SUM);           \
        segment##T##Full *full =                                               \
            segment##T##FullNewFromArray(three, 3, SEGMENT_OP_SUM);            \
        if (segment##T##SmallQuery(small, 0, 2) != (VT)6 ||                    \
            segment##T##FullQuery(full, 0, 2) != (VT)6 ||                      \
            segment##T##SmallQuery(small, 2, 1) != 0 ||                        \
            segment##T##SmallQuery(small, 0, 3) != 0 ||                        \
            segment##T##SmallGet(small, 3) != 0 ||                             \
            segment##T##FullQuery(full, 2, 1) != 0 ||                          \
            segment##T##FullQuery(full, 0, 3) != 0 ||                          \
            segment##T##FullGet(full, 3) != 0) {                               \
            ERR(#T ": a fold or read outside the tree was not the identity");  \
        }                                                                      \
                                                                               \
        /* RangeUpdate: an inverted range or one past the count changes        \
         * nothing; one running past the count stops at the last element */    \
        segment##T##SmallRangeUpdate(small, 2, 1, 9);                          \
        segment##T##SmallRangeUpdate(small, 3, 5, 9);                          \
        segment##T##SmallRangeUpdate(small, 1, 99, 5);                         \
        segment##T##FullRangeUpdate(full, 2, 1, 9);                            \
        segment##T##FullRangeUpdate(full, 3, 5, 9);                            \
        segment##T##FullRangeUpdate(full, 1, 99, 5);                           \
        if (segment##T##SmallCount(small) != 3 ||                              \
            segment##T##SmallQuery(small, 0, 2) != (VT)11 ||                   \
            segment##T##FullCount(full) != 3 ||                                \
            segment##T##FullQuery(full, 0, 2) != (VT)11) {                     \
            ERR(#T ": RangeUpdate past the edges changed the wrong elements"); \
        }                                                                      \
                                                                               \
        /* A capacity past the index type's range is refused, unchanged */     \
        ok = true;                                                             \
        full = segment##T##FullUpdate(full, (FullIdx) - 1, (VT)1, &ok);        \
        if (ok || segment##T##FullCount(full) != 3 ||                          \
            segment##T##FullGet(full, 2) != (VT)5) {                           \
            ERR(#T ": an update at the last Full index was not refused");      \
        }                                                                      \
        /* So is one past one doubling and the jump limit (2^22 cells) */      \
        ok = true;                                                             \
        full = segment##T##FullUpdate(full, (FullIdx)1 << 23, (VT)1, &ok);     \
        if (ok || segment##T##FullCount(full) != 3 ||                          \
            segment##T##FullGet(full, 2) != (VT)5) {                           \
            ERR(#T ": a Full update far past its size was not refused");       \
        }                                                                      \
        segment##T##SmallFree(small);                                          \
        segment##T##FullFree(full);                                            \
                                                                               \
        /* Through the API: a NULL tree pointer becomes a SUM tree, Bytes      \
         * covers both tiers, Get past the count is the identity */            \
        void *seg = nullptr;                                                   \
        segment##T##Update(&seg, 2, (VT)7);                                    \
        const size_t smallBytes = segment##T##Bytes(seg);                      \
        segment##T##Update(&seg, 1 << 17, (VT)1);                              \
        if (segment##T##Count(seg) != (1 << 17) + 1 ||                         \
            segment##T##Get(seg, 2) != (VT)7 ||                                \
            segment##T##Get(seg, (1 << 17) + 9) != 0 || smallBytes == 0 ||     \
            segment##T##Bytes(seg) <= smallBytes) {                            \
            ERR(#T ": Update from NULL, Bytes or Get past the count");         \
        }                                                                      \
        segment##T##Free(seg);                                                 \
                                                                               \
        /* Persist: no operation byte, a wrong op or operand count, and a      \
         * signed index are refused */                                         \
        const persistOps *ops = &persistOpsSegment##T;                         \
        cleaner(flexFree) flex *none = flexNew();                              \
        const databox one = databoxNewUnsigned(1);                             \
        const databox negative = databoxNewSigned(-1);                         \
        const databox value = databoxNewUnsigned(2);                           \
        const databox *lone[] = {&one};                                        \
        const databox *signedIdx[] = {&negative, &value};                      \
        size_t loneLen = 0;                                                    \
        size_t signedLen = 0;                                                  \
        size_t badLen = 0;                                                     \
        cleaner(zfree) void *loneBody =                                        \
            persistOpEncodeBoxes(lone, 1, &loneLen);                           \
        cleaner(zfree) void *signedBody =                                      \
            persistOpEncodeBoxes(signedIdx, 2, &signedLen);                    \
        void *target = segment##T##New(SEGMENT_OP_SUM);                        \
        if (ops->restore((const uint8_t *)none, flexBytes(none)) ||            \
            ops->encodeOp(PERSIST_OP_INSERT, signedIdx, 2, &badLen) ||         \
            ops->encodeOp(PERSIST_OP_UPDATE, lone, 1, &badLen) ||              \
            ops->applyOp(&target, PERSIST_OP_UPDATE, loneBody, loneLen) ||     \
            ops->applyOp(&target, PERSIST_OP_UPDATE, signedBody, signedLen) || \
            segment##T##Count(target) != 0) {                                  \
            ERR(#T ": persist accepted a body that is not its own");           \
        }                                                                      \
        segment##T##Free(target);                                              \
        return err;                                                            \
    }

SGT_TIER_EDGES(I16, int16_t)
SGT_TIER_EDGES(I32, int32_t)
SGT_TIER_EDGES(I64, int64_t)
SGT_TIER_EDGES(U16, uint16_t)
SGT_TIER_EDGES(U32, uint32_t)
SGT_TIER_EDGES(U64, uint64_t)
#ifdef __SIZEOF_INT128__
SGT_TIER_EDGES(I128, __int128_t)
SGT_TIER_EDGES(U128, __uint128_t)
#endif
SGT_TIER_EDGES(Float, float)
SGT_TIER_EDGES(Double, double)

int segmentTypesTest(int, char *[]) {
    int err = 0;

#define SGT_RUN(T)                                                             \
    TEST("differential: segment" #T                                            \
         " SUM, MIN and MAX against a naive array") {                          \
        int typeErr = 0; /* every type runs, whatever failed before */         \
        const segmentOp ops[] = {SEGMENT_OP_SUM, SEGMENT_OP_MIN,               \
                                 SEGMENT_OP_MAX};                              \
        for (size_t o = 0; o < 3 && !typeErr; o++) {                           \
            for (uint64_t seed = 1; seed <= 2 && !typeErr; seed++) {           \
                typeErr += sgtDiff##T(ops[o], seed);                           \
            }                                                                  \
        }                                                                      \
        err += typeErr;                                                        \
    }                                                                          \
    TEST("NewFromArray and CopyTo: segment" #T " SUM, MIN and MAX") {          \
        err += sgtBulk##T(SEGMENT_OP_SUM, 31);                                 \
        err += sgtBulk##T(SEGMENT_OP_MIN, 32);                                 \
        err += sgtBulk##T(SEGMENT_OP_MAX, 33);                                 \
    }                                                                          \
    TEST("tiers, API and persist at their edges: segment" #T) {                \
        err += sgtTierEdges##T();                                              \
    }                                                                          \
    TEST("Repr: segment" #T " nil, Small and Full") {                          \
        segment##T##Repr(nullptr);                                             \
        void *seg = segment##T##New(SEGMENT_OP_SUM);                           \
        segment##T##Update(&seg, 3, 1);                                        \
        segment##T##Repr(seg);                                                 \
        segment##T##Update(&seg, 1 << 17, 1);                                  \
        segment##T##Repr(seg);                                                 \
        segment##T##Free(seg);                                                 \
    }

    SGT_RUN(I16)
    SGT_RUN(I32)
    SGT_RUN(I64)
    SGT_RUN(U16)
    SGT_RUN(U32)
    SGT_RUN(U64)
#ifdef __SIZEOF_INT128__
    SGT_RUN(I128)
    SGT_RUN(U128)
#endif
    SGT_RUN(Float)
    SGT_RUN(Double)
#undef SGT_RUN

    TEST_FINAL_RESULT;
}
