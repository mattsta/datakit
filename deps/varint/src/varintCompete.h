#pragma once

#include "varint.h"
#include "varintTelemetry.h"

__BEGIN_DECLS

/* ====================================================================
 * varintCompete — Evidence-Based Codec Selection
 * ====================================================================
 * Where varintAdaptive picks a codec by heuristic and writes it,
 * varintCompete *runs* a configurable subset of codecs, measures actual
 * encoded sizes, and keeps the smallest output. Each candidate's
 * call/win/byte count is recorded in varintTelemetry when that feature
 * is compiled in.
 *
 * Output frame (separate from varintAdaptive's 1-byte header so the two
 * stay independent):
 *   [magic:4 = 'V' 'C' 'M' 'P'][version:1][codecID:1][bodyLen:tagged][body...]
 *
 * The magic + version make this format identifiable on disk and let
 * future versions change the body framing without ambiguity. */

#define VARINT_COMPETE_MAGIC0 'V'
#define VARINT_COMPETE_MAGIC1 'C'
#define VARINT_COMPETE_MAGIC2 'M'
#define VARINT_COMPETE_MAGIC3 'P'
#define VARINT_COMPETE_VERSION 1

/* Bit flags into the codec mask. Order matches varintCodecID values. */
#define VARINT_COMPETE_BIT(id) (1ULL << (id))

/* Convenient preset masks. */
#define VARINT_COMPETE_DEFAULT_MASK                                            \
    (VARINT_COMPETE_BIT(VARINT_CODEC_TAGGED) |                                 \
     VARINT_COMPETE_BIT(VARINT_CODEC_DELTA) |                                  \
     VARINT_COMPETE_BIT(VARINT_CODEC_DELTA_DELTA) |                            \
     VARINT_COMPETE_BIT(VARINT_CODEC_STRIDE) |                                 \
     VARINT_COMPETE_BIT(VARINT_CODEC_FOR) |                                    \
     VARINT_COMPETE_BIT(VARINT_CODEC_PFOR) |                                   \
     VARINT_COMPETE_BIT(VARINT_CODEC_DICT) |                                   \
     VARINT_COMPETE_BIT(VARINT_CODEC_RLE) |                                    \
     VARINT_COMPETE_BIT(VARINT_CODEC_BP128) |                                  \
     VARINT_COMPETE_BIT(VARINT_CODEC_BP128_DELTA) |                            \
     VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE) |                                \
     VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE_DELTA))

/* Mask of every codec compete knows how to run. */
#define VARINT_COMPETE_ALL_MASK 0xFFFFFFFFULL

/* Per-candidate result, recorded for every codec actually evaluated. */
typedef struct varintCompeteCandidate {
    varintCodecID id;
    size_t encodedSize; /* 0 if the codec declined / errored */
} varintCompeteCandidate;

/* Aggregate result for a single varintCompete call. */
typedef struct varintCompeteResult {
    varintCodecID winner;
    size_t winnerSize;          /* body size, excluding frame header */
    size_t frameSize;           /* total bytes emitted */
    size_t candidatesEvaluated; /* how many codecs ran */
    varintCompeteCandidate candidates[VARINT_CODEC_MAX];
} varintCompeteResult;

_Static_assert(sizeof(varintCompeteCandidate) == 16,
               "varintCompeteCandidate size changed!");

/* Frame header parsed back from src. */
typedef struct varintCompeteHeader {
    varintCodecID codecID;
    uint8_t version;
    size_t bodyLen;
    size_t headerLen; /* bytes occupied by the frame header itself */
} varintCompeteHeader;

/* ====================================================================
 * Encode
 * ==================================================================== */

/* Maximum output size for compete on count values.
 * Frame header is bounded: 4 magic + 1 version + 1 codec + up to 9 tagged
 *   bytes for bodyLen. Body is bounded by varintAdaptiveMaxSize. */
size_t varintCompeteMaxEncodedSize(size_t count);

/* Encode signed array, running every codec in `mask` and keeping the
 * smallest. Returns frame bytes written. result may be NULL. */
size_t varintCompeteEncode(uint8_t *dst, const int64_t *values, size_t count,
                           uint64_t codecMask, varintCompeteResult *result);

/* Encode unsigned array. */
size_t varintCompeteEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                   size_t count, uint64_t codecMask,
                                   varintCompeteResult *result);

/* ====================================================================
 * Decode
 * ==================================================================== */

/* Parse the frame header. Returns bytes consumed, or 0 on malformed
 * input (wrong magic, version mismatch, or unknown codec ID). */
size_t varintCompeteReadHeader(const uint8_t *src, size_t srcBytes,
                               varintCompeteHeader *header);

/* Decode signed array. count must match the encoded count.
 * Returns bytes consumed. */
size_t varintCompeteDecode(const uint8_t *src, size_t srcBytes, size_t count,
                           int64_t *output);

/* Decode unsigned array. */
size_t varintCompeteDecodeUnsigned(const uint8_t *src, size_t srcBytes,
                                   size_t count, uint64_t *output);

#ifdef VARINT_COMPETE_TEST
int varintCompeteTest(int argc, char *argv[]);
#endif

__END_DECLS
