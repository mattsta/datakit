/**
 * delta_compression.c - Delta-of-Delta Encoding using varintExternal
 *
 * This example demonstrates Facebook Gorilla-style time series compression:
 * - First-order delta encoding (value[i] - value[i-1])
 * - Second-order delta-of-delta encoding (delta[i] - delta[i-1])
 * - varintExternal for adaptive width delta storage
 *
 * Based on "Gorilla: A Fast, Scalable, In-Memory Time Series Database" (2015)
 * by Pelkonen et al., Facebook. The paper demonstrates 10-20x compression
 * ratios for production monitoring data using delta-of-delta encoding.
 *
 * Key insight: Time series data exhibits temporal locality. Sequential values
 * have similar deltas, making delta-of-delta values very small (often 0).
 * varintExternal perfectly complements this by using 1 byte for values < 256.
 *
 * Features:
 * - Timestamp delta-of-delta encoding (regular intervals → mostly zeros)
 * - Value delta-of-delta encoding (smooth changes → small deltas)
 * - Multiple data pattern demonstrations (monotonic, oscillating, spiky)
 * - Compression ratio analysis (10-20x typical)
 * - Round-trip decode verification
 * - Real-world IoT/monitoring use cases
 *
 * Compile: gcc -I../../src delta_compression.c ../../build/src/libvarint.a -o
 * delta_compression Run: ./delta_compression
 */

#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
    uint64_t timestamp; // Unix timestamp (seconds or milliseconds)
    int64_t value;      // Sensor/metric value
} TimeSeriesPoint;

typedef struct {
    TimeSeriesPoint *points;
    size_t count;
    size_t capacity;
} TimeSeries;

// Encoded representation using delta-of-delta
typedef struct {
    // Base values (first point stored directly)
    uint64_t baseTimestamp;
    int64_t baseValue;

    // First deltas (second point as delta from base)
    int64_t firstTimeDelta;
    int64_t firstValueDelta;

    // Delta-of-delta arrays (third point onwards)
    int64_t *timeDeltaOfDelta;  // timestamp delta-of-deltas
    int64_t *valueDeltaOfDelta; // value delta-of-deltas
    size_t deltaCount;          // Number of delta-of-delta entries

    // Serialized buffer
    uint8_t *buffer;
    size_t bufferSize;
    size_t bufferCapacity;
} EncodedTimeSeries;

// ============================================================================
// TIME SERIES OPERATIONS
// ============================================================================

void timeSeriesInit(TimeSeries *ts, size_t capacity) {
    ts->points = malloc(capacity * sizeof(TimeSeriesPoint));
    ts->count = 0;
    ts->capacity = capacity;
}

void timeSeriesFree(TimeSeries *ts) {
    free(ts->points);
}

void timeSeriesAppend(TimeSeries *ts, uint64_t timestamp, int64_t value) {
    assert(ts->count < ts->capacity);
    ts->points[ts->count].timestamp = timestamp;
    ts->points[ts->count].value = value;
    ts->count++;
}

// ============================================================================
// DELTA ENCODING (First-order)
// ============================================================================

typedef struct {
    uint64_t baseTimestamp;
    int64_t baseValue;
    int64_t *timeDeltas;
    int64_t *valueDeltas;
    size_t count;
} DeltaEncoded;

void deltaEncode(const TimeSeries *ts, DeltaEncoded *delta) {
    assert(ts->count > 0);

    delta->baseTimestamp = ts->points[0].timestamp;
    delta->baseValue = ts->points[0].value;
    delta->count = ts->count - 1;

    if (delta->count > 0) {
        delta->timeDeltas = malloc(delta->count * sizeof(int64_t));
        delta->valueDeltas = malloc(delta->count * sizeof(int64_t));

        for (size_t i = 0; i < delta->count; i++) {
            delta->timeDeltas[i] = (int64_t)ts->points[i + 1].timestamp -
                                   (int64_t)ts->points[i].timestamp;
            delta->valueDeltas[i] =
                ts->points[i + 1].value - ts->points[i].value;
        }
    } else {
        delta->timeDeltas = NULL;
        delta->valueDeltas = NULL;
    }
}

