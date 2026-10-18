#include "varintDD.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ====================================================================
 * Vector primitive layer
 * ====================================================================
 * Double-double algorithms are pure straight-line dependency chains
 * with no cross-element communication, which makes them a perfect fit
 * for SIMD: N lanes run N independent values at exactly the scalar
 * instruction count. The published scalar cost of double-double add is
 * ~12x a double add; at 2 (NEON) or 4 (AVX2) lanes that amortizes down
 * to roughly 6x and 3x respectively.
 *
 * Rather than write the add/multiply/reduce sequences three times, the
 * handful of primitives they need are abstracted here and the
 * algorithms are expressed once against that vocabulary.
 *
 * AVX2 is paired with an explicit __FMA__ requirement. Every shipping
 * AVX2 part also has FMA3, but the two are separate feature bits and
 * the exact-product transformation is not worth emulating in vector
 * code when the scalar fallback is already correct. */

/* Building with -DVARINT_DD_FORCE_SCALAR selects the portable path on
 * a machine that has a vector unit. That exists to be used: it is how
 * the fallback gets test coverage on developer hardware instead of
 * only on whatever CI happens to run, and it is how the contribution
 * of SIMD is measured - build varintDDBench both ways and compare,
 * rather than trying to defeat the auto-vectorizer inside one binary. */
/* No FMA also means no vector backend. The two are coupled in reality
 * - the AVX2 path already requires __FMA__, and every aarch64 part has
 * FMA - so this only bites when VARINT_DD_FORCE_NO_FMA is used to
 * exercise the fallback. Letting the vector multiply keep its fused
 * cross terms while the scalar one gave them up would break the
 * bit-exactness the two paths are supposed to guarantee. */
#if defined(VARINT_DD_FORCE_SCALAR) || !VARINT_DD_HAS_FMA
#define VARINT_DD_SIMD 0
#define VARINT_DD_BACKEND "scalar (forced)"
#define VARINT_DD_LANES 1

#elif defined(__AVX2__) && defined(__FMA__)
#define VARINT_DD_SIMD 1
#define VARINT_DD_BACKEND "AVX2"
#define VARINT_DD_ISA_AVX2 1
#define VARINT_DD_LANES 4

#include <immintrin.h>

typedef __m256d vdbl;

#define vdblAdd(a, b) _mm256_add_pd((a), (b))
#define vdblSub(a, b) _mm256_sub_pd((a), (b))
#define vdblMul(a, b) _mm256_mul_pd((a), (b))
/* a * b - c evaluated with a single rounding */
#define vdblMulSub(a, b, c) _mm256_fmsub_pd((a), (b), (c))
/* a * b + c evaluated with a single rounding */
#define vdblMulAdd(a, b, c) _mm256_fmadd_pd((a), (b), (c))
#define vdblLoad(p) _mm256_loadu_pd(p)
#define vdblStore(p, v) _mm256_storeu_pd((p), (v))
#define vdblZero() _mm256_setzero_pd()

#elif defined(__ARM_NEON) && defined(__aarch64__)
#define VARINT_DD_SIMD 1
#define VARINT_DD_BACKEND "NEON"
#define VARINT_DD_ISA_NEON 1
#define VARINT_DD_LANES 2

#include <arm_neon.h>

typedef float64x2_t vdbl;

#define vdblAdd(a, b) vaddq_f64((a), (b))
#define vdblSub(a, b) vsubq_f64((a), (b))
#define vdblMul(a, b) vmulq_f64((a), (b))
/* vfmaq_f64(acc, x, y) is acc + x*y, so a*b - c is (-c) + a*b */
#define vdblMulSub(a, b, c) vfmaq_f64(vnegq_f64(c), (a), (b))
/* a * b + c evaluated with a single rounding */
#define vdblMulAdd(a, b, c) vfmaq_f64((c), (a), (b))
#define vdblLoad(p) vld1q_f64(p)
#define vdblStore(p, v) vst1q_f64((p), (v))
#define vdblZero() vdupq_n_f64(0.0)

#else
#define VARINT_DD_SIMD 0
#define VARINT_DD_BACKEND "scalar"
#define VARINT_DD_LANES 1
#endif

const char *varintDDBackend(void) {
    return VARINT_DD_BACKEND;
}

size_t varintDDBackendLanes(void) {
    return VARINT_DD_LANES;
}

#if VARINT_DD_SIMD

/* A vector of double-double values held structure-of-arrays: lane i of
 * hi pairs with lane i of lo. */
typedef struct vdd {
    vdbl hi;
    vdbl lo;
} vdd;

/* Vector forms of the error-free transformations. These mirror the
 * scalar versions in varintDD.h operation for operation. */

static inline vdd vddTwoSum(vdbl a, vdbl b) {
    const vdbl sum = vdblAdd(a, b);
    const vdbl bKept = vdblSub(sum, a);
    const vdbl aKept = vdblSub(sum, bKept);
    const vdbl aLost = vdblSub(a, aKept);
    const vdbl bLost = vdblSub(b, bKept);
    return (vdd){sum, vdblAdd(aLost, bLost)};
}

static inline vdd vddFastTwoSum(vdbl a, vdbl b) {
    const vdbl sum = vdblAdd(a, b);
    return (vdd){sum, vdblSub(b, vdblSub(sum, a))};
}

static inline vdd vddTwoProduct(vdbl a, vdbl b) {
    const vdbl product = vdblMul(a, b);
    return (vdd){product, vdblMulSub(a, b, product)};
}

static inline vdd vddAdd(vdd a, vdd b) {
    vdd s = vddTwoSum(a.hi, b.hi);
    const vdd t = vddTwoSum(a.lo, b.lo);

    s.lo = vdblAdd(s.lo, t.hi);
    s = vddFastTwoSum(s.hi, s.lo);

    s.lo = vdblAdd(s.lo, t.lo);
    return vddFastTwoSum(s.hi, s.lo);
}

static inline vdd vddMul(vdd a, vdd b) {
    vdd p = vddTwoProduct(a.hi, b.hi);

    /* Must fold the cross terms exactly as varintDDMul does, down to
     * the single rounding of the fused multiply-add, or the two paths
     * disagree in the last bit and the backends stop being
     * interchangeable. */
    p.lo = vdblAdd(p.lo, vdblMulAdd(a.hi, b.lo, vdblMul(a.lo, b.hi)));
    return vddFastTwoSum(p.hi, p.lo);
}

/* ====================================================================
 * Deinterleave / interleave
 * ====================================================================
 * varintDD is an array of structs; the vector algorithms need
 * structure-of-arrays. On NEON this is a single instruction in each
 * direction. On AVX2 it costs an unpack pair plus a lane permute. */

static inline vdd vddLoadInterleaved(const varintDD *p) {
#if VARINT_DD_LANES == 2
    const float64x2x2_t t = vld2q_f64((const double *)p);
    return (vdd){t.val[0], t.val[1]};
#else
    /* a = {h0,l0,h1,l1}   b = {h2,l2,h3,l3} */
    const __m256d a = _mm256_loadu_pd((const double *)p);
    const __m256d b = _mm256_loadu_pd((const double *)(p + 2));

    /* unpack works within each 128-bit half: {h0,h2,h1,h3}, {l0,l2,l1,l3} */
    const __m256d hs = _mm256_unpacklo_pd(a, b);
    const __m256d ls = _mm256_unpackhi_pd(a, b);

    /* reorder lanes 0,2,1,3 -> 0,1,2,3 */
    return (vdd){_mm256_permute4x64_pd(hs, _MM_SHUFFLE(3, 1, 2, 0)),
                 _mm256_permute4x64_pd(ls, _MM_SHUFFLE(3, 1, 2, 0))};
#endif
}

static inline void vddStoreInterleaved(varintDD *p, vdd v) {
#if VARINT_DD_LANES == 2
    float64x2x2_t t;
    t.val[0] = v.hi;
    t.val[1] = v.lo;
    vst2q_f64((double *)p, t);
#else
    const __m256d hs = _mm256_permute4x64_pd(v.hi, _MM_SHUFFLE(3, 1, 2, 0));
    const __m256d ls = _mm256_permute4x64_pd(v.lo, _MM_SHUFFLE(3, 1, 2, 0));

    _mm256_storeu_pd((double *)p, _mm256_unpacklo_pd(hs, ls));
    _mm256_storeu_pd((double *)(p + 2), _mm256_unpackhi_pd(hs, ls));
#endif
}

static inline void vdblExtract(vdbl v, double *out) {
    vdblStore(out, v);
}

#endif /* VARINT_DD_SIMD */

/* ====================================================================
 * Elementwise array arithmetic
 * ==================================================================== */

void varintDDAddArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count) {
    size_t i = 0;

#if VARINT_DD_SIMD
    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        vddStoreInterleaved(dst + i, vddAdd(vddLoadInterleaved(a + i),
                                            vddLoadInterleaved(b + i)));
    }
#endif

    for (; i < count; i++) {
        dst[i] = varintDDAdd(a[i], b[i]);
    }
}

void varintDDMulArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count) {
    size_t i = 0;

#if VARINT_DD_SIMD
    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        vddStoreInterleaved(dst + i, vddMul(vddLoadInterleaved(a + i),
                                            vddLoadInterleaved(b + i)));
    }
#endif

    for (; i < count; i++) {
        dst[i] = varintDDMul(a[i], b[i]);
    }
}

/* ====================================================================
 * Reductions
 * ====================================================================
 * Each lane keeps a private (sum, compensation) pair, so the lanes
 * never need to talk to each other inside the loop. Only the final
 * merge is sequential, and it is O(lanes), not O(count).
 *
 * Merging partial sums is not merely allowed here, it is BETTER than a
 * sequential reduction: splitting the input across L accumulators cuts
 * the depth of the dependency chain by L, which reduces error growth
 * as well as latency. */

