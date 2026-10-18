# varintDDStream: Double-Double Stream Compression

## Overview

**varintDDStream** compresses arrays of [`varintDD`](varintDD.md) — 106-bit double-double values — losslessly, or along a continuous precision ladder that runs all the way down to plain `double`.

The insight it trades on is structural rather than statistical. A normalized double-double satisfies `|lo| <= ulp(hi)/2`, so the trailing limb's 11-bit exponent field carries almost no information _given_ the leading limb. Storing the **gap** between the two exponents instead costs about 2.3 bits. And any value promoted from a plain `double` has a trailing limb of exactly zero, which costs one bit.

**Key Features**: gap-coded trailing limbs via Elias-gamma, presence bitmap for exactly-representable values, verbatim or XOR-chained leading limbs selected by measurement, one-integer precision policy, `srcBytes`-bounded decoding safe on untrusted input, 0.8-2.1 GB/s encode and 0.7-12 GB/s decode.

## Key Characteristics

| Property        | Value                                               |
| --------------- | --------------------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)                         |
| Input           | `varintDD` array (16 bytes/value raw)               |
| Output          | 2.4 to 15.1 bytes/value depending on shape          |
| Precision       | One knob: 52 bits (lossless) down to 0              |
| Random access   | None — the trailing-limb bitstream is sequential    |
| Allocation      | None, in either direction                           |
| Untrusted input | Safe — every read bounds-checked against `srcBytes` |
| SIMD Support    | AVX2/NEON deinterleave for the verbatim path        |

## Why the Trailing Limb Is Almost Free

Normalization guarantees `|lo| <= ulp(hi)/2`. Writing `E(x)` for the raw IEEE biased exponent field, that means:

```
E(lo) <= E(hi) - 53        for any normalized value
```

Storing `E(lo)` directly wastes 11 bits on information the leading limb already carries. This codec stores the gap:

```
g = E(hi) - E(lo) - 53     (>= 0 for any normalized value)
```

And `g` is not merely small — it is **geometrically distributed**. For values produced by arithmetic, `lo / ulp(hi)` is roughly uniform over `[0, 1/2]`, which puts `g` at 0 about half the time, 1 a quarter of the time, 2 an eighth, and so on. Expected Elias-gamma cost:

```
sum over j of  2^-(j+1) * gammaBits(j+1)  ~=  2.3 bits
```

versus 11 bits stored raw. Gamma coding is already in this library, in [varintElias](varintElias.md), and it is optimal for exactly this distribution.

The other half of the win needs no coding at all. A double-double promoted from a `double` has `lo == +0.0` exactly, as does any value whose arithmetic happened to come out exact. Those cost **one bitmap bit each and nothing else**, which is what takes an all-exact array from 16 bytes per value to 8.13.

### What this does not do

For fully generic double-double data the lossless win is small, and saying so plainly is more useful than a flattering benchmark: **a trailing mantissa is 52 bits of near-uniform entropy and nothing compresses it.** The gap coding removes the exponent field and the bitmap removes the exact values; what remains is the mantissa, and 15.02 bytes/value is the honest floor.

The real lever there is `loMantissaBits`, which trades precision for space along a continuous ladder rather than in one all-or-nothing step.

## Wire Format

```
[flags:1][count:tagged][hi stream][lo bitmap][lo bitstream]
```

| Field          | Contents                                                                                        |
| -------------- | ----------------------------------------------------------------------------------------------- |
| `flags`        | bits 0-1: leading-limb mode (0 = RAW, 1 = XOR); bits 2-7: trailing mantissa bits retained, 0-52 |
| `count`        | tagged varint (1-9 bytes)                                                                       |
| `hi stream`    | 8 bytes/value verbatim, or the XOR chain                                                        |
| `lo bitmap`    | `ceil(count/8)` bytes, bit set = trailing limb present                                          |
| `lo bitstream` | one self-delimiting entry per set bitmap bit                                                    |

**No length fields appear anywhere.** Each section is self-delimiting given the count: the verbatim leading-limb stream is exactly eight bytes per value, the XOR stream ends when the last value is decoded, the bitmap is one bit per value, and each bitstream entry carries its own width. That removes ~18 bytes of framing and, more importantly, removes the chance of a length field disagreeing with its payload.

