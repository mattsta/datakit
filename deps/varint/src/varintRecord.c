#include "varintRecord.h"
#include "endianIsLittle.h"
#include "varintDD.h"
#include "varintDDStream.h"
#include "varintDelta.h"
#include "varintFloat.h"
#include "varintTagged.h"
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Kind and strategy tables
 * ==================================================================== */

/* Fixed byte width per kind; 0 marks BYTES (any size >= 1). */
static size_t recordKindWidth_(varintRecordFieldKind kind) {
    switch (kind) {
    case VARINT_RECORD_U8:
    case VARINT_RECORD_I8:
    case VARINT_RECORD_BOOL:
        return 1;
    case VARINT_RECORD_U16:
    case VARINT_RECORD_I16:
        return 2;
    case VARINT_RECORD_U32:
    case VARINT_RECORD_I32:
    case VARINT_RECORD_F32:
        return 4;
    case VARINT_RECORD_U64:
    case VARINT_RECORD_I64:
    case VARINT_RECORD_F64:
        return 8;
    case VARINT_RECORD_DD:
        return sizeof(varintDD);
    case VARINT_RECORD_BYTES:
        return 0;
    default:
        return SIZE_MAX; /* invalid kind */
    }
}

static bool recordKindSigned_(varintRecordFieldKind kind) {
    return kind >= VARINT_RECORD_I8 && kind <= VARINT_RECORD_I64;
}

static bool recordKindFloat_(varintRecordFieldKind kind) {
    return kind == VARINT_RECORD_F32 || kind == VARINT_RECORD_F64;
}

/* Which strategies may appear on the wire for a kind. Decode enforces
 * this, so a hostile stream cannot route a payload into a decoder its
 * kind never uses. */
static bool recordStrategyAllowed_(varintRecordFieldKind kind,
                                   varintRecordStrategy strategy) {
    switch (strategy) {
    case VARINT_RECORD_STRAT_VERBATIM:
        return true;
    case VARINT_RECORD_STRAT_COMPETE:
        return kind != VARINT_RECORD_BYTES && kind != VARINT_RECORD_DD;
    case VARINT_RECORD_STRAT_XOR:
        return recordKindFloat_(kind);
    case VARINT_RECORD_STRAT_FLOAT:
        return recordKindFloat_(kind);
    case VARINT_RECORD_STRAT_PLANES:
        return kind == VARINT_RECORD_BYTES ||
               (kind != VARINT_RECORD_DD && recordKindWidth_(kind) >= 2);
    case VARINT_RECORD_STRAT_DD_STREAM:
        return kind == VARINT_RECORD_DD;
    default:
        return false;
    }
}

const char *varintRecordKindName(varintRecordFieldKind kind) {
    switch (kind) {
    case VARINT_RECORD_U8:
        return "U8";
    case VARINT_RECORD_U16:
        return "U16";
    case VARINT_RECORD_U32:
        return "U32";
    case VARINT_RECORD_U64:
        return "U64";
    case VARINT_RECORD_I8:
        return "I8";
    case VARINT_RECORD_I16:
        return "I16";
    case VARINT_RECORD_I32:
        return "I32";
    case VARINT_RECORD_I64:
        return "I64";
    case VARINT_RECORD_F32:
        return "F32";
    case VARINT_RECORD_F64:
        return "F64";
    case VARINT_RECORD_BYTES:
        return "BYTES";
    case VARINT_RECORD_BOOL:
        return "BOOL";
    case VARINT_RECORD_DD:
        return "DD";
    default:
        return "?";
    }
}

const char *varintRecordStrategyName(varintRecordStrategy strategy) {
    switch (strategy) {
    case VARINT_RECORD_STRAT_COMPETE:
        return "COMPETE";
    case VARINT_RECORD_STRAT_XOR:
        return "XOR";
    case VARINT_RECORD_STRAT_FLOAT:
        return "FLOAT";
    case VARINT_RECORD_STRAT_PLANES:
        return "PLANES";
    case VARINT_RECORD_STRAT_VERBATIM:
        return "VERBATIM";
    case VARINT_RECORD_STRAT_DD_STREAM:
        return "DD_STREAM";
    default:
        return "?";
    }
}

/* ====================================================================
 * Schema validation
 * ==================================================================== */

bool varintRecordSchemaValid(size_t recordSize, const varintRecordField *fields,
                             size_t fieldCount) {
    if (!fields || fieldCount == 0 || fieldCount > VARINT_RECORD_MAX_FIELDS ||
        recordSize == 0) {
        return false;
    }

    for (size_t i = 0; i < fieldCount; i++) {
        const size_t width = recordKindWidth_(fields[i].kind);
        if (width == SIZE_MAX) {
            return false;
        }
        if (width == 0) {
            if (fields[i].size == 0) {
                return false;
            }
        } else if (fields[i].size != width) {
            return false;
        }
        if (fields[i].offset > recordSize ||
            fields[i].size > recordSize - fields[i].offset) {
            return false;
        }
    }

    /* No two fields may overlap: sort a copy of the extents by offset
     * (insertion sort — fieldCount is at most 64) and check neighbors. */
    size_t offs[VARINT_RECORD_MAX_FIELDS];
    size_t ends[VARINT_RECORD_MAX_FIELDS];
    for (size_t i = 0; i < fieldCount; i++) {
        size_t o = fields[i].offset;
        size_t e = o + fields[i].size;
        size_t j = i;
        while (j > 0 && offs[j - 1] > o) {
            offs[j] = offs[j - 1];
            ends[j] = ends[j - 1];
            j--;
        }
        offs[j] = o;
        ends[j] = e;
    }
    for (size_t i = 1; i < fieldCount; i++) {
        if (offs[i] < ends[i - 1]) {
            return false;
        }
    }
    return true;
}

/* ====================================================================
 * Field byte access — endianness-explicit, host-independent
 * ==================================================================== */

/* Field loads/stores run once per record per column on both the gather
 * and scatter paths, so they are the record layer's own hot loop. On
 * little-endian hosts each power-of-two width is a single word move
 * (plus a bswap for declared-big-endian fields) via memcpy, which the
 * compiler lowers to one load/store; the byte loops remain as the
 * portable fallback for big-endian hosts. endianIsLittle() folds at
 * compile time, so the fallback costs nothing where it is dead. */

static uint64_t recordLoadField_(const uint8_t *p, size_t size,
                                 bool bigEndian) {
    if (endianIsLittle()) {
        switch (size) {
        case 1:
            return p[0];
        case 2: {
            uint16_t v;
            memcpy(&v, p, sizeof(v));
            return bigEndian ? __builtin_bswap16(v) : v;
        }
        case 4: {
            uint32_t v;
            memcpy(&v, p, sizeof(v));
            return bigEndian ? __builtin_bswap32(v) : v;
        }
        case 8: {
            uint64_t v;
            memcpy(&v, p, sizeof(v));
            return bigEndian ? __builtin_bswap64(v) : v;
        }
        default:
            break;
        }
    }
    uint64_t v = 0;
    if (bigEndian) {
        for (size_t i = 0; i < size; i++) {
            v = (v << 8) | (uint64_t)p[i];
        }
    } else {
        for (size_t i = size; i > 0; i--) {
            v = (v << 8) | (uint64_t)p[i - 1];
        }
    }
    return v;
}

static void recordStoreField_(uint8_t *p, size_t size, bool bigEndian,
                              uint64_t v) {
    if (endianIsLittle()) {
        switch (size) {
        case 1:
            p[0] = (uint8_t)v;
            return;
        case 2: {
            uint16_t w = (uint16_t)v;
            if (bigEndian) {
                w = __builtin_bswap16(w);
            }
            memcpy(p, &w, sizeof(w));
            return;
        }
        case 4: {
            uint32_t w = (uint32_t)v;
            if (bigEndian) {
                w = __builtin_bswap32(w);
            }
            memcpy(p, &w, sizeof(w));
            return;
        }
        case 8: {
            uint64_t w = v;
            if (bigEndian) {
                w = __builtin_bswap64(w);
            }
            memcpy(p, &w, sizeof(w));
            return;
        }
        default:
            break;
        }
    }
    if (bigEndian) {
        for (size_t i = size; i > 0; i--) {
            p[i - 1] = (uint8_t)v;
            v >>= 8;
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            p[i] = (uint8_t)v;
            v >>= 8;
        }
    }
}

static int64_t recordSignExtend_(uint64_t v, size_t size) {
    const size_t shift = 64 - size * 8;
    return (int64_t)(v << shift) >> shift;
}