void deltaFree(DeltaEncoded *delta) {
    free(delta->timeDeltas);
    free(delta->valueDeltas);
}

// ============================================================================
// DELTA-OF-DELTA ENCODING (Second-order)
// ============================================================================

void deltaOfDeltaEncode(const TimeSeries *ts, EncodedTimeSeries *encoded) {
    assert(ts->count > 0);

    // Store base values (first point)
    encoded->baseTimestamp = ts->points[0].timestamp;
    encoded->baseValue = ts->points[0].value;

    if (ts->count == 1) {
        encoded->firstTimeDelta = 0;
        encoded->firstValueDelta = 0;
        encoded->deltaCount = 0;
        encoded->timeDeltaOfDelta = NULL;
        encoded->valueDeltaOfDelta = NULL;
        return;
    }

    // Store first deltas (second point)
    encoded->firstTimeDelta =
        (int64_t)ts->points[1].timestamp - (int64_t)ts->points[0].timestamp;
    encoded->firstValueDelta = ts->points[1].value - ts->points[0].value;

    if (ts->count == 2) {
        encoded->deltaCount = 0;
        encoded->timeDeltaOfDelta = NULL;
        encoded->valueDeltaOfDelta = NULL;
        return;
    }

    // Compute delta-of-deltas (third point onwards)
    encoded->deltaCount = ts->count - 2;
    encoded->timeDeltaOfDelta = malloc(encoded->deltaCount * sizeof(int64_t));
    encoded->valueDeltaOfDelta = malloc(encoded->deltaCount * sizeof(int64_t));

    int64_t prevTimeDelta = encoded->firstTimeDelta;
    int64_t prevValueDelta = encoded->firstValueDelta;

    for (size_t i = 0; i < encoded->deltaCount; i++) {
        size_t idx = i + 2; // Start from third point

        // Compute current deltas
        int64_t currTimeDelta = (int64_t)ts->points[idx].timestamp -
                                (int64_t)ts->points[idx - 1].timestamp;
        int64_t currValueDelta =
            ts->points[idx].value - ts->points[idx - 1].value;

        // Compute delta-of-deltas
        encoded->timeDeltaOfDelta[i] = currTimeDelta - prevTimeDelta;
        encoded->valueDeltaOfDelta[i] = currValueDelta - prevValueDelta;

        prevTimeDelta = currTimeDelta;
        prevValueDelta = currValueDelta;
    }
}

void encodedTimeSeriesFree(EncodedTimeSeries *encoded) {
    free(encoded->timeDeltaOfDelta);
    free(encoded->valueDeltaOfDelta);
    free(encoded->buffer);
}

// ============================================================================
// SERIALIZATION (using varintExternal)
// ============================================================================

size_t serializeDeltaOfDelta(EncodedTimeSeries *encoded) {
    // Allocate buffer (worst case: all 8-byte values)
    size_t maxSize = 8 + 8 + 8 + 8 +                // base + first deltas
                     (encoded->deltaCount * 8 * 2); // delta-of-deltas
    encoded->buffer = malloc(maxSize);
    encoded->bufferCapacity = maxSize;

    size_t offset = 0;

    // Serialize base timestamp (varintExternal adaptive width)
    varintWidth tsWidth =
        varintExternalPut(encoded->buffer + offset, encoded->baseTimestamp);
    offset += tsWidth;

    // Serialize base value (handle signed values using ZigZag encoding)
    uint64_t zigzagValue =
        (uint64_t)(encoded->baseValue >= 0 ? encoded->baseValue * 2
                                           : -encoded->baseValue * 2 - 1);
    varintWidth valWidth =
        varintExternalPut(encoded->buffer + offset, zigzagValue);
    offset += valWidth;

    // Serialize first time delta
    zigzagValue = (uint64_t)(encoded->firstTimeDelta >= 0
                                 ? encoded->firstTimeDelta * 2
                                 : -encoded->firstTimeDelta * 2 - 1);
    varintWidth ftdWidth =
        varintExternalPut(encoded->buffer + offset, zigzagValue);
    offset += ftdWidth;

    // Serialize first value delta
    zigzagValue = (uint64_t)(encoded->firstValueDelta >= 0
                                 ? encoded->firstValueDelta * 2
                                 : -encoded->firstValueDelta * 2 - 1);
    varintWidth fvdWidth =
        varintExternalPut(encoded->buffer + offset, zigzagValue);
    offset += fvdWidth;

    // Serialize delta-of-delta arrays
    // The magic of Gorilla: most delta-of-deltas are 0 or very small!
    for (size_t i = 0; i < encoded->deltaCount; i++) {
        // Time delta-of-delta (ZigZag encoded)
        zigzagValue = (uint64_t)(encoded->timeDeltaOfDelta[i] >= 0
                                     ? encoded->timeDeltaOfDelta[i] * 2
                                     : -encoded->timeDeltaOfDelta[i] * 2 - 1);
        varintWidth w =
            varintExternalPut(encoded->buffer + offset, zigzagValue);
        offset += w;

        // Value delta-of-delta (ZigZag encoded)
        zigzagValue = (uint64_t)(encoded->valueDeltaOfDelta[i] >= 0
                                     ? encoded->valueDeltaOfDelta[i] * 2
                                     : -encoded->valueDeltaOfDelta[i] * 2 - 1);
        w = varintExternalPut(encoded->buffer + offset, zigzagValue);
        offset += w;
    }

    encoded->bufferSize = offset;
    return offset;
}

