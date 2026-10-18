#pragma once

#include "varint.h"
#include <math.h>

__BEGIN_DECLS

/* ====================================================================
 * Double-Double (106-bit) Arithmetic
 * ==================================================================== */
/* varint model Double-Double:
 *   Type represented by: an UNEVALUATED SUM of two IEEE-754 doubles
 *   Size: 16 bytes (two doubles), ~106 significand bits, ~31 decimal digits
 *   Layout: { double hi; double lo; }  with value == hi + lo
 *   Invariant (normalized): hi == (double)(hi + lo) and |lo| <= ulp(hi)/2
 *   Meaning: extended precision without leaving the FPU — every operation
 *            is a short straight-line sequence of hardware double ops
 *   Pros: ~2-9x the cost of double, versus ~20-50x for soft-float
 *           __float128 and ~80x for a bignum library at the same width
 *         No allocation, no global state, trivially vectorizable
 *         Exponent range is unchanged from double
 *   Cons: NOT correctly rounded — guarantees are relative error bounds
 *         Infinities degrade to NaN (error terms evaluate inf - inf)
 *         Full precision only above ~2e-292 (below that, lo goes denormal)
 *         Requires IEEE semantics: -ffast-math BREAKS the error-free
 *           transformations silently (see the guards below)
 *
 * This header is the arithmetic core. Compression of DD arrays lives in
 * varintDDStream.h, which exploits the normalization invariant above:
 * because |lo| <= ulp(hi)/2, the exponent FIELD of lo carries almost no
 * information given hi, and lo is exactly zero for any DD promoted from
 * a plain double.
 *
 * References:
 *   Dekker 1971, "A floating-point technique for extending the available
 *     precision" (fast-two-sum, the splitting multiply)
 *   Knuth TAOCP vol 2 (branch-free two-sum)
 *   Hida, Li & Bailey 2001, "Algorithms for quad-double precision floating
 *     point arithmetic" (the add/mul/div/sqrt sequences used here)
 *   Neumaier 1974 (the compensated-summation variant used for reductions) */

/* ====================================================================
 * Environment guards
 * ====================================================================
 * Every algorithm below depends on each individual double operation
 * being correctly rounded to double, and on the compiler NOT applying
 * real-number algebra to the expressions. Under -ffast-math the
 * sequence
 *      s = a + b;  bb = s - a;  aa = s - bb;  err = (a - aa) + (b - bb);
 * simplifies to err == 0 and the extra precision silently vanishes.
 *
 * These two guards catch the common cases at compile time.
 * varintDDSelfCheck() below catches the rest at runtime, including
 * flags this macro test cannot observe. */
#if defined(__FAST_MATH__)
#error "varintDD requires IEEE-754 semantics: -ffast-math / -Ofast break "     \
       "the error-free transformations. Compile this TU without them."
#endif

#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ == 2
#error "varintDD requires double operations to round to double. This target "  \
       "evaluates double in long double (x87). Use -mfpmath=sse or SSE2+."
#endif

/* ====================================================================
 * Type
 * ==================================================================== */

/* A double-double value. The represented number is exactly hi + lo.
 *
 * Every function here both consumes and produces NORMALIZED values
 * (|lo| <= ulp(hi)/2). Pairs assembled by hand are not necessarily
 * normalized; run them through varintDDNormalize() first. */
typedef struct varintDD {
    double hi; /* leading limb: the value rounded to double */
    double lo; /* trailing limb: the rounding error hi discarded */
} varintDD;

_Static_assert(sizeof(varintDD) == 16,
               "varintDD must be exactly two packed doubles! The SIMD "
               "deinterleave paths and varintDDStream's wire format both "
               "assume a 16-byte stride with no padding.");

/* ====================================================================
 * Error-free transformations
 * ====================================================================
 * These are the primitives. Each computes a result AND the exact amount
 * that result discarded, so that (hi + lo) reproduces the infinitely
 * precise answer with no error whatsoever. */

/* Knuth's two-sum: exact for ANY two doubles, no ordering requirement.
 * 6 flops, branch-free, no bit manipulation.
 * Postcondition: hi + lo == a + b exactly, and hi == (double)(a + b). */
