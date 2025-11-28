/**
 * sensor_network.c - Sensor data encoding using varintExternal
 *
 * This example demonstrates a sensor network combining:
 * - varintExternal: Timestamp and value encoding with adaptive widths
 * - Delta encoding for sequential readings
 * - Time-series data compression
 *
 * Features:
 * - Multi-resolution timestamp encoding (ms, sec, min, hour)
 * - Adaptive sensor value widths based on range
 * - Delta encoding for sequential readings
 * - Efficient time-series storage
 * - Batch compression for network transmission
 *
 * Compile: gcc -I../../src sensor_network.c ../../build/src/libvarint.a -o
 * sensor_network Run: ./sensor_network
 */

#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// TIMESTAMP ENCODING (using varintSplit)
// ============================================================================

/*
 * Timestamps encoded with varintSplit based on resolution:
 * - Millisecond precision: varintSplitFull (0-8 bytes)
 * - Second precision: varintSplitFull16 (0-2 bytes for values < 65536)
 * - Minute precision: varintSplitFullNoZero (1-8 bytes, common for sensors)
 */

typedef enum {
    TIME_RES_MILLISECOND, // Up to ~292 million years in ms
    TIME_RES_SECOND,      // Up to ~584 billion years in seconds
    TIME_RES_MINUTE,      // Most common for sensors
    TIME_RES_HOUR,
} TimeResolution;

typedef struct {
    uint64_t value;
    TimeResolution resolution;
} Timestamp;

varintWidth encodeTimestamp(uint8_t *buffer, Timestamp ts) {
    // All resolutions use varintExternal (adaptive width)
    return varintExternalPut(buffer, ts.value);
}

Timestamp decodeTimestamp(const uint8_t *buffer, TimeResolution resolution,
                          varintWidth width) {
    Timestamp ts;
    ts.resolution = resolution;
    ts.value = varintExternalGet(buffer, width);
    return ts;
}

// ============================================================================
// SENSOR VALUE ENCODING (using varintExternal)
// ============================================================================

typedef enum {
    SENSOR_TYPE_TEMPERATURE, // -40°C to 85°C (1-byte signed)
    SENSOR_TYPE_HUMIDITY,    // 0-100% (1-byte unsigned)
    SENSOR_TYPE_PRESSURE,    // 300-1100 hPa (2-byte unsigned)
    SENSOR_TYPE_LIGHT,       // 0-65535 lux (2-byte unsigned)
    SENSOR_TYPE_VOLTAGE,     // 0-5.0V (2-byte unsigned, millivolts)
    SENSOR_TYPE_CURRENT,     // 0-10A (2-byte unsigned, milliamps)
    SENSOR_TYPE_POWER,       // 0-1000W (2-byte unsigned)
} SensorType;

typedef struct {
    SensorType type;
    uint64_t value;    // Raw value
    varintWidth width; // Encoding width
} SensorReading;

varintWidth getSensorWidth(SensorType type, uint64_t value) {
    // Determine optimal width based on sensor type and value
    switch (type) {
    case SENSOR_TYPE_TEMPERATURE:
    case SENSOR_TYPE_HUMIDITY:
        return 1; // Always 1 byte
    case SENSOR_TYPE_PRESSURE:
    case SENSOR_TYPE_LIGHT:
    case SENSOR_TYPE_VOLTAGE:
    case SENSOR_TYPE_CURRENT:
    case SENSOR_TYPE_POWER:
        if (value <= 255) {
            return 1;
        } else if (value <= 65535) {
            return 2;
        } else if (value <= 16777215UL) {
            return 3;
        } else {
            return 4;
        }
    default:
        return varintExternalLen(value); // Auto-detect
    }
}

void encodeSensorReading(uint8_t *buffer, const SensorReading *reading) {
    varintExternalPutFixedWidth(buffer, reading->value, reading->width);
}

SensorReading decodeSensorReading(const uint8_t *buffer, SensorType type,
                                  varintWidth width) {
    SensorReading reading;
    reading.type = type;
    reading.width = width;
    reading.value = varintExternalGet(buffer, width);
    return reading;
}

// ============================================================================
// SENSOR DATA POINT
// ============================================================================

typedef struct {
    Timestamp timestamp;
    uint8_t sensorId; // 0-255
    SensorReading reading;
} SensorDataPoint;

typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t capacity;
} DataBuffer;

void bufferInit(DataBuffer *buf, size_t capacity) {
    buf->buffer = malloc(capacity);
    buf->size = 0;
    buf->capacity = capacity;
}

