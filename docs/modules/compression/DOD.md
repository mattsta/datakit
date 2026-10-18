# DOD - Delta-of-Delta Encoding

## Overview

`dod` (Delta-of-Delta) is a **high-efficiency compression algorithm for signed 64-bit integers** that excels at compressing monotonically increasing sequences like timestamps, counters, and sequential IDs. It achieves compression by storing only the changes between differences (delta-of-deltas) using variable-length bit encoding.

**Key Features:**

- Lossless compression for int64_t values
- Optimized for monotonically increasing sequences (timestamps, counters)
- Up to ~64x compression for timestamps at a regular interval (measured
  1.00 bit per value), less as the interval jitters
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

Instead of 6 × 64 = 384 bits, a dodWrite() stream stores:
  - Header with the first two values: 48 bits
  - 4 delta-of-deltas of 0: 4 bits
  Total: 52 bits (7.4x compression; measured ~64x on 10,000-value runs)
```

### Delta-of-Delta Calculation

```text
value[0] = t[0]                     (first value)
value[1] = t[1]                     (second value)

For i >= 2:
  delta[i] = value[i] - value[i-1]
  deltaOfDelta[i] = delta[i] - delta[i-1]

Decompression:
  delta[i] = delta[i-1] + deltaOfDelta[i]
  value[i] = value[i-1] + delta[i]
```

All of this arithmetic is done modulo 2^64 (in `uint64_t`). Every `int64_t`
sequence round-trips exactly, including `INT64_MIN`/`INT64_MAX` and steps
whose deltas do not fit in `int64_t`, such as alternating between the two
ends. The stored delta-of-delta is the true one reduced to its `int64_t`
residue, which equals the true value whenever that fits.

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
    dod *d;                 /* Writer stream, owned and grown by the writer */

    /* Helper values for delta-of-delta calculation
     * If OPEN: t[0]=t_n_1, t[1]=t_n_0 (previous two values)
     * If CLOSED: t[0]=first value, t[1]=last value */
    dodVal t[2];

    /* Metadata */
    size_t count;           /* Number of values (if < 2, only in t[2]) */
    size_t usedBits;        /* Next bit offset to write */
    size_t totalBytes;      /* Bytes allocated for d */
} dodWriter;
```

### Stream Kinds

There are two kinds of dod stream:

- **Raw streams** come from `dodAppend()`. They hold only delta-of-delta
  entries, starting at bit 0. The caller keeps the first two values and
  passes them to `dodGet()` or `dodReaderInit()`.
- **Writer streams** come from `dodWrite()`. They start with a header that
  holds the first two values, and the entries follow it. `dodRead()`,
  `dodReadAll()`, `dodInitFromExisting()`, `dodCloseWrites()` and
  `dodReaderInitFromWriter()` read that header, so a writer stream decodes
  from the stream alone.

The header is at most `DOD_WRITER_HEADER_MAX_BITS` (130) bits:

| Code | t0      | t1                                     | Bits |
| ---- | ------- | -------------------------------------- | ---- |
| `00` | 32 bits | t1 - t0, sign-magnitude, 14 bits       | 48   |
| `01` | 32 bits | t1 - t0, sign-magnitude, 16 bits       | 50   |
| `10` | 64 bits | t1 - t0 (mod 2^64), sign-magnitude, 45 | 111  |
| `11` | 64 bits | t1, 64 bits                            | 130  |

The 32-bit forms need t0 in `[0, UINT32_MAX]`. The writer always picks
the smallest form that holds the pair.

Writer rules:

- The writer owns `d`. Start with `dodInit()` (`d` NULL) and `dodWrite()`
  allocates the stream on the second value, which writes the header. After
  that it grows the stream with `zrealloc` as values arrive, keeping
  `totalBytes` current and leaving room for readers to decode everything
  written.
- You may preallocate instead. `d` must then come zero-filled from the
  zmalloc family (`zcalloc`), with `totalBytes` set to its size.
- Free the stream with `zfree(w.d)`.
- Write every value through `dodWrite()`. Never fill `t[]` or `count` by
  hand, or the header is never written.