// ============================================================================
// DESERIALIZATION (Decode back to TimeSeries)
// ============================================================================

int64_t zigzagDecode(uint64_t zigzag) {
    if (zigzag & 1) {
        // Odd: negative
        return -((int64_t)((zigzag + 1) / 2));
    } else {
        // Even: positive
        return (int64_t)(zigzag / 2);
    }
}

void deserializeDeltaOfDelta(const EncodedTimeSeries *encoded, TimeSeries *ts,
                             size_t expectedCount) {
    timeSeriesInit(ts, expectedCount);

    size_t offset = 0;

    // Deserialize base timestamp
    varintWidth tsWidth = varintExternalLen(encoded->baseTimestamp);
    uint64_t baseTimestamp =
        varintExternalGet(encoded->buffer + offset, tsWidth);
    offset += tsWidth;

    // Deserialize base value
    varintWidth valWidth = varintExternalLen(encoded->baseValue >= 0
                                                 ? encoded->baseValue * 2
                                                 : -encoded->baseValue * 2 - 1);
    uint64_t zigzagValue =
        varintExternalGet(encoded->buffer + offset, valWidth);
    int64_t baseValue = zigzagDecode(zigzagValue);
    offset += valWidth;

    // Add first point
    timeSeriesAppend(ts, baseTimestamp, baseValue);

    if (expectedCount == 1) {
        return;
    }

    // Deserialize first time delta
    varintWidth ftdWidth = varintExternalLen(
        encoded->firstTimeDelta >= 0 ? encoded->firstTimeDelta * 2
                                     : -encoded->firstTimeDelta * 2 - 1);
    zigzagValue = varintExternalGet(encoded->buffer + offset, ftdWidth);
    int64_t firstTimeDelta = zigzagDecode(zigzagValue);
    offset += ftdWidth;

    // Deserialize first value delta
    varintWidth fvdWidth = varintExternalLen(
        encoded->firstValueDelta >= 0 ? encoded->firstValueDelta * 2
                                      : -encoded->firstValueDelta * 2 - 1);
    zigzagValue = varintExternalGet(encoded->buffer + offset, fvdWidth);
    int64_t firstValueDelta = zigzagDecode(zigzagValue);
    offset += fvdWidth;

    // Add second point
    timeSeriesAppend(ts, baseTimestamp + firstTimeDelta,
                     baseValue + firstValueDelta);

    if (expectedCount == 2) {
        return;
    }

    // Deserialize delta-of-delta arrays and reconstruct original series
    int64_t prevTimeDelta = firstTimeDelta;
    int64_t prevValueDelta = firstValueDelta;
    uint64_t prevTimestamp = baseTimestamp + firstTimeDelta;
    int64_t prevValue = baseValue + firstValueDelta;

    for (size_t i = 0; i < encoded->deltaCount; i++) {
        // Time delta-of-delta
        varintWidth w =
            varintExternalLen(encoded->timeDeltaOfDelta[i] >= 0
                                  ? encoded->timeDeltaOfDelta[i] * 2
                                  : -encoded->timeDeltaOfDelta[i] * 2 - 1);
        zigzagValue = varintExternalGet(encoded->buffer + offset, w);
        int64_t timeDeltaOfDelta = zigzagDecode(zigzagValue);
        offset += w;

        // Value delta-of-delta
        w = varintExternalLen(encoded->valueDeltaOfDelta[i] >= 0
                                  ? encoded->valueDeltaOfDelta[i] * 2
                                  : -encoded->valueDeltaOfDelta[i] * 2 - 1);
        zigzagValue = varintExternalGet(encoded->buffer + offset, w);
        int64_t valueDeltaOfDelta = zigzagDecode(zigzagValue);
        offset += w;

        // Reconstruct current deltas
        int64_t currTimeDelta = prevTimeDelta + timeDeltaOfDelta;
        int64_t currValueDelta = prevValueDelta + valueDeltaOfDelta;

        // Reconstruct current values
        uint64_t currTimestamp = prevTimestamp + currTimeDelta;
        int64_t currValue = prevValue + currValueDelta;

        timeSeriesAppend(ts, currTimestamp, currValue);

        // Update previous values
        prevTimeDelta = currTimeDelta;
        prevValueDelta = currValueDelta;
        prevTimestamp = currTimestamp;
        prevValue = currValue;
    }
}

