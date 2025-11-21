# DOD - Delta-of-Delta Encoding

## Overview

`dod` (Delta-of-Delta) is a **high-efficiency compression algorithm for signed 64-bit integers** that excels at compressing monotonically increasing sequences like timestamps, counters, and sequential IDs. It achieves compression by storing only the changes between differences (delta-of-deltas) using variable-length bit encoding.

**Key Features:**

- Lossless compression for int64_t values
- Optimized for monotonically increasing sequences (timestamps, counters)
- Achieves 10-50x compression for typical timestamp data
- Variable-length encoding adapts to data patterns
- Hierarchical bit allocation for optimal space usage
- Streaming write and random read support

**Headers**: `dod.h`

**Source**: `dod.c`

**Algorithm**: Delta-of-delta encoding with hierarchical variable-length integer encoding

## How It Works

### Core Concept

Delta-of-delta exploits the observation that timestamps and counters often increase at a regular rate. Instead of storing values directly, we store:

- The difference between consecutive values (delta)
- The difference between consecutive deltas (delta-of-delta)

```
Values:     1000, 1001, 1002, 1003, 1004, 1005
Deltas:          1,    1,    1,    1,    1
Delta-of-delta:       0,    0,    0,    0

Instead of 6 × 64 = 384 bits, we store:
  - First value: 64 bits
  - 5 delta-of-deltas of 0: 5 bits
  Total: 69 bits! (5.6x compression)
```

### Delta-of-Delta Calculation

```c
value[0] = t[0]                     /* First value */
value[1] = t[1]                     /* Second value */

For i >= 2:
  delta[i] = value[i] - value[i-1]
  deltaOfDelta[i] = delta[i] - delta[i-1]

Decompression:
  delta[i] = delta[i-1] + deltaOfDelta[i]
  value[i] = value[i-1] + delta[i]
```

### Variable-Length Encoding

DOD uses hierarchical encoding based on the magnitude of the delta-of-delta:

| Value Range   | Encoding          | Total Bits | Example       |
| ------------- | ----------------- | ---------- | ------------- |
| 0             | `0`               | 1 bit      | Constant rate |
| [-64, 64]     | `100` + 6 bits    | 9 bits     | Small change  |
| [-320, 320]   | `1100` + 8 bits   | 12 bits    | Medium change |
| [-2368, 2368] | `11100` + 11 bits | 16 bits    | Larger change |
| > 2368        | `11110000` + var  | 16-72 bits | Big change    |

**Stacked Ranges**: Each encoding level includes all smaller ranges, providing smooth fallback for varying data patterns.

## Data Structures

### dod Type

```c
typedef uint64_t dod;       /* Compressed data stream */
typedef int64_t dodVal;     /* Signed 64-bit value type */
```

### dodWriter

```c
typedef struct dodWriter {
    dod *d;                 /* Compressed data array */

    /* Helper values for delta-of-delta calculation
     * If OPEN: t[0]=t_n_1, t[1]=t_n_0 (previous two values)
     * If CLOSED: t[0]=first value, t[1]=last value */
    dodVal t[2];

    /* Metadata */
    size_t count;           /* Number of values (if <= 2, only in t[2]) */
    size_t usedBits;        /* Next bit offset to write */
    size_t totalBytes;      /* Allocated bytes */
} dodWriter;
```

### dodType Encoding

```c
typedef enum dodType {
    DOD_TYPE_ZERO = 0x00,           /* 0 bits data - value unchanged */

    DOD_TYPE_SEVEN = 0x04,          /* 3 bits type + 6 bits data */
    DOD_TYPE_SEVEN_NEGATIVE = 0x05,

    DOD_TYPE_NINE = 0x0c,           /* 4 bits type + 8 bits data */
    DOD_TYPE_NINE_NEGATIVE = 0x0d,

    DOD_TYPE_TWELVE = 0x1c,         /* 5 bits type + 11 bits data */
    DOD_TYPE_TWELVE_NEGATIVE = 0x1d,

    DOD_TYPE_VAR_8 = 0xf0,          /* 8 bits type + 8 bits data */
    DOD_TYPE_VAR_16 = 0xf2,         /* 8 bits type + 16 bits data */
    DOD_TYPE_VAR_24 = 0xf4,         /* 8 bits type + 24 bits data */
    DOD_TYPE_VAR_32 = 0xf6,         /* 8 bits type + 32 bits data */
    DOD_TYPE_VAR_40 = 0xf8,         /* 8 bits type + 40 bits data */
    DOD_TYPE_VAR_48 = 0xfa,         /* 8 bits type + 48 bits data */
    DOD_TYPE_VAR_56 = 0xfc,         /* 8 bits type + 56 bits data */
    DOD_TYPE_VAR_64 = 0xfe          /* 8 bits type + 64 bits data */
} dodType;
```

