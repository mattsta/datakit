# XOF - XOR-Encoded Compression for Doubles

## Overview

`xof` is a **compression codec for double-precision floating-point values**
using the XOR-with-previous-value encoding from the Gorilla time-series
paper. It achieves good compression on time-series data by exploiting
temporal locality: consecutive values often share most of their bit pattern.

> Despite the name, `xof` is **not** an XOR filter (the probabilistic
> membership structure). It is a lossless float codec.

**Key Features:**

- Lossless compression for IEEE 754 double-precision floats
- Optimized for time-series data with smooth changes
- Compresses repeated and low-precision values well (~64x for a constant,
  ~14x for readings rounded to 0.1). Full-precision computed doubles,
  whose low mantissa bits change every sample, barely compress (~1.1x).
- Bit-level encoding for maximum space efficiency
- Streaming write and random read support
- Based on XOR operations for fast encoding/decoding

**Headers**: `xof.h`

**Source**: `xof.c`

**Algorithm**: Based on Gorilla: A Fast, Scalable, In-Memory Time Series Database (Facebook, 2015)

## How It Works

### Core Concept

XOF exploits the observation that consecutive double values in time-series data are often similar. By XORing consecutive values, we get a result with many leading and trailing zeros, which can be efficiently compressed.

```
Value 1:  0100000001010000... (64 bits)
Value 2:  0100000001010001... (64 bits)
XOR:      0000000000000001... (mostly zeros!)

Instead of storing 64 bits, we only store the unique bits!
```

### Compression Algorithm

```text
1. First value: Store full 64 bits (no compression possible)

2. For each subsequent value:
   a. XOR with previous value
   b. If XOR == 0: Store single '0' bit (value unchanged)
   c. If XOR has same leading/trailing zero pattern as before:
      - Store '10' (2 bits)
      - Store only the unique middle bits
   d. If XOR has different zero pattern:
      - Store '11' (2 bits)
      - Store 6 bits for # of leading zeros
      - Store 6 bits for length of unique bits
      - Store the unique bits
```

### Encoding Format

```
First value:
  [64 bits: full double value]

Subsequent values (3 cases):

Case 1: Value unchanged
  [0]

Case 2: Same zero pattern as previous XOR
  [10][N bits: unique middle section]
  (N = previous unique section length)

Case 3: New zero pattern
  [11][6 bits: leading zeros][6 bits: bit length][N bits: unique section]
```

## Data Structures

### xof Type

```c
typedef uint64_t xof;       /* Compressed data stream */
typedef uint64_t xofVal;    /* Value type (for casting) */
```

### xofWriter

```c
typedef struct xofWriter {
    xof *d;                             /* Stream, owned and grown by the writer */

    /* Write state */
    size_t usedBits;                    /* Next write position */
    int_fast32_t currentLeadingZeroes;  /* Leading zeros in last XOR */
    int_fast32_t currentTrailingZeroes; /* Trailing zeros in last XOR */
    double prevVal;                     /* Previous value for XOR */

    /* Metadata */
    size_t count;                       /* Number of values */
    size_t totalBytes;                  /* Bytes allocated for d */
} xofWriter;
```

Writer rules:

- Start from `xofWriter writer = {0};` and write every value through
  `xofWrite()`. The first value resets the XOR window, so leftover writer
  state is ignored. Never call `xofInit()` on a writer's stream or set
  `count` by hand.
- The writer owns `d`. `xofWrite()` allocates it when it is NULL, then
  grows it with `zrealloc` as values arrive, keeping `totalBytes` current
  and leaving room for readers to decode everything written.
- You may preallocate instead. `d` must then come zero-filled from the
  zmalloc family (`zcalloc`), with `totalBytes` set to its size.
- Free the stream with `zfree(writer.d)`.
- A stream never records how many values it holds; track the count
  yourself, for example `writer.count`.

### xofReader

The reader holds **only decode state** — no pointer to the stream. Every
call takes the `xof *` explicitly, which is what makes the reader safe
across reallocation of the compressed buffer.

```c
typedef struct xofReader {
    size_t bitOffset;                   /* Bit position in the stream */
    uint64_t currentValueBits;          /* Last decoded value, as bits */
    uint_fast32_t currentLeadingZeroes; /* Cached XOR block metadata */
    uint_fast32_t currentLengthOfBits;  /* Cached XOR block length */
    size_t valuesRead;                  /* How many values decoded */
} xofReader;
```

## API Functions

### Initialization

`xofInit()` and `xofAppend()` write a **raw** stream: you own the buffer
and its size, and carry the XOR window yourself. Most code should use the
writer (below), which does all of this.

