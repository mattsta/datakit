/**
 * example_dd.c - Demonstrates varintDD usage
 *
 * varintDD is 106-bit floating point built from two IEEE doubles held
 * as an unevaluated sum (value == hi + lo), giving ~31 decimal digits
 * at roughly 5-15x the cost of a double - versus 20-50x for soft-float
 * __float128 and ~80x for a bignum library at the same width.
 *
 * Reach for it when double is not quite enough but a bignum is far too
 * much: long accumulations, ill-conditioned sums, iterative refinement,
 * deep fractal zooms, anything where 53 bits of significand runs out
 * before the algorithm does.
 *
 * Compile: gcc -I../../src example_dd.c ../../src/varintDD.c -lm -o example_dd
 * Run:     ./example_dd
 */

#include "varintDD.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convenience wrapper so examples can print inline. Not thread safe;
 * real code should pass its own buffer to varintDDToString. */
static const char *dd(varintDD value) {
    static char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), value, 30);
    return text;
}

/* ====================================================================
 * Example 1: what a double runs out of
 * ==================================================================== */
static void example_precision_loss(void) {
    printf("\n=== Example 1: Where double runs out ===\n\n");

    /* 0.1 is not representable in binary, so the "right" answer is not
     * 1000000.0 - it is one million times the double nearest 0.1. That
     * is the number both methods below should be producing. */
    enum { N = 10000000 };
    const double tenth = 0.1;

    double naive = 0.0;
    for (int i = 0; i < N; i++) {
        naive += tenth;
    }

    /* The exact answer, computed once at double-double width. */
    const varintDD exact =
        varintDDMulDouble(varintDDFromDouble(tenth), (double)N);

    printf("Adding the double nearest 0.1 to itself %d times:\n\n", N);
    printf("  exact answer      %s\n", dd(exact));
    printf("  naive double sum  %.17g\n", naive);
    printf("  error             %.3e\n\n",
           fabs(naive - varintDDToDouble(exact)));

    printf("  The error is not in the last bit. Ten million roundings\n");
    printf("  accumulated into something visible at the 10th digit.\n");
}

/* ====================================================================
 * Example 2: the error-free transformations
 * ==================================================================== */
static void example_error_free_transforms(void) {
    printf("\n=== Example 2: Recovering what a rounding threw away ===\n\n");

    /* Adding these two loses the smaller one entirely: the result needs
     * 61 significand bits and a double has 53. */
    const double large = 1.0;
    const double small = ldexp(1.0, -60); /* 2^-60 */

    const double rounded = large + small;
    const varintDD exact = varintDDTwoSum(large, small);

    printf("  a               %.17g\n", large);
    printf("  b               %.17g  (2^-60)\n", small);
    printf("  a + b as double %.17g   <- b vanished completely\n", rounded);
    printf("  twoSum hi       %.17g\n", exact.hi);
    printf("  twoSum lo       %.17g   <- exactly what the add discarded\n\n",
           exact.lo);

    printf("  hi + lo reproduces a + b with NO error at all. Six\n");
    printf("  operations, no branches, no bit twiddling. Every\n");
    printf("  double-double operation is built from this and twoProduct,\n");
    printf("  which does the same for multiplication using hardware FMA.\n");
}

/* ====================================================================
 * Example 3: arithmetic and printing
 * ==================================================================== */
static void example_arithmetic(void) {
    printf("\n=== Example 3: Arithmetic ===\n\n");

    const varintDD one = varintDDFromDouble(1.0);
    const varintDD three = varintDDFromDouble(3.0);
    const varintDD third = varintDDDiv(one, three);

    printf("  1/3            %s\n", dd(third));
    printf("  as a double    %.17g   <- printf stops at 17 digits\n\n",
           varintDDToDouble(third));

    /* Multiplying back is the standard sanity check: with 106 bits the
     * round trip lands within ~1e-32 of one, not ~1e-16. */
    const varintDD back = varintDDMul(third, three);
    const varintDD drift = varintDDSub(back, one);

    printf("  (1/3) * 3      %s\n", dd(back));
    printf("  error          %.3e\n", fabs(varintDDToDouble(drift)));
    printf("  same in double %.3e\n\n", fabs((1.0 / 3.0) * 3.0 - 1.0));

    const varintDD two = varintDDFromDouble(2.0);
    const varintDD root = varintDDSqrt(two);

    printf("  sqrt(2)        %s\n", dd(root));
    printf("  squared back   %s\n", dd(varintDDSquare(root)));

    printf("\n  NOTE: printf has no format for a double-double - %%g and\n");
    printf("  %%e see one limb and stop. Use varintDDToString, which is\n");
    printf("  trustworthy to about 30 significant digits.\n");
}