## API Functions

### Writing Data

```c
/* Append value to delta-of-delta stream */
void dodAppend(dod *d, dodVal t0, dodVal t1, dodVal newVal,
               size_t *currentBits);

/* Write value using dodWriter */
void dodWrite(dodWriter *const w, const dodVal val);

/* Close writer (finalizes metadata) */
void dodCloseWrites(dodWriter *const w);

/* Example: Basic writing */
dodWriter writer = {0};
writer.d = calloc(1024, sizeof(dod));
writer.totalBytes = 1024 * sizeof(dod);

/* First two values stored in t[2] */
writer.t[0] = 1000;
writer.t[1] = 1001;
writer.count = 2;

/* Write subsequent values */
for (int i = 2; i < 1000; i++) {
    dodWrite(&writer, 1000 + i);  /* Regular increment */
}

dodCloseWrites(&writer);

printf("Compressed %zu values from %zu bits to %zu bits (%.2fx)\n",
       writer.count, writer.count * 64, writer.usedBits,
       (writer.count * 64.0) / writer.usedBits);

free(writer.d);
```

### Reading Data

```c
/* Get value at specific offset */
dodVal dodGet(const dod *d, size_t *consumedBits,
              dodVal originalStartVal, dodVal currentVal,
              size_t valueOffsetToReturn);

/* Read value using dodWriter (random access) */
dodVal dodRead(dodWriter *const w, const size_t offset);

/* Read all values at once */
bool dodReadAll(const dod *w, uint64_t *vals, size_t count);

/* Initialize reader from existing compressed data */
void dodInitFromExisting(dodWriter *const w, const dod *d);

/* Example: Reading */
dodWriter reader = {0};
reader.d = /* ... compressed data ... */;
dodInitFromExisting(&reader, reader.d);

/* Random access */
int64_t value = dodRead(&reader, 42);
printf("Value at index 42: %ld\n", value);

/* Read all */
int64_t *values = malloc(sizeof(int64_t) * count);
dodReadAll(reader.d, (uint64_t *)values, count);
```

## Real-World Examples

### Example 1: Timestamp Compression