```c
/* Initialize first value in stream */
void xofInit(xof *x, size_t *bitsUsed, double val);
```

### Appending Values

```c
/* Append new value to stream. '*prevLeadingZeroes' and
 * '*prevTrailingZeroes' carry the XOR window between calls and must
 * start at -1. */
void xofAppend(xof *x, size_t *bitsUsed,
               int_fast32_t *prevLeadingZeroes,
               int_fast32_t *prevTrailingZeroes,
               double prevVal, double newVal);
```

```c
#include "xof.h"

#include <stdio.h>

/* Example: compress a sequence into a raw stream */
void exampleRawStream(void) {
    /* 64 bits for the first value plus at most 78 per later one */
    xof compressed[8] = {0};
    size_t bitsUsed = 0;
    int_fast32_t leadingZeros = -1; /* no XOR window yet */
    int_fast32_t trailingZeros = -1;

    const double values[] = {25.5, 25.6, 25.7, 25.8, 25.9};

    /* Initialize with first value */
    xofInit(compressed, &bitsUsed, values[0]);
    double prev = values[0];

    /* Append remaining values */
    for (int i = 1; i < 5; i++) {
        xofAppend(compressed, &bitsUsed, &leadingZeros, &trailingZeros,
                  prev, values[i]);
        prev = values[i];
    }

    printf("Compressed %d doubles from 320 bits to %zu bits (%.1fx)\n", 5,
           bitsUsed, 320.0 / bitsUsed);
}
```

### Reading Values

```c
/* Get value at specific offset */
double xofGet(const xof *x, size_t offset);

/* Get value with cached state (faster for sequential reads) */
double xofGetCached(const xof *x, size_t *bitOffset,
                   uint64_t *currentValueBits,
                   uint_fast32_t *currentLeadingZeroes,
                   uint_fast32_t *currentLengthOfBits,
                   size_t offset);

/* Read all values at once */
bool xofReadAll(const xof *d, double *vals, size_t count);
```

```c
#include "xof.h"

#include <stdio.h>

/* Example: reading a stream of 'count' values (count tracked by you) */
void exampleReadStream(const xof *compressed, size_t count) {
    if (count > 42) {
        /* Random access (replays from the start) */
        const double value = xofGet(compressed, 42);
        printf("Value at index 42: %.2f\n", value);
    }

    /* Sequential read (faster) */
    double values[100];
    xofReadAll(compressed, values, count < 100 ? count : 100);
}
```

### Resumable Reader (O(1) Sequential Access)

`xofGet` replays the stream from the beginning on every call, so iterating
with it is quadratic. `xofReader` carries the decode state forward instead:

```c
/* Seed the reader from raw stream bytes */
void xofReaderInit(xofReader *r, const xof *x);

/* Or from a writer */
void xofReaderInitFromWriter(xofReader *r, const xofWriter *w);

/* Decode the next value; O(1), advances the reader */
double xofReaderNext(xofReader *r, const xof *x);

/* Peek the current value without advancing */
double xofReaderCurrent(const xofReader *r);

/* Batch decode up to n values into out[]; returns how many were read */
size_t xofReaderNextN(xofReader *r, const xof *x, double *out, size_t n);

/* Values left, given the total count you track separately */
size_t xofReaderRemaining(const xofReader *r, size_t totalCount);
```

```c
#include "xof.h"

/* Sum every value a writer holds, in one O(n) pass */
double exampleSum(const xofWriter *writer) {
    if (writer->count == 0) {
        return 0.0;
    }

    xofReader r;
    xofReaderInitFromWriter(&r, writer);
    double sum = xofReaderCurrent(&r); /* the first value */

    /* Remaining shrinks as you read, so re-test rather than caching it */
    while (xofReaderRemaining(&r, writer->count) > 0) {
        sum += xofReaderNext(&r, writer->d);
    }

    return sum;
}
```

The reader stores no stream pointer, so passing `x` on each call is
required — and is exactly what makes it safe if the compressed buffer is
reallocated between calls.

`xofGetCached` is the older manual form of the same idea: it takes the
individual pieces of decode state by pointer instead of a `xofReader`. New
code should use `xofReader`.

### Writer API

```c
/* Write value to xofWriter */
void xofWrite(xofWriter *const w, const double val);
```