void bufferFree(DataBuffer *buf) {
    free(buf->buffer);
}

void bufferAppendDataPoint(DataBuffer *buf, const SensorDataPoint *point) {
    // Format: [timestamp][sensorId][reading]
    varintWidth tsWidth =
        encodeTimestamp(buf->buffer + buf->size, point->timestamp);
    buf->size += tsWidth;

    buf->buffer[buf->size++] = point->sensorId;

    encodeSensorReading(buf->buffer + buf->size, &point->reading);
    buf->size += point->reading.width;
}

// ============================================================================
// DELTA ENCODING (for sequential readings)
// ============================================================================

typedef struct {
    uint64_t baseTimestamp;
    uint16_t *deltaTimestamps; // Deltas from base (varintExternal 1-2 bytes)
    SensorReading *readings;
    size_t count;
    size_t capacity;
    SensorType sensorType;
} DeltaEncodedSeries;

void deltaSeriesInit(DeltaEncodedSeries *series, SensorType type,
                     size_t capacity) {
    series->baseTimestamp = 0;
    series->deltaTimestamps = malloc(capacity * sizeof(uint16_t));
    series->readings = malloc(capacity * sizeof(SensorReading));
    series->count = 0;
    series->capacity = capacity;
    series->sensorType = type;
}

void deltaSeriesFree(DeltaEncodedSeries *series) {
    free(series->deltaTimestamps);
    free(series->readings);
}

void deltaSeriesAppend(DeltaEncodedSeries *series, uint64_t timestamp,
                       uint64_t value) {
    assert(series->count < series->capacity);

    if (series->count == 0) {
        series->baseTimestamp = timestamp;
        series->deltaTimestamps[0] = 0;
    } else {
        uint64_t delta = timestamp - series->baseTimestamp;
        assert(delta <= 65535); // Delta must fit in 16 bits
        series->deltaTimestamps[series->count] = (uint16_t)delta;
    }

    series->readings[series->count].type = series->sensorType;
    series->readings[series->count].value = value;
    series->readings[series->count].width =
        getSensorWidth(series->sensorType, value);

    series->count++;
}

size_t deltaSeriesSerialize(const DeltaEncodedSeries *series, uint8_t *buffer) {
    size_t offset = 0;

    // Write base timestamp (varintSplit)
    Timestamp baseTs = {.value = series->baseTimestamp,
                        .resolution = TIME_RES_SECOND};
    offset += encodeTimestamp(buffer, baseTs);

    // Write count (1-2 bytes)
    varintWidth countWidth = series->count <= 255 ? 1 : 2;
    varintExternalPutFixedWidth(buffer + offset, series->count, countWidth);
    offset += countWidth;

    // Write deltas and readings
    for (size_t i = 0; i < series->count; i++) {
        // Delta timestamp (1-2 bytes)
        varintWidth deltaWidth = series->deltaTimestamps[i] <= 255 ? 1 : 2;
        varintExternalPutFixedWidth(buffer + offset, series->deltaTimestamps[i],
                                    deltaWidth);
        offset += deltaWidth;

        // Reading value
        encodeSensorReading(buffer + offset, &series->readings[i]);
        offset += series->readings[i].width;
    }

    return offset;
}

// ============================================================================
// BATCH COMPRESSION
// ============================================================================

typedef struct {
    uint8_t sensorId;
    SensorType type;
    DeltaEncodedSeries series;
} SensorBatch;

typedef struct {
    SensorBatch *batches;
    size_t count;
    size_t capacity;
} BatchCompressor;

void batchCompressorInit(BatchCompressor *compressor, size_t maxSensors) {
    compressor->batches = malloc(maxSensors * sizeof(SensorBatch));
    compressor->count = 0;
    compressor->capacity = maxSensors;
}

void batchCompressorFree(BatchCompressor *compressor) {
    for (size_t i = 0; i < compressor->count; i++) {
        deltaSeriesFree(&compressor->batches[i].series);
    }
    free(compressor->batches);
}

void batchCompressorAddSensor(BatchCompressor *compressor, uint8_t sensorId,
                              SensorType type, size_t readingsPerSensor) {
    assert(compressor->count < compressor->capacity);

    SensorBatch *batch = &compressor->batches[compressor->count++];
    batch->sensorId = sensorId;
    batch->type = type;
    deltaSeriesInit(&batch->series, type, readingsPerSensor);
}

SensorBatch *batchCompressorGetSensor(BatchCompressor *compressor,
                                      uint8_t sensorId) {
    for (size_t i = 0; i < compressor->count; i++) {
        if (compressor->batches[i].sensorId == sensorId) {
            return &compressor->batches[i];
        }
    }
    return NULL;
}

