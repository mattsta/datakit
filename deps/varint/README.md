# varint: variable length integer storage

![CI Status](https://github.com/mattsta/varint/actions/workflows/ci.yml/badge.svg)
![Release](https://github.com/mattsta/varint/actions/workflows/release.yml/badge.svg)
![Nightly](https://github.com/mattsta/varint/actions/workflows/nightly.yml/badge.svg)
[![License](https://img.shields.io/badge/license-MIT%2FPublic%20Domain-blue.svg)](LICENSE)

## Background

Numbers in computers have limited native sizes: one byte, two bytes, four bytes, and eight bytes. The lack of three, five, six, and seven byte quantities can waste a lot of storage space if most of your data is small but you still need the ability to grow to large quantities on-demand. That's where varints come in.

Varints let you store and retrieve variable length integers in units of single byte widths instead of the standard fixed widths of 8, 16, 32, and 64 bits.

Each implementation of a varint has different ways of organizing the "variable" aspect determining the total length of any byte-reduced data representations.

The goal of varints is instead of limiting you to only 8, 16, 32, and 64-bit quantities, you can save/restore any of 8, 16, 24, 32, 40, 48, 56, 64 bit quantities.

Varints don't add any magic to your programming language or environment. Everything is accessed through a varint encode/decode API.

This package also includes packed integer encodings allowing you to store any bit-width integer across bytes if you need to optimize storage for, example, 8,000 integers but only with 14-bits each in a byte array.

The package also allows you to maintain sorted packed integer encodings if you want to maintain a large binary-search capable packed bit array to represent integer sets for easy [membership testing](https://github.com/mattsta/varint/blob/main/src/varintPacked.h#L412-L459) and [fast deletions](https://github.com/mattsta/varint/blob/main/src/varintPacked.h#L516-L531).

This package also includes packed matrix encodings so you can easily operate bit-packed matrix entries of arbitrary size.

## Example Usage

For more complete usage, see the code tests for varints in [`src/varintCompare.c`](https://github.com/mattsta/varint/blob/main/src/varintCompare.c) and code tests for things like packed bit matrices in [`src/varintDimension.c:varintDimensionTest()`](https://github.com/mattsta/varint/blob/main/src/varintDimension.c#L355-L544) and code tests for multi-word packed integer bit level encodings also in [`varintDimension.c`](https://github.com/mattsta/varint/blob/main/src/varintDimension.c#L252-L317).

### Tagged

```c
uint64_t x = 555557;
uint8_t z[9];

varintWidth length = varintTaggedPut64(z, x);
/* 'length' is 4 because the tagged varint used 4 bytes. */
uint64_t y;
varintTaggedGet64(z, &y);
assert(x == y);
```

### External

```c
uint64_t x = 555557;
uint8_t z[8];

varintWidth encoding = varintExternalPut(z, x);
/* 'encoding' is 3 because 'x' fits in 3 bytes */
uint64_t y = varintExternalGet(z, encoding);
assert(x == y);
```

For storage, all varints are evaluated as unsigned byte quantities. Any conversion to/from signed values are the responsibility of the caller.

## Storage Overview

| varint             | length stored in  | 1 byte max | 2 byte max | 3 byte max |    4 byte max |
| ------------------ | ----------------- | ---------: | ---------: | ---------: | ------------: |
| Tagged             | first byte        |        240 |      2,287 |     67,823 |    16,777,215 |
| Bijou (bijou64)    | first byte        |        247 |        503 |     66,039 |    16,843,255 |
| Split              | first byte        |         63 |     16,701 |     81,982 |    16,793,661 |
| Split Full         | first byte        |         63 |     16,446 |  4,276,284 |    20,987,964 |
| Split Full No Zero | first byte        |         64 |     16,447 |  4,276,285 |    20,987,965 |
| Split Full 16      | first byte        |          X |     16,383 |  4,210,686 | 1,077,952,509 |
| Chained            | final flag bit    |        127 |     16,383 |  2,097,151 |   268,435,455 |
| External           | first byte        |          X |        255 |     65,535 |    16,777,215 |
| External           | external metadata |        255 |     65,535 | 16,777,215 | 4,294,967,295 |

Note: Split and Split Full use two level split encodings where certain byte maximums
have two different encodings, so we count the maximum encoding at these boundary byte
positions for the highest numbers capable of being stored:

| varint             | level  | 1 byte max | 2 byte max | 3 byte max | 4 byte max |
| ------------------ | ------ | ---------: | ---------: | ---------: | ---------: |
| Split              | first  |         63 |     16,446 |          X |          X |
| Split              | second |          X |     16,701 |     81,982 | 16,793,661 |
| Split Full         | first  |         63 |     16,446 |  4,210,749 |          X |
| Split Full         | second |          X |          X |  4,276,284 | 20,987,964 |
| Split Full No Zero | first  |         64 |     16,447 |  4,210,750 |          X |
| Split Full No Zero | second |          X |          X |  4,276,285 | 20,987,965 |

## Code Guide

Varints are defined by how they track their size. Since varints have variable lengths, a varint must know how many bytes it contains.

We have **twenty-six types of varints** organized into three categories:

**Basic Encodings** (5 types): tagged, bijou, external, split, and chained. The chained type is the slowest and is not recommended for use in new systems.

**Advanced Encodings** (18 types): delta, delta-of-delta, stride, FOR (Frame-of-Reference), group, PFOR (Patched FOR), dictionary, bitmap, adaptive, compete, record (schema-driven columnar struct compression), float, double-double stream (106-bit values compressed via the normalization invariant), RLE (Run-Length Encoding), Elias (Gamma/Delta universal codes), BP128 (SIMD block-packed), palette (Huffman over top-16 values with verbatim-block outlier routing), and palette-of-deltas (palette coding over wrapped first differences for skewed gap alphabets). These provide 2-100x compression for specialized use cases like sorted data, regular-interval time series, arithmetic progressions, clustered values, repetitive data, floating point arrays, repeated values, small integers, and large sorted arrays. (The `compete` selector is supported by opt-in `varintTelemetry` per-codec counters.)

**Specialized Encodings** (3 types): packed (fixed-width bit arrays), dimension (matrix encoding), and bitstream (bit-level operations).

The goal of a varint isn't to store the _most_ data in the least space (which is impossible since the value here _includes_ metadata information which takes away from user storage space), but to allow you to let users store data without needing to pre-allocate everything as a 64-bit quantity up front.

### Tagged

Tagged varints hold their full width metadata in the first byte. The first byte also contributes to the stored value and can, by itself, also hold a user value up to 240. The maximum length of a 64-bit tagged varint is 9 bytes.

This is a varint format adapted from the abandoned sqlite4 project. Full encoding details are in [source comments](https://github.com/mattsta/varint/blob/main/src/varintTagged.c).

### Bijou (bijou64)

Bijou is a close cousin of Tagged: same tag-byte framing, big-endian payloads, `memcmp`-sortable, and length known from the first byte. It is a faithful port of [bijou64](https://github.com/inkandswitch/subduction/blob/main/bijou64/SPEC.md) (BIJective Offset U64) by Brooklyn Zelenka / Ink & Switch. Values 0–247 store as a single byte; tags `0xF8`–`0xFF` introduce 1–8 big-endian payload bytes holding `value − OFFSET[tier]`.

The difference from Tagged is what the offsets buy: bijou applies a per-tier offset on **every** tier, so it is **canonical by construction** — every value has exactly one encoding and no "overlong" byte sequence can decode to a value that has a shorter form. Tagged (and the sqlite4 varint it derives from) only offsets the first two multi-byte tiers, reusing the spare tag values 241–248 for extra 2-byte capacity instead; this makes Tagged noticeably more compact in the 248–2,287 range but means tiers 3+ admit overlong encodings. Choose bijou when canonicality matters (content-addressed storage, hashing, dedup); choose Tagged when you want the smallest bytes for small values. Full encoding details are in [source comments](https://github.com/mattsta/varint/blob/main/src/varintBijou.c) and the upstream [SPEC](https://github.com/inkandswitch/subduction/blob/main/bijou64/SPEC.md).

### Split

Split varints hold their full width metadata in the first byte. The first byte can hold a user value up to 63. The maximum length of a 64-bit split varint is 9 bytes.

This varint uses split "levels" for storing integers. The first level is one byte prefixed with bits `00` then stores 6 bits of user data (up to number 63). The second level is two bytes, with the first byte prefixed with bits `01` plus the following full byte, and stores 14 bits of user data (16383 + the previous level of 63 = 16446). After the second level, we reserve the first byte only for type data and use an external varint for the following bytes. The third level is between 2 and 9 bytes wide. The third level can store up to value 16701 in two bytes, and up to 81981 in three bytes, all the way up to a full 9 bytes to store a 64 bit value (8 bytes value + 1 byte type metadata).

### External

External varints keep their full width metadata external to the varint encoding itself. You have to track the length of your varint either by manually prefixing your varint with a byte describing the length of the following varint or by implicitly knowing the type by other means. The maximum length of a 64-bit external varint is 8 bytes since your are maintaining the type data external to the varint encoded data.

External varints are equivalent to just splitting a regular 64-bit quantity into single byte slices.

External varints just save the bytes containing data with no internal overhead. So, if you store `3` inside of a uint64_t, it won't use 8 bytes, it will use one byte. Since the external encoding doesn't pollute the bytes with encoding metadata, you can even cast system-width external varints (8, 16, 32, 64) to native types without further decoding (little-endian only).

External varints do store the most data in the least space possible, but you must maintain the type/length of the stored varint external to the varint itself for later retrieval.

### Chained

Chained varints don't know the full type/length of their data until they traverse the entire varint and reach an "end of varint" bit, so they are the slowest variety of varint. Each byte of a chained varint has a "continuation" bit signaling if the end of the varint has been reached yet. The maximum length of a 64-bit chained varint is 9 bytes.

This is the most common legacy varint format and is used in sqlite3, leveldb, and many other places. Full encoding details are in source comments for the [sqlite3 derived version](https://github.com/mattsta/varint/blob/main/src/varintChained.c) and for the [leveldb derived (and herein optimized further) version](https://github.com/mattsta/varint/blob/main/src/varintChainedSimple.c).

### Packed Bit Arrays

Also includes support for arrays of fixed-bit-length packed integers in `varintPacked.c` as well as reading and writing
packed bit arrays into matrices in `varintDimension.c`.

### Delta Encoding (varintDelta)

Delta encoding stores a base value followed by ZigZag-encoded deltas. Ideal for sorted/sequential data like timestamps, IDs, and sensor readings. Achieves 70-90% compression on monotonic sequences. Full details in [varintDelta.h](https://github.com/mattsta/varint/blob/main/src/varintDelta.h).

### Frame-of-Reference (varintFOR)

FOR encoding stores a minimum value and all others as fixed-width offsets. Extremely efficient for clustered values (timestamps in narrow windows, IDs in contiguous ranges). Achieves 2-7.5x compression with SIMD-friendly layout. Full details in [varintFOR.h](https://github.com/mattsta/varint/blob/main/src/varintFOR.h).

### Group Encoding (varintGroup)

Group encoding shares metadata across multiple related values using a compact bitmap. Reduces per-field overhead by 30-40% for struct-like data (database rows, network packets, multi-field records). Full details in [varintGroup.h](https://github.com/mattsta/varint/blob/main/src/varintGroup.h).

### Patched Frame-of-Reference (varintPFOR)

PFOR extends FOR with exception handling for outliers. Optimal for mostly-clustered data with rare spikes (stock prices, network latency). Achieves 57-83% compression while handling outliers efficiently. Full details in [varintPFOR.h](https://github.com/mattsta/varint/blob/main/src/varintPFOR.h).

### Dictionary Encoding (varintDict)

Dictionary encoding maps repetitive values to compact indices. Achieves 83-87% compression (up to 8x) for highly repetitive data like log sources, status codes, and enum values. Uses binary search for O(log n) lookups. Full details in [varintDict.h](https://github.com/mattsta/varint/blob/main/src/varintDict.h).

### Bitmap Encoding (varintBitmap)

Hybrid dense/sparse encoding (Roaring-style) with automatic container adaptation. Uses ARRAY for sparse sets, BITMAP for dense regions, and RUNS for contiguous ranges. Supports set operations (AND, OR, XOR). Ideal for inverted indexes and boolean arrays. Full details in [varintBitmap.h](https://github.com/mattsta/varint/blob/main/src/varintBitmap.h).

### Adaptive Encoding (varintAdaptive)

Intelligent encoding selector that automatically analyzes data characteristics and chooses the optimal encoding strategy (DELTA, FOR, PFOR, DICT, BITMAP, or TAGGED). Achieves 1.35x-6.45x compression automatically without manual encoding selection. Self-describing format with 1-byte header. Ideal for mixed workloads, log compression, and API responses. Full details in [varintAdaptive.h](https://github.com/mattsta/varint/blob/main/src/varintAdaptive.h).

### Floating Point Compression (varintFloat)

Variable-precision floating point compression with configurable precision modes (FULL/HIGH/MEDIUM/LOW) and encoding strategies (INDEPENDENT/COMMON_EXPONENT/DELTA_EXPONENT). Achieves 1.5x-4.0x compression for sensor data, scientific measurements, and GPS coordinates while maintaining specified accuracy bounds. FULL mode is lossless. Full details in [varintFloat.h](https://github.com/mattsta/varint/blob/main/src/varintFloat.h).

### Double-Double Arithmetic (varintDD)

106-bit floating point built from two IEEE doubles held as an unevaluated sum, giving ~31 decimal digits without leaving the FPU. Provides the error-free transformations (`twoSum`, `fastTwoSum`, `twoProduct` via hardware FMA, with Dekker splitting as a fallback), the full arithmetic set (add/sub/mul/div/sqrt), exact `int64` conversion, and vectorized array operations with NEON, AVX2, and portable scalar backends. On an M-series machine a dependent chain costs **11.6x** a `double` add, **4.9x** a multiply, and **15.0x** a divide — against ~20-50x for soft-float `__float128` at similar width.

The compensated reductions (`varintDDSumDoubles`, `varintDDDotDoubles`) are the standout: they run at **~0.6x the cost of a naive `double` summation loop** — _faster than the thing they replace_ — because splitting the accumulator across SIMD lanes breaks the serial dependency that limits `acc += x`, which more than pays for the compensation arithmetic. This is also the one place hand-written vector code is irreplaceable: a compiler may not reassociate a floating-point reduction, so it cannot vectorize this on its own. Building the same source with `-DVARINT_DD_FORCE_SCALAR` costs 1.7x naive instead of 0.6x, a **~2.9x** swing. For _elementwise_ operations the opposite holds — clang already emits the identical `ld2.2d`/`fadd.2d` sequence for a plain loop, and the explicit backend adds nothing beyond making it guaranteed rather than hoped for.

Correctness is not checked against the same arithmetic it is testing: a separate 1536-bit integer oracle evaluates every sum and product exactly, confirming the transformations are bit-exact over 200k random cases and that add/multiply land at ~2^-105 relative error. `varintDDSelfCheck()` re-verifies at runtime that the transformations survived compilation, catching the `-ffast-math` class of breakage that silently collapses the extra precision to zero. Full details in [varintDD.h](https://github.com/mattsta/varint/blob/main/src/varintDD.h) and [module documentation](docs/modules/varintDD.md).

### Double-Double Stream Compression (varintDDStream)

Compresses arrays of `varintDD`. A normalized double-double satisfies `|lo| <= ulp(hi)/2`, so the trailing limb's 11-bit exponent field carries almost no information given the leading limb; the codec stores the **gap** `g = E(hi) - E(lo) - 53` instead, which is geometrically distributed (`g = 0` about half the time) and costs ~2.3 bits under Elias-gamma rather than 11. Trailing limbs that are exactly zero — every value promoted from a plain `double` — cost one bitmap bit and nothing else. Leading limbs are stored verbatim or XOR-chained (Gorilla), selected by measurement rather than estimate.

Measured lossless, 262k values: **8.13 bytes/value (1.97x)** for exactly-representable values, **2.41 (6.64x)** for instrument-style data where both mechanisms fire, **7.25 (2.21x)** constant, and **15.02 (1.07x)** for fully generic double-doubles — that last number is the honest floor, because a full trailing mantissa is 52 bits of incompressible entropy. The real lever there is the precision ladder: one knob from 106-bit lossless down to plain `double`, in ~1-bit steps (20 retained bits → 11.12 bytes/value at 1.1e-22 relative error). Encode 0.77-2.1 GB/s, decode 0.7-12 GB/s, with bit I/O buffered through a 64-bit accumulator (MSB-first packing is big-endian, so a word store reproduces the bit-at-a-time result while touching memory an eighth as often). The `srcBytes`-bounded decoder fully validates untrusted input — it carries its own bounds-checked bit reader rather than the one in `varintElias.h`, which only asserts — and is hardened by a deterministic fuzzer (`varintDDStreamFuzz`) that also asserts encode idempotency. Full details in [varintDDStream.h](https://github.com/mattsta/varint/blob/main/src/varintDDStream.h) and [module documentation](docs/modules/varintDDStream.md).

### Run-Length Encoding (varintRLE)

Run-length encoding compresses sequences with consecutive repeated values by storing (length, value) pairs using varintTagged. Achieves 8-100x compression for data with long runs (sensor readings, audio silence, sparse matrices). Supports random access via `varintRLEGetAt()`, run analysis, and both header-prefixed and raw formats. Ideal for database column compression, game state serialization, and log aggregation. Full details in [varintRLE.h](https://github.com/mattsta/varint/blob/main/src/varintRLE.h) and [module documentation](docs/modules/varintRLE.md).

### Elias Universal Codes (varintElias)

Elias Gamma and Delta universal codes provide bit-level compression optimal for geometric distributions. Gamma codes are optimal for very small integers (1-7), while Delta codes are better for medium integers (8-1000). Self-delimiting prefix-free codes require no delimiters. Perfect for Huffman code lengths, tree depths, gap encoding, and succinct data structures. Achieves 50-80% compression for small positive integers. Full details in [varintElias.h](https://github.com/mattsta/varint/blob/main/src/varintElias.h) and [module documentation](docs/modules/varintElias.md).

### SIMD Block-Packed Encoding (varintBP128)

Binary Packing with 128-value blocks optimized for SIMD processing (AVX2/NEON). Packs integers using minimum bit-width per block with optional delta encoding for sorted sequences. Achieves 2-8x compression for sorted/clustered data with 800+ MB/s encoding and 1.2+ GB/s decoding throughput. Ideal for search engine posting lists, time series timestamps, graph adjacency lists, and column store databases. Full details in [varintBP128.h](https://github.com/mattsta/varint/blob/main/src/varintBP128.h) and [module documentation](docs/modules/varintBP128.md).

### Second-Order Delta (varintDeltaDelta)

Delta-of-delta encoding stores the _difference between successive deltas_, so data arriving at a steady cadence (timestamps every 60 s, sensors at a fixed sample rate, monotonic counters) collapses to a near-zero second-order delta that packs into a 2-byte width-tagged record per element. This is the Gorilla time-series pattern, promoted to a first-class module with the same `Encode/Decode/Analyze` shape as the other codecs. Achieves ~3-4x compression on regular-interval series. Full details in [varintDeltaDelta.h](https://github.com/mattsta/varint/blob/main/src/varintDeltaDelta.h) and [module documentation](docs/modules/varintDeltaDelta.md).

### Arithmetic-Progression Detection (varintStride)

Stride encoding detects arithmetic progressions — sequences with a constant difference — and stores them in a constant-size record (~24 bytes) regardless of length. An **exact** mode requires every delta to match (with O(1) random access); a **fuzzy** mode tolerates up to 20% outliers, stored as `(index, value)` patches. SIMD-accelerated mismatch detection (NEON/AVX2). Ideal for paginated IDs, page offsets, fixed-grid coordinates, and polling intervals. Full details in [varintStride.h](https://github.com/mattsta/varint/blob/main/src/varintStride.h) and [module documentation](docs/modules/varintStride.md).

### Palette Huffman Encoding (varintPalette)

Palette encoding entropy-codes streams dominated by a small value set: the ≤16 most frequent values become a palette coded with canonical Huffman (skewed distributions compress toward their entropy, down to 1 bit/value), while any 64-value block containing an out-of-palette outlier is routed to verbatim storage via a bitmask — the decode hot path stays branchless (flat terminal tables decoding up to two symbols per lookup, no escape codes). Per-block bit offsets let the decoder run four independent bit cursors in lockstep, reaching ~1 Gval/s decode on skewed data (Apple M-series); the srcBytes-bounded decoder fully validates untrusted input and is hardened by a self-contained deterministic fuzzer (`varintPaletteFuzz`). Design adapted from Cloudflare's Unweight report (Cf-TR-2026.04) on lossless LLM weight compression. Ideal for status/enum columns, quantized readings, and mostly-clean data with rare wild outliers. Full details in [varintPalette.h](https://github.com/mattsta/varint/blob/main/src/varintPalette.h) and [module documentation](docs/modules/varintPalette.md).

### Evidence-Based Selection (varintCompete + varintTelemetry)

Where `varintAdaptive` picks a codec by a single-pass heuristic, `varintCompete` _runs_ a configurable subset of codecs, measures the actual encoded size each produces, and emits the smallest inside a self-describing frame (`[VCMP magic][version][codec ID][bodyLen][body]`) so the decoder selects the matching codec automatically. A chunked mode (`varintCompeteEncodeChunked*`) runs the competition independently per block (default 4096 values) so heterogeneous streams get a different winner per region: chunked streams carry their own counts (the decoder needs no external metadata), blocks forming one exact arithmetic progression are grown into a single ~30-byte stride record no matter how long, and a sampling probe (`varintCompetePruneMask`) skips codecs that cannot plausibly win a block before spending encode CPU on them. The companion `varintTelemetry` adds opt-in, lock-free atomic per-codec counters (calls/wins/bytes/wall-clock ns) that compile to zero-cost no-ops unless `-DVARINT_TELEMETRY` is set — use it to learn which codecs actually win, and what they cost, on your data. Full details in [module documentation](docs/modules/varintCompete.md).

### Schema-Driven Record Compression (varintRecord)

Where every other codec starts from a `uint64_t[]` column, `varintRecord` starts from **your structs**. A field-descriptor table (one `VARINT_RECORD_FIELD(struct, member, kind)` line per field, layout captured at compile time by `offsetof`/`sizeof`) describes a fixed-stride record over the full kind set — unsigned/signed integers, `F32`/`F64`, `BOOL`, opaque `BYTES`, and 106-bit `DD` double-doubles. Encode gathers each field into a column and runs it through **every strategy lane its kind supports**, measured, smallest wins per column: the chunked `varintCompete` competition (14 codecs) for integers, XOR-with-previous and `varintFloat` sign/exponent/mantissa streams for floats, byte-plane decomposition for opaque bytes and wide integers, `varintDDStream` for double-doubles, and a verbatim floor that guarantees no column ever expands past raw. Float-pipeline lanes are verified bit-exact by decode before they may win, so every stream round-trips byte-identically — NaN payloads, subnormals, and negative zeros included. The schema travels inside the stream (`VREC` frame), so decode needs zero external metadata, re-validates the schema and per-column strategies by encode's own rules, and bounds-checks all structure — exercised by a per-kind eval matrix (ratio + strategy assertions for every kind) and a deterministic fuzzer (`varintRecordFuzz`). Measured: 12.2x on telemetry rows, 3.8x on float sensor curves, 24.8x on structured tags via byte planes, 2.3x on double-double columns, with a verbatim floor on incompressible data. Full details in [varintRecord.h](https://github.com/mattsta/varint/blob/main/src/varintRecord.h) and [module documentation](docs/modules/varintRecord.md).

## Comprehensive Examples

The `examples/` directory contains **44 production-quality examples** demonstrating real-world applications:

### **17 Standalone Examples** - Individual module demonstrations

- **Basic encodings**: Tagged, External, Split, Chained, Packed, Dimension, Bitstream
- **Advanced encodings**: Delta, FOR, Group, PFOR, Dictionary, Bitmap, Adaptive, Float
- **Run-Length Encoding** (RLE) with varint lengths (11x-2560x compression)
- **Delta-of-Delta** (`varintDeltaDelta`) for regular-interval time series (Gorilla pattern, first-class module)
- **Stride** (`varintStride`) for arithmetic progressions: exact mode ~24 bytes any length, fuzzy mode tolerates outliers
- **Compete + Telemetry**: evidence-based codec selection runner with opt-in atomic per-codec wins/calls counters
- **Record** (`varintRecord`): schema-driven columnar struct compression — describe a struct once, every field competes for its best codec (11.2x on telemetry rows)

### **9 Integration Examples** - Combining multiple varint types

- Database systems, Network protocols, Game engines, Sensor networks
- **Vector Clocks** for distributed systems (923x compression for sparse clocks)
- **Delta-of-Delta Compression** (Gorilla-style, 7.6-7.9x compression)
- **Sparse Matrix CSR** for scientific computing (77.67x compression)

### **15 Advanced Examples** - Production-ready real-world systems

- Blockchain ledgers, DNS servers, Search engines, Financial systems
- **Bloom Filters** for probabilistic membership (2.5M+ ops/sec)
- **Autocomplete Tries** for typeahead search (500K queries/sec)
- **Point Cloud Octrees** for 3D spatial data (sub-ms queries)
- Full trie pattern matching (2391x faster than naive)
- Game replay systems, Bytecode VMs, Log aggregation

### **3 Reference Examples** - Complete implementations

- Key-value store, Time-series database, Graph database

**All examples include:**

- ✅ Comprehensive benchmarks and performance analysis
- ✅ Memory safety validation (AddressSanitizer + UndefinedBehaviorSanitizer)
- ✅ Real-world compression ratios and use cases
- ✅ Production-quality code with proper error handling

See [`examples/README.md`](examples/README.md) for full catalog and learning paths.

## Building

    mkdir build
    cmake ..
    make -j12

## Testing

### Quick Start

```bash
# Run all unit tests via CMake
make test

# Run comprehensive test suite with sanitizers
make varint-test-comprehensive

# Check for compiler warnings
make varint-check-warnings
```

### Manual Testing

**Unit Tests:**

```bash
./scripts/test/run_all_tests.sh both     # All unit tests with ASan+UBSan
./scripts/test/run_unit_tests.sh         # Quick unit test runner
```

**Example Tests:**

```bash
./scripts/test/test_all_comprehensive.sh  # All 43 examples with sanitizers
```

**Compiler Checks:**

```bash
./scripts/build/run_all_compilers.sh      # GCC + Clang warning verification
```

**Performance Benchmarks:**

```bash
./build/src/varint-compare
./build/src/varintDimensionTest
./build/src/varintPackedTest 3000
./build/src/varintCompeteBench     # compete-layer SIMD kernels + chunked codec throughput
./build/src/varintPaletteBench
./build/src/varintDDBench
```

See [`scripts/README.md`](scripts/README.md) for complete testing documentation.

## Repository Structure

```
varint/
├── src/                        # Core library implementation
│   ├── varint*.h               # Header files for all varint encodings
│   ├── varint*.c               # Implementation files
│   ├── varint*Test.c           # Unit tests (8 test suites)
│   ├── varintCompare.c         # Performance benchmarking tool
│   ├── ctest.h                 # Testing framework
│   ├── perf.h                  # Performance measurement utilities
│   └── CMakeLists.txt          # Source build configuration
│
├── examples/                   # 43 production-quality examples
│   ├── standalone/             # 16 individual module demonstrations
│   │   ├── example_tagged.c    # Tagged varint encoding
│   │   ├── example_external.c  # External varint encoding
│   │   ├── example_delta.c     # Delta encoding for sorted data
│   │   ├── example_for.c       # Frame-of-Reference encoding
│   │   ├── example_adaptive.c  # Adaptive encoding selection
│   │   └── ...                 # 11 more encoding examples
│   ├── integration/            # 9 multi-encoding system examples
│   │   ├── database_system.c   # Database row compression
│   │   ├── network_protocol.c  # Network packet encoding
│   │   ├── sensor_network.c    # IoT sensor data compression
│   │   ├── vector_clock.c      # Distributed system timestamps
│   │   └── ...                 # 5 more integration examples
│   ├── advanced/               # 15 production-ready real-world systems
│   │   ├── trie_server.c       # Async autocomplete server (epoll)
│   │   ├── trie_client.c       # Autocomplete client
│   │   ├── bloom_filter.c      # Probabilistic membership testing
│   │   ├── dns_server.c        # DNS caching server
│   │   ├── search_engine.c     # Inverted index search
│   │   └── ...                 # 10 more advanced examples
│   ├── reference/              # 3 complete reference implementations
│   │   ├── kv_store.c          # Key-value store
│   │   ├── timeseries_db.c     # Time-series database
│   │   └── graph_db.c          # Graph database
│   ├── README.md               # Complete example catalog
│   └── CMakeLists.txt          # Example build configuration
│
├── scripts/                    # Automation and testing scripts
│   ├── build/                  # Build and compiler verification
│   │   ├── run_all_compilers.sh    # GCC + Clang warning checks
│   │   └── check_warnings.sh       # Single compiler checker
│   ├── test/                   # Test execution scripts
│   │   ├── run_all_tests.sh            # Unit tests with sanitizers
│   │   ├── run_unit_tests.sh           # Quick unit test runner
│   │   ├── test_all_comprehensive.sh   # All 43 examples with ASan+UBSan
│   │   ├── test_all_examples_sanitizers.sh  # CMake-based example testing
│   │   ├── test_trie_comprehensive.sh  # Full trie system validation
│   │   ├── test_trie_fast.sh           # Quick trie tests
│   │   ├── test_trie_memory_safety.sh  # Valgrind memory analysis
│   │   ├── test_trie_sanitizers.sh     # Trie with ASan+UBSan
│   │   └── test_trie_server.sh         # Basic trie functionality
│   └── README.md               # Complete script documentation
│
├── docs/                             # Comprehensive documentation
│   ├── ARCHITECTURE.md               # System architecture and design
│   ├── CHOOSING_VARINTS.md           # Decision guide for encoding selection
│   ├── ENCODING_ANALYSIS.md          # Deep-dive encoding analysis
│   ├── QUICK_REFERENCE.md            # API quick reference
│   ├── BUILD_AND_TEST.md             # Build system and testing guide
│   ├── modules/                      # Per-module detailed documentation
│   │   ├── varintTagged.md           # Tagged encoding guide
│   │   ├── varintExternal.md         # External encoding guide
│   │   ├── varintSplit.md            # Split encoding guide
│   │   ├── varintChained.md          # Chained encoding guide
│   │   ├── varintPacked.md           # Packed arrays guide
│   │   ├── varintDimension.md        # Matrix encoding guide
│   │   ├── varintBitstream.md        # Bitstream operations guide
│   │   ├── varintRLE.md              # Run-Length Encoding guide
│   │   ├── varintElias.md            # Elias Gamma/Delta codes guide
│   │   └── varintBP128.md            # SIMD block-packed encoding guide
│   └── struct-optimization-guide.md  # Memory layout optimization
│
├── tools/                      # Development and analysis tools
│   ├── struct_analyzer.c       # Struct layout analyzer
│   ├── struct_audit.c          # Struct padding audit tool
│   ├── struct_pahole_analyzer.sh  # pahole-based analysis
│   ├── struct_size_check.c     # Size verification tool
│   └── README.md               # Tool documentation
│
├── util/                       # Utility scripts
│   └── dimensionPairMap.py     # Dimension mapping generator
│
├── .github/                    # GitHub configuration
│   ├── workflows/              # CI/CD pipelines
│   │   ├── ci.yml              # Continuous integration
│   │   ├── release.yml         # Release verification
│   │   └── nightly.yml         # Nightly comprehensive testing
│   ├── CONTRIBUTING.md         # Contribution guidelines
│   └── CI_CD_DOCUMENTATION.md  # CI/CD documentation
│
├── CMakeLists.txt              # Root build configuration
├── README.md                   # This file
├── LICENSE                     # MIT/Public Domain dual-license
└── .gitignore                  # Git ignore patterns
```

### Directory Purpose

- **`src/`** - Core varint library with 15 encoding types, unit tests, and benchmarking
- **`examples/`** - 43 complete, production-quality examples demonstrating real-world usage
- **`scripts/`** - Build automation, testing, and quality assurance scripts
- **`docs/`** - Comprehensive documentation including architecture, guides, and module references
- **`tools/`** - Development tools for struct analysis and optimization
- **`.github/`** - CI/CD workflows and contribution guidelines

### Key Features

- **Zero dependencies** - Pure C11 implementation
- **Extensively tested** - 8 unit test suites + 43 example programs with sanitizers
- **Production-ready** - All examples include benchmarks and memory safety validation
- **Well-documented** - 2,900+ lines of documentation across architecture, guides, and API references
- **Cross-platform** - Tested on Linux (Ubuntu) and macOS with GCC and Clang
- **Quality-assured** - Strict compiler warnings, ASan, UBSan, and Valgrind validation

## License

Many of the functions and routines here are just bit manipulations which aren't uniquely license-able. Otherwise, `varintTagged` is adapted from sqlite4 and `varintChained` is adapted from sqlite3 and `varintChainedSimple` is adapted from leveldb and `varintSplit` metadata layouts were inspired by legacy redis ziplist bit-type management circa 2015 (but code here is a new non-derived implementation and is uniquely expanded over the original ideas). All other implementation details are made available under Apache-2.0 for features such as common bit manipulations and novel implementations of routine data manipulation patterns.