```c
#include "datakit.h"
#include "xof.h"

#include <stdio.h>

/* Example: using xofWriter */
void exampleWriter(void) {
    /* Every field zero: the writer allocates and grows its stream, and
     * the first xofWrite() sets up the XOR window */
    xofWriter writer = {0};

    for (int i = 0; i < 1000; i++) {
        const double val = 20.5 + (i * 0.1);
        xofWrite(&writer, val);
    }

    printf("Compressed %zu values into %zu bits\n", writer.count,
           writer.usedBits);
    printf("Compression ratio: %.2fx\n",
           (writer.count * 64.0) / writer.usedBits);

    zfree(writer.d);
}
```

## Real-World Examples

### Example 1: Temperature Sensor Compression

```c
#include "datakit.h"
#include "xof.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct TempSensor {
    xofWriter writer;
    time_t startTime;
    int sampleInterval;  /* seconds */
} TempSensor;

TempSensor *tempSensorNew(void) {
    /* zcalloc zeroes the writer: it allocates and grows its stream */
    TempSensor *sensor = zcalloc(1, sizeof(*sensor));

    sensor->startTime = time(NULL);
    sensor->sampleInterval = 60;  /* 1 minute */

    return sensor;
}

void tempSensorRecord(TempSensor *sensor, double temperature) {
    /* Every value, the first included, goes through xofWrite() */
    xofWrite(&sensor->writer, temperature);
}

void tempSensorGetHistory(TempSensor *sensor, double *temps, size_t count) {
    xofReadAll(sensor->writer.d, temps, count);
}

double tempSensorGetAverage(TempSensor *sensor) {
    double *temps = zmalloc(sizeof(double) * sensor->writer.count);
    xofReadAll(sensor->writer.d, temps, sensor->writer.count);

    double sum = 0.0;
    for (size_t i = 0; i < sensor->writer.count; i++) {
        sum += temps[i];
    }

    zfree(temps);
    return sum / sensor->writer.count;
}

void tempSensorPrintStats(TempSensor *sensor) {
    size_t uncompressedBits = sensor->writer.count * 64;
    size_t compressedBits = sensor->writer.usedBits;

    printf("Temperature Sensor Statistics:\n");
    printf("  Samples: %zu\n", sensor->writer.count);
    printf("  Uncompressed: %zu bits (%.2f KB)\n",
           uncompressedBits, uncompressedBits / 8192.0);
    printf("  Compressed: %zu bits (%.2f KB)\n",
           compressedBits, compressedBits / 8192.0);
    printf("  Compression ratio: %.2fx\n",
           (double)uncompressedBits / compressedBits);
    printf("  Average temp: %.2f°C\n", tempSensorGetAverage(sensor));
}

void tempSensorFree(TempSensor *sensor) {
    if (sensor) {
        zfree(sensor->writer.d);
        zfree(sensor);
    }
}

/* Usage */
void exampleTempSensor(void) {
    TempSensor *sensor = tempSensorNew();

    /* Simulate 24 hours of readings (every minute) */
    for (int i = 0; i < 1440; i++) {
        /* Temperature varies slowly: 20°C ± 5°C */
        double temp = 20.0 + 5.0 * sin(i * M_PI / 720.0);
        temp += (rand() % 100) / 1000.0;  /* Small noise */

        tempSensorRecord(sensor, temp);
    }

    tempSensorPrintStats(sensor);

    /* Retrieve specific reading */
    double temps[1440];
    tempSensorGetHistory(sensor, temps, 1440);
    printf("Temp at noon: %.2f°C\n", temps[720]);

    tempSensorFree(sensor);
}
```

### Example 2: Stock Price Compression

