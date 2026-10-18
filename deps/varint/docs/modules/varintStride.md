# varintStride: Arithmetic-Progression Detection

## Overview

**varintStride** detects and encodes arithmetic progressions — sequences where consecutive values differ by a constant — into a constant-size record regardless of length. It supports an **exact** mode (every delta matches) and a **fuzzy** mode (up to 20% outliers allowed, stored as `(index, actual)` patches).

Real workloads are full of arithmetic progressions: paginated IDs, page offsets, polling intervals, fixed-grid coordinates. varintStride compresses an arbitrarily long such sequence into ~24 bytes.

**Key Features:** Exact + fuzzy modes, SIMD-accelerated mismatch detection (NEON + AVX2 with scalar fallback), O(1) random access in exact mode, self-describing wire format, fuzzy mode degrades gracefully into per-exception storage.

## Key Characteristics

| Property        | Value                                                       |
| --------------- | ----------------------------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)                                 |
| Encoding Format | `[mode][base][stride][count]` + optional exceptions         |
| Best For        | Arithmetic progressions with ≤20% noise                     |
| Compression     | Effectively infinite for exact (count-independent ~24 B)    |
| Random Access   | O(1) exact mode; O(exceptionCount) fuzzy mode               |
| SIMD            | NEON (2-wide int64) and AVX2 (4-wide int64) when count ≥ 16 |

## Encoding Format

**Exact mode:**

```
[mode:1=0][base:signed-zigzag-width-tagged][stride:signed-zigzag-width-tagged][count:tagged]
```

**Fuzzy mode:**

```
[mode:1=1][base][stride][count][excCount:tagged]
[idx1:tagged][actual1:signed-zigzag-width-tagged]
[idx2:tagged][actual2:signed-zigzag-width-tagged] ...
```

The mode byte is the first byte of the buffer, so dispatchers can peek without parsing further (`varintStrideGetMode`).

## SIMD Path

`strideCountMismatches` is the heart of analysis: given a candidate stride and an array, count how many consecutive deltas fail to match. The scalar loop is one subtract + one compare per element. The SIMD path broadcasts the candidate stride into a vector register and uses `vceqq_s64` (NEON) or `_mm256_cmpeq_epi64` (AVX2) to test 2 or 4 deltas at a time.

The dispatcher (`strideCountMismatches_`) picks NEON/AVX2/scalar at compile time, with a runtime length threshold (`VARINT_STRIDE_SIMD_MIN_COUNT = 16`) below which scalar wins on setup overhead.

A second SIMD primitive, `varintStrideMatchingPrefix[Unsigned]`, returns the length of the longest prefix forming one exact arithmetic progression, exiting at the first deviating vector group so the cost is bounded by the run length. It processes 8 values per iteration with the four compare results ANDed into a single vector→scalar check — per-vector lane extracts are expensive enough (especially on Apple silicon) that a 2-wide check-every-vector loop measures _slower_ than scalar. This is the block-growth planner behind `varintCompete`'s chunked mode; measure it with `varintCompeteBench`.

## API

```c
bool varintStrideAnalyze(const int64_t *values, size_t count,
                         varintStrideMeta *meta);
bool varintStrideAnalyzeUnsigned(const uint64_t *values, size_t count,
                                 varintStrideMeta *meta);
bool varintStrideIsBeneficial(const int64_t *values, size_t count);

size_t varintStrideEncode(uint8_t *dst, const int64_t *values, size_t count,
                          varintStrideMeta *meta);
size_t varintStrideEncodeWithMode(uint8_t *dst, const int64_t *values,
                                  size_t count, varintStrideMode mode,
                                  varintStrideMeta *meta);
size_t varintStrideEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                  size_t count, varintStrideMeta *meta);

size_t varintStrideDecode(const uint8_t *src, size_t count, int64_t *output);
size_t varintStrideDecodeUnsigned(const uint8_t *src, size_t count,
                                  uint64_t *output);

size_t varintStrideReadMeta(const uint8_t *src, varintStrideMeta *meta);
int64_t varintStrideGetAt(const uint8_t *src, size_t index);
varintStrideMode varintStrideGetMode(const uint8_t *src);

size_t varintStrideMaxEncodedSize(size_t count);
```

`varintStrideMeta` carries `base`, `stride`, `count`, `exceptionCount`, `encodedSize`, and `mode`.

## When to Use

| Pattern                                           | Choice           |
| ------------------------------------------------- | ---------------- |
| `1, 2, 3, ..., N` or `N, N+k, N+2k, ...`          | **Stride exact** |
| Arithmetic progression with a handful of outliers | **Stride fuzzy** |
| Regular-interval timestamps                       | Stride exact     |
| Monotonic but irregular                           | varintDelta      |
| Smooth but irregular                              | varintDeltaDelta |
| Random, clustered values                          | varintFOR/PFOR   |

## Integration

- Codec ID: `VARINT_CODEC_STRIDE` (=8) in `varintTelemetry.h`
- Included in `VARINT_COMPETE_DEFAULT_MASK`
- Test target: `varintStrideTest` (registered as ctest `varint-stride`)

## See Also

- [varintDelta](varintDelta.md), [varintDeltaDelta](varintDeltaDelta.md) — neighboring encodings on the regularity spectrum
- [varintCompete](varintCompete.md) — picks Stride when it wins