**Leading limbs come first** because reconstructing a trailing limb needs its partner's exponent — the whole point of gap coding is that the exponent is not stored. Decoding therefore has to see `hi` before `lo`.

When `loMantissaBits == 0`, the bitmap and bitstream are omitted entirely and the result is a plain double stream.

### Trailing-limb entry

```
gamma(g + 1)                        g in [0, 62]: inline
  then  [sign:1][mantissa:k bits]

gamma(64)                           escape
  then  [raw trailing limb:64]
```

The escape covers everything the gap coding cannot express: denormal or non-finite trailing limbs, negative zero, gaps beyond 115, and hand-assembled pairs that never went through `varintDDNormalize`. It is rare on real data — zero escapes across the whole benchmark corpus — but it is what makes the codec lossless for _any_ input bit pattern, not merely for well-formed ones.

Mantissa truncation is **toward zero**, deliberately. Rounding could push `|lo|` past `ulp(hi)/2` and hand back a pair that is no longer a normalized double-double.

### Leading-limb modes

**RAW** — 8 bytes little-endian per value. The right answer whenever consecutive values are unrelated.

**XOR** — the Gorilla encoding, the same shape [varintDeltaDelta](varintDeltaDelta.md) applies to integers:

```
value 0:        64 raw bits
value i, x = bits[i] ^ bits[i-1]:
  x == 0:       [0]
  window fits:  [1][0][meaningful bits]
  new window:   [1][1][leading:6][meaningful-1:6][meaningful bits]
```

Consecutive samples of a smooth quantity share their sign, exponent, and most of their mantissa, so the window is usually a handful of bits.

**AUTO** — encode with XOR, keep it only if it actually came out smaller. The comparison is _exact, not estimated_: the leading-limb stream is written first and nothing has been written past it yet, so losing the bet costs one overwrite and no data movement.

## API Reference

### Modes and precision

```c
typedef enum varintDDStreamHiMode {
    VARINT_DD_STREAM_HI_RAW  = 0,
    VARINT_DD_STREAM_HI_XOR  = 1,
    VARINT_DD_STREAM_HI_AUTO = 2,
} varintDDStreamHiMode;

#define VARINT_DD_STREAM_LOSSLESS 52
#define VARINT_DD_STREAM_DROP_LO   0
```

### Sizing

```c
size_t varintDDStreamMaxSize(size_t count);
double varintDDStreamMaxRelativeError(uint8_t loMantissaBits);
```

`varintDDStreamMaxSize` returns 0 when the bound itself would wrap — silently wrapping would be worse than useless, since a caller doing exactly what this function is for would allocate a small block and hand it to an encoder expecting a large one.

### Encode / decode

```c
bool varintDDStreamAnalyze(const varintDD *values, size_t count,
                           varintDDStreamHiMode hiMode,
                           uint8_t loMantissaBits,
                           varintDDStreamMeta *meta);

size_t varintDDStreamEncode(uint8_t *dst, const varintDD *values, size_t count,
                            varintDDStreamHiMode hiMode,
                            uint8_t loMantissaBits,
                            varintDDStreamMeta *meta);

size_t varintDDStreamDecode(const uint8_t *src, size_t srcBytes,
                            varintDD *values, size_t maxCount);

size_t varintDDStreamGetCount(const uint8_t *src, size_t srcBytes);
```

### Metadata

```c
typedef struct varintDDStreamMeta {
    size_t count;            /* values encoded */
    size_t encodedSize;      /* total bytes */
    size_t hiBytes;          /* spent on leading limbs */
    size_t loBytes;          /* bitmap + bitstream */
    size_t exactValues;      /* trailing limb was exactly +0.0 */
    size_t escapedLimbs;     /* needed the raw fallback */
    double maxRelativeError; /* 0.0 when lossless */
    uint8_t hiMode;          /* mode actually used (AUTO resolved) */
    uint8_t loMantissaBits;
} varintDDStreamMeta;
```

Exactly 64 bytes — one cache line — asserted at compile time.

## Real-World Examples