```c
#include "datakit.h"
#include "xof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct StockTick {
    xofWriter prices;
    xofWriter volumes;
    time_t startTime;
    char symbol[8];
} StockTick;

StockTick *stockTickNew(const char *symbol) {
    StockTick *stock = zcalloc(1, sizeof(*stock));

    /* Preallocate 64 KB per writer to skip early growth (optional) */
    stock->prices.d = zcalloc(8192, sizeof(xof));
    stock->prices.totalBytes = 8192 * sizeof(xof);
    stock->volumes.d = zcalloc(8192, sizeof(xof));
    stock->volumes.totalBytes = 8192 * sizeof(xof);

    stock->startTime = time(NULL);
    strncpy(stock->symbol, symbol, sizeof(stock->symbol) - 1);

    return stock;
}

void stockTickRecord(StockTick *stock, double price, double volume) {
    xofWrite(&stock->prices, price);
    xofWrite(&stock->volumes, volume);
}

void stockTickGetOHLC(StockTick *stock, double *open, double *high,
                     double *low, double *close) {
    double *prices = zmalloc(sizeof(double) * stock->prices.count);
    xofReadAll(stock->prices.d, prices, stock->prices.count);

    *open = prices[0];
    *close = prices[stock->prices.count - 1];
    *high = *low = prices[0];

    for (size_t i = 1; i < stock->prices.count; i++) {
        if (prices[i] > *high) *high = prices[i];
        if (prices[i] < *low) *low = prices[i];
    }

    zfree(prices);
}

void stockTickFree(StockTick *stock) {
    if (stock) {
        zfree(stock->prices.d);
        zfree(stock->volumes.d);
        zfree(stock);
    }
}

/* Usage */
void exampleStockTick(void) {
    StockTick *aapl = stockTickNew("AAPL");

    /* Simulate 1 day of ticks (every second during market hours) */
    double basePrice = 150.00;
    for (int i = 0; i < 23400; i++) {  /* 6.5 hours */
        double price = basePrice + (rand() % 1000 - 500) / 1000.0;
        double volume = 1000.0 + rand() % 10000;
        stockTickRecord(aapl, price, volume);
    }

    /* Get OHLC */
    double open, high, low, close;
    stockTickGetOHLC(aapl, &open, &high, &low, &close);

    printf("%s - OHLC: %.2f / %.2f / %.2f / %.2f\n",
           aapl->symbol, open, high, low, close);

    /* Compression stats */
    printf("Prices: %zu values, %.2fx compression\n",
           aapl->prices.count,
           (aapl->prices.count * 64.0) / aapl->prices.usedBits);

    stockTickFree(aapl);
}
```

### Example 3: Sensor Array Compression

```c
#include "datakit.h"
#include "xof.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_SENSORS 100

typedef struct SensorArray {
    xofWriter sensors[NUM_SENSORS];
    size_t sampleCount;
} SensorArray;

SensorArray *sensorArrayNew(void) {
    /* zcalloc zeroes every writer: each allocates and grows its stream */
    SensorArray *array = zcalloc(1, sizeof(*array));
    return array;
}

void sensorArrayRecord(SensorArray *array, double values[NUM_SENSORS]) {
    for (int i = 0; i < NUM_SENSORS; i++) {
        xofWrite(&array->sensors[i], values[i]);
    }
    array->sampleCount++;
}

void sensorArrayPrintStats(SensorArray *array) {
    size_t totalUncompressed = 0;
    size_t totalCompressed = 0;

    for (int i = 0; i < NUM_SENSORS; i++) {
        totalUncompressed += array->sensors[i].count * 64;
        totalCompressed += array->sensors[i].usedBits;
    }

    printf("Sensor Array Statistics:\n");
    printf("  Sensors: %d\n", NUM_SENSORS);
    printf("  Samples per sensor: %zu\n", array->sampleCount);
    printf("  Total samples: %zu\n", array->sampleCount * NUM_SENSORS);
    printf("  Uncompressed: %.2f KB\n", totalUncompressed / 8192.0);
    printf("  Compressed: %.2f KB\n", totalCompressed / 8192.0);
    printf("  Overall compression: %.2fx\n",
           (double)totalUncompressed / totalCompressed);
}

void sensorArrayFree(SensorArray *array) {
    if (array) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            zfree(array->sensors[i].d);
        }
        zfree(array);
    }
}

/* Usage */
void exampleSensorArray(void) {
    SensorArray *array = sensorArrayNew();

    /* Record 1 hour of data (every second) */
    for (int t = 0; t < 3600; t++) {
        double values[NUM_SENSORS];

        /* Each sensor has slowly varying values */
        for (int s = 0; s < NUM_SENSORS; s++) {
            values[s] = 50.0 + 10.0 * sin((t + s * 10) * M_PI / 1800.0);
            values[s] += (rand() % 100) / 1000.0;  /* Noise */
        }

        sensorArrayRecord(array, values);
    }

    sensorArrayPrintStats(array);
    sensorArrayFree(array);
}
```

## Performance Characteristics

### Compression Ratio

Measured with `xofWrite()` over 10,000 values. What matters is how many
low mantissa bits change from value to value, not how smooth the series
looks:

| Data Pattern                        | Bits/Value | Ratio  |
| ----------------------------------- | ---------- | ------ |
| Constant                            | 1.0        | ~64x   |
| Smooth readings rounded to 0.1      | 4.7        | ~14x   |
| Integer counter `0, 1, 2, ...`      | 16         | 4x     |
| Random walk in cents                | 31         | ~2x    |
| Smooth `25 + sin(i/100)`, full bits | 55         | ~1.16x |
| Linear `20.5 + i*0.1`               | 56         | ~1.14x |
| Random values in [0, 1]             | 65         | ~0.99x |

Round or quantize values to the precision you actually need before
writing them; that is what makes the low bits repeat.

### Speed