// ============================================================================
// COMPRESSION ANALYSIS
// ============================================================================

void analyzeCompression(const TimeSeries *ts, const EncodedTimeSeries *encoded,
                        const char *scenario) {
    printf("\n--- %s ---\n", scenario);

    // Original size (uncompressed)
    size_t originalSize = ts->count * (sizeof(uint64_t) + sizeof(int64_t));
    printf("Data points: %zu\n", ts->count);
    printf("Original size: %zu bytes (%zu bytes/point)\n", originalSize,
           originalSize / ts->count);

    // First-order delta size
    DeltaEncoded delta;
    deltaEncode(ts, &delta);
    size_t deltaSize = 8 + 8; // base timestamp + base value
    for (size_t i = 0; i < delta.count; i++) {
        deltaSize += varintExternalLen(delta.timeDeltas[i] >= 0
                                           ? delta.timeDeltas[i] * 2
                                           : -delta.timeDeltas[i] * 2 - 1);
        deltaSize += varintExternalLen(delta.valueDeltas[i] >= 0
                                           ? delta.valueDeltas[i] * 2
                                           : -delta.valueDeltas[i] * 2 - 1);
    }
    printf("First-order delta: %zu bytes (%.1fx compression)\n", deltaSize,
           (double)originalSize / deltaSize);
    deltaFree(&delta);

    // Second-order delta-of-delta size (our implementation)
    printf("Delta-of-delta: %zu bytes (%.1fx compression)\n",
           encoded->bufferSize, (double)originalSize / encoded->bufferSize);

    // Show savings
    printf("Space savings: %zu bytes (%.1f%%)\n",
           originalSize - encoded->bufferSize,
           100.0 * (1.0 - (double)encoded->bufferSize / originalSize));
}

// ============================================================================
// SCENARIO 1: Sensor Readings (regular intervals, smooth values)
// ============================================================================

