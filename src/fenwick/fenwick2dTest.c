/* Tests for the typed 2D Fenwick trees (fenwick2d.h): every type against a
 * dense grid oracle, through growth in rows, columns and both. */

#include "fenwick2d.h"
#include "../cleaner.h"
#include "../ctest.h"
#include "../datakit.h"
#include "fenwickPersist.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(void *, zfree);

/* The oracle grid, row-major */
static constexpr int F2T_DIM = 400;

/* An index: mostly inside a 48-square, sometimes far out (growth) */
static size_t f2tIndex_(uint64_t *s) {
    return dkSplitmix64Next(s) % 64 == 0
               ? 48 + (size_t)(dkSplitmix64Next(s) % (F2T_DIM - 48))
               : (size_t)(dkSplitmix64Next(s) % 48);
}

/* Differential run for one type.  Integer prefix sums are taken in the
 * same-width unsigned type (the sum modulo 2^N, the documented result
 * when it does not fit); float values are small integers, so every sum is
 * exact in both float types. */
#define F2T_DIFF(T, VT, UT, VMIN, VMAX, IS_FLOAT, FITS_ADD)                    \
    static int f2tDiff##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        cleaner(zfree) void *gridMem =                                         \
            zcalloc((size_t)F2T_DIM * F2T_DIM, sizeof(VT));                    \
        VT *grid = gridMem;                                                    \
        size_t rows = 0;                                                       \
        size_t cols = 0;                                                       \
        fenwick2d##T *t = nullptr;                                             \
        for (size_t op = 0; op < 1500 && !err; op++) {                         \
            const size_t r = f2tIndex_(&seed);                                 \
            const size_t c = f2tIndex_(&seed);                                 \
            VT v = (VT)((int64_t)(dkSplitmix64Next(&seed) % 41) - 20);         \
            if ((VT) - 1 > 0) {                                                \
                v = (VT)(dkSplitmix64Next(&seed) % 21);                        \
            }                                                                  \
            if (!(IS_FLOAT) && dkSplitmix64Next(&seed) % 16 == 0) {            \
                v = dkSplitmix64Next(&seed) % 2 ? (VT)(VMAX) : (VT)(VMIN);     \
            }                                                                  \
            VT *cell = &grid[r * F2T_DIM + c];                                 \
            if (dkSplitmix64Next(&seed) % 4 == 0) {                            \
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
            const size_t r2 =                                                  \
                whole ? F2T_DIM : r1 + dkSplitmix64Next(&seed) % 40;           \
            const size_t c2 =                                                  \
                whole ? F2T_DIM : c1 + dkSplitmix64Next(&seed) % 40;           \
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
            ERR(#T ": Clear did not zero the grid and keep the extents");      \
        }                                                                      \
        fenwick2d##T##Free(t);                                                 \
        return err;                                                            \
    }

/* a + b into *out; false when it does not fit the type */
#define F2T_INT_FITS(a, b, out) (!ckd_add(out, a, b))
#define F2T_FLOAT_FITS(a, b, out) (*(out) = (a) + (b), true)

/* Per-type edges: a NULL tree is empty; refusals change nothing and
 * leave a NULL tree NULL; an element at the type's limit refuses one
 * more step (for floats, a step to infinity); cells are sizeof(VT) each,
 * so one row doubling of a 4 x 4 capacity adds 16 cells. */
#define F2T_EDGES(T, VT, VMAX, IS_FLOAT)                                       \
    static int f2tEdges##T(void) {                                             \
        int err = 0;                                                           \
        if (fenwick2d##T##Rows(nullptr) != 0 ||                                \
            fenwick2d##T##Cols(nullptr) != 0 ||                                \
            fenwick2d##T##Bytes(nullptr) != 0 ||                               \
            fenwick2d##T##Get(nullptr, 0, 0) != 0 ||                           \
            fenwick2d##T##Query(nullptr, 5, 5) != 0 ||                         \
            fenwick2d##T##RangeQuery(nullptr, 0, 0, 5, 5) != 0 ||              \
            fenwick2d##T##Update(nullptr, 0, 0, 1)) {                          \
            ERR(#T ": a NULL tree is not an empty tree");                      \
        }                                                                      \
        fenwick2d##T##Clear(nullptr);                                          \
        fenwick2d##T##Free(nullptr);                                           \
                                                                               \
        fenwick2d##T *t = nullptr;                                             \
        if (fenwick2d##T##Update(&t, (size_t)1 << 32, 0, 1) ||                 \
            fenwick2d##T##Update(&t, (size_t)1 << 20, (size_t)1 << 20, 1) ||   \
            t != nullptr) {                                                    \
            ERR(#T ": a write past 2^32 cells was accepted or left a tree");   \
        }                                                                      \
                                                                               \
        const VT step = (IS_FLOAT) ? (VT)(VMAX) : (VT)1;                       \
        if (!fenwick2d##T##Set(&t, 3, 2, (VT)(VMAX)) ||                        \
            fenwick2d##T##Update(&t, 3, 2, step) ||                            \
            fenwick2d##T##Get(t, 3, 2) != (VT)(VMAX) ||                        \
            fenwick2d##T##Rows(t) != 4 || fenwick2d##T##Cols(t) != 3) {        \
            ERR(#T ": an element overflow was accepted or changed the tree");  \
        }                                                                      \
                                                                               \
        const size_t bytes = fenwick2d##T##Bytes(t);                           \
        if (!fenwick2d##T##Update(&t, 6, 1, 1) ||                              \
            fenwick2d##T##Bytes(t) != bytes + 16 * sizeof(VT)) {               \
            ERR(#T ": Bytes %zu -> %zu, want +%zu for one row doubling",       \
                bytes, fenwick2d##T##Bytes(t), 16 * sizeof(VT));               \
        }                                                                      \
        fenwick2d##T##Free(t);                                                 \
        return err;                                                            \
    }

/* NewFromArray and CopyTo against the source grid and a tree built with
 * one Set per element, over shapes from empty to non-square and past a
 * power of two; float values are small integers (every sum exact) */
#define F2T_BULK(T, VT, IS_FLOAT)                                              \
    static int f2tBulk##T(uint64_t seed) {                                     \
        int err = 0;                                                           \
        static constexpr size_t shapes[][2] = {{0, 0}, {1, 1},   {1, 9},       \
                                               {7, 3}, {33, 65}, {64, 64}};    \
        for (size_t s = 0; s < countof(shapes); s++) {                         \
            const size_t rows = shapes[s][0];                                  \
            const size_t cols = shapes[s][1];                                  \
            const size_t n = rows * cols;                                      \
            cleaner(zfree) void *bufA = zcalloc(n + 1, sizeof(VT));            \
            cleaner(zfree) void *bufB = zcalloc(n + 1, sizeof(VT));            \
            VT *values = bufA;                                                 \
            VT *copied = bufB;                                                 \
            fenwick2d##T *bySet = nullptr;                                     \
            for (size_t i = 0; i < n; i++) {                                   \
                const uint64_t r = dkSplitmix64Next(&seed);                    \
                values[i] = IS_FLOAT ? (VT)((int64_t)(r % 21) - 10) : (VT)r;   \
                (void)fenwick2d##T##Set(&bySet, i / cols, i % cols,            \
                                        values[i]);                            \
            }                                                                  \
                                                                               \
            fenwick2d##T *t =                                                  \
                fenwick2d##T##NewFromArray(n ? values : nullptr, rows, cols);  \
            fenwick2d##T##CopyTo(t, copied);                                   \
            if (!t || fenwick2d##T##Rows(t) != rows ||                         \
                fenwick2d##T##Cols(t) != cols ||                               \
                memcmp(copied, values, n * sizeof(VT)) != 0) {                 \
                ERR(#T ": NewFromArray/CopyTo of %zu x %zu", rows, cols);      \
            }                                                                  \
                                                                               \
            for (size_t i = 0; i < n; i += 1 + n / 40) {                       \
                const size_t r = i / cols;                                     \
                const size_t c = i % cols;                                     \
                if (fenwick2d##T##Get(t, r, c) != values[i] ||                 \
                    fenwick2d##T##Query(t, r, c) !=                            \
                        fenwick2d##T##Query(bySet, r, c)) {                    \
                    ERR(#T ": (%zu, %zu) of %zu x %zu differs from the Set "   \
                           "tree",                                             \
                        r, c, rows, cols);                                     \
                    break;                                                     \
                }                                                              \
            }                                                                  \
                                                                               \
            fenwick2d##T##Free(t);                                             \
            fenwick2d##T##Free(bySet);                                         \
        }                                                                      \
                                                                               \
        return err;                                                            \
    }

#define F2T_TYPE(T, VT, UT, VMIN, VMAX, IS_FLOAT, FITS_ADD)                    \
    F2T_DIFF(T, VT, UT, VMIN, VMAX, IS_FLOAT, FITS_ADD)                        \
    F2T_EDGES(T, VT, VMAX, IS_FLOAT)                                           \
    F2T_BULK(T, VT, IS_FLOAT)

F2T_TYPE(I16, int16_t, uint16_t, INT16_MIN, INT16_MAX, 0, F2T_INT_FITS)
F2T_TYPE(I32, int32_t, uint32_t, INT32_MIN, INT32_MAX, 0, F2T_INT_FITS)
F2T_TYPE(I64, int64_t, uint64_t, INT64_MIN, INT64_MAX, 0, F2T_INT_FITS)
F2T_TYPE(U16, uint16_t, uint16_t, 0, UINT16_MAX, 0, F2T_INT_FITS)
F2T_TYPE(U32, uint32_t, uint32_t, 0, UINT32_MAX, 0, F2T_INT_FITS)
F2T_TYPE(U64, uint64_t, uint64_t, 0, UINT64_MAX, 0, F2T_INT_FITS)
F2T_TYPE(Float, float, float, -FLT_MAX, FLT_MAX, 1, F2T_FLOAT_FITS)
F2T_TYPE(Double, double, double, -DBL_MAX, DBL_MAX, 1, F2T_FLOAT_FITS)
#ifdef __SIZEOF_INT128__
#define F2T_I128_MAX ((__int128_t)(((__uint128_t)1 << 127) - 1))
#define F2T_U128_MAX (~(__uint128_t)0)
F2T_TYPE(I128, __int128_t, __uint128_t, -F2T_I128_MAX - 1, F2T_I128_MAX, 0,
         F2T_INT_FITS)
F2T_TYPE(U128, __uint128_t, __uint128_t, 0, F2T_U128_MAX, 0, F2T_INT_FITS)
#endif

int fenwick2dTest(int, char *[]) {
    int err = 0;

    TEST("differential: every fenwick2d type against a dense grid") {
        for (uint64_t seed = 1; seed <= 3; seed++) {
            err += f2tDiffI16(seed);
            err += f2tDiffI32(seed);
            err += f2tDiffI64(seed);
            err += f2tDiffU16(seed);
            err += f2tDiffU32(seed);
            err += f2tDiffU64(seed);
            err += f2tDiffFloat(seed);
            err += f2tDiffDouble(seed);
#ifdef __SIZEOF_INT128__
            err += f2tDiffI128(seed);
            err += f2tDiffU128(seed);
#endif
        }
    }

    TEST("every type: NULL trees, refusals, limits, cell size") {
        err += f2tEdgesI16();
        err += f2tEdgesI32();
        err += f2tEdgesI64();
        err += f2tEdgesU16();
        err += f2tEdgesU32();
        err += f2tEdgesU64();
        err += f2tEdgesFloat();
        err += f2tEdgesDouble();
#ifdef __SIZEOF_INT128__
        err += f2tEdgesI128();
        err += f2tEdgesU128();
#endif
    }

    TEST("every type: NewFromArray and CopyTo") {
        err += f2tBulkI16(21);
        err += f2tBulkI32(22);
        err += f2tBulkI64(23);
        err += f2tBulkU16(24);
        err += f2tBulkU32(25);
        err += f2tBulkU64(26);
        err += f2tBulkFloat(27);
        err += f2tBulkDouble(28);
#ifdef __SIZEOF_INT128__
        err += f2tBulkI128(29);
        err += f2tBulkU128(30);
#endif
    }

    TEST("float trees refuse non-finite values; a NULL tree stays NULL") {
        fenwick2dFloat *f = nullptr;
        if (fenwick2dFloatUpdate(&f, 0, 0, NAN) ||
            fenwick2dFloatSet(&f, 0, 0, INFINITY) || f != nullptr) {
            ERR("a non-finite float was accepted or left a tree");
        }

        fenwick2dDouble *d = nullptr;
        if (fenwick2dDoubleUpdate(&d, 0, 0, NAN) ||
            fenwick2dDoubleSet(&d, 0, 0, INFINITY) || d != nullptr) {
            ERR("a non-finite double was accepted or left a tree");
        }
    }

    TEST("persist: every 2D tree logs and replays only its own ops") {
        /* Update, Set and Clear are logged; any other op, or an update
         * without its value, is neither encoded nor applied */
        const persistOps *const all[] = {
            &persistOpsFenwick2dI16,   &persistOpsFenwick2dI32,
            &persistOpsFenwick2dI64,   &persistOpsFenwick2dU16,
            &persistOpsFenwick2dU32,   &persistOpsFenwick2dU64,
            &persistOpsFenwick2dFloat, &persistOpsFenwick2dDouble,
#ifdef __SIZEOF_INT128__
            &persistOpsFenwick2dI128,  &persistOpsFenwick2dU128,
#endif
        };
        const databox one = databoxNewUnsigned(1);
        const databox *pair[] = {&one, &one};
        const databox *three[] = {&one, &one, &one};
        size_t bodyLen = 0;
        cleaner(zfree) void *body = persistOpEncodeBoxes(three, 3, &bodyLen);
        for (size_t i = 0; i < countof(all); i++) {
            size_t len = 0;
            void *t = nullptr;
            if (all[i]->encodeOp(PERSIST_OP_DELETE, three, 3, &len) ||
                all[i]->encodeOp(PERSIST_OP_UPDATE, pair, 2, &len) ||
                all[i]->applyOp(&t, PERSIST_OP_DELETE, body, bodyLen) || t) {
                ERR("%s: persist took an op the tree does not log",
                    all[i]->name);
            }
        }
    }

    TEST("growth: a write far past the tree's size is refused") {
        /* 2048 x 2048 is 2^22 cells, taken from anywhere; 4096 x 2048 from
         * a 4 x 4 tree is more than one doubling of the rows */
        fenwick2dI64 *t = nullptr;
        if (!fenwick2dI64Update(&t, 3, 3, 1)) {
            ERR("a first write was refused");
        }
        const size_t bytes = fenwick2dI64Bytes(t);
        if (fenwick2dI64Update(&t, 4096, 2047, 1) ||
            fenwick2dI64Update(&t, (size_t)1 << 30, 0, 1) ||
            fenwick2dI64Bytes(t) != bytes || fenwick2dI64Rows(t) != 4) {
            ERR("a far write was taken or changed the tree");
        }
        if (!fenwick2dI64Update(&t, 2047, 2047, 2) ||
            fenwick2dI64Query(t, 2047, 2047) != 3) {
            ERR("a write at 2047 x 2047 was refused or lost a value");
        }
        fenwick2dI64Free(t);

        /* Past 2^22 cells each side may double, no more: 2048 x 2048 to
         * 4096 x 2048 is taken; then 8192 x 8192 (the columns four times
         * over) is refused, as is 8 x 2^20 from 4 x 4 */
        fenwick2dI16 *s = nullptr;
        if (!fenwick2dI16Update(&s, 2047, 2047, 1) ||
            !fenwick2dI16Update(&s, 4095, 2047, 2) ||
            fenwick2dI16Update(&s, 8191, 8191, 4) ||
            fenwick2dI16Query(s, 4095, 2047) != 3) {
            ERR("a doubling past 2^22 cells was refused, or more was taken");
        }
        fenwick2dI16Free(s);
        fenwick2dI16 *narrow = nullptr;
        if (!fenwick2dI16Update(&narrow, 3, 3, 1) ||
            fenwick2dI16Update(&narrow, 7, (size_t)1 << 20, 1) ||
            fenwick2dI16Rows(narrow) != 4) {
            ERR("a write doubling the rows and jumping the columns was taken");
        }
        fenwick2dI16Free(narrow);
    }

    TEST_FINAL_RESULT;
}
