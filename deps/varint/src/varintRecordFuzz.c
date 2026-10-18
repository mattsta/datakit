/* ====================================================================
 * varintRecordFuzz — self-contained deterministic fuzzer (no libFuzzer)
 * ====================================================================
 * Three properties, checked every iteration under a seeded PRNG:
 *   1. Round trip: random schema + random records encode then decode to
 *      byte-identical records (uncovered bytes zeroed on both sides).
 *   2. Idempotency: re-encoding the decoded records reproduces the
 *      original stream exactly.
 *   3. Structural robustness: any truncation, and any byte corruption
 *      in the header/schema region, is rejected or decodes bounded —
 *      run under ASan/UBSan (the sanitizer test harness) to enforce
 *      the memory-safety half. Column payload bytes are trusted at the
 *      codec layer per the library's trust model (see Trust Model in
 *      docs/ARCHITECTURE.md): containers validate structure, codecs
 *      decode the bytes their encoders produced.
 *
 * Usage: varintRecordFuzz [iterations] [seed]
 */

#include "varintRecord.h"
#include "varintTagged.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rngState_;
static uint64_t rng_(void) {
    rngState_ ^= rngState_ << 13;
    rngState_ ^= rngState_ >> 7;
    rngState_ ^= rngState_ << 17;
    return rngState_;
}

/* Build a random packed schema: fields laid out sequentially with
 * occasional uncovered gaps. Returns the record size. */
static size_t randomSchema_(varintRecordField *fields, size_t *fieldCount) {
    const size_t n = 1 + rng_() % 8;
    size_t offset = 0;
    for (size_t f = 0; f < n; f++) {
        const varintRecordFieldKind kind =
            (varintRecordFieldKind)(rng_() % VARINT_RECORD_KIND_MAX);
        size_t size;
        switch (kind) {
        case VARINT_RECORD_U8:
        case VARINT_RECORD_I8:
        case VARINT_RECORD_BOOL:
            size = 1;
            break;
        case VARINT_RECORD_U16:
        case VARINT_RECORD_I16:
            size = 2;
            break;
        case VARINT_RECORD_U32:
        case VARINT_RECORD_I32:
        case VARINT_RECORD_F32:
            size = 4;
            break;
        case VARINT_RECORD_BYTES:
            size = 1 + rng_() % 6;
            break;
        case VARINT_RECORD_DD:
            size = 16;
            break;
        default:
            size = 8;
            break;
        }
        /* Occasionally leave an uncovered gap before this field. */
        if (rng_() % 4 == 0) {
            offset += rng_() % 3;
        }
        fields[f].offset = offset;
        fields[f].size = size;
        fields[f].kind = kind;
        fields[f].flags = (rng_() % 3 == 0 && kind != VARINT_RECORD_BYTES &&
                           kind != VARINT_RECORD_DD)
                              ? VARINT_RECORD_FLAG_BIG_ENDIAN
                              : 0;
        offset += size;
    }
    *fieldCount = n;
    /* Occasionally add trailing uncovered padding. */
    return offset + rng_() % 4;
}

/* Fill records with per-field patterns that exercise different codecs:
 * constants, strides, small alphabets, and raw noise. */
static void randomRecords_(uint8_t *records, size_t recordCount,
                           size_t recordSize, const varintRecordField *fields,
                           size_t fieldCount) {
    memset(records, 0, recordCount * recordSize);
    for (size_t f = 0; f < fieldCount; f++) {
        const uint64_t style = rng_() % 4;
        const uint64_t base = rng_();
        const uint64_t step = rng_() % 1000;
        for (size_t i = 0; i < recordCount; i++) {
            uint8_t *p = records + i * recordSize + fields[f].offset;
            uint64_t v;
            switch (style) {
            case 0:
                v = base;
                break;
            case 1:
                v = base + i * step;
                break;
            case 2:
                v = base + rng_() % 5;
                break;
            default:
                v = rng_();
                break;
            }
            if (fields[f].kind == VARINT_RECORD_BOOL) {
                /* BOOL fields carry a validated 0/1 contract. */
                v &= 1;
            }
            for (size_t b = 0; b < fields[f].size; b++) {
                p[b] = (uint8_t)(v >> (8 * (b % 8)));
            }
        }
    }
}