static inline varintDD varintDDTwoSum(double a, double b) {
    const double sum = a + b;
    const double bKept = sum - a;     /* the part of b that survived */
    const double aKept = sum - bKept; /* the part of a that survived */
    const double aLost = a - aKept;
    const double bLost = b - bKept;
    return (varintDD){sum, aLost + bLost};
}

/* Dekker's fast-two-sum: exact only when |a| >= |b| (or either is zero).
 * 3 flops. The larger operand keeps all of its bits, so only the
 * smaller one can lose any, and one subtraction recovers the loss.
 *
 * This is the renormalization workhorse: after any sequence that leaves
 * a known-larger leading term, this is half the cost of two-sum. */
static inline varintDD varintDDFastTwoSum(double a, double b) {
    const double sum = a + b;
    const double bKept = sum - a;
    return (varintDD){sum, b - bKept};
}

/* Exact difference. */
static inline varintDD varintDDTwoDiff(double a, double b) {
    const double diff = a - b;
    const double bKept = diff - a;
    const double aKept = diff - bKept;
    const double aLost = a - aKept;
    const double bLost = -b - bKept;
    return (varintDD){diff, aLost + bLost};
}

/* Building with -DVARINT_DD_FORCE_NO_FMA selects Dekker's splitting
 * multiply on a machine that has hardware FMA. That exists to be used:
 * a generic x86-64 build has no FMA unless -mfma or -march=native is
 * passed, so the splitting path is what most Linux builds actually
 * run - and it is invisible on an arm64 developer machine, where FMA
 * is always present. Without a way to select it locally the fallback
 * ships untested. */
#if defined(VARINT_DD_FORCE_NO_FMA)
#define VARINT_DD_HAS_FMA 0
#elif defined(__FP_FAST_FMA) || defined(FP_FAST_FMA)
#define VARINT_DD_HAS_FMA 1
#else
#define VARINT_DD_HAS_FMA 0
#endif

#if !VARINT_DD_HAS_FMA
/* Dekker's splitting constant: 2^27 + 1. Multiplying by this and
 * subtracting isolates the top 26 bits of a 53-bit significand, so the
 * two halves multiply exactly. */
#define VARINT_DD_SPLITTER 134217729.0

/* Operands at or below this can be split and multiplied without any
 * intermediate leaving the double range. 2^996 keeps 28 binary orders
 * of headroom, which covers both the splitting constant and the
 * outward rounding of the halves. */
#define VARINT_DD_SPLIT_THRESHOLD 0x1p996

/* Largest operand whose SQUARE is still under the threshold. */
#define VARINT_DD_SQUARE_THRESHOLD 0x1p498

/* Split a into two 26-bit halves whose sum is exactly a.
 *
 * PRECONDITION: |a| <= VARINT_DD_SPLIT_THRESHOLD. Above that,
 * SPLITTER * a overflows and both halves come back NaN. Callers bring
 * their operands into range first rather than having this scale
 * internally: scaling here would have to scale the halves back out
 * again, and the high half - which rounds outward - can exceed the
 * original magnitude and overflow on the way. That is a real bug, not
 * a hypothetical; it is what made DBL_MAX * DBL_MIN return a finite
 * product with a NaN error term. */
static inline varintDD varintDDSplit_(double a) {
    const double t = VARINT_DD_SPLITTER * a;
    const double high = t - (t - a);
    return (varintDD){high, a - high};
}

/* Dekker's product for operands whose result is clear of overflow. */
static inline varintDD varintDDTwoProductSplit_(double a, double b,
                                                double product) {
    const varintDD as = varintDDSplit_(a);
    const varintDD bs = varintDDSplit_(b);
    const double err =
        ((as.hi * bs.hi - product) + as.hi * bs.lo + as.lo * bs.hi) +
        as.lo * bs.lo;

    return (varintDD){product, err};
}
#endif

/* Exact product: hi + lo == a * b exactly.
 * With hardware FMA this is 2 flops — the FMA evaluates a*b - product
 * with a single rounding, which IS the discarded 53-bit tail.
 * Without it, Dekker's splitting method costs 17. */