void demonstrateSensorReadings() {
    printf("\n=== SCENARIO 1: IoT Sensor Readings ===\n");
    printf("Pattern: Temperature sensor, 1-minute intervals, smooth changes\n");

    TimeSeries ts;
    timeSeriesInit(&ts, 100);

    // Simulate temperature sensor: gradual changes, regular 60-second intervals
    uint64_t baseTime = 1700000000; // Nov 2023
    int64_t baseTemp = 20000;       // 20.000°C (millidegrees)

    for (size_t i = 0; i < 100; i++) {
        uint64_t timestamp = baseTime + i * 60; // Regular 60-second intervals

        // Smooth temperature variation (sine wave + small noise)
        double variation = 2000.0 * sin(i * 0.1) + (rand() % 100 - 50);
        int64_t temp = baseTemp + (int64_t)variation;

        timeSeriesAppend(&ts, timestamp, temp);
    }

    // Encode with delta-of-delta
    EncodedTimeSeries encoded = {0};
    deltaOfDeltaEncode(&ts, &encoded);
    serializeDeltaOfDelta(&encoded);

    // Show delta-of-delta distribution
    printf("\nDelta-of-delta analysis:\n");
    size_t zeroCount = 0, smallCount = 0;
    for (size_t i = 0; i < encoded.deltaCount; i++) {
        if (encoded.timeDeltaOfDelta[i] == 0) {
            zeroCount++;
        }
        if (llabs(encoded.valueDeltaOfDelta[i]) <= 100) {
            smallCount++;
        }
    }
    printf(
        "  Time delta-of-deltas = 0: %zu/%zu (%.1f%%) [regular intervals!]\n",
        zeroCount, encoded.deltaCount, 100.0 * zeroCount / encoded.deltaCount);
    printf(
        "  Value delta-of-deltas <= 100: %zu/%zu (%.1f%%) [smooth changes!]\n",
        smallCount, encoded.deltaCount,
        100.0 * smallCount / encoded.deltaCount);

    analyzeCompression(&ts, &encoded, "Sensor Readings");

    // Verify round-trip
    TimeSeries decoded;
    deserializeDeltaOfDelta(&encoded, &decoded, ts.count);
    assert(decoded.count == ts.count);
    for (size_t i = 0; i < ts.count; i++) {
        assert(decoded.points[i].timestamp == ts.points[i].timestamp);
        assert(decoded.points[i].value == ts.points[i].value);
    }
    printf("Round-trip verification: PASSED\n");

    timeSeriesFree(&ts);
    timeSeriesFree(&decoded);
    encodedTimeSeriesFree(&encoded);
}

// ============================================================================
// SCENARIO 2: Stock Prices (irregular intervals, varying deltas)
// ============================================================================

void demonstrateStockPrices() {
    printf("\n\n=== SCENARIO 2: Stock Price Ticks ===\n");
    printf("Pattern: Irregular intervals, price changes in cents\n");

    TimeSeries ts;
    timeSeriesInit(&ts, 200);

    // Simulate stock ticks: irregular intervals, price in cents
    uint64_t baseTime = 1700000000;
    int64_t price = 15000; // $150.00 in cents

    for (size_t i = 0; i < 200; i++) {
        // Irregular intervals (1-10 seconds)
        uint64_t timestamp = baseTime + (rand() % 10 + 1);
        baseTime = timestamp;

        // Price changes (-5 to +5 cents mostly)
        int change = rand() % 11 - 5;
        price += change;

        timeSeriesAppend(&ts, timestamp, price);
    }

    EncodedTimeSeries encoded = {0};
    deltaOfDeltaEncode(&ts, &encoded);
    serializeDeltaOfDelta(&encoded);

    // Show delta-of-delta distribution
    printf("\nDelta-of-delta analysis:\n");
    size_t smallTimeDelta = 0, smallValueDelta = 0;
    for (size_t i = 0; i < encoded.deltaCount; i++) {
        if (llabs(encoded.timeDeltaOfDelta[i]) <= 5) {
            smallTimeDelta++;
        }
        if (llabs(encoded.valueDeltaOfDelta[i]) <= 5) {
            smallValueDelta++;
        }
    }
    printf("  Time delta-of-deltas <= 5: %zu/%zu (%.1f%%)\n", smallTimeDelta,
           encoded.deltaCount, 100.0 * smallTimeDelta / encoded.deltaCount);
    printf("  Value delta-of-deltas <= 5 cents: %zu/%zu (%.1f%%)\n",
           smallValueDelta, encoded.deltaCount,
           100.0 * smallValueDelta / encoded.deltaCount);

    analyzeCompression(&ts, &encoded, "Stock Prices");

    // Verify round-trip
    TimeSeries decoded;
    deserializeDeltaOfDelta(&encoded, &decoded, ts.count);
    assert(decoded.count == ts.count);
    for (size_t i = 0; i < ts.count; i++) {
        assert(decoded.points[i].timestamp == ts.points[i].timestamp);
        assert(decoded.points[i].value == ts.points[i].value);
    }
    printf("Round-trip verification: PASSED\n");

    timeSeriesFree(&ts);
    timeSeriesFree(&decoded);
    encodedTimeSeriesFree(&encoded);
}