/* ====================================================================
 * Example 4: integers a double cannot hold
 * ==================================================================== */
static void example_exact_integers(void) {
    printf("\n=== Example 4: Exact int64 ===\n\n");

    /* A double holds integers exactly only up to 2^53. Past that they
     * start snapping to even values. A double-double holds every
     * int64 exactly. */
    const int64_t big = 9007199254740993LL; /* 2^53 + 1 */

    printf("  value              %lld  (2^53 + 1)\n", (long long)big);
    printf("  through a double   %.0f   <- rounded to the nearest even\n",
           (double)big);
    printf("  through varintDD   %s\n\n", dd(varintDDFromInt64(big)));

    /* Useful in practice: mixing large counters with fractional scales
     * without losing the counter. */
    const varintDD nanoseconds = varintDDFromInt64(1739462400123456789LL);
    const varintDD seconds = varintDDDivDouble(nanoseconds, 1000000000.0);

    printf("  epoch nanoseconds  %s\n", dd(nanoseconds));
    printf("  as seconds         %s\n", dd(seconds));
    printf("  as a double        %.17g   <- ~200ns of resolution gone\n",
           1739462400123456789.0 / 1e9);
}

/* ====================================================================
 * Example 5: reductions, the fast path
 * ==================================================================== */
static void example_reductions(void) {
    printf("\n=== Example 5: Compensated reductions ===\n\n");

    /* The textbook catastrophe: large values that cancel, with small
     * ones in between that naive summation cannot see. */
    enum { N = 100001 };
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

    printf("  %d values: 1e16, then %d ones, then -1e16\n\n", N, N - 2);
    printf("  correct answer    %d\n", N - 2);
    printf("  naive double sum  %.17g   <- every 1.0 was annihilated\n", naive);
    printf("  varintDDSumDoubles %s\n\n", dd(compensated));

    printf("  This is the cheapest thing in the library to adopt: it takes\n");
    printf("  a plain double array and returns a double-double, so nothing\n");
    printf("  upstream has to change. It also tends to run FASTER than the\n");
    printf("  naive loop it replaces - the naive version is one serial\n");
    printf("  dependency chain, while this one keeps a separate accumulator\n");
    printf(
        "  per SIMD lane, and that wins more than the compensation costs.\n");

    /* Dot products get the same treatment, and additionally recover the
     * tail of every multiply before it can be lost to the addition. */
    static double a[1000];
    static double b[1000];

    for (int i = 0; i < 1000; i++) {
        a[i] = 1.0 + (double)i * 1e-13;
        b[i] = 1.0 - (double)i * 1e-13;
    }

    double naiveDot = 0.0;
    for (int i = 0; i < 1000; i++) {
        naiveDot += a[i] * b[i];
    }

    printf("\n  dot product of 1000 near-one values:\n");
    printf("    naive             %.17g\n", naiveDot);
    printf("    varintDDDotDoubles %s\n", dd(varintDDDotDoubles(a, b, 1000)));
}

/* ====================================================================
 * Example 6: choosing between the two additions
 * ==================================================================== */
