# varintCompete: Evidence-Based Codec Selection

## Overview

**varintCompete** is the self-managing layer above the individual codecs. Where `varintAdaptive` chooses a codec by single-pass heuristics and writes the result, `varintCompete` **runs** a configurable subset of codecs, measures the actual encoded bytes each produces, and emits the smallest — wrapped in a self-describing frame so the decoder can pick the matching codec automatically.

For large arrays there is a **chunked mode** that runs the competition independently per block (default 4096 values), so heterogeneous data gets a different winner per region instead of one whole-array compromise. Chunked streams carry their own value counts — the decoder needs no external count — and blocks that form one exact arithmetic progression are grown past the block target into a single constant-size stride record, so a million-value constant or ramp region costs ~30 bytes total. On the unsigned path each block's candidate set is first narrowed by a sampling probe (`varintCompetePruneMask`) that skips codecs which cannot plausibly win, recovering most of the encode CPU that running every codec would cost.

**Key Features:** Configurable codec mask, self-describing frame format with magic + version, per-block chunked competition with stride growth and probe-based candidate pruning, per-call result struct exposing every candidate's size, opt-in atomic telemetry (`varintTelemetry`, including per-codec wall-clock time) for production tuning, full round-trip across DELTA/DELTA_DELTA/STRIDE/TAGGED on signed input and the additional FOR/PFOR/RLE/DICT/PALETTE/BP128 family for unsigned input.

## Key Characteristics

| Property       | Value                                                           |
| -------------- | --------------------------------------------------------------- |
| Implementation | Header (.h) + Compiled (.c)                                     |
| Frame Format   | `[VCMP magic:4][version:1][codecID:1][bodyLen:tagged][body]`    |
| Chunked Format | `[VCHK magic:4][version:1][totalCount]` + per-block VCMP frames |
| Selection      | Smallest encoded byte count wins, per frame or per block        |
| Cost           | N × encode passes per call/block (chunked mode prunes N first)  |
| Decoder        | Self-describing — chunked streams even carry their own counts   |
| Threading      | Single-threaded; safe across threads when output buffers differ |

## Frame Format

```
Offset  Size  Field
   0     4    Magic = 'V' 'C' 'M' 'P'
   4     1    Version (currently 1)
   5     1    Codec ID (stable enum varintCodecID)
   6     1-9  bodyLen (tagged varint)
   ?     N    Body — produced by the winning codec
```

The magic + version make compete frames identifiable on disk; unknown codec IDs can be skipped using `bodyLen` without parsing the body.

## Chunked Stream Format

```
Offset  Size  Field
   0     4    Magic = 'V' 'C' 'H' 'K'
   4     1    Version (currently 1)
   5     1-9  totalCount (tagged varint)
then per block:
   ?     1-9  blockCount (tagged varint)
   ?     ?    A complete VCMP frame for blockCount values
```

Design notes:

- **Per-block winners.** A stream that is stride-like in one region and dictionary-like in another gets the right codec for each region. Scratch memory is bounded by the block size, not the array size, and one scratch pair is reused across all blocks.
- **Stride growth.** When an entire planned block is one exact arithmetic progression, the block is extended for as long as the progression continues before emitting a single ~30-byte exact-stride record. Without this, a long constant/ramp region would pay one block header per block for identical tiny bodies.
- **Self-contained.** `totalCount` and each `blockCount` live in the stream, so `varintCompeteChunkedReadHeader` + `varintCompeteDecodeChunked*` can size and fill the output with no side-channel metadata. Truncated streams are rejected at the header layer before any body is parsed.
- **Parallel-friendly without threads.** The library stays single-threaded and dependency-free; because block boundaries are discoverable by a cheap header walk (skip `headerLen + bodyLen` per frame) and blocks decode independently, callers that want parallel decode can distribute blocks across their own workers.

## Candidate Pruning

`varintCompetePruneMask(values, count, codecMask)` samples up to 4 windows of 64 adjacent values spread across the array and removes codecs that cannot plausibly win:

| Codec         | Pruned when                                       |
| ------------- | ------------------------------------------------- |
| RLE           | the sample contains no adjacent-equal pair        |
| DICT, PALETTE | >75% of sampled values are distinct               |
| PALETTE_DELTA | >75% of sampled adjacent deltas are distinct      |
| BP128_DELTA   | the sample proves the array unsorted (exact rule) |

TAGGED is never pruned, so the fallback guarantee holds. Pruning only affects which candidates are evaluated — never decodability — and a misjudged probe costs at most a slightly larger winner. Arrays under 128 values are returned unpruned. The chunked unsigned encoder applies this per block automatically; the single-frame encoders leave the mask untouched so their candidate reporting stays exhaustive (pre-prune manually if you want the speedup there).

## Codec Mask

The codec mask is a `uint64_t` bitset over `varintCodecID` values (see `varintTelemetry.h`). Convenience masks:

```c
VARINT_COMPETE_DEFAULT_MASK  /* TAGGED, DELTA, DELTA_DELTA, STRIDE,
                                FOR, PFOR, DICT, RLE, BP128, BP128_DELTA,
                                PALETTE, PALETTE_DELTA,
                                ELIAS_GAMMA, ELIAS_DELTA */
VARINT_COMPETE_ALL_MASK      /* every known codec */
```