varintDD varintDDSumDoubles(const double *values, size_t count) {
    varintDDAccum acc = varintDDAccumInit();
    size_t i = 0;

#if VARINT_DD_SIMD
    vdbl sum = vdblZero();
    vdbl comp = vdblZero();

    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        const vdd s = vddTwoSum(sum, vdblLoad(values + i));
        sum = s.hi;
        comp = vdblAdd(comp, s.lo);
    }

    {
        double sumLanes[VARINT_DD_LANES];
        double compLanes[VARINT_DD_LANES];

        vdblExtract(sum, sumLanes);
        vdblExtract(comp, compLanes);

        for (size_t lane = 0; lane < VARINT_DD_LANES; lane++) {
            varintDDAccumAdd(&acc, sumLanes[lane]);
            acc.comp += compLanes[lane];
        }
    }
#endif

    for (; i < count; i++) {
        varintDDAccumAdd(&acc, values[i]);
    }

    return varintDDAccumResult(&acc);
}

varintDD varintDDSumArray(const varintDD *values, size_t count) {
    varintDDAccum acc = varintDDAccumInit();
    size_t i = 0;

#if VARINT_DD_SIMD
    vdbl sum = vdblZero();
    vdbl comp = vdblZero();

    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        const vdd v = vddLoadInterleaved(values + i);
        const vdd s = vddTwoSum(sum, v.hi);

        sum = s.hi;
        /* the trailing limb of the input is already below the running
         * sum's ulp, so it joins the compensation term directly */
        comp = vdblAdd(comp, vdblAdd(s.lo, v.lo));
    }

    {
        double sumLanes[VARINT_DD_LANES];
        double compLanes[VARINT_DD_LANES];

        vdblExtract(sum, sumLanes);
        vdblExtract(comp, compLanes);

        for (size_t lane = 0; lane < VARINT_DD_LANES; lane++) {
            varintDDAccumAdd(&acc, sumLanes[lane]);
            acc.comp += compLanes[lane];
        }
    }
#endif

    for (; i < count; i++) {
        varintDDAccumAdd(&acc, values[i].hi);
        acc.comp += values[i].lo;
    }

    return varintDDAccumResult(&acc);
}

varintDD varintDDDotDoubles(const double *a, const double *b, size_t count) {
    varintDDAccum acc = varintDDAccumInit();
    size_t i = 0;

#if VARINT_DD_SIMD
    vdbl sum = vdblZero();
    vdbl comp = vdblZero();

    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        /* the product's discarded tail is recovered exactly, so no
         * information is lost between the multiply and the add */
        const vdd p = vddTwoProduct(vdblLoad(a + i), vdblLoad(b + i));
        const vdd s = vddTwoSum(sum, p.hi);

        sum = s.hi;
        comp = vdblAdd(comp, vdblAdd(s.lo, p.lo));
    }

    {
        double sumLanes[VARINT_DD_LANES];
        double compLanes[VARINT_DD_LANES];

        vdblExtract(sum, sumLanes);
        vdblExtract(comp, compLanes);

        for (size_t lane = 0; lane < VARINT_DD_LANES; lane++) {
            varintDDAccumAdd(&acc, sumLanes[lane]);
            acc.comp += compLanes[lane];
        }
    }
#endif

    for (; i < count; i++) {
        const varintDD p = varintDDTwoProduct(a[i], b[i]);
        varintDDAccumAdd(&acc, p.hi);
        acc.comp += p.lo;
    }

    return varintDDAccumResult(&acc);
}

/* ====================================================================
 * Structure-of-arrays conversion
 * ==================================================================== */

void varintDDSplitLimbs(const varintDD *src, double *hi, double *lo,
                        size_t count) {
    size_t i = 0;

#if VARINT_DD_SIMD
    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        const vdd v = vddLoadInterleaved(src + i);
        vdblStore(hi + i, v.hi);
        vdblStore(lo + i, v.lo);
    }
#endif

    for (; i < count; i++) {
        hi[i] = src[i].hi;
        lo[i] = src[i].lo;
    }
}

void varintDDJoinLimbs(varintDD *dst, const double *hi, const double *lo,
                       size_t count) {
    size_t i = 0;

#if VARINT_DD_SIMD
    for (; i + VARINT_DD_LANES <= count; i += VARINT_DD_LANES) {
        const vdd v = {vdblLoad(hi + i), vdblLoad(lo + i)};
        vddStoreInterleaved(dst + i, v);
    }
#endif

    for (; i < count; i++) {
        dst[i].hi = hi[i];
        dst[i].lo = lo[i];
    }
}

/* ====================================================================
 * Nonzero-limb bitmap
 * ====================================================================
 * The test is on the raw BIT PATTERN, not the numeric value: -0.0 must
 * register as present so the codec can round-trip its sign bit. A
 * floating-point compare against zero would silently drop it.
 *
 * The portable path is SWAR — eight independent zero-tests folded into
 * one byte with no branches, which compilers autovectorize well. The
 * NEON path replaces the per-element test with vtstq_u64 (bitwise
 * "any bit set") and folds the lane weights with a horizontal add. */
size_t varintDDNonzeroLimbMask(const double *lo, size_t count,
                               uint8_t *bitmap) {
    const size_t bitmapBytes = (count + 7) / 8;
    size_t setBits = 0;
    size_t i = 0;

    memset(bitmap, 0, bitmapBytes);

#if defined(VARINT_DD_ISA_NEON)
    {
        static const uint64_t laneWeight[4][2] = {
            {1, 2}, {4, 8}, {16, 32}, {64, 128}};

        for (; i + 8 <= count; i += 8) {
            uint64_t byte = 0;

            for (size_t pair = 0; pair < 4; pair++) {
                /* Load as doubles and reinterpret the REGISTER, rather
                 * than casting the pointer. The values are doubles, and
                 * reading them through a uint64_t lvalue is a strict
                 * aliasing violation the optimizer is entitled to
                 * assume never happens. */
                const uint64x2_t v =
                    vreinterpretq_u64_f64(vld1q_f64(lo + i + pair * 2));
                /* all-ones in a lane whose value has any bit set */
                const uint64x2_t nz = vtstq_u64(v, v);
                const uint64x2_t weighted =
                    vandq_u64(nz, vld1q_u64(laneWeight[pair]));
                byte |= vaddvq_u64(weighted);
            }

            bitmap[i / 8] = (uint8_t)byte;
            setBits += (size_t)__builtin_popcountll(byte);
        }
    }
#elif defined(VARINT_DD_ISA_AVX2)
    {
        const __m256i zero = _mm256_setzero_si256();

        for (; i + 8 <= count; i += 8) {
            /* Load as doubles, reinterpret the register: casting the
             * pointer instead would read double objects through an
             * integer lvalue, which is undefined. */
            const __m256i a = _mm256_castpd_si256(_mm256_loadu_pd(lo + i));
            const __m256i b = _mm256_castpd_si256(_mm256_loadu_pd(lo + i + 4));

            /* movemask lifts the sign bit of each equal-to-zero result,
             * giving a 4-bit "is zero" mask that inverts to "is set" */
            const int zeroLow = _mm256_movemask_pd(
                _mm256_castsi256_pd(_mm256_cmpeq_epi64(a, zero)));
            const int zeroHigh = _mm256_movemask_pd(
                _mm256_castsi256_pd(_mm256_cmpeq_epi64(b, zero)));

            const uint64_t byte =
                (uint64_t)((~(zeroLow | (zeroHigh << 4))) & 0xFF);

            bitmap[i / 8] = (uint8_t)byte;
            setBits += (size_t)__builtin_popcountll(byte);
        }
    }
#endif

    /* SWAR tail (and the whole array when no SIMD backend is present).
     *
     * memcpy, not a cast. Reading double objects through a uint64_t
     * lvalue violates strict aliasing, and optimizers act on that
     * assumption to produce a mask with the wrong bits set. The memcpy
     * compiles to the same single load. */
    for (; i + 8 <= count; i += 8) {
        uint64_t byte = 0;

        for (size_t bit = 0; bit < 8; bit++) {
            uint64_t raw;

            memcpy(&raw, lo + i + bit, sizeof(raw));
            byte |= (uint64_t)(raw != 0) << bit;
        }

        bitmap[i / 8] = (uint8_t)byte;
        setBits += (size_t)__builtin_popcountll(byte);
    }

    for (; i < count; i++) {
        uint64_t raw;
        memcpy(&raw, lo + i, sizeof(raw));

        if (raw != 0) {
            bitmap[i / 8] |= (uint8_t)(1U << (i % 8));
            setBits++;
        }
    }

    return setBits;
}

/* ====================================================================
 * Text conversion
 * ==================================================================== */

/* Scale value by 10^power, applied in chunks of at most 10^22.
 *
 * Building the whole power of ten first and applying it in one step
 * looks tidier and is wrong at both ends of the exponent range: 10^324
 * is not representable, so a denormal input would be multiplied by
 * infinity and print as zero. Walking the scaling in chunks keeps every
 * intermediate inside the double range, because the value is moving
 * TOWARDS [1, 10) with each step rather than away from it.
 *
 * 10^22 is the largest power of ten a double holds exactly, so each
 * chunk factor is exact and the only error comes from the chunk
 * operations themselves - about 2^-101 relative across the full range,
 * which is what limits printing to ~30 trustworthy digits. */
static varintDD varintDDScaleByPow10_(varintDD value, int power) {
    static const double exactPowers[23] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

    int remaining = power;

    while (remaining > 0) {
        const int chunk = remaining > 22 ? 22 : remaining;

        value = varintDDMulDouble(value, exactPowers[chunk]);
        remaining -= chunk;
    }

    while (remaining < 0) {
        const int chunk = remaining < -22 ? 22 : -remaining;

        value = varintDDDivDouble(value, exactPowers[chunk]);
        remaining += chunk;
    }

    return value;
}