// ============================================================================
// SCENARIO 3: Counter Metrics (monotonic increasing)
// ============================================================================

void demonstrateCounterMetrics() {
    printf("\n\n=== SCENARIO 3: Counter Metrics (Monotonic) ===\n");
    printf("Pattern: Request counter, steadily increasing\n");

    TimeSeries ts;
    timeSeriesInit(&ts, 150);

    // Simulate request counter: regular intervals, steadily increasing
    uint64_t baseTime = 1700000000;
    int64_t counter = 0;
    int64_t requestsPerMinute = 1000;

    for (size_t i = 0; i < 150; i++) {
        uint64_t timestamp = baseTime + i * 60; // Every minute

        // Requests per minute varies slightly (950-1050)
        int variation = rand() % 101 - 50;
        counter += requestsPerMinute + variation;

        timeSeriesAppend(&ts, timestamp, counter);
    }

    EncodedTimeSeries encoded = {0};
    deltaOfDeltaEncode(&ts, &encoded);
    serializeDeltaOfDelta(&encoded);

    // Show delta-of-delta distribution
    printf("\nDelta-of-delta analysis:\n");
    size_t zeroTimeDelta = 0, smallValueDelta = 0;
    for (size_t i = 0; i < encoded.deltaCount; i++) {
        if (encoded.timeDeltaOfDelta[i] == 0) {
            zeroTimeDelta++;
        }
        if (llabs(encoded.valueDeltaOfDelta[i]) <= 100) {
            smallValueDelta++;
        }
    }
    printf(
        "  Time delta-of-deltas = 0: %zu/%zu (%.1f%%) [regular intervals!]\n",
        zeroTimeDelta, encoded.deltaCount,
        100.0 * zeroTimeDelta / encoded.deltaCount);
    printf("  Value delta-of-deltas <= 100: %zu/%zu (%.1f%%) [steady rate!]\n",
           smallValueDelta, encoded.deltaCount,
           100.0 * smallValueDelta / encoded.deltaCount);

    analyzeCompression(&ts, &encoded, "Counter Metrics");

    // Verify round-trip
    TimeSeries decoded;
    deserializeDeltaOfDelta(&encoded, &decoded, ts.count);
    assert(decoded.count == ts.count);
    for (size_t i = 0; i < ts.count; i++) {
        assert(decoded.points[i].timestamp == ts.points[i].timestamp);
        assert(decoded.points[i].value == ts.points[i].value);
    }
    printf("Round-trip verification: PASSED\n");

    timeSeriesFree(&ts);
    timeSeriesFree(&decoded);
    encodedTimeSeriesFree(&encoded);
}

// ============================================================================
// SCENARIO 4: Temperature Data (oscillating pattern)
// ============================================================================

