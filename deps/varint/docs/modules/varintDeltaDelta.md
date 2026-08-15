# varintDeltaDelta: Second-Order Delta (Gorilla-Style)

## Overview

**varintDeltaDelta** encodes regular-interval time series by storing the _difference between successive deltas_ rather than the raw values. For data that arrives at a steady cadence (timestamps every 60 s, sensors sampled at 1 kHz, monitoring counters with constant slope), the second-order delta is near-zero for almost every element — so each element packs into a 2-byte width-tagged record.

This module promotes the pattern previously demonstrated in `examples/integration/delta_compression.c` to a first-class module with the same `Encode/Decode/Analyze` shape as every other varint codec.

**Key Features:** Signed and unsigned variants, self-describing wire format, analyze-before-encode to predict size, full uint64 base-value range preserved on the unsigned path, ZigZag throughout for symmetric handling of positive and negative deltas.

## Key Characteristics

| Property        | Value                                         |
| --------------- | --------------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)                   |
| Encoding Format | `[base][delta1][dod1][dod2]...` width-tagged  |
| Best For        | Regular-interval timestamps, smooth sensors   |
| Compression     | 3-4× typical, near-zero overhead on flat dods |
| Random Access   | Sequential decode required (O(n) reconstruct) |

## Encoding Format

```
[base_width:1][base:1-8][delta1_width:1][delta1:1-8]
[dod1_width:1][dod1:1-8] [dod2_width:1][dod2:1-8] ...
```

- `base = values[0]`
- `delta1 = values[1] - values[0]`
- `dod[i] = (values[i+2] - values[i+1]) - (values[i+1] - values[i])`

All deltas and dods are ZigZag-encoded so negative values pack as efficiently as positive ones. The width-tag byte costs 1 byte per element; the payload is 1 byte for `dod == 0` (the common case) and grows only as deltas diverge.

## Compression Example

```
Input: timestamps every 60 s, 1000 entries (8 KB raw)
       → dod = 0 for all 998 dods after the first
Output: base (~5 B) + first delta (~2 B) + 998 × 2 B = ~2 KB
Ratio:  ~4× over raw int64
```

For comparison, the bit-packed Gorilla form (variable-width run encoding of dod) reaches 7-8×; our byte-tagged form trades a small slice of ratio for self-describing per-element width and zero bit-manipulation overhead. A future bit-packed mode could be added without changing the file format if a magic byte is reserved at the head.

## API

```c
size_t varintDeltaDeltaEncode(uint8_t *output, const int64_t *values,
                              size_t count, varintDeltaDeltaMeta *meta);

size_t varintDeltaDeltaDecode(const uint8_t *input, size_t count,
                              int64_t *output);

size_t varintDeltaDeltaEncodeUnsigned(uint8_t *output, const uint64_t *values,
                                      size_t count,
                                      varintDeltaDeltaMeta *meta);
size_t varintDeltaDeltaDecodeUnsigned(const uint8_t *input, size_t count,
                                      uint64_t *output);

bool varintDeltaDeltaAnalyze(const int64_t *values, size_t count,
                             varintDeltaDeltaMeta *meta);
bool varintDeltaDeltaIsBeneficial(const int64_t *values, size_t count);
size_t varintDeltaDeltaMaxEncodedSize(size_t count);  /* count * 9 worst-case */
```

`varintDeltaDeltaMeta` exposes `count`, `encodedSize`, `zeroDoD` (how many dods were exactly zero — a direct quality signal), and `oneByteDoD` (how many compressed into a 2-byte record).

## When to Use

| Use case                             | Choice           |
| ------------------------------------ | ---------------- |
| Timestamps at regular cadence        | **DeltaDelta**   |
| Sensor curves with smooth derivative | **DeltaDelta**   |
| Strictly arithmetic progression      | varintStride     |
| Monotonic but irregular intervals    | varintDelta      |
| Random, no temporal structure        | varintTagged/FOR |

## Integration

- Codec ID: `VARINT_CODEC_DELTA_DELTA` (=7) in `varintTelemetry.h`
- Included in `VARINT_COMPETE_DEFAULT_MASK` for `varintCompete`
- Test target: `varintDeltaDeltaTest` (registered as ctest `varint-delta-delta`)

## See Also

- [varintDelta](varintDelta.md) — first-order delta, the building block
- [varintStride](varintStride.md) — strict arithmetic progression (constant-size encoding)
- [varintCompete](varintCompete.md) — runs DoD vs alternatives, keeps smallest