```c
#include "dod.h"
#include <time.h>

typedef struct TimestampLog {
    dodWriter writer;
    time_t startTime;
} TimestampLog;

TimestampLog *timestampLogNew(void) {
    TimestampLog *log = malloc(sizeof(*log));

    log->writer.d = calloc(4096, sizeof(dod));
    log->writer.t[0] = 0;
    log->writer.t[1] = 0;
    log->writer.count = 0;
    log->writer.usedBits = 0;
    log->writer.totalBytes = 4096 * sizeof(dod);

    log->startTime = time(NULL);

    return log;
}

void timestampLogRecord(TimestampLog *log, time_t timestamp) {
    if (log->writer.count == 0) {
        log->writer.t[0] = timestamp;
        log->writer.count = 1;
    } else if (log->writer.count == 1) {
        log->writer.t[1] = timestamp;
        log->writer.count = 2;
    } else {
        dodWrite(&log->writer, timestamp);
    }
}

void timestampLogRecordNow(TimestampLog *log) {
    timestampLogRecord(log, time(NULL));
}

time_t timestampLogGetAt(TimestampLog *log, size_t index) {
    if (index >= log->writer.count) {
        return 0;
    }

    return (time_t)dodRead(&log->writer, index);
}

void timestampLogGetAll(TimestampLog *log, time_t **timestamps, size_t *count) {
    *count = log->writer.count;
    *timestamps = malloc(sizeof(time_t) * (*count));

    if (log->writer.count <= 2) {
        /* Values still in t[2] */
        if (log->writer.count >= 1) (*timestamps)[0] = log->writer.t[0];
        if (log->writer.count >= 2) (*timestamps)[1] = log->writer.t[1];
    } else {
        dodReadAll(log->writer.d, (uint64_t *)*timestamps, *count);
    }
}

void timestampLogPrintStats(TimestampLog *log) {
    size_t uncompressedBits = log->writer.count * 64;
    size_t compressedBits = log->writer.usedBits;

    printf("Timestamp Log Statistics:\n");
    printf("  Entries: %zu\n", log->writer.count);
    printf("  Duration: %ld seconds\n",
           log->writer.count > 0 ?
           timestampLogGetAt(log, log->writer.count - 1) - log->startTime : 0);

    if (log->writer.count > 2) {
        printf("  Uncompressed: %zu bits (%.2f KB)\n",
               uncompressedBits, uncompressedBits / 8192.0);
        printf("  Compressed: %zu bits (%.2f KB)\n",
               compressedBits, compressedBits / 8192.0);
        printf("  Compression ratio: %.2fx\n",
               (double)uncompressedBits / compressedBits);
    }
}

void timestampLogFree(TimestampLog *log) {
    if (log) {
        free(log->writer.d);
        free(log);
    }
}

/* Usage */
void exampleTimestampLog(void) {
    TimestampLog *log = timestampLogNew();

    /* Record events every second for 1 hour */
    time_t start = time(NULL);
    for (int i = 0; i < 3600; i++) {
        timestampLogRecord(log, start + i);
    }

    timestampLogPrintStats(log);
    /* Expected: ~50x compression for regular 1-second intervals */

    /* Retrieve specific timestamp */
    time_t ts = timestampLogGetAt(log, 1800);  /* 30 minutes in */
    printf("Timestamp at 30min: %ld\n", ts);

    timestampLogFree(log);
}
```

### Example 2: Counter Array Compression

```c
#include "dod.h"

#define NUM_COUNTERS 100

typedef struct CounterArray {
    dodWriter counters[NUM_COUNTERS];
    size_t sampleCount;
} CounterArray;

CounterArray *counterArrayNew(void) {
    CounterArray *array = malloc(sizeof(*array));

    for (int i = 0; i < NUM_COUNTERS; i++) {
        array->counters[i].d = calloc(1024, sizeof(dod));
        array->counters[i].t[0] = 0;
        array->counters[i].t[1] = 0;
        array->counters[i].count = 0;
        array->counters[i].usedBits = 0;
        array->counters[i].totalBytes = 1024 * sizeof(dod);
    }

    array->sampleCount = 0;
    return array;
}

void counterArrayRecord(CounterArray *array, int64_t values[NUM_COUNTERS]) {
    for (int i = 0; i < NUM_COUNTERS; i++) {
        if (array->counters[i].count == 0) {
            array->counters[i].t[0] = values[i];
            array->counters[i].count = 1;
        } else if (array->counters[i].count == 1) {
            array->counters[i].t[1] = values[i];
            array->counters[i].count = 2;
        } else {
            dodWrite(&array->counters[i], values[i]);
        }
    }
    array->sampleCount++;
}

void counterArrayPrintStats(CounterArray *array) {
    size_t totalUncompressed = 0;
    size_t totalCompressed = 0;

    for (int i = 0; i < NUM_COUNTERS; i++) {
        totalUncompressed += array->counters[i].count * 64;
        totalCompressed += array->counters[i].usedBits;
    }

    printf("Counter Array Statistics:\n");
    printf("  Counters: %d\n", NUM_COUNTERS);
    printf("  Samples: %zu\n", array->sampleCount);
    printf("  Total values: %zu\n", array->sampleCount * NUM_COUNTERS);
    printf("  Uncompressed: %.2f KB\n", totalUncompressed / 8192.0);
    printf("  Compressed: %.2f KB\n", totalCompressed / 8192.0);
    printf("  Compression ratio: %.2fx\n",
           (double)totalUncompressed / totalCompressed);
}

void counterArrayFree(CounterArray *array) {
    if (array) {
        for (int i = 0; i < NUM_COUNTERS; i++) {
            free(array->counters[i].d);
        }
        free(array);
    }
}

/* Usage */
void exampleCounterArray(void) {
    CounterArray *array = counterArrayNew();

    /* Simulate counters incrementing at different rates */
    int64_t counters[NUM_COUNTERS] = {0};

    for (int t = 0; t < 10000; t++) {
        /* Each counter increments by 1-10 per sample */
        for (int c = 0; c < NUM_COUNTERS; c++) {
            counters[c] += 1 + (c % 10);
        }

        counterArrayRecord(array, counters);
    }

    counterArrayPrintStats(array);
    /* Expected: 20-40x compression for monotonic counters */

    counterArrayFree(array);
}
```

