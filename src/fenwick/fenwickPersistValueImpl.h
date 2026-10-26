/* ====================================================================
 * Exact databox conversion for one typed tree value type
 * ====================================================================
 *
 * Shared by the typed 1D and 2D persistence (fenwickCoreDispatchImpl.h,
 * fenwick2dTypedImpl.h).  Included with:
 *   FPV_T            - the C value type
 *   FPV_IS_FLOATING  - 1 for float and double
 *   FPV_FN(name)     - a static function name for this instantiation
 * and flex.h already included.  Defines Box_, Unbox_ and Append_. */

/* A box holding v exactly: SIGNED/UNSIGNED_64, the 128-bit boxes for the
 * 128-bit types, FLOAT_32 or DOUBLE_64 */
static void FPV_FN(Box_)(FPV_T v, databoxBig *out) {
    memset(out, 0, sizeof(*out));
#if FPV_IS_FLOATING
    if (sizeof(v) == sizeof(float)) {
        out->type = DATABOX_FLOAT_32;
        out->data.f32 = (float)v;
    } else {
        out->type = DATABOX_DOUBLE_64;
        out->data.d64 = (double)v;
    }
#else
    const bool isSigned = (FPV_T)-1 < (FPV_T)0;
    if (sizeof(v) == 16) {
        DATABOX_BIG_INIT(out);
        if (isSigned) {
            out->type = DATABOX_SIGNED_128;
            *out->data.i128 = (__int128_t)v;
        } else {
            out->type = DATABOX_UNSIGNED_128;
            *out->data.u128 = (__uint128_t)v;
        }
    } else if (isSigned) {
        out->type = DATABOX_SIGNED_64;
        out->data.i64 = (int64_t)v;
    } else {
        out->type = DATABOX_UNSIGNED_64;
        out->data.u64 = (uint64_t)v;
    }
#endif
}

/* A value of the type, exactly: any integer box whose value fits it; a
 * float type takes FLOAT_32, double also DOUBLE_64 */
static bool FPV_FN(Unbox_)(const databoxBig *b, FPV_T *out) {
#if FPV_IS_FLOATING
    if (b->type == DATABOX_FLOAT_32) {
        *out = (FPV_T)b->data.f32;
        return true;
    }

    if (b->type == DATABOX_DOUBLE_64 && sizeof(FPV_T) == sizeof(double)) {
        *out = (FPV_T)b->data.d64;
        return true;
    }

    return false;
#else
    const bool isSigned = (FPV_T)-1 < (FPV_T)0;
    const __uint128_t int128Max = ~(__uint128_t)0 >> 1;
    __int128_t s;
    __uint128_t u;
    switch (b->type) {
    case DATABOX_SIGNED_64:
        s = b->data.i64;
        u = (__uint128_t)s;
        break;
    case DATABOX_UNSIGNED_64:
        u = b->data.u64;
        s = (__int128_t)u;
        break;
    case DATABOX_SIGNED_128:
        s = *b->data.i128;
        u = (__uint128_t)s;
        break;
    case DATABOX_UNSIGNED_128:
        u = *b->data.u128;
        if (isSigned && u > int128Max) {
            return false;
        }
        s = (__int128_t)u;
        break;
    default:
        return false;
    }

    if (isSigned) {
        const __int128_t max =
            (__int128_t)((((__uint128_t)1) << (8 * sizeof(FPV_T) - 1)) - 1);
        if (s > max || s < -max - 1) {
            return false;
        }

        *out = (FPV_T)s;
    } else {
        const bool negative =
            (b->type == DATABOX_SIGNED_64 || b->type == DATABOX_SIGNED_128) &&
            s < 0;
        if (negative || u > (__uint128_t)(FPV_T) ~(FPV_T)0) {
            return false;
        }

        *out = (FPV_T)u;
    }

    return true;
#endif
}

/* Append values[0, n) exactly, 64 entries per flex growth */
static void FPV_FN(Append_)(flex **f, const FPV_T *values, uint64_t n) {
    constexpr int BATCH = 64;
    databoxBig boxes[BATCH];
    const databox *ptrs[BATCH];
    for (uint64_t at = 0; at < n; at += BATCH) {
        const uint64_t len = n - at < BATCH ? n - at : BATCH;
        for (uint64_t i = 0; i < len; i++) {
            FPV_FN(Box_)(values[at + i], &boxes[i]);
            ptrs[i] = (const databox *)&boxes[i];
        }

        flexAppendMultipleExact(f, (uint_fast32_t)len, ptrs);
    }
}

#undef FPV_T
#undef FPV_IS_FLOATING
#undef FPV_FN
