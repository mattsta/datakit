#include "multiFenwickFlat.h"
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "fenwick/fenwickI64.h"
#include "multiFenwick.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);
defcleaner(multiFenwick *, multiFenwickFree);
defcleanerName(databox *, zfree, DataboxArray);

/* Same type and the same value bits (doubles compare bitwise, so -0.0 and
 * 0.0 differ, as a stored value would) */
static bool mfwfSameBox_(const databox *a, const databox *b) {
    if (a->type != b->type) {
        return false;
    }

    switch (a->type) {
    case DATABOX_SIGNED_64:
        return a->data.i64 == b->data.i64;
    case DATABOX_UNSIGNED_64:
        return a->data.u64 == b->data.u64;
    case DATABOX_DOUBLE_64:
        return memcmp(&a->data.d64, &b->data.d64, sizeof(double)) == 0;
    case DATABOX_VOID:
        return true;
    default:
        return false;
    }
}

/* A random value or delta: mostly small integers, plus integer limits,
 * floats, doubles, and values the trees must refuse. */
static databox mfwfRandValue_(uint64_t *seed, bool integersOnly) {
    const uint64_t r = dkSplitmix64Next(seed);
    const uint64_t pick = r % (integersOnly ? 5 : 12);
    switch (pick) {
    case 0:
    case 1:
        return DATABOX_SIGNED((int64_t)(dkSplitmix64Next(seed) % 2001) - 1000);
    case 2:
        return DATABOX_UNSIGNED(dkSplitmix64Next(seed) % 5000);
    case 3:
        return DATABOX_SIGNED(dkSplitmix64Next(seed) % 2 ? INT64_MIN
                                                         : INT64_MAX);
    case 4:
        return DATABOX_UNSIGNED(dkSplitmix64Next(seed) % 2 ? UINT64_MAX
                                                           : UINT64_MAX / 3);
    case 5:
    case 6:
        return DATABOX_DOUBLE(
            ((double)(dkSplitmix64Next(seed) % 20001) - 10000.0) / 8.0);
    case 7: {
        databox f;
        DATABOX_SET_FLOAT(&f, (float)(dkSplitmix64Next(seed) % 100) / 4.0f);
        return f;
    }
    case 8:
        return DATABOX_DOUBLE(dkSplitmix64Next(seed) % 2 ? 1e300 : -DBL_MAX);
    case 9:
        return DATABOX_DOUBLE(dkSplitmix64Next(seed) % 2 ? NAN : INFINITY);
    case 10:
        return DATABOX_BOX_VOID;
    default:
        return DATABOX_BOX_TRUE;
    }
}

/* Compare every element and prefix sum of the two trees, and their
 * counts.  Returns the number of mismatches. */
static int mfwfCompareAll_(const multiFenwickFlat *flat,
                           const multiFenwick *orig, const char *when) {
    int err = 0;
    if (multiFenwickFlatCount(flat) != multiFenwickCount(orig)) {
        ERR("%s: count %zu vs %zu", when, multiFenwickFlatCount(flat),
            multiFenwickCount(orig));
        return err;
    }

    for (size_t i = 0; i < multiFenwickCount(orig); i++) {
        databox a;
        databox b;
        const bool okA = multiFenwickFlatGet(flat, i, &a);
        const bool okB = multiFenwickGet(orig, i, &b);
        if (okA != okB || !mfwfSameBox_(&a, &b)) {
            ERR("%s: Get(%zu) differs", when, i);
        }

        const bool qA = multiFenwickFlatQuery(flat, i, &a);
        const bool qB = multiFenwickQuery(orig, i, &b);
        if (qA != qB || !mfwfSameBox_(&a, &b)) {
            ERR("%s: Query(%zu) differs", when, i);
        }
    }

    return err;
}

/* Run 'ops' random operations on the two trees, checking that every call
 * returns the same result.  Returns the number of mismatches. */
