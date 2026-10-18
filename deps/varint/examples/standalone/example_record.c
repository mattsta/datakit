/* ====================================================================
 * example_record.c — Schema-Driven Columnar Record Compression
 * ====================================================================
 * Demonstrates varintRecord end to end:
 *   1. Describe a C struct's layout with a field-descriptor schema
 *   2. Encode an array of records — each field becomes a column and
 *      competes across the whole codec family automatically
 *   3. Inspect the per-column results (which fields cost what)
 *   4. Decode with ZERO external metadata — the schema is in the stream
 *   5. Compare against row-oriented encoding to see why columns win
 *
 * Build:
 *   cc -O2 -I../../src example_record.c <varint sources> -o example_record
 * (or use the CMake target: make example_record)
 */

#include "varintDD.h"
#include "varintRecord.h"
#include "varintTagged.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------
 * The application's record type: an IoT sensor reading as it might
 * arrive from a device fleet. Note the mix: a regular-cadence
 * timestamp, a small device population, an enum, a signed jittery
 * measurement, a raw MAC tag, and struct padding the schema ignores.
 * -------------------------------------------------------------------- */
typedef struct SensorReading {
    uint64_t timestampMs; /* arrives every ~250ms — stride/delta gold */
    uint32_t deviceId;    /* 40 devices — dictionary/palette gold */
    int16_t temperature;  /* tenths of a degree, drifts slowly, signed */
    uint8_t status;       /* enum {OK, WARN, FAIL} — palette gold */
    uint8_t mac[6];       /* structured hardware tag — byte-plane gold */
    float voltage;        /* smooth analog sample — XOR/FLOAT lanes */
    varintDD precise;     /* 106-bit accumulated dose — DD_STREAM lane */
    uint8_t pad_[4];      /* struct padding: not described, not stored */
} SensorReading;

/* The schema: one line per field, layout captured at compile time by
 * offsetof/sizeof. This is the whole "data description language". */