### Example 1: Lossless round trip

```c
#include "varintDDStream.h"

uint8_t *buffer = malloc(varintDDStreamMaxSize(count));

varintDDStreamMeta meta;
const size_t written =
    varintDDStreamEncode(buffer, values, count,
                         VARINT_DD_STREAM_HI_AUTO,
                         VARINT_DD_STREAM_LOSSLESS, &meta);

const size_t got = varintDDStreamDecode(buffer, written, decoded, count);

/* Lossless means BIT-exact, not merely numerically equal: NaN payloads
 * and the sign of a zero survive too. memcmp is the right check, == is
 * not - it would happily accept a changed trailing limb. */
assert(got == count);
assert(memcmp(values, decoded, count * sizeof(varintDD)) == 0);
```

### Example 2: Measure before committing

```c
#include "varintDDStream.h"

/* Analyze runs the encoder's own measurement code without writing
 * anything, so its numbers are exact rather than estimates - and it
 * resolves AUTO, reporting which mode would actually be picked. */
varintDDStreamMeta plan;
const bool worthwhile =
    varintDDStreamAnalyze(values, count, VARINT_DD_STREAM_HI_AUTO,
                          VARINT_DD_STREAM_LOSSLESS, &plan);

printf("would be %zu bytes (%zu raw), %s mode, %zu exact limbs\n",
       plan.encodedSize, count * sizeof(varintDD),
       plan.hiMode == VARINT_DD_STREAM_HI_XOR ? "XOR" : "verbatim",
       plan.exactValues);

if (worthwhile) {
    /* encode - the size will match plan.encodedSize exactly */
}
```

### Example 3: A per-column precision policy

```c
#include "varintDDStream.h"

/* Not every column needs 106 bits. The policy is one integer, so it
 * lives in the schema rather than in the storage code. */
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

    c->byteCount = varintDDStreamEncode(c->bytes, values, rows,
                                        VARINT_DD_STREAM_HI_AUTO,
                                        c->trailingBits, NULL);
    return c->byteCount != 0;
}

column schema[] = {
    {"pressure",    VARINT_DD_STREAM_LOSSLESS, NULL, 0},  /* keep everything */
    {"ratio",       20,                        NULL, 0},  /* 22 digits is plenty */
    {"coarse_temp", VARINT_DD_STREAM_DROP_LO,  NULL, 0},  /* plain double */
};
```

See `examples/integration/dd_column_store.c` for the full version with exact aggregates.

### Example 4: Time-series archive, columnar

```c
#include "varintDDStream.h"

/* Columnar, not row-wise. The codec compresses a run of SIMILAR
 * values; interleaving x and y into one stream would alternate
 * between two unrelated sequences and defeat the leading-limb chain
 * entirely. This is the same reason column stores exist. */
const size_t sizeX = varintDDStreamEncode(bufferX, trajectoryX, samples,
                                          VARINT_DD_STREAM_HI_AUTO,
                                          VARINT_DD_STREAM_LOSSLESS, NULL);
const size_t sizeY = varintDDStreamEncode(bufferY, trajectoryY, samples,
                                          VARINT_DD_STREAM_HI_AUTO,
                                          VARINT_DD_STREAM_LOSSLESS, NULL);
```

### Example 5: Decoding data you did not create

```c
#include "varintDDStream.h"

/* The decoder is told how many bytes exist and never reads past them.
 * It returns 0 rather than a partial result, so there is no "how much
 * did I get" ambiguity to get wrong at the call site. */
const size_t got = varintDDStreamDecode(untrusted, untrustedBytes,
                                        values, capacity);

if (got == 0) {
    /* malformed, truncated, or larger than `capacity` - all one case */
    return handleBadInput();
}
```

Reading the header alone is cheap, which is useful for sizing an output buffer first:

```c
const size_t count = varintDDStreamGetCount(untrusted, untrustedBytes);
```

### Example 6: Verifying the precision you paid for

```c
#include "varintDDStream.h"

const size_t written = varintDDStreamEncode(buffer, values, count,
                                            VARINT_DD_STREAM_HI_AUTO,
                                            20, NULL);
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

printf("bound %.3e, observed %.3e\n", bound, worst);  /* observed <= bound */
```