static void example_choosing_add(void) {
    printf("\n=== Example 6: varintDDAdd vs varintDDAddFast ===\n\n");

    /* The two forms differ by exactly one thing: the residual left over
     * when the two TRAILING limbs are added. The cheap form folds them
     * with a plain add and drops it; the accurate form recovers it with
     * another exact transform.
     *
     * That residual is around 2^-106 of the operands and is frequently
     * zero outright, so rather than assert a difference, measure one.
     * Real guidance comes from what the two actually do on real data. */
    enum { TRIALS = 200000 };

    uint64_t rng = 0x243F6A8885A308D3ULL;
    size_t sameSignDiffer = 0;
    size_t cancellingDiffer = 0;
    double worstCancelling = 0.0;

    for (int i = 0; i < TRIALS; i++) {
        /* cheap xorshift; any spread of bits will do here */
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;

        const double x = (double)(rng >> 11) * ldexp(1.0, -53) + 1.0;

        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;

        const double y = (double)(rng >> 11) * ldexp(1.0, -53) + 1.0;

        /* Case A: same sign, nothing can cancel. */
        {
            const varintDD a =
                varintDDDiv(varintDDFromDouble(x), varintDDFromDouble(3.0));
            const varintDD b =
                varintDDDiv(varintDDFromDouble(y), varintDDFromDouble(7.0));
            const varintDD acc = varintDDAdd(a, b);
            const varintDD fst = varintDDAddFast(a, b);

            if (acc.hi != fst.hi || acc.lo != fst.lo) {
                sameSignDiffer++;
            }
        }

        /* Case B: leading limbs engineered to annihilate, so whatever
         * the trailing limbs carry is the entire answer. */
        {
            const varintDD a =
                varintDDDiv(varintDDFromDouble(x), varintDDFromDouble(3.0));
            const varintDD other =
                varintDDDiv(varintDDFromDouble(y), varintDDFromDouble(7.0));
            const varintDD b =
                varintDDNormalize((varintDD){-a.hi, ldexp(other.lo, -1)});

            const varintDD acc = varintDDAdd(a, b);
            const varintDD fst = varintDDAddFast(a, b);

            if (acc.hi != fst.hi || acc.lo != fst.lo) {
                cancellingDiffer++;

                if (acc.hi != 0.0) {
                    const double gap =
                        fabs(varintDDToDouble(varintDDSub(acc, fst))) /
                        fabs(acc.hi);

                    if (gap > worstCancelling) {
                        worstCancelling = gap;
                    }
                }
            }
        }
    }

    printf("  Over %d random pairs, how often do the two disagree?\n\n",
           TRIALS);
    printf("    same sign, no cancellation   %6.2f%% of pairs\n",
           100.0 * (double)sameSignDiffer / TRIALS);
    printf("    leading limbs annihilate     %6.2f%% of pairs",
           100.0 * (double)cancellingDiffer / TRIALS);

    if (worstCancelling > 0.0) {
        printf(", worst gap %.2e", worstCancelling);
    }

    printf("\n\n");

    printf("  Read those two lines carefully - they say different things.\n\n");
    printf("  Same sign: the results differ in their last bit fairly\n");
    printf("  often, but every one of those differences is far below the\n");
    printf("  accurate form's own error bound. Nothing is lost.\n\n");
    printf("  Full cancellation: the gap reaches ~1e-16. That is not a\n");
    printf("  last-bit disagreement - it means the cheap form has fallen\n");
    printf("  back to roughly plain double precision, because the answer\n");
    printf("  lived entirely in the residual it discarded.\n\n");
    printf("  varintDDAdd     ~20 flops, bound holds under any cancellation\n");
    printf(
        "  varintDDAddFast ~11 flops, needs operands that cannot cancel\n\n");
    printf("  Rule of thumb: accumulating same-sign magnitudes, use Fast.\n");
    printf("  Anything that can subtract, use varintDDAdd. When in doubt\n");
    printf("  use varintDDAdd - it is the default for a reason, and the\n");
    printf("  cheap form's risk appears in exactly the case that is\n");
    printf("  hardest to predict from the call site.\n");
}

int main(void) {
    printf("========================================\n");
    printf("varintDD - 106-bit floating point\n");
    printf("========================================\n");

    /* BEST PRACTICE: check this once at startup, before anything
     * depends on the extra precision being real.
     *
     * Every double-double algorithm relies on each individual operation
     * rounding to double and on the compiler NOT applying real-number
     * algebra to the expressions. Under -ffast-math the error terms
     * simplify to zero, the type silently degrades to a slow double,
     * and nothing announces it. varintDD.h rejects the flag at compile
     * time when the preprocessor can see it; this catches the rest. */
    if (!varintDDSelfCheck()) {
        printf("\nvarintDDSelfCheck FAILED.\n");
        printf("This build does not preserve IEEE semantics - look for\n");
        printf("-ffast-math, -Ofast, -fassociative-math, or a non-default\n");
        printf("FPU rounding mode. Results would be silently wrong.\n");
        return 1;
    }

    printf("\nbackend: %s (%zu lanes)\n", varintDDBackend(),
           varintDDBackendLanes());

    example_precision_loss();
    example_error_free_transforms();
    example_arithmetic();
    example_exact_integers();
    example_reductions();
    example_choosing_add();

    printf("\n========================================\n");
    printf("See example_ddstream.c for storing these compactly.\n");
    printf("========================================\n");
    return 0;
}