static size_t varintDDCopyLiteral_(char *dst, size_t dstLen, const char *text) {
    const size_t length = strlen(text);

    if (length + 1 > dstLen) {
        return 0;
    }

    memcpy(dst, text, length + 1);
    return length;
}

size_t varintDDToString(char *dst, size_t dstLen, varintDD value,
                        uint32_t digits) {
    if (dst == NULL || dstLen == 0) {
        return 0;
    }

    if (digits == 0) {
        digits = VARINT_DD_DIGITS_DEFAULT;
    }

    if (digits > VARINT_DD_DIGITS_MAX) {
        digits = VARINT_DD_DIGITS_MAX;
    }

    if (varintDDIsNaN(value)) {
        return varintDDCopyLiteral_(dst, dstLen, "nan");
    }

    if (isinf(value.hi)) {
        return varintDDCopyLiteral_(dst, dstLen,
                                    value.hi < 0.0 ? "-inf" : "inf");
    }

    /* Signed zero is checked BEFORE normalizing. Normalization adds the
     * limbs, and -0.0 + 0.0 is +0.0 under IEEE rules, so running a
     * genuine negative zero through it would silently drop the sign the
     * caller is asking us to display. */
    if (value.hi == 0.0 && value.lo == 0.0) {
        return varintDDCopyLiteral_(dst, dstLen,
                                    signbit(value.hi) ? "-0" : "0");
    }

    /* A caller may hand us limbs that were assembled rather than
     * computed, and digit extraction assumes the invariant holds. */
    value = varintDDNormalize(value);

    if (value.hi == 0.0) {
        return varintDDCopyLiteral_(dst, dstLen, "0");
    }

    const bool negative = value.hi < 0.0;
    varintDD magnitude = varintDDAbs(value);

    int exponent = (int)floor(log10(magnitude.hi));

    varintDD scaled = varintDDScaleByPow10_(magnitude, -exponent);

    /* log10 and the scaling can each land a decade off, so the result
     * is nudged back into [1, 10).
     *
     * These comparisons MUST look at both limbs. A value just under a
     * power of ten - 999999999999999999, say - has a leading limb that
     * rounds up to exactly 1e18, so testing hi alone reports 1.0 and
     * concludes nothing needs fixing, while the true value is below one
     * and every digit afterwards comes out shifted. */
    {
        const varintDD one = varintDDFromDouble(1.0);
        const varintDD ten = varintDDFromDouble(10.0);

        for (uint32_t pass = 0; pass < 2; pass++) {
            if (varintDDCompare(scaled, ten) >= 0) {
                scaled = varintDDDivDouble(scaled, 10.0);
                exponent++;
            } else if (varintDDCompare(scaled, one) < 0) {
                scaled = varintDDMulDouble(scaled, 10.0);
                exponent--;
            }
        }
    }

    /* One guard digit beyond what the caller asked for, so the last
     * reported digit can be rounded rather than truncated. */
    int digit[VARINT_DD_DIGITS_MAX + 2];
    const uint32_t extracted = digits + 1;

    for (uint32_t i = 0; i < extracted; i++) {
        const int d = (int)scaled.hi;

        digit[i] = d;
        scaled = varintDDMulDouble(varintDDAddDouble(scaled, -(double)d), 10.0);
    }

    /* Accumulated error can push a digit just past either end of its
     * range; borrowing from the neighbour restores a valid decimal. */
    for (int i = (int)extracted - 1; i > 0; i--) {
        if (digit[i] < 0) {
            digit[i] += 10;
            digit[i - 1]--;
        } else if (digit[i] > 9) {
            digit[i] -= 10;
            digit[i - 1]++;
        }
    }

    /* Safety net: if the borrow chain reached all the way to the front,
     * the value was a decade lower than the scaling concluded and the
     * leading digit is now zero. Shifting up costs the guard digit,
     * which is cheaper than emitting "0.99..." with a leading zero. */
    for (uint32_t guard = 0; guard < 2 && digit[0] == 0; guard++) {
        for (uint32_t i = 0; i + 1 < extracted; i++) {
            digit[i] = digit[i + 1];
        }

        digit[extracted - 1] = 0;
        exponent--;
    }

    if (digit[extracted - 1] >= 5) {
        int i = (int)extracted - 2;

        while (i >= 0) {
            if (++digit[i] <= 9) {
                break;
            }

            digit[i] = 0;
            i--;
        }

        if (i < 0) {
            /* rounded past the leading digit: 9.99... became 10.0... */
            memset(digit, 0, sizeof(digit));
            digit[0] = 1;
            exponent++;
        }
    }

    char buffer[VARINT_DD_STRING_MAX];
    size_t used = 0;

    if (negative) {
        buffer[used++] = '-';
    }

    buffer[used++] = (char)('0' + digit[0]);

    if (digits > 1) {
        buffer[used++] = '.';

        for (uint32_t i = 1; i < digits; i++) {
            buffer[used++] = (char)('0' + digit[i]);
        }
    }

    const int tail =
        snprintf(buffer + used, sizeof(buffer) - used, "e%+03d", exponent);

    if (tail < 0 || (size_t)tail >= sizeof(buffer) - used) {
        return 0;
    }

    used += (size_t)tail;

    if (used + 1 > dstLen) {
        return 0;
    }

    memcpy(dst, buffer, used + 1);
    return used;
}

/* ====================================================================
 * Self-check
 * ====================================================================
 * The #error guards in varintDD.h catch -ffast-math when the
 * preprocessor can see it. This catches what it cannot: individual
 * -fassociative-math style flags, reassociation introduced at LTO time,
 * a non-default rounding mode left set by other code, or a toolchain
 * whose fma() is not correctly rounded.
 *
 * The constants below are chosen so that a compiler which applies real
 * arithmetic to the transformations produces exactly zero for the error
 * terms, which is the failure this is looking for. */

/* Defeat constant folding without the code-motion cost of volatile on
 * every operand: the compiler cannot see through an opaque identity. */
static double varintDDOpaque_(double v) {
    volatile double sink = v;
    return sink;
}

bool varintDDSelfCheck(void) {
    /* 1 and 2^-60: the sum needs 61 significand bits, so exactly
     * 2^-60 must survive in the trailing limb. */
    {
        const double a = varintDDOpaque_(1.0);
        const double b = varintDDOpaque_(ldexp(1.0, -60));
        const varintDD s = varintDDTwoSum(a, b);

        if (s.hi != 1.0 || s.lo != ldexp(1.0, -60)) {
            return false;
        }
    }

    /* Reversed operand order must give the identical decomposition;
     * two-sum carries no ordering requirement. */
    {
        const double a = varintDDOpaque_(ldexp(1.0, -60));
        const double b = varintDDOpaque_(1.0);
        const varintDD s = varintDDTwoSum(a, b);

        if (s.hi != 1.0 || s.lo != ldexp(1.0, -60)) {
            return false;
        }
    }

    /* Cancellation: the difference is exact and the error term is zero,
     * but only if each operation rounded to double along the way. */
    {
        const double a = varintDDOpaque_(1.0 + ldexp(1.0, -52));
        const double b = varintDDOpaque_(1.0);
        const varintDD s = varintDDTwoDiff(a, b);

        if (s.hi != ldexp(1.0, -52) || s.lo != 0.0) {
            return false;
        }
    }

    /* Exact product: two full-width significands whose product needs
     * 106 bits. The trailing limb must be nonzero and must complete the
     * product exactly. */
    {
        const double a = varintDDOpaque_(1.0 + ldexp(1.0, -52));
        const varintDD p = varintDDTwoProduct(a, a);

        /* (1 + 2^-52)^2 == 1 + 2^-51 + 2^-104 */
        if (p.hi != 1.0 + ldexp(1.0, -51)) {
            return false;
        }

        if (p.lo != ldexp(1.0, -104)) {
            return false;
        }
    }

    /* The composed operations must deliver more than double precision:
     * 1/3 multiplied back by 3 has to land within a few 2^-106 of one,
     * where the plain-double result is off by ~2^-53. */
    {
        const varintDD third =
            varintDDDiv(varintDDFromDouble(varintDDOpaque_(1.0)),
                        varintDDFromDouble(varintDDOpaque_(3.0)));
        const varintDD back = varintDDMulDouble(third, 3.0);
        const varintDD err = varintDDSub(back, varintDDFromDouble(1.0));

        if (fabs(varintDDToDouble(err)) > ldexp(1.0, -100)) {
            return false;
        }
    }

    /* int64 values above 2^53 must convert without loss. */
    {
        const int64_t big = 9007199254740993LL; /* 2^53 + 1 */
        const varintDD v = varintDDFromInt64(big);
        const varintDD diff =
            varintDDSub(v, varintDDFromDouble(9007199254740992.0));

        if (varintDDToDouble(diff) != 1.0) {
            return false;
        }
    }

    return true;
}

/* ====================================================================
 * TESTS
 * ==================================================================== */
#ifdef VARINT_DD_TEST

#include "ctest.h"
#include <stdlib.h>

/* --------------------------------------------------------------------
 * Exact-arithmetic oracle
 * --------------------------------------------------------------------
 * Validating an extended-precision library against itself proves
 * nothing, so every claim below is checked against integer arithmetic
 * that touches no floating point at all.
 *
 * Two layers, because one width does not fit both jobs:
 *
 *   ddExact  A single term: mant * 2^exp with mant in a 128-bit
 *            integer. Every finite double is exactly (-1)^s * M * 2^E
 *            for M below 2^53, and the product of two doubles needs at
 *            most 106 bits, so one term always fits.
 *
 *   ddAcc    A sum of terms, held as a wide two's-complement fixed-point
 *            integer. This is the layer 128 bits cannot do: the exact
 *            product of two DOUBLE-DOUBLES spans ~212 significant bits
 *            before any alignment, and comparing it against a computed
 *            result needs room for both plus the gap between them.
 *
 * Nothing here needs a big-integer multiply. A double-double product
 * expands to four exact double products, and those are summed in the
 * accumulator - which is exactly how ddAccFromDDProduct is built. */