## Performance Characteristics

Measured on an Apple M-series, 262,144 values, median of 15 runs, lossless.

### Compression by data shape

| Shape                                   | Bytes/value | Ratio     | Mode chosen |
| --------------------------------------- | ----------- | --------- | ----------- |
| Instrument data (exact + correlated)    | **2.41**    | **6.64x** | XOR         |
| Constant                                | 7.25        | 2.21x     | XOR         |
| Exactly representable, varied magnitude | 8.13        | 1.97x     | verbatim    |
| Smooth (full mantissas, correlated)     | 12.91       | 1.24x     | XOR         |
| Generic double-doubles                  | 15.02       | 1.07x     | verbatim    |

The two mechanisms are independent and the table separates them deliberately: the bitmap handles exactly-representable values, the XOR chain handles correlated neighbours, and generic data has neither.

### The precision ladder (generic data)

| Trailing bits | Bytes/value | Ratio | Max relative error |
| ------------- | ----------- | ----- | ------------------ |
| 52 (lossless) | 15.02       | 1.06x | 0                  |
| 40            | 13.56       | 1.18x | 1.01e-28           |
| 30            | 12.34       | 1.30x | 1.03e-25           |
| 20            | 11.12       | 1.44x | 1.06e-22           |
| 10            | 9.90        | 1.62x | 1.08e-19           |
| 0             | 8.00        | 2.00x | 1.11e-16           |

Every bit dropped costs one bit per non-exact value and doubles the error. Even at 20 retained bits the result carries 22 significant digits — six more than a `double` — for 26% fewer bytes. Pick the rung your tolerance allows, not the one that sounds safe.

### Throughput

| Shape    | Encode    | Decode     |
| -------- | --------- | ---------- |
| exact    | 1732 MB/s | 12906 MB/s |
| generic  | 776 MB/s  | 981 MB/s   |
| smooth   | 781 MB/s  | 725 MB/s   |
| sensor   | 2075 MB/s | 2048 MB/s  |
| constant | 1155 MB/s | 1017 MB/s  |

Rates are over the _source_ size (16 bytes/value).

### Time complexity

| Operation     | Complexity    |
| ------------- | ------------- |
| Analyze       | O(n)          |
| Encode        | O(n)          |
| Decode        | O(n)          |
| Random access | not supported |

## Implementation Notes

### Bit I/O is buffered, and that is not incidental

Bits accumulate MSB-first in a 64-bit register and leave in whole eight-byte groups. The ordering is not a coincidence: **packing bits MSB-first _is_ big-endian**, so a big-endian store of the accumulator produces byte for byte what a bit-at-a-time loop would have, while touching memory once per 64 bits instead of once per 8. That is worth about **2.8x on encode**.

It also fixes a real defect by construction. The byte-at-a-time version merged into each output byte, which preserved whatever the caller's buffer happened to contain — so the final partial byte of every bitstream was padded with **uninitialized heap memory**, the same input encoded to different bytes run to run, and the output carried data the caller never put there. The buffered writer only ever emits whole, fully-determined words, and the final partial word is written with its unused low bits zero.

The reader's fast path is a single big-endian eight-byte load. Because a field starts at most 7 bits into a byte, one 64-bit word covers any field of 57 bits or fewer whatever its alignment — every field this codec reads except the 64-bit verbatim escape. It is guarded on the **buffer** length, not the bit length, since the whole point of this reader is that it never touches a byte the caller did not hand it.

### This module carries its own bit reader

Not the one in [varintElias](varintElias.md), for one reason that matters: `varintBitReaderRead` only _asserts_ that it stays in bounds, so in a release build it will read past the end of a truncated buffer. A codec whose decode contract promises safety on untrusted input cannot be built on that. Every read here is checked and latches an overrun flag, so a malformed stream degrades to "returns 0" rather than reading memory it does not own.

### Analyze and Encode cannot disagree

The writer doubles as a measuring tape: give it a `NULL` buffer and it counts bits without storing any. `varintDDStreamAnalyze` reports exact sizes that way, running the _identical code_ the encoder runs rather than a parallel estimate that could drift out of agreement. The test suite asserts `Analyze().encodedSize == Encode()` for every corpus pattern, count, and mode.

