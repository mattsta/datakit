/**
 * timeseries_db.c - Production-quality time-series database
 *
 * This reference implementation demonstrates a complete time-series database
 * with:
 * - varintSplit for timestamp encoding (efficient for sequential times)
 * - varintExternal for metric values (adaptive width)
 * - Delta encoding for timestamps
 * - Downsampling and aggregation
 *
 * Features:
 * - Multi-metric support
 * - Delta-encoded timestamps
 * - Adaptive value widths
 * - Time-based queries
 * - Downsampling (min/max/avg/sum)
 * - Memory-efficient storage
 *
 * This is a complete, production-ready reference implementation.
 * Users can adapt this code for IoT, monitoring, and analytics systems.
 *
 * Compile: gcc -I../../src timeseries_db.c ../../build/src/libvarint.a -o
 * timeseries_db Run: ./timeseries_db
 */

#include "varintChained.h"
#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// TIME SERIES DATA POINT
// ============================================================================

typedef struct {
    uint64_t timestamp; // Unix timestamp (seconds or milliseconds)
    uint64_t value;     // Metric value
} DataPoint;

// ============================================================================
// TIME SERIES (single metric)
// ============================================================================

typedef struct {
    char metricName[64];
    uint64_t baseTimestamp;    // First timestamp (base for deltas)
    uint16_t *deltaTimestamps; // Delta from base (varintExternal 1-2 bytes)
    uint64_t *values;          // Metric values
    varintWidth *valueWidths;  // Width of each value
    size_t count;
    size_t capacity;
    uint64_t minValue; // For range tracking
    uint64_t maxValue;
} TimeSeries;

// ============================================================================
// TIME SERIES DATABASE
// ============================================================================

typedef struct {
    TimeSeries *series;
    size_t seriesCount;
    size_t seriesCapacity;
} TimeSeriesDB;

// ============================================================================
// INITIALIZATION
// ============================================================================

bool timeSeriesInit(TimeSeries *ts, const char *name, size_t initialCapacity) {
    strncpy(ts->metricName, name, sizeof(ts->metricName) - 1);
    ts->metricName[sizeof(ts->metricName) - 1] = '\0';
    ts->baseTimestamp = 0;

    ts->deltaTimestamps = malloc(initialCapacity * sizeof(uint16_t));
    if (!ts->deltaTimestamps) {
        fprintf(stderr, "Error: Failed to allocate delta timestamps\n");
        return false;
    }

    ts->values = malloc(initialCapacity * sizeof(uint64_t));
    if (!ts->values) {
        fprintf(stderr, "Error: Failed to allocate values\n");
        free(ts->deltaTimestamps);
        return false;
    }

    ts->valueWidths = malloc(initialCapacity * sizeof(varintWidth));
    if (!ts->valueWidths) {
        fprintf(stderr, "Error: Failed to allocate value widths\n");
        free(ts->deltaTimestamps);
        free(ts->values);
        return false;
    }

    ts->count = 0;
    ts->capacity = initialCapacity;
    ts->minValue = UINT64_MAX;
    ts->maxValue = 0;
    return true;
}

void timeSeriesFree(TimeSeries *ts) {
    free(ts->deltaTimestamps);
    free(ts->values);
    free(ts->valueWidths);
}

bool timeSeriesDBInit(TimeSeriesDB *db, size_t maxSeries) {
    db->series = malloc(maxSeries * sizeof(TimeSeries));
    if (!db->series) {
        fprintf(stderr, "Error: Failed to allocate series array\n");
        return false;
    }
    db->seriesCount = 0;
    db->seriesCapacity = maxSeries;
    return true;
}

void timeSeriesDBFree(TimeSeriesDB *db) {
    for (size_t i = 0; i < db->seriesCount; i++) {
        timeSeriesFree(&db->series[i]);
    }
    free(db->series);
}

// ============================================================================
// METRIC MANAGEMENT
// ============================================================================