- A stream never records how many values it holds; track the count
  yourself, for example `writer.count`.
- Once `dodCloseWrites()` has run, write nothing more to that writer.

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
    DOD_TYPE_VAR_8_NEGATIVE = 0xf1,
    DOD_TYPE_VAR_16 = 0xf2,         /* 8 bits type + 16 bits data */
    DOD_TYPE_VAR_16_NEGATIVE = 0xf3,
    DOD_TYPE_VAR_24 = 0xf4,         /* 8 bits type + 24 bits data */
    DOD_TYPE_VAR_24_NEGATIVE = 0xf5,
    DOD_TYPE_VAR_32 = 0xf6,         /* 8 bits type + 32 bits data */
    DOD_TYPE_VAR_32_NEGATIVE = 0xf7,
    DOD_TYPE_VAR_40 = 0xf8,         /* 8 bits type + 40 bits data */
    DOD_TYPE_VAR_40_NEGATIVE = 0xf9,
    DOD_TYPE_VAR_48 = 0xfa,         /* 8 bits type + 48 bits data */
    DOD_TYPE_VAR_48_NEGATIVE = 0xfb,
    DOD_TYPE_VAR_56 = 0xfc,         /* 8 bits type + 56 bits data */
    DOD_TYPE_VAR_56_NEGATIVE = 0xfd,
    DOD_TYPE_VAR_64 = 0xfe,         /* 8 bits type + 64 bits data */
    DOD_TYPE_VAR_64_NEGATIVE = 0xff
} __attribute__((packed)) dodType;
```

Every non-zero type comes in a positive/negative pair: the low bit of the
designator carries the sign, and the payload stores the magnitude. The
designator patterns are chosen so the narrow types (3/4/5-bit designators)
cannot be mistaken for the wide ones — each narrow designator ends in a `0`
where the wide family has a `1`.

`dodType` is private to `dod.c`; it is shown here to explain the wire format,
not as a public API.

## API Functions

### Writing Data

```c
/* Append value to delta-of-delta stream */
void dodAppend(dod *d, dodVal t0, dodVal t1, dodVal newVal,
               size_t *currentBits);

/* Write value using dodWriter */
void dodWrite(dodWriter *const w, const dodVal val);

/* Close writer: t[] becomes (first value, last value) */
void dodCloseWrites(dodWriter *const w);
```

```c
#include "datakit.h"
#include "dod.h"

#include <stdio.h>

/* Example: basic writing */
void exampleBasicWriting(void) {
    dodWriter writer;
    dodInit(&writer);

    /* Every value goes through dodWrite(): the second one allocates the
     * stream and writes the header, later ones grow it as needed */
    for (int i = 0; i < 1000; i++) {
        dodWrite(&writer, 1000 + i); /* Regular increment */
    }

    dodCloseWrites(&writer);

    printf("Compressed %zu values from %zu bits to %zu bits (%.2fx)\n",
           writer.count, writer.count * 64, writer.usedBits,
           (writer.count * 64.0) / writer.usedBits);

    zfree(writer.d);
}
```

### Reading Data

```c
/* Decode from bit *consumedBits: after 'valueOffsetToReturn' values, given
 * the two values before that position (raw stream: start at bit 0 with
 * the first two values; writer stream: start at the header's end) */
dodVal dodGet(const dod *d, size_t *consumedBits,
              dodVal originalStartVal, dodVal currentVal,
              size_t valueOffsetToReturn);

/* Value at index 'offset' (0 = first) of a writer, open or closed */
dodVal dodRead(dodWriter *const w, const size_t offset);

/* Read the first 'count' values of a writer stream (room for at least 2) */
bool dodReadAll(const dod *w, uint64_t *vals, size_t count);

/* Read a writer stream's header: t[] = first two values, usedBits = the
 * header's end, count = 2. Does not change w->d. */
void dodInitFromExisting(dodWriter *const w, const dod *d);
```

```c
#include "datakit.h"
#include "dod.h"

#include <inttypes.h>
#include <stdio.h>

/* Example: reading a writer stream you only have the bytes of, plus the
 * value count you tracked ('stream' holds at least two values) */
