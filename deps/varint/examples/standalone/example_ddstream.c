/**
 * example_ddstream.c - Demonstrates varintDDStream usage
 *
 * varintDDStream compresses arrays of varintDD. The insight it trades
 * on: a normalized double-double satisfies |lo| <= ulp(hi)/2, so the
 * trailing limb's 11-bit exponent field carries almost no information
 * once you know the leading limb. Storing the GAP between the two
 * exponents instead costs ~2 bits, and any value promoted from a plain
 * double has a trailing limb of exactly zero, which costs one bit.
 *
 * Compile: gcc -I../../src example_ddstream.c ../../src/varintDDStream.c
 *          ../../src/varintDD.c ../../src/varintTagged.c -lm -o
 * example_ddstream Run:     ./example_ddstream
 */

#include "varintDDStream.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *dd(varintDD value) {
    static char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), value, 30);
    return text;
}

static uint64_t rngState = 0x9E3779B97F4A7C15ULL;

static uint64_t rng(void) {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return rngState;
}

/* ====================================================================
 * Example 1: the basic round trip
 * ==================================================================== */
static void example_round_trip(void) {
    printf("\n=== Example 1: Encode and decode ===\n\n");

    varintDD values[6];
    values[0] = varintDDDiv(varintDDFromDouble(1.0), varintDDFromDouble(3.0));
    values[1] = varintDDSqrt(varintDDFromDouble(2.0));
    values[2] = varintDDFromDouble(42.0); /* trailing limb is zero */
    values[3] = varintDDFromInt64(9007199254740993LL);
    values[4] = varintDDDiv(varintDDFromDouble(-22.0), varintDDFromDouble(7.0));
    values[5] = varintDDZero();

    const size_t count = sizeof(values) / sizeof(values[0]);

    /* ALWAYS size the buffer with varintDDStreamMaxSize. The worst case
     * is larger than the input, because a trailing limb that cannot be
     * gap-coded falls back to storing all 64 bits verbatim. */
    uint8_t *buffer = malloc(varintDDStreamMaxSize(count));

    varintDDStreamMeta meta;
    const size_t written =
        varintDDStreamEncode(buffer, values, count, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, &meta);

    varintDD decoded[6];
    const size_t got = varintDDStreamDecode(buffer, written, decoded, count);

    printf("  %zu values -> %zu bytes (%zu raw, %.2fx)\n", count, written,
           count * sizeof(varintDD),
           (double)(count * sizeof(varintDD)) / (double)written);
    printf("  decoded %zu values\n\n", got);

    for (size_t i = 0; i < count; i++) {
        printf("    [%zu] %-34s %s\n", i, dd(decoded[i]),
               memcmp(&values[i], &decoded[i], sizeof(varintDD)) == 0
                   ? "exact"
                   : "DIFFERS");
    }

    /* Lossless means bit-exact, not merely numerically equal: NaN
     * payloads and the sign of a zero survive too. memcmp is the right
     * check, == is not. */
    printf("\n  bit-exact round trip: %s\n",
           memcmp(values, decoded, count * sizeof(varintDD)) == 0 ? "yes"
                                                                  : "no");

    free(buffer);
}

/* ====================================================================
 * Example 2: where the compression comes from
 * ==================================================================== */