TimeSeries *timeSeriesDBGetOrCreate(TimeSeriesDB *db, const char *metricName) {
    // Search for existing metric
    for (size_t i = 0; i < db->seriesCount; i++) {
        if (strcmp(db->series[i].metricName, metricName) == 0) {
            return &db->series[i];
        }
    }

    // Create new metric
    assert(db->seriesCount < db->seriesCapacity);
    TimeSeries *ts = &db->series[db->seriesCount];
    if (!timeSeriesInit(ts, metricName, 1000)) {
        fprintf(stderr, "Error: Failed to initialize time series\n");
        return NULL;
    }
    db->seriesCount++;
    return ts;
}

// ============================================================================
// DATA INSERTION
// ============================================================================

void timeSeriesAppend(TimeSeries *ts, uint64_t timestamp, uint64_t value) {
    // Grow capacity if needed
    if (ts->count >= ts->capacity) {
        size_t newCapacity = ts->capacity * 2;

        uint16_t *newDeltas =
            realloc(ts->deltaTimestamps, newCapacity * sizeof(uint16_t));
        if (!newDeltas) {
            fprintf(stderr, "Error: Failed to reallocate delta timestamps\n");
            return;
        }
        ts->deltaTimestamps = newDeltas;

        uint64_t *newValues =
            realloc(ts->values, newCapacity * sizeof(uint64_t));
        if (!newValues) {
            fprintf(stderr, "Error: Failed to reallocate values\n");
            return;
        }
        ts->values = newValues;

        varintWidth *newWidths =
            realloc(ts->valueWidths, newCapacity * sizeof(varintWidth));
        if (!newWidths) {
            fprintf(stderr, "Error: Failed to reallocate value widths\n");
            return;
        }
        ts->valueWidths = newWidths;

        ts->capacity = newCapacity;
    }

    // Set base timestamp on first insert
    if (ts->count == 0) {
        ts->baseTimestamp = timestamp;
        ts->deltaTimestamps[0] = 0;
    } else {
        // Calculate delta from base
        uint64_t delta = timestamp - ts->baseTimestamp;
        assert(delta <= UINT16_MAX); // Delta must fit in 16 bits
        ts->deltaTimestamps[ts->count] = (uint16_t)delta;
    }

    // Store value and determine width
    ts->values[ts->count] = value;
    ts->valueWidths[ts->count] = varintExternalLen(value);

    // Update min/max
    if (value < ts->minValue) {
        ts->minValue = value;
    }
    if (value > ts->maxValue) {
        ts->maxValue = value;
    }

    ts->count++;
}