void exampleReadStream(dod *stream, size_t count) {
    dodWriter reader = {0};
    reader.d = stream;
    dodInitFromExisting(&reader, stream);

    if (count > 42) {
        /* Random access */
        int64_t value = dodRead(&reader, 42);
        printf("Value at index 42: %" PRId64 "\n", value);

        /* Value 42 again, decoded with dodGet() from the header's end */
        size_t bits = reader.usedBits;
        value = dodGet(stream, &bits, reader.t[0], reader.t[1], 41);
        printf("Value at index 42: %" PRId64 "\n", value);
    }

    /* Read all; the stream does not record its count */
    int64_t *values = zmalloc(sizeof(int64_t) * (count < 2 ? 2 : count));
    dodReadAll(stream, (uint64_t *)values, count);
    zfree(values);
}
```

### Resumable Reader (O(1) Sequential Access)

`dodRead` is a **random**-access call: it replays the stream from the start
every time, so walking a stream with it is quadratic. For sequential reads
use `dodReader`, which carries the decode state forward:

```c
typedef struct dodReader {
    size_t consumedBits; /* Bit position in the stream */
    dodVal t0;           /* Second-to-last decoded value */
    dodVal t1;           /* Last decoded value */
    size_t valuesRead;   /* How many values decoded so far */
} dodReader;

/* Raw stream: seed from the first two values, which you kept */
void dodReaderInit(dodReader *r, dodVal firstVal, dodVal secondVal);

/* Writer stream: seed from the writer (open or closed); with two or more
 * values written it starts after the header */
void dodReaderInitFromWriter(dodReader *r, const dodWriter *w);

/* Decode the next value; O(1), advances the reader */
dodVal dodReaderNext(dodReader *r, const dod *d);

/* Peek the current value (returns t1) without advancing */
dodVal dodReaderCurrent(const dodReader *r);

/* Batch decode up to n values into out[]; returns how many were read */
size_t dodReaderNextN(dodReader *r, const dod *d, dodVal *out, size_t n);

/* Values left, given the total count you are tracking separately */
size_t dodReaderRemaining(const dodReader *r, size_t totalCount);
```

```c
#include "dod.h"

/* Sequential walk in O(n) total instead of O(n^2). The reader starts with
 * the first two values already known; the loop yields the rest. */
int64_t exampleSumAfterFirstTwo(const dodWriter *writer) {
    dodReader r;
    dodReaderInitFromWriter(&r, writer);

    /* Remaining shrinks as you read, so re-test it rather than
     * caching it as a loop bound. */
    int64_t sum = 0;
    while (dodReaderRemaining(&r, writer->count) > 0) {
        const dodVal v = dodReaderNext(&r, writer->d);
        sum += v;
    }

    return sum;
}
```

The stream carries no count of its own, so `dodReaderRemaining` needs the
total you already track (`writer.count`).

## Real-World Examples

### Example 1: Timestamp Compression

```c
#include "datakit.h"
#include "dod.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

typedef struct TimestampLog {
    dodWriter writer;
    time_t startTime;
} TimestampLog;

TimestampLog *timestampLogNew(void) {
    TimestampLog *log = zcalloc(1, sizeof(*log));

    /* The writer allocates and grows its stream on demand */
    dodInit(&log->writer);

    log->startTime = time(NULL);

    return log;
}

void timestampLogRecord(TimestampLog *log, time_t timestamp) {
    /* dodWrite() keeps the first value in t[0] and writes the header on
     * the second, so every value goes through it */
    dodWrite(&log->writer, timestamp);
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
    *timestamps = zmalloc(sizeof(time_t) * (*count));

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
    printf("  Duration: %" PRId64 " seconds\n",
           log->writer.count > 0
               ? (int64_t)(timestampLogGetAt(log, log->writer.count - 1) -
                           log->startTime)
               : (int64_t)0);

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
        zfree(log->writer.d);
        zfree(log);
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
    /* Measured for this run: 1.01 bits per value, ~63x; a constant
     * 1-second step costs 1 bit per value */

    /* Retrieve specific timestamp */
    time_t ts = timestampLogGetAt(log, 1800);  /* 30 minutes in */
    printf("Timestamp at 30min: %" PRId64 "\n", (int64_t)ts);

    timestampLogFree(log);
}
```

### Example 2: Counter Array Compression

```c
#include "datakit.h"
#include "dod.h"