static inline varintDD varintDDTwoProduct(double a, double b) {
    const double product = a * b;
#if VARINT_DD_HAS_FMA
    return (varintDD){product, fma(a, b, -product)};
#else
    /* Dekker's method forms a_hi*b_hi, and because the 26-bit halves
     * round outward that intermediate can exceed the true product. Once
     * the product is within a factor of 2^27 of DBL_MAX the
     * intermediate saturates to infinity and the error term comes back
     * NaN - so a perfectly finite multiply near the top of the range
     * silently produces garbage.
     *
     * Scaling both operands down first avoids it. The scaling is exact
     * (powers of two) and so is undoing it, because the error term is
     * far from both range limits.
     *
     * Hardware FMA has none of this trouble, which is exactly why the
     * defect is invisible on any machine that has one - and why an
     * FMA-capable compiler may CONTRACT the expression above back into
     * an fmsub and hide it again. Testing this path honestly needs both
     * VARINT_DD_FORCE_NO_FMA and -ffp-contract=off. */
    double sa = a;
    double sb = b;
    int shift = 0;

    /* Bring the product into range first: the cross term a_hi*b_hi can
     * exceed the true product because the halves round outward. */
    if (product > VARINT_DD_SPLIT_THRESHOLD ||
        product < -VARINT_DD_SPLIT_THRESHOLD) {
        sa *= 0x1p-27;
        sb *= 0x1p-27;
        shift += 54;
    }

    /* Then each operand, so the splitting constant cannot overflow.
     * This is checked AFTER the product scaling because that step can
     * itself leave an operand above the threshold. */
    if (sa > VARINT_DD_SPLIT_THRESHOLD || sa < -VARINT_DD_SPLIT_THRESHOLD) {
        sa *= 0x1p-53;
        shift += 53;
    }

    if (sb > VARINT_DD_SPLIT_THRESHOLD || sb < -VARINT_DD_SPLIT_THRESHOLD) {
        sb *= 0x1p-53;
        shift += 53;
    }

    if (shift == 0) {
        return varintDDTwoProductSplit_(a, b, product);
    }

    /* Every scale factor is a power of two, so the scaled product is
     * exactly the real one shifted and the recovered error scales back
     * without loss. */
    const varintDD scaled = varintDDTwoProductSplit_(sa, sb, sa * sb);
    return (varintDD){product, ldexp(scaled.lo, shift)};
#endif
}

/* Exact square. Cheaper than the general product off the FMA path
 * because only one split is needed. */
static inline varintDD varintDDTwoSquare(double a) {
    const double product = a * a;
#if VARINT_DD_HAS_FMA
    return (varintDD){product, fma(a, a, -product)};
#else
    /* Same overflow hazard as varintDDTwoProduct. Rather than repeat
     * the scaling logic, hand the rare large case to the general
     * product, which already gets it right; the fast path below is
     * then guaranteed an operand whose square is in range. */
    if (a > VARINT_DD_SQUARE_THRESHOLD || a < -VARINT_DD_SQUARE_THRESHOLD) {
        return varintDDTwoProduct(a, a);
    }

    const varintDD as = varintDDSplit_(a);
    const double err =
        ((as.hi * as.hi - product) + 2.0 * as.hi * as.lo) + as.lo * as.lo;
    return (varintDD){product, err};
#endif
}

/* ====================================================================
 * Construction and conversion
 * ==================================================================== */

static inline varintDD varintDDFromDouble(double a) {
    return (varintDD){a, 0.0};
}

static inline varintDD varintDDZero(void) {
    return (varintDD){0.0, 0.0};
}

/* Nearest double to the represented value. Note this is just hi: for a
 * normalized value, hi is BY DEFINITION the value rounded to double. */
static inline double varintDDToDouble(const varintDD a) {
    return a.hi;
}

/* Restore the normalization invariant for a hand-assembled pair.
 * Every arithmetic entry point below assumes its inputs are normalized;
 * this is how you get there from arbitrary limbs. */
static inline varintDD varintDDNormalize(const varintDD a) {
    return varintDDFastTwoSum(a.hi, a.lo);
}