static int mfwfDifferential_(uint64_t seed, size_t ops, size_t maxIdx,
                             bool integersOnly) {
    int err = 0;
    multiFenwickFlat *flat = nullptr;
    multiFenwick *orig = nullptr;

    for (size_t step = 0; step < ops; step++) {
        const uint64_t op = dkSplitmix64Next(&seed) % 10;
        size_t idx = dkSplitmix64Next(&seed) % (maxIdx + 1);
        if (dkSplitmix64Next(&seed) % 64 == 0) {
            idx = (size_t)MULTIFENWICK_INDEX_MAX + 1; /* refused */
        }

        const databox v = mfwfRandValue_(&seed, integersOnly);
        databox a = DATABOX_BOX_VOID;
        databox b = DATABOX_BOX_VOID;
        bool okA = false;
        bool okB = false;
        size_t posA = 0;
        size_t posB = 0;
        switch (op) {
        case 0:
        case 1:
        case 2:
            okA = multiFenwickFlatUpdate(&flat, idx, &v);
            okB = multiFenwickUpdate(&orig, idx, &v);
            break;
        case 3:
        case 4:
            okA = multiFenwickFlatSet(&flat, idx, &v);
            okB = multiFenwickSet(&orig, idx, &v);
            break;
        case 5:
            okA = multiFenwickFlatGet(flat, idx, &a);
            okB = multiFenwickGet(orig, idx, &b);
            break;
        case 6:
            okA = multiFenwickFlatQuery(flat, idx, &a);
            okB = multiFenwickQuery(orig, idx, &b);
            break;
        case 7: {
            const size_t left = dkSplitmix64Next(&seed) % (maxIdx + 1);
            okA = multiFenwickFlatRangeQuery(flat, left, idx, &a);
            okB = multiFenwickRangeQuery(orig, left, idx, &b);
            break;
        }
        case 8:
            posA = multiFenwickFlatLowerBound(flat, &v);
            posB = multiFenwickLowerBound(orig, &v);
            break;
        default:
            if (dkSplitmix64Next(&seed) % 16 == 0) {
                multiFenwickFlatClear(flat);
                multiFenwickClear(orig);
            }
            break;
        }

        if (okA != okB || posA != posB || !mfwfSameBox_(&a, &b) ||
            (flat == nullptr) != (orig == nullptr) ||
            multiFenwickFlatCount(flat) != multiFenwickCount(orig)) {
            ERR("step %zu op %" PRIu64 " idx %zu: flat ok=%d pos=%zu, "
                "multiFenwick ok=%d pos=%zu",
                step, op, idx, okA, posA, okB, posB);
            break;
        }

        if (step % 512 == 511) {
            err += mfwfCompareAll_(flat, orig, "periodic sweep");
        }
    }

    err += mfwfCompareAll_(flat, orig, "final sweep");
    multiFenwickFlatFree(flat);
    multiFenwickFree(orig);
    return err;
}

/* ns per call of 'ops' queries or updates at random indexes below n */
static double mfwfBenchFlat_(multiFenwickFlat **flat, size_t n, size_t ops,
                             bool update, int64_t *checksum) {
    uint64_t seed = 54321;
    int64_t sum = 0;
    const databox one = DATABOX_SIGNED(1);
    PERF_TIMERS_SETUP;
    for (size_t i = 0; i < ops; i++) {
        const size_t idx = dkSplitmix64Next(&seed) % n;
        if (update) {
            multiFenwickFlatUpdate(flat, idx, &one);
        } else {
            databox r;
            multiFenwickFlatQuery(*flat, idx, &r);
            sum += r.data.i64;
        }
    }
    PERF_TIMERS_FINISH;
    *checksum = sum;
    return (double)lps.global.us.duration * 1e3 / (double)ops;
}

