# varintCompete: Evidence-Based Codec Selection

## Overview

**varintCompete** is the self-managing layer above the individual codecs. Where `varintAdaptive` chooses a codec by single-pass heuristics and writes the result, `varintCompete` **runs** a configurable subset of codecs, measures the actual encoded bytes each produces, and emits the smallest — wrapped in a self-describing frame so the decoder can pick the matching codec automatically.

**Key Features:** Configurable codec mask, self-describing frame format with magic + version, per-call result struct exposing every candidate's size, opt-in atomic telemetry (`varintTelemetry`) for production tuning, full round-trip across DELTA/DELTA_DELTA/STRIDE/TAGGED on signed input and the additional FOR/PFOR/RLE/DICT for unsigned input.

## Key Characteristics

| Property       | Value                                                           |
| -------------- | --------------------------------------------------------------- |
| Implementation | Header (.h) + Compiled (.c)                                     |
| Frame Format   | `[VCMP magic:4][version:1][codecID:1][bodyLen:tagged][body]`    |
| Selection      | Smallest encoded byte count wins                                |
| Cost           | N × encode passes per call (N = enabled codecs)                 |
| Decoder        | Self-describing — no need to know the codec ahead of time       |
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

## Codec Mask

The codec mask is a `uint64_t` bitset over `varintCodecID` values (see `varintTelemetry.h`). Convenience masks:

```c
VARINT_COMPETE_DEFAULT_MASK  /* TAGGED, DELTA, DELTA_DELTA, STRIDE,
                                FOR, PFOR, DICT, RLE, BP128_DELTA */
VARINT_COMPETE_ALL_MASK      /* every known codec */
```

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
```

`varintCompeteResult` carries the winner ID, winner body size, frame size, and a `candidates[]` array recording every codec evaluated and its produced size (0 means the codec declined). Use this to learn which codecs are pulling weight on your data.

## Telemetry Integration

When compiled with `-DVARINT_TELEMETRY` (see `varintTelemetry.h`), every compete call updates atomic per-codec counters:

```c
#include "varintTelemetry.h"

varintTelemetryEntry snap[VARINT_CODEC_MAX];
varintTelemetrySnapshot(snap, VARINT_CODEC_MAX);
for (int i = 0; i < VARINT_CODEC_MAX; i++) {
    if (snap[i].calls) {
        printf("%s: %lu calls, %lu wins, %lu bytes\n",
               varintCodecName((varintCodecID)i),
               snap[i].calls, snap[i].wins, snap[i].bytesOut);
    }
}
```

Counters compile to zero-cost no-ops when `VARINT_TELEMETRY` isn't defined.

## When to Use

- Pick **varintAdaptive** when speed matters and a 1-pass heuristic is enough.
- Pick **varintCompete** when you want the best possible size on heterogeneous data, are encoding once / reading many, or you're tuning which codecs to include in your build.

## Integration

- Test target: `varintCompeteTest` (registered as ctest `varint-compete`)
- Frame format is intentionally distinct from `varintAdaptive`'s 1-byte codec header so the two layers don't conflict.

## See Also

- [varintAdaptive](../../src/varintAdaptive.h) — the heuristic counterpart
- [varintDeltaDelta](varintDeltaDelta.md), [varintStride](varintStride.md) — codecs new to this layer
- `varintTelemetry.h` — opt-in per-codec counters
