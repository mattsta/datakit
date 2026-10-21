/* Tests for the typed 2D Fenwick trees (fenwick2d.h): every type against a
 * dense grid oracle, through growth in rows, columns and both. */

#include "fenwick2d.h"
#include "../cleaner.h"
#include "../ctest.h"
#include "../datakit.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

/* splitmix64 */
static uint64_t f2tRand_(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* The oracle grid, row-major */
#define F2T_DIM 400

/* An index: mostly inside a 48-square, sometimes far out (growth) */
static size_t f2tIndex_(uint64_t *s) {
    return f2tRand_(s) % 64 == 0 ? 48 + (size_t)(f2tRand_(s) % (F2T_DIM - 48))
                                 : (size_t)(f2tRand_(s) % 48);
}

/* Differential run for one type.  Integer prefix sums are taken in the
 * unsigned type (the sum modulo 2^64, the documented result when it does
 * not fit); Double values are small integers, so every sum is exact. */
#define F2T_DIFF(T, VT, UT, VMIN, VMAX, IS_FLOAT, FITS_ADD)                    \
    static int f2tDiff##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        cleaner(zfree) void *gridMem =                                         \
            zcalloc((size_t)F2T_DIM * F2T_DIM, sizeof(VT));                    \
        VT *grid = gridMem;                                                    \
        size_t rows = 0;                                                       \
        size_t cols = 0;                                                       \
        fenwick2d##T *t = NULL;                                                \
        for (size_t op = 0; op < 1500 && !err; op++) {                         \
            const size_t r = f2tIndex_(&seed);                                 \
            const size_t c = f2tIndex_(&seed);                                 \
            VT v = (VT)((int64_t)(f2tRand_(&seed) % 41) - 20);                 \
            if ((VT) - 1 > 0) {                                                \
                v = (VT)(f2tRand_(&seed) % 21);                                \
            }                                                                  \
            if (!(IS_FLOAT) && f2tRand_(&seed) % 16 == 0) {                    \
                v = f2tRand_(&seed) % 2 ? (VT)(VMAX) : (VT)(VMIN);             \
            }                                                                  \
            VT *cell = &grid[r * F2T_DIM + c];                                 \
            if (f2tRand_(&seed) % 4 == 0) {                                    \
                if (!fenwick2d##T##Set(&t, r, c, v)) {                         \
                    ERR(#T " op %zu: Set(%zu, %zu) refused", op, r, c);        \
                }                                                              \
                *cell = v;                                                     \
            } else {                                                           \
                VT want;                                                       \
                const bool fits = FITS_ADD(*cell, v, &want);                   \
                if (fenwick2d##T##Update(&t, r, c, v) != fits) {               \
                    ERR(#T " op %zu: Update(%zu, %zu) %s", op, r, c,           \
                        fits ? "refused"                                       \
                             : "accepted an out-of-range element");            \
                }                                                              \
                if (!fits) {                                                   \
                    continue;                                                  \
                }                                                              \
                *cell = want;                                                  \
            }                                                                  \
            rows = r + 1 > rows ? r + 1 : rows;                                \
            cols = c + 1 > cols ? c + 1 : cols;                                \
            if (fenwick2d##T##Rows(t) != rows ||                               \
                fenwick2d##T##Cols(t) != cols) {                               \
                ERR(#T " op %zu: extents %zux%zu, want %zux%zu", op,           \
                    fenwick2d##T##Rows(t), fenwick2d##T##Cols(t), rows, cols); \
            }                                                                  \
                                                                               \
            /* A rectangle (sometimes reaching past the extents) and an        \
             * element; the whole grid now and then */                         \
            const bool whole = op % 128 == 127;                                \
            const size_t r1 = whole ? 0 : f2tIndex_(&seed);                    \
            const size_t c1 = whole ? 0 : f2tIndex_(&seed);                    \
            const size_t r2 = whole ? F2T_DIM : r1 + f2tRand_(&seed) % 40;     \
            const size_t c2 = whole ? F2T_DIM : c1 + f2tRand_(&seed) % 40;     \
            UT sum = 0;                                                        \
            VT fsum = 0;                                                       \
            for (size_t i = r1; i <= r2 && i < F2T_DIM; i++) {                 \
                for (size_t j = c1; j <= c2 && j < F2T_DIM; j++) {             \
                    if (IS_FLOAT) {                                            \
                        fsum += grid[i * F2T_DIM + j];                         \
                    } else {                                                   \
                        sum += (UT)grid[i * F2T_DIM + j];                      \
                    }                                                          \
                }                                                              \
            }                                                                  \
            const VT want = (IS_FLOAT) ? fsum : (VT)sum;                       \
            if (fenwick2d##T##RangeQuery(t, r1, c1, r2, c2) != want) {         \
                ERR(#T " op %zu: RangeQuery(%zu,%zu,%zu,%zu) wrong", op, r1,   \
                    c1, r2, c2);                                               \
            }                                                                  \
            if (r1 == 0 && c1 == 0 &&                                          \
                fenwick2d##T##Query(t, r2, c2) != want) {                      \
                ERR(#T " op %zu: Query(%zu,%zu) wrong", op, r2, c2);           \
            }                                                                  \
            if (fenwick2d##T##Get(t, r1, c1) != (r1 < F2T_DIM && c1 < F2T_DIM  \
                                                     ? grid[r1 * F2T_DIM + c1] \
                                                     : (VT)0)) {               \
                ERR(#T " op %zu: Get(%zu,%zu) wrong", op, r1, c1);             \
            }                                                                  \
        }                                                                      \
                                                                               \
        for (size_t i = 0; i < rows && !err; i++) {                            \
            for (size_t j = 0; j < cols && !err; j++) {                        \
                if (fenwick2d##T##Get(t, i, j) != grid[i * F2T_DIM + j]) {     \
                    ERR(#T " sweep: Get(%zu,%zu) wrong", i, j);                \
                }                                                              \
            }                                                                  \
        }                                                                      \
        fenwick2d##T##Clear(t);                                                \
        if (fenwick2d##T##Rows(t) != rows ||                                   \
            fenwick2d##T##Query(t, F2T_DIM, F2T_DIM) != 0 ||                   \
            fenwick2d##T##Get(t, 0, 0) != 0) {                                 \
            ERRR(#T ": Clear did not zero the grid and keep the extents");     \
        }                                                                      \
        fenwick2d##T##Free(t);                                                 \
        return err;                                                            \
    }

/* a + b into *out; false when it does not fit the type */
#define F2T_INT_FITS(a, b, out) (!__builtin_add_overflow(a, b, out))
#define F2T_DOUBLE_FITS(a, b, out) (*(out) = (a) + (b), true)

F2T_DIFF(I64, int64_t, uint64_t, INT64_MIN, INT64_MAX, 0, F2T_INT_FITS)
F2T_DIFF(U64, uint64_t, uint64_t, 0, UINT64_MAX, 0, F2T_INT_FITS)
F2T_DIFF(Double, double, double, -DBL_MAX, DBL_MAX, 1, F2T_DOUBLE_FITS)

int fenwick2dTest(int argc, char *argv[]) {
    int err = 0;
    (void)argc;
    (void)argv;

    TEST("differential: fenwick2d I64, U64, Double against a dense grid") {
        for (uint64_t seed = 1; seed <= 3; seed++) {
            err += f2tDiffI64(seed);
            err += f2tDiffU64(seed);
            err += f2tDiffDouble(seed);
        }
    }

    TEST("NULL and empty trees") {
        if (fenwick2dI64Rows(NULL) != 0 || fenwick2dI64Bytes(NULL) != 0 ||
            fenwick2dI64Get(NULL, 0, 0) != 0 ||
            fenwick2dI64Query(NULL, 5, 5) != 0 ||
            fenwick2dI64RangeQuery(NULL, 0, 0, 5, 5) != 0 ||
            fenwick2dI64Update(NULL, 0, 0, 1)) {
            ERRR("NULL tree is not an empty tree");
        }

        fenwick2dI64Clear(NULL);
        fenwick2dI64Free(NULL);
    }

    TEST("refusals change nothing; a NULL tree stays NULL") {
        fenwick2dI64 *t = NULL;
        if (fenwick2dI64Update(&t, (size_t)1 << 32, 0, 1) ||
            fenwick2dI64Update(&t, (size_t)1 << 20, (size_t)1 << 20, 1) ||
            t != NULL) {
            ERRR("a write past 2^32 cells was accepted or left a tree");
        }

        fenwick2dDouble *d = NULL;
        if (fenwick2dDoubleUpdate(&d, 0, 0, NAN) ||
            fenwick2dDoubleSet(&d, 0, 0, INFINITY) || d != NULL) {
            ERRR("a non-finite double was accepted or left a tree");
        }

        if (!fenwick2dI64Update(&t, 3, 2, INT64_MAX) ||
            fenwick2dI64Update(&t, 3, 2, 1) ||
            fenwick2dI64Get(t, 3, 2) != INT64_MAX || fenwick2dI64Rows(t) != 4 ||
            fenwick2dI64Cols(t) != 3) {
            ERRR("an element overflow was accepted or changed the tree");
        }

        /* Bytes: the struct plus 8 bytes per capacity cell (4 x 4) */
        const size_t bytes = fenwick2dI64Bytes(t);
        if (!fenwick2dI64Update(&t, 6, 1, 1) ||
            fenwick2dI64Bytes(t) != bytes + 8 * 16) {
            ERR("Bytes %zu -> %zu, want +128 for one row doubling", bytes,
                fenwick2dI64Bytes(t));
        }

        fenwick2dI64Free(t);
    }

    TEST_FINAL_RESULT;
}