/* 128-bit integers have no stdint spelling, so these use the compiler's
 * own __int128_t / __uint128_t type names rather than the "unsigned
 * __int128" keyword form. __extension__ keeps -pedantic quiet about a
 * type the standard does not define. */
__extension__ typedef __int128_t ddInt128;
__extension__ typedef __uint128_t ddUInt128;

typedef struct ddExact {
    ddInt128 mant;
    int exp; /* value == mant * 2^exp */
    bool ok; /* false for NaN and infinity, which have no exact form */
} ddExact;

static ddExact ddExactFromDouble(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));

    const uint32_t expField = (uint32_t)((bits >> 52) & 0x7FF);
    const uint64_t mantField = bits & 0xFFFFFFFFFFFFFULL;

    ddExact r = {0, 0, true};

    if (expField == 0x7FF) {
        r.ok = false;
        return r;
    }

    if (expField == 0) {
        r.mant = (ddInt128)mantField; /* denormal: no implicit leading bit */
        r.exp = -1074;
    } else {
        r.mant = (ddInt128)(mantField | (1ULL << 52));
        r.exp = (int)expField - 1075;
    }

    if (bits >> 63) {
        r.mant = -r.mant;
    }

    return r;
}

/* Exact product of two doubles. Two 53-bit significands make at most
 * 106 bits, so this never rounds and never overflows. */
static ddExact ddExactProduct(double a, double b) {
    const ddExact x = ddExactFromDouble(a);
    const ddExact y = ddExactFromDouble(b);
    ddExact r = {0, 0, false};

    if (!x.ok || !y.ok) {
        return r;
    }

    r.ok = true;
    r.mant = x.mant * y.mant;
    r.exp = x.exp + y.exp;
    return r;
}

static ddExact ddExactFromInt64(int64_t v) {
    const ddExact r = {(ddInt128)v, 0, true};
    return r;
}

/* --------------------------------------------------------------------
 * Wide fixed-point accumulator
 * --------------------------------------------------------------------
 * 1536 bits of two's complement at a fixed scale of 2^-700. Two's
 * complement rather than sign-and-magnitude so that add and subtract
 * are plain ripple loops with no sign analysis.
 *
 * The scale bounds what the oracle can represent: terms below 2^-700
 * would silently lose bits, and terms above ~2^830 would overflow, so
 * both are reported by clearing `ok` rather than being absorbed. Tests
 * draw their operands from exponent windows well inside that range,
 * and each one asserts a minimum number of successfully verified cases
 * so a generator that drifted out of range cannot pass vacuously. */

#define DD_ACC_LIMBS 24
#define DD_ACC_SCALE (-700)

typedef struct ddAcc {
    uint64_t limb[DD_ACC_LIMBS]; /* little-endian, two's complement */
    bool ok;
} ddAcc;

static ddAcc ddAccZero(void) {
    ddAcc a;
    memset(a.limb, 0, sizeof(a.limb));
    a.ok = true;
    return a;
}