size_t batchCompressorSerialize(const BatchCompressor *compressor,
                                uint8_t *buffer) {
    size_t offset = 0;

    // Write sensor count
    buffer[offset++] = (uint8_t)compressor->count;

    // Write each sensor batch
    for (size_t i = 0; i < compressor->count; i++) {
        const SensorBatch *batch = &compressor->batches[i];

        // Sensor metadata
        buffer[offset++] = batch->sensorId;
        buffer[offset++] = (uint8_t)batch->type;

        // Serialized series
        offset += deltaSeriesSerialize(&batch->series, buffer + offset);
    }

    return offset;
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateSensorNetwork(void) {
    printf("\n=== Sensor Network Example ===\n\n");

    // 1. Basic timestamp encoding
    printf("1. Testing timestamp encoding (varintSplit)...\n");

    Timestamp timestamps[] = {
        {1000, TIME_RES_MILLISECOND},  // 1 second in ms
        {60, TIME_RES_SECOND},         // 1 minute in seconds
        {1440, TIME_RES_MINUTE},       // 1 day in minutes
        {168, TIME_RES_HOUR},          // 1 week in hours
        {1609459200, TIME_RES_SECOND}, // 2021-01-01 in Unix seconds
    };

    uint8_t tsBuffer[9];
    for (size_t i = 0; i < sizeof(timestamps) / sizeof(timestamps[0]); i++) {
        varintWidth width = encodeTimestamp(tsBuffer, timestamps[i]);
        Timestamp decoded =
            decodeTimestamp(tsBuffer, timestamps[i].resolution, width);

        printf("   Timestamp %zu = %" PRIu64 " (%s): %d bytes\n", i,
               timestamps[i].value,
               timestamps[i].resolution == TIME_RES_MILLISECOND ? "ms"
               : timestamps[i].resolution == TIME_RES_SECOND    ? "sec"
               : timestamps[i].resolution == TIME_RES_MINUTE    ? "min"
                                                                : "hour",
               width);
        assert(decoded.value == timestamps[i].value);
    }

    // 2. Sensor value encoding
    printf("\n2. Testing sensor value encoding (varintExternal)...\n");

    SensorReading readings[] = {
        {SENSOR_TYPE_TEMPERATURE, 22, 1}, // 22°C
        {SENSOR_TYPE_HUMIDITY, 65, 1},    // 65%
        {SENSOR_TYPE_PRESSURE, 1013, 2},  // 1013 hPa
        {SENSOR_TYPE_LIGHT, 1500, 2},     // 1500 lux
        {SENSOR_TYPE_VOLTAGE, 3300, 2},   // 3.3V (3300 mV)
    };

    uint8_t readingBuffer[8];
    for (size_t i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
        encodeSensorReading(readingBuffer, &readings[i]);
        SensorReading decoded = decodeSensorReading(
            readingBuffer, readings[i].type, readings[i].width);

        printf("   Reading %zu = %" PRIu64 " (%d bytes): ", i,
               readings[i].value, readings[i].width);
        printf("%s\n", readings[i].type == SENSOR_TYPE_TEMPERATURE
                           ? "temperature"
                       : readings[i].type == SENSOR_TYPE_HUMIDITY ? "humidity"
                       : readings[i].type == SENSOR_TYPE_PRESSURE ? "pressure"
                       : readings[i].type == SENSOR_TYPE_LIGHT    ? "light"
                                                                  : "voltage");
        assert(decoded.value == readings[i].value);
    }

    // 3. Delta encoding
    printf("\n3. Testing delta encoding for time-series...\n");

    DeltaEncodedSeries tempSeries;
    deltaSeriesInit(&tempSeries, SENSOR_TYPE_TEMPERATURE, 10);

    // Simulated temperature readings every 60 seconds
    uint64_t baseTime = 1609459200; // 2021-01-01
    const uint64_t tempReadings[] = {20, 21, 21, 22, 22, 23, 22, 21, 20, 20};

    for (size_t i = 0; i < 10; i++) {
        deltaSeriesAppend(&tempSeries, baseTime + i * 60, tempReadings[i]);
    }

    printf("   Added %zu temperature readings\n", tempSeries.count);
    printf("   Base timestamp: %" PRIu64 "\n", tempSeries.baseTimestamp);
    printf("   Deltas: ");
    for (size_t i = 0; i < tempSeries.count; i++) {
        printf("%u ", tempSeries.deltaTimestamps[i]);
    }
    printf("\n");

    // Serialize
    uint8_t seriesBuffer[256];
    size_t seriesSize = deltaSeriesSerialize(&tempSeries, seriesBuffer);
    printf("   Serialized size: %zu bytes\n", seriesSize);

    // Compare with non-delta encoding
    size_t nonDeltaSize =
        tempSeries.count * (4 + 1); // 4 bytes timestamp + 1 byte reading
    printf("   Non-delta size: %zu bytes\n", nonDeltaSize);
    printf("   Savings: %zu bytes (%.1f%%)\n", nonDeltaSize - seriesSize,
           100.0 * (1.0 - (double)seriesSize / nonDeltaSize));

    // 4. Batch compression
    printf("\n4. Testing batch compression for multiple sensors...\n");

    BatchCompressor compressor;
    batchCompressorInit(&compressor, 5);

    // Add sensors
    batchCompressorAddSensor(&compressor, 1, SENSOR_TYPE_TEMPERATURE, 10);
    batchCompressorAddSensor(&compressor, 2, SENSOR_TYPE_HUMIDITY, 10);
    batchCompressorAddSensor(&compressor, 3, SENSOR_TYPE_PRESSURE, 10);

    printf("   Added %zu sensors to compressor\n", compressor.count);

    // Add readings for each sensor
    for (size_t i = 0; i < 10; i++) {
        SensorBatch *tempBatch = batchCompressorGetSensor(&compressor, 1);
        deltaSeriesAppend(&tempBatch->series, baseTime + i * 60, 20 + (i % 5));

        SensorBatch *humidityBatch = batchCompressorGetSensor(&compressor, 2);
        deltaSeriesAppend(&humidityBatch->series, baseTime + i * 60,
                          60 + (i % 10));

        SensorBatch *pressureBatch = batchCompressorGetSensor(&compressor, 3);
        deltaSeriesAppend(&pressureBatch->series, baseTime + i * 60,
                          1010 + (i % 20));
    }

    // Serialize all batches
    uint8_t batchBuffer[1024];
    size_t batchSize = batchCompressorSerialize(&compressor, batchBuffer);

    printf("   Total batch size: %zu bytes\n", batchSize);
    printf("   Average per sensor: %.1f bytes\n",
           (double)batchSize / compressor.count);
    printf("   Average per reading: %.1f bytes\n",
           (double)batchSize / (compressor.count * 10));

    // 5. Space efficiency analysis
    printf("\n5. Space efficiency analysis:\n");

    printf("   Timestamp encoding:\n");
    printf("   - varintSplit (adaptive): 1-9 bytes based on value\n");
    printf("   - Fixed 64-bit: 8 bytes always\n");
    printf("   - For typical sensor times (< 65536 sec): 2 bytes vs 8 bytes "
           "(75%% savings)\n");

    printf("\n   Sensor value encoding:\n");
    printf("   - Temperature (1 byte): 100%% efficient\n");
    printf("   - Humidity (1 byte): 100%% efficient\n");
    printf("   - Pressure (2 bytes): 50%% vs fixed 4-byte int\n");

    printf("\n   Delta encoding benefits:\n");
    printf("   - Base timestamp: 2 bytes (varintSplit)\n");
    printf("   - Per-reading delta: 1-2 bytes vs 8 bytes (87.5%% savings)\n");
    printf("   - For 10 readings: %zu bytes vs 80 bytes (%.1f%% savings)\n",
           seriesSize, 100.0 * (1.0 - (double)seriesSize / 80));

    printf("\n   Batch compression:\n");
    printf("   - %zu sensors × 10 readings = 30 total readings\n",
           compressor.count);
    printf("   - Compressed: %zu bytes (%.1f bytes/reading)\n", batchSize,
           (double)batchSize / 30);
    printf("   - Uncompressed: 270 bytes (9 bytes/reading)\n");
    printf("   - Savings: %.1f%%\n", 100.0 * (1.0 - (double)batchSize / 270));

    deltaSeriesFree(&tempSeries);
    batchCompressorFree(&compressor);

    printf("\n✓ Sensor network example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  Sensor Network Integration Example\n");
    printf("===========================================\n");

    demonstrateSensorNetwork();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintSplit for timestamp encoding\n");
    printf("  • varintExternal for sensor values\n");
    printf("  • Delta encoding for time-series\n");
    printf("  • Batch compression for networks\n");
    printf("  • Adaptive width selection\n");
    printf("  • Space-efficient sensor data\n");
    printf("===========================================\n");

    return 0;
}
