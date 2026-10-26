/* Tests for multiFenwick2d: exact results against an int128 grid, mixed
 * integer/double values, one-column agreement with multiFenwickFlat, and
 * the refusal rules. */

#include "multiFenwick2d.h"
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "multiFenwickFlat.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);
defcleaner(multiFenwick2d *, multiFenwick2dFree);
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);

/* splitmix64 */
static uint64_t mf2tRand_(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

#define MF2T_DIM 300

static size_t mf2tIndex_(uint64_t *s) {
    return mf2tRand_(s) % 64 == 0
               ? 40 + (size_t)(mf2tRand_(s) % (MF2T_DIM - 40))
               : (size_t)(mf2tRand_(s) % 40);
}

/* The integer value of 'box' (SIGNED_64 or UNSIGNED_64) into *v */
static bool mf2tInt_(const databox *box, __int128_t *v) {
    if (box->type == DATABOX_SIGNED_64) {
        *v = box->data.i64;
        return true;
    }

    if (box->type == DATABOX_UNSIGNED_64) {
        *v = box->data.u64;
        return true;
    }

    return false;
}

/* An exact integer result: the value, and SIGNED exactly when negative */
static bool mf2tIsInt_(const databox *box, __int128_t want) {
    __int128_t got;
    return mf2tInt_(box, &got) && got == want &&
           (box->type == DATABOX_SIGNED_64) == (want < 0);
}

/* The numeric value of any result box, as a double */
static double mf2tNum_(const databox *box) {
    switch (box->type) {
    case DATABOX_SIGNED_64:
        return (double)box->data.i64;
    case DATABOX_UNSIGNED_64:
        return (double)box->data.u64;
    case DATABOX_DOUBLE_64:
        return box->data.d64;
    default:
        return NAN;
    }
}

/* Random writes against a grid.  Integer runs check exact values and
 * types; mixed runs (small integers and quarter-step doubles, so every
 * sum is exact in double too) check values. */
static int mf2tDiff_(uint64_t seed, bool mixed) {
    int err = 0;
    cleaner(zfree) void *gridMem =
        zcalloc((size_t)MF2T_DIM * MF2T_DIM, sizeof(double));
    double *grid = gridMem;
    cleaner(zfree) void *igridMem =
        zcalloc((size_t)MF2T_DIM * MF2T_DIM, sizeof(__int128_t));
    __int128_t *igrid = igridMem;
    cleaner(multiFenwick2dFree) multiFenwick2d *t = NULL;
    size_t rows = 0;
    size_t cols = 0;
    for (size_t op = 0; op < 1500 && !err; op++) {
        const size_t r = mf2tIndex_(&seed);
        const size_t c = mf2tIndex_(&seed);
        const int64_t iv = (int64_t)(mf2tRand_(&seed) % 2001) - 1000;
        const bool asDouble = mixed && mf2tRand_(&seed) % 3 == 0;
        const double dv = (double)iv / 4.0;
        const databox v = asDouble ? DATABOX_DOUBLE(dv) : DATABOX_SIGNED(iv);
        const bool set = mf2tRand_(&seed) % 4 == 0;
        const bool ok = set ? multiFenwick2dSet(&t, r, c, &v)
                            : multiFenwick2dUpdate(&t, r, c, &v);
        if (!ok) {
            ERR("op %zu: write (%zu, %zu) refused", op, r, c);
            break;
        }

        double *g = &grid[r * MF2T_DIM + c];
        __int128_t *ig = &igrid[r * MF2T_DIM + c];
        *g = set ? (asDouble ? dv : (double)iv)
                 : *g + (asDouble ? dv : (double)iv);
        *ig = set ? iv : *ig + iv;
        rows = r + 1 > rows ? r + 1 : rows;
        cols = c + 1 > cols ? c + 1 : cols;
        if (multiFenwick2dRows(t) != rows || multiFenwick2dCols(t) != cols) {
            ERR("op %zu: extents wrong", op);
        }

        const bool whole = op % 128 == 127;
        const size_t r1 = whole ? 0 : mf2tIndex_(&seed);
        const size_t c1 = whole ? 0 : mf2tIndex_(&seed);
        const size_t r2 = whole ? MF2T_DIM : r1 + mf2tRand_(&seed) % 40;
        const size_t c2 = whole ? MF2T_DIM : c1 + mf2tRand_(&seed) % 40;
        double sum = 0;
        __int128_t isum = 0;
        for (size_t i = r1; i <= r2 && i < MF2T_DIM; i++) {
            for (size_t j = c1; j <= c2 && j < MF2T_DIM; j++) {
                sum += grid[i * MF2T_DIM + j];
                isum += igrid[i * MF2T_DIM + j];
            }
        }

        databox got;
        databox elem;
        const size_t gr = r1 < MF2T_DIM ? r1 : 0;
        const size_t gc = c1 < MF2T_DIM ? c1 : 0;
        if (!multiFenwick2dRangeQuery(t, r1, c1, r2, c2, &got) ||
            !multiFenwick2dGet(t, gr, gc, &elem)) {
            ERR("op %zu: a query failed", op);
        } else if (mixed) {
            if (mf2tNum_(&got) != sum ||
                mf2tNum_(&elem) != grid[gr * MF2T_DIM + gc]) {
                ERR("op %zu: RangeQuery(%zu,%zu,%zu,%zu) = %g, want %g", op, r1,
                    c1, r2, c2, mf2tNum_(&got), sum);
            }
        } else if (!mf2tIsInt_(&got, isum) ||
                   !mf2tIsInt_(&elem, igrid[gr * MF2T_DIM + gc])) {
            ERR("op %zu: RangeQuery(%zu,%zu,%zu,%zu) or Get wrong", op, r1, c1,
                r2, c2);
        }
    }

    for (size_t i = 0; i < rows && !err; i++) {
        for (size_t j = 0; j < cols && !err; j++) {
            databox e;
            if (!multiFenwick2dGet(t, i, j, &e) ||
                (mixed ? mf2tNum_(&e) != grid[i * MF2T_DIM + j]
                       : !mf2tIsInt_(&e, igrid[i * MF2T_DIM + j]))) {
                ERR("sweep: Get(%zu,%zu) wrong", i, j);
            }
        }
    }

    return err;
}

int multiFenwick2dTest(int argc, char *argv[]) {
    int err = 0;
    (void)argc;
    (void)argv;

    TEST("differential: exact integer results against an int128 grid") {
        for (uint64_t seed = 1; seed <= 3; seed++) {
            err += mf2tDiff_(seed, false);
        }
    }

    TEST("differential: mixed integer and double values") {
        for (uint64_t seed = 11; seed <= 13; seed++) {
            err += mf2tDiff_(seed, true);
        }
    }

    TEST("one column agrees with multiFenwickFlat") {
        uint64_t seed = 77;
        cleaner(multiFenwick2dFree) multiFenwick2d *t = NULL;
        cleaner(multiFenwickFlatFree) multiFenwickFlat *f = NULL;
        for (size_t op = 0; op < 2000 && !err; op++) {
            const size_t r = (size_t)(mf2tRand_(&seed) % 500);
            const databox v =
                mf2tRand_(&seed) % 2
                    ? DATABOX_SIGNED((int64_t)(mf2tRand_(&seed) % 99) - 49)
                    : DATABOX_UNSIGNED(mf2tRand_(&seed) >> 8);
            const bool a = multiFenwick2dUpdate(&t, r, 0, &v);
            const bool b = multiFenwickFlatUpdate(&f, r, &v);
            databox x;
            databox y;
            const size_t q = (size_t)(mf2tRand_(&seed) % 500);
            const bool qa = multiFenwick2dQuery(t, q, 0, &x);
            const bool qb = q < multiFenwickFlatCount(f)
                                ? multiFenwickFlatQuery(f, q, &y)
                                : multiFenwickFlatQuery(
                                      f, multiFenwickFlatCount(f) - 1, &y);
            if (a != b || qa != qb ||
                (qa && (x.type != y.type || x.data.u64 != y.data.u64))) {
                ERR("op %zu: 2D and 1D disagree", op);
            }
        }
    }

    TEST("refusals change nothing; a NULL tree stays NULL") {
        cleaner(multiFenwick2dFree) multiFenwick2d *t = NULL;
        const databox big = DATABOX_UNSIGNED(UINT64_MAX);
        const databox one = DATABOX_UNSIGNED(1);
        const databox nan = DATABOX_DOUBLE(NAN);
        const databox bytes = databoxNewBytesString("nope");
        if (multiFenwick2dUpdate(&t, 0, 0, &nan) ||
            multiFenwick2dUpdate(&t, 0, 0, &bytes) ||
            multiFenwick2dUpdate(&t, (size_t)1 << 32, 0, &one) ||
            multiFenwick2dUpdate(&t, (size_t)1 << 20, (size_t)1 << 20, &one) ||
            t != NULL) {
            ERR("a refused write was accepted or left a tree");
        }

        /* (0, 0) = UINT64_MAX fills every cell covering it; adding 1 at
         * (1, 1) would push cell (2, 2), which covers both, past it */
        if (!multiFenwick2dUpdate(&t, 0, 0, &big)) {
            ERR("UINT64_MAX refused");
        }

        const size_t bytesBefore = multiFenwick2dBytes(t);
        databox r;
        if (multiFenwick2dUpdate(&t, 1, 1, &one) ||
            multiFenwick2dBytes(t) != bytesBefore ||
            multiFenwick2dRows(t) != 1 || multiFenwick2dCols(t) != 1 ||
            !multiFenwick2dGet(t, 0, 0, &r) || r.data.u64 != UINT64_MAX) {
            ERR("an out-of-range cell was accepted or changed the tree");
        }
    }

    TEST("storage: 8 bytes per int64 cell, 9 with kinds, 8 after Clear") {
        cleaner(multiFenwick2dFree) multiFenwick2d *t = multiFenwick2dNew();
        const size_t base = multiFenwick2dBytes(t);
        const databox three = DATABOX_SIGNED(3);
        const databox half = DATABOX_DOUBLE(0.5);
        const databox big = DATABOX_UNSIGNED(UINT64_MAX);
        databox r;

        /* (5, 2): capacity 8 x 4 */
        if (!multiFenwick2dUpdate(&t, 5, 2, &three) ||
            !multiFenwick2dUpdate(&t, 1, 1, &three) ||
            multiFenwick2dBytes(t) != base + 8 * 32) {
            ERR("int64 cells: %zu bytes, want %zu", multiFenwick2dBytes(t),
                base + 8 * (size_t)32);
        }

        if (!multiFenwick2dUpdate(&t, 0, 0, &half) ||
            multiFenwick2dBytes(t) != base + 9 * 32 ||
            !multiFenwick2dQuery(t, 7, 3, &r) || r.type != DATABOX_DOUBLE_64 ||
            r.data.d64 != 6.5 || !multiFenwick2dGet(t, 5, 2, &r) ||
            !mf2tIsInt_(&r, 3)) {
            ERR("adding a double lost values or did not add the kinds");
        }

        /* A cell above INT64_MAX also needs kinds; Clear drops them */
        multiFenwick2dClear(t);
        if (multiFenwick2dBytes(t) != base + 8 * 32 ||
            !multiFenwick2dQuery(t, 7, 3, &r) || !mf2tIsInt_(&r, 0) ||
            !multiFenwick2dUpdate(&t, 2, 0, &big) ||
            multiFenwick2dBytes(t) != base + 9 * 32 ||
            !multiFenwick2dGet(t, 2, 0, &r) || !mf2tIsInt_(&r, UINT64_MAX)) {
            ERR("Clear or an unsigned cell mishandled the kinds array");
        }
    }

    TEST("exact sums past 64 bits: refused cells, unreturnable prefixes") {
        const __int128_t u = UINT64_MAX;
        const __int128_t m = INT64_MIN;
        const databox big = DATABOX_UNSIGNED(UINT64_MAX);
        const databox min = DATABOX_SIGNED(INT64_MIN);
        databox r;

        /* One element: Set across the whole 64-bit range, a difference
         * that needs more than 64 bits, is exact */
        cleaner(multiFenwick2dFree) multiFenwick2d *one = NULL;
        if (!multiFenwick2dSet(&one, 0, 0, &big) ||
            !multiFenwick2dSet(&one, 0, 0, &min) ||
            !multiFenwick2dGet(one, 0, 0, &r) || !mf2tIsInt_(&r, m) ||
            !multiFenwick2dSet(&one, 0, 0, &big) ||
            !multiFenwick2dGet(one, 0, 0, &r) || !mf2tIsInt_(&r, u)) {
            ERR("exact Set across the 64-bit range failed");
        }

        /* e[0][0] = U, e[3][3] = M, e[3][2] = M, e[2][2] = U, written in an
         * order that keeps every cell in range.  Every cell then fits and
         * the total (2U + 2M = U - 1) fits, but the prefix through (2, 2)
         * is 2U, and the rectangle [2, 3] x [2, 3] is U + 2M = -1. */
        cleaner(multiFenwick2dFree) multiFenwick2d *t = NULL;
        if (!multiFenwick2dUpdate(&t, 0, 0, &big) ||
            !multiFenwick2dUpdate(&t, 3, 3, &min) ||
            !multiFenwick2dUpdate(&t, 3, 2, &min) ||
            !multiFenwick2dUpdate(&t, 2, 2, &big)) {
            ERR("in-range cells refused");
        }

        if (!multiFenwick2dQuery(t, 3, 3, &r) || !mf2tIsInt_(&r, u - 1) ||
            multiFenwick2dQuery(t, 2, 2, &r) || r.type != DATABOX_VOID ||
            !multiFenwick2dRangeQuery(t, 2, 2, 3, 3, &r) ||
            !mf2tIsInt_(&r, u + 2 * m) || !multiFenwick2dGet(t, 3, 2, &r) ||
            !mf2tIsInt_(&r, m)) {
            ERR("exact sums past 64 bits mishandled");
        }

        /* Setting e[0][0] to M needs cell (4, 4) at U - 1 - U + M, below
         * INT64_MIN: refused, and nothing changes */
        if (multiFenwick2dSet(&t, 0, 0, &min) ||
            !multiFenwick2dGet(t, 0, 0, &r) || !mf2tIsInt_(&r, u) ||
            !multiFenwick2dQuery(t, 3, 3, &r) || !mf2tIsInt_(&r, u - 1)) {
            ERR("an out-of-range Set was accepted or changed the tree");
        }
    }

    TEST_FINAL_RESULT;
}