static const varintRecordField sensorSchema[] = {
    VARINT_RECORD_FIELD(SensorReading, timestampMs, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(SensorReading, deviceId, VARINT_RECORD_U32),
    VARINT_RECORD_FIELD(SensorReading, temperature, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(SensorReading, status, VARINT_RECORD_U8),
    {offsetof(SensorReading, mac), 6, VARINT_RECORD_BYTES, 0},
    VARINT_RECORD_FIELD(SensorReading, voltage, VARINT_RECORD_F32),
    VARINT_RECORD_FIELD(SensorReading, precise, VARINT_RECORD_DD),
};
static const char *sensorFieldNames[] = {
    "timestampMs", "deviceId", "temperature", "status",
    "mac",         "voltage",  "precise"};
#define SENSOR_FIELDS (sizeof(sensorSchema) / sizeof(sensorSchema[0]))

static uint64_t rngState = UINT64_C(0x9E3779B97F4A7C15);
static uint64_t rng(void) {
    rngState = rngState * UINT64_C(6364136223846793005) +
               UINT64_C(1442695040888963407);
    return rngState >> 16;
}

static void fillReadings(SensorReading *rows, size_t count) {
    int16_t temp = 215; /* 21.5 C */
    for (size_t i = 0; i < count; i++) {
        rows[i].timestampMs = UINT64_C(1700000000000) + i * 250;
        rows[i].deviceId = 9000 + (uint32_t)(rng() % 40);
        temp = (int16_t)(temp + (int16_t)(rng() % 5) - 2);
        rows[i].temperature = temp;
        rows[i].status = (rng() % 50 == 0) ? 2 : (rng() % 10 == 0) ? 1 : 0;
        /* Vendor OUI prefix is fleet-constant; the low bytes derive
         * from the device population. */
        rows[i].mac[0] = 0xA4;
        rows[i].mac[1] = 0x83;
        rows[i].mac[2] = 0xE7;
        rows[i].mac[3] = 0x10;
        rows[i].mac[4] = (uint8_t)(rows[i].deviceId >> 5);
        rows[i].mac[5] = (uint8_t)(0xA0 + (rows[i].deviceId % 40));
        rows[i].voltage = 3.30f + (float)((int64_t)(rng() % 9) - 4) * 0.005f;
        rows[i].precise.hi = 0.125 * (double)(rng() % 100000);
        rows[i].precise.lo = 0.0;
        memset(rows[i].pad_, 0, sizeof(rows[i].pad_));
    }
}

/* Row-oriented baseline: every field of every record tagged in place —
 * the natural "just varint everything" approach varintRecord replaces. */
static size_t encodeRowOriented(uint8_t *dst, const SensorReading *rows,
                                size_t count) {
    uint8_t *p = dst;
    for (size_t i = 0; i < count; i++) {
        p += varintTaggedPut64(p, rows[i].timestampMs);
        p += varintTaggedPut64(p, rows[i].deviceId);
        p += varintTaggedPut64(p, (uint64_t)(int64_t)rows[i].temperature);
        p += varintTaggedPut64(p, rows[i].status);
        memcpy(p, rows[i].mac, 6);
        p += 6;
        memcpy(p, &rows[i].voltage, 4);
        p += 4;
        memcpy(p, &rows[i].precise, 16);
        p += 16;
    }
    return (size_t)(p - dst);
}

/* Shared reporter: per-column strategy + ratio table from encode meta. */
static void printColumns(const varintRecordField *schema,
                         const char *const *names, size_t fieldCount,
                         size_t count, const varintRecordMeta *meta) {
    for (size_t f = 0; f < fieldCount; f++) {
        const size_t colRaw = count * schema[f].size;
        printf("   %-12s %-5s %9zu B raw -> %8zu B (%8.2fx) via %s\n", names[f],
               varintRecordKindName(schema[f].kind), colRaw,
               meta->columnBytes[f],
               (double)colRaw / (double)meta->columnBytes[f],
               varintRecordStrategyName(
                   (varintRecordStrategy)meta->columnStrategy[f]));
    }
}

/* Shared driver: encode, report, decode, verify byte-identity. */
static int runScenario(const char *title, const void *rows, size_t count,
                       size_t recordSize, const varintRecordField *schema,
                       const char *const *names, size_t fieldCount) {
    printf("\n===========================================\n");
    printf("   %s\n", title);
    printf("===========================================\n");
    const size_t rawBytes = count * recordSize;
    printf("Dataset: %zu records x %zu bytes = %.2f MB raw\n", count,
           recordSize, (double)rawBytes / (1024.0 * 1024.0));

    uint8_t *enc = malloc(
        varintRecordMaxEncodedSize(count, recordSize, schema, fieldCount));
    varintRecordMeta meta;
    const size_t written = varintRecordEncode(enc, rows, count, recordSize,
                                              schema, fieldCount, 0, &meta);
    if (written == 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("Encoded: %zu bytes (%.2fx compression)\n", written,
           (double)rawBytes / (double)written);
    printColumns(schema, names, fieldCount, count, &meta);

    uint8_t *dec = malloc(rawBytes);
    size_t decodedCount = 0;
    const size_t read =
        varintRecordDecode(enc, written, dec, count, recordSize, &decodedCount);
    if (read != written || decodedCount != count ||
        memcmp(dec, rows, rawBytes) != 0) {
        fprintf(stderr, "decode round trip failed\n");
        return 1;
    }
    printf("Round trip: %zu records, byte-identical ✓\n", decodedCount);
    free(enc);
    free(dec);
    return 0;
}

/* --------------------------------------------------------------------
 * Scenario B: market data capture — records exactly as they arrive off
 * the wire, big-endian network fields included. The schema declares the
 * byte order per field, so the stream round-trips the wire bytes
 * bit-exactly on any host while the columns still compress in the
 * value domain.
 * -------------------------------------------------------------------- */
typedef struct MarketTick {
    uint64_t exchangeTsNs; /* big-endian on the wire, ~1us cadence */
    int64_t priceE8;       /* signed fixed-point, random-walks */
    uint32_t quantity;     /* small lot sizes */
    uint16_t venue;        /* big-endian, 6 venues */
    uint8_t isBid;         /* side flag */
    char symbol[8];        /* padded ticker — plane-structured */
    uint8_t pad_[1];
} MarketTick;

static const varintRecordField tickSchema[] = {
    VARINT_RECORD_FIELD_BE(MarketTick, exchangeTsNs, VARINT_RECORD_U64),
    VARINT_RECORD_FIELD(MarketTick, priceE8, VARINT_RECORD_I64),
    VARINT_RECORD_FIELD(MarketTick, quantity, VARINT_RECORD_U32),
    VARINT_RECORD_FIELD_BE(MarketTick, venue, VARINT_RECORD_U16),
    VARINT_RECORD_FIELD(MarketTick, isBid, VARINT_RECORD_BOOL),
    {offsetof(MarketTick, symbol), 8, VARINT_RECORD_BYTES, 0},
};
static const char *tickFieldNames[] = {"exchangeTsNs", "priceE8", "quantity",
                                       "venue",        "isBid",   "symbol"};

static void storeBE64(uint8_t *p, uint64_t v) {
    for (size_t i = 8; i > 0; i--) {
        p[i - 1] = (uint8_t)v;
        v >>= 8;
    }
}

static void fillTicks(MarketTick *rows, size_t count) {
    static const char *symbols[] = {"AAPL    ", "MSFT    ", "NVDA    ",
                                    "TSLA    "};
    uint64_t ts = UINT64_C(1700000000000000000);
    int64_t price = INT64_C(19000000000);
    for (size_t i = 0; i < count; i++) {
        ts += 800 + rng() % 400;
        storeBE64((uint8_t *)&rows[i].exchangeTsNs, ts);
        price += (int64_t)(rng() % 200001) - 100000;
        rows[i].priceE8 = price;
        rows[i].quantity = 100 * (1 + (uint32_t)(rng() % 20));
        const uint16_t venue = (uint16_t)(rng() % 6);
        ((uint8_t *)&rows[i].venue)[0] = (uint8_t)(venue >> 8);
        ((uint8_t *)&rows[i].venue)[1] = (uint8_t)venue;
        rows[i].isBid = (uint8_t)(rng() & 1);
        memcpy(rows[i].symbol, symbols[rng() % 4], 8);
    }
}

/* --------------------------------------------------------------------
 * Scenario C: game entity snapshots — grid coordinates, animation
 * enums, liveness flags. Small signed values and near-constant bytes,
 * the shape a replay or netcode system serializes every frame.
 * -------------------------------------------------------------------- */
typedef struct EntitySnapshot {
    uint32_t entityId; /* dense id space */
    int16_t x, y, z;   /* grid coordinates, entities cluster */
    uint8_t alive;     /* almost always 1 */
    uint8_t animState; /* 5 animations, idle-heavy */
    uint8_t skin[4];   /* cosmetic id — few distinct */
} EntitySnapshot;

static const varintRecordField entitySchema[] = {
    VARINT_RECORD_FIELD(EntitySnapshot, entityId, VARINT_RECORD_U32),
    VARINT_RECORD_FIELD(EntitySnapshot, x, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(EntitySnapshot, y, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(EntitySnapshot, z, VARINT_RECORD_I16),
    VARINT_RECORD_FIELD(EntitySnapshot, alive, VARINT_RECORD_BOOL),
    VARINT_RECORD_FIELD(EntitySnapshot, animState, VARINT_RECORD_U8),
    {offsetof(EntitySnapshot, skin), 4, VARINT_RECORD_BYTES, 0},
};
static const char *entityFieldNames[] = {"entityId", "x",         "y",   "z",
                                         "alive",    "animState", "skin"};

static void fillEntities(EntitySnapshot *rows, size_t count) {
    static const uint8_t skins[][4] = {
        {1, 0, 0, 0}, {2, 0, 1, 0}, {7, 3, 0, 0}};
    for (size_t i = 0; i < count; i++) {
        rows[i].entityId = 1000 + (uint32_t)i;
        rows[i].x = (int16_t)((int64_t)(rng() % 401) - 200);
        rows[i].y = (int16_t)((int64_t)(rng() % 401) - 200);
        rows[i].z = (int16_t)((int64_t)(rng() % 33) - 16);
        rows[i].alive = (rng() % 50 != 0);
        rows[i].animState = (rng() % 4 == 0) ? (uint8_t)(rng() % 5) : 0;
        memcpy(rows[i].skin, skins[rng() % 3], 4);
    }
}

int main(void) {
    printf("===========================================\n");
    printf("   varintRecord Example: Sensor Fleet\n");
    printf("===========================================\n\n");

    enum { N = 200000 };
    SensorReading *rows = calloc(N, sizeof(*rows));
    fillReadings(rows, N);
    const size_t rawBytes = N * sizeof(SensorReading);
    printf("Dataset: %d readings x %zu bytes = %.2f MB raw\n\n", N,
           sizeof(SensorReading), (double)rawBytes / (1024.0 * 1024.0));

    /* 1. Encode: one call, schema in, compressed stream out. */
    uint8_t *enc = malloc(varintRecordMaxEncodedSize(
        N, sizeof(SensorReading), sensorSchema, SENSOR_FIELDS));
    varintRecordMeta meta;
    const size_t written =
        varintRecordEncode(enc, rows, N, sizeof(SensorReading), sensorSchema,
                           SENSOR_FIELDS, 0, &meta);
    if (written == 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }

    printf("1. Columnar encode: %zu bytes (%.2fx compression)\n", written,
           (double)rawBytes / (double)written);

    /* 2. Per-column breakdown: winning strategy + cost per field. */
    printf("\n2. Per-column results (each field ran every strategy its\n"
           "   kind supports; the smallest verified result won):\n");
    for (size_t f = 0; f < SENSOR_FIELDS; f++) {
        const size_t colRaw = N * sensorSchema[f].size;
        printf("   %-12s %-5s %9zu B raw -> %8zu B (%8.2fx) via %s\n",
               sensorFieldNames[f], varintRecordKindName(sensorSchema[f].kind),
               colRaw, meta.columnBytes[f],
               (double)colRaw / (double)meta.columnBytes[f],
               varintRecordStrategyName(
                   (varintRecordStrategy)meta.columnStrategy[f]));
    }

    /* 3. Self-description: size and decode with no external metadata. */
    varintRecordHeader hdr;
    if (varintRecordReadHeader(enc, written, &hdr) == 0) {
        fprintf(stderr, "header parse failed\n");
        return 1;
    }
    printf("\n3. Stream is self-describing: header says %" PRIu64
           " records x %" PRIu64 " bytes, %" PRIu64 " fields\n",
           hdr.recordCount, hdr.recordSize, hdr.fieldCount);

    SensorReading *dec =
        malloc((size_t)hdr.recordCount * (size_t)hdr.recordSize);
    size_t decodedCount = 0;
    const size_t read =
        varintRecordDecode(enc, written, dec, (size_t)hdr.recordCount,
                           sizeof(SensorReading), &decodedCount);
    if (read != written || decodedCount != N ||
        memcmp(dec, rows, rawBytes) != 0) {
        fprintf(stderr, "decode round trip failed\n");
        return 1;
    }
    printf("   Decoded %zu records, byte-identical to the originals ✓\n",
           decodedCount);

    /* The same schema drives a generic paginated printer — inspect any
     * window of records without writing per-struct print code. */
    printf("\n   First 3 decoded records via varintRecordPrintRecords:\n");
    varintRecordPrintRecords(stdout, dec, N, sizeof(SensorReading),
                             sensorSchema, SENSOR_FIELDS, sensorFieldNames, 0,
                             3);

    /* 4. Why columns beat rows. */
    uint8_t *rowEnc = malloc(rawBytes * 2);
    const size_t rowBytes = encodeRowOriented(rowEnc, rows, N);
    printf("\n4. Row-oriented tagged baseline: %zu bytes (%.2fx)\n", rowBytes,
           (double)rawBytes / (double)rowBytes);
    printf("   Columnar advantage: %.2fx smaller than row-oriented\n",
           (double)rowBytes / (double)written);
    printf("   (Columns expose the structure — stride timestamps, a\n"
           "   40-device dictionary, near-constant status — that\n"
           "   per-record varints cannot see.)\n");

    free(rows);
    free(enc);
    free(dec);
    free(rowEnc);

    /* Scenario B: wire-format market data (big-endian network fields). */
    MarketTick *ticks = calloc(N, sizeof(*ticks));
    fillTicks(ticks, N);
    if (runScenario("Scenario B: Market Data Capture (wire format)", ticks, N,
                    sizeof(MarketTick), tickSchema, tickFieldNames, 6)) {
        return 1;
    }
    free(ticks);

    /* Scenario C: game entity snapshots (small ints, flags, planes). */
    EntitySnapshot *entities = calloc(N, sizeof(*entities));
    fillEntities(entities, N);
    if (runScenario("Scenario C: Game Entity Snapshots", entities, N,
                    sizeof(EntitySnapshot), entitySchema, entityFieldNames,
                    7)) {
        return 1;
    }
    free(entities);

    printf("\n✅ All operations completed successfully!\n");
    return 0;
}