/* Overflow-checked multiply for allocation and bound math. */
static bool recordMul_(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

/* ====================================================================
 * Reusable context — allocation reuse across calls
 * ====================================================================
 * All working memory lives in grow-only slots: a slot is reallocated
 * only when a call needs more than any earlier call did, so repeated
 * same-shape calls (the sharding pattern) run with zero allocations.
 * Buffer contents never survive between calls — only capacity does. */

struct varintRecordCtx {
    uint64_t *column; /* one column of values */
    uint64_t *aux;    /* transform scratch (signed map, XOR) */
    uint64_t *bits;   /* fused gather staging arena */
    double *doubles;  /* FLOAT lane values */
    varintDD *dd0;    /* DD lane values */
    varintDD *dd1;    /* DD lane verification */
    uint8_t *lane0;   /* candidate payload (ping) */
    uint8_t *lane1;   /* candidate payload (pong) */
    uint8_t *guard;   /* bounded varintFloat decode scratch */
    uint8_t *cs0;     /* chunked-compete scratch (ping) */
    uint8_t *cs1;     /* chunked-compete scratch (pong) */
    size_t columnCap;
    size_t auxCap;
    size_t bitsCap;
    size_t doublesCap;
    size_t dd0Cap;
    size_t dd1Cap;
    size_t lane0Cap;
    size_t lane1Cap;
    size_t guardCap;
    size_t cs0Cap;
    size_t cs1Cap;
};

/* Grow-only reservation: contents are not preserved (no call needs
 * them to be), so growth is free+malloc rather than realloc-copy. */
static bool recordReserve_(void *slotAny, size_t *cap, size_t need) {
    void **slot = slotAny;
    if (need == 0) {
        return true;
    }
    if (*slot && need <= *cap) {
        return true;
    }
    free(*slot);
    *slot = malloc(need);
    *cap = *slot ? need : 0;
    return *slot != NULL;
}

static void recordCtxRelease_(varintRecordCtx *ws) {
    free(ws->column);
    free(ws->aux);
    free(ws->bits);
    free(ws->doubles);
    free(ws->dd0);
    free(ws->dd1);
    free(ws->lane0);
    free(ws->lane1);
    free(ws->guard);
    free(ws->cs0);
    free(ws->cs1);
    memset(ws, 0, sizeof(*ws));
}

varintRecordCtx *varintRecordCtxNew(void) {
    return calloc(1, sizeof(varintRecordCtx));
}

void varintRecordCtxFree(varintRecordCtx *ctx) {
    if (!ctx) {
        return;
    }
    recordCtxRelease_(ctx);
    free(ctx);
}

/* ====================================================================
 * Column gathers
 * ==================================================================== */

/* COMPETE-domain gather: field bits per declared endianness, signed
 * kinds sign-extended + ZigZag so small negatives stay small, float
 * kinds as raw bit patterns. BOOL validates its 0/1 contract. */
static bool recordGatherNormalized_(const uint8_t *base, size_t recordCount,
                                    size_t recordSize,
                                    const varintRecordField *fld,
                                    uint64_t *column) {
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
    const bool isSigned = recordKindSigned_(fld->kind);
    const bool isBool = (fld->kind == VARINT_RECORD_BOOL);
    for (size_t i = 0; i < recordCount; i++) {
        uint64_t v = recordLoadField_(base + i * recordSize + fld->offset,
                                      fld->size, bigEndian);
        if (isBool && v > 1) {
            return false;
        }
        if (isSigned) {
            v = varintDeltaZigZag(recordSignExtend_(v, fld->size));
        }
        column[i] = v;
    }
    return true;
}

/* Fields narrow enough to stage as a u64 bits column. */
static bool recordFieldNarrow_(const varintRecordField *fld) {
    return fld->kind != VARINT_RECORD_DD &&
           (fld->kind != VARINT_RECORD_BYTES || fld->size <= 8);
}

/* Cache-block size: the record window all per-field passes revisit
 * while it is still resident, bounding record-array DRAM traffic near
 * one pass regardless of field count. */
static size_t recordBlockRecords_(size_t recordSize) {
    size_t block = (256 * 1024) / recordSize;
    if (block < 64) {
        block = 64;
    }
    if (block > 8192) {
        block = 8192;
    }
    return block;
}

/* Staging memory ceiling for fused gather/scatter; schemas whose
 * columns exceed it fall back to the per-field paths. */
#define VARINT_RECORD_FUSE_BUDGET ((size_t)256 << 20)

/* Fused gather: fill every narrow field's raw-bits column (value bits
 * per declared endianness, no sign mapping) in one cache-blocked sweep
 * over the records — field passes within a block hit hot cache instead
 * of re-streaming the record array once per field. BYTES columns stage
 * their raw bytes as little-endian-packed u64s. Returns false on a
 * BOOL contract violation. */
static bool recordGatherBitsFused_(const uint8_t *base, size_t recordCount,
                                   size_t recordSize,
                                   const varintRecordField *fields,
                                   size_t fieldCount, uint64_t *const *cols) {
    const size_t block = recordBlockRecords_(recordSize);
    for (size_t start = 0; start < recordCount; start += block) {
        size_t end = start + block;
        if (end > recordCount) {
            end = recordCount;
        }
        for (size_t f = 0; f < fieldCount; f++) {
            if (!cols[f]) {
                continue;
            }
            const varintRecordField *fld = &fields[f];
            const bool bigEndian =
                (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN) &&
                fld->kind != VARINT_RECORD_BYTES;
            uint64_t *col = cols[f];
            const uint8_t *p = base + start * recordSize + fld->offset;
            if (fld->kind == VARINT_RECORD_BOOL) {
                for (size_t i = start; i < end; i++, p += recordSize) {
                    const uint64_t v = p[0];
                    if (v > 1) {
                        return false;
                    }
                    col[i] = v;
                }
            } else {
                for (size_t i = start; i < end; i++, p += recordSize) {
                    col[i] = recordLoadField_(p, fld->size, bigEndian);
                }
            }
        }
    }
    return true;
}

/* Raw-byte gather: the VERBATIM payload, exactly as laid out in the
 * records (no endianness or sign transform). */
static void recordGatherRaw_(const uint8_t *base, size_t recordCount,
                             size_t recordSize, const varintRecordField *fld,
                             uint8_t *out) {
    for (size_t i = 0; i < recordCount; i++) {
        memcpy(out + i * fld->size, base + i * recordSize + fld->offset,
               fld->size);
    }
}

/* Value-domain gather for the FLOAT lane: every value as a double. */
static void recordGatherDoubles_(const uint8_t *base, size_t recordCount,
                                 size_t recordSize,
                                 const varintRecordField *fld, double *out) {
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
    for (size_t i = 0; i < recordCount; i++) {
        const uint64_t bits = recordLoadField_(
            base + i * recordSize + fld->offset, fld->size, bigEndian);
        if (fld->kind == VARINT_RECORD_F32) {
            const uint32_t b32 = (uint32_t)bits;
            float f;
            memcpy(&f, &b32, sizeof(f));
            out[i] = (double)f;
        } else {
            memcpy(&out[i], &bits, sizeof(double));
        }
    }
}

/* ====================================================================
 * FLOAT lane bounds and guarded decode
 * ====================================================================
 * varintFloat's decoder derives its read lengths from four header bytes
 * plus the count, so a bounded decode requires (a) validating those
 * header fields against the encoder's real ranges, and (b) running the
 * decoder against a scratch copy sized to the worst case those
 * validated fields allow. */

static size_t recordFloatReadBound_(size_t count) {
    /* header(4) + special bitmap + sign bitmap + exponents (tagged
     * varints, <= 9 each) + mantissas (<= 52 bits each) + slack */
    return 4 + 2 * ((count + 7) / 8) + count * 9 + (count * 52 + 7) / 8 + 64;
}

/* guard is caller-reserved workspace of recordFloatReadBound_(count)
 * bytes (both call sites reserve exactly that through the context). */
static bool recordFloatDecodeGuarded_(const uint8_t *payload, size_t colBytes,
                                      size_t count, double *out,
                                      uint8_t *guard) {
    if (colBytes < 4 || count == 0 || !guard) {
        return false;
    }
    if (payload[0] > 3 || payload[1] > 16 || payload[2] > 52 ||
        payload[3] > 2) {
        return false;
    }
    const size_t bound = recordFloatReadBound_(count);
    if (colBytes > bound) {
        return false;
    }
    memcpy(guard, payload, colBytes);
    memset(guard + colBytes, 0, bound - colBytes);
    const size_t consumed = varintFloatDecode(guard, count, out);
    return consumed == colBytes;
}

/* ====================================================================
 * Encode
 * ==================================================================== */

/* Chunked-compete payload bound shared by COMPETE/XOR/plane streams. */
static size_t recordCompeteBound_(size_t recordCount) {
    return varintCompeteMaxEncodedSizeChunked(recordCount, 0);
}

/* Largest payload any lane may produce for this field. */
static bool recordColumnBound_(size_t recordCount, const varintRecordField *fld,
                               size_t *out) {
    size_t verbatim, bound;
    if (!recordMul_(recordCount, fld->size, &verbatim)) {
        return false;
    }
    bound = verbatim;
    if (fld->kind != VARINT_RECORD_DD) {
        /* PLANES: one chunked stream (plus length prefix) per byte. */
        size_t planes;
        if (!recordMul_(fld->size, 9 + recordCompeteBound_(recordCount),
                        &planes)) {
            return false;
        }
        if (planes > bound) {
            bound = planes;
        }
    }
    if (recordKindFloat_(fld->kind)) {
        const size_t f =
            varintFloatMaxEncodedSize(recordCount, VARINT_FLOAT_PRECISION_FULL);
        if (f > bound) {
            bound = f;
        }
    }
    if (fld->kind == VARINT_RECORD_DD) {
        const size_t d = varintDDStreamMaxSize(recordCount);
        if (d > bound) {
            bound = d;
        }
    }
    if (recordCompeteBound_(recordCount) > bound) {
        bound = recordCompeteBound_(recordCount);
    }
    *out = bound;
    return true;
}

size_t varintRecordMaxEncodedSize(size_t recordCount, size_t recordSize,
                                  const varintRecordField *fields,
                                  size_t fieldCount) {
    if (!varintRecordSchemaValid(recordSize, fields, fieldCount)) {
        return 0;
    }
    /* magic(4) + version(1) + 3 tagged counts + schema table */
    size_t total = 4 + 1 + 3 * 9 + fieldCount * (1 + 1 + 9 + 9);
    if (recordCount == 0) {
        return total;
    }
    for (size_t i = 0; i < fieldCount; i++) {
        size_t bound;
        if (!recordColumnBound_(recordCount, &fields[i], &bound)) {
            return 0;
        }
        /* strategy(1) + columnBytes(<=9) + payload */
        if (bound > SIZE_MAX - total - 10) {
            return 0;
        }
        total += 1 + 9 + bound;
    }
    return total;
}

/* Working state shared by every column of one encode call. All buffer
 * pointers are borrowed from the varintRecordCtx workspace. */
typedef struct recordEncodeCtx {
    uint64_t *column;   /* recordCount normalized/plane values */
    uint64_t *aux;      /* recordCount transform scratch */
    double *doubles;    /* recordCount, FLOAT lane (aliases: verify out) */
    varintDD *ddVals;   /* recordCount, DD lane (NULL unless needed) */
    varintDD *ddVerify; /* recordCount, DD verification (NULL unless) */
    uint8_t *bestBuf;   /* winning payload so far */
    uint8_t *tryBuf;    /* candidate payload */
    uint8_t *guard;     /* FLOAT verification decode workspace */
    const varintCompeteChunkedScratch *cs; /* chunked-compete scratch */
    size_t recordCount;
    uint64_t codecMask;
} recordEncodeCtx;

/* Consider one candidate payload of length n (0 = lane declined). */
static void recordConsider_(recordEncodeCtx *ctx, varintRecordStrategy strat,
                            size_t n, varintRecordStrategy *bestStrat,
                            size_t *bestLen) {
    if (n > 0 && n < *bestLen) {
        uint8_t *tmp = ctx->bestBuf;
        ctx->bestBuf = ctx->tryBuf;
        ctx->tryBuf = tmp;
        *bestStrat = strat;
        *bestLen = n;
    }
}

/* PLANES lane: byte position p of every value becomes its own column
 * through the chunked competition. Payload: size x [planeBytes][VCHK].
 * Operates on raw record bytes, so it is transform-free and lossless
 * for every kind. When a staged bits column exists, plane bytes derive
 * from it sequentially instead of re-striding the record array once
 * per byte of width. */
static size_t recordEncodePlanes_(recordEncodeCtx *ctx, const uint8_t *base,
                                  size_t recordSize,
                                  const varintRecordField *fld,
                                  const uint64_t *bits, uint8_t *dst) {
    const size_t n = ctx->recordCount;
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN) &&
                           fld->kind != VARINT_RECORD_BYTES;
    size_t written = 0;
    for (size_t p = 0; p < fld->size; p++) {
        if (bits) {
            /* Raw byte p of the field: for little-endian layouts that
             * is bits byte p; declared-big-endian fields store their
             * raw bytes reversed relative to the value bits. */
            const size_t shift = 8 * (bigEndian ? (fld->size - 1 - p) : p);
            for (size_t i = 0; i < n; i++) {
                ctx->column[i] = (bits[i] >> shift) & 0xFF;
            }
        } else {
            for (size_t i = 0; i < n; i++) {
                ctx->column[i] = base[i * recordSize + fld->offset + p];
            }
        }
        /* Reserve the worst-case tagged prefix, then move the stream
         * back over the gap once its real length is known. */
        uint8_t *payload = dst + written + 9;
        const size_t planeBytes = varintCompeteEncodeChunkedUnsignedScratch(
            payload, ctx->column, n, ctx->codecMask, 0, NULL, ctx->cs);
        if (planeBytes == 0) {
            return 0;
        }
        const size_t prefix =
            (size_t)varintTaggedPut64(dst + written, planeBytes);
        memmove(dst + written + prefix, payload, planeBytes);
        written += prefix + planeBytes;
    }
    return written;
}