#include <stdio.h>

#define NUM_COUNTERS 100

typedef struct CounterArray {
    dodWriter counters[NUM_COUNTERS];
    size_t sampleCount;
} CounterArray;

CounterArray *counterArrayNew(void) {
    CounterArray *array = zcalloc(1, sizeof(*array));

    for (int i = 0; i < NUM_COUNTERS; i++) {
        /* Preallocate 8 KB each (optional: writers grow on demand) */
        dodInit(&array->counters[i]);
        array->counters[i].d = zcalloc(1024, sizeof(dod));
        array->counters[i].totalBytes = 1024 * sizeof(dod);
    }

    array->sampleCount = 0;
    return array;
}

void counterArrayRecord(CounterArray *array, int64_t values[NUM_COUNTERS]) {
    for (int i = 0; i < NUM_COUNTERS; i++) {
        dodWrite(&array->counters[i], values[i]);
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
            zfree(array->counters[i].d);
        }
        zfree(array);
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
    /* Measured for this run: 1.00 bits per value, ~64x; each counter's
     * step is constant, so every value after the first two costs 1 bit */

    counterArrayFree(array);
}
```

### Example 3: Event ID Sequence

```c
#include "datakit.h"
#include "dod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    EventStream *stream = zcalloc(1, sizeof(*stream));

    /* The writer allocates and grows its stream on demand */
    dodInit(&stream->ids);

    stream->events = zmalloc(sizeof(Event) * 1000);
    stream->count = 0;
    stream->capacity = 1000;

    return stream;
}