| Operation        | Complexity | Notes                                            |
| ---------------- | ---------- | ------------------------------------------------ |
| `xofWrite`       | O(1)       | one XOR, two bit-count builtins, a store         |
| `xofReaderNext`  | O(1)       | carries decode state forward                     |
| `xofReaderNextN` | O(n)       | batch form of the above                          |
| `xofGet`         | O(offset)  | replays the stream from the start                |
| `xofGetCached`   | O(1)       | when called with monotonically advancing offsets |
| `xofReadAll`     | O(n)       | one pass over the whole stream                   |

Iterating with `xofGet(x, i)` is O(n^2). Use `xofReader` or `xofReadAll`.

### Memory Usage

```text
Storage per value:
  First value:   64 bits
  Unchanged:     1 bit
  Same window:   2 + N bits (N = the current window's width)
  New window:    2 + 6 + 6 + N bits (at most 78)

N is the span of mantissa bits that changed: a few bits for values
rounded to a coarse precision, ~50 for full-precision computed doubles.
```

## Best Practices

### 1. Use for Time-Series Data

```c
#include "datakit.h"
#include "xof.h"

#include <math.h>
#include <stdlib.h>

void exampleSeriesShapes(void) {
    /* GOOD: Smooth readings at the precision you need (0.1): ~14x */
    xofWriter rounded = {0};
    for (int i = 0; i < 1000; i++) {
        const double temp = round((25.0 + sin(i / 100.0)) * 10.0) / 10.0;
        xofWrite(&rounded, temp);
    }

    /* POOR: The same curve at full precision: ~1.16x, since the low
     * mantissa bits change every sample */
    xofWriter full = {0};
    for (int i = 0; i < 1000; i++) {
        xofWrite(&full, 25.0 + sin(i / 100.0));
    }

    /* BAD: Random values: ~1x */
    xofWriter random = {0};
    for (int i = 0; i < 1000; i++) {
        xofWrite(&random, rand() / (double)RAND_MAX);
    }

    zfree(rounded.d);
    zfree(full.d);
    zfree(random.d);
}
```

### 2. Batch Decompression

```c
#include "datakit.h"
#include "xof.h"

double exampleBatchRead(const xof *compressed, size_t count) {
    double sum = 0.0;

    /* SLOW: Random access replays from the start each time: O(n^2) */
    for (size_t i = 0; i < count; i++) {
        sum += xofGet(compressed, i);
    }

    /* FAST: Decompress all at once: O(n) */
    double *values = zmalloc(sizeof(double) * (count ? count : 1));
    xofReadAll(compressed, values, count);
    for (size_t i = 0; i < count; i++) {
        sum += values[i];
    }

    zfree(values);
    return sum;
}
```

### 3. Preallocate to Skip Growth (Optional)

A `xofWriter` grows its own stream, so preallocating only saves the
reallocations. Raw `xofInit()`/`xofAppend()` streams do not grow; size
those for the worst case yourself.

```c
#include "datakit.h"
#include "xof.h"

void examplePreallocate(xofWriter *writer, size_t count) {
    /* Worst case: 64 bits for the first value, 78 for each later one, plus
     * one slot because a read near the end may touch the next word */
    const size_t safeSize = (64 + count * 78 + 63) / 64 + 1;

    *writer = (xofWriter){0};
    writer->d = zcalloc(safeSize, sizeof(xof));
    writer->totalBytes = safeSize * sizeof(xof);
}
```

### 4. Monitor Compression Ratio

```c
#include "xof.h"

#include <stdio.h>

void exampleCheckRatio(const xofWriter *writer) {
    /* Check if data is suitable for XOF */
    if (writer->count > 0 &&
        (writer->count * 64.0) / writer->usedBits < 2.0) {
        printf("Warning: Poor compression ratio - data may not be suitable\n");
        /* Quantize the values, use another codec, or store them as is */
    }
}
```

## See Also

- [DOD.md](DOD.md) - Delta-of-delta encoding for integers
- [FLOAT16.md](FLOAT16.md) - Half-precision float encoding
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic arrays for storing compressed data

## References

- Tuomas Pelkonen et al., "Gorilla: A Fast, Scalable, In-Memory Time Series Database", VLDB 2015
- Facebook Engineering Blog: https://code.fb.com/core-data/gorilla-a-fast-scalable-in-memory-time-series-database/

## Testing

Run the xof test suite:

```bash
./src/datakit-test test xof
```

The test suite includes:

- Constant value compression
- Slowly varying values
- Random values
- Edge cases (NaN, infinity, zero)
- Round-trip accuracy verification
- Performance benchmarks
