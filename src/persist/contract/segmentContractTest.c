/* segmentContractTest.c - the typed segment trees (persistOpsSegment<T>)
 * in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../segment/segmentDouble.h"
#include "../../segment/segmentFloat.h"
#include "../../segment/segmentI128.h"
#include "../../segment/segmentI16.h"
#include "../../segment/segmentI32.h"
#include "../../segment/segmentI64.h"
#include "../../segment/segmentPersist.h"
#include "../../segment/segmentU128.h"
#include "../../segment/segmentU16.h"
#include "../../segment/segmentU32.h"
#include "../../segment/segmentU64.h"
#include "../../str.h"

#include <string.h>

/* A value of a C type in a box the descriptors take: integers over their
 * full width (narrowed to the type), floats modest or large */
static void pcSegmentBox(databoxBig *out, bool isFloat, bool isSigned,
                         size_t bytes, uint64_t *rng, __uint128_t *bits,
                         double *real) {
    memset(out, 0, sizeof(*out));
    if (isFloat) {
        const double unit =
            (double)(splitmix64(rng) >> 11) / (double)(UINT64_C(1) << 53);
        *real = (unit - 0.5) * (splitmix64(rng) % 8 == 0 ? 1e15 : 1e3);
        if (bytes == sizeof(float)) {
            out->type = DATABOX_FLOAT_32;
            out->data.f32 = (float)*real;
            *real = out->data.f32;
        } else {
            out->type = DATABOX_DOUBLE_64;
            out->data.d64 = *real;
        }

        return;
    }

    *bits = ((__uint128_t)splitmix64(rng) << 64) | splitmix64(rng);
    if (bytes < 16) {
        /* Narrow to the type, then sign-extend a signed one */
        const unsigned shift = (unsigned)(128 - 8 * bytes);
        *bits = isSigned
                    ? (__uint128_t)(((__int128_t)(*bits << shift)) >> shift)
                    : (*bits << shift) >> shift;
    }

    if (bytes == 16) {
        DATABOX_BIG_INIT(out);
        out->type = isSigned ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
        *out->data.u128 = *bits;
    } else if (isSigned) {
        out->type = DATABOX_SIGNED_64;
        out->data.i64 = (int64_t)(uint64_t)*bits;
    } else {
        out->type = DATABOX_UNSIGNED_64;
        out->data.u64 = (uint64_t)*bits;
    }
}

/* T: suffix; C: C type; FLOAT: 1 for float types */
#define PC_SEGMENT(T, C, FLOAT)                                                \
    static void *pcSegment##T##Build(const persistContractBuild *b) {          \
        void *seg = segment##T##New((segmentOp)(b->seed % 3));                 \
        persistContractStart(b->log, seg);                                     \
                                                                               \
        uint64_t rng = b->seed;                                                \
        const size_t span = b->size + 1;                                       \
        for (size_t i = 0; i < b->size; i++) {                                 \
            databoxBig vb;                                                     \
            __uint128_t bits = 0;                                              \
            double real = 0;                                                   \
            pcSegmentBox(&vb, FLOAT, (C) - 1 < (C)0, sizeof(C), &rng, &bits,   \
                         &real);                                               \
            const C v = FLOAT ? (C)real : (C)bits;                             \
            const size_t count = segment##T##Count(seg);                       \
            if (count && splitmix64(&rng) % 3 == 0) {                          \
                const size_t a = (size_t)(splitmix64(&rng) % count);           \
                const size_t z = (size_t)(splitmix64(&rng) % count);           \
                const size_t left = a < z ? a : z;                             \
                const size_t right = a < z ? z : a;                            \
                const databox lb = DATABOX_UNSIGNED(left);                     \
                const databox rb = DATABOX_UNSIGNED(right);                    \
                segment##T##RangeUpdate(seg, left, right, v);                  \
                const databox *args[] = {&lb, &rb, (const databox *)&vb};      \
                persistContractRecord(b->log, PERSIST_OP_UPDATE, args, 3);     \
            } else {                                                           \
                const size_t idx = (size_t)(splitmix64(&rng) % span);          \
                const databox ib = DATABOX_UNSIGNED(idx);                      \
                segment##T##Update(&seg, idx, v);                              \
                const databox *args[] = {&ib, (const databox *)&vb};           \
                persistContractRecord(b->log, PERSIST_OP_UPDATE, args, 2);     \
            }                                                                  \
        }                                                                      \
                                                                               \
        return seg;                                                            \
    }                                                                          \
                                                                               \
    const persistContractType persistContractSegment##T = {                    \
        .ops = &persistOpsSegment##T,                                          \
        .build = pcSegment##T##Build,                                          \
    };

PC_SEGMENT(I16, int16_t, 0)
PC_SEGMENT(I32, int32_t, 0)
PC_SEGMENT(I64, int64_t, 0)
PC_SEGMENT(U16, uint16_t, 0)
PC_SEGMENT(U32, uint32_t, 0)
PC_SEGMENT(U64, uint64_t, 0)
PC_SEGMENT(Float, float, 1)
PC_SEGMENT(Double, double, 1)
PC_SEGMENT(I128, __int128_t, 0)
PC_SEGMENT(U128, __uint128_t, 0)

#endif /* DATAKIT_TEST */
