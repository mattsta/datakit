/* fenwickContractTest.c - the typed Fenwick trees (persistOpsFenwick<T>)
 * in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../fenwick/fenwickDouble.h"
#include "../../fenwick/fenwickFloat.h"
#include "../../fenwick/fenwickI128.h"
#include "../../fenwick/fenwickI16.h"
#include "../../fenwick/fenwickI32.h"
#include "../../fenwick/fenwickI64.h"
#include "../../fenwick/fenwickPersist.h"
#include "../../fenwick/fenwickU128.h"
#include "../../fenwick/fenwickU16.h"
#include "../../fenwick/fenwickU32.h"
#include "../../fenwick/fenwickU64.h"
#include "../../str.h"

#include <string.h>

/* A box for a value of an integer type of 'bytes' bytes */
static void pcFenwickIntBox(databoxBig *out, bool isSigned, size_t bytes,
                            __uint128_t bits) {
    memset(out, 0, sizeof(*out));
    if (bytes == 16) {
        DATABOX_BIG_INIT(out);
        out->type = isSigned ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
        *out->data.u128 = bits;
    } else if (isSigned) {
        out->type = DATABOX_SIGNED_64;
        out->data.i64 = (int64_t)(uint64_t)bits;
    } else {
        out->type = DATABOX_UNSIGNED_64;
        out->data.u64 = (uint64_t)bits;
    }
}

/* Mostly small values (deltas that keep elements in range), sometimes
 * the full width of the type (so some Updates are refused) */
static __uint128_t pcFenwickBits(uint64_t *rng) {
    const uint64_t roll = splitmix64(rng) % 8;
    const __uint128_t wide =
        ((__uint128_t)splitmix64(rng) << 64) | splitmix64(rng);
    if (roll == 0) {
        return wide;
    }

    const int64_t small = (int64_t)(splitmix64(rng) % 2001) - 1000;
    return (__uint128_t)(__int128_t)small;
}

/* A float: mostly modest, sometimes large enough that cells round */
static double pcFenwickReal(uint64_t *rng) {
    const double unit =
        (double)(splitmix64(rng) >> 11) / (double)(UINT64_C(1) << 53);
    const double scale = splitmix64(rng) % 8 == 0 ? 1e15 : 1e3;
    return (unit - 0.5) * scale;
}

/* T: suffix; C: C type; FLOAT: 1 for float types */
#define PC_FENWICK(T, C, FLOAT)                                                \
    static void *pcFenwick##T##Build(const persistContractBuild *b) {          \
        void *fw = fenwick##T##New();                                          \
        persistContractStart(b->log, fw);                                      \
                                                                               \
        uint64_t rng = b->seed;                                                \
        const size_t span = b->size + 1;                                       \
        for (size_t i = 0; i < b->size; i++) {                                 \
            const bool isUpdate = splitmix64(&rng) % 8 < 5;                    \
            const size_t idx = (size_t)(splitmix64(&rng) % span);              \
            const databox ib = DATABOX_UNSIGNED(idx);                          \
            databoxBig vb;                                                     \
            C v;                                                               \
            if (FLOAT) {                                                       \
                v = (C)pcFenwickReal(&rng);                                    \
                memset(&vb, 0, sizeof(vb));                                    \
                if (sizeof(C) == sizeof(float)) {                              \
                    vb.type = DATABOX_FLOAT_32;                                \
                    vb.data.f32 = (float)v;                                    \
                } else {                                                       \
                    vb.type = DATABOX_DOUBLE_64;                               \
                    vb.data.d64 = (double)v;                                   \
                }                                                              \
            } else {                                                           \
                const __uint128_t bits = pcFenwickBits(&rng);                  \
                v = (C)bits;                                                   \
                pcFenwickIntBox(&vb, (C) - 1 < (C)0, sizeof(C),                \
                                (__uint128_t)v);                               \
            }                                                                  \
                                                                               \
            const bool ok = isUpdate ? fenwick##T##Update(&fw, idx, v)         \
                                     : fenwick##T##Set(&fw, idx, v);           \
            if (ok) {                                                          \
                const databox *args[] = {&ib, (const databox *)&vb};           \
                persistContractRecord(b->log,                                  \
                                      isUpdate ? PERSIST_OP_NUM_INCR           \
                                               : PERSIST_OP_UPDATE,            \
                                      args, 2);                                \
            }                                                                  \
                                                                               \
            if (b->size >= 100 && i == b->size / 3) {                          \
                fenwick##T##Clear(fw);                                         \
                persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);      \
            }                                                                  \
        }                                                                      \
                                                                               \
        return fw;                                                             \
    }                                                                          \
                                                                               \
    const persistContractType persistContractFenwick##T = {                    \
        .ops = &persistOpsFenwick##T,                                          \
        .build = pcFenwick##T##Build,                                          \
    };

PC_FENWICK(I16, int16_t, 0)
PC_FENWICK(I32, int32_t, 0)
PC_FENWICK(I64, int64_t, 0)
PC_FENWICK(U16, uint16_t, 0)
PC_FENWICK(U32, uint32_t, 0)
PC_FENWICK(U64, uint64_t, 0)
PC_FENWICK(Float, float, 1)
PC_FENWICK(Double, double, 1)
PC_FENWICK(I128, __int128_t, 0)
PC_FENWICK(U128, __uint128_t, 0)

#endif /* DATAKIT_TEST */