The Elias lanes encode values shifted by +1 (universal codes cover positive integers) and self-gate to columns whose values stay below 2^30 — the small-integer territory where gamma/delta codes actually win.

Build your own with `VARINT_COMPETE_BIT(VARINT_CODEC_x)`.

## API

```c
size_t varintCompeteMaxEncodedSize(size_t count);

size_t varintCompeteEncode(uint8_t *dst, const int64_t *values, size_t count,
                           uint64_t codecMask, varintCompeteResult *result);
size_t varintCompeteEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                   size_t count, uint64_t codecMask,
                                   varintCompeteResult *result);

size_t varintCompeteReadHeader(const uint8_t *src, size_t srcBytes,
                               varintCompeteHeader *header);

size_t varintCompeteDecode(const uint8_t *src, size_t srcBytes, size_t count,
                           int64_t *output);
size_t varintCompeteDecodeUnsigned(const uint8_t *src, size_t srcBytes,
                                   size_t count, uint64_t *output);

/* Chunked per-block competition (blockValues = 0 for the 4096 default) */
size_t varintCompeteMaxEncodedSizeChunked(size_t count, size_t blockValues);
size_t varintCompeteEncodeChunked(uint8_t *dst, const int64_t *values,
                                  size_t count, uint64_t codecMask,
                                  size_t blockValues, size_t *blocksOut);
size_t varintCompeteEncodeChunkedUnsigned(uint8_t *dst, const uint64_t *values,
                                          size_t count, uint64_t codecMask,
                                          size_t blockValues, size_t *blocksOut);
size_t varintCompeteChunkedReadHeader(const uint8_t *src, size_t srcBytes,
                                      varintCompeteChunkedHeader *header);
size_t varintCompeteDecodeChunked(const uint8_t *src, size_t srcBytes,
                                  int64_t *output, size_t maxCount,
                                  size_t *decodedCount);
size_t varintCompeteDecodeChunkedUnsigned(const uint8_t *src, size_t srcBytes,
                                          uint64_t *output, size_t maxCount,
                                          size_t *decodedCount);

/* Probe-based candidate pruning */
uint64_t varintCompetePruneMask(const uint64_t *values, size_t count,
                                uint64_t codecMask);
```

`varintCompeteResult` carries the winner ID, winner body size, frame size, and a `candidates[]` array recording every codec evaluated and its produced size (0 means the codec declined). Use this to learn which codecs are pulling weight on your data.

## Telemetry Integration

When compiled with `-DVARINT_TELEMETRY` (see `varintTelemetry.h`), every compete call updates atomic per-codec counters:

```c
#include "varintTelemetry.h"

varintTelemetryEntry snap[VARINT_CODEC_MAX];
varintTelemetrySnapshot(snap, VARINT_CODEC_MAX);
for (int32_t i = 0; i < VARINT_CODEC_MAX; i++) {
    if (snap[i].calls) {
        printf("%s: %" PRIu64 " calls, %" PRIu64 " wins, %" PRIu64
               " bytes, %" PRIu64 " ns\n",
               varintCodecName((varintCodecID)i), snap[i].calls, snap[i].wins,
               snap[i].bytesOut, snap[i].timeNs);
    }
}
```

`timeNs` accumulates the wall-clock nanoseconds each codec spent being evaluated during competitions, so `bytesOut`-per-win can be weighed against actual encode cost when deciding which codecs earn their place in a production mask. Counters compile to zero-cost no-ops when `VARINT_TELEMETRY` isn't defined.

## When to Use

- Pick **varintAdaptive** when speed matters and a 1-pass heuristic is enough.
- Pick **varintCompete** when you want the best possible size on heterogeneous data, are encoding once / reading many, or you're tuning which codecs to include in your build.

## Performance Validation

`varintCompeteBench` (built alongside the tests, not ctest-registered) is the permanent benchmark for this layer. It measures the SIMD-sensitive scan kernels the chunked encoder leans on per block — `varintStrideMatchingPrefixUnsigned`, `varintBP128IsSorted64/32`, and the pruning probe — against scalar references replicating the pre-SIMD code (with compiler barriers so neither side is hoisted), in both a cache-hot block regime and a bandwidth-bound streaming regime. It then reports chunked-vs-single-frame size and encode/decode throughput across dataset shapes; add a row to `datasets_[]` to grow coverage.

```bash
./build/src/varintCompeteBench [values] [repeats] [block-values]
```

Representative numbers (Apple M-series, NEON): matching-prefix scan ~6.2 Gval/s (~2.1x scalar hot, 2.6x streaming), isSorted64 ~8.2 Gval/s (~2.8x), isSorted32 ~8.2 Gval/s (~2.7x). The vectorized scans keep only one vector→scalar transfer per 8 values — a 2-wide NEON loop with per-vector lane extracts measured _slower_ than scalar, which is exactly the regression class this benchmark exists to catch.

## Integration

- Test target: `varintCompeteTest` (registered as ctest `varint-compete`)
- Benchmark target: `varintCompeteBench` (see Performance Validation)
- Frame format is intentionally distinct from `varintAdaptive`'s 1-byte codec header so the two layers don't conflict.

## See Also

- [varintAdaptive](../../src/varintAdaptive.h) — the heuristic counterpart
- [varintDeltaDelta](varintDeltaDelta.md), [varintStride](varintStride.md) — codecs new to this layer
- `varintTelemetry.h` — opt-in per-codec counters
