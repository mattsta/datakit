/* ====================================================================
 * varintDDDocTest - compiles and runs the code shown in the docs
 * ====================================================================
 * Every code block in docs/modules/varintDD.md and
 * docs/modules/varintDDStream.md appears here verbatim, so a snippet
 * that drifts out of step with the API becomes a build failure rather
 * than something a reader discovers.
 *
 * Documentation rots quietly. This is the cheapest way to stop it:
 * the examples are not described as working, they are demonstrated to
 * work on every build. Assertions check the claims the prose makes
 * about the output, not merely that the code compiles.
 *
 * When a doc example changes, change it here too - they are meant to
 * be the same text. */
#include "varintDDStream.h"

/* The snippets below use assert() exactly as the documentation prints
 * them. A Release build defines NDEBUG and would compile every one of
 * those checks away, leaving a test that proves only that the code
 * parses. Force them back on before assert.h is pulled in. */
#undef NDEBUG
#include <assert.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- varintDD.md Example 1: simulation clock ---- */
static void docClock(void) {
    const double dt = (1.0 / 3.0) * 1e-6;
    const varintDD step = varintDDFromDouble(dt);
    varintDD clock = varintDDZero();

    for (size_t i = 0; i < 100000; i++) {
        clock = varintDDAdd(clock, step);
    }

    char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), clock, 30);
    printf("elapsed: %s seconds\n", text);
}

/* ---- varintDD.md Example 2: summation under cancellation ---- */
static void docSum(void) {
    static double values[100001];
    values[0] = 1e16;
    values[100000] = -1e16;
    for (size_t i = 1; i < 100000; i++) {
        values[i] = 1.0;
    }

    const varintDD total = varintDDSumDoubles(values, 100001);
    printf("%.17g\n", varintDDToDouble(total));
    assert(varintDDToDouble(total) == 99999.0);
}

/* ---- varintDD.md Example 4: integers past 2^53 ---- */
static void docInt64(void) {
    const int64_t eventId = 9007199254740993LL;
    printf("as double: %.0f\n", (double)eventId);

    char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), varintDDFromInt64(eventId), 20);
    printf("as varintDD: %s\n", text);
}

/* ---- varintDD.md Example 6 + build requirements ---- */
static int docBackendAndSelfCheck(varintDD *dst, const varintDD *a,
                                  const varintDD *b, size_t count) {
    if (!varintDDSelfCheck()) {
        fprintf(stderr, "build does not preserve IEEE semantics\n");
        return 1;
    }

    printf("backend: %s (%zu lanes)\n", varintDDBackend(),
           varintDDBackendLanes());

    varintDDAddArray(dst, a, b, count);
    varintDDMulArray(dst, a, b, count);
    return 0;
}

/* ---- varintDDStream.md Example 1: lossless round trip ---- */
static void docRoundTrip(const varintDD *values, varintDD *decoded,
                         size_t count) {
    uint8_t *buffer = malloc(varintDDStreamMaxSize(count));
    assert(buffer != NULL);

    varintDDStreamMeta meta;
    const size_t written =
        varintDDStreamEncode(buffer, values, count, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, &meta);

    const size_t got = varintDDStreamDecode(buffer, written, decoded, count);

    assert(got == count);
    assert(memcmp(values, decoded, count * sizeof(varintDD)) == 0);
    free(buffer);
}

/* ---- varintDDStream.md Example 2: measure before committing ---- */
static void docAnalyze(const varintDD *values, size_t count) {
    varintDDStreamMeta plan;
    const bool worthwhile =
        varintDDStreamAnalyze(values, count, VARINT_DD_STREAM_HI_AUTO,
                              VARINT_DD_STREAM_LOSSLESS, &plan);

    printf("would be %zu bytes (%zu raw), %s mode, %zu exact limbs\n",
           plan.encodedSize, count * sizeof(varintDD),
           plan.hiMode == VARINT_DD_STREAM_HI_XOR ? "XOR" : "verbatim",
           plan.exactValues);

    (void)worthwhile;
}

/* ---- varintDDStream.md Example 3: per-column precision policy ---- */
typedef struct column {
    const char *name;
    uint8_t trailingBits;
    uint8_t *bytes;
    size_t byteCount;
} column;

static bool columnStore(column *c, const varintDD *values, size_t rows) {
    c->bytes = malloc(varintDDStreamMaxSize(rows));
    if (c->bytes == NULL) {
        return false;
    }

    c->byteCount =
        varintDDStreamEncode(c->bytes, values, rows, VARINT_DD_STREAM_HI_AUTO,
                             c->trailingBits, NULL);
    return c->byteCount != 0;
}

/* ---- varintDDStream.md Example 5: untrusted input ---- */
static size_t docUntrusted(const uint8_t *untrusted, size_t untrustedBytes,
                           varintDD *values, size_t capacity) {
    const size_t got =
        varintDDStreamDecode(untrusted, untrustedBytes, values, capacity);

    if (got == 0) {
        return 0;
    }

    return varintDDStreamGetCount(untrusted, untrustedBytes);
}

/* ---- varintDDStream.md Example 6: verifying the precision ---- */
static void docLadder(const varintDD *values, varintDD *decoded, size_t count) {
    uint8_t *buffer = malloc(varintDDStreamMaxSize(count));
    assert(buffer != NULL);

    const size_t written = varintDDStreamEncode(
        buffer, values, count, VARINT_DD_STREAM_HI_AUTO, 20, NULL);
    varintDDStreamDecode(buffer, written, decoded, count);

    const double bound = varintDDStreamMaxRelativeError(20);
    double worst = 0.0;

    for (size_t i = 0; i < count; i++) {
        const varintDD diff = varintDDSub(decoded[i], values[i]);
        const double relative =
            fabs(varintDDToDouble(diff)) / fabs(values[i].hi);

        if (relative > worst) {
            worst = relative;
        }
    }

    printf("bound %.3e, observed %.3e\n", bound, worst);
    assert(worst <= bound);
    free(buffer);
}

int main(void) {
    enum { N = 512 };
    static varintDD values[N];
    static varintDD other[N];
    static varintDD decoded[N];

    for (size_t i = 0; i < N; i++) {
        values[i] = varintDDDiv(varintDDFromDouble((double)i + 1.0),
                                varintDDFromDouble(7.0));
        other[i] = varintDDDiv(varintDDFromDouble((double)i + 3.0),
                               varintDDFromDouble(11.0));
    }

    if (docBackendAndSelfCheck(decoded, values, other, N) != 0) {
        return 1;
    }

    docClock();
    docSum();
    docInt64();
    docRoundTrip(values, decoded, N);
    docAnalyze(values, N);
    docLadder(values, decoded, N);

    column c = {"ratio", 20, NULL, 0};
    /* The call must survive NDEBUG builds — c.bytes/c.byteCount are used
     * below — so it cannot live inside the assert. */
    const bool stored = columnStore(&c, values, N);
    assert(stored);
    (void)stored;
    printf("column %s: %zu bytes\n", c.name, c.byteCount);

    uint8_t *stream = malloc(varintDDStreamMaxSize(N));
    const size_t n =
        varintDDStreamEncode(stream, values, N, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, NULL);
    printf("untrusted decode reports %zu\n",
           docUntrusted(stream, n, decoded, N));

    free(c.bytes);
    free(stream);
    printf("ALL DOC SNIPPETS COMPILED AND RAN\n");
    return 0;
}