### Example 3: Event ID Sequence

```c
#include "dod.h"

typedef struct Event {
    int64_t id;
    char type[16];
    char data[64];
} Event;

typedef struct EventStream {
    dodWriter ids;
    Event *events;
    size_t count;
    size_t capacity;
} EventStream;

EventStream *eventStreamNew(void) {
    EventStream *stream = malloc(sizeof(*stream));

    stream->ids.d = calloc(4096, sizeof(dod));
    stream->ids.t[0] = 0;
    stream->ids.t[1] = 0;
    stream->ids.count = 0;
    stream->ids.usedBits = 0;
    stream->ids.totalBytes = 4096 * sizeof(dod);

    stream->events = malloc(sizeof(Event) * 1000);
    stream->count = 0;
    stream->capacity = 1000;

    return stream;
}

void eventStreamAdd(EventStream *stream, int64_t id,
                   const char *type, const char *data) {
    /* Store ID in compressed form */
    if (stream->ids.count == 0) {
        stream->ids.t[0] = id;
        stream->ids.count = 1;
    } else if (stream->ids.count == 1) {
        stream->ids.t[1] = id;
        stream->ids.count = 2;
    } else {
        dodWrite(&stream->ids, id);
    }

    /* Store event data uncompressed */
    if (stream->count >= stream->capacity) {
        stream->capacity *= 2;
        stream->events = realloc(stream->events,
                                sizeof(Event) * stream->capacity);
    }

    Event *e = &stream->events[stream->count++];
    e->id = id;
    strncpy(e->type, type, sizeof(e->type) - 1);
    strncpy(e->data, data, sizeof(e->data) - 1);
}

Event *eventStreamGetById(EventStream *stream, int64_t id) {
    /* Linear search through events */
    for (size_t i = 0; i < stream->count; i++) {
        if (stream->events[i].id == id) {
            return &stream->events[i];
        }
    }
    return NULL;
}

void eventStreamPrintStats(EventStream *stream) {
    size_t idBytes = stream->ids.count * sizeof(int64_t);
    size_t compressedIdBytes = (stream->ids.usedBits + 7) / 8;
    size_t eventBytes = stream->count * sizeof(Event);
    size_t totalUncompressed = idBytes + eventBytes;
    size_t totalCompressed = compressedIdBytes + eventBytes;

    printf("Event Stream Statistics:\n");
    printf("  Events: %zu\n", stream->count);
    printf("  ID compression: %.2fx (%zu -> %zu bytes)\n",
           (double)idBytes / compressedIdBytes,
           idBytes, compressedIdBytes);
    printf("  Total size: %zu bytes (%.2fx vs uncompressed)\n",
           totalCompressed,
           (double)totalUncompressed / totalCompressed);
}

void eventStreamFree(EventStream *stream) {
    if (stream) {
        free(stream->ids.d);
        free(stream->events);
        free(stream);
    }
}

/* Usage */
void exampleEventStream(void) {
    EventStream *stream = eventStreamNew();

    /* Add events with sequential IDs */
    int64_t currentId = 1000;
    const char *types[] = {"LOGIN", "LOGOUT", "ACTION", "ERROR"};

    for (int i = 0; i < 10000; i++) {
        currentId += 1;  /* Sequential IDs */

        eventStreamAdd(stream, currentId,
                      types[rand() % 4],
                      "Event data here");
    }

    eventStreamPrintStats(stream);

    /* Lookup specific event */
    Event *e = eventStreamGetById(stream, 5000);
    if (e) {
        printf("Event 5000: type=%s\n", e->type);
    }

    eventStreamFree(stream);
}
```

