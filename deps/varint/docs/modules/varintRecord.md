# varintRecord: Schema-Driven Columnar Record Compression

## Overview

**varintRecord** compresses arrays of fixed-stride C records (structs) by describing their layout with a runtime field-descriptor table. Encode gathers each field across all records into a column and runs that column through **every compression strategy its kind supports**; strategies are measured against each other and the smallest wins, per column, with the winner recorded on the wire. Decode reverses the whole pipeline from the stream alone: **the schema travels inside the stream**, so the decoder needs zero external metadata.

The schema is a C array of descriptors where `offsetof`/`sizeof` capture the struct layout with compile-time correctness:

```c
typedef struct SensorReading {
    uint64_t timestampMs;
    uint32_t deviceId;
    int16_t temperature;
    uint8_t status;
    uint8_t mac[6];
    float voltage;
    varintDD precise;    /* 106-bit double-double */
} SensorReading;

static const varintRecordField schema[] = {
    VARINT_RECORD_FIELD(SensorReading, timestampMs, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(SensorReading, deviceId, VARINT_RECORD_U32),
    VARINT_RECORD_FIELD(SensorReading, temperature, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(SensorReading, status, VARINT_RECORD_U8),
    {offsetof(SensorReading, mac), 6, VARINT_RECORD_BYTES, 0},
    VARINT_RECORD_FIELD(SensorReading, voltage, VARINT_RECORD_F32),
    VARINT_RECORD_FIELD(SensorReading, precise, VARINT_RECORD_DD),
};

size_t n = varintRecordEncode(dst, rows, rowCount, sizeof(SensorReading),
                              schema, 7, 0, NULL);
```

## Strategy Lanes

Each column competes across the lanes its kind supports; the winner is tagged on the wire and validated against the kind at decode:

| Strategy    | What it does                                                                                                                                                                                                             | Kinds                    |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------ |
| `COMPETE`   | Normalized u64 column through `varintCompete`'s chunked per-block competition — tagged, delta, delta-of-delta, stride (with growth), FOR, PFOR, dict, RLE, BP128, BP128-delta, palette, palette-delta, Elias gamma/delta | all numeric + BOOL       |
| `XOR`       | XOR-with-previous transform, then `COMPETE` — consecutive similar floats cancel sign/exponent/high-mantissa bits, turning smooth float columns into small-integer columns                                                | F32, F64                 |
| `FLOAT`     | `varintFloat` lossless stream: sign/exponent/mantissa planes, with all three exponent-coding modes (independent, common, delta) measured                                                                                 | F32, F64                 |
| `PLANES`    | Byte-plane decomposition: byte position _p_ of every value becomes its own 0–255 column through `COMPETE` — captures per-position structure in opaque bytes and wide integers                                            | BYTES, numeric width ≥ 2 |
| `DD_STREAM` | `varintDDStream` lossless stream: gap-coded trailing limbs, XOR-chained leading limbs                                                                                                                                    | DD                       |
| `VERBATIM`  | Raw column bytes — the floor guaranteeing a column never expands beyond raw size plus tag overhead                                                                                                                       | all                      |

**Verified selection.** Lanes with float pipelines (`FLOAT`, `DD_STREAM`) are decoded and compared bit-for-bit against the source before they may win; if the round trip is not exact for this data (NaN payloads, subnormals, negative zeros), the lane is discarded and the next-best measured lane ships. Every emitted stream decodes to byte-identical records.

## Field Kinds

| Kind     | Width | Column treatment                                          |
| -------- | ----- | --------------------------------------------------------- |
| U8–U64   | 1–8   | bits per declared endianness                              |
| I8–I64   | 1–8   | sign-extended + ZigZag, so small negatives stay small     |
| F32/F64  | 4/8   | bit patterns (COMPETE/XOR/PLANES) or values (FLOAT)       |
| BOOL     | 1     | validated 0/1 at encode; palette's 1-bit floor + RLE runs |
| BYTES(n) | n ≥ 1 | opaque run; PLANES or VERBATIM                            |
| DD       | 16    | `varintDD {double hi; double lo}`; DD_STREAM or VERBATIM  |

Per-field `VARINT_RECORD_FLAG_BIG_ENDIAN` declares wire-format byte order, so network records round-trip bit-exactly on any host. Record bytes not covered by any field (padding, ignored regions) are not stored; decode zero-fills them.

## Stream Format

