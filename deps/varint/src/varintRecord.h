#pragma once

#include "varint.h"
#include "varintCompete.h"

#include <stddef.h>

__BEGIN_DECLS

/* ====================================================================
 * varintRecord — Schema-Driven Columnar Record Compression
 * ====================================================================
 * Compresses arrays of fixed-stride C records (structs) by describing
 * their layout with a runtime field-descriptor table. Encode gathers
 * each field across all records into a column and hands that column to
 * every compression strategy its kind supports; the strategies are
 * measured against each other and the smallest wins, per column, with
 * the winner recorded on the wire. The strategy lanes cover the whole
 * framework:
 *
 *   COMPETE   normalized u64 column through varintCompete's chunked
 *             per-block competition (tagged, delta, delta-of-delta,
 *             stride, FOR, PFOR, dict, RLE, BP128, BP128-delta,
 *             palette, palette-delta, Elias gamma/delta)
 *   XOR       XOR-with-previous transform, then COMPETE — consecutive
 *             similar floats cancel sign/exponent/high-mantissa bits,
 *             so smooth float columns become small-integer columns
 *   FLOAT     varintFloat lossless stream (sign/exponent/mantissa
 *             planes with three exponent-coding modes, all measured)
 *   PLANES    byte-plane decomposition: byte position p of every value
 *             becomes its own 0-255 column through COMPETE — captures
 *             per-position structure in opaque bytes and wide integers
 *   DD_STREAM varintDDStream lossless stream for 106-bit double-double
 *             fields (gap-coded trailing limbs, XOR-chained leading)
 *   VERBATIM  raw column bytes — the floor that guarantees a column
 *             never expands beyond its raw size plus tag overhead
 *
 * Lossy-risk lanes (FLOAT, DD_STREAM) are verified by decode before
 * they are allowed to win: if the round trip is not bit-exact for this
 * data, the lane is discarded and the next-best measured lane ships.
 * Every emitted stream therefore decodes to byte-identical records.
 *
 * The schema travels inside the stream: the decoder reconstructs the
 * records from the bytes alone, with no external metadata. Schemas are
 * plain C descriptor arrays — offsetof/sizeof capture the struct layout
 * with compile-time correctness.
 *
 * Stream format (all multi-byte scalars are tagged varints):
 *   [magic:4 = 'V' 'R' 'E' 'C'][version:1]
 *   [recordCount][recordSize][fieldCount]
 *   fieldCount x [kind:1][flags:1][offset][size]      (the schema)
 *   fieldCount x [strategy:1][columnBytes][payload]   (the columns)
 * PLANES payloads hold size x [planeBytes][chunked-compete stream].
 *
 * Field bytes are extracted per the declared endianness (little-endian
 * unless VARINT_RECORD_FLAG_BIG_ENDIAN), so wire-format records
 * round-trip bit-exactly on any host. Signed kinds are sign-extended
 * and ZigZag-mapped before competition so small negatives stay small.
 * BOOL fields are validated to hold only 0/1 at encode time. Record
 * bytes not covered by any field (padding, ignored regions) are not
 * stored; decode zero-fills them. */

#define VARINT_RECORD_MAGIC0 'V'
#define VARINT_RECORD_MAGIC1 'R'
#define VARINT_RECORD_MAGIC2 'E'
#define VARINT_RECORD_MAGIC3 'C'
#define VARINT_RECORD_VERSION 1

/* Bounded so schema scratch and validation stay O(1). */
#define VARINT_RECORD_MAX_FIELDS 64

/* Field kinds. Values are stable wire constants — append only. */
typedef enum varintRecordFieldKind {
    VARINT_RECORD_U8 = 0,
    VARINT_RECORD_U16 = 1,
    VARINT_RECORD_U32 = 2,
    VARINT_RECORD_U64 = 3,
    VARINT_RECORD_I8 = 4,
    VARINT_RECORD_I16 = 5,
    VARINT_RECORD_I32 = 6,
    VARINT_RECORD_I64 = 7,
    VARINT_RECORD_F32 = 8,
    VARINT_RECORD_F64 = 9,
    VARINT_RECORD_BYTES = 10, /* fixed-length opaque run */
    VARINT_RECORD_BOOL = 11,  /* one byte, value 0 or 1 (validated) */
    VARINT_RECORD_DD = 12,    /* varintDD {double hi; double lo}, 16 bytes */
    VARINT_RECORD_KIND_MAX = 13,
} varintRecordFieldKind;