/* Exact conversion from a 64-bit integer. Doubles hold integers exactly
 * only to 2^53; a double-double holds every int64 exactly. */
static inline varintDD varintDDFromInt64(int64_t v) {
    /* Split at 2^32 so both halves are exactly representable. */
    const int64_t high = v >> 32;
    const uint64_t low = (uint64_t)v & 0xFFFFFFFFULL;
    const varintDD scaled = varintDDTwoProduct((double)high, 4294967296.0);
    const varintDD sum = varintDDTwoSum(scaled.hi, (double)low);
    return varintDDFastTwoSum(sum.hi, sum.lo + scaled.lo);
}

/* ====================================================================
 * Arithmetic
 * ==================================================================== */

static inline varintDD varintDDNegate(const varintDD a) {
    return (varintDD){-a.hi, -a.lo};
}

/* Accurate addition (Hida-Li-Bailey "IEEE" add): ~20 flops.
 * Relative error below 3 * 2^-106 for all inputs, including the
 * catastrophic-cancellation cases the cheap variant mishandles. */
static inline varintDD varintDDAdd(const varintDD a, const varintDD b) {
    varintDD s = varintDDTwoSum(a.hi, b.hi);
    const varintDD t = varintDDTwoSum(a.lo, b.lo);

    s.lo += t.hi;
    s = varintDDFastTwoSum(s.hi, s.lo);

    s.lo += t.lo;
    return varintDDFastTwoSum(s.hi, s.lo);
}

/* Cheap addition ("sloppy" add): ~11 flops, nearly half the cost.
 * Error bound degrades when the leading limbs cancel almost exactly,
 * because the low limbs are folded in with a plain double add. Safe
 * for accumulating same-sign values; prefer varintDDAdd otherwise. */
static inline varintDD varintDDAddFast(const varintDD a, const varintDD b) {
    varintDD s = varintDDTwoSum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return varintDDFastTwoSum(s.hi, s.lo);
}

static inline varintDD varintDDSub(const varintDD a, const varintDD b) {
    return varintDDAdd(a, varintDDNegate(b));
}

/* Mixed precision add: strictly cheaper than promoting b first. */
static inline varintDD varintDDAddDouble(const varintDD a, double b) {
    varintDD s = varintDDTwoSum(a.hi, b);
    s.lo += a.lo;
    return varintDDFastTwoSum(s.hi, s.lo);
}

/* Multiplication: one exact product of the leading limbs, plus the two
 * cross terms. The lo*lo term is below 2^-106 relative and is dropped,
 * which is what keeps this at ~5x double rather than ~12x. */
static inline varintDD varintDDMul(const varintDD a, const varintDD b) {
    varintDD p = varintDDTwoProduct(a.hi, b.hi);

#if VARINT_DD_HAS_FMA
    /* The two cross terms are folded with a single rounding. Spelling
     * the FMA out rather than leaving it to the compiler's contraction
     * pass matters for more than accuracy: it is what keeps this and
     * the vectorized path in varintDD.c bit-identical, whatever
     * -ffp-contract happens to be set to. Vector intrinsics are never
     * contracted, so an implicit FMA here and an explicit multiply
     * there would silently disagree in the last bit. */
    p.lo += fma(a.hi, b.lo, a.lo * b.hi);
#else
    p.lo += a.hi * b.lo + a.lo * b.hi;
#endif

    return varintDDFastTwoSum(p.hi, p.lo);
}

static inline varintDD varintDDMulDouble(const varintDD a, double b) {
    varintDD p = varintDDTwoProduct(a.hi, b);
    p.lo += a.lo * b;
    return varintDDFastTwoSum(p.hi, p.lo);
}

static inline varintDD varintDDSquare(const varintDD a) {
    varintDD p = varintDDTwoSquare(a.hi);
    p.lo += 2.0 * a.hi * a.lo;
    return varintDDFastTwoSum(p.hi, p.lo);
}

/* Division. No error-free transformation exists for division, so this
 * is long division: take a double-precision quotient digit, subtract
 * its exact contribution at full double-double width, repeat. Three
 * digits reach ~106 bits. */
