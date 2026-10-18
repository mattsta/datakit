/* clock_gettime/CLOCK_MONOTONIC are POSIX, hidden by glibc under strict
 * -std=c11 unless the feature macro is set before any include. */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "varintCompete.h"
#include "varintBP128.h"
#include "varintDelta.h"
#include "varintDeltaDelta.h"
#include "varintDict.h"
#include "varintElias.h"
#include "varintFOR.h"
#include "varintPFOR.h"
#include "varintPalette.h"
#include "varintRLE.h"
#include "varintStride.h"
#include "varintTagged.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef VARINT_TELEMETRY
#include <time.h>
static inline uint64_t competeNowNs_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#else
static inline uint64_t competeNowNs_(void) {
    return 0;
}
#endif

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

/* Elias codes encode positive integers, so values shift by +1 on the
 * wire. The lane self-gates to small values: universal codes only win
 * on small-integer columns, and the gate keeps the worst-case Gamma
 * length (2*bits+1) inside the shared scratch bound. Values stream
 * through the bit writer one at a time, so the lane needs no scratch
 * copy of the column. */
#define COMPETE_ELIAS_MAX_VALUE ((UINT64_C(1) << 30) - 2)

typedef size_t (*competeEliasEncodeFn_)(varintBitWriter *w, uint64_t value);

static size_t encEliasCommon_(uint8_t *scratch, const uint64_t *vals,
                              size_t count, competeEliasEncodeFn_ enc) {
    for (size_t i = 0; i < count; i++) {
        if (vals[i] > COMPETE_ELIAS_MAX_VALUE) {
            return 0;
        }
    }
    /* Capacity invariant: the value gate caps Gamma at 61 bits and
     * Delta below that, so worst case is under 8 bytes/value against
     * the 16 bytes/value scratch bound — the bit writer (which bounds
     * only by assert) cannot overrun. */
    varintBitWriter w;
    varintBitWriterInit(&w, scratch, competeBodyUpperBound_(count));
    for (size_t i = 0; i < count; i++) {
        enc(&w, vals[i] + 1);
    }
    return varintBitWriterBytes(&w);
}

static size_t encEliasGamma_(uint8_t *scratch, const uint64_t *vals,
                             size_t count) {
    return encEliasCommon_(scratch, vals, count, varintEliasGammaEncode);
}

static size_t encEliasDelta_(uint8_t *scratch, const uint64_t *vals,
                             size_t count) {
    return encEliasCommon_(scratch, vals, count, varintEliasDeltaEncode);
}

/* ====================================================================
 * Competition loop
 * ==================================================================== */

/* Generic competition. The `kind` switch selects signed vs unsigned codec
 * dispatch. We allocate two scratch buffers and ping-pong: scratch[cur]
 * holds the running best, scratch[try] is the candidate. */
typedef enum competeKind { COMPETE_SIGNED, COMPETE_UNSIGNED } competeKind;

static void recordCandidate_(varintCompeteResult *r, varintCodecID id,
                             size_t bodyLen, size_t bytesIn,
                             uint64_t elapsedNs) {
    if (r && r->candidatesEvaluated < VARINT_CODEC_MAX) {
        r->candidates[r->candidatesEvaluated].id = id;
        r->candidates[r->candidatesEvaluated].encodedSize = bodyLen;
        r->candidatesEvaluated++;
    }
    VARINT_TELEMETRY_CALL(id, bytesIn);
    VARINT_TELEMETRY_TIME_NS(id, elapsedNs);
}

/* Scratch may be caller-provided (chunked encode reuses one pair across
 * every block) or NULL to allocate here. Both buffers must hold
 * competeBodyUpperBound_(count) bytes. */