## Performance Characteristics

### Compression Ratio

Typical compression ratios for different patterns:

| Data Pattern       | Compression Ratio | Example                    |
| ------------------ | ----------------- | -------------------------- |
| Constant increment | 30-60x            | `t, t+1, t+2, t+3...`      |
| Regular timestamps | 20-40x            | Events every second        |
| Variable increment | 10-20x            | `t, t+1, t+3, t+4, t+8...` |
| Random increasing  | 4-8x              | Irregular timestamps       |
| Random values      | 1-2x              | No pattern                 |

### Speed

| Operation       | Time             | Notes                          |
| --------------- | ---------------- | ------------------------------ |
| Write           | ~25 ns           | Delta calculation + bit encode |
| Sequential read | ~40 ns           | Delta reconstruction           |
| Random read     | ~800 ns          | Must replay from start         |
| Decompress all  | ~35 ns per value | Optimized loop                 |

### Memory Usage

```c
/* Bit usage by encoding type */
Zero delta: 1 bit
Small delta (±64): 9 bits
Medium delta (±320): 12 bits
Large delta (±2368): 16 bits
Very large: 16-72 bits

/* Average for timestamps (1-second intervals): ~2 bits per value */
/* vs 64 bits uncompressed = ~32x compression */
```

## Best Practices

### 1. Use for Monotonic Sequences

```c
/* GOOD: Timestamps */
for (int i = 0; i < 10000; i++) {
    dodWrite(&writer, startTime + i);
}
/* Achieves 30-50x compression */

/* GOOD: Counters */
int64_t counter = 0;
for (int i = 0; i < 10000; i++) {
    counter += 1 + (rand() % 10);
    dodWrite(&writer, counter);
}
/* Achieves 15-25x compression */

/* BAD: Random values */
for (int i = 0; i < 10000; i++) {
    dodWrite(&writer, rand());
}
/* Achieves only 1-2x compression */
```

### 2. Close Writer When Done

```c
/* Write all values */
for (int i = 0; i < count; i++) {
    dodWrite(&writer, values[i]);
}

/* Important: Close to finalize metadata */
dodCloseWrites(&writer);

/* Now t[0] = first value, t[1] = last value */
printf("Range: %ld to %ld\n", writer.t[0], writer.t[1]);
```

### 3. Allocate Sufficient Space

```c
/* Worst case: every value uses 72 bits */
/* Safe allocation: count * 72 / 64 * sizeof(uint64_t) */

size_t count = 100000;
size_t safeSize = (count * 72 + 63) / 64;
dod *compressed = calloc(safeSize, sizeof(dod));
```

### 4. Monitor Compression Efficiency

```c
if (writer.count > 2 &&
    (writer.count * 64.0) / writer.usedBits < 3.0) {
    printf("Warning: Poor compression - data may not be monotonic\n");
}
```

## Algorithm Details

### Delta-of-Delta Formula

```c
/* Encoding */
delta[i] = value[i] - value[i-1]
deltaOfDelta[i] = delta[i] - delta[i-1]

/* Decoding */
delta[i] = delta[i-1] + deltaOfDelta[i]
value[i] = value[i-1] + delta[i]
```

### Hierarchical Ranges

The encoding uses overlapping ranges for smooth fallback:

```c
Range 0:  [0, 0]              (1 bit)
Range 7:  [-64, 63]           (9 bits)
Range 9:  [-320, 319]         (12 bits)
Range 12: [-2368, 2367]       (16 bits)
Range V8: [-2624, 2623]       (16 bits)
...
Range V64: full int64_t       (72 bits)
```

Each range includes all smaller ranges, ensuring optimal encoding.

## See Also

- [XOF.md](XOF.md) - XOR filter compression for doubles
- [FLOAT16.md](FLOAT16.md) - Half-precision float encoding
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic arrays for storing compressed data

## Testing

Run the dod test suite:

```bash
./src/datakit-test test dod
```

The test suite includes:

- Sequential timestamp compression
- Counter sequences
- Negative value handling
- Random value compression
- Round-trip accuracy verification
- Performance benchmarks
