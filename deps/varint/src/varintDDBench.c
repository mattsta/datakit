/* ====================================================================
 * varintDDBench - throughput and accuracy benchmark for varintDD
 * ====================================================================
 * Not a correctness test (see varintDDTest / varintDDStreamTest /
 * varintDDStreamFuzz). This measures the two costs that actually
 * decide whether double-double is usable, and they are not the same
 * number:
 *
 *   LATENCY   a dependent chain, acc = op(acc, x). This is what an
 *             iterative kernel feels - a Mandelbrot escape loop, an
 *             ODE step, a Newton iteration - because each operation
 *             waits on the previous one. Published double-double
 *             figures are this measure, and SIMD cannot help it: the
 *             chain is serial no matter how many lanes exist.
 *
 *   THROUGHPUT independent work over arrays. This is what bulk
 *             processing feels, it is where the vector backend pays
 *             off, and it is also where memory bandwidth starts to
 *             matter - a double-double array is twice the bytes of a
 *             double array, so part of any slowdown is traffic rather
 *             than arithmetic.
 *
 * Reporting only one of them would flatter the result. Both are here.
 *
 * Usage: varintDDBench [values-per-run] [repeats]
 *   defaults: 1 << 16 values, 9 repeats (median reported) */

/* clock_gettime/CLOCK_MONOTONIC are POSIX, hidden by glibc under strict
 * -std=c11 unless requested. (Apple exposes them unconditionally.) */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "varintDDStream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rngState_ = 88172645463325252ULL;

static uint64_t rng_(void) {
    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return rngState_ >> 16;
}