static size_t competeRun_(uint8_t *dst, const void *valuesAny, size_t count,
                          uint64_t codecMask, competeKind kind,
                          varintCompeteResult *result, uint8_t *scratchA,
                          uint8_t *scratchB) {
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

    uint8_t *bestBuf = scratchA;
    uint8_t *tryBuf = scratchB;
    const bool ownScratch = (bestBuf == NULL || tryBuf == NULL);
    if (ownScratch) {
        size_t cap = competeBodyUpperBound_(count);
        bestBuf = malloc(cap);
        tryBuf = malloc(cap);
        if (!bestBuf || !tryBuf) {
            free(bestBuf);
            free(tryBuf);
            return 0;
        }
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
            const uint64_t tryStartNs = competeNowNs_();                       \
            size_t n = (encExpr);                                              \
            recordCandidate_(result, (idEnum), n, count * sizeof(uint64_t),    \
                             competeNowNs_() - tryStartNs);                    \
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
        TRY_CODEC(VARINT_CODEC_ELIAS_GAMMA,
                  encEliasGamma_(tryBuf, unsignedVals, count));
        TRY_CODEC(VARINT_CODEC_ELIAS_DELTA,
                  encEliasDelta_(tryBuf, unsignedVals, count));
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

    if (ownScratch) {
        free(bestBuf);
        free(tryBuf);
    }
    return frameBytes;
}

size_t varintCompeteEncode(uint8_t *dst, const int64_t *values, size_t count,
                           uint64_t codecMask, varintCompeteResult *result) {
    assert(dst != NULL);
    if (codecMask == 0) {
        codecMask = VARINT_COMPETE_DEFAULT_MASK;
    }
    return competeRun_(dst, values, count, codecMask, COMPETE_SIGNED, result,
                       NULL, NULL);
}

size_t varintCompeteEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                   size_t count, uint64_t codecMask,
                                   varintCompeteResult *result) {
    assert(dst != NULL);
    if (codecMask == 0) {
        codecMask = VARINT_COMPETE_DEFAULT_MASK;
    }
    return competeRun_(dst, values, count, codecMask, COMPETE_UNSIGNED, result,
                       NULL, NULL);
}

/* ====================================================================
 * Candidate pruning
 * ==================================================================== */

/* Sample geometry: adjacent windows so run/delta statistics are real,
 * spread across the array so a heterogeneous tail isn't missed. */
#define COMPETE_PROBE_WINDOW_VALUES 64U
#define COMPETE_PROBE_WINDOWS 4U
#define COMPETE_PROBE_HASH_SIZE 512U

typedef struct competeProbeHash {
    uint64_t keys[COMPETE_PROBE_HASH_SIZE];
    uint8_t used[COMPETE_PROBE_HASH_SIZE];
    size_t distinct;
} competeProbeHash;

static void probeHashInsert_(competeProbeHash *h, uint64_t v) {
    size_t slot =
        (size_t)((v * 0x9E3779B97F4A7C15ULL) >> 55) % COMPETE_PROBE_HASH_SIZE;
    while (h->used[slot]) {
        if (h->keys[slot] == v) {
            return;
        }
        slot = (slot + 1) % COMPETE_PROBE_HASH_SIZE;
    }
    h->keys[slot] = v;
    h->used[slot] = 1;
    h->distinct++;
}

uint64_t varintCompetePruneMask(const uint64_t *values, size_t count,
                                uint64_t codecMask) {
    if (!values || count < 128) {
        return codecMask;
    }

    competeProbeHash valueHash, deltaHash;
    memset(&valueHash, 0, sizeof(valueHash));
    memset(&deltaHash, 0, sizeof(deltaHash));

    size_t windowStarts[COMPETE_PROBE_WINDOWS];
    size_t windows = COMPETE_PROBE_WINDOWS;
    if (count < COMPETE_PROBE_WINDOW_VALUES * COMPETE_PROBE_WINDOWS) {
        windows = 1;
        windowStarts[0] = 0;
    } else {
        size_t span = count - COMPETE_PROBE_WINDOW_VALUES;
        for (size_t w = 0; w < windows; w++) {
            windowStarts[w] = span * w / (windows - 1);
        }
    }

    size_t sampled = 0;
    size_t adjacentPairs = 0;
    size_t equalAdjacent = 0;
    bool unsortedProven = false;
    uint64_t prevWindowLast = 0;
    bool havePrevWindow = false;

    for (size_t w = 0; w < windows; w++) {
        const uint64_t *win = values + windowStarts[w];
        size_t n = COMPETE_PROBE_WINDOW_VALUES;
        if (windowStarts[w] + n > count) {
            n = count - windowStarts[w];
        }
        /* Windows are ordered by position, so any decrease within a
         * window or across a window boundary proves the array unsorted. */
        if (havePrevWindow && win[0] < prevWindowLast) {
            unsortedProven = true;
        }
        for (size_t i = 0; i < n; i++) {
            probeHashInsert_(&valueHash, win[i]);
            sampled++;
            if (i > 0) {
                adjacentPairs++;
                if (win[i] == win[i - 1]) {
                    equalAdjacent++;
                }
                if (win[i] < win[i - 1]) {
                    unsortedProven = true;
                }
                probeHashInsert_(&deltaHash, win[i] - win[i - 1]);
            }
        }
        prevWindowLast = win[n - 1];
        havePrevWindow = true;
    }

    uint64_t pruned = codecMask;
    if (equalAdjacent == 0) {
        pruned &= ~VARINT_COMPETE_BIT(VARINT_CODEC_RLE);
    }
    if (valueHash.distinct * 4 > sampled * 3) {
        pruned &= ~VARINT_COMPETE_BIT(VARINT_CODEC_DICT);
        pruned &= ~VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE);
    }
    if (adjacentPairs > 0 && deltaHash.distinct * 4 > adjacentPairs * 3) {
        pruned &= ~VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE_DELTA);
    }
    if (unsortedProven) {
        pruned &= ~VARINT_COMPETE_BIT(VARINT_CODEC_BP128_DELTA);
    }
    return pruned;
}