/* FLOAT lane: value-domain doubles through varintFloat, all three
 * exponent-coding modes measured, winner verified bit-exact by decode
 * before it may compete. Doubles and the verification reference both
 * derive from the staged bits column when present. */
static size_t recordEncodeFloat_(recordEncodeCtx *ctx, const uint8_t *base,
                                 size_t recordSize,
                                 const varintRecordField *fld,
                                 const uint64_t *bits, uint8_t *dst) {
    const size_t n = ctx->recordCount;
    if (bits) {
        for (size_t i = 0; i < n; i++) {
            if (fld->kind == VARINT_RECORD_F32) {
                const uint32_t b32 = (uint32_t)bits[i];
                float fv;
                memcpy(&fv, &b32, sizeof(fv));
                ctx->doubles[i] = (double)fv;
            } else {
                memcpy(&ctx->doubles[i], &bits[i], sizeof(double));
            }
        }
    } else {
        recordGatherDoubles_(base, n, recordSize, fld, ctx->doubles);
    }

    static const varintFloatEncodingMode modes[] = {
        VARINT_FLOAT_MODE_INDEPENDENT,
        VARINT_FLOAT_MODE_COMMON_EXPONENT,
        VARINT_FLOAT_MODE_DELTA_EXPONENT,
    };
    size_t best = 0;
    varintFloatEncodingMode bestMode = VARINT_FLOAT_MODE_INDEPENDENT;
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        const size_t len = varintFloatEncode(
            dst, ctx->doubles, n, VARINT_FLOAT_PRECISION_FULL, modes[m]);
        if (len > 0 && (best == 0 || len < best)) {
            best = len;
            bestMode = modes[m];
        }
    }
    if (best == 0) {
        return 0;
    }
    /* dst holds the last mode tried; re-emit the winner. */
    if (varintFloatEncode(dst, ctx->doubles, n, VARINT_FLOAT_PRECISION_FULL,
                          bestMode) != best) {
        return 0;
    }

    /* Verification: the lane may only compete if its round trip is
     * bit-exact for this exact data (NaN payloads, subnormals, and
     * negative zeros are where float pipelines quietly diverge). */
    double *verify = (double *)(void *)ctx->aux;
    if (!recordFloatDecodeGuarded_(dst, best, n, verify, ctx->guard)) {
        return 0;
    }
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
    for (size_t i = 0; i < n; i++) {
        uint64_t roundTrip;
        if (fld->kind == VARINT_RECORD_F32) {
            const float f = (float)verify[i];
            uint32_t b32;
            memcpy(&b32, &f, sizeof(b32));
            roundTrip = b32;
        } else {
            memcpy(&roundTrip, &verify[i], sizeof(roundTrip));
        }
        const uint64_t original =
            bits ? bits[i]
                 : recordLoadField_(base + i * recordSize + fld->offset,
                                    fld->size, bigEndian);
        if (roundTrip != original) {
            return 0;
        }
    }
    return best;
}

/* DD lane: varintDDStream lossless, verified bit-exact by decode. */
static size_t recordEncodeDD_(recordEncodeCtx *ctx, const uint8_t *base,
                              size_t recordSize, const varintRecordField *fld,
                              uint8_t *dst) {
    const size_t n = ctx->recordCount;
    for (size_t i = 0; i < n; i++) {
        memcpy(&ctx->ddVals[i], base + i * recordSize + fld->offset,
               sizeof(varintDD));
    }
    const size_t len =
        varintDDStreamEncode(dst, ctx->ddVals, n, VARINT_DD_STREAM_HI_AUTO,
                             VARINT_DD_STREAM_LOSSLESS, NULL);
    if (len == 0) {
        return 0;
    }
    if (varintDDStreamDecode(dst, len, ctx->ddVerify, n) != n ||
        memcmp(ctx->ddVerify, ctx->ddVals, n * sizeof(varintDD)) != 0) {
        return 0;
    }
    return len;
}