void eventStreamAdd(EventStream *stream, int64_t id,
                   const char *type, const char *data) {
    /* Store ID in compressed form */
    dodWrite(&stream->ids, id);

    /* Store event data uncompressed */
    if (stream->count >= stream->capacity) {
        stream->capacity *= 2;
        stream->events = zrealloc(stream->events,
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
        zfree(stream->ids.d);
        zfree(stream->events);
        zfree(stream);
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

Each value after the first two costs the bits of its delta-of-delta's
range. Measured with `dodWrite()`, 10,000 values per pattern (header
included):

| Data Pattern                                   | Bits/Value | Ratio vs 64-bit |
| ---------------------------------------------- | ---------- | --------------- |
| Constant step `t, t+1, t+2, ...`               | 1.00       | ~64x            |
| Counter stepping by `1 + rand() % 10`          | 8.2        | ~7.8x           |
| Delta-of-delta uniform in [-64, 64]            | 8.9        | ~7.2x           |
| Step 1000 with jitter uniform in [-64, 64]     | 9.7        | ~6.6x           |
| Delta-of-delta uniform in [-2368, 2368]        | 15.4       | ~4.2x           |
| `rand()` values (31-bit)                       | 40         | ~1.6x           |
| Uniform 64-bit values                          | 71.9       | ~0.89x          |

### Speed

| Operation            | Complexity | Notes                               |
| -------------------- | ---------- | ----------------------------------- |
| `dodWrite`           | O(1)       | delta math plus a bit-packed append |
| `dodReaderNext`      | O(1)       | carries decode state forward        |
| `dodReaderNextN`     | O(n)       | batch form of the above             |
| `dodRead` / `dodGet` | O(offset)  | replays the stream from the start   |
| `dodReadAll`         | O(n)       | one pass over the whole stream      |

The only operation with a bad shape is `dodRead`: it is a _random_-access
call and re-decodes from the beginning each time. Iterating a stream with
`dodRead(w, i)` in a loop is O(n^2) — use `dodReader` or `dodReadAll`.

### Memory Usage

```text
Bit usage by delta-of-delta range:
  0                1 bit
  within ±64       9 bits
  within ±320      12 bits
  within ±2368     16 bits
  larger           16-72 bits

Timestamps at a constant 1-second interval: 1 bit per value after the
first two, plus a 48-130 bit header (measured 1.00 bits per value over
10,000 values, ~64x against 64-bit values).
```

## Best Practices

### 1. Use for Monotonic Sequences

```c
#include "datakit.h"
#include "dod.h"

#include <stdlib.h>

void exampleSequenceShapes(int64_t startTime) {
    /* GOOD: Timestamps at a constant step: measured 1.00 bits per value,
     * ~64x */
    dodWriter timestamps;
    dodInit(&timestamps);
    for (int i = 0; i < 10000; i++) {
        dodWrite(&timestamps, startTime + i);
    }

    /* OK: Counters with a step of 1-10: the delta-of-delta stays within
     * ±9, measured 8.2 bits per value, ~7.8x */
    dodWriter counters;
    dodInit(&counters);
    int64_t counter = 0;
    for (int i = 0; i < 10000; i++) {
        counter += 1 + (rand() % 10);
        dodWrite(&counters, counter);
    }

    /* BAD: Random values: measured 40 bits per value, ~1.6x */
    dodWriter random;
    dodInit(&random);
    for (int i = 0; i < 10000; i++) {
        dodWrite(&random, rand());
    }

    zfree(timestamps.d);
    zfree(counters.d);
    zfree(random.d);
}
```

### 2. Close Writer When Done

```c
#include "datakit.h"
#include "dod.h"

#include <inttypes.h>
#include <stdio.h>

void exampleCloseWriter(const int64_t *values, size_t count) {
    dodWriter writer;
    dodInit(&writer);

    /* Write all values */
    for (size_t i = 0; i < count; i++) {
        dodWrite(&writer, values[i]);
    }

    /* Close: t[] switches to (first value, last value); write nothing
     * more to this writer afterwards */
    dodCloseWrites(&writer);

    printf("Range: %" PRId64 " to %" PRId64 "\n", writer.t[0], writer.t[1]);

    zfree(writer.d);
}
```

### 3. Preallocate to Skip Growth (Optional)

A `dodWriter` grows its own stream, so preallocating only saves the
reallocations. Raw `dodAppend()` streams do not grow; size those for the
worst case yourself.

```c
#include "datakit.h"
#include "dod.h"

void examplePreallocate(dodWriter *writer, size_t count) {
    /* Worst case: the header plus 72 bits for every value after the first
     * two, plus one slot of slack because a read near the end may touch
     * the next word */
    const size_t safeSize =
        (DOD_WRITER_HEADER_MAX_BITS + count * 72 + 63) / 64 + 1;

    dodInit(writer);
    writer->d = zcalloc(safeSize, sizeof(dod));
    writer->totalBytes = safeSize * sizeof(dod);
}
```

### 4. Monitor Compression Efficiency

```c
#include "dod.h"

#include <stdio.h>

void exampleCheckRatio(const dodWriter *writer) {
    if (writer->count > 2 &&
        (writer->count * 64.0) / writer->usedBits < 3.0) {
        printf("Warning: Poor compression - data may not be monotonic\n");
    }
}
```

## Algorithm Details

### Delta-of-Delta Formula

```text
Encoding (modulo 2^64):
delta[i] = value[i] - value[i-1]
deltaOfDelta[i] = delta[i] - delta[i-1]

Decoding (modulo 2^64):
delta[i] = delta[i-1] + deltaOfDelta[i]
value[i] = value[i-1] + delta[i]
```

### Hierarchical Ranges

The encoding uses overlapping ranges for smooth fallback:

```text
Range 0:  [0, 0]              (1 bit)
Range 7:  [-64, 64]           (9 bits)
Range 9:  [-320, 320]         (12 bits)
Range 12: [-2368, 2368]       (16 bits)
Range V8: [-2624, 2624]       (16 bits)
...
Range V64: full int64_t       (72 bits)
```

The payload of each range stores the magnitude minus the smallest
magnitude the range can hold, so `100` + `000000` is ±1 and `1100` +
`00000000` is ±65.

Each range includes all smaller ranges, ensuring optimal encoding.

## See Also

- [XOF.md](XOF.md) - XOR-encoded compression for doubles
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