static bool ddAccIsZero(const ddAcc *a) {
    for (size_t i = 0; i < DD_ACC_LIMBS; i++) {
        if (a->limb[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool ddAccIsNegative(const ddAcc *a) {
    return (a->limb[DD_ACC_LIMBS - 1] >> 63) != 0;
}

static void ddAccNegate(ddAcc *a) {
    ddUInt128 carry = 1;

    for (size_t i = 0; i < DD_ACC_LIMBS; i++) {
        const ddUInt128 v = (ddUInt128)(~a->limb[i]) + carry;
        a->limb[i] = (uint64_t)v;
        carry = v >> 64;
    }
}

/* Add mant * 2^exp into the accumulator. */
static void ddAccAdd(ddAcc *acc, ddExact term) {
    if (!term.ok) {
        acc->ok = false;
        return;
    }

    if (term.mant == 0) {
        return;
    }

    const bool negative = term.mant < 0;
    const ddUInt128 magnitude =
        negative ? (ddUInt128)(-term.mant) : (ddUInt128)term.mant;

    const int shift = term.exp - DD_ACC_SCALE;

    if (shift < 0) {
        acc->ok = false; /* finer than the oracle's resolution */
        return;
    }

    /* Spread the 128-bit magnitude across three limbs at a bit offset. */
    uint64_t word[3] = {(uint64_t)magnitude, (uint64_t)(magnitude >> 64), 0};
    const size_t limbOffset = (size_t)shift / 64;
    const uint32_t bitOffset = (uint32_t)shift % 64;

    if (bitOffset != 0) {
        word[2] = word[1] >> (64 - bitOffset);
        word[1] = (word[1] << bitOffset) | (word[0] >> (64 - bitOffset));
        word[0] = word[0] << bitOffset;
    }

    for (size_t i = 0; i < 3; i++) {
        if (word[i] != 0 && limbOffset + i >= DD_ACC_LIMBS) {
            acc->ok = false; /* wider than the oracle can hold */
            return;
        }
    }

    if (!negative) {
        ddUInt128 carry = 0;

        for (size_t i = limbOffset; i < DD_ACC_LIMBS; i++) {
            ddUInt128 sum = (ddUInt128)acc->limb[i] + carry;

            if (i - limbOffset < 3) {
                sum += word[i - limbOffset];
            }

            acc->limb[i] = (uint64_t)sum;
            carry = sum >> 64;
        }
    } else {
        ddUInt128 borrow = 0;

        for (size_t i = limbOffset; i < DD_ACC_LIMBS; i++) {
            const ddUInt128 w = (i - limbOffset < 3) ? word[i - limbOffset] : 0;
            const ddUInt128 diff = (ddUInt128)acc->limb[i] - w - borrow;

            acc->limb[i] = (uint64_t)diff;
            borrow = (diff >> 64) & 1;
        }
    }

    /* The top limb must remain pure sign extension; anything else means
     * the value outgrew the accumulator. */
    if (acc->limb[DD_ACC_LIMBS - 1] != 0 &&
        acc->limb[DD_ACC_LIMBS - 1] != UINT64_MAX) {
        acc->ok = false;
    }
}

static void ddAccSubAcc(ddAcc *acc, const ddAcc *other) {
    ddUInt128 borrow = 0;

    for (size_t i = 0; i < DD_ACC_LIMBS; i++) {
        const ddUInt128 diff =
            (ddUInt128)acc->limb[i] - other->limb[i] - borrow;
        acc->limb[i] = (uint64_t)diff;
        borrow = (diff >> 64) & 1;
    }

    acc->ok = acc->ok && other->ok;
}

static bool ddAccEqual(const ddAcc *a, const ddAcc *b) {
    if (!a->ok || !b->ok) {
        return false;
    }

    return memcmp(a->limb, b->limb, sizeof(a->limb)) == 0;
}

/* Rounded magnitude, used only for relative-error ratios where an
 * approximation is entirely sufficient. */
static double ddAccToDouble(const ddAcc *a) {
    if (!a->ok) {
        return (double)NAN;
    }

    ddAcc t = *a;
    const bool negative = ddAccIsNegative(&t);

    if (negative) {
        ddAccNegate(&t);
    }

    int top = -1;

    for (int i = DD_ACC_LIMBS - 1; i >= 0; i--) {
        if (t.limb[i] != 0) {
            top = i * 64 + (63 - __builtin_clzll(t.limb[i]));
            break;
        }
    }

    if (top < 0) {
        return 0.0;
    }

    /* Take the 64 bits ending at the most significant set bit. */
    uint64_t window = 0;

    for (int b = 0; b < 64; b++) {
        const int bit = top - 63 + b;

        if (bit < 0) {
            continue;
        }

        if ((t.limb[bit / 64] >> (bit % 64)) & 1) {
            window |= 1ULL << b;
        }
    }

    const double magnitude = (double)window;
    return ldexp(negative ? -magnitude : magnitude, DD_ACC_SCALE + top - 63);
}

static ddAcc ddAccFromDouble(double d) {
    ddAcc a = ddAccZero();
    ddAccAdd(&a, ddExactFromDouble(d));
    return a;
}

static ddExact ddExactAbs(ddExact t) {
    if (t.mant < 0) {
        t.mant = -t.mant;
    }

    return t;
}

/* The exact value of a double-double is just the sum of its limbs. */
static ddAcc ddAccFromDD(varintDD v) {
    ddAcc a = ddAccZero();
    ddAccAdd(&a, ddExactFromDouble(v.hi));
    ddAccAdd(&a, ddExactFromDouble(v.lo));
    return a;
}

static ddAcc ddAccFromDDSum(varintDD x, varintDD y) {
    ddAcc a = ddAccFromDD(x);
    ddAccAdd(&a, ddExactFromDouble(y.hi));
    ddAccAdd(&a, ddExactFromDouble(y.lo));
    return a;
}

/* (xh + xl)(yh + yl) expanded into four exact double products. This is
 * why the accumulator needs to be wide and why it needs no multiply. */
static ddAcc ddAccFromDDProduct(varintDD x, varintDD y) {
    ddAcc a = ddAccZero();
    ddAccAdd(&a, ddExactProduct(x.hi, y.hi));
    ddAccAdd(&a, ddExactProduct(x.hi, y.lo));
    ddAccAdd(&a, ddExactProduct(x.lo, y.hi));
    ddAccAdd(&a, ddExactProduct(x.lo, y.lo));
    return a;
}

/* Exact relative error of a computed double-double against the exact
 * answer. Returns 0.0 when the result is exact, NaN when the oracle
 * could not represent the comparison. */
static double ddAccRelativeError(varintDD got, const ddAcc *want) {
    ddAcc diff = ddAccFromDD(got);

    ddAccSubAcc(&diff, want);

    if (!diff.ok || !want->ok) {
        return (double)NAN;
    }

    if (ddAccIsZero(&diff)) {
        return 0.0;
    }

    const double magnitude = fabs(ddAccToDouble(want));

    if (magnitude == 0.0) {
        return (double)INFINITY;
    }

    return fabs(ddAccToDouble(&diff)) / magnitude;
}

/* Exact ABSOLUTE error, which is the right measure for a reduction.
 *
 * Relative-to-the-result is the wrong yardstick for summation: a sum
 * whose terms largely cancel has a tiny result and an enormous
 * condition number, so even a perfect algorithm shows a huge relative
 * error. The standard criterion compares the error against the sum of
 * the term MAGNITUDES instead, which is what ddAccReductionBound
 * builds. */
static double ddAccAbsoluteError(varintDD got, const ddAcc *want) {
    ddAcc diff = ddAccFromDD(got);

    ddAccSubAcc(&diff, want);

    if (!diff.ok || !want->ok) {
        return (double)NAN;
    }

    return fabs(ddAccToDouble(&diff));
}

/* Error bound for compensated summation of n terms, from the
 * Kahan-Babuska-Neumaier analysis: the leading u|sum| term is exactly
 * what the compensation limb recovers, leaving a residual of order
 * n * u^2 * sum|x|. The factor of 4 is headroom, not tuning. */
static double ddAccReductionBound(const ddAcc *absoluteTerms, size_t n) {
    return 4.0 * (double)n * ldexp(1.0, -106) *
           fabs(ddAccToDouble(absoluteTerms));
}

/* --------------------------------------------------------------------
 * Deterministic generators
 * -------------------------------------------------------------------- */

static uint64_t ddRandState;

static uint64_t ddRand64(void) {
    /* splitmix64 */
    uint64_t z = (ddRandState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* A random double with a full 52-bit significand and an exponent drawn
 * from [minExp, maxExp], so callers can keep the oracle in range. */
static double ddRandDouble(int minExp, int maxExp) {
    const uint64_t mantissa = ddRand64() & 0xFFFFFFFFFFFFFULL;
    const int span = maxExp - minExp + 1;
    const int exponent = minExp + (int)(ddRand64() % (uint64_t)span);

    double v = ldexp(1.0 + (double)mantissa / 4503599627370496.0, exponent);

    if (ddRand64() & 1) {
        v = -v;
    }

    return v;
}

/* Build a genuine double-double the way real ones arise - through the
 * arithmetic itself - so the trailing limb carries the natural
 * exponent-gap distribution that varintDDStream is built around. */
static varintDD ddRandDD(int minExp, int maxExp) {
    const varintDD a = varintDDFromDouble(ddRandDouble(minExp, maxExp));
    const varintDD b = varintDDFromDouble(ddRandDouble(minExp, maxExp));
    const varintDD c = varintDDFromDouble(ddRandDouble(minExp, maxExp));
    return varintDDAdd(varintDDMul(a, b), c);
}

/* --------------------------------------------------------------------
 * Test: the error-free transformations really are error free
 * -------------------------------------------------------------------- */

static int ddTestErrorFreeTransforms(void) {
    int err = 0;
    const int iterations = 200000;
    size_t verifiedSum = 0;
    size_t verifiedProduct = 0;

    TEST("error-free transformations reproduce the exact result");

    for (int i = 0; i < iterations; i++) {
        const double a = ddRandDouble(-30, 30);
        const double b = ddRandDouble(-30, 30);

        /* --- two-sum --- */
        {
            const varintDD s = varintDDTwoSum(a, b);

            ddAcc want = ddAccFromDouble(a);
            ddAccAdd(&want, ddExactFromDouble(b));

            const ddAcc got = ddAccFromDD(s);

            if (want.ok && got.ok) {
                verifiedSum++;

                if (!ddAccEqual(&got, &want)) {
                    ERR("twoSum(%.17g, %.17g) is not exact", a, b);
                    return err;
                }

                /* the leading limb must be the correctly rounded sum,
                 * and the trailing limb must be invisible to it */
                if (s.hi != a + b || s.hi + s.lo != s.hi) {
                    ERR("twoSum(%.17g, %.17g) is not normalized", a, b);
                    return err;
                }
            }

            /* operand order must not matter */
            const varintDD swapped = varintDDTwoSum(b, a);

            if (swapped.hi != s.hi || swapped.lo != s.lo) {
                ERR("twoSum is not symmetric at (%.17g, %.17g)", a, b);
                return err;
            }
        }

        /* --- fast-two-sum, on correctly ordered operands --- */
        {
            const double big = fabs(a) >= fabs(b) ? a : b;
            const double small = fabs(a) >= fabs(b) ? b : a;
            const varintDD s = varintDDFastTwoSum(big, small);

            ddAcc want = ddAccFromDouble(big);
            ddAccAdd(&want, ddExactFromDouble(small));

            const ddAcc got = ddAccFromDD(s);

            if (want.ok && got.ok && !ddAccEqual(&got, &want)) {
                ERR("fastTwoSum(%.17g, %.17g) is not exact", big, small);
                return err;
            }
        }

        /* --- two-diff --- */
        {
            const varintDD d = varintDDTwoDiff(a, b);

            ddAcc want = ddAccFromDouble(a);
            ddAccAdd(&want, ddExactFromDouble(-b));

            const ddAcc got = ddAccFromDD(d);

            if (want.ok && got.ok && !ddAccEqual(&got, &want)) {
                ERR("twoDiff(%.17g, %.17g) is not exact", a, b);
                return err;
            }
        }

        /* --- two-product --- */
        {
            const varintDD p = varintDDTwoProduct(a, b);

            ddAcc want = ddAccZero();
            ddAccAdd(&want, ddExactProduct(a, b));

            const ddAcc got = ddAccFromDD(p);

            if (want.ok && got.ok) {
                verifiedProduct++;

                if (!ddAccEqual(&got, &want)) {
                    ERR("twoProduct(%.17g, %.17g) is not exact", a, b);
                    return err;
                }

                if (p.hi != a * b) {
                    ERR("twoProduct(%.17g, %.17g) leading limb is not the "
                        "rounded product",
                        a, b);
                    return err;
                }
            }
        }

        /* --- two-square --- */
        {
            const varintDD q = varintDDTwoSquare(a);

            ddAcc want = ddAccZero();
            ddAccAdd(&want, ddExactProduct(a, a));

            const ddAcc got = ddAccFromDD(q);

            if (want.ok && got.ok && !ddAccEqual(&got, &want)) {
                ERR("twoSquare(%.17g) is not exact", a);
                return err;
            }
        }
    }

    /* A generator that drifted out of the oracle's range would make
     * every check above vacuous, so require real coverage. */
    if (verifiedSum < (size_t)iterations / 2 ||
        verifiedProduct < (size_t)iterations / 2) {
        ERR("oracle verified too few cases (sums %zu, products %zu of %d)",
            verifiedSum, verifiedProduct, iterations);
        return err;
    }

    printf("\tverified exactly: %zu sums, %zu products\n", verifiedSum,
           verifiedProduct);
    return err;
}

/* --------------------------------------------------------------------
 * Test: transformations hold across the whole exponent range
 * --------------------------------------------------------------------
 * The oracle's fixed scale cannot reach the extremes of the double
 * range, so these cases are checked against the defining invariants
 * instead: the leading limb is the correctly rounded result, and the
 * trailing limb sits entirely below its last place. That second
 * property is exactly what varintDDStream's exponent-gap coding
 * depends on. */

static int ddTestTransformInvariantsWideRange(void) {
    int err = 0;

    TEST("transformations stay normalized across the full exponent range");

    for (int i = 0; i < 200000; i++) {
        const double a = ddRandDouble(-500, 500);
        const double b = ddRandDouble(-500, 500);
        const varintDD s = varintDDTwoSum(a, b);

        if (!isfinite(s.hi)) {
            continue;
        }

        if (s.hi != a + b) {
            ERR("twoSum leading limb wrong at (%.17g, %.17g)", a, b);
            return err;
        }

        if (s.hi + s.lo != s.hi) {
            ERR("twoSum trailing limb is not below the ulp at (%.17g, %.17g)",
                a, b);
            return err;
        }

        if (s.hi != 0.0 && s.lo != 0.0) {
            /* |lo| <= ulp(hi)/2 */
            const double ulpHalf = ldexp(fabs(s.hi), -53);

            if (fabs(s.lo) > ulpHalf) {
                ERR("twoSum violates |lo| <= ulp(hi)/2 at (%.17g, %.17g)", a,
                    b);
                return err;
            }
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: products near the top and bottom of the exponent range
 * --------------------------------------------------------------------
 * The exact oracle cannot reach these magnitudes, so this checks the
 * defining invariants instead - which is enough, because the failure
 * mode here is not a lost bit but an infinity.
 *
 * Dekker's splitting multiply forms a_hi*b_hi from halves that round
 * outward, so that intermediate can exceed the true product. Within a
 * factor of 2^27 of DBL_MAX it saturates and the error term returns
 * NaN. Hardware FMA cannot fail this way, so the whole class of bug is
 * invisible on a machine that has one - and an FMA-capable compiler
 * may contract the splitting expression back into an fmsub and hide it
 * even when the fallback is selected. Exercising this path therefore
 * needs -DVARINT_DD_FORCE_NO_FMA together with -ffp-contract=off, or a
 * target without FMA such as generic x86-64. */

static int ddTestExtremeMagnitudeProducts(void) {
    int err = 0;

    static const double extremes[] = {
        1.7976931348623157e308, /* DBL_MAX */
        -1.7976931348623157e308,
        8.98846567431158e307, /* DBL_MAX / 2 */
        1e308,
        1e300,
        1e290,
        6.69692879491417e299, /* the splitting threshold itself */
        1.3e300,
        2.2250738585072014e-308, /* DBL_MIN */
        5e-324,                  /* smallest denormal */
        1e-300,
        1.0,
        3.0,
    };

    const size_t count = sizeof(extremes) / sizeof(extremes[0]);

    TEST("exact products stay finite across the whole exponent range");

    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            const double a = extremes[i];
            const double b = extremes[j];
            const varintDD p = varintDDTwoProduct(a, b);

            /* An overflowing intermediate shows up as a non-finite
             * error term sitting under a perfectly finite product. */
            if (isfinite(p.hi) && !isfinite(p.lo)) {
                ERR("twoProduct(%.17g, %.17g) gave a finite product with a "
                    "non-finite error term",
                    a, b);
                return err;
            }

            if (!isfinite(p.hi)) {
                continue; /* genuine overflow; nothing to check */
            }

            if (p.hi != a * b) {
                ERR("twoProduct(%.17g, %.17g) leading limb is not the rounded "
                    "product",
                    a, b);
                return err;
            }

            /* The trailing limb must sit below the leading one's last
             * place, which a poisoned error term never does. */
            if (p.hi + p.lo != p.hi) {
                ERR("twoProduct(%.17g, %.17g) trailing limb is not below the "
                    "ulp",
                    a, b);
                return err;
            }
        }

        /* Same treatment for the squaring shortcut. */
        {
            const double a = extremes[i];
            const varintDD q = varintDDTwoSquare(a);

            if (isfinite(q.hi) && !isfinite(q.lo)) {
                ERR("twoSquare(%.17g) gave a finite product with a "
                    "non-finite error term",
                    a);
                return err;
            }

            if (isfinite(q.hi) && (q.hi != a * a || q.hi + q.lo != q.hi)) {
                ERR("twoSquare(%.17g) is not normalized", a);
                return err;
            }
        }
    }

    /* And the composed operations built on top of them.
     *
     * Denormals are excluded deliberately rather than accidentally:
     * halving one loses its last bit or underflows to zero outright,
     * so it cannot come back. That is IEEE arithmetic behaving
     * correctly, not the codec failing, and asserting otherwise would
     * be testing a false claim. */
    for (size_t i = 0; i < count; i++) {
        if (fabs(extremes[i]) < 2.2250738585072014e-308) {
            continue;
        }

        const varintDD v = varintDDFromDouble(extremes[i]);
        const varintDD half = varintDDDivDouble(v, 2.0);
        const varintDD back = varintDDMulDouble(half, 2.0);

        if (!varintDDIsFinite(half) || !varintDDIsFinite(back)) {
            ERR("halving and doubling %.17g left a non-finite value",
                extremes[i]);
            return err;
        }

        if (varintDDToDouble(back) != extremes[i]) {
            ERR("halving then doubling %.17g did not return it", extremes[i]);
            return err;
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: composed operations meet their error bounds
 * -------------------------------------------------------------------- */

static int ddTestArithmeticAccuracy(void) {
    int err = 0;

    /* Joldes, Muller & Popescu (2017) prove a relative bound of
     * 3u^2/(1-4u) for this addition algorithm and 5u^2 for this
     * multiplication, with u = 2^-53. Both are given headroom here
     * rather than being tuned down to whatever this build produces. */
    const double addBound = ldexp(1.0, -103);
    const double mulBound = ldexp(1.0, -103);
    size_t checkedAdd = 0;
    size_t checkedMul = 0;
    double worstAdd = 0.0;
    double worstMul = 0.0;

    TEST("double-double add and multiply meet their error bounds");

    for (int i = 0; i < 100000; i++) {
        const varintDD a = ddRandDD(-25, 25);
        const varintDD b = ddRandDD(-25, 25);

        /* --- accurate add --- */
        {
            const ddAcc want = ddAccFromDDSum(a, b);
            const double rel = ddAccRelativeError(varintDDAdd(a, b), &want);

            if (!isnan(rel)) {
                checkedAdd++;

                if (rel > worstAdd) {
                    worstAdd = rel;
                }

                if (rel > addBound) {
                    ERR("varintDDAdd relative error %.3e exceeds %.3e", rel,
                        addBound);
                    return err;
                }
            }
        }

        /* --- a - a is exactly zero --- */
        {
            const varintDD zero = varintDDSub(a, a);

            if (zero.hi != 0.0 || zero.lo != 0.0) {
                ERR("a - a is not zero (%.17g, %.17g)", zero.hi, zero.lo);
                return err;
            }
        }

        /* --- multiply --- */
        {
            const ddAcc want = ddAccFromDDProduct(a, b);
            const double rel = ddAccRelativeError(varintDDMul(a, b), &want);

            if (!isnan(rel)) {
                checkedMul++;

                if (rel > worstMul) {
                    worstMul = rel;
                }

                if (rel > mulBound) {
                    ERR("varintDDMul relative error %.3e exceeds %.3e", rel,
                        mulBound);
                    return err;
                }
            }
        }
    }

    if (checkedAdd < 10000 || checkedMul < 10000) {
        ERR("bounds checked on too few cases (add %zu, mul %zu)", checkedAdd,
            checkedMul);
        return err;
    }

    printf("\tworst relative error: add %.3e (%zu cases), mul %.3e (%zu "
           "cases)\n",
           worstAdd, checkedAdd, worstMul, checkedMul);
    return err;
}

/* --------------------------------------------------------------------
 * Test: the cheap add stays close to the accurate one
 * --------------------------------------------------------------------
 * varintDDAddFast folds the trailing limbs in with a plain double add,
 * which costs accuracy exactly when the leading limbs cancel. This
 * pins down what it is still guaranteed to deliver: same-sign operands
 * cannot cancel, so there it must match the accurate form to full
 * double-double precision. */

static int ddTestCheapAddBound(void) {
    int err = 0;
    double worst = 0.0;
    size_t checked = 0;

    TEST("cheap add matches the accurate add when nothing cancels");

    for (int i = 0; i < 100000; i++) {
        const varintDD a = varintDDAbs(ddRandDD(-25, 25));
        const varintDD b = varintDDAbs(ddRandDD(-25, 25));

        const ddAcc want = ddAccFromDDSum(a, b);
        const double rel = ddAccRelativeError(varintDDAddFast(a, b), &want);

        if (isnan(rel)) {
            continue;
        }

        checked++;

        if (rel > worst) {
            worst = rel;
        }

        if (rel > ldexp(1.0, -100)) {
            ERR("varintDDAddFast relative error %.3e on same-sign operands",
                rel);
            return err;
        }
    }

    if (checked < 10000) {
        ERR("cheap add checked on too few cases (%zu)", checked);
        return err;
    }

    printf("\tworst relative error: %.3e over %zu same-sign cases\n", worst,
           checked);
    return err;
}

/* --------------------------------------------------------------------
 * Test: divide and square root
 * -------------------------------------------------------------------- */

static int ddTestDivideAndSqrt(void) {
    int err = 0;
    const double bound = ldexp(1.0, -100);

    TEST("divide and square root round-trip to double-double precision");

    for (int i = 0; i < 50000; i++) {
        const varintDD a = ddRandDD(-25, 25);
        varintDD b = ddRandDD(-25, 25);

        if (b.hi == 0.0) {
            b = varintDDFromDouble(1.0);
        }

        /* (a / b) * b must return to a */
        {
            const varintDD back = varintDDMul(varintDDDiv(a, b), b);
            const varintDD diff = varintDDSub(back, a);

            if (a.hi != 0.0) {
                const double rel = fabs(varintDDToDouble(diff)) / fabs(a.hi);

                if (rel > bound) {
                    ERR("(a/b)*b relative error %.3e exceeds %.3e", rel, bound);
                    return err;
                }
            }
        }

        /* sqrt(|a|)^2 must return to |a| */
        {
            const varintDD positive = varintDDAbs(a);
            const varintDD root = varintDDSqrt(positive);
            const varintDD back = varintDDSquare(root);
            const varintDD diff = varintDDSub(back, positive);

            if (positive.hi != 0.0) {
                const double rel =
                    fabs(varintDDToDouble(diff)) / fabs(positive.hi);

                if (rel > bound) {
                    ERR("sqrt(a)^2 relative error %.3e exceeds %.3e", rel,
                        bound);
                    return err;
                }
            }
        }
    }

    /* Edge cases the random loop will not produce. */
    {
        const varintDD zeroRoot = varintDDSqrt(varintDDZero());

        if (zeroRoot.hi != 0.0 || zeroRoot.lo != 0.0) {
            ERRR("sqrt(0) is not zero");
            return err;
        }

        if (!varintDDIsNaN(varintDDSqrt(varintDDFromDouble(-1.0)))) {
            ERRR("sqrt of a negative value is not NaN");
            return err;
        }

        if (isfinite(varintDDDiv(varintDDFromDouble(1.0), varintDDZero()).hi)) {
            ERRR("division by zero produced a finite result");
            return err;
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: int64 conversion is exact past 2^53
 * -------------------------------------------------------------------- */

static int ddTestInt64Conversion(void) {
    int err = 0;

    static const int64_t edges[] = {0,
                                    1,
                                    -1,
                                    9007199254740992LL, /* 2^53 */
                                    9007199254740993LL, /* 2^53 + 1 */
                                    -9007199254740993LL,
                                    4611686018427387903LL,
                                    INT64_MAX,
                                    INT64_MIN,
                                    INT64_MAX - 1,
                                    INT64_MIN + 1};

    TEST("every int64 converts to double-double without loss");

    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        ddAcc want = ddAccZero();
        ddAccAdd(&want, ddExactFromInt64(edges[i]));

        const ddAcc got = ddAccFromDD(varintDDFromInt64(edges[i]));

        if (!ddAccEqual(&got, &want)) {
            ERR("varintDDFromInt64(%" PRId64 ") lost precision",
                (int64_t)edges[i]);
            return err;
        }
    }

    for (int i = 0; i < 200000; i++) {
        const int64_t v = (int64_t)ddRand64();

        ddAcc want = ddAccZero();
        ddAccAdd(&want, ddExactFromInt64(v));

        const ddAcc got = ddAccFromDD(varintDDFromInt64(v));

        if (!ddAccEqual(&got, &want)) {
            ERR("varintDDFromInt64(%" PRId64 ") lost precision", (int64_t)v);
            return err;
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: compensated reductions
 * -------------------------------------------------------------------- */

static int ddTestReductions(void) {
    int err = 0;

    TEST("compensated reductions recover what naive summation loses");

    /* The textbook catastrophe: a huge value, many small ones, then the
     * huge value again with the opposite sign. Naive double summation
     * annihilates every small term. */
    {
        enum { N = 10003 };
        static double values[N];

        values[0] = 1e16;
        values[N - 1] = -1e16;

        for (int i = 1; i < N - 1; i++) {
            values[i] = 1.0;
        }

        double naive = 0.0;

        for (int i = 0; i < N; i++) {
            naive += values[i];
        }

        const varintDD compensated = varintDDSumDoubles(values, N);
        const double expected = (double)(N - 2);

        if (varintDDToDouble(compensated) != expected) {
            ERR("compensated sum gave %.17g, expected %.17g",
                varintDDToDouble(compensated), expected);
            return err;
        }

        if (naive == expected) {
            ERRR("naive summation was expected to fail here but did not; the "
                 "compiler may be reassociating the reduction");
            return err;
        }

        printf("\tnaive %.17g vs compensated %.17g (exact %.17g)\n", naive,
               varintDDToDouble(compensated), expected);
    }

    /* Random arrays checked against the exact integer sum. */
    {
        enum { N = 20000 };
        static double values[N];

        double worstRatio = 0.0;

        for (size_t trial = 0; trial < 20; trial++) {
            ddAcc want = ddAccZero();
            ddAcc magnitudes = ddAccZero();

            for (int i = 0; i < N; i++) {
                const ddExact term = ddExactFromDouble(ddRandDouble(-10, 10));

                values[i] = ldexp((double)term.mant, term.exp);
                ddAccAdd(&want, term);
                ddAccAdd(&magnitudes, ddExactAbs(term));
            }

            if (!want.ok || !magnitudes.ok) {
                ERRR("oracle overflowed while summing");
                return err;
            }

            const double got =
                ddAccAbsoluteError(varintDDSumDoubles(values, N), &want);
            const double bound = ddAccReductionBound(&magnitudes, N);

            if (isnan(got) || got > bound) {
                ERR("varintDDSumDoubles absolute error %.3e exceeds bound %.3e",
                    got, bound);
                return err;
            }

            if (bound > 0.0 && got / bound > worstRatio) {
                worstRatio = got / bound;
            }
        }

        printf("\tsum of %d: worst error was %.4f of the proven bound\n", N,
               worstRatio);
    }

    /* Dot product: the oracle multiplies before summing, which checks
     * that each product's discarded tail survived into the
     * accumulator rather than being rounded away. */
    {
        enum { N = 8000 };
        static double a[N];
        static double b[N];

        double worstRatio = 0.0;

        for (size_t trial = 0; trial < 20; trial++) {
            ddAcc want = ddAccZero();
            ddAcc magnitudes = ddAccZero();

            for (int i = 0; i < N; i++) {
                a[i] = ddRandDouble(-6, 6);
                b[i] = ddRandDouble(-6, 6);

                const ddExact term = ddExactProduct(a[i], b[i]);

                ddAccAdd(&want, term);
                ddAccAdd(&magnitudes, ddExactAbs(term));
            }

            if (!want.ok || !magnitudes.ok) {
                ERRR("oracle overflowed while accumulating the dot product");
                return err;
            }

            const double got =
                ddAccAbsoluteError(varintDDDotDoubles(a, b, N), &want);
            const double bound = ddAccReductionBound(&magnitudes, N);

            if (isnan(got) || got > bound) {
                ERR("varintDDDotDoubles absolute error %.3e exceeds bound %.3e",
                    got, bound);
                return err;
            }

            if (bound > 0.0 && got / bound > worstRatio) {
                worstRatio = got / bound;
            }
        }

        printf("\tdot of %d: worst error was %.4f of the proven bound\n", N,
               worstRatio);
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: the vector paths agree with the scalar ones
 * --------------------------------------------------------------------
 * Elementwise operations run the identical instruction sequence in
 * every lane, so agreement must be BIT EXACT, not approximate.
 * Reductions legitimately differ because each lane accumulates its own
 * partial, so those go to the oracle instead.
 *
 * Counts sweep every remainder class so the scalar tail that follows
 * each vector loop is exercised at every backend width. */

static int ddTestVectorAgreesWithScalar(void) {
    int err = 0;

    enum { MAX = 64 };
    static varintDD a[MAX];
    static varintDD b[MAX];
    static varintDD vector[MAX];
    static varintDD scalar[MAX];

    TEST_DESC("vector backend (%s, %zu lanes) matches scalar bit for bit",
              varintDDBackend(), varintDDBackendLanes());

    for (size_t count = 0; count <= MAX; count++) {
        for (size_t trial = 0; trial < 60; trial++) {
            for (size_t i = 0; i < count; i++) {
                a[i] = ddRandDD(-20, 20);
                b[i] = ddRandDD(-20, 20);
            }

            /* --- elementwise add --- */
            varintDDAddArray(vector, a, b, count);

            for (size_t i = 0; i < count; i++) {
                scalar[i] = varintDDAdd(a[i], b[i]);
            }

            for (size_t i = 0; i < count; i++) {
                if (vector[i].hi != scalar[i].hi ||
                    vector[i].lo != scalar[i].lo) {
                    ERR("varintDDAddArray differs from scalar at count %zu "
                        "index %zu",
                        count, i);
                    return err;
                }
            }

            /* --- elementwise multiply --- */
            varintDDMulArray(vector, a, b, count);

            for (size_t i = 0; i < count; i++) {
                scalar[i] = varintDDMul(a[i], b[i]);
            }

            for (size_t i = 0; i < count; i++) {
                if (vector[i].hi != scalar[i].hi ||
                    vector[i].lo != scalar[i].lo) {
                    ERR("varintDDMulArray differs from scalar at count %zu "
                        "index %zu",
                        count, i);
                    return err;
                }
            }

            /* --- writing over an input must stay correct --- */
            memcpy(vector, a, count * sizeof(varintDD));
            varintDDAddArray(vector, vector, b, count);

            for (size_t i = 0; i < count; i++) {
                scalar[i] = varintDDAdd(a[i], b[i]);
            }

            for (size_t i = 0; i < count; i++) {
                if (vector[i].hi != scalar[i].hi ||
                    vector[i].lo != scalar[i].lo) {
                    ERR("varintDDAddArray is not alias-safe at count %zu "
                        "index %zu",
                        count, i);
                    return err;
                }
            }

            /* --- reduction, against the oracle --- */
            {
                ddAcc want = ddAccZero();
                ddAcc magnitudes = ddAccZero();

                for (size_t i = 0; i < count; i++) {
                    ddAccAdd(&want, ddExactFromDouble(a[i].hi));
                    ddAccAdd(&want, ddExactFromDouble(a[i].lo));
                    ddAccAdd(&magnitudes,
                             ddExactAbs(ddExactFromDouble(a[i].hi)));
                    ddAccAdd(&magnitudes,
                             ddExactAbs(ddExactFromDouble(a[i].lo)));
                }

                const varintDD got = varintDDSumArray(a, count);

                if (count == 0) {
                    if (got.hi != 0.0 || got.lo != 0.0) {
                        ERRR("varintDDSumArray of an empty array is not zero");
                        return err;
                    }
                } else if (want.ok && magnitudes.ok && !ddAccIsZero(&want)) {
                    const double absErr = ddAccAbsoluteError(got, &want);
                    const double bound =
                        ddAccReductionBound(&magnitudes, count);

                    if (isnan(absErr) || absErr > bound) {
                        ERR("varintDDSumArray error %.3e exceeds bound %.3e at "
                            "count %zu",
                            absErr, bound, count);
                        return err;
                    }
                }
            }
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: limb split/join and the nonzero bitmap
 * -------------------------------------------------------------------- */

static int ddTestLimbHelpers(void) {
    int err = 0;

    enum { MAX = 64 };
    static varintDD source[MAX];
    static varintDD rebuilt[MAX];
    static double hi[MAX];
    static double lo[MAX];
    static uint8_t bitmap[(MAX + 7) / 8];

    TEST("limb deinterleave, reinterleave, and the nonzero bitmap");

    for (size_t count = 0; count <= MAX; count++) {
        for (size_t trial = 0; trial < 60; trial++) {
            size_t expectedSet = 0;

            for (size_t i = 0; i < count; i++) {
                switch (ddRand64() % 5) {
                case 0:
                    /* exactly representable: trailing limb is +0.0 */
                    source[i] = varintDDFromDouble(ddRandDouble(-20, 20));
                    break;
                case 1:
                    /* negative zero must register as present so the
                     * codec round-trips its sign bit */
                    source[i] = (varintDD){ddRandDouble(-20, 20), -0.0};
                    break;
                default:
                    source[i] = ddRandDD(-20, 20);
                    break;
                }

                uint64_t raw;
                memcpy(&raw, &source[i].lo, sizeof(raw));

                if (raw != 0) {
                    expectedSet++;
                }
            }

            varintDDSplitLimbs(source, hi, lo, count);

            for (size_t i = 0; i < count; i++) {
                uint64_t got;
                uint64_t want;

                if (hi[i] != source[i].hi) {
                    ERR("split lost the leading limb at index %zu", i);
                    return err;
                }

                memcpy(&got, &lo[i], sizeof(got));
                memcpy(&want, &source[i].lo, sizeof(want));

                if (got != want) {
                    ERR("split lost the trailing limb bit pattern at index %zu",
                        i);
                    return err;
                }
            }

            varintDDJoinLimbs(rebuilt, hi, lo, count);

            if (count > 0 &&
                memcmp(rebuilt, source, count * sizeof(varintDD)) != 0) {
                ERR("join did not restore the array at count %zu", count);
                return err;
            }

            const size_t setBits = varintDDNonzeroLimbMask(lo, count, bitmap);

            if (setBits != expectedSet) {
                ERR("nonzero mask counted %zu, expected %zu at count %zu",
                    setBits, expectedSet, count);
                return err;
            }

            for (size_t i = 0; i < count; i++) {
                uint64_t raw;
                bool marked;

                memcpy(&raw, &lo[i], sizeof(raw));
                marked = (bitmap[i / 8] & (uint8_t)(1U << (i % 8))) != 0;

                if (marked != (raw != 0)) {
                    ERR("nonzero mask wrong at index %zu (count %zu)", i,
                        count);
                    return err;
                }
            }
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: decimal conversion
 * --------------------------------------------------------------------
 * Two independent references, because a printer checked against itself
 * proves nothing:
 *
 *   Integers get an exact reference for free. A double-double holds
 *   every int64 exactly, and printf("%" PRId64 "") is an exact decimal
 *   conversion, so the digits must match to the last one. That pins
 *   down digit extraction, the borrow repair, and rounding carries.
 *
 *   Arbitrary values are checked by reading the output back with
 *   strtod. A normalized double-double rounds to its own leading limb
 *   by definition, so a correctly printed decimal must round back to
 *   exactly that double. */

static int ddTestToString(void) {
    int err = 0;

    TEST("decimal conversion matches an exact reference");

    /* --- specials --- */
    {
        char text[VARINT_DD_STRING_MAX];

        varintDDToString(text, sizeof(text), varintDDZero(), 0);

        if (strcmp(text, "0") != 0) {
            ERR("zero printed as \"%s\"", text);
            return err;
        }

        varintDDToString(text, sizeof(text), (varintDD){-0.0, 0.0}, 0);

        if (strcmp(text, "-0") != 0) {
            ERR("negative zero printed as \"%s\"", text);
            return err;
        }

        varintDDToString(text, sizeof(text),
                         varintDDFromDouble((double)INFINITY), 0);

        if (strcmp(text, "inf") != 0) {
            ERR("infinity printed as \"%s\"", text);
            return err;
        }

        varintDDToString(text, sizeof(text),
                         varintDDFromDouble(-(double)INFINITY), 0);

        if (strcmp(text, "-inf") != 0) {
            ERR("negative infinity printed as \"%s\"", text);
            return err;
        }

        varintDDToString(text, sizeof(text), varintDDFromDouble((double)NAN),
                         0);

        if (strcmp(text, "nan") != 0) {
            ERR("NaN printed as \"%s\"", text);
            return err;
        }

        /* a buffer that cannot hold the result must be refused, not
         * partially filled */
        char tiny[4];

        if (varintDDToString(tiny, sizeof(tiny), varintDDFromDouble(1.5), 20) !=
            0) {
            ERRR("conversion into an undersized buffer was not refused");
            return err;
        }
    }

    /* --- integers, digit for digit against printf --- */
    {
        static const int64_t edges[] = {1,
                                        -1,
                                        7,
                                        10,
                                        99,
                                        100,
                                        999999999999999999LL,
                                        9007199254740993LL, /* 2^53 + 1 */
                                        -9007199254740993LL,
                                        123456789012345678LL};

        for (size_t trial = 0; trial < 40000; trial++) {
            int64_t v;

            if (trial < sizeof(edges) / sizeof(edges[0])) {
                v = edges[trial];
            } else {
                /* stay inside 18 digits so the value fits comfortably
                 * within the digits we ask for */
                v = (int64_t)(ddRand64() % 1000000000000000000ULL);

                if (ddRand64() & 1) {
                    v = -v;
                }

                if (v == 0) {
                    continue;
                }
            }

            char got[VARINT_DD_STRING_MAX];
            char reference[32];

            varintDDToString(got, sizeof(got), varintDDFromInt64(v), 25);
            snprintf(reference, sizeof(reference), "%" PRId64 "", (int64_t)v);

            /* Build the expected scientific form from the exact decimal:
             * first digit, point, the rest zero-padded to 25, exponent. */
            const char *refDigits = reference + (v < 0 ? 1 : 0);
            const size_t refLength = strlen(refDigits);

            char want[VARINT_DD_STRING_MAX];
            size_t at = 0;

            if (v < 0) {
                want[at++] = '-';
            }

            want[at++] = refDigits[0];
            want[at++] = '.';

            for (size_t i = 1; i < 25; i++) {
                want[at++] = i < refLength ? refDigits[i] : '0';
            }

            snprintf(want + at, sizeof(want) - at, "e%+03d",
                     (int)refLength - 1);

            if (strcmp(got, want) != 0) {
                ERR("printing %" PRId64 " gave \"%s\", expected \"%s\"",
                    (int64_t)v, got, want);
                return err;
            }
        }
    }

    /* --- the extremes of the exponent range ---
     * Scaling a value into [1, 10) multiplies or divides by a power of
     * ten, and near the ends of the double range that power is itself
     * close to overflowing. These are the cases where a naive scaling
     * step silently produces an infinity and prints nonsense. */
    {
        static const double extremes[] = {
            1.7976931348623157e308, /* DBL_MAX */
            -1.7976931348623157e308,
            2.2250738585072014e-308, /* DBL_MIN, smallest normal */
            -2.2250738585072014e-308,
            5e-324, /* smallest denormal */
            -5e-324,
            1e-320, /* mid denormal range */
            1e300,
            1e-300,
            1e-310,
        };

        for (size_t i = 0; i < sizeof(extremes) / sizeof(extremes[0]); i++) {
            const varintDD v = varintDDFromDouble(extremes[i]);
            char text[VARINT_DD_STRING_MAX];

            if (varintDDToString(text, sizeof(text), v, 30) == 0) {
                ERR("conversion failed on %.17g", extremes[i]);
                return err;
            }

            const double back = strtod(text, NULL);

            if (back != extremes[i]) {
                ERR("%.17g printed as \"%s\", which reads back as %.17g",
                    extremes[i], text, back);
                return err;
            }
        }
    }

    /* --- arbitrary values must read back to the same double --- */
    {
        size_t checked = 0;

        for (size_t trial = 0; trial < 100000; trial++) {
            const varintDD v = ddRandDD(-60, 60);

            if (v.hi == 0.0 || !isfinite(v.hi)) {
                continue;
            }

            char text[VARINT_DD_STRING_MAX];

            if (varintDDToString(text, sizeof(text), v, 30) == 0) {
                ERRR("conversion failed on a finite value");
                return err;
            }

            if (strtod(text, NULL) != v.hi) {
                ERR("\"%s\" read back as %.17g, expected %.17g", text,
                    strtod(text, NULL), v.hi);
                return err;
            }

            checked++;
        }

        if (checked < 50000) {
            ERR("round trip checked on too few values (%zu)", checked);
            return err;
        }
    }

    /* --- the trailing limb has to be visible in the output, which is
     *     the entire point of printing more than 17 digits --- */
    {
        const varintDD third =
            varintDDDiv(varintDDFromDouble(1.0), varintDDFromDouble(3.0));
        char text[VARINT_DD_STRING_MAX];

        varintDDToString(text, sizeof(text), third, 30);

        /* 1/3 to 30 places is 0.333... with the 3s running well past
         * the 17 digits a double could have carried */
        if (strncmp(text, "3.33333333333333333333333333333e-01", 20) != 0) {
            ERR("1/3 printed as \"%s\"", text);
            return err;
        }

        printf("\t1/3 to 30 digits: %s\n", text);
    }

    return err;
}

/* --------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------- */

int varintDDTest(int argc, char *argv[]) {
    /* Ordered from the primitives outward. A failure early on makes
     * every later result meaningless, so the run stops at the first one
     * rather than burying the root cause in follow-on noise. */
    static int (*const suites[])(void) = {
        ddTestErrorFreeTransforms,
        ddTestTransformInvariantsWideRange,
        ddTestExtremeMagnitudeProducts,
        ddTestArithmeticAccuracy,
        ddTestCheapAddBound,
        ddTestDivideAndSqrt,
        ddTestInt64Conversion,
        ddTestReductions,
        ddTestToString,
        ddTestVectorAgreesWithScalar,
        ddTestLimbHelpers,
    };

    int err = 0;

    ddRandState = argc > 1 ? strtoull(argv[1], NULL, 10) : 0x5DEECE66DULL;

    printf("varintDD: backend %s (%zu lanes), hardware FMA %s, seed %" PRIu64
           "\n",
           varintDDBackend(), varintDDBackendLanes(),
           VARINT_DD_HAS_FMA ? "yes" : "no (Dekker splitting)",
           (uint64_t)ddRandState);

    TEST("compiled arithmetic preserves IEEE semantics");

    if (!varintDDSelfCheck()) {
        ERRR("varintDDSelfCheck failed: the error-free transformations did "
             "not survive compilation. Check for -ffast-math, "
             "-fassociative-math, or a non-default rounding mode.");
        return err;
    }

    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        err += suites[i]();

        if (err) {
            break;
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* VARINT_DD_TEST */