static double now_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int cmpDouble_(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_(double *samples, size_t n) {
    qsort(samples, n, sizeof(double), cmpDouble_);
    return samples[n / 2];
}

/* Somewhere for results to go that the optimizer cannot reason about,
 * so none of the loops below can be deleted as dead code. */
static volatile double benchSink;

/* Double-double arithmetic is branch-free straight-line floating point,
 * which is exactly the shape an auto-vectorizer recognizes. At -O3
 * clang compiles a plain `for (i) dst[i] = varintDDAdd(a[i], b[i])`
 * loop into the same ld2.2d/fadd.2d NEON sequence varintDDAddArray
 * writes by hand, so the "obvious" comparison - hand-written vector
 * code against a scalar-looking loop - is really vector against vector
 * and reports a speedup of roughly 1.0 no matter how good either side
 * is. Suppressing vectorization on one loop with a pragma does not fix
 * this either: it also disables interleaving, and the resulting
 * baseline measures the optimizer sulking rather than the cost of
 * scalar arithmetic.
 *
 * The measurement that means something is a build-level one. Compile
 * this benchmark twice, once normally and once with
 * -DVARINT_DD_FORCE_SCALAR, and compare the two runs: identical source,
 * identical flags, the only difference being whether the vector backend
 * exists. That is what the CMake target pair and the note printed at
 * the end of the run are for. */

#define BENCH_MAX_REPEATS 64

/* Run `body` `repeats` times and report the median seconds per pass. */
#define BENCH(label, repeats, body)                                            \
    do {                                                                       \
        static double samples_[BENCH_MAX_REPEATS];                             \
        for (size_t r_ = 0; r_ < (repeats); r_++) {                            \
            const double start_ = now_();                                      \
            body;                                                              \
            samples_[r_] = now_() - start_;                                    \
        }                                                                      \
        (label) = median_(samples_, (repeats));                                \
    } while (0)

/* --------------------------------------------------------------------
 * Latency: dependent chains
 * -------------------------------------------------------------------- */

static void benchLatency(size_t count, size_t repeats) {
    double *plain = malloc(count * sizeof(double));
    varintDD *wide = malloc(count * sizeof(varintDD));

    if (plain == NULL || wide == NULL) {
        printf("out of memory\n");
        free(plain);
        free(wide);
        return;
    }

    /* Values near one keep every chain well conditioned, so nothing is
     * measuring a denormal or infinity slow path by accident. */
    for (size_t i = 0; i < count; i++) {
        plain[i] = 1.0 + (double)(rng_() % 1000) * 1e-9;
        wide[i] = varintDDFromDouble(plain[i]);
    }

    double tDoubleAdd = 0.0;
    double tWideAdd = 0.0;
    double tDoubleMul = 0.0;
    double tWideMul = 0.0;
    double tDoubleDiv = 0.0;
    double tWideDiv = 0.0;

    BENCH(tDoubleAdd, repeats, {
        double acc = 0.0;
        for (size_t i = 0; i < count; i++) {
            acc = acc + plain[i];
        }
        benchSink = acc;
    });

    BENCH(tWideAdd, repeats, {
        varintDD acc = varintDDZero();
        for (size_t i = 0; i < count; i++) {
            acc = varintDDAdd(acc, wide[i]);
        }
        benchSink = varintDDToDouble(acc);
    });

    BENCH(tDoubleMul, repeats, {
        double acc = 1.0;
        for (size_t i = 0; i < count; i++) {
            acc = acc * plain[i];
        }
        benchSink = acc;
    });

    BENCH(tWideMul, repeats, {
        varintDD acc = varintDDFromDouble(1.0);
        for (size_t i = 0; i < count; i++) {
            acc = varintDDMul(acc, wide[i]);
        }
        benchSink = varintDDToDouble(acc);
    });

    BENCH(tDoubleDiv, repeats, {
        double acc = 1.0;
        for (size_t i = 0; i < count; i++) {
            acc = acc / plain[i];
        }
        benchSink = acc;
    });

    BENCH(tWideDiv, repeats, {
        varintDD acc = varintDDFromDouble(1.0);
        for (size_t i = 0; i < count; i++) {
            acc = varintDDDiv(acc, wide[i]);
        }
        benchSink = varintDDToDouble(acc);
    });

    const double scale = 1e9 / (double)count;

    printf("\nLatency (dependent chain, ns per operation)\n");
    printf("  %-14s %10s %10s %8s\n", "operation", "double", "double2",
           "ratio");
    printf("  %-14s %10.3f %10.3f %7.1fx\n", "add", tDoubleAdd * scale,
           tWideAdd * scale, tWideAdd / tDoubleAdd);
    printf("  %-14s %10.3f %10.3f %7.1fx\n", "multiply", tDoubleMul * scale,
           tWideMul * scale, tWideMul / tDoubleMul);
    printf("  %-14s %10.3f %10.3f %7.1fx\n", "divide", tDoubleDiv * scale,
           tWideDiv * scale, tWideDiv / tDoubleDiv);

    free(plain);
    free(wide);
}

/* --------------------------------------------------------------------
 * Throughput: independent work, scalar against vector
 * -------------------------------------------------------------------- */

static void benchThroughput(size_t count, size_t repeats) {
    /* Elementwise double-double work touches 48 bytes per value across
     * three arrays. Over any array worth the name that saturates memory
     * bandwidth long before it saturates the FPU, and no amount of
     * widening the arithmetic speeds up a loop that is waiting on RAM -
     * measured that way, the vector backend looks like a regression.
     *
     * To measure the arithmetic rather than the memory system, the
     * working set is held inside L1 and swept repeatedly until the same
     * total number of values has gone through. The per-pass write to
     * the volatile sink is what stops the optimizer from noticing that
     * every pass computes the same thing and keeping only the last. */
    const size_t working = count < 512 ? count : 512;
    const size_t passes = count / working ? count / working : 1;
    const size_t total = working * passes;

    double *plainA = malloc(working * sizeof(double));
    double *plainB = malloc(working * sizeof(double));
    double *plainOut = malloc(working * sizeof(double));
    varintDD *wideA = malloc(working * sizeof(varintDD));
    varintDD *wideB = malloc(working * sizeof(varintDD));
    varintDD *wideOut = malloc(working * sizeof(varintDD));

    if (plainA == NULL || plainB == NULL || plainOut == NULL || wideA == NULL ||
        wideB == NULL || wideOut == NULL) {
        printf("out of memory\n");
        goto done;
    }

    for (size_t i = 0; i < working; i++) {
        plainA[i] = 1.0 + (double)(rng_() % 100000) * 1e-7;
        plainB[i] = 1.0 + (double)(rng_() % 100000) * 1e-7;
        wideA[i] =
            varintDDMul(varintDDFromDouble(plainA[i]), varintDDFromDouble(3.0));
        wideB[i] =
            varintDDMul(varintDDFromDouble(plainB[i]), varintDDFromDouble(7.0));
    }

    double tPlain = 0.0;
    double tAutoAdd = 0.0;
    double tVectorAdd = 0.0;
    double tAutoMul = 0.0;
    double tVectorMul = 0.0;

    BENCH(tPlain, repeats, {
        for (size_t p = 0; p < passes; p++) {
            for (size_t i = 0; i < working; i++) {
                plainOut[i] = plainA[i] + plainB[i];
            }
            benchSink = plainOut[p % working];
        }
    });

    BENCH(tAutoAdd, repeats, {
        for (size_t p = 0; p < passes; p++) {
            for (size_t i = 0; i < working; i++) {
                wideOut[i] = varintDDAdd(wideA[i], wideB[i]);
            }
            benchSink = wideOut[p % working].hi;
        }
    });

    BENCH(tVectorAdd, repeats, {
        for (size_t p = 0; p < passes; p++) {
            varintDDAddArray(wideOut, wideA, wideB, working);
            benchSink = wideOut[p % working].hi;
        }
    });

    BENCH(tAutoMul, repeats, {
        for (size_t p = 0; p < passes; p++) {
            for (size_t i = 0; i < working; i++) {
                wideOut[i] = varintDDMul(wideA[i], wideB[i]);
            }
            benchSink = wideOut[p % working].hi;
        }
    });

    BENCH(tVectorMul, repeats, {
        for (size_t p = 0; p < passes; p++) {
            varintDDMulArray(wideOut, wideA, wideB, working);
            benchSink = wideOut[p % working].hi;
        }
    });

    const double scale = 1e9 / (double)total;

    printf("\nThroughput (independent, L1-resident, ns per value) - backend "
           "%s, %zu lanes\n",
           varintDDBackend(), varintDDBackendLanes());

    printf("  %-10s %11s %11s %12s\n", "operation", "plain loop", "backend",
           "vs double");
    printf("  %-10s %11.3f %11.3f %11.1fx\n", "add", tAutoAdd * scale,
           tVectorAdd * scale, tVectorAdd / tPlain);
    printf("  %-10s %11.3f %11.3f %11.1fx\n", "multiply", tAutoMul * scale,
           tVectorMul * scale, tVectorMul / tPlain);
    printf("  plain double add: %.3f ns/value (same loop, half the bytes)\n",
           tPlain * scale);
    printf("  'plain loop' is ordinary C the compiler may vectorize on its "
           "own; 'backend' is varintDD*Array.\n");
    printf("  To measure what SIMD contributes, rebuild with "
           "-DVARINT_DD_FORCE_SCALAR and compare runs.\n");

done:
    free(plainA);
    free(plainB);
    free(plainOut);
    free(wideA);
    free(wideB);
    free(wideOut);
}

/* --------------------------------------------------------------------
 * Reductions: cost and accuracy together
 * -------------------------------------------------------------------- */

static void benchReductions(size_t count, size_t repeats) {
    double *values = malloc(count * sizeof(double));

    if (values == NULL) {
        printf("out of memory\n");
        return;
    }

    /* An ill-conditioned sum: large values that cancel, with small ones
     * in between that naive summation cannot see. */
    for (size_t i = 0; i < count; i++) {
        values[i] = (i % 2) ? 1e16 : -1e16 + 1.0;
    }

    double tNaive = 0.0;
    double tCompensated = 0.0;
    double naiveResult = 0.0;

    BENCH(tNaive, repeats, {
        double acc = 0.0;
        for (size_t i = 0; i < count; i++) {
            acc += values[i];
        }
        naiveResult = acc;
        benchSink = acc;
    });

    BENCH(tCompensated, repeats,
          { benchSink = varintDDToDouble(varintDDSumDoubles(values, count)); });

    const varintDD compensated = varintDDSumDoubles(values, count);

    /* The array pairs -1e16+1.0 with 1e16, so each complete pair
     * contributes exactly 1.0 and an odd trailing element contributes
     * nothing measurable. The truncation here is the intended count of
     * whole pairs, not a lost fraction. */
    const size_t survivingPairs = count / 2;
    const double exact = (double)survivingPairs;
    const double scale = 1e9 / (double)count;

    printf("\nReduction over %zu ill-conditioned values\n", count);
    printf("  %-24s %10.3f ns/value  result %.17g\n", "naive double sum",
           tNaive * scale, naiveResult);
    printf("  %-24s %10.3f ns/value  result %.17g\n", "compensated (two-sum)",
           tCompensated * scale, varintDDToDouble(compensated));
    printf("  %-24s %10s             exact  %.17g\n", "", "", exact);
    printf("  cost of exactness: %.2fx, error removed: %.17g\n",
           tCompensated / tNaive, fabs(naiveResult - exact));

    free(values);
}

/* --------------------------------------------------------------------
 * Codec: throughput and what it actually saves
 * -------------------------------------------------------------------- */

/* Each shape isolates one of the two mechanisms, so a number can be
 * attributed rather than admired:
 *   exact     trailing limbs are all zero, magnitudes unrelated - the
 *             bitmap does all the work, the XOR chain does none
 *   generic   full trailing mantissas, unrelated magnitudes - neither
 *             mechanism has anything to grip, the honest floor
 *   smooth    full trailing mantissas, correlated magnitudes - the XOR
 *             chain works, the bitmap does not
 *   sensor    zero trailing limbs AND correlated magnitudes - both fire
 *             at once, which is what real instrument data looks like
 *   constant  one repeated value - the ceiling */
typedef enum benchShape {
    BENCH_SHAPE_EXACT,
    BENCH_SHAPE_GENERIC,
    BENCH_SHAPE_SMOOTH,
    BENCH_SHAPE_SENSOR,
    BENCH_SHAPE_CONSTANT,
    BENCH_SHAPE_COUNT
} benchShape;

static void benchFill(varintDD *values, size_t count, benchShape shape) {
    double walk = 1000.0;

    for (size_t i = 0; i < count; i++) {
        switch (shape) {
        case BENCH_SHAPE_EXACT:
            /* A full 52-bit significand spread across many exponents,
             * so consecutive values share no bits and the leading-limb
             * chain has nothing to remove. A generator with only 20
             * significant bits would leave the low mantissa zeroed and
             * quietly hand the XOR chain a win it has not earned. */
            values[i] = varintDDFromDouble(
                ldexp(1.0 + (double)(rng_() & 0xFFFFFFFFFFFFFULL) /
                                4503599627370496.0,
                      (int)(rng_() % 60) - 30));
            break;

        case BENCH_SHAPE_SENSOR:
            walk += (double)(int64_t)(rng_() % 64) - 32.0;
            values[i] = varintDDFromDouble(walk * 0.5);
            break;

        case BENCH_SHAPE_GENERIC:
            values[i] = varintDDDiv(
                varintDDFromDouble((double)(rng_() % 1000000) + 1.0),
                varintDDFromDouble((double)(rng_() % 999) + 3.0));
            break;

        case BENCH_SHAPE_SMOOTH:
            walk += (double)(int64_t)(rng_() % 200) * 1e-6;
            values[i] = varintDDMul(varintDDFromDouble(walk),
                                    varintDDFromDouble(1.0000000001));
            break;

        case BENCH_SHAPE_CONSTANT:
        default:
            values[i] =
                varintDDDiv(varintDDFromDouble(1.0), varintDDFromDouble(3.0));
            break;
        }
    }
}

static const char *benchShapeName(benchShape shape) {
    static const char *const names[BENCH_SHAPE_COUNT] = {
        "exact", "generic", "smooth", "sensor", "constant"};
    return names[shape];
}

static void benchCodec(size_t count, size_t repeats) {
    varintDD *source = malloc(count * sizeof(varintDD));
    varintDD *decoded = malloc(count * sizeof(varintDD));
    uint8_t *buffer = malloc(varintDDStreamMaxSize(count));

    if (source == NULL || decoded == NULL || buffer == NULL) {
        printf("out of memory\n");
        goto done;
    }

    printf("\nCodec, lossless (%zu values per run)\n", count);
    printf("  %-10s %9s %8s %11s %11s %8s\n", "shape", "bytes/val", "ratio",
           "encode", "decode", "mode");

    for (uint32_t shape = 0; shape < BENCH_SHAPE_COUNT; shape++) {
        benchFill(source, count, (benchShape)shape);

        varintDDStreamMeta meta;
        double tEncode = 0.0;
        double tDecode = 0.0;
        size_t written = 0;

        BENCH(tEncode, repeats, {
            written = varintDDStreamEncode(buffer, source, count,
                                           VARINT_DD_STREAM_HI_AUTO,
                                           VARINT_DD_STREAM_LOSSLESS, &meta);
        });

        BENCH(tDecode, repeats, {
            benchSink =
                (double)varintDDStreamDecode(buffer, written, decoded, count);
        });

        if (memcmp(source, decoded, count * sizeof(varintDD)) != 0) {
            printf("  %-10s ROUND TRIP FAILED\n",
                   benchShapeName((benchShape)shape));
            continue;
        }

        const double sourceBytes = (double)(count * sizeof(varintDD));

        printf("  %-10s %9.2f %7.2fx %8.0f MB/s %8.0f MB/s %8s\n",
               benchShapeName((benchShape)shape),
               (double)written / (double)count, sourceBytes / (double)written,
               sourceBytes / tEncode / 1e6, sourceBytes / tDecode / 1e6,
               meta.hiMode == VARINT_DD_STREAM_HI_XOR ? "xor" : "raw");
    }

    /* The precision ladder is the codec's real lever on generic data,
     * so show what each rung costs and what it gives up. */
    printf("\n  Precision ladder on generic data\n");
    printf("  %-16s %9s %8s %14s\n", "trailing bits", "bytes/val", "ratio",
           "max rel error");

    benchFill(source, count, BENCH_SHAPE_GENERIC);

    static const uint8_t rungs[] = {52, 40, 30, 20, 10, 0};

    for (size_t i = 0; i < sizeof(rungs) / sizeof(rungs[0]); i++) {
        const size_t written = varintDDStreamEncode(
            buffer, source, count, VARINT_DD_STREAM_HI_AUTO, rungs[i], NULL);
        const double sourceBytes = (double)(count * sizeof(varintDD));
        const double error = varintDDStreamMaxRelativeError(rungs[i]);

        printf("  %-16" PRIu32 " %9.2f %7.2fx %14.3e\n", (uint32_t)rungs[i],
               (double)written / (double)count, sourceBytes / (double)written,
               error);
    }

done:
    free(source);
    free(decoded);
    free(buffer);
}

int main(int argc, char *argv[]) {
    const size_t count =
        argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : (size_t)1 << 16;
    size_t repeats = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 9;

    if (count == 0) {
        printf("value count must be positive\n");
        return 1;
    }

    if (repeats == 0 || repeats > BENCH_MAX_REPEATS) {
        repeats = 9;
    }

    printf("varintDD benchmark: %zu values, %zu repeats (median)\n", count,
           repeats);
    printf("backend %s (%zu lanes), hardware FMA %s\n", varintDDBackend(),
           varintDDBackendLanes(), VARINT_DD_HAS_FMA ? "yes" : "no");

    if (!varintDDSelfCheck()) {
        printf("\nvarintDDSelfCheck FAILED - this build does not preserve "
               "IEEE semantics, so every number below is meaningless.\n");
        return 1;
    }

    benchLatency(count, repeats);
    benchThroughput(count, repeats);
    benchReductions(count, repeats);
    benchCodec(count, repeats);

    return 0;
}