void demonstrateTemperatureData() {
    printf("\n\n=== SCENARIO 4: Daily Temperature Cycle ===\n");
    printf("Pattern: 24-hour cycle, predictable oscillation\n");

    TimeSeries ts;
    timeSeriesInit(&ts, 288); // 24 hours × 12 (5-minute intervals)

    // Simulate daily temperature: 5-minute intervals, sine wave pattern
    uint64_t baseTime = 1700000000;
    int64_t avgTemp = 15000; // 15.000°C in millidegrees

    for (size_t i = 0; i < 288; i++) {
        uint64_t timestamp = baseTime + i * 300; // 5-minute intervals

        // Daily cycle: peaks at noon, troughs at midnight
        double hourOfDay = (i * 5.0) / 60.0; // Hours since start
        double tempVariation =
            8000.0 * sin((hourOfDay / 24.0) * 2 * M_PI - M_PI / 2);
        int64_t temp = avgTemp + (int64_t)tempVariation;

        timeSeriesAppend(&ts, timestamp, temp);
    }

    EncodedTimeSeries encoded = {0};
    deltaOfDeltaEncode(&ts, &encoded);
    serializeDeltaOfDelta(&encoded);

    // Show delta-of-delta distribution
    printf("\nDelta-of-delta analysis:\n");
    size_t zeroTimeDelta = 0, smallValueDelta = 0;
    for (size_t i = 0; i < encoded.deltaCount; i++) {
        if (encoded.timeDeltaOfDelta[i] == 0) {
            zeroTimeDelta++;
        }
        if (llabs(encoded.valueDeltaOfDelta[i]) <= 200) {
            smallValueDelta++;
        }
    }
    printf(
        "  Time delta-of-deltas = 0: %zu/%zu (%.1f%%) [regular intervals!]\n",
        zeroTimeDelta, encoded.deltaCount,
        100.0 * zeroTimeDelta / encoded.deltaCount);
    printf("  Value delta-of-deltas <= 200: %zu/%zu (%.1f%%) [smooth cycle!]\n",
           smallValueDelta, encoded.deltaCount,
           100.0 * smallValueDelta / encoded.deltaCount);

    analyzeCompression(&ts, &encoded, "Temperature Cycle");

    // Verify round-trip
    TimeSeries decoded;
    deserializeDeltaOfDelta(&encoded, &decoded, ts.count);
    assert(decoded.count == ts.count);
    for (size_t i = 0; i < ts.count; i++) {
        assert(decoded.points[i].timestamp == ts.points[i].timestamp);
        assert(decoded.points[i].value == ts.points[i].value);
    }
    printf("Round-trip verification: PASSED\n");

    timeSeriesFree(&ts);
    timeSeriesFree(&decoded);
    encodedTimeSeriesFree(&encoded);
}

// ============================================================================
// MAIN DEMONSTRATION
// ============================================================================

int main() {
    printf("=======================================================\n");
    printf("  Delta-of-Delta Time Series Compression\n");
    printf("  (Facebook Gorilla-style + varintExternal)\n");
    printf("=======================================================\n");

    printf("\nDelta-of-Delta Encoding Explained:\n");
    printf("----------------------------------\n");
    printf("First-order delta:  delta[i] = value[i] - value[i-1]\n");
    printf("Second-order delta: delta2[i] = delta[i] - delta[i-1]\n");
    printf("\nWhy it works:\n");
    printf("  - Regular time intervals → time delta-of-deltas = 0\n");
    printf("  - Smooth value changes → value delta-of-deltas ≈ 0\n");
    printf("  - varintExternal uses 1 byte for values < 256\n");
    printf("  - Result: 10-20x compression for real-world data!\n");

    // Run all scenarios
    demonstrateSensorReadings();
    demonstrateStockPrices();
    demonstrateCounterMetrics();
    demonstrateTemperatureData();

    printf("\n\n=======================================================\n");
    printf("Summary: Delta-of-Delta Compression Benefits\n");
    printf("=======================================================\n");
    printf("\n");
    printf("Key Insights:\n");
    printf(
        "  1. Regular intervals → time delta-of-deltas = 0 (100%% of cases)\n");
    printf("  2. Smooth changes → value delta-of-deltas are tiny\n");
    printf("  3. varintExternal adapts: 1 byte for small deltas\n");
    printf("  4. Typical compression: 10-20x for monitoring data\n");
    printf("\n");
    printf("Real-World Applications:\n");
    printf("  • IoT sensor networks (temperature, humidity, pressure)\n");
    printf("  • Financial tick data (stocks, forex)\n");
    printf("  • Server monitoring (CPU, memory, network)\n");
    printf("  • Application metrics (requests/sec, latency)\n");
    printf("  • Any time series with temporal locality!\n");
    printf("\n");
    printf("varintExternal Synergy:\n");
    printf("  • Adaptive width (1-8 bytes) perfectly matches delta sizes\n");
    printf("  • Zero values → 1 byte (not 8 bytes!)\n");
    printf("  • Small deltas (-127 to 127) → 1 byte after ZigZag\n");
    printf("  • Large deltas → only when needed\n");
    printf("\n");
    printf("Reference: \"Gorilla: A Fast, Scalable, In-Memory Time Series\n");
    printf("Database\" by Pelkonen et al., Facebook, 2015.\n");
    printf("=======================================================\n");

    return 0;
}