/* Per-column strategies. Values are stable wire constants — append
 * only. Selection is by measurement; decoders validate that a stream's
 * strategy is one its field kind supports. */
typedef enum varintRecordStrategy {
    VARINT_RECORD_STRAT_COMPETE = 0,
    VARINT_RECORD_STRAT_XOR = 1,
    VARINT_RECORD_STRAT_FLOAT = 2,
    VARINT_RECORD_STRAT_PLANES = 3,
    VARINT_RECORD_STRAT_VERBATIM = 4,
    VARINT_RECORD_STRAT_DD_STREAM = 5,
    VARINT_RECORD_STRAT_MAX = 6,
} varintRecordStrategy;

/* Field flag bits (wire-stable). */
#define VARINT_RECORD_FLAG_BIG_ENDIAN 0x01

/* One field of the record schema. */
typedef struct varintRecordField {
    size_t offset; /* byte offset within the record (offsetof) */
    size_t size;   /* byte size of the field (sizeof member) */
    varintRecordFieldKind kind;
    uint8_t flags;
} varintRecordField;

/* Describe a struct member with compile-time layout capture. */
#define VARINT_RECORD_FIELD(structType, member, kindEnum)                      \
    {offsetof(structType, member), sizeof(((structType *)0)->member),          \
     (kindEnum), 0}

/* Same, for big-endian (wire-format) members. */
#define VARINT_RECORD_FIELD_BE(structType, member, kindEnum)                   \
    {offsetof(structType, member), sizeof(((structType *)0)->member),          \
     (kindEnum), VARINT_RECORD_FLAG_BIG_ENDIAN}

/* Stream header parsed back from src. */
typedef struct varintRecordHeader {
    uint64_t recordCount;
    uint64_t recordSize;
    uint64_t fieldCount;
    size_t headerLen; /* bytes up to (not including) the schema table */
    uint8_t version;
} varintRecordHeader;

/* Optional encode result detail, index-aligned with the schema. */
typedef struct varintRecordMeta {
    size_t recordCount;
    size_t recordSize;
    size_t fieldCount;
    size_t encodedSize;                               /* total stream bytes */
    size_t columnBytes[VARINT_RECORD_MAX_FIELDS];     /* payload bytes */
    uint8_t columnStrategy[VARINT_RECORD_MAX_FIELDS]; /* winning lane */
} varintRecordMeta;

/* Human-readable names for diagnostics and telemetry displays. */
const char *varintRecordKindName(varintRecordFieldKind kind);
const char *varintRecordStrategyName(varintRecordStrategy strategy);

/* ====================================================================
 * Encode
 * ==================================================================== */

/* Validate a schema against a record size. Checks: 1..64 fields, every
 * field in-bounds, field size matching its kind's width (BYTES: any
 * size >= 1; DD: exactly 16), no two fields overlapping. */
bool varintRecordSchemaValid(size_t recordSize, const varintRecordField *fields,
                             size_t fieldCount);

/* Conservative output bound for recordCount records under this schema.
 * Returns 0 if the schema is invalid or the bound overflows. */
size_t varintRecordMaxEncodedSize(size_t recordCount, size_t recordSize,
                                  const varintRecordField *fields,
                                  size_t fieldCount);

/* Encode recordCount records of recordSize bytes each, columnar per the
 * schema, measuring every strategy each field kind supports and keeping
 * the smallest verified result per column. codecMask selects the
 * varintCompete candidate set for COMPETE/XOR/PLANES lanes (0 =
 * VARINT_COMPETE_DEFAULT_MASK). meta may be NULL. Returns stream bytes
 * written; 0 on invalid schema/arguments, BOOL fields holding non-0/1
 * values, or allocation failure. */
size_t varintRecordEncode(uint8_t *dst, const void *records, size_t recordCount,
                          size_t recordSize, const varintRecordField *fields,
                          size_t fieldCount, uint64_t codecMask,
                          varintRecordMeta *meta);

/* ====================================================================
 * Decode
 * ==================================================================== */