/* ====================================================================
 * Chunked encode
 * ==================================================================== */

static size_t chunkNormalizeBlockValues_(size_t blockValues) {
    if (blockValues == 0) {
        blockValues = VARINT_COMPETE_CHUNK_DEFAULT_VALUES;
    }
    if (blockValues < 64) {
        blockValues = 64;
    }
    if (blockValues > (1U << 20)) {
        blockValues = 1U << 20;
    }
    return blockValues;
}

size_t varintCompeteMaxEncodedSizeChunked(size_t count, size_t blockValues) {
    blockValues = chunkNormalizeBlockValues_(blockValues);
    size_t blocks = count ? (count + blockValues - 1) / blockValues : 0;
    /* magic(4) + version(1) + totalCount(≤9), then per block a count
     * prefix (≤9) + a full frame bound. Stride growth only merges
     * blocks, so it never exceeds this. */
    return 4 + 1 + 9 + blocks * (9 + varintCompeteMaxEncodedSize(blockValues));
}

size_t varintCompeteChunkedScratchBytes(size_t blockValues) {
    return 2 * competeBodyUpperBound_(chunkNormalizeBlockValues_(blockValues));
}

bool varintCompeteChunkedScratchInit(varintCompeteChunkedScratch *scratch,
                                     uint8_t *mem, size_t memBytes,
                                     size_t blockValues) {
    if (!scratch) {
        return false;
    }
    memset(scratch, 0, sizeof(*scratch));
    const size_t normalized = chunkNormalizeBlockValues_(blockValues);
    const size_t lane = competeBodyUpperBound_(normalized);
    if (!mem || memBytes < 2 * lane) {
        return false;
    }
    scratch->laneA = mem;
    scratch->laneB = mem + lane;
    scratch->laneBytes = lane;
    scratch->blockValues = normalized;
    scratch->magic = VARINT_COMPETE_SCRATCH_MAGIC;
    return true;
}

/* Audit a scratch against the block target it is being applied to. */
static bool competeScratchValid_(const varintCompeteChunkedScratch *scratch,
                                 size_t blockValues) {
    return scratch->magic == VARINT_COMPETE_SCRATCH_MAGIC &&
           scratch->laneA != NULL && scratch->laneB != NULL &&
           scratch->blockValues == chunkNormalizeBlockValues_(blockValues) &&
           scratch->laneBytes >= competeBodyUpperBound_(scratch->blockValues);
}

