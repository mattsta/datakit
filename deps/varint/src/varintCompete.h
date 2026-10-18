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
 * future versions change the body framing without ambiguity.
 *
 * The chunked layer splits large arrays into blocks and runs the
 * competition per block, so heterogeneous data gets a different winner
 * per region instead of one whole-array compromise. Chunked streams are
 * fully self-describing (they carry their own counts):
 *   [magic:4 = 'V' 'C' 'H' 'K'][version:1][totalCount:tagged]
 *   then per block: [blockCount:tagged][VCMP frame]
 * Blocks whose values form one exact arithmetic progression are extended
 * past the target block size (the stride record is constant-size, so a
 * constant or ramp region costs one tiny block no matter how long). */

#define VARINT_COMPETE_MAGIC0 'V'
#define VARINT_COMPETE_MAGIC1 'C'
#define VARINT_COMPETE_MAGIC2 'M'
#define VARINT_COMPETE_MAGIC3 'P'
#define VARINT_COMPETE_VERSION 1

#define VARINT_COMPETE_CHUNK_MAGIC0 'V'
#define VARINT_COMPETE_CHUNK_MAGIC1 'C'
#define VARINT_COMPETE_CHUNK_MAGIC2 'H'
#define VARINT_COMPETE_CHUNK_MAGIC3 'K'
#define VARINT_COMPETE_CHUNK_VERSION 1

/* Default per-block value count for the chunked encoders (blockValues=0). */
#define VARINT_COMPETE_CHUNK_DEFAULT_VALUES 4096U

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
     VARINT_COMPETE_BIT(VARINT_CODEC_PALETTE_DELTA) |                          \
     VARINT_COMPETE_BIT(VARINT_CODEC_ELIAS_GAMMA) |                            \
     VARINT_COMPETE_BIT(VARINT_CODEC_ELIAS_DELTA))

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

/* Chunked stream header parsed back from src. */
typedef struct varintCompeteChunkedHeader {
    uint64_t totalCount; /* values in the whole stream */
    size_t headerLen;    /* bytes occupied by the stream header itself */
    uint8_t version;
} varintCompeteChunkedHeader;

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
 * Candidate pruning
 * ==================================================================== */

/* Probe a small sample of the array and drop codecs that cannot
 * plausibly win from `codecMask`:
 *   - RLE when the sample contains no adjacent-equal pair
 *   - DICT / PALETTE when nearly every sampled value is distinct
 *   - PALETTE_DELTA when nearly every sampled delta is distinct
 *   - BP128_DELTA when the sample proves the array unsorted
 * TAGGED is never pruned, so the fallback guarantee holds. Pruning only
 * changes which candidates run (encode CPU), never decodability; a
 * misjudged probe costs at most a slightly larger winner. Arrays under
 * 128 values are returned unpruned — the probe savings are negligible. */
uint64_t varintCompetePruneMask(const uint64_t *values, size_t count,
                                uint64_t codecMask);

/* ====================================================================
 * Chunked encode — per-block competition
 * ==================================================================== */

/* Maximum output size for a chunked encode of count values at the given
 * per-block target (0 = VARINT_COMPETE_CHUNK_DEFAULT_VALUES). */
size_t varintCompeteMaxEncodedSizeChunked(size_t count, size_t blockValues);

/* Encode signed array as a chunked stream: the codec competition runs
 * independently per block of ~blockValues values (0 = default), so each
 * region gets its own winner. blocksOut (optional) receives the number
 * of blocks emitted. Returns stream bytes written, 0 on failure. */
size_t varintCompeteEncodeChunked(uint8_t *dst, const int64_t *values,
                                  size_t count, uint64_t codecMask,
                                  size_t blockValues, size_t *blocksOut);

/* Encode unsigned array as a chunked stream. Each block's candidate set
 * is first narrowed by varintCompetePruneMask over that block. */
size_t varintCompeteEncodeChunkedUnsigned(uint8_t *dst, const uint64_t *values,
                                          size_t count, uint64_t codecMask,
                                          size_t blockValues,
                                          size_t *blocksOut);

/* Typed scratch for the chunked encoder: the client owns the backing
 * memory (stack or heap) and varintCompeteChunkedScratchInit carves and
 * validates it, stamping the geometry so every later use can audit
 * that the scratch is initialized, sized, and shaped for the block
 * target it is applied to. Never fill this struct by hand. */
typedef struct varintCompeteChunkedScratch {
    uint8_t *laneA;
    uint8_t *laneB;
    size_t laneBytes;   /* capacity of each lane */
    size_t blockValues; /* normalized block target this scratch serves */
    uint32_t magic;     /* proves Init ran; every use validates it */
} varintCompeteChunkedScratch;

#define VARINT_COMPETE_SCRATCH_MAGIC UINT32_C(0x56435353) /* "VCSS" */

/* Total backing bytes Init needs for a given per-block target
 * (0 = default). */
size_t varintCompeteChunkedScratchBytes(size_t blockValues);

/* Bind scratch to caller memory: mem/memBytes is the backing store
 * (at least varintCompeteChunkedScratchBytes(blockValues) bytes; the
 * requirement is enforced, not trusted). Returns false on NULL or
 * undersized memory, leaving the scratch unusable. */
bool varintCompeteChunkedScratchInit(varintCompeteChunkedScratch *scratch,
                                     uint8_t *mem, size_t memBytes,
                                     size_t blockValues);

/* As varintCompeteEncodeChunkedUnsigned, but working in caller-provided
 * scratch instead of allocating per call. The scratch must have been
 * bound by varintCompeteChunkedScratchInit for the SAME blockValues
 * target — magic, geometry, and block target are all validated, and a
 * mismatch is rejected (returns 0) rather than trusted. NULL scratch
 * falls back to internal allocation. */
size_t varintCompeteEncodeChunkedUnsignedScratch(
    uint8_t *dst, const uint64_t *values, size_t count, uint64_t codecMask,
    size_t blockValues, size_t *blocksOut,
    const varintCompeteChunkedScratch *scratch);

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

/* ====================================================================
 * Chunked decode
 * ==================================================================== */

/* Parse the chunked stream header. Returns bytes consumed, or 0 on
 * malformed input. Use header->totalCount to size the output buffer —
 * chunked streams carry their own counts, so the decoder needs no
 * external count. */
size_t varintCompeteChunkedReadHeader(const uint8_t *src, size_t srcBytes,
                                      varintCompeteChunkedHeader *header);

/* Decode a chunked signed stream into output (capacity maxCount values).
 * decodedCount (optional) receives the number of values produced.
 * Returns bytes consumed, or 0 on malformed input or if the stream
 * holds more than maxCount values. */
size_t varintCompeteDecodeChunked(const uint8_t *src, size_t srcBytes,
                                  int64_t *output, size_t maxCount,
                                  size_t *decodedCount);

/* Decode a chunked unsigned stream. */
size_t varintCompeteDecodeChunkedUnsigned(const uint8_t *src, size_t srcBytes,
                                          uint64_t *output, size_t maxCount,
                                          size_t *decodedCount);

#ifdef VARINT_COMPETE_TEST
int varintCompeteTest(int argc, char *argv[]);
#endif

__END_DECLS