```
Offset  Size  Field
   0     4    Magic = 'V' 'R' 'E' 'C'
   4     1    Version (currently 1)
   5     1-9  recordCount (tagged varint)
   ?     1-9  recordSize  (tagged varint)
   ?     1-9  fieldCount  (tagged varint)
then fieldCount schema entries:
   ?     1    kind, then 1 byte flags
   ?     1-9  offset, then 1-9 size (tagged varints)
then fieldCount columns (omitted entirely when recordCount == 0):
   ?     1    strategy (validated against the field's kind)
   ?     1-9  columnBytes (tagged varint)
   ?     ?    payload
PLANES payloads hold size x [planeBytes:tagged][chunked-compete stream].
```

## Decode Safety

Structure is fully validated: `srcBytes`-bounded framing, magic/version, count/size overflow guards, the schema re-checked by encode's own rules (bounds, kind/width agreement, no overlap), each strategy checked against its kind, every column and plane length checked against the remaining bytes, and the caller's record stride required to equal the stream's. `DD_STREAM` payloads decode through `varintDDStream`'s bounded decoder; `FLOAT` payloads decode through a guard that validates the header fields against the encoder's real ranges and runs the decoder against a bounded scratch copy, requiring exact length consumption.

## API

```c
const char *varintRecordKindName(varintRecordFieldKind kind);
const char *varintRecordStrategyName(varintRecordStrategy strategy);

bool varintRecordSchemaValid(size_t recordSize, const varintRecordField *fields,
                             size_t fieldCount);
size_t varintRecordMaxEncodedSize(size_t recordCount, size_t recordSize,
                                  const varintRecordField *fields,
                                  size_t fieldCount);
size_t varintRecordEncode(uint8_t *dst, const void *records,
                          size_t recordCount, size_t recordSize,
                          const varintRecordField *fields, size_t fieldCount,
                          uint64_t codecMask, varintRecordMeta *meta);
size_t varintRecordReadHeader(const uint8_t *src, size_t srcBytes,
                              varintRecordHeader *header);
size_t varintRecordDecode(const uint8_t *src, size_t srcBytes, void *records,
                          size_t maxRecords, size_t recordSize,
                          size_t *decodedCount);

/* Reusable workspace: grow-only buffers across calls */
varintRecordCtx *varintRecordCtxNew(void);
void varintRecordCtxFree(varintRecordCtx *ctx);
size_t varintRecordEncodeWithCtx(varintRecordCtx *ctx, ...);
size_t varintRecordDecodeWithCtx(varintRecordCtx *ctx, ...);

/* Sharding: streams are self-delimiting */
size_t varintRecordStreamBytes(const uint8_t *src, size_t srcBytes);

/* Diagnostics: schema-driven paginated record printing */
size_t varintRecordPrintRecords(FILE *out, const void *records,
                                size_t recordCount, size_t recordSize,
                                const varintRecordField *fields,
                                size_t fieldCount,
                                const char *const *fieldNames,
                                size_t firstRecord, size_t maxRecords);
```

## Reusable Workspace