static void example_what_compresses(void) {
    printf("\n=== Example 2: What compresses, and what does not ===\n\n");

    enum { N = 20000 };
    static varintDD values[N];

    uint8_t *buffer = malloc(varintDDStreamMaxSize(N));
    const double raw = (double)(N * sizeof(varintDD));

    printf("  %-38s %10s %8s\n", "data shape", "bytes/val", "ratio");

    /* (a) Values promoted from plain doubles: trailing limb is zero, so
     *     it costs one bitmap bit and nothing else. */
    for (int i = 0; i < N; i++) {
        values[i] = varintDDFromDouble(
            ldexp((double)(rng() & 0xFFFFF) + 1.0, (int)(rng() % 40) - 20));
    }

    size_t written =
        varintDDStreamEncode(buffer, values, N, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, NULL);
    printf("  %-38s %10.2f %7.2fx\n", "exact values (trailing limb zero)",
           (double)written / N, raw / (double)written);

    /* (b) Real double-double results: full 52-bit trailing mantissas
     *     with unrelated magnitudes. This is the honest floor - a
     *     trailing mantissa is incompressible entropy. */
    for (int i = 0; i < N; i++) {
        values[i] =
            varintDDDiv(varintDDFromDouble((double)(rng() % 1000000) + 1.0),
                        varintDDFromDouble((double)(rng() % 997) + 3.0));
    }

    written = varintDDStreamEncode(buffer, values, N, VARINT_DD_STREAM_HI_AUTO,
                                   VARINT_DD_STREAM_LOSSLESS, NULL);
    printf("  %-38s %10.2f %7.2fx\n", "generic double-doubles",
           (double)written / N, raw / (double)written);

    /* (c) Instrument-style data: exactly representable readings that
     *     drift slowly. Both mechanisms fire - the trailing limbs are
     *     zero AND consecutive leading limbs share nearly all bits. */
    {
        double walk = 1013.25; /* millibars, say */

        for (int i = 0; i < N; i++) {
            walk += (double)(int64_t)(rng() % 21) * 0.01 - 0.1;
            values[i] = varintDDFromDouble(walk);
        }
    }

    varintDDStreamMeta meta;
    written = varintDDStreamEncode(buffer, values, N, VARINT_DD_STREAM_HI_AUTO,
                                   VARINT_DD_STREAM_LOSSLESS, &meta);
    printf("  %-38s %10.2f %7.2fx\n", "sensor readings (both apply)",
           (double)written / N, raw / (double)written);

    printf("\n  On that last one the encoder chose the %s leading-limb\n",
           meta.hiMode == VARINT_DD_STREAM_HI_XOR ? "XOR" : "verbatim");
    printf("  mode, and %zu of %d trailing limbs were exactly zero.\n",
           meta.exactValues, N);

    printf("\n  The lesson: lossless compression of GENERIC double-double\n");
    printf("  data is modest, because 52 bits of trailing mantissa cannot\n");
    printf("  be compressed by anything. The wins come from values that\n");
    printf("  never needed the extra precision, and from neighbours that\n");
    printf("  resemble each other. Example 3 is the lever for the rest.\n");

    free(buffer);
}

/* ====================================================================
 * Example 3: the precision ladder
 * ==================================================================== */
static void example_precision_ladder(void) {
    printf("\n=== Example 3: Trading precision for space ===\n\n");

    enum { N = 20000 };
    static varintDD values[N];
    static varintDD decoded[N];

    for (int i = 0; i < N; i++) {
        values[i] =
            varintDDDiv(varintDDFromDouble((double)(rng() % 1000000) + 1.0),
                        varintDDFromDouble((double)(rng() % 997) + 3.0));
    }

    uint8_t *buffer = malloc(varintDDStreamMaxSize(N));
    const double raw = (double)(N * sizeof(varintDD));

    printf("  %-14s %10s %8s %14s %14s\n", "trailing bits", "bytes/val",
           "ratio", "error bound", "worst seen");

    static const uint8_t rungs[] = {52, 40, 30, 20, 10, 0};

    for (size_t r = 0; r < sizeof(rungs) / sizeof(rungs[0]); r++) {
        const size_t written = varintDDStreamEncode(
            buffer, values, N, VARINT_DD_STREAM_HI_AUTO, rungs[r], NULL);

        varintDDStreamDecode(buffer, written, decoded, N);

        /* Verify the promise rather than trusting it. */
        double worst = 0.0;

        for (int i = 0; i < N; i++) {
            const varintDD diff = varintDDSub(decoded[i], values[i]);
            const double relative =
                fabs(varintDDToDouble(diff)) / fabs(values[i].hi);

            if (relative > worst) {
                worst = relative;
            }
        }

        printf("  %-14" PRIu32 " %10.2f %7.2fx %14.3e %14.3e\n",
               (uint32_t)rungs[r], (double)written / N, raw / (double)written,
               varintDDStreamMaxRelativeError(rungs[r]), worst);
    }

    printf("\n  One knob spans the whole range: 52 is bit-exact, 0 drops\n");
    printf("  the trailing limb entirely and leaves a plain double\n");
    printf("  stream. Every bit dropped costs one bit per non-exact value\n");
    printf("  and doubles the error. Pick the rung your tolerance allows,\n");
    printf("  not the one that sounds safe.\n");

    free(buffer);
}

/* ====================================================================
 * Example 4: deciding before committing
 * ==================================================================== */
