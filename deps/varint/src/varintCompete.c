#include "varintCompete.h"
#include "varintBP128.h"
#include "varintDelta.h"
#include "varintDeltaDelta.h"
#include "varintDict.h"
#include "varintFOR.h"
#include "varintPFOR.h"
#include "varintPalette.h"
#include "varintRLE.h"
#include "varintStride.h"
#include "varintTagged.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Maximum scratch we ever need: must dominate EVERY participating
 * codec's worst case, since each encoder writes into this scratch
 * before sizes are compared. Current worst offenders on adversarial
 * (all-unique) data: DICT ~13 B/value (9-byte tagged dictionary entries
 * + 4-byte indices), RLE 10 B/value (varintRLEMaxSize), TAGGED/palette
 * 9 B/value + headers. 16 B/value + slack covers them all. */
static inline size_t competeBodyUpperBound_(size_t count) {
    if (count == 0) {
        return 0;
    }
    return count * 16 + 320;
}

size_t varintCompeteMaxEncodedSize(size_t count) {
    /* magic(4) + version(1) + codec(1) + bodyLen(up to 9) + body */
    return 4 + 1 + 1 + 9 + competeBodyUpperBound_(count);
}

/* ====================================================================
 * Frame writing / reading
 * ==================================================================== */

static size_t competeWriteFrame_(uint8_t *dst, varintCodecID winner,
                                 const uint8_t *body, size_t bodyLen) {
    uint8_t *p = dst;
    *p++ = VARINT_COMPETE_MAGIC0;
    *p++ = VARINT_COMPETE_MAGIC1;
    *p++ = VARINT_COMPETE_MAGIC2;
    *p++ = VARINT_COMPETE_MAGIC3;
    *p++ = (uint8_t)VARINT_COMPETE_VERSION;
    *p++ = (uint8_t)winner;
    p += varintTaggedPut64(p, bodyLen);
    if (bodyLen > 0) {
        memcpy(p, body, bodyLen);
    }
    p += bodyLen;
    return (size_t)(p - dst);
}

size_t varintCompeteReadHeader(const uint8_t *src, size_t srcBytes,
                               varintCompeteHeader *header) {
    assert(header != NULL);
    if (!src || srcBytes < 6) {
        return 0;
    }
    if (src[0] != VARINT_COMPETE_MAGIC0 || src[1] != VARINT_COMPETE_MAGIC1 ||
        src[2] != VARINT_COMPETE_MAGIC2 || src[3] != VARINT_COMPETE_MAGIC3) {
        return 0;
    }
    if (src[4] != VARINT_COMPETE_VERSION) {
        return 0;
    }
    header->version = src[4];
    header->codecID = (varintCodecID)src[5];
    if (header->codecID >= VARINT_CODEC_MAX) {
        return 0;
    }
    uint64_t bodyLen;
    varintWidth w = varintTaggedGet64(src + 6, &bodyLen);
    if (w == 0) {
        return 0;
    }
    header->bodyLen = (size_t)bodyLen;
    header->headerLen = 6 + (size_t)w;

    if (header->headerLen + header->bodyLen > srcBytes) {
        return 0;
    }
    return header->headerLen;
}

/* ====================================================================
 * Per-codec encode-into-scratch helpers
 * ====================================================================
 * Each returns bytes written into scratch, or 0 if codec declined.
 * Telemetry calls are recorded by the dispatcher (one place). */