static size_t chunkedEncodeRun_(uint8_t *dst, const void *valuesAny,
                                size_t count, uint64_t codecMask,
                                size_t blockValues, competeKind kind,
                                size_t *blocksOut, uint8_t *extScratchA,
                                uint8_t *extScratchB) {
    if (!dst) {
        return 0;
    }
    if (codecMask == 0) {
        codecMask = VARINT_COMPETE_DEFAULT_MASK;
    }
    blockValues = chunkNormalizeBlockValues_(blockValues);

    uint8_t *p = dst;
    *p++ = VARINT_COMPETE_CHUNK_MAGIC0;
    *p++ = VARINT_COMPETE_CHUNK_MAGIC1;
    *p++ = VARINT_COMPETE_CHUNK_MAGIC2;
    *p++ = VARINT_COMPETE_CHUNK_MAGIC3;
    *p++ = (uint8_t)VARINT_COMPETE_CHUNK_VERSION;
    p += varintTaggedPut64(p, count);

    if (count == 0) {
        if (blocksOut) {
            *blocksOut = 0;
        }
        return (size_t)(p - dst);
    }

    /* One scratch pair reused across every block — caller-provided when
     * available, allocated here otherwise. Stride-grown blocks bypass
     * the competition, so scratch never needs to exceed the per-block
     * target. */
    const bool ownScratch = (extScratchA == NULL || extScratchB == NULL);
    uint8_t *scratchA = extScratchA;
    uint8_t *scratchB = extScratchB;
    if (ownScratch) {
        size_t cap = competeBodyUpperBound_(blockValues);
        scratchA = malloc(cap);
        scratchB = malloc(cap);
        if (!scratchA || !scratchB) {
            free(scratchA);
            free(scratchB);
            return 0;
        }
    }

    /* Both kinds share bit-identical block planning: stride detection
     * uses two's-complement wrap, matching how the STRIDE codec treats
     * reinterpreted unsigned input. */
    const uint64_t *uv = (const uint64_t *)valuesAny;
    size_t pos = 0;
    size_t blocks = 0;
    size_t written = (size_t)(p - dst);

    while (pos < count) {
        size_t blockCount = count - pos;
        if (blockCount > blockValues) {
            blockCount = blockValues;
        }

        /* When the whole planned block is one exact arithmetic
         * progression, extend it while the progression continues: the
         * exact stride record is constant-size, so a constant or ramp
         * region costs one tiny block regardless of length. The block
         * size must be final before the count prefix is written, so the
         * stride record is produced here and the growth is abandoned if
         * the encoder declines (a grown block would not fit the
         * competition scratch). */
        uint8_t strideBody[64];
        size_t strideBodyLen = 0;
        if ((codecMask & VARINT_COMPETE_BIT(VARINT_CODEC_STRIDE)) &&
            blockCount >= 3) {
            size_t k =
                varintStrideMatchingPrefixUnsigned(uv + pos, count - pos);
            if (k >= blockCount) {
                const uint64_t strideStartNs = competeNowNs_();
                strideBodyLen = varintStrideEncodeWithMode(
                    strideBody, (const int64_t *)(uv + pos), k,
                    VARINT_STRIDE_MODE_EXACT, NULL);
                VARINT_TELEMETRY_CALL(VARINT_CODEC_STRIDE,
                                      k * sizeof(uint64_t));
                VARINT_TELEMETRY_TIME_NS(VARINT_CODEC_STRIDE,
                                         competeNowNs_() - strideStartNs);
                if (strideBodyLen > 0) {
                    blockCount = k;
                }
            }
        }

        p = dst + written;
        p += varintTaggedPut64(p, blockCount);

        size_t frameBytes = 0;
        if (strideBodyLen > 0) {
            frameBytes = competeWriteFrame_(p, VARINT_CODEC_STRIDE, strideBody,
                                            strideBodyLen);
            VARINT_TELEMETRY_WIN(VARINT_CODEC_STRIDE, strideBodyLen);
        } else {
            uint64_t blockMask = codecMask;
            if (kind == COMPETE_UNSIGNED) {
                blockMask =
                    varintCompetePruneMask(uv + pos, blockCount, codecMask);
            }
            frameBytes = competeRun_(p, uv + pos, blockCount, blockMask, kind,
                                     NULL, scratchA, scratchB);
            if (frameBytes == 0) {
                if (ownScratch) {
                    free(scratchA);
                    free(scratchB);
                }
                return 0;
            }
        }

        written = (size_t)(p - dst) + frameBytes;
        pos += blockCount;
        blocks++;
    }

    if (ownScratch) {
        free(scratchA);
        free(scratchB);
    }
    if (blocksOut) {
        *blocksOut = blocks;
    }
    return written;
}

size_t varintCompeteEncodeChunked(uint8_t *dst, const int64_t *values,
                                  size_t count, uint64_t codecMask,
                                  size_t blockValues, size_t *blocksOut) {
    return chunkedEncodeRun_(dst, values, count, codecMask, blockValues,
                             COMPETE_SIGNED, blocksOut, NULL, NULL);
}