static void example_analyze_first(void) {
    printf("\n=== Example 4: Measure before you encode ===\n\n");

    enum { N = 5000 };
    static varintDD values[N];

    for (int i = 0; i < N; i++) {
        values[i] = varintDDFromDouble((double)i * 0.25);
    }

    /* Analyze runs the encoder's own measurement code without writing
     * anything, so its numbers are exact rather than estimates - and it
     * resolves AUTO, telling you which mode would actually be picked. */
    varintDDStreamMeta plan;
    const bool worthwhile = varintDDStreamAnalyze(
        values, N, VARINT_DD_STREAM_HI_AUTO, VARINT_DD_STREAM_LOSSLESS, &plan);

    printf("  predicted size    %zu bytes (%zu raw)\n", plan.encodedSize,
           N * sizeof(varintDD));
    printf("  leading limbs     %zu bytes, %s mode\n", plan.hiBytes,
           plan.hiMode == VARINT_DD_STREAM_HI_XOR ? "XOR" : "verbatim");
    printf("  trailing limbs    %zu bytes\n", plan.loBytes);
    printf("  exactly zero      %zu of %d\n", plan.exactValues, N);
    printf("  needed escape     %zu\n", plan.escapedLimbs);
    printf("  worth encoding    %s\n\n", worthwhile ? "yes" : "no");

    uint8_t *buffer = malloc(varintDDStreamMaxSize(N));
    const size_t actual =
        varintDDStreamEncode(buffer, values, N, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, NULL);

    printf("  actual size       %zu bytes -> prediction was %s\n", actual,
           actual == plan.encodedSize ? "exact" : "WRONG");

    free(buffer);
}

/* ====================================================================
 * Example 5: decoding data you did not create
 * ==================================================================== */
static void example_untrusted_input(void) {
    printf("\n=== Example 5: Decoding untrusted input ===\n\n");

    enum { N = 64 };
    static varintDD values[N];
    static varintDD decoded[N];

    for (int i = 0; i < N; i++) {
        values[i] = varintDDDiv(varintDDFromDouble((double)i + 1.0),
                                varintDDFromDouble(7.0));
    }

    uint8_t *buffer = malloc(varintDDStreamMaxSize(N));
    const size_t written =
        varintDDStreamEncode(buffer, values, N, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, NULL);

    /* The decoder is told how many bytes exist and never reads past
     * them. It returns 0 rather than a partial result, so there is no
     * "how much did I get" ambiguity to get wrong at the call site. */
    printf("  valid stream, %zu bytes    -> %zu values\n", written,
           varintDDStreamDecode(buffer, written, decoded, N));

    printf("  truncated to %zu bytes     -> %zu values\n", written / 2,
           varintDDStreamDecode(buffer, written / 2, decoded, N));

    printf("  output buffer too small    -> %zu values\n",
           varintDDStreamDecode(buffer, written, decoded, N - 1));

    /* Corruption cannot be detected in general - there is no checksum -
     * but it can never make the decoder read memory it does not own. */
    size_t rejected = 0;
    size_t survived = 0;

    for (size_t trial = 0; trial < 2000; trial++) {
        uint8_t *corrupt = malloc(written);
        memcpy(corrupt, buffer, written);
        corrupt[rng() % written] ^= (uint8_t)(rng() & 0xFF);

        if (varintDDStreamDecode(corrupt, written, decoded, N) == 0) {
            rejected++;
        } else {
            survived++;
        }

        free(corrupt);
    }

    printf("\n  2000 single-byte corruptions: %zu rejected, %zu decoded\n",
           rejected, survived);
    printf("  The survivors decode to WRONG VALUES, not to unsafe memory\n");
    printf("  access. If you need to detect tampering, checksum the\n");
    printf("  stream yourself - this codec guarantees memory safety, not\n");
    printf("  integrity.\n");

    /* Reading the header alone is cheap, which is useful for sizing an
     * output buffer before committing to the decode. */
    printf("\n  varintDDStreamGetCount says %zu without decoding the body\n",
           varintDDStreamGetCount(buffer, written));

    free(buffer);
}

int main(void) {
    printf("========================================\n");
    printf("varintDDStream - compressing 106-bit values\n");
    printf("========================================\n");

    if (!varintDDSelfCheck()) {
        printf("\nvarintDDSelfCheck FAILED - see example_dd.c\n");
        return 1;
    }

    example_round_trip();
    example_what_compresses();
    example_precision_ladder();
    example_analyze_first();
    example_untrusted_input();

    printf("\n========================================\n");
    return 0;
}