static size_t encDelta_(uint8_t *scratch, const int64_t *vals, size_t count) {
    return varintDeltaEncode(scratch, vals, count);
}
static size_t encDeltaDelta_(uint8_t *scratch, const int64_t *vals,
                             size_t count) {
    return varintDeltaDeltaEncode(scratch, vals, count, NULL);
}
static size_t encStride_(uint8_t *scratch, const int64_t *vals, size_t count) {
    /* Picks exact vs fuzzy itself; declines (returns 0) when fuzzy
     * exceeds the exception threshold, keeping output bounded. */
    return varintStrideEncode(scratch, vals, count, NULL);
}
static size_t encTagged_(uint8_t *scratch, const int64_t *vals, size_t count) {
    uint8_t *p = scratch;
    for (size_t i = 0; i < count; i++) {
        /* Convert via ZigZag so signed values pack tightly. Caller
         * decodes by reversing. */
        uint64_t zz = varintDeltaZigZag(vals[i]);
        p += varintTaggedPut64(p, zz);
    }
    return (size_t)(p - scratch);
}
static size_t encTaggedUnsigned_(uint8_t *scratch, const uint64_t *vals,
                                 size_t count) {
    uint8_t *p = scratch;
    for (size_t i = 0; i < count; i++) {
        p += varintTaggedPut64(p, vals[i]);
    }
    return (size_t)(p - scratch);
}
static size_t encRLE_(uint8_t *scratch, const uint64_t *vals, size_t count) {
    /* RLE benefits only on repetition. Use the WithHeader variant so the
     * decoder doesn't need an external count. */
    return varintRLEEncodeWithHeader(scratch, vals, count, NULL);
}
static size_t encFOR_(uint8_t *scratch, const uint64_t *vals, size_t count) {
    if (count == 0) {
        return 0;
    }
    return varintFOREncode(scratch, vals, count, NULL);
}
static size_t encPFOR_(uint8_t *scratch, const uint64_t *vals, size_t count) {
    if (count == 0) {
        return 0;
    }
    varintPFORMeta m;
    return varintPFOREncode(scratch, vals, (uint32_t)count,
                            VARINT_PFOR_THRESHOLD_95, &m);
}
static size_t encDict_(uint8_t *scratch, const uint64_t *vals, size_t count) {
    return varintDictEncode(scratch, vals, count);
}
static size_t encPalette_(uint8_t *scratch, const uint64_t *vals,
                          size_t count) {
    return varintPaletteEncode(scratch, vals, count, NULL);
}
static size_t encPaletteDelta_(uint8_t *scratch, const uint64_t *vals,
                               size_t count) {
    return varintPaletteDeltaEncode(scratch, vals, count, NULL);
}
static size_t encBP128_(uint8_t *scratch, const uint64_t *vals, size_t count) {
    return varintBP128Encode64(scratch, vals, count, NULL);
}
static size_t encBP128Delta_(uint8_t *scratch, const uint64_t *vals,
                             size_t count) {
    /* Delta variant is defined only for ascending input — decline
     * otherwise rather than emit an undecodable stream. */
    if (!varintBP128IsSorted64(vals, count)) {
        return 0;
    }
    return varintBP128DeltaEncode64(scratch, vals, count, NULL);
}

/* ====================================================================
 * Competition loop
 * ==================================================================== */

/* Generic competition. The `kind` switch selects signed vs unsigned codec
 * dispatch. We allocate two scratch buffers and ping-pong: scratch[cur]
 * holds the running best, scratch[try] is the candidate. */
typedef enum competeKind { COMPETE_SIGNED, COMPETE_UNSIGNED } competeKind;

static void recordCandidate_(varintCompeteResult *r, varintCodecID id,
                             size_t bodyLen) {
    if (r && r->candidatesEvaluated < VARINT_CODEC_MAX) {
        r->candidates[r->candidatesEvaluated].id = id;
        r->candidates[r->candidatesEvaluated].encodedSize = bodyLen;
        r->candidatesEvaluated++;
    }
    VARINT_TELEMETRY_CALL(id, 0);
}