static double mfwfBenchOrig_(multiFenwick **orig, size_t n, size_t ops,
                             bool update, int64_t *checksum) {
    uint64_t seed = 54321;
    int64_t sum = 0;
    const databox one = DATABOX_SIGNED(1);
    PERF_TIMERS_SETUP;
    for (size_t i = 0; i < ops; i++) {
        const size_t idx = dkSplitmix64Next(&seed) % n;
        if (update) {
            multiFenwickUpdate(orig, idx, &one);
        } else {
            databox r;
            multiFenwickQuery(*orig, idx, &r);
            sum += r.data.i64;
        }
    }
    PERF_TIMERS_FINISH;
    *checksum = sum;
    return (double)lps.global.us.duration * 1e3 / (double)ops;
}

static double mfwfBenchI64_(void **fw, size_t n, size_t ops, bool update,
                            int64_t *checksum) {
    uint64_t seed = 54321;
    int64_t sum = 0;
    PERF_TIMERS_SETUP;
    for (size_t i = 0; i < ops; i++) {
        const size_t idx = dkSplitmix64Next(&seed) % n;
        if (update) {
            fenwickI64Update(fw, idx, 1);
        } else {
            sum += fenwickI64Query(*fw, idx);
        }
    }
    PERF_TIMERS_FINISH;
    *checksum = sum;
    return (double)lps.global.us.duration * 1e3 / (double)ops;
}