### SIMD

The verbatim leading-limb path is a strided gather out of an array of structs, which scalar code does at one value per iteration. NEON deinterleaves with `vld2q_f64` / `vst2q_f64` in one instruction pair; AVX2 uses an unpack pair plus a lane permute. Build with `-DVARINT_DD_FORCE_SCALAR` for the portable path.

## Security Properties

**What is guaranteed:** memory safety on arbitrary input. Decoding never reads past `srcBytes`, never writes past `maxCount`, and returns 0 on anything malformed. The test suite copies every truncation prefix into an exactly-sized heap block so a sanitizer build catches an over-read of even one byte.

**What is not:** integrity. There is no checksum. A corrupted stream may decode to wrong values rather than being rejected — in testing, roughly 7% of single-byte corruptions were rejected outright and the rest decoded to garbage. **If you need to detect tampering, checksum the stream yourself.**

## When to Use varintDDStream

### Use when:

- Storing or transmitting **arrays of `varintDD`**
- Values are **exactly representable** as doubles (2x for free)
- Consecutive values are **correlated** — sensors, trajectories, sampled signals
- You want a **precision policy** rather than a precision decision
- Decoding **untrusted** input

### Don't use when:

- You need **random access** — decode is sequential
- Values are **generic double-doubles and you need lossless** — 1.07x may not justify the codec; consider whether the precision ladder applies instead
- Arrays are **tiny** — the header is ~10 bytes
- You need **integrity checking** — layer a checksum on top

## Implementation Details

### Source Files

- **Header**: `src/varintDDStream.h`
- **Implementation**: `src/varintDDStream.c`
- **Test entry**: `src/varintDDStreamTest.c`

### Dependencies

- `varintDD.h` — the value type and its normalization invariant
- `varintTagged.h` — the count field

### Testing

`src/varintDDStream.c` (test section, `VARINT_DD_STREAM_TEST`). Round-trip testing on well-behaved data proves only that the happy path works, so the corpus targets what the format has to survive:

| Pattern         | Targets                                            |
| --------------- | -------------------------------------------------- |
| `exact`         | all-zero trailing limbs, the bitmap path           |
| `generic`       | full trailing mantissas                            |
| `mixed`         | bitmap with both states                            |
| `negative-zero` | a bit pattern, not a value — the sign must survive |
| `denormal-lo`   | no usable exponent gap; must escape                |
| `nonfinite-hi`  | NaN and infinity in the leading limb               |
| `constant`      | the XOR chain's best case                          |
| `smooth`        | XOR windows                                        |
| `unnormalized`  | pairs that never went through normalize            |
| `random-bits`   | arbitrary bit patterns in both limbs               |
| `huge-gap`      | trailing limb far below the escape range           |

Suites: lossless round trip across every pattern, mode, and count (with a guard region after the output buffer); the precision ladder verified against its declared bound; malformed input — every truncation prefix in an exactly-sized heap block, plus random corruption; mode selection (AUTO never loses to the mode it chooses between); `Analyze` predicting `Encode` exactly.

`varintDDStreamFuzz` is a self-contained deterministic fuzzer that additionally asserts **encode idempotency** — re-encoding a decoded stream must reproduce it byte for byte, which is a much stronger statement than the values merely matching. That check is what caught the uninitialized-padding defect described above.

Registered as `varint-dd-stream`, `varint-dd-stream-scalar`, and `varint-dd-stream-fuzz` in ctest.

## See Also

- [varintDD](varintDD.md) — the value type being compressed
- [varintElias](varintElias.md) — the gamma code used for the exponent gap
- [varintDeltaDelta](varintDeltaDelta.md) — the same XOR/Gorilla pattern over integers
- [varintFloat](https://github.com/mattsta/varint/blob/main/src/varintFloat.h) — variable-precision compression for plain `double`
- [Architecture Overview](../ARCHITECTURE.md)
- `examples/standalone/example_ddstream.c` — annotated tour
- `examples/integration/dd_column_store.c` — per-column precision policy