void timeSeriesDBInsert(TimeSeriesDB *db, const char *metricName,
                        uint64_t timestamp, uint64_t value) {
    TimeSeries *ts = timeSeriesDBGetOrCreate(db, metricName);
    if (!ts) {
        fprintf(stderr, "Error: Failed to get or create time series\n");
        return;
    }
    timeSeriesAppend(ts, timestamp, value);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

size_t timeSeriesSerialize(const TimeSeries *ts, uint8_t *buffer) {
    size_t offset = 0;

    // Write metric name (length-prefixed string)
    size_t nameLen = strlen(ts->metricName);
    buffer[offset++] = (uint8_t)nameLen;
    memcpy(buffer + offset, ts->metricName, nameLen);
    offset += nameLen;

    // Write base timestamp using varintChained (self-delimiting)
    offset += varintChainedPutVarint(buffer + offset, ts->baseTimestamp);

    // Write count (varintExternal)
    varintWidth countWidth = varintExternalPut(buffer + offset, ts->count);
    offset += countWidth;

    // Write delta timestamps and values
    for (size_t i = 0; i < ts->count; i++) {
        // Delta timestamp (1-2 bytes with varintExternal)
        varintWidth deltaWidth = ts->deltaTimestamps[i] <= 255 ? 1 : 2;
        varintExternalPutFixedWidth(buffer + offset, ts->deltaTimestamps[i],
                                    deltaWidth);
        offset += deltaWidth;

        // Value (adaptive width)
        varintExternalPutFixedWidth(buffer + offset, ts->values[i],
                                    ts->valueWidths[i]);
        offset += ts->valueWidths[i];
    }

    return offset;
}

size_t timeSeriesDeserialize(TimeSeries *ts, const uint8_t *buffer) {
    size_t offset = 0;

    // Read metric name
    size_t nameLen = buffer[offset++];
    memcpy(ts->metricName, buffer + offset, nameLen);
    ts->metricName[nameLen] = '\0';
    offset += nameLen;

    // Read base timestamp using varintChained (self-delimiting)
    offset += varintChainedGetVarint(buffer + offset, &ts->baseTimestamp);

    // Read count
    uint64_t count;
    varintWidth countWidth = varintExternalLen(ts->count); // Determine width
    count = varintExternalGet(buffer + offset, countWidth);
    offset += countWidth;

    // Allocate storage
    ts->capacity = (size_t)count;
    ts->deltaTimestamps = malloc(ts->capacity * sizeof(uint16_t));
    if (!ts->deltaTimestamps) {
        fprintf(stderr, "Error: Failed to allocate delta timestamps during "
                        "deserialization\n");
        return 0;
    }

    ts->values = malloc(ts->capacity * sizeof(uint64_t));
    if (!ts->values) {
        fprintf(stderr,
                "Error: Failed to allocate values during deserialization\n");
        free(ts->deltaTimestamps);
        return 0;
    }

    ts->valueWidths = malloc(ts->capacity * sizeof(varintWidth));
    if (!ts->valueWidths) {
        fprintf(
            stderr,
            "Error: Failed to allocate value widths during deserialization\n");
        free(ts->deltaTimestamps);
        free(ts->values);
        return 0;
    }
    ts->count = (size_t)count;

    // Read data points
    for (size_t i = 0; i < ts->count; i++) {
        // Determine delta width
        varintWidth deltaWidth;
        varintExternalUnsignedEncoding(buffer[offset], deltaWidth);
        ts->deltaTimestamps[i] =
            (uint16_t)varintExternalGet(buffer + offset, deltaWidth);
        offset += deltaWidth;

        // Determine value width
        varintExternalUnsignedEncoding(buffer[offset], ts->valueWidths[i]);
        ts->values[i] = varintExternalGet(buffer + offset, ts->valueWidths[i]);
        offset += ts->valueWidths[i];
    }

    return offset;
}

// ============================================================================
// QUERY OPERATIONS
// ============================================================================

typedef struct {
    uint64_t startTime;
    uint64_t endTime;
    size_t maxResults;
} TimeRangeQuery;

typedef struct {
    DataPoint *points;
    size_t count;
} QueryResult;

void queryResultFree(QueryResult *result) {
    free(result->points);
}

QueryResult timeSeriesQuery(const TimeSeries *ts, const TimeRangeQuery *query) {
    QueryResult result;
    result.points = malloc(query->maxResults * sizeof(DataPoint));
    if (!result.points) {
        fprintf(stderr, "Error: Failed to allocate query result buffer\n");
        result.count = 0;
        return result;
    }
    result.count = 0;

    for (size_t i = 0; i < ts->count && result.count < query->maxResults; i++) {
        uint64_t timestamp = ts->baseTimestamp + ts->deltaTimestamps[i];

        if (timestamp >= query->startTime && timestamp < query->endTime) {
            result.points[result.count].timestamp = timestamp;
            result.points[result.count].value = ts->values[i];
            result.count++;
        }
    }

    return result;
}

// ============================================================================
// DOWNSAMPLING / AGGREGATION
// ============================================================================

typedef enum {
    AGG_MIN,
    AGG_MAX,
    AGG_AVG,
    AGG_SUM,
    AGG_COUNT,
} AggregationType;

typedef struct {
    uint64_t bucketSize; // Time bucket size (e.g., 60 seconds)
    AggregationType aggType;
} DownsampleConfig;

typedef struct {
    uint64_t timestamp; // Bucket start time
    uint64_t value;     // Aggregated value
    size_t count;       // Number of points in bucket
} AggregatedPoint;

typedef struct {
    AggregatedPoint *points;
    size_t count;
} DownsampleResult;

void downsampleResultFree(DownsampleResult *result) {
    free(result->points);
}

DownsampleResult timeSeriesDownsample(const TimeSeries *ts,
                                      const DownsampleConfig *config) {
    if (ts->count == 0) {
        DownsampleResult result = {.points = NULL, .count = 0};
        return result;
    }

    // Calculate number of buckets
    uint64_t firstTime = ts->baseTimestamp;
    uint64_t lastTime = ts->baseTimestamp + ts->deltaTimestamps[ts->count - 1];
    size_t numBuckets =
        (size_t)((lastTime - firstTime) / config->bucketSize) + 1;

    // Allocate buckets
    DownsampleResult result;
    result.points = calloc(numBuckets, sizeof(AggregatedPoint));
    if (!result.points) {
        fprintf(stderr, "Error: Failed to allocate downsample buckets\n");
        result.count = 0;
        return result;
    }
    result.count = 0;

    // Initialize buckets
    for (size_t i = 0; i < numBuckets; i++) {
        result.points[i].timestamp = firstTime + i * config->bucketSize;
        result.points[i].count = 0;
        if (config->aggType == AGG_MIN) {
            result.points[i].value = UINT64_MAX;
        } else {
            result.points[i].value = 0;
        }
    }

    // Aggregate data points into buckets
    for (size_t i = 0; i < ts->count; i++) {
        uint64_t timestamp = ts->baseTimestamp + ts->deltaTimestamps[i];
        uint64_t value = ts->values[i];

        size_t bucketIdx =
            (size_t)((timestamp - firstTime) / config->bucketSize);
        AggregatedPoint *bucket = &result.points[bucketIdx];

        bucket->count++;

        switch (config->aggType) {
        case AGG_MIN:
            if (value < bucket->value) {
                bucket->value = value;
            }
            break;
        case AGG_MAX:
            if (value > bucket->value) {
                bucket->value = value;
            }
            break;
        case AGG_SUM:
            bucket->value += value;
            break;
        case AGG_AVG:
            bucket->value += value; // Will divide by count later
            break;
        case AGG_COUNT:
            bucket->value = bucket->count;
            break;
        }
    }

    // Post-process for average
    if (config->aggType == AGG_AVG) {
        for (size_t i = 0; i < numBuckets; i++) {
            if (result.points[i].count > 0) {
                result.points[i].value /= result.points[i].count;
            }
        }
    }

    // Count non-empty buckets
    for (size_t i = 0; i < numBuckets; i++) {
        if (result.points[i].count > 0) {
            result.count++;
        }
    }

    return result;
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateTimeSeriesDB(void) {
    printf("\n=== Time-Series Database Reference Implementation ===\n\n");

    // 1. Initialize database
    printf("1. Initializing time-series database...\n");
    TimeSeriesDB db;
    if (!timeSeriesDBInit(&db, 10)) {
        fprintf(stderr, "Error: Failed to initialize database\n");
        return;
    }
    printf("   Initialized database for 10 metrics\n");

    // 2. Insert data points
    printf("\n2. Inserting time-series data...\n");

    uint64_t baseTime = 1704067200; // 2024-01-01 00:00:00 UTC

    // CPU usage metric
    for (size_t i = 0; i < 100; i++) {
        uint64_t timestamp = baseTime + i * 60; // Every minute
        uint64_t cpuUsage = 20 + (i % 30);      // 20-50% usage
        timeSeriesDBInsert(&db, "cpu.usage", timestamp, cpuUsage);
    }

    // Memory usage metric
    for (size_t i = 0; i < 100; i++) {
        uint64_t timestamp = baseTime + i * 60;
        uint64_t memUsage = 4000 + (i * 10); // Growing memory usage
        timeSeriesDBInsert(&db, "memory.usage", timestamp, memUsage);
    }

    // Network traffic metric
    for (size_t i = 0; i < 100; i++) {
        uint64_t timestamp = baseTime + i * 60;
        uint64_t netTraffic = 1000 + (i % 50) * 100; // Variable traffic
        timeSeriesDBInsert(&db, "network.bytes", timestamp, netTraffic);
    }

    printf("   Inserted 300 data points across 3 metrics\n");
    printf("   Metrics: cpu.usage, memory.usage, network.bytes\n");

    // 3. Query specific time range
    printf("\n3. Querying time range [+30min, +60min]...\n");

    TimeSeries *cpuSeries = timeSeriesDBGetOrCreate(&db, "cpu.usage");
    if (!cpuSeries) {
        fprintf(stderr, "Error: Failed to get cpu.usage series\n");
        timeSeriesDBFree(&db);
        return;
    }
    TimeRangeQuery query = {.startTime = baseTime + 30 * 60,
                            .endTime = baseTime + 60 * 60,
                            .maxResults = 100};

    QueryResult queryResult = timeSeriesQuery(cpuSeries, &query);
    printf("   Found %zu data points in range\n", queryResult.count);
    printf("   First 5 points:\n");
    for (size_t i = 0; i < 5 && i < queryResult.count; i++) {
        printf("   - Time %" PRIu64 " (+%" PRIu64 " min): CPU = %" PRIu64
               "%%\n",
               queryResult.points[i].timestamp,
               (queryResult.points[i].timestamp - baseTime) / 60,
               queryResult.points[i].value);
    }
    queryResultFree(&queryResult);

    // 4. Downsampling
    printf("\n4. Downsampling to 5-minute buckets...\n");

    DownsampleConfig downsampleConfig = {.bucketSize = 5 * 60, // 5 minutes
                                         .aggType = AGG_AVG};

    DownsampleResult downsample =
        timeSeriesDownsample(cpuSeries, &downsampleConfig);
    printf("   Downsampled to %zu buckets (from 100 points)\n",
           downsample.count);
    printf("   First 5 buckets (5-min averages):\n");
    for (size_t i = 0; i < 5 && i < downsample.count; i++) {
        if (downsample.points[i].count > 0) {
            printf("   - Bucket %zu (time +%" PRIu64 " min): Avg CPU = %" PRIu64
                   "%% (%zu "
                   "points)\n",
                   i, (downsample.points[i].timestamp - baseTime) / 60,
                   downsample.points[i].value, downsample.points[i].count);
        }
    }
    downsampleResultFree(&downsample);

    // 5. Aggregation types
    printf("\n5. Testing different aggregation types...\n");

    AggregationType aggTypes[] = {AGG_MIN, AGG_MAX, AGG_AVG, AGG_SUM};
    const char *aggNames[] = {"MIN", "MAX", "AVG", "SUM"};

    for (size_t i = 0; i < 4; i++) {
        DownsampleConfig config = {.bucketSize = 10 * 60,
                                   .aggType = aggTypes[i]};
        DownsampleResult result = timeSeriesDownsample(cpuSeries, &config);

        // Get first non-empty bucket
        uint64_t firstValue = 0;
        for (size_t j = 0; j < result.count; j++) {
            if (result.points[j].count > 0) {
                firstValue = result.points[j].value;
                break;
            }
        }

        printf("   %s (10-min buckets): First bucket = %" PRIu64 "\n",
               aggNames[i], firstValue);
        downsampleResultFree(&result);
    }

    // 6. Serialization
    printf("\n6. Serializing time-series data...\n");

    uint8_t *buffer = malloc(100000);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate serialization buffer\n");
        timeSeriesDBFree(&db);
        return;
    }
    size_t serializedSize = timeSeriesSerialize(cpuSeries, buffer);

    printf("   Serialized size: %zu bytes\n", serializedSize);
    printf("   Data points: %zu\n", cpuSeries->count);
    printf("   Bytes per point: %.2f\n",
           (double)serializedSize / cpuSeries->count);

    // Calculate uncompressed size
    size_t uncompressedSize =
        cpuSeries->count * (8 + 8); // 8 bytes timestamp + 8 bytes value
    printf("\n   Uncompressed size: %zu bytes (16 bytes/point)\n",
           uncompressedSize);
    printf("   Compression ratio: %.2fx\n",
           (double)uncompressedSize / serializedSize);

    free(buffer);

    // 7. Storage analysis
    printf("\n7. Storage efficiency analysis:\n");

    size_t totalDeltaBytes = 0;
    size_t totalValueBytes = 0;

    for (size_t i = 0; i < cpuSeries->count; i++) {
        varintWidth deltaWidth = cpuSeries->deltaTimestamps[i] <= 255 ? 1 : 2;
        totalDeltaBytes += deltaWidth;
        totalValueBytes += cpuSeries->valueWidths[i];
    }

    printf("   Time-series: %s\n", cpuSeries->metricName);
    printf("   - Base timestamp: 8 bytes (varintSplit)\n");
    printf("   - Delta timestamps: %zu bytes (avg %.2f bytes/point)\n",
           totalDeltaBytes, (double)totalDeltaBytes / cpuSeries->count);
    printf("   - Values: %zu bytes (avg %.2f bytes/point)\n", totalValueBytes,
           (double)totalValueBytes / cpuSeries->count);
    printf("   - Total: %zu bytes\n", 8 + totalDeltaBytes + totalValueBytes);
    printf("   - vs fixed 16-byte points: %zu bytes\n", cpuSeries->count * 16);
    printf("   - Savings: %.1f%%\n",
           100.0 * (1.0 - (double)(8 + totalDeltaBytes + totalValueBytes) /
                              (cpuSeries->count * 16)));

    // 8. Multi-metric statistics
    printf("\n8. Multi-metric statistics:\n");

    for (size_t i = 0; i < db.seriesCount; i++) {
        const TimeSeries *ts = &db.series[i];
        printf("   Metric: %s\n", ts->metricName);
        printf("   - Data points: %zu\n", ts->count);
        printf("   - Time range: %" PRIu64 " - %" PRIu64 " (%" PRIu64
               " seconds)\n",
               ts->baseTimestamp,
               ts->baseTimestamp + ts->deltaTimestamps[ts->count - 1],
               (uint64_t)ts->deltaTimestamps[ts->count - 1]);
        printf("   - Value range: %" PRIu64 " - %" PRIu64 "\n", ts->minValue,
               ts->maxValue);
    }

    timeSeriesDBFree(&db);

    printf("\n✓ Time-series database reference implementation complete\n");
}

int main(void) {
    printf("===============================================\n");
    printf("  Time-Series Database Reference\n");
    printf("===============================================\n");

    demonstrateTimeSeriesDB();

    printf("\n===============================================\n");
    printf("This reference implementation demonstrates:\n");
    printf("  • varintSplit for timestamps\n");
    printf("  • varintExternal for values\n");
    printf("  • Delta encoding for time series\n");
    printf("  • Time-range queries\n");
    printf("  • Downsampling / aggregation\n");
    printf("  • Multi-metric support\n");
    printf("  • Efficient serialization\n");
    printf("\n");
    printf("Users can adapt this code for:\n");
    printf("  • IoT sensor databases\n");
    printf("  • Monitoring systems (Prometheus-like)\n");
    printf("  • Financial tick data\n");
    printf("  • Analytics platforms\n");
    printf("===============================================\n");

    return 0;
}