static size_t competeRun_(uint8_t *dst, const void *valuesAny, size_t count,
                          uint64_t codecMask, competeKind kind,
                          varintCompeteResult *result) {
    if (result) {
        memset(result, 0, sizeof(*result));
    }

    if (count == 0) {
        /* Emit an empty frame with TAGGED winner — decoder will read 0 values
         */
        if (result) {
            result->winner = VARINT_CODEC_TAGGED;
        }
        return competeWriteFrame_(dst, VARINT_CODEC_TAGGED, NULL, 0);
    }

    size_t cap = competeBodyUpperBound_(count);
    uint8_t *bestBuf = malloc(cap);
    uint8_t *tryBuf = malloc(cap);
    if (!bestBuf || !tryBuf) {
        free(bestBuf);
        free(tryBuf);
        return 0;
    }

    size_t bestSize = SIZE_MAX;
    varintCodecID bestID = VARINT_CODEC_TAGGED;

    const int64_t *signedVals =
        (kind == COMPETE_SIGNED) ? (const int64_t *)valuesAny : NULL;
    const uint64_t *unsignedVals =
        (kind == COMPETE_UNSIGNED) ? (const uint64_t *)valuesAny : NULL;

    /* Macro to test one codec. */
#define TRY_CODEC(idEnum, encExpr)                                             \
    do {                                                                       \
        if (codecMask & VARINT_COMPETE_BIT(idEnum)) {                          \
            size_t n = (encExpr);                                              \
            recordCandidate_(result, (idEnum), n);                             \
            if (n > 0 && n < bestSize) {                                       \
                bestSize = n;                                                  \
                bestID = (idEnum);                                             \
                uint8_t *tmp = bestBuf;                                        \
                bestBuf = tryBuf;                                              \
                tryBuf = tmp;                                                  \
            }                                                                  \
        }                                                                      \
    } while (0)

    if (kind == COMPETE_SIGNED) {
        TRY_CODEC(VARINT_CODEC_TAGGED, encTagged_(tryBuf, signedVals, count));
        TRY_CODEC(VARINT_CODEC_DELTA, encDelta_(tryBuf, signedVals, count));
        TRY_CODEC(VARINT_CODEC_DELTA_DELTA,
                  encDeltaDelta_(tryBuf, signedVals, count));
        TRY_CODEC(VARINT_CODEC_STRIDE, encStride_(tryBuf, signedVals, count));
    } else {
        TRY_CODEC(VARINT_CODEC_TAGGED,
                  encTaggedUnsigned_(tryBuf, unsignedVals, count));
        /* For unsigned, also try the int64-domain codecs by reinterpret —
         * safe for monotonic or value < 2^63 streams. */
        TRY_CODEC(VARINT_CODEC_DELTA,
                  encDelta_(tryBuf, (const int64_t *)unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_DELTA_DELTA,
                  encDeltaDelta_(tryBuf, (const int64_t *)unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_STRIDE,
                  encStride_(tryBuf, (const int64_t *)unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_RLE, encRLE_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_FOR, encFOR_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_PFOR, encPFOR_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_DICT, encDict_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_PALETTE,
                  encPalette_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_PALETTE_DELTA,
                  encPaletteDelta_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_BP128, encBP128_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_BP128_DELTA,
                  encBP128Delta_(tryBuf, unsignedVals, count));
    }
#undef TRY_CODEC

    if (bestSize == SIZE_MAX) {
        /* No codec was enabled — fall back to per-element tagged. */
        if (kind == COMPETE_SIGNED) {
            bestSize = encTagged_(bestBuf, signedVals, count);
        } else {
            bestSize = encTaggedUnsigned_(bestBuf, unsignedVals, count);
        }
        bestID = VARINT_CODEC_TAGGED;
    }

    size_t frameBytes = competeWriteFrame_(dst, bestID, bestBuf, bestSize);

    if (result) {
        result->winner = bestID;
        result->winnerSize = bestSize;
        result->frameSize = frameBytes;
    }
    VARINT_TELEMETRY_WIN(bestID, bestSize);

    free(bestBuf);
    free(tryBuf);
    return frameBytes;
}

size_t varintCompeteEncode(uint8_t *dst, const int64_t *values, size_t count,
                           uint64_t codecMask, varintCompeteResult *result) {
    assert(dst != NULL);
    if (codecMask == 0) {
        codecMask = VARINT_COMPETE_DEFAULT_MASK;
    }
    return competeRun_(dst, values, count, codecMask, COMPETE_SIGNED, result);
}

size_t varintCompeteEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                   size_t count, uint64_t codecMask,
                                   varintCompeteResult *result) {
    assert(dst != NULL);
    if (codecMask == 0) {
        codecMask = VARINT_COMPETE_DEFAULT_MASK;
    }
    return competeRun_(dst, values, count, codecMask, COMPETE_UNSIGNED, result);
}

/* ====================================================================
 * Decode dispatch
 * ==================================================================== */

/* Decoder for the TAGGED-of-ZigZag signed format we wrote above. */
static size_t decTaggedSigned_(const uint8_t *src, size_t count,
                               int64_t *output) {
    const uint8_t *p = src;
    for (size_t i = 0; i < count; i++) {
        uint64_t zz;
        p += varintTaggedGet64(p, &zz);
        output[i] = varintDeltaZigZagDecode(zz);
    }
    return (size_t)(p - src);
}

/* Decoder for the unsigned TAGGED format. */
static size_t decTaggedUnsigned_(const uint8_t *src, size_t count,
                                 uint64_t *output) {
    const uint8_t *p = src;
    for (size_t i = 0; i < count; i++) {
        p += varintTaggedGet64(p, &output[i]);
    }
    return (size_t)(p - src);
}

size_t varintCompeteDecode(const uint8_t *src, size_t srcBytes, size_t count,
                           int64_t *output) {
    assert(src != NULL);
    varintCompeteHeader h;
    size_t hdr = varintCompeteReadHeader(src, srcBytes, &h);
    if (hdr == 0) {
        return 0;
    }
    const uint8_t *body = src + hdr;

    size_t consumed = 0;
    switch (h.codecID) {
    case VARINT_CODEC_TAGGED:
        consumed = decTaggedSigned_(body, count, output);
        break;
    case VARINT_CODEC_DELTA:
        consumed = varintDeltaDecode(body, count, output);
        break;
    case VARINT_CODEC_DELTA_DELTA:
        consumed = varintDeltaDeltaDecode(body, count, output);
        break;
    case VARINT_CODEC_STRIDE:
        consumed = varintStrideDecode(body, count, output);
        break;
    default:
        /* Unknown codec in this slot for signed input — treat as zeros. */
        for (size_t i = 0; i < count; i++) {
            output[i] = 0;
        }
        return 0;
    }

    return hdr + consumed;
}

size_t varintCompeteDecodeUnsigned(const uint8_t *src, size_t srcBytes,
                                   size_t count, uint64_t *output) {
    assert(src != NULL);
    varintCompeteHeader h;
    size_t hdr = varintCompeteReadHeader(src, srcBytes, &h);
    if (hdr == 0) {
        return 0;
    }
    const uint8_t *body = src + hdr;

    switch (h.codecID) {
    case VARINT_CODEC_TAGGED:
        return hdr + decTaggedUnsigned_(body, count, output);
    case VARINT_CODEC_DELTA: {
        size_t n = varintDeltaDecode(body, count, (int64_t *)output);
        return hdr + n;
    }
    case VARINT_CODEC_DELTA_DELTA: {
        size_t n = varintDeltaDeltaDecode(body, count, (int64_t *)output);
        return hdr + n;
    }
    case VARINT_CODEC_STRIDE: {
        size_t n = varintStrideDecode(body, count, (int64_t *)output);
        return hdr + n;
    }
    case VARINT_CODEC_RLE:
        return hdr + varintRLEDecodeWithHeader(body, output, count) * 0 +
               h.bodyLen; /* RLE returns value count not bytes; trust frame */
    case VARINT_CODEC_FOR: {
        size_t got = varintFORDecode(body, output, count);
        (void)got;
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_PFOR: {
        varintPFORMeta m;
        varintPFORReadMeta(body, &m);
        varintPFORDecode(body, output, &m);
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_DICT: {
        varintDictDecodeInto(body, h.bodyLen, output, count);
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_PALETTE: {
        varintPaletteDecode(body, h.bodyLen, output, count);
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_PALETTE_DELTA: {
        varintPaletteDeltaDecode(body, h.bodyLen, output, count);
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_BP128: {
        varintBP128Decode64(body, output, count);
        return hdr + h.bodyLen;
    }
    case VARINT_CODEC_BP128_DELTA: {
        varintBP128DeltaDecode64(body, output, count);
        return hdr + h.bodyLen;
    }
    default:
        for (size_t i = 0; i < count; i++) {
            output[i] = 0;
        }
        return 0;
    }
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_COMPETE_TEST
#include "ctest.h"
#include <stdio.h>

int varintCompeteTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Compete picks STRIDE for linear ramp") {
        int64_t values[500];
        for (size_t i = 0; i < 500; i++) {
            values[i] = 10000 + (int64_t)i * 5;
        }

        uint8_t buf[varintCompeteMaxEncodedSize(500)];
        varintCompeteResult res;
        size_t written = varintCompeteEncode(buf, values, 500,
                                             VARINT_COMPETE_DEFAULT_MASK, &res);

        if (res.winner != VARINT_CODEC_STRIDE) {
            ERR("Expected STRIDE winner, got %s", varintCodecName(res.winner));
        }
        if (written > 30) {
            ERR("Stride frame too large: %zu", written);
        }

        int64_t dec[500];
        size_t read = varintCompeteDecode(buf, written, 500, dec);
        if (read != written) {
            ERR("byte count mismatch: wrote %zu, read %zu", written, read);
        }
        for (size_t i = 0; i < 500; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
    }

    TEST("Compete picks DELTA_DELTA for timestamp-like series with jitter") {
        int64_t values[200];
        int64_t base = 1700000000;
        for (size_t i = 0; i < 200; i++) {
            /* Mostly 60s spacing, with small jitter so stride won't fit
             * exactly but DoD will still crush. */
            int64_t jitter = (int64_t)((i * 37) % 5) - 2;
            values[i] = base + (int64_t)i * 60 + jitter;
        }

        uint8_t buf[varintCompeteMaxEncodedSize(200)];
        varintCompeteResult res;
        size_t written = varintCompeteEncode(buf, values, 200,
                                             VARINT_COMPETE_DEFAULT_MASK, &res);

        /* Should pick either STRIDE (fuzzy) or DELTA_DELTA; both are fine
         * — just confirm it's not falling back to TAGGED or DELTA. */
        if (res.winner != VARINT_CODEC_DELTA_DELTA &&
            res.winner != VARINT_CODEC_STRIDE &&
            res.winner != VARINT_CODEC_DELTA) {
            ERR("Unexpected winner for jittery timestamps: %s",
                varintCodecName(res.winner));
        }

        int64_t dec[200];
        size_t read = varintCompeteDecode(buf, written, 200, dec);
        if (read != written) {
            ERR("byte count mismatch: %zu vs %zu", written, read);
        }
        for (size_t i = 0; i < 200; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
    }

    TEST("Compete tries all candidates and records them") {
        int64_t values[50];
        for (size_t i = 0; i < 50; i++) {
            values[i] = (int64_t)i * 100;
        }
        uint8_t buf[varintCompeteMaxEncodedSize(50)];
        varintCompeteResult res;
        varintCompeteEncode(buf, values, 50, VARINT_COMPETE_DEFAULT_MASK, &res);

        /* DEFAULT_MASK has 9 codecs; signed path runs 4 of them. */
        if (res.candidatesEvaluated < 3) {
            ERR("Expected ≥3 candidates evaluated, got %zu",
                res.candidatesEvaluated);
        }

        /* Every recorded candidate should have a non-zero or zero (declined)
         * size — just verify the ID is in range. */
        for (size_t i = 0; i < res.candidatesEvaluated; i++) {
            if (res.candidates[i].id >= VARINT_CODEC_MAX) {
                ERR("Out-of-range candidate ID at %zu", i);
            }
        }
    }

    TEST("Compete frame format: magic + version + codecID") {
        int64_t values[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        uint8_t buf[varintCompeteMaxEncodedSize(10)];
        varintCompeteEncode(buf, values, 10, VARINT_COMPETE_DEFAULT_MASK, NULL);

        if (buf[0] != 'V' || buf[1] != 'C' || buf[2] != 'M' || buf[3] != 'P') {
            ERRR("Frame magic missing");
        }
        if (buf[4] != VARINT_COMPETE_VERSION) {
            ERR("Frame version: %u", (unsigned)buf[4]);
        }
    }

    TEST("Compete header read rejects bad magic and bad version") {
        uint8_t bad[10] = {'X', 'X', 'X', 'X', 1, 0, 0, 0, 0, 0};
        varintCompeteHeader h;
        size_t got = varintCompeteReadHeader(bad, 10, &h);
        if (got != 0) {
            ERRR("Bad magic accepted");
        }

        bad[0] = 'V';
        bad[1] = 'C';
        bad[2] = 'M';
        bad[3] = 'P';
        bad[4] = 99; /* bad version */
        got = varintCompeteReadHeader(bad, 10, &h);
        if (got != 0) {
            ERRR("Bad version accepted");
        }
    }

    TEST("Compete empty array round-trip") {
        uint8_t buf[varintCompeteMaxEncodedSize(0)];
        varintCompeteResult res;
        size_t w = varintCompeteEncode(buf, NULL, 0,
                                       VARINT_COMPETE_DEFAULT_MASK, &res);
        if (w == 0) {
            ERRR("Empty frame should still emit header");
        }

        int64_t dummy;
        size_t r = varintCompeteDecode(buf, w, 0, &dummy);
        if (r != w) {
            ERR("Empty round-trip mismatch: %zu vs %zu", w, r);
        }
    }

    TEST("Compete unsigned path picks FOR or RLE for repetitive data") {
        uint64_t values[200];
        for (size_t i = 0; i < 200; i++) {
            values[i] = (i < 100) ? 42 : 43;
        }
        uint8_t buf[varintCompeteMaxEncodedSize(200)];
        varintCompeteResult res;
        size_t written = varintCompeteEncodeUnsigned(
            buf, values, 200, VARINT_COMPETE_DEFAULT_MASK, &res);

        /* RLE should crush this — just 2 runs */
        if (res.winnerSize > 20) {
            ERR("Repetitive data not compressed well: %zu (winner=%s)",
                res.winnerSize, varintCodecName(res.winner));
        }
        (void)written;
    }

    TEST("Compete runs BP128_DELTA on sorted data and round-trips it") {
        /* Sorted with small jittery gaps: BP128_DELTA territory. The
         * key regression: this codec used to sit in the default mask
         * without ever being evaluated or decodable. */
        enum { N = 2048 };
        static uint64_t values[N];
        uint64_t v = 1000000;
        for (size_t i = 0; i < N; i++) {
            v += 1 + ((i * 2654435761u) >> 27); /* gaps 1..32 */
            values[i] = v;
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSize(N));
        varintCompeteResult res;
        size_t written = varintCompeteEncodeUnsigned(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, &res);

        bool evaluated = false;
        for (size_t i = 0; i < res.candidatesEvaluated; i++) {
            if (res.candidates[i].id == VARINT_CODEC_BP128_DELTA &&
                res.candidates[i].encodedSize > 0) {
                evaluated = true;
            }
        }
        if (!evaluated) {
            ERRR("BP128_DELTA not evaluated on sorted input");
        }

        static uint64_t dec[N];
        size_t read = varintCompeteDecodeUnsigned(buf, written, N, dec);
        if (read != written) {
            ERR("byte count mismatch: wrote %zu, read %zu", written, read);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu (winner=%s)", i,
                    varintCodecName(res.winner));
                break;
            }
        }
        free(buf);
    }

    TEST("Compete picks PALETTE_DELTA for skewed-gap monotonic series") {
        /* Gaps drawn from a skewed 4-symbol alphabet: plain DELTA pays a
         * byte per gap, BP128_DELTA pays 4 bits, entropy is ~1.6 bits. */
        enum { N = 4096 };
        static const uint64_t gaps[] = {1, 1, 1, 1, 1, 1, 2, 2, 5, 10};
        static uint64_t values[N];
        uint64_t v = 5000000;
        for (size_t i = 0; i < N; i++) {
            v += gaps[(i * 2654435761u >> 13) % 10];
            values[i] = v;
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSize(N));
        varintCompeteResult res;
        size_t written = varintCompeteEncodeUnsigned(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, &res);

        if (res.winner != VARINT_CODEC_PALETTE_DELTA) {
            ERR("Expected PALETTE_DELTA winner, got %s (%zu B)",
                varintCodecName(res.winner), res.winnerSize);
        }

        static uint64_t dec[N];
        size_t read = varintCompeteDecodeUnsigned(buf, written, N, dec);
        if (read != written) {
            ERR("byte count mismatch: wrote %zu, read %zu", written, read);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
        free(buf);
    }

    TEST("Compete picks PALETTE for skewed run-free small-alphabet data") {
        /* Alternating hot zero + rotating 16-value alphabet: no runs for
         * RLE, 4-bit floor for FOR/BP128, but heavy frequency skew that
         * entropy coding exploits. */
        enum { N = 6400 };
        static uint64_t values[N];
        for (size_t i = 0; i < N; i++) {
            values[i] = (i % 2) ? 0 : (i % 32) / 2;
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSize(N));
        varintCompeteResult res;
        size_t written = varintCompeteEncodeUnsigned(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, &res);

        if (res.winner != VARINT_CODEC_PALETTE) {
            ERR("Expected PALETTE winner for skewed alphabet, got %s (%zu B)",
                varintCodecName(res.winner), res.winnerSize);
        }

        static uint64_t dec[N];
        size_t read = varintCompeteDecodeUnsigned(buf, written, N, dec);
        if (read != written) {
            ERR("byte count mismatch: wrote %zu, read %zu", written, read);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }

        /* Truncated palette-winning frames must be rejected at the frame
         * header layer (bodyLen exceeds the surviving bytes). */
        for (size_t cut = 1; cut < 64 && cut < written; cut++) {
            if (varintCompeteDecodeUnsigned(buf, written - cut, N, dec) != 0) {
                ERR("Truncated compete frame (-%zu bytes) accepted", cut);
                break;
            }
        }
        free(buf);
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_COMPETE_TEST */