int multiFenwickFlatTest(int, char *[]) {
    int err = 0;

    TEST("NULL and empty trees") {
        databox r = DATABOX_SIGNED(7);
        const databox one = DATABOX_SIGNED(1);
        if (multiFenwickFlatCount(nullptr) != 0 ||
            multiFenwickFlatBytes(nullptr) != 0 ||
            multiFenwickFlatQuery(nullptr, 0, &r) || r.type != DATABOX_VOID ||
            multiFenwickFlatGet(nullptr, 0, nullptr) ||
            multiFenwickFlatLowerBound(nullptr, &one) != SIZE_MAX) {
            ERR("NULL tree is not an empty tree");
        }

        multiFenwickFlatClear(nullptr);
        multiFenwickFlatFree(nullptr);
        if (multiFenwickFlatUpdate(nullptr, 0, &one)) {
            ERR("Update accepted a NULL tree pointer");
        }

        cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw =
            multiFenwickFlatNew();
        r = DATABOX_SIGNED(7);
        if (!mfw || multiFenwickFlatCount(mfw) != 0 ||
            multiFenwickFlatQuery(mfw, 0, &r) || r.type != DATABOX_VOID ||
            multiFenwickFlatRangeQuery(mfw, 0, 0, &r) ||
            multiFenwickFlatLowerBound(mfw, &one) != SIZE_MAX) {
            ERR("new tree is not empty");
        }

        multiFenwickFlatClear(mfw); /* no cells: nothing to clear */
        cleaner(multiFenwickFlatFree) multiFenwickFlat *fromNone =
            multiFenwickFlatNewFromArray(nullptr, 0);
        if (!fromNone || multiFenwickFlatCount(fromNone) != 0) {
            ERR("NewFromArray(NULL, 0) is not a new empty tree");
        }

        cleaner(multiFenwickFlatFree) multiFenwickFlat *refused =
            multiFenwickFlatNewFromArray(nullptr, 3);
        if (refused) {
            ERR("NewFromArray accepted NULL values with a count");
        }
    }

    TEST("storage: 8 bytes per int64 cell, 9 with kinds, capacity doubles") {
        cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw =
            multiFenwickFlatNew();
        const size_t base = multiFenwickFlatBytes(mfw);
        const databox one = DATABOX_UNSIGNED(1);
        const struct {
            size_t idx;
            size_t capacity;
        } steps[] = {{0, 1}, {1, 2}, {2, 4}, {4, 8}, {5, 8}, {1000, 1024}};
        for (size_t s = 0; s < countof(steps); s++) {
            if (!multiFenwickFlatUpdate(&mfw, steps[s].idx, &one)) {
                ERR("Update(%zu) refused", steps[s].idx);
            }

            if (multiFenwickFlatBytes(mfw) != base + 8 * steps[s].capacity) {
                ERR("after Update(%zu): %zu bytes, want %zu", steps[s].idx,
                    multiFenwickFlatBytes(mfw), base + 8 * steps[s].capacity);
            }
        }

        /* A cell above INT64_MAX or a double adds the kinds array; every
         * value survives the switch */
        const databox half = DATABOX_DOUBLE(0.5);
        if (!multiFenwickFlatUpdate(&mfw, 3, &half) ||
            multiFenwickFlatBytes(mfw) != base + 9 * 1024) {
            ERR("after a double: %zu bytes, want %zu",
                multiFenwickFlatBytes(mfw), base + 9 * (size_t)1024);
        }

        databox d;
        if (!multiFenwickFlatQuery(mfw, 1000, &d) ||
            d.type != DATABOX_DOUBLE_64 || d.data.d64 != 6.5 ||
            !multiFenwickFlatGet(mfw, 4, &d) || d.type != DATABOX_UNSIGNED_64 ||
            d.data.u64 != 1) {
            ERR("values changed when the kinds array was added");
        }

        const databox minusHalf = DATABOX_DOUBLE(-0.5);
        if (!multiFenwickFlatUpdate(&mfw, 3, &minusHalf)) {
            ERR("Update(-0.5) refused");
        }

        /* Elements 0, 1, 2, 4, 5 and 1000 are 1; everything else is 0,
         * though cells touched by element 3 stay doubles */
        databox r;
        if (!multiFenwickFlatQuery(mfw, 1000, &r) ||
            r.type != DATABOX_DOUBLE_64 || r.data.d64 != 6 ||
            !multiFenwickFlatGet(mfw, 3, &r) || r.data.u64 != 0 ||
            !multiFenwickFlatGet(mfw, 1000, &r) || r.data.u64 != 1) {
            ERR("values lost across growth");
        }

        multiFenwickFlatClear(mfw);
        if (multiFenwickFlatCount(mfw) != 1001 ||
            multiFenwickFlatBytes(mfw) != base + 8 * 1024 ||
            !multiFenwickFlatQuery(mfw, 1000, &r) ||
            r.type != DATABOX_UNSIGNED_64 || r.data.u64 != 0) {
            ERR("Clear did not zero every cell, keep count and capacity, "
                "and drop the kinds array");
        }
    }

    TEST("transactional: a refused write changes nothing") {
        cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw = nullptr;
        const databox big = DATABOX_UNSIGNED(UINT64_MAX);
        const databox nan = DATABOX_DOUBLE(NAN);
        const databox bytes = databoxNewBytesString("nope");
        if (multiFenwickFlatUpdate(&mfw, 0, &nan) ||
            multiFenwickFlatUpdate(&mfw, 0, &bytes) ||
            multiFenwickFlatSet(&mfw, MULTIFENWICK_FLAT_INDEX_MAX + 1, &big) ||
            mfw != nullptr) {
            ERR("a refused write created a tree");
        }

        /* Elements [UINT64_MAX, 0, 2^63, INT64_MIN]: cell 2 (elements
         * 0..1) and cell 4 (the total) both hold UINT64_MAX. */
        const databox half = DATABOX_UNSIGNED(UINT64_C(1) << 63);
        const databox min = DATABOX_SIGNED(INT64_MIN);
        if (!multiFenwickFlatUpdate(&mfw, 0, &big) ||
            !multiFenwickFlatUpdate(&mfw, 3, &min) ||
            !multiFenwickFlatUpdate(&mfw, 2, &half)) {
            ERR("in-range writes refused");
        }

        /* Adding to element 1 overflows cell 2; growing to index 4 adds
         * to cell 8, which starts as the total. */
        const size_t bytesBefore = multiFenwickFlatBytes(mfw);
        const databox one = DATABOX_UNSIGNED(1);
        if (multiFenwickFlatUpdate(&mfw, 1, &one) ||
            multiFenwickFlatUpdate(&mfw, 4, &one) ||
            multiFenwickFlatSet(&mfw, 0, &bytes)) {
            ERR("an out-of-range block sum was accepted");
        }

        databox r;
        if (multiFenwickFlatCount(mfw) != 4 ||
            multiFenwickFlatBytes(mfw) != bytesBefore ||
            !multiFenwickFlatGet(mfw, 0, &r) || r.type != DATABOX_UNSIGNED_64 ||
            r.data.u64 != UINT64_MAX || !multiFenwickFlatGet(mfw, 1, &r) ||
            r.data.u64 != 0 || !multiFenwickFlatGet(mfw, 2, &r) ||
            r.data.u64 != half.data.u64 || !multiFenwickFlatGet(mfw, 3, &r) ||
            r.type != DATABOX_SIGNED_64 || r.data.i64 != INT64_MIN) {
            ERR("a refused write changed the tree");
        }

        /* The prefix sum through 2 (UINT64_MAX + 2^63) has no 64-bit
         * value; the range [1, 2] and the total do. */
        if (multiFenwickFlatQuery(mfw, 2, &r) || r.type != DATABOX_VOID ||
            !multiFenwickFlatRangeQuery(mfw, 1, 2, &r) ||
            r.data.u64 != half.data.u64 || !multiFenwickFlatQuery(mfw, 3, &r) ||
            r.data.u64 != UINT64_MAX) {
            ERR("out-of-range prefix or in-range range handled wrongly");
        }
    }

    TEST("independent oracle: exact integer sums against an int128 array") {
        constexpr int N = 300;
        __int128_t oracle[N] = {};
        cleaner(multiFenwickFlatFree) multiFenwickFlat *mfw = nullptr;
        uint64_t seed = 77;
        size_t count = 0;
        for (size_t step = 0; step < 4000; step++) {
            const size_t idx = dkSplitmix64Next(&seed) % N;
            const int64_t v =
                (int64_t)(dkSplitmix64Next(&seed) % 2000001) - 1'000'000;
            const databox box = DATABOX_SIGNED(v);
            const bool set = dkSplitmix64Next(&seed) % 4 == 0;
            const bool ok = set ? multiFenwickFlatSet(&mfw, idx, &box)
                                : multiFenwickFlatUpdate(&mfw, idx, &box);
            if (!ok) {
                ERR("step %zu refused an in-range write", step);
                break;
            }

            oracle[idx] = set ? v : oracle[idx] + v;
            count = idx + 1 > count ? idx + 1 : count;

            const size_t q = dkSplitmix64Next(&seed) % count;
            __int128_t want = 0;
            for (size_t i = 0; i <= q; i++) {
                want += oracle[i];
            }

            databox r;
            const __int128_t got =
                !multiFenwickFlatQuery(mfw, q, &r) ? INT64_MIN
                : r.type == DATABOX_SIGNED_64      ? r.data.i64
                : r.type == DATABOX_UNSIGNED_64    ? (__int128_t)r.data.u64
                                                   : INT64_MIN;
            if (got != want || (want < 0) != (r.type == DATABOX_SIGNED_64)) {
                ERR("step %zu: Query(%zu) wrong", step, q);
                break;
            }

            if (!multiFenwickFlatGet(mfw, q, &r) ||
                (r.type == DATABOX_SIGNED_64
                     ? (__int128_t)r.data.i64
                     : (__int128_t)r.data.u64) != oracle[q]) {
                ERR("step %zu: Get(%zu) wrong", step, q);
                break;
            }
        }

        if (multiFenwickFlatCount(mfw) != count) {
            ERR("count %zu, want %zu", multiFenwickFlatCount(mfw), count);
        }
    }

    TEST("differential: identical results to multiFenwick, integers") {
        for (uint64_t seed = 1; seed <= 4; seed++) {
            err += mfwfDifferential_(seed, 3000, 200, true);
        }
    }

    TEST("differential: identical results to multiFenwick, mixed values") {
        for (uint64_t seed = 11; seed <= 14; seed++) {
            err += mfwfDifferential_(seed, 3000, 200, false);
        }
    }

    TEST("differential: NewFromArray matches multiFenwick") {
        uint64_t seed = 99;
        const size_t sizes[] = {1, 2, 3, 7, 8, 9, 100, 513};
        for (size_t s = 0; s < countof(sizes); s++) {
            const size_t n = sizes[s];
            for (size_t round = 0; round < 6; round++) {
                cleanerName(zfree, DataboxArray) databox *vals =
                    zcalloc(n, sizeof(databox));
                for (size_t i = 0; i < n; i++) {
                    vals[i] = mfwfRandValue_(&seed, round % 2 == 0);
                }

                cleaner(multiFenwickFlatFree) multiFenwickFlat *flat =
                    multiFenwickFlatNewFromArray(vals, n);
                cleaner(multiFenwickFree) multiFenwick *orig =
                    multiFenwickNewFromArray(vals, n);
                if ((flat == nullptr) != (orig == nullptr)) {
                    ERR("n=%zu round %zu: one tree refused the array", n,
                        round);
                } else if (flat) {
                    err += mfwfCompareAll_(flat, orig, "NewFromArray");
                }
            }
        }
    }

    TEST("COMPARISON: flat vs multiFenwick vs fenwickI64") {
        printf("\n=== ns per call (200K calls, random indexes) ===\n");
        printf("    N | op     | flat     | multiFenwick | fenwickI64 | "
               "mfw/flat | flat/i64\n");
        const size_t sizes[] = {1000, 100'000};
        const size_t OPS = 200'000;
        for (size_t s = 0; s < countof(sizes); s++) {
            const size_t n = sizes[s];
            cleanerName(zfree, DataboxArray) databox *vals =
                zcalloc(n, sizeof(databox));
            uint64_t seed = 12345;
            void *fw = fenwickI64New();
            for (size_t i = 0; i < n; i++) {
                const int64_t v =
                    (int64_t)(dkSplitmix64Next(&seed) % 1000) - 500;
                vals[i] = DATABOX_SIGNED(v);
                fenwickI64Update(&fw, i, v);
            }

            multiFenwickFlat *flat = multiFenwickFlatNewFromArray(vals, n);
            multiFenwick *orig = multiFenwickNewFromArray(vals, n);
            for (int update = 0; update <= 1; update++) {
                int64_t sumFlat;
                int64_t sumOrig;
                int64_t sumI64;
                const double tFlat =
                    mfwfBenchFlat_(&flat, n, OPS, update, &sumFlat);
                const double tOrig =
                    mfwfBenchOrig_(&orig, n, OPS, update, &sumOrig);
                const double tI64 = mfwfBenchI64_(&fw, n, OPS, update, &sumI64);
                if (sumFlat != sumOrig || sumFlat != sumI64) {
                    ERR("N=%zu checksums differ: %" PRId64 " %" PRId64
                        " %" PRId64,
                        n, sumFlat, sumOrig, sumI64);
                }

                printf("%6zu | %-6s | %8.1f | %12.1f | %10.1f | %8.1fx | "
                       "%7.1fx\n",
                       n, update ? "update" : "query", tFlat, tOrig, tI64,
                       tOrig / tFlat, tFlat / tI64);
            }

            printf("%6zu | bytes  | %8zu | %12zu | %10zu |\n", n,
                   multiFenwickFlatBytes(flat), multiFenwickBytes(orig),
                   fenwickI64Bytes(fw));
            multiFenwickFlatFree(flat);
            multiFenwickFree(orig);
            fenwickI64Free(fw);
        }

        printf("\n");
    }

    TEST("Repr: nil, empty and holding values") {
        multiFenwickFlatRepr(nullptr);
        multiFenwickFlat *mfw = multiFenwickFlatNew();
        multiFenwickFlatRepr(mfw);
        const databox one = DATABOX_SIGNED(1);
        multiFenwickFlatUpdate(&mfw, 5, &one);
        multiFenwickFlatRepr(mfw);
        multiFenwickFlatFree(mfw);
    }

    TEST_FINAL_RESULT;
}