size_t varintRecordEncodeWithCtx(varintRecordCtx *ws, uint8_t *dst,
                                 const void *records, size_t recordCount,
                                 size_t recordSize,
                                 const varintRecordField *fields,
                                 size_t fieldCount, uint64_t codecMask,
                                 varintRecordMeta *meta) {
    if (!ws || !dst || (!records && recordCount > 0) ||
        !varintRecordSchemaValid(recordSize, fields, fieldCount)) {
        return 0;
    }
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        meta->recordCount = recordCount;
        meta->recordSize = recordSize;
        meta->fieldCount = fieldCount;
    }

    uint8_t *p = dst;
    *p++ = VARINT_RECORD_MAGIC0;
    *p++ = VARINT_RECORD_MAGIC1;
    *p++ = VARINT_RECORD_MAGIC2;
    *p++ = VARINT_RECORD_MAGIC3;
    *p++ = (uint8_t)VARINT_RECORD_VERSION;
    p += varintTaggedPut64(p, recordCount);
    p += varintTaggedPut64(p, recordSize);
    p += varintTaggedPut64(p, fieldCount);

    for (size_t f = 0; f < fieldCount; f++) {
        *p++ = (uint8_t)fields[f].kind;
        *p++ = fields[f].flags;
        p += varintTaggedPut64(p, fields[f].offset);
        p += varintTaggedPut64(p, fields[f].size);
    }

    if (recordCount == 0) {
        if (meta) {
            meta->encodedSize = (size_t)(p - dst);
        }
        return (size_t)(p - dst);
    }

    /* One scratch set reused across every column, sized by the largest
     * lane bound any field can reach. */
    size_t laneBound = 0;
    bool needFloat = false;
    bool needDD = false;
    for (size_t f = 0; f < fieldCount; f++) {
        size_t bound;
        if (!recordColumnBound_(recordCount, &fields[f], &bound)) {
            return 0;
        }
        if (bound > laneBound) {
            laneBound = bound;
        }
        needFloat = needFloat || recordKindFloat_(fields[f].kind);
        needDD = needDD || fields[f].kind == VARINT_RECORD_DD;
    }

    /* Reserve every buffer in the workspace: grow-only, so repeated
     * same-shape calls (sharding) reuse memory with zero allocations. */
    const size_t csBytes = varintCompeteChunkedScratchBytes(0);
    varintCompeteChunkedScratch cs;
    bool allocOk =
        recordReserve_(&ws->column, &ws->columnCap,
                       recordCount * sizeof(uint64_t)) &&
        recordReserve_(&ws->aux, &ws->auxCap, recordCount * sizeof(uint64_t)) &&
        recordReserve_(&ws->lane0, &ws->lane0Cap, laneBound) &&
        recordReserve_(&ws->lane1, &ws->lane1Cap, laneBound) &&
        recordReserve_(&ws->cs0, &ws->cs0Cap, csBytes) &&
        (!needFloat || (recordReserve_(&ws->doubles, &ws->doublesCap,
                                       recordCount * sizeof(double)) &&
                        recordReserve_(&ws->guard, &ws->guardCap,
                                       recordFloatReadBound_(recordCount)))) &&
        (!needDD || (recordReserve_(&ws->dd0, &ws->dd0Cap,
                                    recordCount * sizeof(varintDD)) &&
                     recordReserve_(&ws->dd1, &ws->dd1Cap,
                                    recordCount * sizeof(varintDD))));
    allocOk =
        allocOk && varintCompeteChunkedScratchInit(&cs, ws->cs0, ws->cs0Cap, 0);

    recordEncodeCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.recordCount = recordCount;
    ctx.codecMask = codecMask;
    ctx.column = ws->column;
    ctx.aux = ws->aux;
    ctx.doubles = ws->doubles;
    ctx.ddVals = ws->dd0;
    ctx.ddVerify = ws->dd1;
    ctx.bestBuf = ws->lane0;
    ctx.tryBuf = ws->lane1;
    ctx.guard = ws->guard;
    ctx.cs = &cs;

    /* Fused staging: every narrow field's bits column fills in one
     * cache-blocked sweep, and all lanes (compete input, XOR transform,
     * float values, verbatim bytes, plane bytes, verification) derive
     * from it — the record array streams once instead of once per lane
     * per field. Falls back to per-field gathers past the budget. */
    uint64_t *bitsCols[VARINT_RECORD_MAX_FIELDS] = {NULL};
    size_t narrowCount = 0;
    for (size_t f = 0; f < fieldCount; f++) {
        if (recordFieldNarrow_(&fields[f])) {
            narrowCount++;
        }
    }
    size_t arenaBytes;
    if (allocOk && narrowCount > 0 &&
        recordMul_(narrowCount * sizeof(uint64_t), recordCount, &arenaBytes) &&
        arenaBytes <= VARINT_RECORD_FUSE_BUDGET &&
        recordReserve_(&ws->bits, &ws->bitsCap, arenaBytes)) {
        size_t slot = 0;
        for (size_t f = 0; f < fieldCount; f++) {
            if (recordFieldNarrow_(&fields[f])) {
                bitsCols[f] = ws->bits + slot * recordCount;
                slot++;
            }
        }
        if (!recordGatherBitsFused_(records, recordCount, recordSize, fields,
                                    fieldCount, bitsCols)) {
            allocOk = false; /* BOOL contract violation */
        }
    }

    const uint8_t *base = records;
    size_t written = (size_t)(p - dst);

    for (size_t f = 0; allocOk && f < fieldCount; f++) {
        const varintRecordField *fld = &fields[f];
        const uint64_t *bits = bitsCols[f];
        varintRecordStrategy bestStrat = VARINT_RECORD_STRAT_VERBATIM;
        size_t bestLen = SIZE_MAX;
        size_t rawLen = recordCount * fld->size;

        /* VERBATIM first: it is the guaranteed floor, so every later
         * lane must beat raw bytes to win. Raw bytes derive from the
         * staged bits (BYTES columns stage little-endian-packed, so a
         * plain LE store reproduces them). */
        if (bits) {
            const bool bigEndian =
                (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN) &&
                fld->kind != VARINT_RECORD_BYTES;
            for (size_t i = 0; i < recordCount; i++) {
                recordStoreField_(ctx.tryBuf + i * fld->size, fld->size,
                                  bigEndian, bits[i]);
            }
        } else {
            recordGatherRaw_(base, recordCount, recordSize, fld, ctx.tryBuf);
        }
        recordConsider_(&ctx, VARINT_RECORD_STRAT_VERBATIM, rawLen, &bestStrat,
                        &bestLen);

        if (recordStrategyAllowed_(fld->kind, VARINT_RECORD_STRAT_COMPETE)) {
            const uint64_t *competeInput;
            if (bits) {
                if (recordKindSigned_(fld->kind)) {
                    for (size_t i = 0; i < recordCount; i++) {
                        ctx.column[i] = varintDeltaZigZag(
                            recordSignExtend_(bits[i], fld->size));
                    }
                    competeInput = ctx.column;
                } else {
                    competeInput = bits;
                }
            } else {
                if (!recordGatherNormalized_(base, recordCount, recordSize, fld,
                                             ctx.column)) {
                    allocOk = false; /* BOOL contract violation */
                    break;
                }
                competeInput = ctx.column;
            }
            recordConsider_(&ctx, VARINT_RECORD_STRAT_COMPETE,
                            varintCompeteEncodeChunkedUnsignedScratch(
                                ctx.tryBuf, competeInput, recordCount,
                                codecMask, 0, NULL, ctx.cs),
                            &bestStrat, &bestLen);

            if (recordStrategyAllowed_(fld->kind, VARINT_RECORD_STRAT_XOR)) {
                ctx.aux[0] = competeInput[0];
                for (size_t i = 1; i < recordCount; i++) {
                    ctx.aux[i] = competeInput[i] ^ competeInput[i - 1];
                }
                recordConsider_(&ctx, VARINT_RECORD_STRAT_XOR,
                                varintCompeteEncodeChunkedUnsignedScratch(
                                    ctx.tryBuf, ctx.aux, recordCount, codecMask,
                                    0, NULL, ctx.cs),
                                &bestStrat, &bestLen);
            }
        }

        /* FLOAT costs three varintFloat passes plus a verification
         * decode; like PLANES it runs only when the cheap lanes left
         * compression on the table. */
        if (recordStrategyAllowed_(fld->kind, VARINT_RECORD_STRAT_FLOAT) &&
            bestLen * 10 > rawLen * 6) {
            recordConsider_(&ctx, VARINT_RECORD_STRAT_FLOAT,
                            recordEncodeFloat_(&ctx, base, recordSize, fld,
                                               bits, ctx.tryBuf),
                            &bestStrat, &bestLen);
        }

        if (recordStrategyAllowed_(fld->kind, VARINT_RECORD_STRAT_DD_STREAM)) {
            recordConsider_(
                &ctx, VARINT_RECORD_STRAT_DD_STREAM,
                recordEncodeDD_(&ctx, base, recordSize, fld, ctx.tryBuf),
                &bestStrat, &bestLen);
        }

        /* PLANES costs one competition per byte of width, so it runs
         * only when the cheap lanes left compression on the table. */
        if (recordStrategyAllowed_(fld->kind, VARINT_RECORD_STRAT_PLANES) &&
            bestLen * 10 > rawLen * 6) {
            recordConsider_(&ctx, VARINT_RECORD_STRAT_PLANES,
                            recordEncodePlanes_(&ctx, base, recordSize, fld,
                                                bits, ctx.tryBuf),
                            &bestStrat, &bestLen);
        }

        p = dst + written;
        *p++ = (uint8_t)bestStrat;
        p += varintTaggedPut64(p, bestLen);
        memcpy(p, ctx.bestBuf, bestLen);
        written = (size_t)(p - dst) + bestLen;
        if (meta) {
            meta->columnBytes[f] = bestLen;
            meta->columnStrategy[f] = (uint8_t)bestStrat;
        }
    }

    if (!allocOk) {
        return 0;
    }
    if (meta) {
        meta->encodedSize = written;
    }
    return written;
}

/* Cleanup hook so plain-API workspaces release automatically on every
 * return path. */
static void recordCtxCleanup_(varintRecordCtx *ws) {
    recordCtxRelease_(ws);
}

size_t varintRecordEncode(uint8_t *dst, const void *records, size_t recordCount,
                          size_t recordSize, const varintRecordField *fields,
                          size_t fieldCount, uint64_t codecMask,
                          varintRecordMeta *meta) {
    __attribute__((cleanup(recordCtxCleanup_))) varintRecordCtx ws;
    memset(&ws, 0, sizeof(ws));
    return varintRecordEncodeWithCtx(&ws, dst, records, recordCount, recordSize,
                                     fields, fieldCount, codecMask, meta);
}

/* ====================================================================
 * Decode
 * ==================================================================== */

static varintWidth recordReadTagged_(const uint8_t *src, size_t remain,
                                     uint64_t *out) {
    return varintTaggedGet(src, remain > 9 ? 9 : (int32_t)remain, out);
}

size_t varintRecordReadHeader(const uint8_t *src, size_t srcBytes,
                              varintRecordHeader *header) {
    assert(header != NULL);
    if (!src || srcBytes < 8) {
        return 0;
    }
    if (src[0] != VARINT_RECORD_MAGIC0 || src[1] != VARINT_RECORD_MAGIC1 ||
        src[2] != VARINT_RECORD_MAGIC2 || src[3] != VARINT_RECORD_MAGIC3) {
        return 0;
    }
    if (src[4] != VARINT_RECORD_VERSION) {
        return 0;
    }
    size_t cursor = 5;
    uint64_t counts[3];
    for (size_t i = 0; i < 3; i++) {
        varintWidth w =
            recordReadTagged_(src + cursor, srcBytes - cursor, &counts[i]);
        if (w == 0) {
            return 0;
        }
        cursor += (size_t)w;
    }
    header->recordCount = counts[0];
    header->recordSize = counts[1];
    header->fieldCount = counts[2];
    header->version = src[4];
    header->headerLen = cursor;
    if (header->recordSize == 0 || header->fieldCount == 0 ||
        header->fieldCount > VARINT_RECORD_MAX_FIELDS) {
        return 0;
    }
    /* recordCount * recordSize must be representable — this is the
     * output size the caller will allocate. */
    if (header->recordCount > 0 &&
        header->recordSize > SIZE_MAX / header->recordCount) {
        return 0;
    }
    return cursor;
}

/* Scatter a decoded u64 column back into the record fields, undoing
 * the COMPETE-domain normalization. */
static void recordScatterNormalized_(uint8_t *base, size_t recordCount,
                                     size_t recordSize,
                                     const varintRecordField *fld,
                                     const uint64_t *column) {
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
    const bool isSigned = recordKindSigned_(fld->kind);
    for (size_t i = 0; i < recordCount; i++) {
        uint64_t v = column[i];
        if (isSigned) {
            v = (uint64_t)varintDeltaZigZagDecode(v);
        }
        recordStoreField_(base + i * recordSize + fld->offset, fld->size,
                          bigEndian, v);
    }
}