Encode and decode need working buffers proportional to the record count (staging columns, lane scratch, verification space, the chunked competition's scratch pair). The plain entry points allocate and release them per call; a `varintRecordCtx` owns them across calls in grow-only slots — a slot reallocates only when a call needs more than any earlier call did, so repeated same-shape calls run with **zero allocations** in steady state. One context serves encode and decode, any schemas and record counts, in any order; it holds no stream state, only memory (one context per thread). The sharding pattern is where it pays: `varintRecordBench`'s sharded section measures 1.17x on 4096-record telemetry shards, growing as shards shrink. The fuzzer drives one persistent context across every random shape while cross-checking the per-call path, so reuse and fresh-allocation behavior are proven byte-identical.

The chunked competition exposes the same discipline one layer down as a **typed scratch**: `varintCompeteChunkedScratch` binds client-owned memory (stack or heap) via `varintCompeteChunkedScratchInit`, which validates the size and stamps the geometry; every use re-audits the stamp, the capacity, and the block target, rejecting a mismatched scratch instead of trusting it.

## Sharding

Within a stream, every numeric column already adapts per block — the COMPETE/XOR/PLANES lanes ride `varintCompete`'s chunked format, so a column that changes character mid-stream gets different per-block winners. At the stream level, sharding is concatenation: encode shards of whatever record count fits the memory and parallelism budget, append the streams, and walk them with `varintRecordStreamBytes`, which finds each stream's end in O(fieldCount) by following the length prefixes without decoding any payload. Each shard is independently self-describing, so shards decode in parallel or stream one at a time, and different shards may even use different schemas.

## Record Printing

`varintRecordPrintRecords` renders any window of records (`firstRecord`, `maxRecords` — pagination for large datasets) as a table driven entirely by the schema: integers in decimal, floats as `%g`, BOOL as 0/1, BYTES as hex, DD through `varintDDToString` (~30 significant digits, so the 106-bit value prints as itself), with the same per-field endianness handling as the codec paths. `fieldNames` is an optional label array (NULL prints `f0..fN`), so one helper replaces per-struct debug printing everywhere a schema already exists.

`varintRecordMeta` reports per-column payload bytes **and the winning strategy**, index-aligned with the schema, so callers see exactly which fields dominate storage and how each was encoded. `codecMask` passes through to the COMPETE/XOR/PLANES lanes (0 = `VARINT_COMPETE_DEFAULT_MASK`).

## Measured Results

`varintRecordBench` (Apple M-series, 1 Mi records, median of 15):

| Shape     | Records                           | Ratio     | Decode    | Winning lanes                         |
| --------- | --------------------------------- | --------- | --------- | ------------------------------------- |
| telemetry | stride ts, jitter i32, enum, bool | 12.2x     | ~1.3 GB/s | COMPETE ×4                            |
| ticks     | clustered prices, sparse flags    | 5.8x      | ~0.7 GB/s | COMPETE ×4                            |
| floats    | smooth F64 + F32 curves           | 3.8x      | ~1.1 GB/s | PLANES + COMPETE/XOR (data-dependent) |
| ddcol     | double-double column              | 2.3x      | ~1.8 GB/s | DD_STREAM                             |
| tags      | structured 12-byte tags           | 24.8x     | ~1.1 GB/s | PLANES                                |
| constant  | identical records                 | ~110,000x | ~2.0 GB/s | COMPETE (stride)                      |
| noise     | incompressible                    | 1.13x     | ~1.4 GB/s | VERBATIM floor                        |

Field loads and stores are the record layer's own hot loop (once per record per column on gather and scatter); on little-endian hosts every power-of-two width is a single word move plus a bswap for declared-big-endian fields, worth 10–18% on decode versus byte-at-a-time assembly. The compression work itself lives in the delegated codecs, which carry their own SIMD (BP128, palette, stride scans, DD stream).

Encode throughput scales with how many lanes a column's kind runs (4 MB/s to 1.1 GB/s across the shapes above; multi-lane float columns are the most expensive). Encode stages every narrow field's value bits in one cache-blocked sweep over the records, and every lane — compete input, XOR transform, float values, verbatim bytes, plane bytes, and verification references — derives from that staging, so the record array streams once instead of once per lane per field. The FLOAT lane, like PLANES, runs only when cheaper lanes left more than 60% of raw on the table. The per-kind eval matrix in `varintRecordTest` enforces both minimum ratios and expected winning strategies for every kind, so a regression in any lane fails the build.

## When to Use

- **Use varintRecord** when you have arrays of fixed-stride structs — sensor packets, DB rows, tick data, log records — and want one call that compresses every field well without picking codecs or transforms per field.
- **Use varintCompete directly** when you already hold columnar `uint64_t` arrays.
- **Use varintFloat / varintDDStream directly** for standalone float or double-double arrays.
- **Use varintGroup** for encoding _individual_ records compactly (row-oriented, random access per record).
- For formats with header-dependent or conditional record layouts, parse into fixed-stride record arrays first, then hand those to varintRecord.

## Integration

- Test target: `varintRecordTest` (ctest `varint-record`) — includes the per-kind eval matrix
- Fuzz target: `varintRecordFuzz` (ctest `varint-record-fuzz`) — round trips, re-encode idempotency, truncation, header/schema/strategy corruption; run under the sanitizer harness for the memory-safety half
- Benchmark: `varintRecordBench` (not ctest-registered)
- Example: `examples/standalone/example_record.c` — three scenarios: IoT sensor fleet (all seven kinds + row-oriented comparison), wire-format market data (big-endian network fields), and game entity snapshots (small signed ints, flags, byte planes)

## See Also

- [varintCompete](varintCompete.md) — the competition layer behind COMPETE/XOR/PLANES
- `varintFloat.h` — the FLOAT lane's codec
- [varintDDStream](varintDDStream.md) — the DD_STREAM lane's codec
- [varintPalette](varintPalette.md), [varintBP128](varintBP128.md), [varintStride](varintStride.md), [varintElias](varintElias.md) — codecs columns commonly win with