int main(int argc, char *argv[]) {
    const uint64_t iterations = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20000;
    const uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 0) : 24151;
    rngState_ = seed ? seed : 1;

    /* One persistent workspace across every iteration stresses the
     * grow-only reuse path over random shapes and sizes. */
    varintRecordCtx *ws = varintRecordCtxNew();
    if (!ws) {
        fprintf(stderr, "ctx allocation failed\n");
        return 1;
    }

    for (uint64_t iter = 0; iter < iterations; iter++) {
        varintRecordField fields[VARINT_RECORD_MAX_FIELDS];
        size_t fieldCount = 0;
        const size_t recordSize = randomSchema_(fields, &fieldCount);
        const size_t recordCount = rng_() % 600;

        uint8_t *records = malloc(recordCount * recordSize + 1);
        randomRecords_(records, recordCount, recordSize, fields, fieldCount);

        const size_t bound = varintRecordMaxEncodedSize(recordCount, recordSize,
                                                        fields, fieldCount);
        if (bound == 0) {
            fprintf(stderr, "iter %" PRIu64 ": generated schema rejected\n",
                    iter);
            return 1;
        }
        uint8_t *enc = malloc(bound);
        const size_t written =
            varintRecordEncodeWithCtx(ws, enc, records, recordCount, recordSize,
                                      fields, fieldCount, 0, NULL);
        if (written == 0 || written > bound) {
            fprintf(stderr, "iter %" PRIu64 ": encode failed (%zu of %zu)\n",
                    iter, written, bound);
            return 1;
        }

        uint8_t *dec = malloc(recordCount * recordSize + 1);
        size_t decodedCount = 0;
        const size_t read = varintRecordDecodeWithCtx(
            ws, enc, written, dec, recordCount, recordSize, &decodedCount);
        if (read != written || decodedCount != recordCount ||
            (recordCount > 0 &&
             memcmp(dec, records, recordCount * recordSize) != 0)) {
            fprintf(stderr, "iter %" PRIu64 ": round trip failed\n", iter);
            return 1;
        }

        /* Idempotency, via the plain API so the context path and the
         * per-call path continuously cross-check each other: decoded
         * records must re-encode to the same bytes. */
        uint8_t *enc2 = malloc(bound);
        const size_t written2 = varintRecordEncode(
            enc2, dec, recordCount, recordSize, fields, fieldCount, 0, NULL);
        if (written2 != written || memcmp(enc2, enc, written) != 0) {
            fprintf(stderr, "iter %" PRIu64 ": re-encode not idempotent\n",
                    iter);
            return 1;
        }

        /* Structural robustness: flip a byte within the header + schema
         * region (everything before the first column payload), and
         * truncate at a random point; decode must stay bounded
         * (sanitizers verify the memory-safety half). */
        if (written > 0) {
            varintRecordHeader hdr;
            size_t structuralEnd = varintRecordReadHeader(enc, written, &hdr);
            for (uint64_t f = 0; f < hdr.fieldCount; f++) {
                structuralEnd += 2; /* kind + flags */
                uint64_t scratch;
                structuralEnd +=
                    (size_t)varintTaggedGet(enc + structuralEnd, 9, &scratch);
                structuralEnd +=
                    (size_t)varintTaggedGet(enc + structuralEnd, 9, &scratch);
            }
            const size_t pos = rng_() % structuralEnd;
            enc[pos] ^= (uint8_t)(1 + rng_() % 255);
            (void)varintRecordDecode(enc, written, dec, recordCount, recordSize,
                                     &decodedCount);
            enc[pos] = enc2[pos];
            /* The first column's strategy byte sits right after the
             * schema — flip it too (decode validates strategy/kind). */
            if (structuralEnd < written) {
                enc[structuralEnd] ^= (uint8_t)(1 + rng_() % 255);
                (void)varintRecordDecode(enc, written, dec, recordCount,
                                         recordSize, &decodedCount);
                enc[structuralEnd] = enc2[structuralEnd];
            }
            (void)varintRecordDecode(enc, rng_() % written, dec, recordCount,
                                     recordSize, &decodedCount);
        }

        free(records);
        free(enc);
        free(enc2);
        free(dec);
    }

    varintRecordCtxFree(ws);
    printf("varintRecordFuzz: %" PRIu64 " iterations OK (seed %" PRIu64 ")\n",
           iterations, seed);
    return 0;
}