/* Parse the stream header (magic, version, counts). Returns bytes
 * consumed, 0 on malformed input. Use recordCount * recordSize to size
 * the output buffer — the stream is fully self-describing. */
size_t varintRecordReadHeader(const uint8_t *src, size_t srcBytes,
                              varintRecordHeader *header);

/* Decode a record stream into records (capacity maxRecords records of
 * recordSize bytes each). recordSize must equal the stream's record
 * size — a mismatch fails rather than risk mis-strided writes. The
 * schema is reconstructed from the stream and re-validated by encode's
 * rules; each column's strategy is validated against its field kind.
 * Bytes not covered by any schema field are zero-filled. decodedCount
 * (optional) receives the number of records produced. Returns bytes
 * consumed, 0 on malformed/truncated input, size mismatch, overflow,
 * or allocation failure. */
size_t varintRecordDecode(const uint8_t *src, size_t srcBytes, void *records,
                          size_t maxRecords, size_t recordSize,
                          size_t *decodedCount);

/* ====================================================================
 * Reusable context — allocation reuse across calls
 * ====================================================================
 * Encode and decode need working buffers proportional to the record
 * count (staging columns, lane scratch, verification space). The plain
 * entry points allocate and free them per call; a varintRecordCtx owns
 * them across calls instead, growing each buffer to the largest size
 * any call needed and reusing it thereafter. The win is workloads that
 * call repeatedly — especially the sharding pattern, where every shard
 * shares one schema and one size, so steady state runs with zero
 * allocations. One context serves both encode and decode, any schemas
 * and record counts, in any order; it holds no stream state, only
 * memory. Contexts are not thread-safe: use one per thread. */

typedef struct varintRecordCtx varintRecordCtx;

varintRecordCtx *varintRecordCtxNew(void);
void varintRecordCtxFree(varintRecordCtx *ctx);

/* varintRecordEncode / varintRecordDecode with caller-owned working
 * memory. Identical semantics and output to the plain entry points. */
size_t varintRecordEncodeWithCtx(varintRecordCtx *ctx, uint8_t *dst,
                                 const void *records, size_t recordCount,
                                 size_t recordSize,
                                 const varintRecordField *fields,
                                 size_t fieldCount, uint64_t codecMask,
                                 varintRecordMeta *meta);
size_t varintRecordDecodeWithCtx(varintRecordCtx *ctx, const uint8_t *src,
                                 size_t srcBytes, void *records,
                                 size_t maxRecords, size_t recordSize,
                                 size_t *decodedCount);

/* ====================================================================
 * Stream walking — sharding support
 * ==================================================================== */

/* Total bytes of the record stream starting at src, discovered by
 * walking the header, schema, and column length prefixes without
 * decoding any payload — O(fieldCount). Returns 0 on malformed or
 * truncated input.
 *
 * Streams are self-delimiting, so sharding is concatenation: encode
 * shards of whatever record count fits the memory/parallelism budget,
 * append the streams, and walk them with this function. Each shard is
 * independently decodable (and independently schema'd), so shards can
 * decode in parallel or stream one at a time. */
size_t varintRecordStreamBytes(const uint8_t *src, size_t srcBytes);

/* ====================================================================
 * Diagnostics — schema-driven record printing
 * ==================================================================== */

#include <stdio.h>

/* Print records [firstRecord, firstRecord + maxRecords) as a table, one
 * row per record, formatted per field kind: integers in decimal, floats
 * as %g, BOOL as 0/1, BYTES as hex, and DD through varintDDToString so
 * the 106-bit value prints with its full ~30 significant digits (a
 * plain %g on the hi limb would hide the precision the type exists
 * for). fieldNames may be NULL (columns print as f0..fN) or an array of
 * fieldCount labels, index-aligned with the schema. Returns records
 * printed (0 for an invalid schema or a window past the end). */
size_t varintRecordPrintRecords(FILE *out, const void *records,
                                size_t recordCount, size_t recordSize,
                                const varintRecordField *fields,
                                size_t fieldCount,
                                const char *const *fieldNames,
                                size_t firstRecord, size_t maxRecords);

#ifdef VARINT_RECORD_TEST
int varintRecordTest(int argc, char *argv[]);
#endif

__END_DECLS