size_t varintRecordDecodeWithCtx(varintRecordCtx *ws, const uint8_t *src,
                                 size_t srcBytes, void *records,
                                 size_t maxRecords, size_t recordSize,
                                 size_t *decodedCount) {
    if (!ws || !src || !records) {
        return 0;
    }
    varintRecordHeader hdr;
    size_t cursor = varintRecordReadHeader(src, srcBytes, &hdr);
    if (cursor == 0) {
        return 0;
    }
    if (hdr.recordCount > maxRecords || hdr.recordSize != recordSize) {
        return 0;
    }

    /* Reconstruct and re-validate the schema from the stream, so a
     * hostile stream is rejected by the same rules encode enforces. */
    varintRecordField fields[VARINT_RECORD_MAX_FIELDS];
    const size_t fieldCount = (size_t)hdr.fieldCount;
    for (size_t f = 0; f < fieldCount; f++) {
        if (srcBytes - cursor < 2) {
            return 0;
        }
        fields[f].kind = (varintRecordFieldKind)src[cursor++];
        fields[f].flags = src[cursor++];
        uint64_t off, sz;
        varintWidth w =
            recordReadTagged_(src + cursor, srcBytes - cursor, &off);
        if (w == 0) {
            return 0;
        }
        cursor += (size_t)w;
        w = recordReadTagged_(src + cursor, srcBytes - cursor, &sz);
        if (w == 0) {
            return 0;
        }
        cursor += (size_t)w;
        if (off > SIZE_MAX || sz > SIZE_MAX) {
            return 0;
        }
        fields[f].offset = (size_t)off;
        fields[f].size = (size_t)sz;
    }
    if (!varintRecordSchemaValid(recordSize, fields, fieldCount)) {
        return 0;
    }

    const size_t recordCount = (size_t)hdr.recordCount;
    /* Uncovered bytes (padding, undeclared regions) decode as zeros. */
    memset(records, 0, recordCount * recordSize);
    if (recordCount == 0) {
        if (decodedCount) {
            *decodedCount = 0;
        }
        return cursor;
    }

    if (!recordReserve_(&ws->column, &ws->columnCap,
                        recordCount * sizeof(uint64_t))) {
        return 0;
    }
    uint64_t *column = ws->column;

    uint8_t *base = records;
    bool ok = true;
    for (size_t f = 0; ok && f < fieldCount; f++) {
        const varintRecordField *fld = &fields[f];
        if (srcBytes - cursor < 1) {
            ok = false;
            break;
        }
        const varintRecordStrategy strat = (varintRecordStrategy)src[cursor++];
        if (strat >= VARINT_RECORD_STRAT_MAX ||
            !recordStrategyAllowed_(fld->kind, strat)) {
            ok = false;
            break;
        }

        uint64_t colBytes64;
        varintWidth w =
            recordReadTagged_(src + cursor, srcBytes - cursor, &colBytes64);
        if (w == 0 || colBytes64 > srcBytes - cursor - (size_t)w) {
            ok = false;
            break;
        }
        cursor += (size_t)w;
        const size_t colBytes = (size_t)colBytes64;
        const uint8_t *payload = src + cursor;
        cursor += colBytes;

        switch (strat) {
        case VARINT_RECORD_STRAT_VERBATIM: {
            if (colBytes != recordCount * fld->size) {
                ok = false;
                break;
            }
            for (size_t i = 0; i < recordCount; i++) {
                memcpy(base + i * recordSize + fld->offset,
                       payload + i * fld->size, fld->size);
            }
            break;
        }
        case VARINT_RECORD_STRAT_COMPETE:
        case VARINT_RECORD_STRAT_XOR: {
            size_t got = 0;
            if (varintCompeteDecodeChunkedUnsigned(payload, colBytes, column,
                                                   recordCount, &got) == 0 ||
                got != recordCount) {
                ok = false;
                break;
            }
            if (strat == VARINT_RECORD_STRAT_XOR) {
                for (size_t i = 1; i < recordCount; i++) {
                    column[i] ^= column[i - 1];
                }
                /* XOR columns carry raw bit patterns — scatter without
                 * the signed un-mapping. */
                const bool bigEndian =
                    (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
                for (size_t i = 0; i < recordCount; i++) {
                    recordStoreField_(base + i * recordSize + fld->offset,
                                      fld->size, bigEndian, column[i]);
                }
            } else {
                recordScatterNormalized_(base, recordCount, recordSize, fld,
                                         column);
            }
            break;
        }
        case VARINT_RECORD_STRAT_FLOAT: {
            if (!recordReserve_(&ws->doubles, &ws->doublesCap,
                                recordCount * sizeof(double)) ||
                !recordReserve_(&ws->guard, &ws->guardCap,
                                recordFloatReadBound_(recordCount)) ||
                !recordFloatDecodeGuarded_(payload, colBytes, recordCount,
                                           ws->doubles, ws->guard)) {
                ok = false;
                break;
            }
            const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
            for (size_t i = 0; i < recordCount; i++) {
                uint64_t bits;
                if (fld->kind == VARINT_RECORD_F32) {
                    const float fv = (float)ws->doubles[i];
                    uint32_t b32;
                    memcpy(&b32, &fv, sizeof(b32));
                    bits = b32;
                } else {
                    memcpy(&bits, &ws->doubles[i], sizeof(bits));
                }
                recordStoreField_(base + i * recordSize + fld->offset,
                                  fld->size, bigEndian, bits);
            }
            break;
        }
        case VARINT_RECORD_STRAT_PLANES: {
            size_t planeCursor = 0;
            for (size_t plane = 0; ok && plane < fld->size; plane++) {
                uint64_t planeBytes64;
                varintWidth pw =
                    recordReadTagged_(payload + planeCursor,
                                      colBytes - planeCursor, &planeBytes64);
                if (pw == 0 ||
                    planeBytes64 > colBytes - planeCursor - (size_t)pw) {
                    ok = false;
                    break;
                }
                planeCursor += (size_t)pw;
                size_t got = 0;
                if (varintCompeteDecodeChunkedUnsigned(
                        payload + planeCursor, (size_t)planeBytes64, column,
                        recordCount, &got) == 0 ||
                    got != recordCount) {
                    ok = false;
                    break;
                }
                planeCursor += (size_t)planeBytes64;
                for (size_t i = 0; i < recordCount; i++) {
                    base[i * recordSize + fld->offset + plane] =
                        (uint8_t)column[i];
                }
            }
            if (ok && planeCursor != colBytes) {
                ok = false;
            }
            break;
        }
        case VARINT_RECORD_STRAT_DD_STREAM: {
            if (!recordReserve_(&ws->dd0, &ws->dd0Cap,
                                recordCount * sizeof(varintDD)) ||
                varintDDStreamDecode(payload, colBytes, ws->dd0, recordCount) !=
                    recordCount) {
                ok = false;
                break;
            }
            for (size_t i = 0; i < recordCount; i++) {
                memcpy(base + i * recordSize + fld->offset, &ws->dd0[i],
                       sizeof(varintDD));
            }
            break;
        }
        default:
            ok = false;
            break;
        }
    }

    if (!ok) {
        return 0;
    }
    if (decodedCount) {
        *decodedCount = recordCount;
    }
    return cursor;
}

size_t varintRecordDecode(const uint8_t *src, size_t srcBytes, void *records,
                          size_t maxRecords, size_t recordSize,
                          size_t *decodedCount) {
    __attribute__((cleanup(recordCtxCleanup_))) varintRecordCtx ws;
    memset(&ws, 0, sizeof(ws));
    return varintRecordDecodeWithCtx(&ws, src, srcBytes, records, maxRecords,
                                     recordSize, decodedCount);
}

/* ====================================================================
 * Stream walking — sharding support
 * ==================================================================== */

size_t varintRecordStreamBytes(const uint8_t *src, size_t srcBytes) {
    varintRecordHeader hdr;
    size_t cursor = varintRecordReadHeader(src, srcBytes, &hdr);
    if (cursor == 0) {
        return 0;
    }
    /* Schema table: kind + flags + two tagged scalars per field. */
    for (uint64_t f = 0; f < hdr.fieldCount; f++) {
        if (srcBytes - cursor < 2) {
            return 0;
        }
        cursor += 2;
        for (int32_t part = 0; part < 2; part++) {
            uint64_t scratch;
            const varintWidth w =
                recordReadTagged_(src + cursor, srcBytes - cursor, &scratch);
            if (w == 0) {
                return 0;
            }
            cursor += (size_t)w;
        }
    }
    if (hdr.recordCount == 0) {
        return cursor;
    }
    /* Columns: strategy byte + length prefix + skipped payload. */
    for (uint64_t f = 0; f < hdr.fieldCount; f++) {
        if (srcBytes - cursor < 1) {
            return 0;
        }
        cursor += 1;
        uint64_t colBytes;
        const varintWidth w =
            recordReadTagged_(src + cursor, srcBytes - cursor, &colBytes);
        if (w == 0 || colBytes > srcBytes - cursor - (size_t)w) {
            return 0;
        }
        cursor += (size_t)w + (size_t)colBytes;
    }
    return cursor;
}

/* ====================================================================
 * Diagnostics — schema-driven record printing
 * ==================================================================== */

static void recordPrintValue_(FILE *out, const uint8_t *fieldBytes,
                              const varintRecordField *fld) {
    const bool bigEndian = (fld->flags & VARINT_RECORD_FLAG_BIG_ENDIAN);
    switch (fld->kind) {
    case VARINT_RECORD_U8:
    case VARINT_RECORD_U16:
    case VARINT_RECORD_U32:
    case VARINT_RECORD_U64:
    case VARINT_RECORD_BOOL: {
        const uint64_t v = recordLoadField_(fieldBytes, fld->size, bigEndian);
        fprintf(out, "%" PRIu64, v);
        break;
    }
    case VARINT_RECORD_I8:
    case VARINT_RECORD_I16:
    case VARINT_RECORD_I32:
    case VARINT_RECORD_I64: {
        const int64_t v = recordSignExtend_(
            recordLoadField_(fieldBytes, fld->size, bigEndian), fld->size);
        fprintf(out, "%" PRId64, v);
        break;
    }
    case VARINT_RECORD_F32: {
        const uint32_t bits =
            (uint32_t)recordLoadField_(fieldBytes, fld->size, bigEndian);
        float f;
        memcpy(&f, &bits, sizeof(f));
        fprintf(out, "%g", (double)f);
        break;
    }
    case VARINT_RECORD_F64: {
        const uint64_t bits =
            recordLoadField_(fieldBytes, fld->size, bigEndian);
        double d;
        memcpy(&d, &bits, sizeof(d));
        fprintf(out, "%g", d);
        break;
    }
    case VARINT_RECORD_DD: {
        /* printf cannot show a double-double — %g on the hi limb hides
         * the 106-bit precision the type exists for. varintDDToString
         * renders the actual value to ~30 significant digits. */
        varintDD dd;
        memcpy(&dd, fieldBytes, sizeof(dd));
        char ddText[VARINT_DD_STRING_MAX];
        varintDDToString(ddText, sizeof(ddText), dd, 0);
        fprintf(out, "%s", ddText);
        break;
    }
    case VARINT_RECORD_BYTES:
    default: {
        for (size_t b = 0; b < fld->size; b++) {
            fprintf(out, "%02x", fieldBytes[b]);
        }
        break;
    }
    }
}

size_t varintRecordPrintRecords(FILE *out, const void *records,
                                size_t recordCount, size_t recordSize,
                                const varintRecordField *fields,
                                size_t fieldCount,
                                const char *const *fieldNames,
                                size_t firstRecord, size_t maxRecords) {
    if (!out || !records ||
        !varintRecordSchemaValid(recordSize, fields, fieldCount) ||
        firstRecord >= recordCount) {
        return 0;
    }
    size_t n = recordCount - firstRecord;
    if (n > maxRecords) {
        n = maxRecords;
    }

    fprintf(out, "%10s", "#");
    for (size_t f = 0; f < fieldCount; f++) {
        if (fieldNames && fieldNames[f]) {
            fprintf(out, "  %s", fieldNames[f]);
        } else {
            fprintf(out, "  f%zu", f);
        }
        fprintf(out, "(%s)", varintRecordKindName(fields[f].kind));
    }
    fprintf(out, "\n");

    const uint8_t *base = records;
    for (size_t i = 0; i < n; i++) {
        const size_t idx = firstRecord + i;
        fprintf(out, "%10zu", idx);
        for (size_t f = 0; f < fieldCount; f++) {
            fprintf(out, "  ");
            recordPrintValue_(out, base + idx * recordSize + fields[f].offset,
                              &fields[f]);
        }
        fprintf(out, "\n");
    }
    return n;
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_RECORD_TEST
#include "ctest.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static uint64_t testRng_ = UINT64_C(0x1234ABCD5678EF01);
static uint64_t testRand_(void) {
    testRng_ ^= testRng_ << 13;
    testRng_ ^= testRng_ >> 7;
    testRng_ ^= testRng_ << 17;
    return testRng_;
}

/* --------------------------------------------------------------------
 * Per-kind eval matrix: every kind gets a structured dataset, a
 * required minimum compression ratio, and the set of strategies allowed
 * to win it. A generic runner encodes/decodes single-field records and
 * enforces bit-exact round trips plus the quality floor.
 * -------------------------------------------------------------------- */

typedef void (*evalFill_)(uint8_t *field);

typedef struct evalRow {
    const char *name;
    varintRecordFieldKind kind;
    size_t size;
    void (*fill)(uint8_t *base, size_t count, size_t stride);
    double minRatio;        /* rawBytes / encoded column payload */
    uint32_t allowedStrats; /* bitmask over varintRecordStrategy */
} evalRow;

#define STRAT_BIT(s) (UINT32_C(1) << (s))
#define STRAT_ANY 0xFFFFFFFFU

static void fillU64Stride_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        const uint64_t v = 100000 + (uint64_t)i * 60;
        memcpy(base + i * stride, &v, 8);
    }
}