static inline varintDD varintDDDiv(const varintDD a, const varintDD b) {
    const double q1 = a.hi / b.hi;
    if (!isfinite(q1)) {
        return (varintDD){q1, 0.0};
    }

    /* r = a - q1*b */
    varintDD r = varintDDSub(a, varintDDMulDouble(b, q1));

    const double q2 = r.hi / b.hi;
    r = varintDDSub(r, varintDDMulDouble(b, q2));

    const double q3 = r.hi / b.hi;

    const varintDD q = varintDDFastTwoSum(q1, q2);
    return varintDDAddDouble(q, q3);
}

static inline varintDD varintDDDivDouble(const varintDD a, double b) {
    return varintDDDiv(a, varintDDFromDouble(b));
}

/* Square root by one Newton refinement of the hardware root.
 * Accurate to roughly 1 ulp at double-double width, not correctly
 * rounded. Negative input yields NaN; zero yields zero. */
static inline varintDD varintDDSqrt(const varintDD a) {
    if (a.hi == 0.0) {
        return varintDDZero();
    }

    if (a.hi < 0.0) {
        return (varintDD){(double)NAN, 0.0};
    }

    const double approxInv = 1.0 / sqrt(a.hi);
    const double approx = a.hi * approxInv;

    /* residual = a - approx^2, evaluated at full width */
    const varintDD sq = varintDDTwoSquare(approx);
    const varintDD residual = varintDDSub(a, sq);
    const double correction = residual.hi * approxInv * 0.5;

    return varintDDFastTwoSum(approx, correction);
}

static inline varintDD varintDDAbs(const varintDD a) {
    return a.hi < 0.0 ? varintDDNegate(a) : a;
}

/* ====================================================================
 * Comparison
 * ==================================================================== */

/* Returns <0, 0, >0. Undefined for NaN limbs, as with double. */
static inline int varintDDCompare(const varintDD a, const varintDD b) {
    if (a.hi < b.hi) {
        return -1;
    }

    if (a.hi > b.hi) {
        return 1;
    }

    if (a.lo < b.lo) {
        return -1;
    }

    if (a.lo > b.lo) {
        return 1;
    }

    return 0;
}

static inline bool varintDDIsZero(const varintDD a) {
    return a.hi == 0.0;
}

static inline bool varintDDIsNaN(const varintDD a) {
    return isnan(a.hi) || isnan(a.lo);
}

static inline bool varintDDIsFinite(const varintDD a) {
    return isfinite(a.hi) && isfinite(a.lo);
}

/* ====================================================================
 * Compensated summation accumulator
 * ====================================================================
 * For reductions specifically, full double-double addition is overkill.
 * Neumaier's variant of Kahan summation costs ~7 flops per element
 * versus ~20 for varintDDAdd, and for a REDUCTION it produces the same
 * practical accuracy, because the compensation term never needs to
 * carry forward more than one rounding's worth of information.
 *
 * Reach for varintDDAdd when precision must propagate through a chain
 * of dependent operations; reach for this when collapsing an array. */
typedef struct varintDDAccum {
    double sum;  /* running total */
    double comp; /* accumulated compensation */
} varintDDAccum;

static inline varintDDAccum varintDDAccumInit(void) {
    return (varintDDAccum){0.0, 0.0};
}

static inline void varintDDAccumAdd(varintDDAccum *acc, double value) {
    /* Neumaier's formulation branches on which operand is larger so it
     * can use the 3-flop fast-two-sum. Knuth's two-sum is exact for
     * EITHER ordering, so we pay 6 flops instead of 3-plus-a-branch and
     * get the identical result with nothing to mispredict. That also
     * leaves the loop free of control flow, which is what lets the SIMD
     * reductions in varintDD.c run one accumulator per lane. */
    const varintDD s = varintDDTwoSum(acc->sum, value);
    acc->sum = s.hi;
    acc->comp += s.lo;
}

static inline varintDD varintDDAccumResult(const varintDDAccum *acc) {
    return varintDDFastTwoSum(acc->sum, acc->comp);
}