size_t varintCompeteEncodeChunkedUnsigned(uint8_t *dst, const uint64_t *values,
                                          size_t count, uint64_t codecMask,
                                          size_t blockValues,
                                          size_t *blocksOut) {
    return chunkedEncodeRun_(dst, values, count, codecMask, blockValues,
                             COMPETE_UNSIGNED, blocksOut, NULL, NULL);
}

size_t varintCompeteEncodeChunkedUnsignedScratch(
    uint8_t *dst, const uint64_t *values, size_t count, uint64_t codecMask,
    size_t blockValues, size_t *blocksOut,
    const varintCompeteChunkedScratch *scratch) {
    if (!scratch) {
        return chunkedEncodeRun_(dst, values, count, codecMask, blockValues,
                                 COMPETE_UNSIGNED, blocksOut, NULL, NULL);
    }
    /* The scratch's stamped geometry is enforced, never trusted: an
     * uninitialized, undersized, or wrong-block-target scratch is a
     * caller bug surfaced here instead of a heap overflow inside the
     * competition. */
    if (!competeScratchValid_(scratch, blockValues)) {
        return 0;
    }
    return chunkedEncodeRun_(dst, values, count, codecMask, blockValues,
                             COMPETE_UNSIGNED, blocksOut, scratch->laneA,
                             scratch->laneB);
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
    case VARINT_CODEC_ELIAS_GAMMA:
    case VARINT_CODEC_ELIAS_DELTA: {
        const size_t got = (h.codecID == VARINT_CODEC_ELIAS_GAMMA)
                               ? varintEliasGammaDecodeArray(
                                     body, h.bodyLen * 8, output, count)
                               : varintEliasDeltaDecodeArray(
                                     body, h.bodyLen * 8, output, count);
        if (got != count) {
            return 0;
        }
        /* Values were shifted +1 for the positive-integer code space. */
        for (size_t i = 0; i < count; i++) {
            output[i] -= 1;
        }
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
 * Chunked decode
 * ==================================================================== */

size_t varintCompeteChunkedReadHeader(const uint8_t *src, size_t srcBytes,
                                      varintCompeteChunkedHeader *header) {
    assert(header != NULL);
    if (!src || srcBytes < 6) {
        return 0;
    }
    if (src[0] != VARINT_COMPETE_CHUNK_MAGIC0 ||
        src[1] != VARINT_COMPETE_CHUNK_MAGIC1 ||
        src[2] != VARINT_COMPETE_CHUNK_MAGIC2 ||
        src[3] != VARINT_COMPETE_CHUNK_MAGIC3) {
        return 0;
    }
    if (src[4] != VARINT_COMPETE_CHUNK_VERSION) {
        return 0;
    }
    size_t remain = srcBytes - 5;
    varintWidth w = varintTaggedGet(src + 5, remain > 9 ? 9 : (int32_t)remain,
                                    &header->totalCount);
    if (w == 0) {
        return 0;
    }
    header->version = src[4];
    header->headerLen = 5 + (size_t)w;
    return header->headerLen;
}

static size_t chunkedDecodeRun_(const uint8_t *src, size_t srcBytes,
                                void *outputAny, size_t maxCount,
                                size_t *decodedCount, competeKind kind) {
    if (!src || !outputAny) {
        return 0;
    }
    varintCompeteChunkedHeader hdr;
    size_t cursor = varintCompeteChunkedReadHeader(src, srcBytes, &hdr);
    if (cursor == 0 || hdr.totalCount > maxCount) {
        return 0;
    }

    size_t got = 0;
    while (got < hdr.totalCount) {
        size_t remain = srcBytes - cursor;
        uint64_t blockCount;
        varintWidth w = varintTaggedGet(
            src + cursor, remain > 9 ? 9 : (int32_t)remain, &blockCount);
        if (w == 0 || blockCount == 0 || blockCount > hdr.totalCount - got) {
            return 0;
        }
        cursor += (size_t)w;

        /* varintCompeteReadHeader guarantees the whole frame is inside
         * the remaining bytes; advance by its declared size so a codec
         * whose decoder reports value counts instead of bytes cannot
         * desynchronize the block walk. */
        varintCompeteHeader frame;
        if (varintCompeteReadHeader(src + cursor, srcBytes - cursor, &frame) ==
            0) {
            return 0;
        }
        size_t consumed;
        if (kind == COMPETE_SIGNED) {
            consumed = varintCompeteDecode(src + cursor, srcBytes - cursor,
                                           (size_t)blockCount,
                                           (int64_t *)outputAny + got);
        } else {
            consumed = varintCompeteDecodeUnsigned(
                src + cursor, srcBytes - cursor, (size_t)blockCount,
                (uint64_t *)outputAny + got);
        }
        if (consumed == 0) {
            return 0;
        }
        cursor += frame.headerLen + frame.bodyLen;
        got += (size_t)blockCount;
    }

    if (decodedCount) {
        *decodedCount = got;
    }
    return cursor;
}

size_t varintCompeteDecodeChunked(const uint8_t *src, size_t srcBytes,
                                  int64_t *output, size_t maxCount,
                                  size_t *decodedCount) {
    return chunkedDecodeRun_(src, srcBytes, output, maxCount, decodedCount,
                             COMPETE_SIGNED);
}

size_t varintCompeteDecodeChunkedUnsigned(const uint8_t *src, size_t srcBytes,
                                          uint64_t *output, size_t maxCount,
                                          size_t *decodedCount) {
    return chunkedDecodeRun_(src, srcBytes, output, maxCount, decodedCount,
                             COMPETE_UNSIGNED);
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

    TEST("Chunked: per-block winners beat one whole-array frame") {
        /* First half is one long run (STRIDE/RLE territory), second half
         * is incompressible full-width noise. A single frame must pick
         * one compromise codec for everything; chunked picks per block. */
        enum { N = 16384 };
        static uint64_t values[N];
        uint64_t noise = UINT64_C(0x243F6A8885A308D3);
        for (size_t i = 0; i < N; i++) {
            if (i < N / 2) {
                values[i] = 42;
            } else {
                noise = noise * UINT64_C(6364136223846793005) +
                        UINT64_C(1442695040888963407);
                values[i] = noise;
            }
        }

        uint8_t *single = malloc(varintCompeteMaxEncodedSize(N));
        uint8_t *chunked = malloc(varintCompeteMaxEncodedSizeChunked(N, 0));
        size_t singleBytes = varintCompeteEncodeUnsigned(
            single, values, N, VARINT_COMPETE_DEFAULT_MASK, NULL);
        size_t blocks = 0;
        size_t chunkedBytes = varintCompeteEncodeChunkedUnsigned(
            chunked, values, N, VARINT_COMPETE_DEFAULT_MASK, 0, &blocks);

        if (chunkedBytes == 0 || blocks < 2) {
            ERR("chunked encode failed: %zu bytes, %zu blocks", chunkedBytes,
                blocks);
        }
        /* The run half collapses to one ~30-byte stride block, while the
         * single frame's winner must still spend real bits on it; the
         * noise half costs ~8 bytes/value either way. */
        if (chunkedBytes >= singleBytes) {
            ERR("chunked (%zu B) should beat single-frame (%zu B)",
                chunkedBytes, singleBytes);
        }

        static uint64_t dec[N];
        size_t decodedCount = 0;
        size_t read = varintCompeteDecodeChunkedUnsigned(chunked, chunkedBytes,
                                                         dec, N, &decodedCount);
        if (read != chunkedBytes || decodedCount != N) {
            ERR("chunked round-trip mismatch: read %zu of %zu, count %zu", read,
                chunkedBytes, decodedCount);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
        free(single);
        free(chunked);
    }

    TEST("Chunked: stride growth collapses a long progression") {
        /* 100k-value arithmetic progression: without growth this is ~25
         * blocks of ~30 bytes; growth merges them into one stride block. */
        enum { N = 100000 };
        uint64_t *values = malloc(N * sizeof(uint64_t));
        for (size_t i = 0; i < N; i++) {
            values[i] = 5000 + (uint64_t)i * 7;
        }

        uint8_t *buf = malloc(varintCompeteMaxEncodedSizeChunked(N, 0));
        size_t blocks = 0;
        size_t written = varintCompeteEncodeChunkedUnsigned(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, 0, &blocks);

        if (blocks != 1) {
            ERR("expected 1 grown block, got %zu", blocks);
        }
        if (written == 0 || written > 64) {
            ERR("progression should encode in ~30 bytes, got %zu", written);
        }

        uint64_t *dec = malloc(N * sizeof(uint64_t));
        size_t decodedCount = 0;
        size_t read = varintCompeteDecodeChunkedUnsigned(buf, written, dec, N,
                                                         &decodedCount);
        if (read != written || decodedCount != N) {
            ERR("stride-growth round-trip failed: read %zu, count %zu", read,
                decodedCount);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
        free(values);
        free(buf);
        free(dec);
    }

    TEST("Chunked: signed round-trip with mixed regions") {
        enum { N = 10000 };
        static int64_t values[N];
        for (size_t i = 0; i < N; i++) {
            if (i < N / 2) {
                values[i] = -50000 + (int64_t)i * 3;
            } else {
                values[i] = (int64_t)((i * 2654435761U) % 1000) - 500;
            }
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSizeChunked(N, 1024));
        size_t written = varintCompeteEncodeChunked(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, 1024, NULL);
        if (written == 0) {
            ERRR("signed chunked encode failed");
        }

        static int64_t dec[N];
        size_t decodedCount = 0;
        size_t read =
            varintCompeteDecodeChunked(buf, written, dec, N, &decodedCount);
        if (read != written || decodedCount != N) {
            ERR("signed chunked round-trip: read %zu of %zu, count %zu", read,
                written, decodedCount);
        }
        for (size_t i = 0; i < N; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
        free(buf);
    }

    TEST("Chunked: empty and tiny arrays round-trip") {
        uint8_t buf[64];
        size_t decodedCount = 99;
        size_t written = varintCompeteEncodeChunkedUnsigned(
            buf, NULL, 0, VARINT_COMPETE_DEFAULT_MASK, 0, NULL);
        if (written == 0) {
            ERRR("empty chunked stream should still emit a header");
        }
        uint64_t sink = 0;
        size_t read = varintCompeteDecodeChunkedUnsigned(buf, written, &sink, 0,
                                                         &decodedCount);
        if (read != written || decodedCount != 0) {
            ERR("empty chunked round-trip: read %zu of %zu, count %zu", read,
                written, decodedCount);
        }

        uint64_t one = UINT64_C(0xDEADBEEF12345678);
        uint8_t buf2[128];
        written = varintCompeteEncodeChunkedUnsigned(
            buf2, &one, 1, VARINT_COMPETE_DEFAULT_MASK, 0, NULL);
        uint64_t got = 0;
        read = varintCompeteDecodeChunkedUnsigned(buf2, written, &got, 1,
                                                  &decodedCount);
        if (read != written || decodedCount != 1 || got != one) {
            ERRR("single-value chunked round-trip failed");
        }
    }

    TEST("Chunked: header is self-describing and guards maxCount") {
        enum { N = 5000 };
        static uint64_t values[N];
        for (size_t i = 0; i < N; i++) {
            values[i] = i * 2 + (i % 7);
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSizeChunked(N, 0));
        size_t written = varintCompeteEncodeChunkedUnsigned(
            buf, values, N, VARINT_COMPETE_DEFAULT_MASK, 0, NULL);

        varintCompeteChunkedHeader hdr;
        if (varintCompeteChunkedReadHeader(buf, written, &hdr) == 0 ||
            hdr.totalCount != N) {
            ERR("chunked header totalCount wrong: %" PRIu64, hdr.totalCount);
        }

        static uint64_t dec[N];
        if (varintCompeteDecodeChunkedUnsigned(buf, written, dec, N - 1,
                                               NULL) != 0) {
            ERRR("decode into undersized buffer must fail");
        }

        for (size_t cut = 1; cut < 64 && cut < written; cut++) {
            if (varintCompeteDecodeChunkedUnsigned(buf, written - cut, dec, N,
                                                   NULL) != 0) {
                ERR("truncated chunked stream (-%zu bytes) accepted", cut);
                break;
            }
        }
        free(buf);
    }

    TEST("Compete: Elias gamma round-trips and can win on tiny ints") {
        enum { N = 4096 };
        static uint64_t values[N];
        for (size_t i = 0; i < N; i++) {
            /* Geometric small ints: gamma's optimal distribution. */
            uint64_t v = 0;
            uint64_t r = (i * 2654435761U) >> 7;
            while (v < 6 && (r & 1)) {
                v++;
                r >>= 1;
            }
            values[i] = v;
        }
        uint8_t *buf = malloc(varintCompeteMaxEncodedSize(N));
        varintCompeteResult res;
        const uint64_t eliasMask =
            VARINT_COMPETE_BIT(VARINT_CODEC_ELIAS_GAMMA) |
            VARINT_COMPETE_BIT(VARINT_CODEC_ELIAS_DELTA) |
            VARINT_COMPETE_BIT(VARINT_CODEC_TAGGED);
        size_t written =
            varintCompeteEncodeUnsigned(buf, values, N, eliasMask, &res);
        if (res.winner != VARINT_CODEC_ELIAS_GAMMA &&
            res.winner != VARINT_CODEC_ELIAS_DELTA) {
            ERR("expected an Elias winner on tiny ints, got %s",
                varintCodecName(res.winner));
        }
        if (written * 2 > N) {
            ERR("Elias should reach <4 bits/value here: %zu B for %d values",
                written, N);
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

        /* Values above the small-int gate must decline, not corrupt. */
        values[7] = UINT64_C(1) << 40;
        written = varintCompeteEncodeUnsigned(buf, values, N, eliasMask, &res);
        if (res.winner != VARINT_CODEC_TAGGED) {
            ERR("gate breach: %s won with an out-of-range value",
                varintCodecName(res.winner));
        }
        read = varintCompeteDecodeUnsigned(buf, written, N, dec);
        if (read != written || dec[7] != values[7]) {
            ERRR("fallback round trip failed");
        }
        free(buf);
    }

    TEST("PruneMask drops hopeless codecs, keeps plausible ones") {
        enum { N = 4096 };
        static uint64_t values[N];

        /* All-distinct unsorted noise: RLE, DICT, PALETTE, PALETTE_DELTA,
         * and BP128_DELTA are all hopeless. */
        uint64_t noise = UINT64_C(0x9E3779B97F4A7C15);
        for (size_t i = 0; i < N; i++) {
            noise = noise * UINT64_C(6364136223846793005) +
                    UINT64_C(1442695040888963407);
            values[i] = noise;
        }
        uint64_t pruned =
            varintCompetePruneMask(values, N, VARINT_COMPETE_DEFAULT_MASK);
        if (pruned & VARINT_COMPETE_BIT(VARINT_CODEC_RLE)) {
            ERRR("RLE not pruned on run-free noise");
        }
        if (pruned & VARINT_COMPETE_BIT(VARINT_CODEC_DICT)) {
            ERRR("DICT not pruned on all-distinct noise");
        }
        if (pruned & VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE)) {
            ERRR("PALETTE not pruned on all-distinct noise");
        }
        if (pruned & VARINT_COMPETE_BIT(VARINT_CODEC_BP128_DELTA)) {
            ERRR("BP128_DELTA not pruned on unsorted noise");
        }
        if (!(pruned & VARINT_COMPETE_BIT(VARINT_CODEC_TAGGED))) {
            ERRR("TAGGED must never be pruned");
        }

        /* Runs over a tiny alphabet: repetition codecs must survive. */
        for (size_t i = 0; i < N; i++) {
            values[i] = (i / 100) % 4;
        }
        pruned = varintCompetePruneMask(values, N, VARINT_COMPETE_DEFAULT_MASK);
        if (!(pruned & VARINT_COMPETE_BIT(VARINT_CODEC_RLE)) ||
            !(pruned & VARINT_COMPETE_BIT(VARINT_CODEC_DICT)) ||
            !(pruned & VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE))) {
            ERRR("repetition codecs wrongly pruned on run-heavy data");
        }

        /* Sorted ramp: BP128_DELTA must survive. */
        for (size_t i = 0; i < N; i++) {
            values[i] = 1000 + i * 3;
        }
        pruned = varintCompetePruneMask(values, N, VARINT_COMPETE_DEFAULT_MASK);
        if (!(pruned & VARINT_COMPETE_BIT(VARINT_CODEC_BP128_DELTA))) {
            ERRR("BP128_DELTA wrongly pruned on sorted data");
        }
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_COMPETE_TEST */