static void fillU64Random_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        const uint64_t v = testRand_();
        memcpy(base + i * stride, &v, 8);
    }
}

static void fillU16Skewed_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        const uint16_t v =
            (testRand_() % 10 < 7) ? 7 : (uint16_t)(testRand_() % 6);
        memcpy(base + i * stride, &v, 2);
    }
}

static void fillU8TinyInts_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        /* Geometric small ints: universal-code / palette territory. */
        uint8_t v = 1;
        while (v < 7 && (testRand_() & 1)) {
            v++;
        }
        base[i * stride] = v;
    }
}

static void fillI16Jitter_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        const int16_t v = (int16_t)((int64_t)(testRand_() % 41) - 20);
        memcpy(base + i * stride, &v, 2);
    }
}

static void fillI64Extremes_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        const int64_t v = (i % 2) ? INT64_MIN + (int64_t)(i % 1000)
                                  : INT64_MAX - (int64_t)(i % 1000);
        memcpy(base + i * stride, &v, 8);
    }
}

static void fillF64Smooth_(uint8_t *base, size_t count, size_t stride) {
    double v = 21.5;
    for (size_t i = 0; i < count; i++) {
        v += ((double)(testRand_() % 100) - 50.0) * 0.001;
        memcpy(base + i * stride, &v, 8);
    }
}

static void fillF64FewDistinct_(uint8_t *base, size_t count, size_t stride) {
    static const double vals[] = {0.0, 0.25, 0.5, 1.0};
    for (size_t i = 0; i < count; i++) {
        const double v = vals[testRand_() % 4];
        memcpy(base + i * stride, &v, 8);
    }
}

static void fillF32Smooth_(uint8_t *base, size_t count, size_t stride) {
    float v = 3.25f;
    for (size_t i = 0; i < count; i++) {
        v += (float)((int64_t)(testRand_() % 9) - 4) * 0.125f;
        memcpy(base + i * stride, &v, 4);
    }
}

static void fillBoolSparse_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        base[i * stride] = (testRand_() % 20 == 0) ? 1 : 0;
    }
}

static void fillBytesStructured_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        /* Per-position structure: constant prefix, tiny alphabet tail. */
        base[i * stride + 0] = 0xA0;
        base[i * stride + 1] = 0xB1;
        base[i * stride + 2] = (uint8_t)(testRand_() % 4);
        base[i * stride + 3] = (uint8_t)(0xC0 + testRand_() % 3);
        base[i * stride + 4] = 0x00;
        base[i * stride + 5] = (uint8_t)(testRand_() % 2);
    }
}

static void fillBytesRandom_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        for (size_t b = 0; b < 6; b++) {
            base[i * stride + b] = (uint8_t)testRand_();
        }
    }
}

static void fillDDPromoted_(uint8_t *base, size_t count, size_t stride) {
    for (size_t i = 0; i < count; i++) {
        /* Values promoted from plain doubles: lo limb exactly zero,
         * the case varintDDStream's zero bitmap targets. */
        varintDD dd;
        dd.hi = 1000.0 + (double)(testRand_() % 5000) * 0.5;
        dd.lo = 0.0;
        memcpy(base + i * stride, &dd, sizeof(dd));
    }
}

static const evalRow evalMatrix_[] = {
    {"U64 stride", VARINT_RECORD_U64, 8, fillU64Stride_, 500.0,
     STRAT_BIT(VARINT_RECORD_STRAT_COMPETE)},
    {"U64 random floor", VARINT_RECORD_U64, 8, fillU64Random_, 0.90, STRAT_ANY},
    {"U16 skewed", VARINT_RECORD_U16, 2, fillU16Skewed_, 4.0,
     STRAT_BIT(VARINT_RECORD_STRAT_COMPETE) |
         STRAT_BIT(VARINT_RECORD_STRAT_PLANES)},
    {"U8 tiny ints", VARINT_RECORD_U8, 1, fillU8TinyInts_, 3.0,
     STRAT_BIT(VARINT_RECORD_STRAT_COMPETE)},
    {"I16 jitter", VARINT_RECORD_I16, 2, fillI16Jitter_, 2.0, STRAT_ANY},
    {"I64 extremes", VARINT_RECORD_I64, 8, fillI64Extremes_, 1.5, STRAT_ANY},
    {"F64 smooth", VARINT_RECORD_F64, 8, fillF64Smooth_, 1.25,
     STRAT_BIT(VARINT_RECORD_STRAT_XOR) | STRAT_BIT(VARINT_RECORD_STRAT_FLOAT) |
         STRAT_BIT(VARINT_RECORD_STRAT_PLANES)},
    {"F64 few distinct", VARINT_RECORD_F64, 8, fillF64FewDistinct_, 4.0,
     STRAT_ANY},
    {"F32 smooth", VARINT_RECORD_F32, 4, fillF32Smooth_, 1.25, STRAT_ANY},
    {"BOOL sparse", VARINT_RECORD_BOOL, 1, fillBoolSparse_, 4.0,
     STRAT_BIT(VARINT_RECORD_STRAT_COMPETE)},
    {"BYTES structured", VARINT_RECORD_BYTES, 6, fillBytesStructured_, 3.0,
     STRAT_BIT(VARINT_RECORD_STRAT_PLANES)},
    {"BYTES random floor", VARINT_RECORD_BYTES, 6, fillBytesRandom_, 0.95,
     STRAT_BIT(VARINT_RECORD_STRAT_VERBATIM) |
         STRAT_BIT(VARINT_RECORD_STRAT_PLANES)},
    {"DD promoted", VARINT_RECORD_DD, 16, fillDDPromoted_, 1.5,
     STRAT_BIT(VARINT_RECORD_STRAT_DD_STREAM)},
};

int varintRecordTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Record eval matrix: every kind hits its ratio + strategy") {
        enum { N = 8192 };
        for (size_t row = 0; row < sizeof(evalMatrix_) / sizeof(evalMatrix_[0]);
             row++) {
            const evalRow *e = &evalMatrix_[row];
            const size_t stride = e->size;
            uint8_t *records = calloc(N, stride);
            e->fill(records, N, stride);

            const varintRecordField field[] = {
                {0, e->size, e->kind, 0},
            };
            const size_t bound =
                varintRecordMaxEncodedSize(N, stride, field, 1);
            if (bound == 0) {
                ERR("[%s] bound rejected valid schema", e->name);
                free(records);
                continue;
            }
            uint8_t *enc = malloc(bound);
            varintRecordMeta meta;
            const size_t written =
                varintRecordEncode(enc, records, N, stride, field, 1, 0, &meta);
            if (written == 0 || written > bound) {
                ERR("[%s] encode failed (%zu of %zu)", e->name, written, bound);
                free(records);
                free(enc);
                continue;
            }

            const double rawBytes = (double)(N * e->size);
            const double ratio = rawBytes / (double)meta.columnBytes[0];
            if (ratio < e->minRatio) {
                ERR("[%s] ratio %.2f below floor %.2f (strategy %s)", e->name,
                    ratio, e->minRatio,
                    varintRecordStrategyName(
                        (varintRecordStrategy)meta.columnStrategy[0]));
            }
            if (!(e->allowedStrats & STRAT_BIT(meta.columnStrategy[0]))) {
                ERR("[%s] unexpected winning strategy %s", e->name,
                    varintRecordStrategyName(
                        (varintRecordStrategy)meta.columnStrategy[0]));
            }

            uint8_t *dec = malloc(N * stride);
            size_t decodedCount = 0;
            const size_t read =
                varintRecordDecode(enc, written, dec, N, stride, &decodedCount);
            if (read != written || decodedCount != N ||
                memcmp(dec, records, N * stride) != 0) {
                ERR("[%s] round trip failed", e->name);
            }
            free(records);
            free(enc);
            free(dec);
        }
    }

    TEST("Record: float columns with NaN/Inf/-0/subnormal are bit-exact") {
        enum { N = 4096 };
        double *vals = malloc(N * sizeof(double));
        for (size_t i = 0; i < N; i++) {
            switch (i % 6) {
            case 0:
                vals[i] = NAN;
                break;
            case 1:
                vals[i] = INFINITY;
                break;
            case 2:
                vals[i] = -0.0;
                break;
            case 3:
                vals[i] = 5e-324; /* smallest subnormal */
                break;
            case 4:
                vals[i] = -INFINITY;
                break;
            default:
                vals[i] = (double)i * 0.5;
                break;
            }
        }
        const varintRecordField field[] = {{0, 8, VARINT_RECORD_F64, 0}};
        uint8_t *enc = malloc(varintRecordMaxEncodedSize(N, 8, field, 1));
        varintRecordMeta meta;
        const size_t written =
            varintRecordEncode(enc, vals, N, 8, field, 1, 0, &meta);
        if (written == 0) {
            ERRR("special-value float encode failed");
        }
        double *dec = malloc(N * sizeof(double));
        const size_t read = varintRecordDecode(enc, written, dec, N, 8, NULL);
        if (read != written || memcmp(dec, vals, N * sizeof(double)) != 0) {
            ERR("special-value round trip not bit-exact (strategy %s)",
                varintRecordStrategyName(
                    (varintRecordStrategy)meta.columnStrategy[0]));
        }
        free(vals);
        free(enc);
        free(dec);
    }

    TEST("Record: multi-kind struct with padding, BE wire field, meta") {
        typedef struct wide {
            uint64_t ts;
            double reading;
            int32_t deltaBE;
            uint8_t status;
            uint8_t tag[6];
            uint8_t pad_[5];
        } wide;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(wide, ts, VARINT_RECORD_U64),
            VARINT_RECORD_FIELD(wide, reading, VARINT_RECORD_F64),
            VARINT_RECORD_FIELD_BE(wide, deltaBE, VARINT_RECORD_I32),
            VARINT_RECORD_FIELD(wide, status, VARINT_RECORD_BOOL),
            {offsetof(wide, tag), 6, VARINT_RECORD_BYTES, 0},
        };
        enum { N = 6000 };
        wide *rows = calloc(N, sizeof(*rows));
        double r = 100.0;
        for (size_t i = 0; i < N; i++) {
            rows[i].ts = UINT64_C(1700000000) + i * 5;
            r += ((double)(testRand_() % 21) - 10.0) * 0.01;
            rows[i].reading = r;
            const int32_t d = (int32_t)((int64_t)(testRand_() % 2001) - 1000);
            uint8_t *b = (uint8_t *)&rows[i].deltaBE;
            b[0] = (uint8_t)((uint32_t)d >> 24);
            b[1] = (uint8_t)((uint32_t)d >> 16);
            b[2] = (uint8_t)((uint32_t)d >> 8);
            b[3] = (uint8_t)(uint32_t)d;
            rows[i].status = (testRand_() % 16 == 0);
            memset(rows[i].tag, 0xAB, 5);
            rows[i].tag[5] = (uint8_t)(testRand_() % 3);
        }

        uint8_t *enc =
            malloc(varintRecordMaxEncodedSize(N, sizeof(wide), schema, 5));
        varintRecordMeta meta;
        const size_t written =
            varintRecordEncode(enc, rows, N, sizeof(wide), schema, 5, 0, &meta);
        if (written == 0) {
            ERRR("multi-kind encode failed");
        }
        if (written * 2 > N * sizeof(wide)) {
            ERR("expected >=2x on structured struct, got %zu of %zu", written,
                N * sizeof(wide));
        }
        for (size_t f = 0; f < 5; f++) {
            if (meta.columnBytes[f] == 0) {
                ERR("meta missing column %zu", f);
            }
        }

        wide *dec = malloc(N * sizeof(*dec));
        memset(dec, 0x5A, N * sizeof(*dec));
        size_t decodedCount = 0;
        const size_t read = varintRecordDecode(enc, written, dec, N,
                                               sizeof(wide), &decodedCount);
        if (read != written || decodedCount != N ||
            memcmp(dec, rows, N * sizeof(wide)) != 0) {
            ERRR("multi-kind round trip failed");
        }
        free(rows);
        free(enc);
        free(dec);
    }

    TEST("Record: BOOL contract violation is rejected at encode") {
        uint8_t vals[64];
        for (size_t i = 0; i < 64; i++) {
            vals[i] = (uint8_t)i; /* includes values > 1 */
        }
        const varintRecordField field[] = {{0, 1, VARINT_RECORD_BOOL, 0}};
        uint8_t buf[4096];
        if (varintRecordEncode(buf, vals, 64, 1, field, 1, 0, NULL) != 0) {
            ERRR("BOOL field with non-0/1 values accepted");
        }
    }

    TEST("Record: identical records collapse across kinds") {
        typedef struct rec {
            uint64_t a;
            double d;
            uint32_t b;
        } rec;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(rec, a, VARINT_RECORD_U64),
            VARINT_RECORD_FIELD(rec, d, VARINT_RECORD_F64),
            VARINT_RECORD_FIELD(rec, b, VARINT_RECORD_U32),
        };
        enum { N = 100000 };
        rec *rows = calloc(N, sizeof(*rows));
        for (size_t i = 0; i < N; i++) {
            rows[i].a = 42;
            rows[i].d = 3.14159;
            rows[i].b = 7;
        }
        uint8_t *enc =
            malloc(varintRecordMaxEncodedSize(N, sizeof(rec), schema, 3));
        const size_t written =
            varintRecordEncode(enc, rows, N, sizeof(rec), schema, 3, 0, NULL);
        if (written == 0 || written > 512) {
            ERR("constant struct should collapse, got %zu bytes", written);
        }
        rec *dec = malloc(N * sizeof(*dec));
        const size_t read =
            varintRecordDecode(enc, written, dec, N, sizeof(rec), NULL);
        if (read != written || memcmp(dec, rows, N * sizeof(rec)) != 0) {
            ERRR("constant round trip failed");
        }
        free(rows);
        free(enc);
        free(dec);
    }

    TEST("Record: empty and single-record streams") {
        typedef struct one {
            uint32_t x;
        } one;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(one, x, VARINT_RECORD_U32),
        };
        uint8_t buf[256];
        size_t written =
            varintRecordEncode(buf, NULL, 0, sizeof(one), schema, 1, 0, NULL);
        if (written == 0) {
            ERRR("empty stream should still emit header + schema");
        }
        one sink;
        size_t decodedCount = 99;
        size_t read = varintRecordDecode(buf, written, &sink, 0, sizeof(one),
                                         &decodedCount);
        if (read != written || decodedCount != 0) {
            ERRR("empty round trip failed");
        }

        one v = {UINT32_C(0xDEADBEEF)};
        written =
            varintRecordEncode(buf, &v, 1, sizeof(one), schema, 1, 0, NULL);
        one got = {0};
        read = varintRecordDecode(buf, written, &got, 1, sizeof(one),
                                  &decodedCount);
        if (read != written || decodedCount != 1 || got.x != v.x) {
            ERRR("single-record round trip failed");
        }
    }

    TEST("Record: invalid schemas are rejected") {
        typedef struct s {
            uint32_t a;
            uint32_t b;
        } s;
        uint8_t buf[512];
        s rows[2] = {{1, 2}, {3, 4}};

        varintRecordField overlap[] = {
            {0, 4, VARINT_RECORD_U32, 0},
            {2, 4, VARINT_RECORD_U32, 0},
        };
        if (varintRecordEncode(buf, rows, 2, sizeof(s), overlap, 2, 0, NULL)) {
            ERRR("overlapping fields accepted");
        }

        varintRecordField oob[] = {{8, 4, VARINT_RECORD_U32, 0}};
        if (varintRecordEncode(buf, rows, 2, sizeof(s), oob, 1, 0, NULL)) {
            ERRR("out-of-bounds field accepted");
        }

        varintRecordField badWidth[] = {{0, 2, VARINT_RECORD_U32, 0}};
        if (varintRecordEncode(buf, rows, 2, sizeof(s), badWidth, 1, 0, NULL)) {
            ERRR("kind/size mismatch accepted");
        }

        varintRecordField badDD[] = {{0, 8, VARINT_RECORD_DD, 0}};
        if (varintRecordEncode(buf, rows, 2, sizeof(s), badDD, 1, 0, NULL)) {
            ERRR("DD field with wrong size accepted");
        }

        if (varintRecordEncode(buf, rows, 2, sizeof(s), NULL, 0, 0, NULL)) {
            ERRR("empty schema accepted");
        }
    }

    TEST("Record: hostile input is rejected, never overruns") {
        typedef struct s {
            uint64_t a;
            double d;
            uint8_t b;
        } s;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(s, a, VARINT_RECORD_U64),
            VARINT_RECORD_FIELD(s, d, VARINT_RECORD_F64),
            VARINT_RECORD_FIELD(s, b, VARINT_RECORD_U8),
        };
        enum { N = 500 };
        s rows[N];
        memset(rows, 0, sizeof(rows));
        double d = 1.5;
        for (size_t i = 0; i < N; i++) {
            rows[i].a = i * 3;
            d += 0.25;
            rows[i].d = d;
            rows[i].b = (uint8_t)(i % 7);
        }
        uint8_t *enc =
            malloc(varintRecordMaxEncodedSize(N, sizeof(s), schema, 3));
        size_t written =
            varintRecordEncode(enc, rows, N, sizeof(s), schema, 3, 0, NULL);
        s dec[N];

        for (size_t cut = 1; cut <= written; cut++) {
            if (varintRecordDecode(enc, written - cut, dec, N, sizeof(s),
                                   NULL) != 0) {
                ERR("truncated stream (-%zu) accepted", cut);
                break;
            }
        }
        if (varintRecordDecode(enc, written, dec, N - 1, sizeof(s), NULL)) {
            ERRR("undersized output accepted");
        }
        if (varintRecordDecode(enc, written, dec, N, sizeof(s) + 8, NULL)) {
            ERRR("record-size mismatch accepted");
        }

        uint8_t save = enc[4];
        enc[4] = 99;
        if (varintRecordDecode(enc, written, dec, N, sizeof(s), NULL)) {
            ERRR("bad version accepted");
        }
        enc[4] = save;
        save = enc[0];
        enc[0] = 'X';
        if (varintRecordDecode(enc, written, dec, N, sizeof(s), NULL)) {
            ERRR("bad magic accepted");
        }
        enc[0] = save;
        free(enc);
    }

    TEST("Record: concatenated shards walk with StreamBytes") {
        typedef struct s {
            uint64_t a;
            uint8_t b;
        } s;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(s, a, VARINT_RECORD_U64),
            VARINT_RECORD_FIELD(s, b, VARINT_RECORD_U8),
        };
        enum { SHARD = 700, SHARDS = 3 };
        s rows[SHARD * SHARDS];
        memset(rows, 0, sizeof(rows));
        for (size_t i = 0; i < SHARD * SHARDS; i++) {
            rows[i].a = i * 7;
            rows[i].b = (uint8_t)(i % 3);
        }

        /* Encode three shards back to back into one buffer. */
        const size_t bound =
            varintRecordMaxEncodedSize(SHARD, sizeof(s), schema, 2);
        uint8_t *stream = malloc(bound * SHARDS);
        size_t total = 0;
        size_t shardSizes[SHARDS];
        for (size_t sh = 0; sh < SHARDS; sh++) {
            shardSizes[sh] =
                varintRecordEncode(stream + total, rows + sh * SHARD, SHARD,
                                   sizeof(s), schema, 2, 0, NULL);
            if (shardSizes[sh] == 0) {
                ERRR("shard encode failed");
            }
            total += shardSizes[sh];
        }

        /* Walk without decoding, then decode each shard independently. */
        s dec[SHARD * SHARDS];
        size_t cursor = 0;
        size_t got = 0;
        while (cursor < total) {
            const size_t bytes =
                varintRecordStreamBytes(stream + cursor, total - cursor);
            if (bytes == 0 || bytes != shardSizes[got / SHARD]) {
                ERR("walk mismatch at shard %zu: %zu vs %zu", got / SHARD,
                    bytes, shardSizes[got / SHARD]);
                break;
            }
            size_t decodedCount = 0;
            if (varintRecordDecode(stream + cursor, bytes, dec + got, SHARD,
                                   sizeof(s), &decodedCount) != bytes ||
                decodedCount != SHARD) {
                ERRR("shard decode failed");
                break;
            }
            cursor += bytes;
            got += decodedCount;
        }
        if (got != SHARD * SHARDS || memcmp(dec, rows, sizeof(rows)) != 0) {
            ERRR("sharded round trip incomplete");
        }

        /* Truncated walks must fail, not lie. */
        if (varintRecordStreamBytes(stream, shardSizes[0] - 1) != 0) {
            ERRR("truncated stream walk reported a size");
        }
        free(stream);
    }

    TEST("Record: schema-driven printer paginates and formats kinds") {
        typedef struct row {
            int32_t temp;
            uint8_t on;
            uint8_t id[2];
            varintDD dd;
        } row;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(row, temp, VARINT_RECORD_I32),
            VARINT_RECORD_FIELD(row, on, VARINT_RECORD_BOOL),
            {offsetof(row, id), 2, VARINT_RECORD_BYTES, 0},
            VARINT_RECORD_FIELD(row, dd, VARINT_RECORD_DD),
        };
        static const char *names[] = {"temp", "on", "id", "dd"};
        enum { N = 100 };
        row rows[N];
        memset(rows, 0, sizeof(rows));
        for (size_t i = 0; i < N; i++) {
            rows[i].temp = (int32_t)i - 50;
            rows[i].on = (i % 2);
            rows[i].id[0] = 0xAB;
            rows[i].id[1] = (uint8_t)i;
            /* A value whose lo limb carries real precision: hi + tiny. */
            rows[i].dd = varintDDTwoSum(1.0, 1e-25);
        }

        /* The DD column must render through varintDDToString: beyond
         * double precision, so the tiny limb must appear in the text. */
        {
            char oneRow[512];
            FILE *cap = tmpfile();
            if (cap) {
                varintRecordPrintRecords(cap, rows, N, sizeof(row), schema, 4,
                                         names, 0, 1);
                rewind(cap);
                size_t got = fread(oneRow, 1, sizeof(oneRow) - 1, cap);
                oneRow[got] = '\0';
                if (!strstr(oneRow, "1.0000000000000000000000001")) {
                    ERR("DD column lost sub-double precision: %s", oneRow);
                }
                fclose(cap);
            }
        }

        FILE *sink = tmpfile();
        if (!sink) {
            ERRR("tmpfile failed");
        } else {
            /* Middle page: records 40..49. */
            size_t printed = varintRecordPrintRecords(
                sink, rows, N, sizeof(row), schema, 4, names, 40, 10);
            if (printed != 10) {
                ERR("expected 10 printed, got %zu", printed);
            }
            /* Window clipped at the end. */
            printed = varintRecordPrintRecords(sink, rows, N, sizeof(row),
                                               schema, 4, NULL, 95, 10);
            if (printed != 5) {
                ERR("expected clipped 5 printed, got %zu", printed);
            }
            /* Window past the end prints nothing. */
            if (varintRecordPrintRecords(sink, rows, N, sizeof(row), schema, 4,
                                         names, N, 10) != 0) {
                ERRR("out-of-range window printed rows");
            }
            if (ftell(sink) <= 0) {
                ERRR("printer produced no output");
            }
            fclose(sink);
        }
    }

    TEST("Record: one context serves mixed shapes, output identical") {
        varintRecordCtx *ws = varintRecordCtxNew();
        if (!ws) {
            ERRR("ctx alloc failed");
        }

        /* Shape 1: wide multi-kind records (grows every buffer). */
        typedef struct big {
            uint64_t a;
            double d;
            varintDD dd;
            uint8_t tag[4];
        } big;
        static const varintRecordField bigSchema[] = {
            VARINT_RECORD_FIELD(big, a, VARINT_RECORD_U64),
            VARINT_RECORD_FIELD(big, d, VARINT_RECORD_F64),
            VARINT_RECORD_FIELD(big, dd, VARINT_RECORD_DD),
            {offsetof(big, tag), 4, VARINT_RECORD_BYTES, 0},
        };
        enum { NB = 3000, NS = 500 };
        big *rows = calloc(NB, sizeof(*rows));
        double dv = 5.0;
        for (size_t i = 0; i < NB; i++) {
            rows[i].a = i * 11;
            dv += 0.125;
            rows[i].d = dv;
            rows[i].dd = varintDDTwoSum((double)i, 1e-20);
            memset(rows[i].tag, (int32_t)(i % 3), 4);
        }
        uint8_t *encPlain =
            malloc(varintRecordMaxEncodedSize(NB, sizeof(big), bigSchema, 4));
        uint8_t *encCtx =
            malloc(varintRecordMaxEncodedSize(NB, sizeof(big), bigSchema, 4));
        const size_t wPlain = varintRecordEncode(
            encPlain, rows, NB, sizeof(big), bigSchema, 4, 0, NULL);
        const size_t wCtx = varintRecordEncodeWithCtx(
            ws, encCtx, rows, NB, sizeof(big), bigSchema, 4, 0, NULL);
        if (wPlain == 0 || wPlain != wCtx ||
            memcmp(encPlain, encCtx, wPlain) != 0) {
            ERRR("ctx encode diverged from plain encode");
        }

        /* Shape 2: a smaller, different schema on the same context —
         * reuse must not leak state between shapes. */
        typedef struct small {
            int16_t v;
        } small;
        static const varintRecordField smallSchema[] = {
            VARINT_RECORD_FIELD(small, v, VARINT_RECORD_I16),
        };
        small srows[NS];
        for (size_t i = 0; i < NS; i++) {
            srows[i].v = (int16_t)((int64_t)i - 250);
        }
        uint8_t sbuf[8192];
        const size_t sw = varintRecordEncodeWithCtx(
            ws, sbuf, srows, NS, sizeof(small), smallSchema, 1, 0, NULL);
        small sdec[NS];
        size_t got = 0;
        if (sw == 0 ||
            varintRecordDecodeWithCtx(ws, sbuf, sw, sdec, NS, sizeof(small),
                                      &got) != sw ||
            got != NS || memcmp(sdec, srows, sizeof(srows)) != 0) {
            ERRR("ctx reuse across shapes failed");
        }

        /* Back to shape 1 for decode, same context. */
        big *dec = malloc(NB * sizeof(*dec));
        if (varintRecordDecodeWithCtx(ws, encCtx, wCtx, dec, NB, sizeof(big),
                                      &got) != wCtx ||
            got != NB || memcmp(dec, rows, NB * sizeof(big)) != 0) {
            ERRR("ctx decode after reuse failed");
        }

        varintRecordCtxFree(ws);
        free(rows);
        free(encPlain);
        free(encCtx);
        free(dec);
    }

    TEST("Compete: typed scratch is validated before use") {
        enum { N = 2048 };
        static uint64_t values[N];
        for (size_t i = 0; i < N; i++) {
            values[i] = 100 + i;
        }
        uint8_t *dst = malloc(varintCompeteMaxEncodedSizeChunked(N, 0));
        const size_t need = varintCompeteChunkedScratchBytes(0);
        uint8_t *mem = malloc(need);
        varintCompeteChunkedScratch scratch;

        if (varintCompeteChunkedScratchInit(&scratch, mem, need - 1, 0)) {
            ERRR("undersized scratch memory accepted by Init");
        }
        if (!varintCompeteChunkedScratchInit(&scratch, mem, need, 0)) {
            ERRR("correctly sized scratch rejected");
        }

        const size_t plain =
            varintCompeteEncodeChunkedUnsigned(dst, values, N, 0, 0, NULL);
        const size_t withScratch = varintCompeteEncodeChunkedUnsignedScratch(
            dst, values, N, 0, 0, NULL, &scratch);
        if (plain == 0 || plain != withScratch) {
            ERRR("scratch encode diverged from plain encode");
        }

        /* A scratch bound for one block target must be rejected when
         * applied to a different one, and an unstamped scratch must be
         * rejected outright. */
        if (varintCompeteEncodeChunkedUnsignedScratch(dst, values, N, 0, 128,
                                                      NULL, &scratch) != 0) {
            ERRR("scratch applied across block targets accepted");
        }
        scratch.magic = 0;
        if (varintCompeteEncodeChunkedUnsignedScratch(dst, values, N, 0, 0,
                                                      NULL, &scratch) != 0) {
            ERRR("unstamped scratch accepted");
        }
        free(dst);
        free(mem);
    }

    TEST("Record: header is self-describing") {
        typedef struct s {
            uint16_t v;
        } s;
        static const varintRecordField schema[] = {
            VARINT_RECORD_FIELD(s, v, VARINT_RECORD_U16),
        };
        s rows[10];
        for (size_t i = 0; i < 10; i++) {
            rows[i].v = (uint16_t)(i * 11);
        }
        uint8_t buf[1024];
        const size_t written =
            varintRecordEncode(buf, rows, 10, sizeof(s), schema, 1, 0, NULL);
        varintRecordHeader hdr;
        if (varintRecordReadHeader(buf, written, &hdr) == 0 ||
            hdr.recordCount != 10 || hdr.recordSize != sizeof(s) ||
            hdr.fieldCount != 1) {
            ERR("header parse wrong: count=%" PRIu64 " size=%" PRIu64
                " fields=%" PRIu64,
                hdr.recordCount, hdr.recordSize, hdr.fieldCount);
        }
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_RECORD_TEST */