/* Merge two independently-accumulated partials. This is what makes the
 * SIMD reductions in varintDD.c possible: each lane runs its own
 * accumulator and they combine at the end. */
static inline varintDDAccum varintDDAccumMerge(const varintDDAccum a,
                                               const varintDDAccum b) {
    varintDDAccum out = a;
    varintDDAccumAdd(&out, b.sum);
    out.comp += b.comp;
    return out;
}

/* ====================================================================
 * Text conversion
 * ====================================================================
 * printf has no conversion that can show a double-double: %g and %e
 * see one double and stop, so the entire trailing limb - the whole
 * reason the type exists - is invisible. Anything that prints a
 * double-double has to do the decimal conversion itself.
 *
 * Digits come out by repeated scaling in double-double arithmetic
 * rather than through a bignum, which is what keeps this allocation
 * free. The cost is the last digit: about 30 significant digits are
 * trustworthy, and asking for 31 may leave the final one off by one.
 * Exact printing of all ~31 digits needs arbitrary-precision integers,
 * which is a dependency this library does not take. */

/* Sign, leading digit, point, 31 digits, exponent, terminator. */
#define VARINT_DD_STRING_MAX 48
#define VARINT_DD_DIGITS_MAX 31
#define VARINT_DD_DIGITS_DEFAULT 30

/* Format value in scientific notation, e.g. "-1.66666...e-01".
 * digits: significant digits, 1 to VARINT_DD_DIGITS_MAX (0 selects the
 *         default). Values above 30 are not fully trustworthy.
 * Returns characters written excluding the terminator, or 0 if dst is
 * too small. VARINT_DD_STRING_MAX bytes is always enough. */
size_t varintDDToString(char *dst, size_t dstLen, varintDD value,
                        uint32_t digits);

/* ====================================================================
 * Array operations (vectorized — see varintDD.c)
 * ==================================================================== */

/* Name of the compiled-in SIMD backend: "AVX2", "NEON", or "scalar". */
const char *varintDDBackend(void);

/* Lanes processed per SIMD step (1 when scalar). */
size_t varintDDBackendLanes(void);

/* Elementwise dst = a + b (accurate add) and dst = a * b.
 * dst may alias a or b. */
void varintDDAddArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count);
void varintDDMulArray(varintDD *dst, const varintDD *a, const varintDD *b,
                      size_t count);

/* Reductions. The Doubles variants take a plain double array and are the
 * accurate replacement for a naive `for (i) sum += v[i]` loop. */
varintDD varintDDSumArray(const varintDD *values, size_t count);
varintDD varintDDSumDoubles(const double *values, size_t count);
varintDD varintDDDotDoubles(const double *a, const double *b, size_t count);

/* ====================================================================
 * Structure-of-arrays conversion
 * ====================================================================
 * varintDDStream compresses the hi and lo limbs with entirely different
 * strategies, so it needs them in separate arrays. These deinterleave
 * and reinterleave in place of a scalar copy loop. */
void varintDDSplitLimbs(const varintDD *src, double *hi, double *lo,
                        size_t count);
void varintDDJoinLimbs(varintDD *dst, const double *hi, const double *lo,
                       size_t count);

/* Set bit i of bitmap when lo[i] has a nonzero BIT PATTERN (so -0.0
 * counts as nonzero — the codec must round-trip its sign bit).
 * bitmap must hold at least (count + 7) / 8 bytes.
 * Returns the number of bits set. */
size_t varintDDNonzeroLimbMask(const double *lo, size_t count, uint8_t *bitmap);

/* ====================================================================
 * Self-check
 * ====================================================================
 * Verifies at RUNTIME that the error-free transformations actually
 * survived compilation. The #error guards above catch -ffast-math when
 * it is spelled in a way the preprocessor can see; this catches
 * everything else (individual -f flags, LTO-time reassociation, an
 * FPU left in a non-default rounding mode).
 *
 * Costs a few dozen flops. Call it once at startup in any program that
 * depends on double-double precision being real. */
bool varintDDSelfCheck(void);

#ifdef VARINT_DD_TEST
int varintDDTest(int argc, char *argv[]);
#endif

__END_DECLS
